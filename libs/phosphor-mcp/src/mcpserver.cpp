// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorMcp/McpProtocol.h>
#include <PhosphorMcp/McpServer.h>
#include <PhosphorMcp/McpTransport.h>

#include "mcplogging.h"
#include "mcpschema.h"
#include "mcpsignals.h"

#include <QCborValue>
#include <QCoreApplication>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLatin1StringView>
#include <QMetaObject>
#include <QRegularExpression>
#include <QTimer>

#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstring>
#include <utility>

#include <unistd.h>

namespace PhosphorMcp {

namespace {

const QLatin1StringView MethodDiscover("server/discover");
const QLatin1StringView MethodToolsList("tools/list");
const QLatin1StringView MethodToolsCall("tools/call");
const QLatin1StringView MethodInitialize("initialize");
const QLatin1StringView NotificationCancelled("notifications/cancelled");

// JSON-RPC ids are strings or numbers, and MCP rules out null. A number
// must be an integer.
bool isValidId(const QJsonValue& id)
{
    if (id.isString()) {
        return true;
    }
    if (!id.isDouble()) {
        return false;
    }
    const double value = id.toDouble();
    return std::isfinite(value) && std::trunc(value) == value;
}

// A key that tells a string id from a number id with the same text. An
// integer id keeps all its digits: through a double, ids past 2^53 would
// share a key (9007199254740992 and ...993).
QString idKey(const QJsonValue& id)
{
    if (id.isString()) {
        return QLatin1Char('s') + id.toString();
    }
    const QCborValue number = QCborValue::fromJsonValue(id);
    if (number.isInteger()) {
        return QLatin1Char('n') + QString::number(number.toInteger());
    }
    return QLatin1Char('n') + QString::number(id.toDouble(), 'g', 17);
}

bool isValidToolName(const QString& name)
{
    // Anchored at the very end: "$" would also match before a final "\n".
    static const QRegularExpression pattern(QRegularExpression::anchoredPattern(u"[A-Za-z0-9_.-]{1,128}"));
    return pattern.match(name).hasMatch();
}

// Each capability that `required` names must be declared as an object, as
// the spec declares every capability ({} for one without options), and for
// a capability that is a non-empty object, each of its own keys too. null,
// false or a string does not declare a capability.
bool hasCapabilities(const QJsonObject& declared, const QJsonObject& required)
{
    for (auto it = required.begin(); it != required.end(); ++it) {
        const QJsonValue have = declared.value(it.key());
        if (!have.isObject()) {
            return false;
        }
        if (it.value().isObject() && !hasCapabilities(have.toObject(), it.value().toObject())) {
            return false;
        }
    }
    return true;
}

QString cacheScopeName(McpServer::CacheScope scope)
{
    return scope == McpServer::CacheScope::Public ? QStringLiteral("public") : QStringLiteral("private");
}

struct ToolEntry
{
    McpTool tool;
    McpServer::Handler handler;
    std::shared_ptr<McpSchema> input;
    // Null for a tool without an output schema.
    std::shared_ptr<McpSchema> output;
};

struct PendingCall
{
    QJsonValue id;
    QString idKey;
    qsizetype tool = 0;
    QJsonValue progressToken;
    std::optional<double> lastProgress;
    std::function<void()> onCancelled;
    // Kept after a cancellation until the McpCall goes, so that it can
    // still say that it was cancelled.
    bool cancelled = false;
};

} // namespace

struct McpServer::Private
{
    QString name;
    QString version;
    QString instructions;
    qint64 ttlMs = 0;
    CacheScope cacheScope = CacheScope::Private;
    qint64 maxLineBytes = McpTransport::DefaultMaxLineBytes;
    qint64 maxPendingWriteBytes = McpTransport::DefaultMaxPendingWriteBytes;

    QList<ToolEntry> tools;
    QHash<QString, qsizetype> toolIndex;
    QJsonArray toolsJson;

    McpTransport* transport = nullptr;
    bool started = false;
    bool ending = false;
    bool finished = false;

