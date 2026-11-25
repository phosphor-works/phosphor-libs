// SPDX-FileCopyrightText: 2026 Phosphor Works Team
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
#include <QPointer>
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
const QLatin1StringView NotificationProgress("notifications/progress");
const QLatin1StringView KeyMeta("_meta");
// The key of notifications/progress params that echoes the request's token.
const QLatin1StringView KeyProgressToken("progressToken");

// The id of an error answer to a message whose id cannot be read. MCP's
// schema has an error's id be a string or a number, or absent, so the id is
// left out rather than null.
const QJsonValue NoId(QJsonValue::Undefined);

// The longest cancel reason that goes to the log.
constexpr qsizetype MaxLoggedReason = 200;
// The most places of numbers that are not finite that one result reports.
constexpr qsizetype MaxNonFiniteReported = 16;
// The longest property name that a reported place spells out.
constexpr qsizetype MaxPointerToken = 128;

// The three methods the server offers.
bool isServerMethod(const QString& method)
{
    return method == MethodDiscover || method == MethodToolsList || method == MethodToolsCall;
}

// JSON-RPC ids are strings or numbers, and MCP rules out null. A number must
// be an integer that a qint64 holds: echoed back, a larger one would come out
// as another literal (2^63 as 9223372036854776000). A string must be valid
// UTF-16: an unpaired surrogate goes out as U+FFFD (see
// replaceLoneSurrogates), and the client could not match the reply.
// Progress tokens follow the same rule.
bool isValidId(const QJsonValue& id)
{
    if (id.isString()) {
        const QString text = id.toString();
        return QStringView(text).isValidUtf16();
    }
    if (!id.isDouble()) {
        return false;
    }
    if (QCborValue::fromJsonValue(id).isInteger()) {
        return true;
    }
    const double value = id.toDouble();
    return std::isfinite(value) && std::trunc(value) == value && value >= -0x1p63 && value < 0x1p63;
}

// A key that tells a string id from a number id with the same text. A valid
// number id is an integer that a qint64 holds, and keeps all its digits
// here: through a double, ids past 2^53 would share a key
// (9007199254740992 and ...993).
QString idKey(const QJsonValue& id)
{
    if (id.isString()) {
        return QLatin1Char('s') + id.toString();
    }
    const QCborValue number = QCborValue::fromJsonValue(id);
    const qint64 value = number.isInteger() ? number.toInteger() : static_cast<qint64>(id.toDouble());
    return QLatin1Char('n') + QString::number(value);
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

// The data of an error about the protocol version: the one this server
// supports, and the one the client asked for when it named one.
QJsonObject versionData(const QJsonValue& requested)
{
    QJsonObject data{{QLatin1StringView("supported"), QJsonArray{ProtocolVersion}}};
    if (requested.isString()) {
        data.insert(QLatin1StringView("requested"), requested);
    }
    return data;
}

// A property name as a JSON pointer token, cut when it is very long.
QString pointerToken(const QString& name)
{
    QString token = name.size() > MaxPointerToken ? name.left(MaxPointerToken) + QStringLiteral("...") : name;
    token.replace(QLatin1Char('~'), QLatin1StringView("~0"));
    token.replace(QLatin1Char('/'), QLatin1StringView("~1"));
    return token;
}

// The pointer of the first value in `required` that is not an object, or an
// empty string. hasCapabilities() takes a declared capability only as an
// object, so a requirement with any other value could never be met.
QString capabilityThatIsNotAnObject(const QJsonObject& required, const QString& pointer)
{
    for (auto it = required.begin(); it != required.end(); ++it) {
        const QString at = pointer + QLatin1Char('/') + pointerToken(it.key());
        if (!it.value().isObject()) {
            return at;
        }
        const QString inner = capabilityThatIsNotAnObject(it.value().toObject(), at);
        if (!inner.isEmpty()) {
            return inner;
        }
    }
    return QString();
}

// Appends to `found` the places in `value` of the numbers that are not
// finite, which JSON has no form for (Qt writes them as null), each as
// "<pointer>: <what>" like McpSchema::validate().
void findNonFinite(const QJsonValue& value, const QString& pointer, QStringList& found)
{
    if (found.size() >= MaxNonFiniteReported) {
        return;
    }
    if (value.isDouble()) {
        if (!std::isfinite(value.toDouble())) {
            found.append(pointer + QStringLiteral(": is a number that is not finite, which JSON cannot carry"));
        }
        return;
    }
    if (value.isArray()) {
        const QJsonArray array = value.toArray();
        for (qsizetype i = 0; i < array.size(); ++i) {
            findNonFinite(array.at(i), pointer + QLatin1Char('/') + QString::number(i), found);
        }
        return;
    }
    if (value.isObject()) {
        const QJsonObject object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it) {
            findNonFinite(it.value(), pointer + QLatin1Char('/') + pointerToken(it.key()), found);
        }
    }
}

