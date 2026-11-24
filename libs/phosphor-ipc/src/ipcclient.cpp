// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorIpc/IpcClient.h>

#include <PhosphorIpc/IpcProtocol.h>

#include <QJsonDocument>
#include <QJsonParseError>
#include <QLatin1String>
#include <QLocalSocket>
#include <QMetaObject>
#include <QPointer>

#include <algorithm>
#include <utility>

namespace PhosphorIpc {

namespace {

QJsonObject request(const char* type)
{
    QJsonObject object;
    object.insert(QLatin1String(Field::Type), QLatin1String(type));
    return object;
}

} // namespace

IpcClient::IpcClient(QObject* parent)
    : QObject(parent)
{
}

IpcClient::~IpcClient()
{
    // The socket is a child and goes with the client. Its last signals
    // would reach a client that is half destroyed.
    if (m_socket) {
        m_socket->disconnect(this);
    }
}

bool IpcClient::connectToServer(const QString& path)
{
    if (m_state != State::Unconnected) {
        qWarning("PhosphorIpc::IpcClient::connectToServer: the client is connecting or connected already");
        return false;
    }
    m_path = path;
    m_state = State::Connecting;
    m_socket = new QLocalSocket(this);
    connect(m_socket, &QLocalSocket::connected, this, &IpcClient::onConnected);
    connect(m_socket, &QLocalSocket::readyRead, this, &IpcClient::onReadyRead);
    connect(m_socket, &QLocalSocket::disconnected, this, &IpcClient::onDisconnected);
    connect(m_socket, &QLocalSocket::errorOccurred, this, &IpcClient::onSocketError);
    // May report the connection, or its failure, before it returns. The
    // handlers send nothing to the caller synchronously.
    m_socket->connectToServer(path);
    return true;
}

void IpcClient::close()
{
    endConnection(QStringLiteral("the client closed the connection"));
}

IpcClient::State IpcClient::state() const
{
    return m_state;
}

QString IpcClient::serverPath() const
{
    return m_path;
}

qint64 IpcClient::call(const QString& target, const QString& fn, const QJsonArray& args, Callback callback)
{
    QJsonObject object = request(RequestType::Call);
    object.insert(QLatin1String(Field::Target), target);
    object.insert(QLatin1String(Field::Fn), fn);
    object.insert(QLatin1String(Field::Args), args);
    return send(std::move(object), Pending{RequestKind::Call, std::move(callback), {}, {}});
}

qint64 IpcClient::list(Callback callback)
{
    return send(request(RequestType::List), Pending{RequestKind::List, std::move(callback), {}, {}});
}

qint64 IpcClient::schema(const QString& target, Callback callback)
{
    QJsonObject object = request(RequestType::Schema);
    object.insert(QLatin1String(Field::Target), target);
    return send(std::move(object), Pending{RequestKind::Schema, std::move(callback), {}, {}});
}

qint64 IpcClient::subscribe(const QString& target, const QString& signalName, Callback callback)
{
    QJsonObject object = request(RequestType::Subscribe);
    object.insert(QLatin1String(Field::Target), target);
    object.insert(QLatin1String(Field::Signal), signalName);
    return send(std::move(object), Pending{RequestKind::Subscribe, std::move(callback), target, signalName});
}

qint64 IpcClient::unsubscribe(qint64 subscriptionId, Callback callback)
{
    m_subscriptions.remove(subscriptionId);
    // A subscribe still on its way: its answer must not start a stream that
    // the caller ended.
    const auto subscribing = m_pending.find(subscriptionId);
    if (subscribing != m_pending.end() && subscribing->kind == RequestKind::Subscribe) {
        subscribing->dropped = true;
    }
    QJsonObject object = request(RequestType::Unsubscribe);
    object.insert(QLatin1String(Field::SubscriptionId), static_cast<double>(subscriptionId));
    return send(std::move(object), Pending{RequestKind::Unsubscribe, std::move(callback), {}, {}});
}

qint64 IpcClient::send(QJsonObject request, Pending pending)
{
    // Ids only grow, so a request never takes the id of a live
    // subscription, which is the id of the subscribe that made it.
    const qint64 id = ++m_lastId;
    if (m_state == State::Unconnected) {
        fail(std::move(pending.callback), ClientErrorCode::NotConnected,
             QStringLiteral("the client is not connected to a server"));
        return id;
    }
    request.insert(QLatin1String(Field::Id), static_cast<double>(id));
    const QByteArray line = writeLine(request);
    const qint64 queued = m_unsent.size() + (m_state == State::Connected ? m_socket->bytesToWrite() : 0);
    if (queued + line.size() > MaxPendingWriteBytes) {
        fail(std::move(pending.callback), ClientErrorCode::WriteQueueFull,
             QStringLiteral("the server has not read %1 bytes of earlier requests").arg(queued));
        return id;
    }
    m_pending.insert(id, std::move(pending));
    if (m_state == State::Connected) {
        m_socket->write(line);
    } else {
        m_unsent.append(line);
    }
    return id;
}

void IpcClient::fail(Callback callback, const char* code, const QString& message)
{
    if (!callback) {
        return;
    }
    IpcReply reply;
    reply.errorCode = QString::fromLatin1(code);
    reply.errorMessage = message;
    // From the event loop, never from inside the request that failed. The
    // queued call is dropped with the client.
    QMetaObject::invokeMethod(
        this,
        [callback = std::move(callback), reply = std::move(reply)] {
            callback(reply);
        },
        Qt::QueuedConnection);
}

void IpcClient::onConnected()
{
    m_state = State::Connected;
    if (!m_unsent.isEmpty()) {
        m_socket->write(std::exchange(m_unsent, {}));
    }
    // connectToServer() may get here before it returns.
    QMetaObject::invokeMethod(
        this,
        [this] {
            Q_EMIT connected();
        },
        Qt::QueuedConnection);
}

void IpcClient::onReadyRead()
{
    readLines();
}

void IpcClient::onDisconnected()
{
    // The last replies may still be in the socket's buffer.
    if (m_socket->bytesAvailable() > 0 && !readLines()) {
        return;
    }
    endConnection(QStringLiteral("the server closed the connection"));
}

void IpcClient::onSocketError()
{
    if (m_state == State::Connecting) {
        endConnection(QStringLiteral("cannot connect to %1: %2").arg(m_path, m_socket->errorString()));
        return;
    }
    if (m_socket->error() == QLocalSocket::PeerClosedError) {
        onDisconnected();
        return;
    }
    if (m_socket->bytesAvailable() > 0 && !readLines()) {
        return;
    }
    endConnection(m_socket->errorString());
}

bool IpcClient::readLines()
{
    QLocalSocket* const socket = m_socket;
    m_input.append(socket->readAll());
    qsizetype start = 0;
    for (qsizetype newline = m_input.indexOf('\n'); newline >= 0; newline = m_input.indexOf('\n', start)) {
        m_lines.append(m_input.mid(start, newline - start));
        start = newline + 1;
    }
    m_input.remove(0, start);
    if (m_input.size() > MaxLineBytes) {
        endConnection(QStringLiteral("the server sent a line longer than %1 bytes").arg(MaxLineBytes));
        return false;
    }
    if (m_handlingLines) {
        // A callback of the loop below spins an event loop. The lines wait
        // for that loop, in order.
        return true;
    }
    const QPointer<IpcClient> self(this);
    m_handlingLines = true;
    while (!m_lines.isEmpty()) {
        handleLine(m_lines.takeFirst());
        if (!self) {
            return false;
        }
        if (m_socket != socket) {
            m_handlingLines = false;
            return false;
        }
    }
    m_handlingLines = false;
    return true;
}

void IpcClient::handleLine(const QByteArray& line)
{
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (!line.trimmed().isEmpty()) {
            qWarning("PhosphorIpc::IpcClient: skipped a line from the server that is not a JSON object");
        }
        return;
    }
    const QJsonObject object = document.object();
    const QString type = object.value(QLatin1String(Field::Type)).toString();

