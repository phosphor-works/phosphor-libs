// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <PhosphorMcp/McpCall.h>
#include <PhosphorMcp/McpResult.h>
#include <PhosphorMcp/McpTool.h>
#include <PhosphorMcp/phosphormcp_export.h>

#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QObject>
#include <QString>
#include <QtCore/qtclasshelpermacros.h>

#include <functional>
#include <memory>
#include <optional>

namespace PhosphorMcp {

class McpTransport;

// An MCP server over stdio (protocol revision 2026-07-28) that offers
// tools. The application names itself, declares its tools with their
// handlers, and starts the server in its QCoreApplication:
//
//     PhosphorMcp::McpServer server;
//     server.setServerInfo(QStringLiteral("example"), QStringLiteral("1.0"));
//     server.setCache(60 * 60 * 1000, PhosphorMcp::McpServer::CacheScope::Public);
//     server.addTool(tool, [](PhosphorMcp::McpCall call) { call.finish(...); });
//     server.handleTerminationSignals();
//     using Reason = PhosphorMcp::McpServer::FinishReason;
//     QObject::connect(&server, &PhosphorMcp::McpServer::finished, &app, [](Reason reason) {
//         // A failed read or write is an error. The other reasons are ends.
//         QCoreApplication::exit(reason == Reason::InputFailed || reason == Reason::OutputFailed ? 1 : 0);
//     });
//     // A server that does not start never finishes, so the application
//     // must not wait for it.
//     if (!server.start())
//         return 1;
//     return app.exec();
//
// The server answers server/discover, tools/list and tools/call, and does
// every check of the protocol: the _meta of each request, the version, the
// client capabilities a tool needs, the arguments against the tool's input
// schema. It keeps nothing from one request for the next. It offers no
// resources, prompts, logging or subscriptions, and sends no requests to the
// client. It writes nothing to stdout but protocol messages. Its diagnostics
// go to the logging category "phosphormcp", which Qt writes to stderr when
// the process has a terminal or QT_FORCE_STDERR_LOGGING=1 is set, and
// otherwise to the systemd journal when Qt is built with it. A server that
// an MCP client starts often has no terminal (see the README, "Wire
// protocol").
//
// Single-threaded: use it on the thread that owns it. Never destroy it from
// inside a handler or a cancel callback, but deleteLater() is fine. A
// handler that runs a nested event loop holds back every request after its
// own until it returns, a cancel of its own call and the end of stdin
// included. Answer later through the McpCall instead.
// Handlers and cancel callbacks must not throw. The library is built
// without exceptions, and an exception that leaves a callback is undefined
// behavior.
//
// Destroying a server that runs ends it at once, without finished(). It
// writes nothing more, runs no cancel callback, and the calls it leaves
// read isCancelled() true. The descriptors stay open, as they are the
// caller's.
class PHOSPHORMCP_EXPORT McpServer : public QObject
{
    Q_OBJECT
public:
    // Whether one client's copy of the answers of server/discover and
    // tools/list may serve other clients (setCache()).
    enum class CacheScope {
        Public,
        Private,
    };
    Q_ENUM(CacheScope)

    enum class FinishReason {
        // stdin reached its end. The client is done.
        EndOfInput,
        // A read of stdin failed.
        InputFailed,
        // The client closed stdout (EPIPE).
        OutputClosed,
        // SIGTERM, with handleTerminationSignals().
        Terminated,
        // SIGHUP, with handleTerminationSignals().
        HangUp,
        // A write to stdout failed for another reason than its reader going
        // away (a full disk, an I/O error). The reason is in a warning.
        OutputFailed,
    };
    Q_ENUM(FinishReason)

    // After the end of stdin, how long the server waits for the client to
    // read more of the replies already queued. The wait starts again each
    // time the client reads some, so a slow reader of a large reply gets it
    // whole, and a client that reads nothing for this long loses the rest.
    static constexpr int DrainTimeoutMs = 2000;

    using Handler = std::function<void(McpCall call)>;

    explicit McpServer(QObject* parent = nullptr);
    ~McpServer() override;
    Q_DISABLE_COPY_MOVE(McpServer)

    // The name and version in each result's _meta. By default, or when
    // empty, the application's name (QCoreApplication sets it to the
    // executable's name unless the application gives one) and version ("0"
    // when the application gives none).
    void setServerInfo(const QString& name, const QString& version);
    // The instructions of server/discover. Omitted when empty.
    void setInstructions(const QString& instructions);
    // ttlMs and cacheScope of server/discover and tools/list. The default
    // is 0 and Private. False, and nothing changed, for a negative ttl.
    bool setCache(qint64 ttlMs, CacheScope scope);
    // The longest request line, without its newline. A longer one is
    // answered with an Invalid Request error, and dropped. A value under 1
    // counts as 1. McpTransport::DefaultMaxLineBytes by
    // default. Takes effect at once, on a running server too.
    void setMaxLineBytes(qint64 bytes);
    // The high-water mark of the replies that wait for the client to read
    // them. Past it, the server reads no more requests until the client took
    // half of them, and progress notifications are dropped. Replies are
    // never dropped. A value under 1 counts as 1.
    // McpTransport::DefaultMaxPendingWriteBytes (64 MiB) by default. Takes
    // effect at once, on a running server too.
    void setMaxPendingWriteBytes(qint64 bytes);

