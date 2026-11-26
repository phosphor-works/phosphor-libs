// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorIpc/IpcClient.h>

#include <PhosphorIpc/IpcProtocol.h>

#include <QByteArray>
#include <QChar>
#include <QCoreApplication>
#include <QEvent>
#include <QHash>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLatin1String>
#include <QList>
#include <QLocalSocket>
#include <QMetaObject>
#include <QPointer>
#include <QSet>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

namespace PhosphorIpc {

namespace {

// The most lines that one pass handles before it gives the event loop a
// turn, so that a server that writes without a pause cannot hold the loop.
constexpr int MaxLinesPerPass = 64;

// The most bytes that one read takes from the socket. QIODevice::read()
// makes an array of the size that it is asked for, so a read of the whole
// cap of a line would make 64 MiB for each reply.
constexpr qint64 ReadPieceBytes = 64 * 1024;

// The largest cap of a line. The client adds 1 to the cap.
constexpr qint64 MaxReplyLineBytesLimit = std::numeric_limits<qint64>::max() - 1;

// The most characters of a text of the server that the client keeps for
// the reason of a close.
constexpr qsizetype MaxPeerTextLength = 256;

QJsonObject request(const char* type)
{
    QJsonObject object;
    object.insert(QLatin1String(Field::Type), QLatin1String(type));
    return object;
}

// A text that the server sent, cut, and with a space for each character
// that could change a log line: the control characters (the escape), the
// format characters of the basic plane (the bidirectional overrides) and
// the line separators.
QString peerText(const QString& text)
{
    QString out = text.left(MaxPeerTextLength);
    for (QChar& character : out) {
        switch (character.category()) {
        case QChar::Other_Control:
        case QChar::Other_Format:
        case QChar::Separator_Line:
        case QChar::Separator_Paragraph:
            character = QLatin1Char(' ');
            break;
        default:
            break;
        }
    }
    return out;
}

} // namespace

struct IpcClient::Private
{
    enum class RequestKind {
        Call,
        List,
        Schema,
        Subscribe,
        Unsubscribe,
    };

    struct Pending
    {
        RequestKind kind = RequestKind::Call;
        Callback callback;
        QString target;
        QString signalName;
        // A subscribe that unsubscribe() ended before its answer came.
        bool dropped = false;
    };

    struct Subscription
    {
        QString target;
        QString signalName;
    };

    explicit Private(IpcClient* client)
        : q(client)
    {
    }

    qint64 send(const char* function, QJsonObject request, Pending pending);
    void fail(Callback callback, const char* code, const QString& message);
    void onConnected();
    void onSocketError();
    void onConnectTimeout(quint64 serial, int milliseconds);
    // Handles the lines that came.
    void readLines();
    void readLinesLater();
    // The subscriptions that unsubscribe() ended after their connection
    // ended. The events of such a connection that still wait in the queue
    // look here.
    using EndedLater = std::shared_ptr<QSet<qint64>>;
    // What one line of the server means. It changes the client's own
    // books immediately, and returns what the caller sees of the line (a
    // callback, a signal), to run now or from the event loop. `problem`
    // gets a text for a line that the client cannot read. `endedLater` is
    // given for a call that goes into the queue.
    std::function<void()> interpret(const QByteArray& line, QString* problem, const EndedLater& endedLater = {});
    // What becomes of the lines that the client has not handled yet.
    enum class Rest {
        Deliver,
        Drop,
    };
    void endConnection(const QString& reason, Rest rest = Rest::Deliver);