    if (type == QLatin1String(ResponseType::Event)) {
        const QJsonArray args = object.value(QLatin1String(Field::Args)).toArray();
        const QJsonValue subscriptionId = object.value(QLatin1String(Field::SubscriptionId));
        if (subscriptionId.isDouble()) {
            const qint64 id = static_cast<qint64>(subscriptionId.toDouble());
            const auto subscription = m_subscriptions.constFind(id);
            // An event of a subscription that unsubscribe() ended.
            if (subscription == m_subscriptions.cend()) {
                return;
            }
            const Subscription copy = *subscription;
            Q_EMIT eventReceived(id, copy.target, copy.signalName, args);
            return;
        }
        Q_EMIT eventReceived(0, object.value(QLatin1String(Field::Target)).toString(),
                             object.value(QLatin1String(Field::Signal)).toString(), args);
        return;
    }

    const QJsonValue idValue = object.value(QLatin1String(Field::Id));
    if (!idValue.isDouble()) {
        // The client sends no request without an id, so the server answered
        // a line it could not read.
        if (type == QLatin1String(ResponseType::Error)) {
            qWarning("PhosphorIpc::IpcClient: the server sent an error that answers no request: %s",
                     qPrintable(object.value(QLatin1String(Field::Message)).toString()));
        }
        return;
    }
    const qint64 id = static_cast<qint64>(idValue.toDouble());

