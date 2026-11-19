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
//     server.setCache(60000, PhosphorMcp::McpServer::CacheScope::Public);
//     server.addTool(tool, [](PhosphorMcp::McpCall call) { call.finish(...); });
//     server.handleTerminationSignals();
//     QObject::connect(&server, &PhosphorMcp::McpServer::finished, &app, &QCoreApplication::quit);
//     server.start();
//     return app.exec();
//
// The server answers server/discover, tools/list and tools/call, and does
// every check of the protocol: the _meta of each request, the version, the
// client capabilities a tool needs, the arguments against the tool's input
// schema. It keeps nothing from one request for the next. It offers no
// resources, prompts, logging or subscriptions, sends no requests to the
// client, and writes nothing to stdout but protocol messages: its
// diagnostics go to the logging category "phosphormcp", on stderr.
//
// Single-threaded: use it on the thread that owns it. Never destroy it from
// inside a handler or a cancel callback; deleteLater() is fine.
class PHOSPHORMCP_EXPORT McpServer : public QObject
{
    Q_OBJECT
public:
    // How long a client may keep the answers of server/discover and
    // tools/list, and whether one client's copy serves others.
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
        // stdout went away, or a write to it failed.
        OutputClosed,
        // SIGTERM, with handleTerminationSignals().
        Terminated,
        // SIGHUP, with handleTerminationSignals().
        HangUp,
    };
    Q_ENUM(FinishReason)

    // How long the server waits, after the end of stdin, for the client to
    // read the replies already queued.
    static constexpr int DrainTimeoutMs = 2000;

    using Handler = std::function<void(McpCall call)>;

    explicit McpServer(QObject* parent = nullptr);
    ~McpServer() override;
    Q_DISABLE_COPY_MOVE(McpServer)

    // The name and version in each result's _meta. The default is the
    // application's name and version.
    void setServerInfo(const QString& name, const QString& version);
    // The instructions of server/discover. Omitted when empty.
    void setInstructions(const QString& instructions);
    // ttlMs and cacheScope of server/discover and tools/list. The default
    // is 0 and Private. False, and nothing changed, for a negative ttl.
    bool setCache(qint64 ttlMs, CacheScope scope);
    // The longest request line, without its newline. A longer one is
    // answered with an Invalid Request error, and dropped.
    // McpTransport::DefaultMaxLineBytes by default.
    void setMaxLineBytes(qint64 bytes);
    // The high-water mark of the replies that wait for the client to read
    // them. Past it, the server reads no more requests until the client took
    // half of them. Replies are never dropped.
    // McpTransport::DefaultMaxPendingWriteBytes (64 MiB) by default.
    void setMaxPendingWriteBytes(qint64 bytes);

    // Adds a tool, in the order tools/list shows them. Returns false, with
    // the reason in `error` and a warning, for a name that is not 1 to 128
    // of [A-Za-z0-9_.-] or that another tool has, for a schema that the
    // server cannot check arguments against (see the README, "Schemas"), for
    // a handler that is empty, and after start(): the list stays the same
    // for the life of the server.
    bool addTool(const McpTool& tool, Handler handler, QString* error = nullptr);
    [[nodiscard]] QList<McpTool> tools() const;

    // SIGTERM and SIGHUP end the server (finished(Terminated) or
    // finished(HangUp)). Signal dispositions are global to the process, so
    // this is the application's choice. The old dispositions come back when
    // the server is destroyed. False, with a warning, when another server
    // handles them, or when the self-pipe cannot be made.
    //
    // SIGPIPE is left alone, here and everywhere in the library: an ignored
    // SIGPIPE survives exec, and the programs the application starts would
    // inherit it. The transport keeps a closed stdout from raising it.
    bool handleTerminationSignals();

    // Reads requests on stdin and writes replies on stdout.
    bool start();
    // The same on other descriptors, which stay the caller's. One
    // descriptor may do both. False when the server was started before, or
    // the transport does not open.
    bool start(int readFd, int writeFd);
    [[nodiscard]] bool isRunning() const;

    // The transport while the server runs, else nullptr.
    [[nodiscard]] McpTransport* transport() const;

Q_SIGNALS:
    // The server ended and reads and writes nothing more. Comes once, from
    // the event loop. Every call still running was cancelled first (its
    // cancel callback ran). After the end of stdin it comes when the
    // replies already queued were written, or after DrainTimeoutMs.
    void finished(PhosphorMcp::McpServer::FinishReason reason);

private:
    friend class McpCall;

    struct Private;
    std::unique_ptr<Private> d;

    void onLine(const QByteArray& line);
    void handleRequest(const QJsonValue& id, const QString& method, const QJsonObject& params, bool hasParams);
    void handleNotification(const QString& method, const QJsonObject& params);
    void handleDiscover(const QJsonValue& id);
    void handleToolsList(const QJsonValue& id, const QJsonObject& params);
    void handleToolsCall(const QJsonValue& id, const QJsonObject& params);
    // Checks params._meta. Sends the error and returns false when it fails.
    bool checkMeta(const QJsonValue& id, const QJsonObject& params, bool hasParams);

    void sendResult(const QJsonValue& id, QJsonObject result);
    void sendError(const QJsonValue& id, int code, const QString& message, const QJsonValue& data = QJsonValue());
    void sendMessage(const QJsonObject& message);

    void end(FinishReason reason, bool drain);
    void emitFinished(FinishReason reason);

    // For McpCall.
    bool callIsValid(quint64 token) const;
    bool callIsCancelled(quint64 token) const;
    bool finishCall(quint64 token, const McpResult& result);
    bool progressCall(quint64 token, double progress, std::optional<double> total, const QString& message);
    void setCancelCallback(quint64 token, std::function<void()> callback);
    void abandonCall(quint64 token);
};

} // namespace PhosphorMcp
