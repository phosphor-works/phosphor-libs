// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <PhosphorIpc/phosphoripc_export.h>

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QtCore/qtclasshelpermacros.h>
#include <QtTypes>

#include <functional>
#include <memory>

namespace PhosphorIpc {

// Error codes that the client makes itself. They never travel on the
// wire, and differ from every code of ErrorCode in IpcProtocol.h. A
// reply with one of them has IpcReply::fromClient set.
namespace ClientErrorCode {
// The client had no connection, and was not making one, when the request
// was made.
constexpr auto NotConnected = "NOT_CONNECTED";
// The connection ended before the answer came: the server went away, the
// connection failed or did not come within the connect timeout, the
// server sent a line that the client cannot read, or close() ran. The
// request may have reached the server and run all the same. Do not make
// a call again on this code alone, unless a second run does no harm.
constexpr auto ConnectionClosed = "CONNECTION_CLOSED";
// The request would have put more than maxPendingWriteBytes() in the
// queue of bytes that the client has not written to the socket yet.
constexpr auto WriteQueueFull = "WRITE_QUEUE_FULL";
// The request line is longer than MaxRequestLineBytes, the most that the
// router reads. The client did not send it.
constexpr auto RequestTooLarge = "REQUEST_TOO_LARGE";
// The router would refuse the request, so the client did not send it: an
// empty target, method or signal name, more than MaxRequestArgs arguments,
// or arguments nested deeper than a JSON line holds. Or it is an
// unsubscribe with the id 0, or with an id outside -2^53 to 2^53. Also a
// request that was made from another thread.
constexpr auto InvalidRequest = "INVALID_REQUEST";
// The server answered with an error frame that has no string code.
constexpr auto MalformedReply = "MALFORMED_REPLY";
} // namespace ClientErrorCode

// The answer to one request: the result of a reply, or an error. The error
// is the server's (a code of ErrorCode, or a code of the target's own for a
// deferred call) or the client's (ClientErrorCode).
struct IpcReply
{
    // Null for an error, and for a method that returns nothing.
    QJsonValue result;
    QString errorCode;
    QString errorMessage;
    // What the error frame holds as `detail`. IpcRouter sends none today.
    QJsonObject errorDetail;
    // The client made the error, and errorCode is one of ClientErrorCode.
    // A target can choose any code for a deferred call that it fails, also
    // one that reads like a client code. This flag tells the two apart.
    bool fromClient = false;

    [[nodiscard]] bool isError() const
    {
        return !errorCode.isEmpty();
    }
};

} // namespace PhosphorIpc

Q_DECLARE_METATYPE(PhosphorIpc::IpcReply)

namespace PhosphorIpc {

// A client of the wire format of IpcRouter (IpcProtocol.h) over one
// QLocalSocket. It never blocks the event loop: each request returns
// immediately, and its callback runs when the answer comes.
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
//   - Each request gets an id of its own and returns it. A request that
//     is made from another thread, or while the client is destroyed,
//     returns 0. Ids only grow, also from one connection to the next. The
//     client matches each answer to its request, so any number of
//     requests can be in flight, and the answers can come in any order (a
//     deferred call answers after later ones). The id of subscribe() is
//     the id of the subscription. The other ids are for logs: no call
//     takes one, and a request cannot be cancelled.
//   - A request that is made while the client connects waits, and goes
//     out when the connection is made. It fails with CONNECTION_CLOSED
//     when the connection is not made. A request without a connection
//     fails with NOT_CONNECTED.
//   - The client does not send a request that the router would refuse or
//     that would make it close the connection. Such a request fails with
//     INVALID_REQUEST or REQUEST_TOO_LARGE.
//   - Each callback runs one time, from the event loop, never from inside
//     the call that made the request. When the connection ends, every
//     request that still waits fails with CONNECTION_CLOSED. An answer
//     that the client received before the end still reaches its callback.
//     The exception is a line that the client cannot read. It ends the
//     connection, and the client drops the lines that came after it.
//   - A callback is copied, and the client holds it until it runs. The
//     client does not know the objects that a callback names: guard an
//     object that can go away first (a QPointer).
//   - The client has no timeout for an answer. A deferred call can wait
//     for as long as its target takes. close() ends every wait.
//
// Callbacks that run an event loop
//   - This rule is for the callback of an answer of the server, and for a
//     slot of eventReceived() or subscriptionEnded(), on a connection that
//     is still open. While one of them runs an event loop of its own, the
//     client handles no further line of the server. Every later answer
//     and event waits until it returns. A loop that waits for one of them
//     never ends.
//   - The end of the connection does not wait. When the connection ends
//     inside such a loop, these come inside that loop: the answers and
//     events that the client had received, the failures of the requests
//     that wait, and closed().
//
// Writes
//   - The bytes that the client has not written to the socket yet wait in
//     a queue, capped at maxPendingWriteBytes(). A request past the cap
//     fails with WRITE_QUEUE_FULL, so a server that stops reading cannot
//     grow the client's memory without bound. The client writes from the
//     event loop, so the requests that are made in one turn of it all
//     count against the cap.
//
// Events
//   - subscribe() asks for the events of a signal. They come as
//     eventReceived() with the subscription's id until unsubscribe(), the
//     end of the subscription by the server (subscriptionEnded()) or the
//     end of the connection. The end of the connection ends every
//     subscription without subscriptionEnded().
//   - The router takes one subscription for each signal of a target on a
//     connection, and 256 subscriptions on a connection. It refuses more
//     with MALFORMED_REQUEST.
//   - Events that a target sends to the connection (IpcRouter::sendEvent)
//     come as eventReceived() with subscription id 0.
//
// Lifetime
//   - closed(reason) comes one time for each connection that
//     connectToServer() started, from the event loop, after the callbacks
//     of its requests. connected() comes only for a connection that is
//     still open then.
//   - state() changes inside connectToServer() and close(), before the
//     signals come. connectToServer() can start a connection as soon as
//     state() is Unconnected, also before closed() of the last one came.
//   - Destroying the client drops the callbacks that wait, without a run.
//     They can hold objects that are gone by then. What a callback holds
//     is destroyed in the destructor of the client. The destructor of
//     such an object must not run an event loop or delete the client's
//     parent. A request that it makes is dropped.
//   - Single-threaded. Every function runs in the thread of the client.
//     Never destroy the client from inside one of its callbacks or
//     signals. deleteLater() is fine there, but not inside an event loop
//     that such a callback runs.
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