    // Adds a tool, in the order tools/list shows them. Returns false, with
    // the reason in `error` and a warning, for a name that is not 1 to 128
    // of [A-Za-z0-9_.-] or that another tool has, for a schema that the
    // server cannot check arguments against (see the README, "Schemas"), for
    // required client capabilities that hold anything but objects (no client
    // could declare them), for a handler that is empty, and after start(),
    // since the list stays the same for the life of the server.
    bool addTool(const McpTool& tool, Handler handler, QString* error = nullptr);
    // The tools added, in the order tools/list shows them.
    [[nodiscard]] QList<McpTool> tools() const;

    // SIGTERM and SIGHUP end the server (finished(Terminated) or
    // finished(HangUp)). Signal dispositions are global to the process, so
    // this is the application's choice. A signal that the process ignores
    // when this runs (nohup ignores SIGHUP) stays ignored. The old
    // dispositions come back as soon as the server ends without waiting for
    // its replies (a signal, a closed or failed output), before the cancel
    // callbacks run. A signal during a slow callback or teardown then has its
    // usual effect: the second one after a signal, and the first one after a
    // closed or failed output. After the end of stdin or a failed read they
    // come back when the server finishes, at the latest when it is destroyed,
    // so that a signal cuts the drain short. The server reads a signal from
    // its event loop, so one that comes while a cancel callback blocks
    // without one is read only after the callbacks returned, if the server
    // has not finished by then. A handler that someone else installed since
    // stays. Call it after QCoreApplication exists, and before the server
    // ends.
    //
    // True when the server handles the signals, also when it did before.
    // False, with a warning, when another server handles them, when the
    // server's thread has no event dispatcher, when the self-pipe cannot be
    // made, and after the server ended.
    //
    // SIGPIPE is left alone, here and everywhere in the library: an ignored
    // SIGPIPE survives exec, and the programs the application starts would
    // inherit it. The transport keeps a closed stdout from raising it.
    //
    // The handler belongs to the process that called this. A child forked
    // after it, before an exec, takes the old dispositions. So a process
    // that forks to keep the server running in the child (a daemon) calls
    // this in the child, after the fork, and not in the parent before it.
    // After a call in the parent, a call in the child returns false, with a
    // warning, while the child holds its copy of that server, since the
    // handler stays the parent's. Once the child destroyed its copy, a server in the
    // child takes the signals.
    bool handleTerminationSignals();

    // Reads requests on stdin and writes replies on stdout. A server with no
    // tool starts too, with a warning, and offers no tools capability.
    [[nodiscard]] bool start();
    // The same on other descriptors, which stay the caller's. One
    // descriptor may do both. False, with a warning, when the server was
    // started before, when it ended before it started (a signal, whose
    // finished() came already), and when the transport does not open. In
    // the last case finished() never comes.
    [[nodiscard]] bool start(int readFd, int writeFd);
    // True from start() until the server ends, which is before finished()
    // when replies still drain.
    [[nodiscard]] bool isRunning() const;

    // The transport, for its read-only state (pendingWriteBytes(),
    // isReadingPaused()), from start() until finished(), else nullptr. It
    // stays during the drain after the end of stdin.
    [[nodiscard]] const McpTransport* transport() const;

Q_SIGNALS:
    // The server ended and reads and writes nothing more. Comes once, from
    // the event loop. Every call still running was cancelled first (its
    // cancel callback ran). After the end of stdin or a failed read it
    // comes when the replies already queued were written, or when the
    // client read none of them for DrainTimeoutMs. A signal or a closed
    // output during that wait ends it at once, with its own reason. One
    // that comes while the cancel callbacks run, and that a nested event
    // loop of a callback reads, gives its reason too, but finished() comes
    // only after the last callback returned. The signals are put back at
    // once then, so a second signal, during a callback that hangs, has its
    // usual effect.
    void finished(PhosphorMcp::McpServer::FinishReason reason);

private:
    friend class McpCall;

    struct Private;
    std::unique_ptr<Private> d;

    void onLine(const QByteArray& line);
    void handleRequest(const QJsonValue& id, const QString& method, const QJsonObject& params);
    void handleNotification(const QString& method, const QJsonObject& params);
    void handleDiscover(const QJsonValue& id);
    void handleToolsList(const QJsonValue& id, const QJsonObject& params);
    void handleToolsCall(const QJsonValue& id, const QJsonObject& params);
    // Checks params._meta. Sends the error and returns false when it fails.
    bool checkMeta(const QJsonValue& id, const QJsonObject& params);

    // Each returns whether the message was queued. An undefined id leaves
    // the id out of an error, for a request whose id cannot be read.
    bool sendResult(const QJsonValue& id, QJsonObject result);
    bool sendError(const QJsonValue& id, int code, const QString& message, const QJsonValue& data = QJsonValue());
    bool sendMessage(const QJsonObject& message);

    void end(FinishReason reason, bool drain);
    void drainThenFinish(FinishReason reason);
    void finishSoon(FinishReason reason);
    void emitFinished(FinishReason reason);
    void cancelCall(quint64 token);
    void runCancelCallback(quint64 token);

    // For McpCall.
    bool callIsValid(quint64 token) const;
    bool callIsCancelled(quint64 token) const;
    bool finishCall(quint64 token, const McpResult& result);
    bool progressCall(quint64 token, double progress, std::optional<double> total, const QString& message);
    void setCancelCallback(quint64 token, std::function<void()> callback);
    void abandonCall(quint64 token);
};

} // namespace PhosphorMcp