// The text with each unpaired surrogate replaced by U+FFFD, as
// replaceLoneSurrogates() does on the wire.
QString withoutLoneSurrogates(const QString& text)
{
    QString clean;
    clean.reserve(text.size());
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (c.isHighSurrogate() && i + 1 < text.size() && text.at(i + 1).isLowSurrogate()) {
            clean += c;
            clean += text.at(++i);
        } else if (c.isSurrogate()) {
            clean += QChar(QChar::ReplacementCharacter);
        } else {
            clean += c;
        }
    }
    return clean;
}

bool hasLoneSurrogateKey(const QJsonValue& value)
{
    if (value.isArray()) {
        const QJsonArray array = value.toArray();
        for (const QJsonValue& element : array) {
            if (hasLoneSurrogateKey(element)) {
                return true;
            }
        }
        return false;
    }
    if (value.isObject()) {
        const QJsonObject object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it) {
            const QString key = it.key();
            if (!QStringView(key).isValidUtf16() || hasLoneSurrogateKey(it.value())) {
                return true;
            }
        }
    }
    return false;
}

// Two keys that differ only in an unpaired surrogate would both go out as
// the same key, and a line with a key twice is read differently by different
// parsers. Their U+FFFD form is made here, in the object, so that the object
// keeps one of them.
QJsonValue withLoneSurrogateKeysReplaced(const QJsonValue& value)
{
    if (value.isArray()) {
        QJsonArray array;
        for (const QJsonValue& element : value.toArray()) {
            array.append(withLoneSurrogateKeysReplaced(element));
        }
        return array;
    }
    if (value.isObject()) {
        const QJsonObject source = value.toObject();
        QJsonObject object;
        for (auto it = source.begin(); it != source.end(); ++it) {
            object.insert(withoutLoneSurrogates(it.key()), withLoneSurrogateKeysReplaced(it.value()));
        }
        return object;
    }
    return value;
}

