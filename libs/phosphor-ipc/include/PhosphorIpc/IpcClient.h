// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <PhosphorIpc/phosphoripc_export.h>

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QtCore/qtclasshelpermacros.h>

#include <functional>

QT_BEGIN_NAMESPACE
class QLocalSocket;
QT_END_NAMESPACE

namespace PhosphorIpc {

// Error codes that the client gives a callback itself, for a request that
// got no answer from the server. They never travel on the wire, and differ
// from every code of ErrorCode in IpcProtocol.h.
namespace ClientErrorCode {
// The client had no connection, and was not making one, when the request
// was made.
constexpr auto NotConnected = "NOT_CONNECTED";
// The connection ended before the answer came: the server went away, the
// connection failed, or close() ran.
constexpr auto ConnectionClosed = "CONNECTION_CLOSED";
// The request would have put more than IpcClient::MaxPendingWriteBytes in
// the queue of bytes that the server has not read yet.
constexpr auto WriteQueueFull = "WRITE_QUEUE_FULL";
// The server answered with an error frame that has no string code.
constexpr auto MalformedReply = "MALFORMED_REPLY";
} // namespace ClientErrorCode

// The answer to one request: the result of a reply, or an error. The error
// is the server's (a code of ErrorCode, or a code of the target's own for a
// deferred call) or the client's (ClientErrorCode).
struct IpcReply
{
    // Null for an error.
    QJsonValue result;
    QString errorCode;
    QString errorMessage;
    QJsonObject errorDetail;

    [[nodiscard]] bool isError() const
    {
        return !errorCode.isEmpty();
    }
};

} // namespace PhosphorIpc

Q_DECLARE_METATYPE(PhosphorIpc::IpcReply)

namespace PhosphorIpc {

// A client of the wire format of IpcRouter (IpcProtocol.h) over one
// QLocalSocket. It never blocks the event loop: each request returns at
// once, and its callback runs when the answer comes.
//
//     PhosphorIpc::IpcClient client;
//     client.connectToServer(path);
//     client.call(QStringLiteral("theme"), QStringLiteral("setMode"), {QStringLiteral("dark")},
//                 [](const PhosphorIpc::IpcReply& reply) {
//                     if (reply.isError())
//                         qWarning() << reply.errorCode << reply.errorMessage;
//                 });
//
// Requests
//   - Each request gets an id of its own, and the client matches the answer
//     to it, so any number of requests may be in flight and answered in any
//     order (a deferred call answers after later ones).
//   - A request made while the client connects waits and goes out once it
//     is connected. One made without a connection fails with NOT_CONNECTED.
//   - Each callback runs once, from the event loop, never from inside the
//     call that made the request. When the connection ends, every request
//     still waiting fails with CONNECTION_CLOSED.
//   - Destroying the client drops the callbacks that wait, unrun, as
//     McpServer does: they may hold objects that are gone by then.
//
// Writes
//   - The bytes that the server has not read yet wait in a queue, capped
//     at MaxPendingWriteBytes. A request past it fails with
//     WRITE_QUEUE_FULL, so a server that stops reading cannot grow the
//     client's memory without bound.
//
// Events
//   - subscribe() asks for the events of a signal, which come as
//     eventReceived() with the subscription's id until unsubscribe(), the
//     end of the subscription by the server (subscriptionEnded()) or the
//     end of the connection.
//   - Events that a target sends to the connection (IpcRouter::sendEvent)
//     come as eventReceived() with subscription id 0.
//
// Lifetime
//   - closed(reason) comes once for each connection that connectToServer()
//     started, from the event loop, after the callbacks of its requests.
//     connectToServer() may then start another.
//   - Single-threaded. Never destroy the client from inside one of its
//     callbacks or signals; deleteLater() is fine there.
class PHOSPHORIPC_EXPORT IpcClient : public QObject
{
    Q_OBJECT
public:
    using Callback = std::function<void(const IpcReply& reply)>;