    IpcReply reply;
    if (type == QLatin1String(ResponseType::Error)) {
        const QJsonValue code = object.value(QLatin1String(Field::Code));
        reply.errorCode = code.isString() && !code.toString().isEmpty()
            ? code.toString()
            : QString::fromLatin1(ClientErrorCode::MalformedReply);
        reply.errorMessage = object.value(QLatin1String(Field::Message)).toString();
        reply.errorDetail = object.value(QLatin1String(Field::Detail)).toObject();
        if (!m_pending.contains(id)) {
            // The server ends a subscription with an error under its id.
            if (m_subscriptions.remove(id)) {
                Q_EMIT subscriptionEnded(id, reply);
            }
            return;
        }
    } else if (type == QLatin1String(ResponseType::Reply)) {
        reply.result = object.value(QLatin1String(Field::Result));
    } else {
        return;
    }

    const auto found = m_pending.find(id);
    if (found == m_pending.end()) {
        return;
    }
    const Pending pending = std::move(*found);
    m_pending.erase(found);
    if (pending.kind == RequestKind::Subscribe && !reply.isError() && !pending.dropped) {
        m_subscriptions.insert(id, Subscription{pending.target, pending.signalName});
    }
    if (pending.callback) {
        pending.callback(reply);
    }
}

void IpcClient::endConnection(const QString& reason)
{
    if (!m_socket) {
        return;
    }
    QLocalSocket* const socket = std::exchange(m_socket, nullptr);
    socket->disconnect(this);
    socket->abort();
    socket->deleteLater();
    m_state = State::Unconnected;
    m_unsent.clear();
    m_input.clear();
    m_lines.clear();
    m_subscriptions.clear();

    // In the order the requests were made.
    QHash<qint64, Pending> pending = std::exchange(m_pending, {});
    QList<qint64> ids = pending.keys();
    std::sort(ids.begin(), ids.end());
    for (const qint64 id : std::as_const(ids)) {
        fail(std::move(pending[id].callback), ClientErrorCode::ConnectionClosed, reason);
    }
    QMetaObject::invokeMethod(
        this,
        [this, reason] {
            Q_EMIT closed(reason);
        },
        Qt::QueuedConnection);
}

} // namespace PhosphorIpc