bool isHexDigitFrom8(char c)
{
    return (c >= '8' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// True when the JSON may hold a lone surrogate escape. The text "\ud" after
// an escaped backslash matches too, which only costs a closer look.
bool hasSurrogateEscape(const QByteArray& json)
{
    return json.contains("\\ud") || json.contains("\\uD");
}

// Qt's JSON writer encodes a surrogate pair as UTF-8, and a lone surrogate,
// which UTF-8 cannot hold, as a \uD800 to \uDFFF escape that strict parsers
// refuse. Each such escape becomes U+FFFD, as a UTF-8 decoder does with
// bytes that are not UTF-8. The escape has the length of the one it
// replaces, so the line changes in place.
void replaceLoneSurrogates(QByteArray& json)
{
    if (!hasSurrogateEscape(json)) {
        return;
    }
    char* data = json.data();
    const qsizetype size = json.size();
    for (qsizetype i = 0; i + 1 < size; ++i) {
        if (data[i] != '\\') {
            continue;
        }
        // An escape is skipped whole, so that the second backslash of an
        // escaped backslash never starts another escape.
        if (data[i + 1] == 'u' && i + 5 < size) {
            if ((data[i + 2] == 'd' || data[i + 2] == 'D') && isHexDigitFrom8(data[i + 3])) {
                std::memcpy(data + i + 2, "fffd", 4);
            }
            i += 5;
        } else {
            ++i;
        }
    }
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
    bool finishQueued = false;
    bool finished = false;
    // From the start of ~McpServer: an unanswered call reads isCancelled().
    bool destroying = false;
    // While end() runs the cancel callbacks. An end that does not wait,
    // coming from inside one of them (a signal or a closed output in a
    // nested event loop), is kept here and posted after the last callback:
    // finished() comes after every callback, and its slot may delete the
    // server.
    bool cancelling = false;
    std::optional<FinishReason> cutShort;
    // While the replies drain after the end of the input: started again at
    // each write, so that it measures a client that reads nothing.
    QTimer* drainTimer = nullptr;

    // One entry for each McpCall that holds a call, answered or not: the
    // application bounds this table, as it holds the calls.
    QHash<quint64, PendingCall> calls;
    // The calls that run (not cancelled), by request id, for the check of a
    // reused id and for a cancel.
    QHash<QString, quint64> running;
    quint64 nextToken = 1;

    std::unique_ptr<McpTerminationSignals> terminationSignals;

    QJsonObject resultMeta() const
    {
        // The application's name is never empty while a server runs:
        // QCoreApplication gives it the executable's name by default.
        QString serverVersion = version.isEmpty() ? QCoreApplication::applicationVersion() : version;
        if (serverVersion.isEmpty()) {
            serverVersion = QStringLiteral("0");
        }
        return QJsonObject{
            {MetaServerInfo,
             QJsonObject{
                 {QLatin1StringView("name"), name.isEmpty() ? QCoreApplication::applicationName() : name},
                 {QLatin1StringView("version"), serverVersion},
             }},
        };
    }

    // The running call with this request id, or 0.
    quint64 activeCall(const QString& key) const
    {
        return running.value(key, 0);
    }

    // The call no longer runs under its id, which a new call may take.
    void forgetRunning(quint64 token, const PendingCall& call)
    {
        const auto it = running.constFind(call.idKey);
        if (it != running.constEnd() && *it == token) {
            running.erase(it);
        }
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
    // destructor looks its call up. The handlers go here too, while d is
    // whole, and not in d's own destruction after the table: a handler may
    // own an McpCall as well.
    d->destroying = true;
    d->ending = true;
    d->finished = true;
    d->running.clear();
    {
        const QHash<quint64, PendingCall> calls = std::exchange(d->calls, {});
        Q_UNUSED(calls)
    }
    {
        const QList<ToolEntry> tools = std::exchange(d->tools, {});
        Q_UNUSED(tools)
    }
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
        qCWarning(lcMcp).noquote() << "McpServer::setCache: a negative ttl of" << ttlMs << "ms is refused";
        return false;
    }
    d->ttlMs = ttlMs;
    d->cacheScope = scope;
    return true;
}

// The transport clamps both limits to at least 1. The server keeps the value
// it was given and hands it on at start().
void McpServer::setMaxLineBytes(qint64 bytes)
{
    d->maxLineBytes = bytes;
    if (d->transport) {
        d->transport->setMaxLineBytes(bytes);
    }
}

void McpServer::setMaxPendingWriteBytes(qint64 bytes)
{
    d->maxPendingWriteBytes = bytes;
    if (d->transport) {
        d->transport->setMaxPendingWriteBytes(bytes);
    }
}

bool McpServer::addTool(const McpTool& tool, Handler handler, QString* error)
{
    const auto refuse = [&](const QString& why) {
        qCWarning(lcMcp) << "McpServer::addTool:" << tool.name() << why;
        if (error) {
            *error = why;
        }
        return false;
    };
    if (d->started) {
        return refuse(QStringLiteral("the server has started, and its tool list does not change"));
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
    const QString notAnObject = capabilityThatIsNotAnObject(tool.requiredClientCapabilities(), QString());
    if (!notAnObject.isEmpty()) {
        return refuse(QStringLiteral("requiredClientCapabilities%1 is not an object, and no client could declare it")
                          .arg(notAnObject));
    }
    auto input = std::make_shared<McpSchema>();
    QString why = input->compile(tool.inputSchema());
    if (!why.isEmpty()) {
        return refuse(QStringLiteral("inputSchema%1").arg(why));
    }
    std::shared_ptr<McpSchema> output;
    if (!tool.outputSchema().isEmpty()) {
        // Structured content may be any JSON value, so the root of an
        // output schema may be any type.
        output = std::make_shared<McpSchema>();
        why = output->compile(tool.outputSchema(), McpSchema::Root::Any);
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
    if (d->ending) {
        qCWarning(lcMcp).noquote() << "McpServer::handleTerminationSignals: the server has ended";
        return false;
    }
    if (d->terminationSignals) {
        // In a child forked after the parent's call, the copied handler
        // takes the old dispositions, so the server handles nothing here.
        if (!d->terminationSignals->belongsToThisProcess()) {
            qCWarning(lcMcp).noquote()
                << "McpServer::handleTerminationSignals: the parent called this before the fork, and a "
                   "child cannot take its handler. In the child, destroy this copy and call it on a server made "
                   "there.";
            return false;
        }
        return true;
    }
    // The callback runs from the self-pipe's notifier. end() releases the
    // signals inside it, before the cancel callbacks run, and finishes
    // later, from the event loop.
    d->terminationSignals = McpTerminationSignals::install(this, [this](int signal) {
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
        qCWarning(lcMcp).noquote() << "McpServer::start: started or ended before";
        return false;
    }
    auto* transport = new McpTransport(readFd, writeFd, this);
    transport->setMaxLineBytes(d->maxLineBytes);
    transport->setMaxPendingWriteBytes(d->maxPendingWriteBytes);
    if (!transport->open()) {
        delete transport;
        return false;
    }
    if (d->tools.isEmpty()) {
        qCWarning(lcMcp).noquote() << "McpServer::start: the server has no tools, so it offers none";
    }
    d->transport = transport;
    d->started = true;
    for (const ToolEntry& entry : std::as_const(d->tools)) {
        d->toolsJson.append(entry.tool.toJson());
    }
    connect(transport, &McpTransport::lineReceived, this, &McpServer::onLine);
    connect(transport, &McpTransport::lineTooLong, this, [this] {
        sendError(
            NoId, ErrorCode::InvalidRequest,
            QStringLiteral("Invalid Request: the message is longer than %1 bytes").arg(d->transport->maxLineBytes()));
    });
    connect(transport, &McpTransport::endOfInput, this, [this] {
        end(FinishReason::EndOfInput, true);
    });
    // stdout may be fine: the replies already queued still go out. A client
    // on a socket that closes with replies it did not read resets the
    // connection, and the next read fails with ECONNRESET: that is the
    // client's end, as on a pipe.
    connect(transport, &McpTransport::inputFailed, this, [this](int error) {
        if (error == ECONNRESET) {
            end(FinishReason::EndOfInput, true);
            return;
        }
        qCWarning(lcMcp).noquote() << "McpServer: reading the input failed:" << qt_error_string(error);
        end(FinishReason::InputFailed, true);
    });
    // The client closing stdout is a normal end. Any other failure is not.
    connect(transport, &McpTransport::outputClosed, this, [this](int error) {
        if (error == EPIPE) {
            end(FinishReason::OutputClosed, false);
            return;
        }
        qCWarning(lcMcp).noquote() << "McpServer: writing the output failed:" << qt_error_string(error);
        end(FinishReason::OutputFailed, false);
    });
    return true;
}

bool McpServer::isRunning() const
{
    return d->started && !d->ending;
}

const McpTransport* McpServer::transport() const
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
    // A JSON value, not a document: a line that is JSON but not an object
    // is an Invalid Request, not a Parse error.
    QJsonParseError parseError;
    const QJsonValue document = QJsonValue::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        sendError(NoId, ErrorCode::ParseError,
                  QStringLiteral("Parse error: %1 at offset %2")
                      .arg(parseError.errorString(), QString::number(parseError.offset)));
        return;
    }
    if (document.isArray()) {
        sendError(NoId, ErrorCode::InvalidRequest, QStringLiteral("Invalid Request: batches are not supported"));
        return;
    }
    if (!document.isObject()) {
        sendError(NoId, ErrorCode::InvalidRequest, QStringLiteral("Invalid Request: a message is a JSON object"));
        return;
    }
    const QJsonObject message = document.toObject();
    const bool hasId = message.contains(QLatin1StringView("id"));
    const QJsonValue id = message.value(QLatin1StringView("id"));
    const QJsonValue replyId = hasId && isValidId(id) ? id : NoId;

    if (message.value(QLatin1StringView("jsonrpc")) != QJsonValue(QStringLiteral("2.0"))) {
        sendError(replyId, ErrorCode::InvalidRequest, QStringLiteral("Invalid Request: jsonrpc must be \"2.0\""));
        return;
    }
    if (!message.contains(QLatin1StringView("method"))) {
        if (message.contains(QLatin1StringView("result")) || message.contains(QLatin1StringView("error"))) {
            // The server sends no requests, so no response is awaited.
            qCDebug(lcMcp).noquote() << "McpServer: dropped a response, since the server sends no requests";
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
    const QString name = method.toString();
    const bool hasParams = message.contains(QLatin1StringView("params"));
    const QJsonValue params = message.value(QLatin1StringView("params"));
    if (!hasId) {
        // A notification is never answered, not even when its params are
        // wrong.
        if (params.isObject() || !hasParams) {
            handleNotification(name, params.toObject());
        }
        return;
    }
    if (!isValidId(id)) {
        sendError(
            NoId, ErrorCode::InvalidRequest,
            QStringLiteral("Invalid Request: id must be a string of valid UTF-16 or an integer of at most 64 bits"));
        return;
    }
    if (name == MethodInitialize) {
        sendError(id, ErrorCode::MethodNotFound,
                  QStringLiteral("This server supports MCP %1 only, which has no initialize. Use server/discover.")
                      .arg(ProtocolVersion),
                  versionData(params.toObject().value(QLatin1StringView("protocolVersion"))));
        return;
    }
    // Only for the methods the server offers: an unknown method is -32601
    // whatever its params.
    if (hasParams && !params.isObject() && isServerMethod(name)) {
        sendError(id, ErrorCode::InvalidParams, QStringLiteral("Invalid params: params must be an object"));
        return;
    }
    handleRequest(id, name, params.toObject());
}

void McpServer::handleRequest(const QJsonValue& id, const QString& method, const QJsonObject& params)
{
    if (!isServerMethod(method)) {
        sendError(id, ErrorCode::MethodNotFound, QStringLiteral("Method not found: %1").arg(method));
        return;
    }
    if (!checkMeta(id, params)) {
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
    // The spec has the receiver log the reason, for debugging. It is the
    // client's text, so it is cut, and quoted with its control characters
    // escaped.
    const QString reason = params.value(QLatin1StringView("reason")).toString();
    qCDebug(lcMcp) << "McpServer: the client cancelled a call of tool"
                   << d->tools.at(d->calls.constFind(token)->tool).tool.name() << "with the reason"
                   << (reason.size() > MaxLoggedReason ? reason.left(MaxLoggedReason) + QStringLiteral("...") : reason);
    cancelCall(token);
    runCancelCallback(token);
}

bool McpServer::checkMeta(const QJsonValue& id, const QJsonObject& params)
{
    const QJsonValue meta = params.value(KeyMeta);
    if (!meta.isObject()) {
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
    // Before the other keys: a client of another revision may require other
    // keys, and what it needs to learn is the version.
    if (version.toString() != ProtocolVersion) {
        sendError(id, ErrorCode::UnsupportedProtocolVersion, QStringLiteral("Unsupported protocol version"),
                  versionData(version));
        return false;
    }
    if (!metaObject.value(MetaClientCapabilities).isObject()) {
        sendError(id, ErrorCode::InvalidParams,
                  QStringLiteral("Invalid params: params._meta[\"%1\"] is required").arg(MetaClientCapabilities));
        return false;
    }
    if (metaObject.contains(MetaProgressToken) && !isValidId(metaObject.value(MetaProgressToken))) {
        sendError(id, ErrorCode::InvalidParams,
                  QStringLiteral("Invalid params: params._meta.progressToken must be a string of valid UTF-16 or an "
                                 "integer of at most 64 bits"));
        return false;
    }
    return true;
}

void McpServer::handleDiscover(const QJsonValue& id)
{
    // The spec has the tools capability present when the server offers
    // tools.
    QJsonObject capabilities;
    if (!d->tools.isEmpty()) {
        capabilities.insert(QLatin1StringView("tools"), QJsonObject());
    }
    QJsonObject result{
        {QLatin1StringView("supportedVersions"), QJsonArray{ProtocolVersion}},
        {QLatin1StringView("capabilities"), capabilities},
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

    const QJsonObject meta = params.value(KeyMeta).toObject();
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
    d->running.insert(key, token);

    entry.handler(McpCall(this, token, name, arguments, id, meta, meta.contains(MetaProgressToken)));
}

bool McpServer::sendResult(const QJsonValue& id, QJsonObject result)
{
    result.insert(QLatin1StringView("resultType"), QStringLiteral("complete"));
    result.insert(KeyMeta, d->resultMeta());
    return sendMessage(QJsonObject{
        {QLatin1StringView("jsonrpc"), QStringLiteral("2.0")},
        {QLatin1StringView("id"), id},
        {QLatin1StringView("result"), result},
    });
}

bool McpServer::sendError(const QJsonValue& id, int code, const QString& message, const QJsonValue& data)
{
    QJsonObject error{
        {QLatin1StringView("code"), code},
        {QLatin1StringView("message"), message},
    };
    if (!data.isUndefined() && !data.isNull()) {
        error.insert(QLatin1StringView("data"), data);
    }
    QJsonObject envelope{
        {QLatin1StringView("jsonrpc"), QStringLiteral("2.0")},
        {QLatin1StringView("error"), error},
    };
    if (!id.isUndefined()) {
        envelope.insert(QLatin1StringView("id"), id);
    }
    return sendMessage(envelope);
}

bool McpServer::sendMessage(const QJsonObject& message)
{
    if (!d->transport || d->finished) {
        return false;
    }
    // Compact JSON escapes every newline inside strings, so one message is
    // one line. A lone surrogate is written as a \uD8xx to \uDFxx escape, so
    // only a line with such an escape can hold a key that has one. Those are
    // rare (a tool's own JSON), and the object is walked and rebuilt only
    // then.
    QByteArray line = QJsonDocument(message).toJson(QJsonDocument::Compact);
    if (hasSurrogateEscape(line) && hasLoneSurrogateKey(message)) {
        line = QJsonDocument(withLoneSurrogateKeysReplaced(message).toObject()).toJson(QJsonDocument::Compact);
    }
    replaceLoneSurrogates(line);
    return d->transport->writeLine(std::move(line));
}

void McpServer::end(FinishReason reason, bool drain)
{
    if (d->finished) {
        return;
    }
    if (d->ending) {
        // An end that does not wait (a signal, the output gone) cuts short
        // the wait for the replies to drain.
        if (!drain) {
            if (d->cancelling) {
                if (!d->cutShort) {
                    d->cutShort = reason;
                }
                // finished() waits for the callbacks, but the signals need
                // not: a second one, during a callback that hangs, has its
                // usual effect again. Released, not destroyed, as in
                // emitFinished(): this may run inside the signals' callback.
                if (d->terminationSignals) {
                    d->terminationSignals->release();
                }
            } else {
                finishSoon(reason);
            }
        }
        return;
    }
    d->ending = true;
    if (d->transport) {
        d->transport->stopReading();
    }
    // An end that does not wait (the signal itself, the output gone) has no
    // drain for a signal to cut short. The signals go back now, before the
    // callbacks run, so that a second one during a callback that hangs has
    // its usual effect. An end that drains keeps them until it finishes.
    if (!drain && d->terminationSignals) {
        d->terminationSignals->release();
    }

    // Every running call is cancelled, as if the client had cancelled it, so
    // that the application stops its work. All are marked before any
    // callback runs, so that a callback finds every other call cancelled
    // already. A callback may destroy calls: the callback of a destroyed
    // call never runs.
    const QList<quint64> tokens = d->calls.keys();
    for (const quint64 token : tokens) {
        cancelCall(token);
    }
    const QPointer<McpServer> self(this);
    d->cancelling = true;
    for (const quint64 token : tokens) {
        runCancelCallback(token);
        if (!self) {
            return;
        }
    }
    d->cancelling = false;
    // An end that does not wait came from inside a callback, in a nested
    // event loop. It decides the reason, and nothing waits for the replies.
    if (d->cutShort) {
        finishSoon(*d->cutShort);
        return;
    }
    if (d->finished || d->finishQueued) {
        return;
    }

    if (drain && d->transport && d->transport->isOpen() && d->transport->pendingWriteBytes() > 0) {
        drainThenFinish(reason);
        return;
    }
    finishSoon(reason);
}

void McpServer::drainThenFinish(FinishReason reason)
{
    // A client that reads slowly gets the replies whole: the timer measures
    // how long the client read nothing, not how long the whole queue takes.
    // A closed output or a signal meanwhile ends the wait through end().
    d->drainTimer = new QTimer(this);
    d->drainTimer->setSingleShot(true);
    d->drainTimer->setInterval(DrainTimeoutMs);
    connect(d->drainTimer, &QTimer::timeout, this, [this, reason] {
        qCWarning(lcMcp).noquote() << "McpServer: the client read none of its replies for" << DrainTimeoutMs
                                   << "ms after the end of its input, so the last" << d->transport->pendingWriteBytes()
                                   << "bytes of them are dropped";
        emitFinished(reason);
    });
    QTimer* timer = d->drainTimer;
    connect(d->transport, &McpTransport::bytesWritten, timer, [timer] {
        timer->start();
    });
    connect(d->transport, &McpTransport::drained, this, [this, reason] {
        emitFinished(reason);
    });
    timer->start();
}

void McpServer::finishSoon(FinishReason reason)
{
    if (d->finished || d->finishQueued) {
        return;
    }
    d->finishQueued = true;
    // Queued: end() runs inside a handler's call, the signals' callback or a
    // transport signal, and a slot of finished() may destroy the server.
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
    if (d->drainTimer) {
        d->drainTimer->stop();
    }
    if (d->transport) {
        d->transport->close();
    }
    // A second signal, during a slow teardown of the application, has its
    // usual effect again. Released, not destroyed: this may run from a
    // nested event loop inside the signals' own callback.
    if (d->terminationSignals) {
        d->terminationSignals->release();
    }
    Q_EMIT finished(reason);
}

void McpServer::cancelCall(quint64 token)
{
    const auto it = d->calls.find(token);
    if (it == d->calls.end() || it->cancelled) {
        return;
    }
    it->cancelled = true;
    d->forgetRunning(token, *it);
}

void McpServer::runCancelCallback(quint64 token)
{
    const auto it = d->calls.find(token);
    if (it == d->calls.end()) {
        return;
    }
    // Out of the table before it runs: it runs once, and may destroy calls.
    const std::function<void()> callback = std::exchange(it->onCancelled, nullptr);
    if (callback) {
        callback();
    }
}

bool McpServer::callIsValid(quint64 token) const
{
    const auto it = d->calls.constFind(token);
    return it != d->calls.constEnd() && !it->cancelled && !d->ending;
}

bool McpServer::callIsCancelled(quint64 token) const
{
    const auto it = d->calls.constFind(token);
    if (it == d->calls.constEnd()) {
        // While the server is destroyed its table is gone, and a call that
        // still holds a token was never answered (an answer drops it).
        return d->destroying;
    }
    return it->cancelled;
}

bool McpServer::finishCall(quint64 token, const McpResult& result)
{
    if (!callIsValid(token)) {
        return false;
    }
    // Taken, not erased: the call's cancel callback may own calls, and its
    // destruction must not run inside QHash::erase(). It goes with `call`,
    // after the last use.
    const PendingCall call = d->calls.take(token);
    d->forgetRunning(token, call);
    const ToolEntry& entry = d->tools.at(call.tool);

    QStringList errors;
    if (result.hasStructuredContent()) {
        if (entry.output) {
            // The check refuses a number that is not finite itself, at its
            // place, before it checks anything else.
            errors = entry.output->validate(result.structuredContent());
        } else {
            findNonFinite(result.structuredContent(), QString(), errors);
        }
    } else if (entry.output && !result.isError()) {
        errors.append(QStringLiteral(": no structured content"));
    }
    if (!errors.isEmpty() && entry.output) {
        // A bug of the tool, not a mistake of the model: the spec has a
        // server send structured content that matches the schema, failed
        // results included.
        qCWarning(lcMcp) << "McpServer: the result of tool" << entry.tool.name()
                         << "does not match its output schema:" << errors;
        QJsonArray list;
        for (const QString& error : std::as_const(errors)) {
            list.append(QString(QStringLiteral("structuredContent") + error));
        }
        sendError(call.id, ErrorCode::InternalError,
                  QStringLiteral("Internal error: the result of tool %1 does not match its output schema")
                      .arg(entry.tool.name()),
                  QJsonObject{{QLatin1StringView("errors"), list}});
        return false;
    }
    if (!errors.isEmpty()) {
        qCWarning(lcMcp) << "McpServer: the structured content of tool" << entry.tool.name()
                         << "has numbers that are not finite, which go out as null:" << errors;
    }
    return sendResult(call.id, result.toJson());
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
        qCWarning(lcMcp) << "McpServer: progress of tool" << d->tools.at(it->tool).tool.name()
                         << "refused, as it is not a finite number";
        return false;
    }
    if (it->lastProgress && progress <= *it->lastProgress) {
        qCWarning(lcMcp) << "McpServer: progress of tool" << d->tools.at(it->tool).tool.name() << "is refused, as"
                         << progress << "does not grow past" << *it->lastProgress;
        return false;
    }
    // Progress is a hint. While the client leaves more than the high-water
    // mark of replies unread, a notification would only grow the queue.
    if (!d->transport || d->transport->pendingWriteBytes() > d->transport->maxPendingWriteBytes()) {
        return false;
    }
    QJsonObject params{
        {KeyProgressToken, it->progressToken},
        {QLatin1StringView("progress"), progress},
    };
    if (total) {
        params.insert(QLatin1StringView("total"), *total);
    }
    if (!message.isEmpty()) {
        params.insert(QLatin1StringView("message"), message);
    }
    // sendMessage() runs no application code, so `it` stays valid.
    const bool sent = sendMessage(QJsonObject{
        {QLatin1StringView("jsonrpc"), QStringLiteral("2.0")},
        {QLatin1StringView("method"), NotificationProgress},
        {QLatin1StringView("params"), params},
    });
    if (sent) {
        it->lastProgress = progress;
    }
    return sent;
}

void McpServer::setCancelCallback(quint64 token, std::function<void()> callback)
{
    const auto it = d->calls.find(token);
    if (it == d->calls.end()) {
        return;
    }
    if (it->cancelled) {
        // The call's first callback may not have run yet, when end() is on
        // its way to it. The new one replaces it, and runs now: one callback
        // per call. The replaced one goes after, since it may own calls.
        const std::function<void()> replaced = std::exchange(it->onCancelled, nullptr);
        callback();
        Q_UNUSED(replaced)
        return;
    }
    // Exchanged, not assigned over: the callback it replaces may own calls,
    // whose destruction changes d->calls. It goes at the end, after the
    // last use of `it`.
    const std::function<void()> replaced = std::exchange(it->onCancelled, std::move(callback));
    Q_UNUSED(replaced)
}

void McpServer::abandonCall(quint64 token)
{
    if (!d->calls.contains(token)) {
        return;
    }
    // Taken, not erased, as in finishCall().
    const PendingCall call = d->calls.take(token);
    d->forgetRunning(token, call);
    if (call.cancelled || d->ending) {
        return;
    }
    const QString tool = d->tools.at(call.tool).tool.name();
    qCWarning(lcMcp) << "McpServer: tool" << tool << "dropped a call without answering it";
    sendError(call.id, ErrorCode::InternalError,
              QStringLiteral("Internal error: tool %1 dropped the call without an answer").arg(tool));
}

} // namespace PhosphorMcp