    // 16 MiB, the router's own cap on what one connection can be behind on
    // events.
    static constexpr qint64 DefaultMaxPendingWriteBytes = 16 * 1024 * 1024;
    // 64 MiB. Large results (the text of a document) fit.
    static constexpr qint64 DefaultMaxReplyLineBytes = 64 * 1024 * 1024;
    // 5 seconds. A local socket connects immediately. Only a server whose
    // queue of connections is full makes a client wait.
    static constexpr int DefaultConnectTimeoutMs = 5000;

    explicit IpcClient(QObject* parent = nullptr);
    ~IpcClient() override;
    Q_DISABLE_COPY_MOVE(IpcClient)

    // The cap on the bytes of requests that the client has not written to
    // the socket yet, the requests that wait for the connection included.
    // At least 1.
    void setMaxPendingWriteBytes(qint64 bytes);
    [[nodiscard]] qint64 maxPendingWriteBytes() const;

    // The longest line that the client reads from the server, without its
    // newline. A longer one ends the connection, so a server that never
    // ends a line cannot grow the client's memory without bound. From 1
    // to the largest qint64 less 1, and a value outside becomes the
    // nearest of the two. The cap on a request line is not a setting. It
    // is the router's, MaxRequestLineBytes.
    void setMaxReplyLineBytes(qint64 bytes);
    [[nodiscard]] qint64 maxReplyLineBytes() const;

    // How long connectToServer() waits for the connection, in
    // milliseconds. An attempt that takes longer ends as a failed one. 0
    // or less waits without a limit: Qt itself reports no failure for a
    // server that never accepts the connection.
    void setConnectTimeout(int milliseconds);
    [[nodiscard]] int connectTimeout() const;

    // Starts to connect to the router's socket at `path`. Give an absolute
    // path, as from IpcRouter::socketPath() or
    // IpcRouter::socketPathForApp(): a name that does not start with '/'
    // is a file in the temporary directory, as QLocalSocket takes it.
    // connected() comes when the connection is made, closed() when it
    // fails, both from the event loop. A connection that fails
    // immediately (no socket at the path) leaves the client Unconnected
    // when this returns, so the requests made next fail with
    // NOT_CONNECTED. False, with a warning, while a connection is made or
    // open, and from another thread. False while the client is destroyed.
    bool connectToServer(const QString& path);

    // Ends the connection. The requests that wait fail with
    // CONNECTION_CLOSED, and closed(reason) follows, all from the event
    // loop. A request that was made in the same turn of the event loop
    // may never reach the server: the client writes from the event loop,
    // and close() drops what is not written. Nothing happens without a
    // connection.
    void close();

    [[nodiscard]] State state() const;
    // The path of the last connectToServer(). It is the server's path as
    // this client was given it, where IpcRouter::socketPath() is the path
    // that a router listens on.
    [[nodiscard]] QString serverPath() const;

    // The callback of each request can be empty when the answer does not
    // matter.

    // Calls target.fn with positional arguments. The result is the method's
    // return value, null for a method that returns nothing. A number that
    // is not finite (NaN, an infinity) is null on the wire, in arguments
    // and in results.
    qint64 call(const QString& target, const QString& fn, const QJsonArray& args, Callback callback = {});
    // The names of the router's targets, as an array of strings.
    qint64 list(Callback callback);
    // The schema of one target (IpcSchemaGenerator.h).
    qint64 schema(const QString& target, Callback callback);
    // Subscribes to a signal of a target. The callback tells whether the
    // server took the subscription. Events come only after that.
    qint64 subscribe(const QString& target, const QString& signalName, Callback callback = {});
    // Ends a subscription. No event of it comes after this call, whatever
    // the server answers. That holds also for an event that waits in the
    // event loop after the end of a connection. When the client cannot
    // send the request (it fails with a client code), the server keeps the
    // subscription until the connection ends, and refuses a new one to the
    // same signal.
    qint64 unsubscribe(qint64 subscriptionId, Callback callback = {});

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
    struct Private;
    std::unique_ptr<Private> d;
};

} // namespace PhosphorIpc