    enum class State {
        Unconnected,
        Connecting,
        Connected,
    };
    Q_ENUM(State)

    // The cap on the bytes written to the server that it has not read yet,
    // the requests that wait for the connection included. 16 MiB, the
    // router's own cap on what one connection may be behind on events.
    static constexpr qint64 MaxPendingWriteBytes = 16 * 1024 * 1024;
    // The longest line the client reads from the server. A longer one ends
    // the connection, so a server that never ends a line cannot grow the
    // client's memory without bound. Large results (the text of a document)
    // fit.
    static constexpr qint64 MaxLineBytes = 64 * 1024 * 1024;

    explicit IpcClient(QObject* parent = nullptr);
    ~IpcClient() override;
    Q_DISABLE_COPY_MOVE(IpcClient)

    // Starts to connect to the router's socket at `path`. connected() comes
    // when the connection is made, closed() when it fails, both from the
    // event loop. A connection that fails at once (no socket at the path)
    // leaves the client Unconnected when this returns, so the requests made
    // next fail with NOT_CONNECTED. False, with a warning, while a
    // connection is made or open.
    bool connectToServer(const QString& path);

    // Ends the connection. The requests that wait fail with
    // CONNECTION_CLOSED, and closed(reason) follows, all from the event
    // loop. Nothing happens without a connection.
    void close();

    [[nodiscard]] State state() const;
    // The path of the last connectToServer().
    [[nodiscard]] QString serverPath() const;

    // Each request returns its id, which is also the subscription's id for
    // subscribe(). The callback may be empty when the answer does not
    // matter.

    // Calls target.fn with positional arguments. The result is the method's
    // return value, null for a method that returns nothing.
    qint64 call(const QString& target, const QString& fn, const QJsonArray& args, Callback callback);
    // The names of the router's targets, as an array of strings.
    qint64 list(Callback callback);
    // The schema of one target (IpcSchemaGenerator.h).
    qint64 schema(const QString& target, Callback callback);
    // Subscribes to a signal of a target. The callback tells whether the
    // server took the subscription. Events may come only after that.
    qint64 subscribe(const QString& target, const QString& signalName, Callback callback);
    // Ends a subscription. No event of it comes after this call, whatever
    // the server answers.
    qint64 unsubscribe(qint64 subscriptionId, Callback callback);

Q_SIGNALS:
    void connected();
    // The connection ended: the server closed it, it failed, or close()
    // ran. `reason` is for people.
    void closed(const QString& reason);
    // An event. `subscriptionId` is 0 for one that a target sent to the
    // connection (IpcRouter::sendEvent), which names its target and signal
    // itself.
    void eventReceived(qint64 subscriptionId, const QString& target, const QString& signalName, const QJsonArray& args);
    // The server ended a subscription, with the error it sent (the target
    // went away: NO_SUCH_TARGET).
    void subscriptionEnded(qint64 subscriptionId, const PhosphorIpc::IpcReply& error);

private:
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

    qint64 send(QJsonObject request, Pending pending);
    void fail(Callback callback, const char* code, const QString& message);
    void onConnected();
    void onReadyRead();
    void onDisconnected();
    void onSocketError();
    // Reads the complete lines that came. False when the connection ended
    // meanwhile (a line too long, or a callback that closed the client).
    bool readLines();
    void handleLine(const QByteArray& line);
    void endConnection(const QString& reason);

    QLocalSocket* m_socket = nullptr;
    State m_state = State::Unconnected;
    QString m_path;
    // Requests made while the client connects.
    QByteArray m_unsent;
    // What came from the server after the last complete line.
    QByteArray m_input;
    // Complete lines not handled yet, and whether a handleLine() loop runs:
    // a callback that spins a nested event loop reads more lines, which
    // must wait for the ones before them.
    QList<QByteArray> m_lines;
    bool m_handlingLines = false;
    qint64 m_lastId = 0;
    QHash<qint64, Pending> m_pending;
    QHash<qint64, Subscription> m_subscriptions;
};

} // namespace PhosphorIpc