    QHash<quint64, PendingCall> calls;
    quint64 nextToken = 1;

    std::unique_ptr<McpTerminationSignals> terminationSignals;

    QJsonObject resultMeta() const
    {
        QString serverName = name.isEmpty() ? QCoreApplication::applicationName() : name;
        if (serverName.isEmpty()) {
            serverName = QStringLiteral("phosphor-mcp");
        }
        QString serverVersion = version.isEmpty() ? QCoreApplication::applicationVersion() : version;
        if (serverVersion.isEmpty()) {
            serverVersion = QStringLiteral("0");
        }
        return QJsonObject{
            {MetaServerInfo,
             QJsonObject{
                 {QLatin1StringView("name"), serverName},
                 {QLatin1StringView("version"), serverVersion},
             }},
        };
    }

    // The active call with this request id, or 0.
    quint64 activeCall(const QString& key) const
    {
        for (auto it = calls.begin(); it != calls.end(); ++it) {
            if (!it->cancelled && it->idKey == key) {
                return it.key();
            }
        }
        return 0;
    }
};

McpServer::McpServer(QObject* parent)
    : QObject(parent)
    , d(std::make_unique<Private>())
{
}

McpServer::~McpServer()
{
    // Nothing goes out from here on, and the cancel callbacks do not run:
    // the application is tearing down. The calls leave the table before
    // they are destroyed, because a callback may own an McpCall, whose
    // destructor looks its call up.
    d->ending = true;
    d->finished = true;
    const QHash<quint64, PendingCall> calls = std::exchange(d->calls, {});
    Q_UNUSED(calls)
    d->terminationSignals.reset();
}

void McpServer::setServerInfo(const QString& name, const QString& version)
{
    d->name = name;
    d->version = version;
}

void McpServer::setInstructions(const QString& instructions)
{
    d->instructions = instructions;
}

bool McpServer::setCache(qint64 ttlMs, CacheScope scope)
{
    if (ttlMs < 0) {
        qCWarning(lcMcp, "McpServer::setCache: a negative ttl (%lld ms)", static_cast<long long>(ttlMs));
        return false;
    }
    d->ttlMs = ttlMs;
    d->cacheScope = scope;
    return true;
}

void McpServer::setMaxLineBytes(qint64 bytes)
{
    d->maxLineBytes = qMax<qint64>(1, bytes);
    if (d->transport) {
        d->transport->setMaxLineBytes(d->maxLineBytes);
    }
}

void McpServer::setMaxPendingWriteBytes(qint64 bytes)
{
    d->maxPendingWriteBytes = qMax<qint64>(1, bytes);
    if (d->transport) {
        d->transport->setMaxPendingWriteBytes(d->maxPendingWriteBytes);
    }
}

bool McpServer::addTool(const McpTool& tool, Handler handler, QString* error)
{
    const auto refuse = [&](const QString& why) {
        qCWarning(lcMcp, "McpServer::addTool: %s: %s", qPrintable(tool.name()), qPrintable(why));
        if (error) {
            *error = why;
        }
        return false;
    };
    if (d->started) {
        return refuse(QStringLiteral("the server has started: its tool list does not change"));
    }
    if (!isValidToolName(tool.name())) {
        return refuse(QStringLiteral("a tool name is 1 to 128 of A-Z, a-z, 0-9, '_', '-' and '.'"));
    }
    if (d->toolIndex.contains(tool.name())) {
        return refuse(QStringLiteral("another tool has the name"));
    }
    if (!handler) {
        return refuse(QStringLiteral("the handler is empty"));
    }
    auto input = std::make_shared<McpSchema>();
    QString why = input->compile(tool.inputSchema());
    if (!why.isEmpty()) {
        return refuse(QStringLiteral("inputSchema%1").arg(why));
    }
    std::shared_ptr<McpSchema> output;
    if (!tool.outputSchema().isEmpty()) {
        output = std::make_shared<McpSchema>();
        why = output->compile(tool.outputSchema());
        if (!why.isEmpty()) {
            return refuse(QStringLiteral("outputSchema%1").arg(why));
        }
    }
    d->toolIndex.insert(tool.name(), d->tools.size());
    d->tools.append(ToolEntry{tool, std::move(handler), std::move(input), std::move(output)});
    if (error) {
        error->clear();
    }
    return true;
}

QList<McpTool> McpServer::tools() const
{
    QList<McpTool> list;
    list.reserve(d->tools.size());
    for (const ToolEntry& entry : d->tools) {
        list.append(entry.tool);
    }
    return list;
}

bool McpServer::handleTerminationSignals()
{
    if (d->terminationSignals) {
        return true;
    }
    d->terminationSignals = McpTerminationSignals::install([this](int signal) {
        end(signal == SIGHUP ? FinishReason::HangUp : FinishReason::Terminated, false);
    });
    return d->terminationSignals != nullptr;
}

bool McpServer::start()
{
    return start(STDIN_FILENO, STDOUT_FILENO);
}

bool McpServer::start(int readFd, int writeFd)
{
    if (d->started || d->ending) {
        qCWarning(lcMcp, "McpServer::start: started or ended before");
        return false;
    }
    auto* transport = new McpTransport(readFd, writeFd, this);
    transport->setMaxLineBytes(d->maxLineBytes);
    transport->setMaxPendingWriteBytes(d->maxPendingWriteBytes);
    if (!transport->open()) {
        delete transport;
        return false;
    }
    d->transport = transport;
    d->started = true;
    for (const ToolEntry& entry : std::as_const(d->tools)) {
        d->toolsJson.append(entry.tool.toJson());
    }
    connect(transport, &McpTransport::lineReceived, this, &McpServer::onLine);
    connect(transport, &McpTransport::lineTooLong, this, [this] {
        sendError(QJsonValue::Null, ErrorCode::InvalidRequest,
                  QStringLiteral("Invalid Request: the message is longer than %1 bytes").arg(d->maxLineBytes));
    });
    connect(transport, &McpTransport::endOfInput, this, [this] {
        end(FinishReason::EndOfInput, true);
    });
    connect(transport, &McpTransport::inputFailed, this, [this](int error) {
        qCWarning(lcMcp, "McpServer: reading the input failed: %s", strerror(error));
        end(FinishReason::InputFailed, false);
    });
    connect(transport, &McpTransport::outputClosed, this, [this](int error) {
        if (error != EPIPE) {
            qCWarning(lcMcp, "McpServer: writing the output failed: %s", strerror(error));
        }
        end(FinishReason::OutputClosed, false);
    });
    return true;
}

bool McpServer::isRunning() const
{
    return d->started && !d->ending;
}

McpTransport* McpServer::transport() const
{
    return d->finished ? nullptr : d->transport;
}

void McpServer::onLine(const QByteArray& line)
{
    if (d->ending) {
        return;
    }
    if (line.trimmed().isEmpty()) {
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        sendError(QJsonValue::Null, ErrorCode::ParseError,
                  QStringLiteral("Parse error: %1 at offset %2").arg(parseError.errorString()).arg(parseError.offset));
        return;
    }
    if (document.isArray()) {
        sendError(QJsonValue::Null, ErrorCode::InvalidRequest,
                  QStringLiteral("Invalid Request: batches are not supported"));
        return;
    }
    const QJsonObject message = document.object();
    const bool hasId = message.contains(QLatin1StringView("id"));
    const QJsonValue id = message.value(QLatin1StringView("id"));
    const QJsonValue replyId = hasId && isValidId(id) ? id : QJsonValue(QJsonValue::Null);

    if (message.value(QLatin1StringView("jsonrpc")) != QJsonValue(QStringLiteral("2.0"))) {
        sendError(replyId, ErrorCode::InvalidRequest, QStringLiteral("Invalid Request: jsonrpc must be \"2.0\""));
        return;
    }
    if (!message.contains(QLatin1StringView("method"))) {
        if (message.contains(QLatin1StringView("result")) || message.contains(QLatin1StringView("error"))) {
            // The server sends no requests, so no response is awaited.
            qCDebug(lcMcp, "McpServer: dropped a response that no request asked for");
            return;
        }
        sendError(replyId, ErrorCode::InvalidRequest, QStringLiteral("Invalid Request: no method"));
        return;
    }
    const QJsonValue method = message.value(QLatin1StringView("method"));
    if (!method.isString()) {
        sendError(replyId, ErrorCode::InvalidRequest, QStringLiteral("Invalid Request: method must be a string"));
        return;
    }
    const bool hasParams = message.contains(QLatin1StringView("params"));
    const QJsonValue params = message.value(QLatin1StringView("params"));
    if (!hasId) {
        if (params.isObject() || !hasParams) {
            handleNotification(method.toString(), params.toObject());
        }
        return;
    }
    if (!isValidId(id)) {
        sendError(QJsonValue::Null, ErrorCode::InvalidRequest,
                  QStringLiteral("Invalid Request: id must be a string or an integer"));
        return;
    }
    if (method.toString() == MethodInitialize) {
        QJsonObject data{{QLatin1StringView("supported"), QJsonArray{ProtocolVersion}}};
        const QJsonValue requested = params.toObject().value(QLatin1StringView("protocolVersion"));
        if (requested.isString()) {
            data.insert(QLatin1StringView("requested"), requested);
        }
        sendError(id, ErrorCode::MethodNotFound,
                  QStringLiteral("This server supports MCP %1 only, which has no initialize: use server/discover")
                      .arg(ProtocolVersion),
                  data);
        return;
    }
    if (hasParams && !params.isObject()) {
        // Only for the methods the server offers: an unknown method is
        // -32601 whatever its params.
        const QString name = method.toString();
        if (name == MethodDiscover || name == MethodToolsList || name == MethodToolsCall) {
            sendError(id, ErrorCode::InvalidParams, QStringLiteral("Invalid params: params must be an object"));
            return;
        }
    }
    handleRequest(id, method.toString(), params.toObject(), hasParams);
}

void McpServer::handleRequest(const QJsonValue& id, const QString& method, const QJsonObject& params, bool hasParams)
{
    if (method != MethodDiscover && method != MethodToolsList && method != MethodToolsCall) {
        sendError(id, ErrorCode::MethodNotFound, QStringLiteral("Method not found: %1").arg(method));
        return;
    }
    if (!checkMeta(id, params, hasParams)) {
        return;
    }
    if (method == MethodDiscover) {
        handleDiscover(id);
    } else if (method == MethodToolsList) {
        handleToolsList(id, params);
    } else {
        handleToolsCall(id, params);
    }
}

void McpServer::handleNotification(const QString& method, const QJsonObject& params)
{
    // notifications/initialized and every other notification: nothing to
    // do, and a notification is never answered.
    if (method != NotificationCancelled) {
        return;
    }
    const QJsonValue requestId = params.value(QLatin1StringView("requestId"));
    if (!isValidId(requestId)) {
        return;
    }
    // A call that already finished, or an id the server never saw: the
    // spec has both cases ignored, since the cancel and the reply may cross.
    const quint64 token = d->activeCall(idKey(requestId));
    if (token == 0) {
        return;
    }
    PendingCall& call = d->calls[token];
    call.cancelled = true;
    std::function<void()> callback = std::exchange(call.onCancelled, nullptr);
    if (callback) {
        callback();
    }
}

bool McpServer::checkMeta(const QJsonValue& id, const QJsonObject& params, bool hasParams)
{
    const QJsonValue meta = params.value(QLatin1StringView("_meta"));
    if (!hasParams || !meta.isObject()) {
        sendError(id, ErrorCode::InvalidParams, QStringLiteral("Invalid params: params._meta is required"));
        return false;
    }
    const QJsonObject metaObject = meta.toObject();
    const QJsonValue version = metaObject.value(MetaProtocolVersion);
    if (!version.isString()) {
        sendError(id, ErrorCode::InvalidParams,
                  QStringLiteral("Invalid params: params._meta[\"%1\"] is required").arg(MetaProtocolVersion));
        return false;
    }
    if (!metaObject.value(MetaClientCapabilities).isObject()) {
        sendError(id, ErrorCode::InvalidParams,
                  QStringLiteral("Invalid params: params._meta[\"%1\"] is required").arg(MetaClientCapabilities));
        return false;
    }
    if (metaObject.contains(MetaProgressToken) && !isValidId(metaObject.value(MetaProgressToken))) {
        sendError(id, ErrorCode::InvalidParams,
                  QStringLiteral("Invalid params: params._meta.progressToken must be a string or an integer"));
        return false;
    }
    if (version.toString() != ProtocolVersion) {
        sendError(id, ErrorCode::UnsupportedProtocolVersion, QStringLiteral("Unsupported protocol version"),
                  QJsonObject{
                      {QLatin1StringView("supported"), QJsonArray{ProtocolVersion}},
                      {QLatin1StringView("requested"), version},
                  });
        return false;
    }
    return true;
}

void McpServer::handleDiscover(const QJsonValue& id)
{
    QJsonObject result{
        {QLatin1StringView("supportedVersions"), QJsonArray{ProtocolVersion}},
        {QLatin1StringView("capabilities"), QJsonObject{{QLatin1StringView("tools"), QJsonObject()}}},
        {QLatin1StringView("ttlMs"), d->ttlMs},
        {QLatin1StringView("cacheScope"), cacheScopeName(d->cacheScope)},
    };
    if (!d->instructions.isEmpty()) {
        result.insert(QLatin1StringView("instructions"), d->instructions);
    }
    sendResult(id, result);
}

void McpServer::handleToolsList(const QJsonValue& id, const QJsonObject& params)
{
    // The whole list goes out in one page, so no cursor names a page.
    if (params.contains(QLatin1StringView("cursor"))) {
        sendError(id, ErrorCode::InvalidParams, QStringLiteral("Invalid params: no such cursor"));
        return;
    }
    sendResult(id,
               QJsonObject{
                   {QLatin1StringView("tools"), d->toolsJson},
                   {QLatin1StringView("ttlMs"), d->ttlMs},
                   {QLatin1StringView("cacheScope"), cacheScopeName(d->cacheScope)},
               });
}

void McpServer::handleToolsCall(const QJsonValue& id, const QJsonObject& params)
{
    const QJsonValue nameValue = params.value(QLatin1StringView("name"));
    if (!nameValue.isString()) {
        sendError(id, ErrorCode::InvalidParams, QStringLiteral("Invalid params: name must be a string"));
        return;
    }
    const QString name = nameValue.toString();
    const auto found = d->toolIndex.constFind(name);
    if (found == d->toolIndex.constEnd()) {
        sendError(id, ErrorCode::InvalidParams, QStringLiteral("Unknown tool: %1").arg(name));
        return;
    }
    const qsizetype index = *found;
    const ToolEntry& entry = d->tools.at(index);

    const QJsonObject meta = params.value(QLatin1StringView("_meta")).toObject();
    const QJsonObject required = entry.tool.requiredClientCapabilities();
    if (!required.isEmpty() && !hasCapabilities(meta.value(MetaClientCapabilities).toObject(), required)) {
        sendError(id, ErrorCode::MissingRequiredClientCapability,
                  QStringLiteral("Missing required client capability for tool %1").arg(name),
                  QJsonObject{{QLatin1StringView("requiredCapabilities"), required}});
        return;
    }

    const QString key = idKey(id);
    if (d->activeCall(key) != 0) {
        sendError(id, ErrorCode::InvalidRequest, QStringLiteral("Invalid Request: a call with this id is running"));
        return;
    }

    const bool hasArguments = params.contains(QLatin1StringView("arguments"));
    const QJsonValue argumentsValue = params.value(QLatin1StringView("arguments"));
    if (hasArguments && !argumentsValue.isObject()) {
        sendError(id, ErrorCode::InvalidParams, QStringLiteral("Invalid params: arguments must be an object"));
        return;
    }
    const QJsonObject arguments = argumentsValue.toObject();

    // Arguments that do not match are the model's mistake, which it can
    // mend: a tool error that it reads, not a protocol error.
    const QStringList errors = entry.input->validate(arguments);
    if (!errors.isEmpty()) {
        QString text = QStringLiteral("Invalid arguments for tool %1:").arg(name);
        for (const QString& error : errors) {
            text += QStringLiteral("\n- arguments") + error;
        }
        sendResult(id, McpResult::error(text).toJson());
        return;
    }

    const quint64 token = d->nextToken++;
    PendingCall pending;
    pending.id = id;
    pending.idKey = key;
    pending.tool = index;
    pending.progressToken = meta.value(MetaProgressToken);
    d->calls.insert(token, std::move(pending));

    entry.handler(McpCall(this, token, name, arguments, id, meta, meta.contains(MetaProgressToken)));
}

void McpServer::sendResult(const QJsonValue& id, QJsonObject result)
{
    result.insert(QLatin1StringView("resultType"), QStringLiteral("complete"));
    result.insert(QLatin1StringView("_meta"), d->resultMeta());
    sendMessage(QJsonObject{
        {QLatin1StringView("jsonrpc"), QStringLiteral("2.0")},
        {QLatin1StringView("id"), id},
        {QLatin1StringView("result"), result},
    });
}

void McpServer::sendError(const QJsonValue& id, int code, const QString& message, const QJsonValue& data)
{
    QJsonObject error{
        {QLatin1StringView("code"), code},
        {QLatin1StringView("message"), message},
    };
    if (!data.isUndefined() && !data.isNull()) {
        error.insert(QLatin1StringView("data"), data);
    }
    sendMessage(QJsonObject{
        {QLatin1StringView("jsonrpc"), QStringLiteral("2.0")},
        {QLatin1StringView("id"), id},
        {QLatin1StringView("error"), error},
    });
}

void McpServer::sendMessage(const QJsonObject& message)
{
    if (!d->transport || d->finished) {
        return;
    }
    // Compact JSON escapes every newline inside strings, so one message is
    // one line.
    d->transport->writeLine(QJsonDocument(message).toJson(QJsonDocument::Compact));
}

void McpServer::end(FinishReason reason, bool drain)
{
    if (d->ending) {
        return;
    }
    d->ending = true;
    if (d->transport) {
        d->transport->stopReading();
    }

    // Every running call is cancelled, as if the client had cancelled it, so
    // that the application stops its work. Tokens first: a callback may
    // destroy calls, which changes d->calls.
    const QList<quint64> tokens = d->calls.keys();
    for (const quint64 token : tokens) {
        const auto it = d->calls.find(token);
        if (it == d->calls.end() || it->cancelled) {
            continue;
        }
        it->cancelled = true;
        std::function<void()> callback = std::exchange(it->onCancelled, nullptr);
        if (callback) {
            callback();
        }
    }

    if (drain && d->transport && d->transport->isOpen() && d->transport->pendingWriteBytes() > 0) {
        connect(d->transport, &McpTransport::drained, this, [this, reason] {
            emitFinished(reason);
        });
        connect(d->transport, &McpTransport::outputClosed, this, [this, reason] {
            emitFinished(reason);
        });
        QTimer::singleShot(DrainTimeoutMs, this, [this, reason] {
            emitFinished(reason);
        });
        return;
    }
    // Queued: end() runs inside a handler's call, a signal handler's
    // callback or a transport signal, and a slot of finished() may destroy
    // the server.
    QMetaObject::invokeMethod(
        this,
        [this, reason] {
            emitFinished(reason);
        },
        Qt::QueuedConnection);
}

void McpServer::emitFinished(FinishReason reason)
{
    if (d->finished) {
        return;
    }
    d->finished = true;
    if (d->transport) {
        d->transport->close();
    }
    Q_EMIT finished(reason);
}

bool McpServer::callIsValid(quint64 token) const
{
    const auto it = d->calls.constFind(token);
    return it != d->calls.constEnd() && !it->cancelled && !d->ending;
}

bool McpServer::callIsCancelled(quint64 token) const
{
    const auto it = d->calls.constFind(token);
    return it != d->calls.constEnd() && it->cancelled;
}

bool McpServer::finishCall(quint64 token, const McpResult& result)
{
    const auto it = d->calls.find(token);
    if (it == d->calls.end() || it->cancelled || d->ending) {
        return false;
    }
    const QJsonValue id = it->id;
    const ToolEntry& entry = d->tools.at(it->tool);
    d->calls.erase(it);

    if (entry.output && !result.isError()) {
        const QStringList errors = result.hasStructuredContent()
            ? entry.output->validate(result.structuredContent())
            : QStringList{QStringLiteral(": no structured content")};
        if (!errors.isEmpty()) {
            // A bug of the tool, not a mistake of the model: the spec has a
            // server send structured content that matches the schema.
            qCWarning(lcMcp, "McpServer: the result of tool %s does not match its output schema: %s",
                      qPrintable(entry.tool.name()), qPrintable(errors.join(QStringLiteral("; "))));
            QJsonArray list;
            for (const QString& error : errors) {
                list.append(QString(QStringLiteral("structuredContent") + error));
            }
            sendError(id, ErrorCode::InternalError,
                      QStringLiteral("Internal error: the result of tool %1 does not match its output schema")
                          .arg(entry.tool.name()),
                      QJsonObject{{QLatin1StringView("errors"), list}});
            return false;
        }
    }
    sendResult(id, result.toJson());
    return true;
}

bool McpServer::progressCall(quint64 token, double progress, std::optional<double> total, const QString& message)
{
    const auto it = d->calls.find(token);
    if (it == d->calls.end() || it->cancelled || d->ending) {
        return false;
    }
    if (it->progressToken.isUndefined()) {
        return false;
    }
    if (!std::isfinite(progress) || (total && !std::isfinite(*total))) {
        qCWarning(lcMcp, "McpServer: progress of tool %s refused: not a finite number",
                  qPrintable(d->tools.at(it->tool).tool.name()));
        return false;
    }
    if (it->lastProgress && progress <= *it->lastProgress) {
        qCWarning(lcMcp, "McpServer: progress of tool %s refused: %g after %g, and progress must grow",
                  qPrintable(d->tools.at(it->tool).tool.name()), progress, *it->lastProgress);
        return false;
    }
    it->lastProgress = progress;
    QJsonObject params{
        {QLatin1StringView("progressToken"), it->progressToken},
        {QLatin1StringView("progress"), progress},
    };
    if (total) {
        params.insert(QLatin1StringView("total"), *total);
    }
    if (!message.isEmpty()) {
        params.insert(QLatin1StringView("message"), message);
    }
    sendMessage(QJsonObject{
        {QLatin1StringView("jsonrpc"), QStringLiteral("2.0")},
        {QLatin1StringView("method"), QStringLiteral("notifications/progress")},
        {QLatin1StringView("params"), params},
    });
    return true;
}

void McpServer::setCancelCallback(quint64 token, std::function<void()> callback)
{
    const auto it = d->calls.find(token);
    if (it == d->calls.end()) {
        return;
    }
    if (it->cancelled) {
        callback();
        return;
    }
    it->onCancelled = std::move(callback);
}

void McpServer::abandonCall(quint64 token)
{
    const auto it = d->calls.find(token);
    if (it == d->calls.end()) {
        return;
    }
    const bool answer = !it->cancelled && !d->ending;
    const QJsonValue id = it->id;
    const QString tool = d->tools.at(it->tool).tool.name();
    d->calls.erase(it);
    if (answer) {
        qCWarning(lcMcp, "McpServer: tool %s dropped a call without answering it", qPrintable(tool));
        sendError(id, ErrorCode::InternalError,
                  QStringLiteral("Internal error: tool %1 dropped the call without an answer").arg(tool));
    }
}

} // namespace PhosphorMcp