    IpcClient* const q;
    QLocalSocket* socket = nullptr;
    State state = State::Unconnected;
    // Changes when a connection starts and when it ends, so that code that
    // ran a callback can tell whether its connection is still the one.
    quint64 connection = 0;
    QString path;
    qint64 maxPendingWriteBytes = DefaultMaxPendingWriteBytes;
    qint64 maxReplyLineBytes = DefaultMaxReplyLineBytes;
    int connectTimeoutMs = DefaultConnectTimeoutMs;
    // Requests made while the client connects.
    QByteArray unsent;
    // What came from the server and is not handled yet. The next line
    // starts at `lineStart`, and no newline is before `scanned`.
    QByteArray input;
    qsizetype lineStart = 0;
    qsizetype scanned = 0;
    // A loop of readLines() is on the stack. A callback of it that runs an
    // event loop makes the socket report more data or its end meanwhile.
    bool reading = false;
    // Sockets of connections that ended while `reading` was set. A
    // deleteLater() inside an event loop that a callback runs would free
    // the socket under its own signal, which is on the stack below.
    QList<QLocalSocket*> parked;
    // The last error of this connection that names no request. The router
    // sends one before it closes a connection, with the reason.
    QString serverError;
    qint64 lastId = 0;
    QHash<qint64, Pending> pending;
    QHash<qint64, Subscription> subscriptions;
    // One for each ended connection whose calls still wait in the queue.
    QList<std::weak_ptr<QSet<qint64>>> endedSets;
    // The destructor runs. A request from now on is dropped.
    bool destroying = false;
};

IpcClient::IpcClient(QObject* parent)
    : QObject(parent)
    , d(std::make_unique<Private>(this))
{
}

IpcClient::~IpcClient()
{
    // A callback can hold an object whose destructor calls the client. The
    // callbacks go here, while the client is whole, not with its members.
    // The ones that wait in the event queue would otherwise go later
    // still, in ~QObject. With the flag set, a request from such a
    // destructor is dropped, so that neither step adds a callback.
    d->destroying = true;
    std::exchange(d->pending, {}).clear();
    QCoreApplication::removePostedEvents(this, QEvent::MetaCall);
}

void IpcClient::setMaxPendingWriteBytes(qint64 bytes)
{
    d->maxPendingWriteBytes = std::max<qint64>(1, bytes);
}

qint64 IpcClient::maxPendingWriteBytes() const
{
    return d->maxPendingWriteBytes;
}

void IpcClient::setMaxReplyLineBytes(qint64 bytes)
{
    d->maxReplyLineBytes = std::clamp<qint64>(bytes, 1, MaxReplyLineBytesLimit);
    if (d->socket) {
        d->socket->setReadBufferSize(d->maxReplyLineBytes + 1);
    }
}

qint64 IpcClient::maxReplyLineBytes() const
{
    return d->maxReplyLineBytes;
}

void IpcClient::setConnectTimeout(int milliseconds)
{
    d->connectTimeoutMs = milliseconds;
}

int IpcClient::connectTimeout() const
{
    return d->connectTimeoutMs;
}

bool IpcClient::connectToServer(const QString& path)
{
    if (QThread::currentThread() != thread()) {
        qWarning("PhosphorIpc::IpcClient::connectToServer: called from another thread than the client's");
        return false;
    }
    if (d->destroying) {
        return false;
    }
    if (d->state != State::Unconnected) {
        qWarning("PhosphorIpc::IpcClient::connectToServer: the client is connecting or connected already");
        return false;
    }
    d->path = path;
    d->state = State::Connecting;
    const quint64 serial = ++d->connection;
    d->serverError.clear();
    QLocalSocket* const socket = new QLocalSocket(this);
    d->socket = socket;
    // One byte more than the longest line that the client takes. With the
    // buffer full, Qt reads no further until the client has read, so a
    // callback that runs an event loop holds back the server, not memory.
    socket->setReadBufferSize(d->maxReplyLineBytes + 1);
    connect(socket, &QLocalSocket::connected, this, [this] {
        d->onConnected();
    });
    connect(socket, &QLocalSocket::readyRead, this, [this] {
        d->readLines();
    });
    connect(socket, &QLocalSocket::disconnected, this, [this] {
        d->endConnection(QStringLiteral("the server closed the connection"));
    });
    connect(socket, &QLocalSocket::errorOccurred, this, [this] {
        d->onSocketError();
    });
    // Qt gives up a connection that the server does not accept without a
    // signal, and stays in the connecting state.
    if (d->connectTimeoutMs > 0) {
        const int milliseconds = d->connectTimeoutMs;
        QTimer::singleShot(milliseconds, this, [this, serial, milliseconds] {
            d->onConnectTimeout(serial, milliseconds);
        });
    }
    // May report the connection, or its failure, before it returns. The
    // handlers send nothing to the caller synchronously.
    socket->connectToServer(path);
    return true;
}

void IpcClient::close()
{
    if (QThread::currentThread() != thread()) {
        qWarning("PhosphorIpc::IpcClient::close: called from another thread than the client's");
        return;
    }
    if (d->destroying) {
        return;
    }
    d->endConnection(QStringLiteral("the client closed the connection"));
}

IpcClient::State IpcClient::state() const
{
    return d->state;
}

QString IpcClient::serverPath() const
{
    return d->path;
}

qint64 IpcClient::call(const QString& target, const QString& fn, const QJsonArray& args, Callback callback)
{
    QJsonObject object = request(RequestType::Call);
    object.insert(QLatin1String(Field::Target), target);
    object.insert(QLatin1String(Field::Fn), fn);
    object.insert(QLatin1String(Field::Args), args);
    return d->send("call", std::move(object),
                   Private::Pending{Private::RequestKind::Call, std::move(callback), {}, {}});
}

qint64 IpcClient::list(Callback callback)
{
    return d->send("list", request(RequestType::List),
                   Private::Pending{Private::RequestKind::List, std::move(callback), {}, {}});
}

qint64 IpcClient::schema(const QString& target, Callback callback)
{
    QJsonObject object = request(RequestType::Schema);
    object.insert(QLatin1String(Field::Target), target);
    return d->send("schema", std::move(object),
                   Private::Pending{Private::RequestKind::Schema, std::move(callback), {}, {}});
}

qint64 IpcClient::subscribe(const QString& target, const QString& signalName, Callback callback)
{
    QJsonObject object = request(RequestType::Subscribe);
    object.insert(QLatin1String(Field::Target), target);
    object.insert(QLatin1String(Field::Signal), signalName);
    return d->send("subscribe", std::move(object),
                   Private::Pending{Private::RequestKind::Subscribe, std::move(callback), target, signalName});
}

qint64 IpcClient::unsubscribe(qint64 subscriptionId, Callback callback)
{
    if (QThread::currentThread() == thread()) {
        d->subscriptions.remove(subscriptionId);
        // Events of a connection that ended can still wait in the queue.
        d->endedSets.removeIf([subscriptionId](const std::weak_ptr<QSet<qint64>>& entry) {
            const Private::EndedLater ids = entry.lock();
            if (ids) {
                ids->insert(subscriptionId);
            }
            return !ids;
        });
        // A subscribe still on its way: its answer must not start a stream
        // that the caller ended.
        const auto subscribing = d->pending.find(subscriptionId);
        if (subscribing != d->pending.end() && subscribing->kind == Private::RequestKind::Subscribe) {
            subscribing->dropped = true;
        }
    }
    QJsonObject object = request(RequestType::Unsubscribe);
    object.insert(QLatin1String(Field::SubscriptionId), QJsonValue(subscriptionId));
    return d->send("unsubscribe", std::move(object),
                   Private::Pending{Private::RequestKind::Unsubscribe, std::move(callback), {}, {}});
}

qint64 IpcClient::Private::send(const char* function, QJsonObject request, Pending waiting)
{
    if (QThread::currentThread() != q->thread()) {
        qWarning("PhosphorIpc::IpcClient::%s: called from another thread than the client's", function);
        // No state of the client is safe to read here. The failure goes to
        // the client's own thread, so that the callback still runs there.
        if (waiting.callback) {
            IpcReply reply;
            reply.errorCode = QString::fromLatin1(ClientErrorCode::InvalidRequest);
            reply.errorMessage = QStringLiteral("the request was made from another thread than the client's");
            reply.fromClient = true;
            QMetaObject::invokeMethod(
                q,
                [callback = std::move(waiting.callback), reply = std::move(reply)] {
                    callback(reply);
                },
                Qt::QueuedConnection);
        }
        return 0;
    }
    if (destroying) {
        return 0;
    }
    // Ids only grow, so a request never takes the id of a live
    // subscription, which is the id of the subscribe that made it.
    const qint64 id = ++lastId;
    request.insert(QLatin1String(Field::Id), QJsonValue(id));
    const QByteArray line = writeLine(request);
    // The router closes a connection that sends a longer line, which would
    // fail every other request of the client too.
    if (line.size() > MaxRequestLineBytes) {
        fail(std::move(waiting.callback), ClientErrorCode::RequestTooLarge,
             QStringLiteral("the request line has %1 bytes, and the server reads at most %2")
                 .arg(line.size())
                 .arg(MaxRequestLineBytes));
        return id;
    }
    // The router answers a line that it cannot read with an error that
    // names no request, so the callback of such a request would never
    // run. The router's own check tells which lines those are.
    QString problem;
    if (!parseRequest(line, &problem)) {
        fail(std::move(waiting.callback), ClientErrorCode::InvalidRequest, problem);
        return id;
    }
    if (state == State::Unconnected) {
        fail(std::move(waiting.callback), ClientErrorCode::NotConnected,
             QStringLiteral("the client is not connected to a server"));
        return id;
    }
    const qint64 queued = unsent.size() + (state == State::Connected ? socket->bytesToWrite() : 0);
    if (queued + line.size() > maxPendingWriteBytes) {
        fail(std::move(waiting.callback), ClientErrorCode::WriteQueueFull,
             QStringLiteral("the client holds %1 bytes of requests that it has not written yet, and the cap is %2")
                 .arg(queued)
                 .arg(maxPendingWriteBytes));
        return id;
    }
    pending.insert(id, std::move(waiting));
    if (state == State::Connected) {
        socket->write(line);
    } else {
        unsent.append(line);
    }
    return id;
}

void IpcClient::Private::fail(Callback callback, const char* code, const QString& message)
{
    if (!callback || destroying) {
        return;
    }
    IpcReply reply;
    reply.errorCode = QString::fromLatin1(code);
    reply.errorMessage = message;
    reply.fromClient = true;
    // From the event loop, never from inside the request that failed. The
    // queued call is dropped with the client.
    QMetaObject::invokeMethod(
        q,
        [callback = std::move(callback), reply = std::move(reply)] {
            callback(reply);
        },
        Qt::QueuedConnection);
}

void IpcClient::Private::onConnected()
{
    state = State::Connected;
    if (!unsent.isEmpty()) {
        socket->write(std::exchange(unsent, {}));
    }
    // connectToServer() may get here before it returns. A close() before
    // the event loop runs ends this connection, and connected() would then
    // tell of a connection that is gone.
    const quint64 serial = connection;
    QMetaObject::invokeMethod(
        q,
        [this, serial] {
            if (connection == serial) {
                Q_EMIT q->connected();
            }
        },
        Qt::QueuedConnection);
}

void IpcClient::Private::onSocketError()
{
    if (state == State::Connecting) {
        endConnection(QStringLiteral("cannot connect to %1: %2").arg(path, socket->errorString()));
        return;
    }
    if (socket->error() == QLocalSocket::PeerClosedError) {
        endConnection(QStringLiteral("the server closed the connection"));
        return;
    }
    endConnection(socket->errorString());
}

void IpcClient::Private::onConnectTimeout(quint64 serial, int milliseconds)
{
    if (connection != serial || state != State::Connecting) {
        return;
    }
    // One arg() call: a path can hold "%2".
    endConnection(QStringLiteral("cannot connect to %1: the server did not accept the connection within %2 ms")
                      .arg(path, QString::number(milliseconds)));
}

void IpcClient::Private::readLines()
{
    if (reading) {
        // A callback of the loop below runs an event loop, in which a
        // socket reported data. That is the socket of a second connection,
        // or the loop's own socket when the pass came from readLinesLater().
        // (Its own socket reports nothing while its signal is on the
        // stack.) The loop reads on when its callback returns, or starts
        // a new pass for a second connection.
        return;
    }
    const quint64 entered = connection;
    const QPointer<IpcClient> self(q);
    reading = true;
    int handled = 0;
    bool more = false;
    QString problem;
    for (;;) {
        const qsizetype newline = input.indexOf('\n', scanned);
        if (newline < 0) {
            scanned = input.size();
            if (input.size() - lineStart > maxReplyLineBytes) {
                problem = QStringLiteral("the server sent a line longer than %1 bytes").arg(maxReplyLineBytes);
                break;
            }
            if (lineStart > 0) {
                input.remove(0, lineStart);
                scanned -= lineStart;
                lineStart = 0;
            }
            // Also after each callback: what came while a callback ran an
            // event loop is in the socket, and no signal tells of it.
            // The loop ends only on an empty read: that read turns the read
            // notifier of Qt on again after a full buffer.
            const qint64 room = maxReplyLineBytes + 1 - input.size();
            const QByteArray bytes = socket->read(std::min(room, ReadPieceBytes));
            if (bytes.isEmpty()) {
                break;
            }
            input.append(bytes);
            continue;
        }
        const QByteArray line = input.mid(lineStart, newline - lineStart);
        lineStart = newline + 1;
        scanned = lineStart;
        if (line.size() > maxReplyLineBytes) {
            problem = QStringLiteral("the server sent a line longer than %1 bytes").arg(maxReplyLineBytes);
            break;
        }
        std::function<void()> delivery = interpret(line, &problem);
        if (!problem.isEmpty()) {
            break;
        }
        if (delivery) {
            delivery();
            // The callback goes here, before the checks below. An object
            // that it holds can close the client in its destructor, and
            // the loop must see that before it reads the socket again.
            delivery = nullptr;
            if (!self) {
                return;
            }
            if (connection != entered) {
                break;
            }
        }
        if (++handled >= MaxLinesPerPass) {
            more = true;
            break;
        }
    }
    reading = false;
    for (QLocalSocket* const ended : std::exchange(parked, {})) {
        ended->deleteLater();
    }
    if (connection != entered) {
        // A callback ended the connection. When it also made a new one,
        // the data of that one waits in its socket.
        if (socket) {
            readLinesLater();
        }
        return;
    }
    if (!problem.isEmpty()) {
        // The client cannot tell which request such a line answers, and
        // has no timeout, so that request would wait without end. The
        // lines after it are dropped, as endConnection() drops them.
        endConnection(problem, Rest::Drop);
        return;
    }
    if (more) {
        readLinesLater();
    }
}

void IpcClient::Private::readLinesLater()
{
    const quint64 serial = connection;
    QMetaObject::invokeMethod(
        q,
        [this, serial] {
            // A socket that still connects has nothing to read.
            if (connection == serial && socket && state == State::Connected) {
                readLines();
            }
        },
        Qt::QueuedConnection);
}

std::function<void()> IpcClient::Private::interpret(const QByteArray& line, QString* problem,
                                                    const EndedLater& endedLater)
{
    if (line.trimmed().isEmpty()) {
        return {};
    }
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        *problem = QStringLiteral("the server sent a line that is not a JSON object");
        return {};
    }
    const QJsonObject object = document.object();
    const QString type = object.value(QLatin1String(Field::Type)).toString();

    if (type == QLatin1String(ResponseType::Event)) {
        const QJsonArray args = object.value(QLatin1String(Field::Args)).toArray();
        const QJsonValue subscriptionId = object.value(QLatin1String(Field::SubscriptionId));
        if (!subscriptionId.isUndefined() && !subscriptionId.isNull()) {
            const std::optional<qint64> id = integerId(subscriptionId);
            if (!id) {
                *problem = QStringLiteral("the server sent an event with a subscription id that is not an integer");
                return {};
            }
            const auto subscription = subscriptions.constFind(*id);
            // An event of a subscription that unsubscribe() ended.
            if (subscription == subscriptions.cend()) {
                return {};
            }
            return [client = q, id = *id, names = *subscription, args, endedLater] {
                if (endedLater && endedLater->contains(id)) {
                    return;
                }
                Q_EMIT client->eventReceived(id, names.target, names.signalName, args);
            };
        }
        const QString target = object.value(QLatin1String(Field::Target)).toString();
        const QString signalName = object.value(QLatin1String(Field::Signal)).toString();
        // An event to the connection names its target and signal. One that
        // names neither is of no kind that the client knows.
        if (target.isEmpty() || signalName.isEmpty()) {
            return {};
        }
        return [client = q, target, signalName, args] {
            Q_EMIT client->eventReceived(0, target, signalName, args);
        };
    }

    const bool isError = type == QLatin1String(ResponseType::Error);
    if (!isError && type != QLatin1String(ResponseType::Reply)) {
        // A frame of a later version of the protocol.
        return {};
    }
    const QJsonValue idValue = object.value(QLatin1String(Field::Id));
    if (idValue.isUndefined() || idValue.isNull()) {
        // An error that names no request. The router sends one for a line
        // that it could not read, and before it closes a connection. The
        // client sends no line of the first kind, so this is the reason of
        // a close that follows.
        if (isError) {
            serverError = peerText(object.value(QLatin1String(Field::Message)).toString());
        }
        return {};
    }
    const std::optional<qint64> id = integerId(idValue);
    if (!id) {
        *problem = QStringLiteral("the server sent an answer with an id that is not an integer");
        return {};
    }

    IpcReply reply;
    if (isError) {
        const QJsonValue code = object.value(QLatin1String(Field::Code));
        if (code.isString() && !code.toString().isEmpty()) {
            reply.errorCode = code.toString();
        } else {
            reply.errorCode = QString::fromLatin1(ClientErrorCode::MalformedReply);
            reply.fromClient = true;
        }
        reply.errorMessage = object.value(QLatin1String(Field::Message)).toString();
        reply.errorDetail = object.value(QLatin1String(Field::Detail)).toObject();
        if (!pending.contains(*id)) {
            // The server ends a subscription with an error under its id.
            if (subscriptions.remove(*id)) {
                return [client = q, id = *id, reply, endedLater] {
                    if (endedLater && endedLater->contains(id)) {
                        return;
                    }
                    Q_EMIT client->subscriptionEnded(id, reply);
                };
            }
            return {};
        }
    } else {
        // A reply always has a result. One without it reads as null, as
        // the reply of a method that returns nothing.
        const QJsonValue result = object.value(QLatin1String(Field::Result));
        reply.result = result.isUndefined() ? QJsonValue(QJsonValue::Null) : result;
    }

    const auto found = pending.find(*id);
    if (found == pending.end()) {
        return {};
    }
    Pending answered = std::move(*found);
    pending.erase(found);
    if (answered.kind == RequestKind::Subscribe && !reply.isError() && !answered.dropped) {
        subscriptions.insert(*id, Subscription{answered.target, answered.signalName});
    }
    if (!answered.callback) {
        return {};
    }
    return [callback = std::move(answered.callback), reply] {
        callback(reply);
    };
}

void IpcClient::Private::endConnection(const QString& reason, Rest rest)
{
    if (!socket) {
        return;
    }
    QLocalSocket* const ended = std::exchange(socket, nullptr);
    ended->disconnect(q);
    if (rest == Rest::Drop) {
        input.clear();
        lineStart = 0;
    } else {
        // What the server sent before the end is still an answer. All of
        // it: the buffer of Qt holds whole lines too, not only the rest of
        // one line. It is at most one byte more than the cap of a line at
        // the time Qt read it, so this adds no more than that to `input`.
        while (ended->bytesAvailable() > 0) {
            const QByteArray bytes = ended->read(ReadPieceBytes);
            if (bytes.isEmpty()) {
                break;
            }
            input.append(bytes);
        }
    }
    ended->abort();
    if (reading) {
        parked.append(ended);
    } else {
        ended->deleteLater();
    }
    state = State::Unconnected;
    ++connection;
    unsent.clear();

    // No code of the caller runs in here: close() may be on the stack of a
    // callback, and the end of a connection may come inside an event loop
    // that a callback runs. Everything goes through the event loop, in the
    // order of the wire: the answers that came, then the failure of the
    // requests that got none, then closed().
    // unsubscribe() can run between this function and a queued event.
    const EndedLater later = std::make_shared<QSet<qint64>>();
    endedSets.removeIf([](const std::weak_ptr<QSet<qint64>>& entry) {
        return entry.expired();
    });
    endedSets.append(later);
    QList<std::function<void()>> deliveries;
    for (;;) {
        const qsizetype newline = input.indexOf('\n', lineStart);
        if (newline < 0) {
            break;
        }
        const QByteArray line = input.mid(lineStart, newline - lineStart);
        lineStart = newline + 1;
        if (line.size() > maxReplyLineBytes) {
            break;
        }
        QString problem;
        std::function<void()> delivery = interpret(line, &problem, later);
        if (!problem.isEmpty()) {
            break;
        }
        if (delivery) {
            deliveries.append(std::move(delivery));
        }
    }
    input.clear();
    lineStart = 0;
    scanned = 0;
    subscriptions.clear();
    const QString text = serverError.isEmpty() ? reason : QStringLiteral("%1: %2").arg(reason, serverError);
    serverError.clear();

    for (std::function<void()>& delivery : deliveries) {
        QMetaObject::invokeMethod(q, std::move(delivery), Qt::QueuedConnection);
    }
    // In the order the requests were made.
    QHash<qint64, Pending> waiting = std::exchange(pending, {});
    QList<qint64> ids = waiting.keys();
    std::sort(ids.begin(), ids.end());
    for (const qint64 id : std::as_const(ids)) {
        fail(std::move(waiting[id].callback), ClientErrorCode::ConnectionClosed, text);
    }
    QMetaObject::invokeMethod(
        q,
        [this, text] {
            Q_EMIT q->closed(text);
        },
        Qt::QueuedConnection);
}

} // namespace PhosphorIpc
