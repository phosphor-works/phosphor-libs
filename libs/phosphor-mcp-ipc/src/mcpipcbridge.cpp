// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorMcpIpc/McpIpcBridge.h>

#include <PhosphorIpc/IpcClient.h>
#include <PhosphorIpc/IpcProtocol.h>
#include <PhosphorMcp/McpCall.h>
#include <PhosphorMcp/McpResult.h>
#include <PhosphorMcp/McpServer.h>
#include <PhosphorMcp/McpTool.h>

#include <QChar>
#include <QDir>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLatin1StringView>
#include <QLoggingCategory>
#include <QMetaObject>
#include <QSet>
#include <QTimer>

#include <memory>
#include <optional>
#include <utility>

namespace PhosphorMcpIpc {

namespace {

Q_LOGGING_CATEGORY(lcMcpIpc, "phosphormcpipc")

using PhosphorIpc::IpcClient;
using PhosphorIpc::IpcReply;

// The most characters of a text of the socket that goes into a warning or
// into the reason of a failed start.
constexpr qsizetype MaxPeerTextLength = 256;

// The most parameter names and the most overloads that one message lists.
constexpr qsizetype MaxListedNames = 16;
constexpr qsizetype MaxListedOverloads = 8;

// A text that the socket sent, cut, and with a space for each control
// character, format character of the basic plane and line separator. The program writes
// warnings and failures to its log. A peer must not be able to add lines
// or terminal sequences there, or to fill it.
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

// A tool that the bridge built, and the call it makes.
struct Tool
{
    PhosphorMcp::McpTool tool;
    QString target;
    QString method;
    // The parameter names, in the order of the call's positional arguments.
    QStringList params;
};

// What buildTools() made of a manifest: the tools and the lines about what
// it left out, or the reason why the manifest cannot be served.
struct Built
{
    QList<Tool> tools;
    QStringList warnings;
    QString error;
};

// The names of a method's parameters in the phosphor-ipc schema, in order,
// or nothing when one has no name (a C++ declaration without one).
std::optional<QStringList> parameterNames(const QJsonObject& method)
{
    QStringList names;
    const QJsonArray params = method.value(QLatin1StringView("params")).toArray();
    for (const QJsonValue param : params) {
        const QString name = param.toObject().value(QLatin1StringView("name")).toString();
        if (name.isEmpty()) {
            return std::nullopt;
        }
        names.append(name);
    }
    return names;
}

// "(pane, title)", for messages. An unnamed parameter shows as "?". The
// names come from the socket, so each goes through peerText(), and the
// list ends with "..." after MaxListedNames.
QString signature(const QJsonObject& method)
{
    QStringList names;
    const QJsonArray params = method.value(QLatin1StringView("params")).toArray();
    for (const QJsonValue param : params) {
        if (names.size() == MaxListedNames) {
            names.append(QStringLiteral("..."));
            break;
        }
        const QString name = param.toObject().value(QLatin1StringView("name")).toString();
        names.append(name.isEmpty() ? QStringLiteral("?") : peerText(name));
    }
    return QStringLiteral("(%1)").arg(names.join(QStringLiteral(", ")));
}

// "(pane), (pane, direction)", for messages, with the count of the
// overloads that it leaves out after MaxListedOverloads.
QString signatures(const QList<QJsonObject>& overloads)
{
    QStringList listed;
    for (const QJsonObject& overload : overloads) {
        if (listed.size() == MaxListedOverloads) {
            listed.append(QStringLiteral("and %1 more").arg(overloads.size() - MaxListedOverloads));
            break;
        }
        listed.append(signature(overload));
    }
    return listed.join(QStringLiteral(", "));
}

// The JSON Schema of one parameter, from its fragment of the phosphor-ipc
// schema. It takes only the members that the bridge knows. The application
// may run a later phosphor-ipc that adds members, and the MCP server
// refuses a schema with a keyword that it does not know. A fragment has a
// type, the item type of a string list, or a description alone for a type
// that has no JSON form. A QVariant (an untyped QML parameter) has
// nothing, and its property {} takes any value. Nothing, with the type in
// `unknownType`, for a type that the bridge does not know.
std::optional<QJsonObject> propertyOf(const QJsonObject& fragment, QString* unknownType)
{
    static const QSet<QString> knownTypes{
        QStringLiteral("boolean"), QStringLiteral("integer"), QStringLiteral("number"),
        QStringLiteral("string"),  QStringLiteral("array"),   QStringLiteral("object"),
    };
    QJsonObject property;
    const QJsonValue type = fragment.value(QLatin1StringView("type"));
    if (!type.isUndefined()) {
        if (!type.isString() || !knownTypes.contains(type.toString())) {
            *unknownType = type.isString() ? peerText(type.toString()) : QStringLiteral("(not a string)");
            return std::nullopt;
        }
        property.insert(QLatin1StringView("type"), type);
        const QJsonValue itemType =
            fragment.value(QLatin1StringView("items")).toObject().value(QLatin1StringView("type"));
        if (type.toString() == QLatin1StringView("array") && itemType.isString()
            && knownTypes.contains(itemType.toString())) {
            property.insert(QLatin1StringView("items"), QJsonObject{{QLatin1StringView("type"), itemType}});
        }
    }
    const QJsonValue description = fragment.value(QLatin1StringView("description"));
    if (description.isString()) {
        property.insert(QLatin1StringView("description"), description);
    }
    return property;
}

// The tools of a manifest, from the schemas of the targets that the socket
// has. A target that is not in `schemas` gives no tool and no line: the
// caller reported it.
Built buildTools(const McpIpcManifest& manifest, const QHash<QString, QJsonObject>& schemas)
{
    Built built;
    for (const McpIpcManifestTool& entry : manifest.tools) {
        const auto schema = schemas.constFind(entry.target);
        if (schema == schemas.cend()) {
            continue;
        }
        const QString call = QStringLiteral("%1.%2").arg(entry.target, entry.method);
        QList<QJsonObject> overloads;
        const QJsonArray methods = schema->value(QLatin1StringView("functions")).toArray();
        for (const QJsonValue method : methods) {
            if (method.toObject().value(QLatin1StringView("name")).toString() == entry.method) {
                overloads.append(method.toObject());
            }
        }
        if (overloads.isEmpty()) {
            built.warnings.append(
                QStringLiteral("tool \"%1\": the target \"%2\" has no method \"%3\", so the tool is left out")
                    .arg(entry.name, entry.target, entry.method));
            continue;
        }
        qsizetype chosen = -1;
        if (!entry.params) {
            if (overloads.size() > 1) {
                built.error = QStringLiteral(
                                  "tool \"%1\": %2 has %3 overloads, %4, so \"params\" must name the "
                                  "parameters of one")
                                  .arg(entry.name, call, QString::number(overloads.size()), signatures(overloads));
                return built;
            }
            chosen = 0;
        } else {
            for (qsizetype i = 0; i < overloads.size(); ++i) {
                if (parameterNames(overloads.at(i)) == entry.params) {
                    chosen = i;
                    break;
                }
            }
            if (chosen < 0) {
                built.error =
                    QStringLiteral("tool \"%1\": %2 has no overload with the parameters (%3). It has %4")
                        .arg(entry.name, call, entry.params->join(QStringLiteral(", ")), signatures(overloads));
                return built;
            }
        }
        const QJsonObject& method = overloads.at(chosen);
        const std::optional<QStringList> names = parameterNames(method);
        if (!names) {
            built.error = QStringLiteral("tool \"%1\": a parameter of %2%3 has no name, so no argument can name it")
                              .arg(entry.name, call, signature(method));
            return built;
        }
        // The router picks an overload by the number of arguments alone,
        // so the manifest cannot choose between two with the same count.
        for (qsizetype i = 0; i < overloads.size(); ++i) {
            if (i != chosen && overloads.at(i).value(QLatin1StringView("params")).toArray().size() == names->size()) {
                built.error = QStringLiteral(
                                  "tool \"%1\": %2%3 has as many parameters as the overload %2%4, and the "
                                  "socket chooses an overload by its number of arguments alone")
                                  .arg(entry.name, call, signature(method), signature(overloads.at(i)));
                return built;
            }
        }

        // What follows is a method that this bridge cannot call. That is a
        // fact about the application, as a missing method is: the tool is
        // left out, and the other tools stay.
        if (names->size() > PhosphorIpc::MaxCallArgs) {
            built.warnings.append(
                QStringLiteral("tool \"%1\": %2%3 has %4 parameters, and a call takes at most %5, so the tool is "
                               "left out")
                    .arg(entry.name, call, signature(method), QString::number(names->size()),
                         QString::number(PhosphorIpc::MaxCallArgs)));
            continue;
        }
        QJsonObject properties;
        QJsonArray required;
        QString problem;
        const QJsonArray params = method.value(QLatin1StringView("params")).toArray();
        for (qsizetype i = 0; i < params.size(); ++i) {
            const QString& name = names->at(i);
            if (properties.contains(name)) {
                problem = QStringLiteral("has two parameters named \"%1\"").arg(peerText(name));
                break;
            }
            QString unknownType;
            const std::optional<QJsonObject> property = propertyOf(params.at(i).toObject(), &unknownType);
            if (!property) {
                problem = QStringLiteral(
                              "has the parameter \"%1\" of the type \"%2\", which this version does "
                              "not know")
                              .arg(peerText(name), peerText(unknownType));
                break;
            }
            properties.insert(name, *property);
            required.append(name);
        }
        if (!problem.isEmpty()) {
            built.warnings.append(QStringLiteral("tool \"%1\": %2%3 %4, so the tool is left out")
                                      .arg(entry.name, call, signature(method), problem));
            continue;
        }

        // An object with one required property for each parameter, and no
        // other.
        QJsonObject inputSchema{
            {QLatin1StringView("type"), QStringLiteral("object")},
            {QLatin1StringView("additionalProperties"), false},
        };
        if (!required.isEmpty()) {
            inputSchema.insert(QLatin1StringView("properties"), properties);
            inputSchema.insert(QLatin1StringView("required"), required);
        }
        PhosphorMcp::McpTool tool(entry.name);
        tool.setDescription(entry.description).setInputSchema(inputSchema);
        if (!entry.title.isEmpty()) {
            tool.setTitle(entry.title);
        }
        if (entry.readOnly) {
            tool.setReadOnlyHint(*entry.readOnly);
        }
        if (entry.destructive) {
            tool.setDestructiveHint(*entry.destructive);
        }
        if (entry.idempotent) {
            tool.setIdempotentHint(*entry.idempotent);
        }
        if (entry.openWorld) {
            tool.setOpenWorldHint(*entry.openWorld);
        }
        // The server of addTools() checks each tool, and cannot take a tool
        // back. The same check runs here, so that a tool that the server
        // would refuse is left out and addTools() adds all tools. An
        // example is a parameter name that is not valid UTF-16.
        const QString refusal = PhosphorMcp::McpServer::toolProblem(tool);
        if (!refusal.isEmpty()) {
            built.warnings.append(QStringLiteral("tool \"%1\": the MCP server does not take it (%2), so the tool is "
                                                 "left out")
                                      .arg(entry.name, peerText(refusal)));
            continue;
        }
        built.tools.append(Tool{tool, entry.target, entry.method, *names});
    }
    return built;
}

// The text of an error that the client made itself, for the model. The
// client's own words speak of "the server" and "the client", which to the
// model are this MCP server and itself. `closeReason` is why the last
// connection ended or was not made.
QString clientErrorText(const IpcReply& reply, const QString& closeReason)
{
    using namespace PhosphorIpc;
    const QString code = reply.errorCode;
    if (code == QLatin1StringView(ClientErrorCode::NotConnected)) {
        return closeReason.isEmpty()
            ? QStringLiteral("%1: the application's socket is not reachable. The next call tries again.").arg(code)
            : QStringLiteral("%1: the application's socket is not reachable (%2). The next call tries again.")
                  .arg(code, closeReason);
    }
    if (code == QLatin1StringView(ClientErrorCode::ConnectionClosed)) {
        return QStringLiteral(
                   "%1: the connection to the application ended before the answer came (%2). The call "
                   "may have run all the same. The next call connects again.")
            .arg(code, reply.errorMessage);
    }
    if (code == QLatin1StringView(ClientErrorCode::WriteQueueFull)) {
        return QStringLiteral(
                   "%1: the application does not read its socket, and too many calls wait. Try again "
                   "later.")
            .arg(code);
    }
    if (code == QLatin1StringView(ClientErrorCode::RequestTooLarge)) {
        return QStringLiteral(
                   "%1: the arguments are too large. The application's socket takes %2 bytes for one "
                   "call. Send less.")
            .arg(code, QString::number(MaxRequestLineBytes));
    }
    if (code == QLatin1StringView(ClientErrorCode::MalformedReply)) {
        return QStringLiteral("%1: the application answered with an error that has no code: %2")
            .arg(code, reply.errorMessage);
    }
    return QStringLiteral("%1: the application's socket does not take this call: %2").arg(code, reply.errorMessage);
}

// The tool result of a reply. An object is structured content, which the
// library also sends as JSON text. A string is its text, so that the model
// reads it without quotes and escapes. Any other value (a number, a
// boolean, an array, null for a method that returns nothing) is its
// compact JSON as text. An error is a failed call with its code and
// message, and its detail as JSON when it has one.
PhosphorMcp::McpResult resultOf(const IpcReply& reply, const QString& closeReason)
{
    if (reply.isError()) {
        // A target can fail a deferred call with a code of its own choice,
        // also one that reads like a code of the client. Only an error
        // that the client made gets the client's text.
        if (reply.fromClient) {
            return PhosphorMcp::McpResult::error(clientErrorText(reply, closeReason));
        }
        QString text = QStringLiteral("%1: %2").arg(reply.errorCode, reply.errorMessage);
        if (!reply.errorDetail.isEmpty()) {
            text += QStringLiteral("\ndetail: %1")
                        .arg(QString::fromUtf8(QJsonDocument(reply.errorDetail).toJson(QJsonDocument::Compact)));
        }
        return PhosphorMcp::McpResult::error(text);
    }
    if (reply.result.isObject()) {
        return PhosphorMcp::McpResult::structured(reply.result);
    }
    if (reply.result.isString()) {
        return PhosphorMcp::McpResult::text(reply.result.toString());
    }
    return PhosphorMcp::McpResult::text(QString::fromUtf8(reply.result.toJson(QJsonValue::JsonFormat::Compact)));
}

} // namespace

struct McpIpcBridge::Private
{
    enum class Phase {
        Idle,
        Preparing,
        Prepared,
        Failed,
    };

    Private(McpIpcBridge* bridge, McpIpcManifest manifest)
        : q(bridge)
        , manifest(std::move(manifest))
        , client(bridge)
        , timeout(bridge)
    {
    }

    void onSchema(const QString& target, const IpcReply& reply);
    // Why the connection that prepare() made is of no use, from the reason
    // of the client.
    QString connectionProblem(const QString& reason) const;
    // The one end of prepare(). It writes the phase, and prepared() or
    // failed() follows from the event loop.
    void finish(Phase outcome, const QString& error);

    McpIpcBridge* const q;
    McpIpcManifest manifest;
    // Both with the bridge as their parent, so that they move with it to
    // another thread.
    IpcClient client;
    QTimer timeout;
    QString socketPath;
    Phase phase = Phase::Idle;
    // The connection of prepare() was made, so its end is the end of a
    // connection and not a failure to connect.
    bool connected = false;
    // Why the last connection ended or was not made, for the tool calls
    // that find no connection.
    QString closeReason;
    // The schema of each target the socket has, and the targets still to
    // answer. Both are of use only while the phase is Preparing.
    QHash<QString, QJsonObject> schemas;
    int waiting = 0;
    QList<Tool> tools;
    QStringList warnings;
};

McpIpcBridge::McpIpcBridge(McpIpcManifest manifest, QObject* parent)
    : QObject(parent)
    , d(std::make_unique<Private>(this, std::move(manifest)))
{
    connect(&d->client, &IpcClient::connected, this, [this] {
        d->connected = true;
    });
    connect(&d->client, &IpcClient::closed, this, [this](const QString& reason) {
        d->closeReason = reason;
        if (d->phase == Private::Phase::Preparing) {
            d->finish(Private::Phase::Failed, d->connectionProblem(reason));
        }
    });
    d->timeout.setSingleShot(true);
    connect(&d->timeout, &QTimer::timeout, this, [this] {
        if (d->phase == Private::Phase::Preparing) {
            d->finish(Private::Phase::Failed,
                      QStringLiteral("the application's socket at %1 did not answer within %2 ms")
                          .arg(d->socketPath, QString::number(d->timeout.interval())));
        }
    });
}

McpIpcBridge::~McpIpcBridge() = default;

void McpIpcBridge::prepare(const QString& socketPath, int timeoutMs)
{
    using Phase = Private::Phase;
    if (d->phase != Phase::Idle) {
        qCWarning(lcMcpIpc, "McpIpcBridge::prepare runs one time");
        return;
    }
    d->phase = Phase::Preparing;
    d->socketPath = socketPath;

    // A manifest that code built without the parser gets the parser's
    // checks here.
    QString problem = d->manifest.problem();
    if (!problem.isEmpty()) {
        problem = QStringLiteral("the manifest is not valid: %1").arg(problem);
    } else if (d->manifest.tools.isEmpty()) {
        // No tool makes a call, so the socket is of no interest.
        d->finish(Phase::Prepared, {});
        return;
    } else if (!QDir::isAbsolutePath(socketPath)) {
        problem = QStringLiteral("the socket path \"%1\" is not absolute").arg(socketPath);
    } else if (timeoutMs < 1) {
        problem = QStringLiteral("the timeout of %1 ms is under 1 ms").arg(QString::number(timeoutMs));
    }
    if (!problem.isEmpty()) {
        d->finish(Phase::Failed, problem);
        return;
    }

    d->timeout.start(timeoutMs);
    d->client.connectToServer(socketPath);
    // A local socket connects inside the call. connected() comes later.
    d->connected = d->client.state() == IpcClient::State::Connected;

    QStringList targets;
    for (const McpIpcManifestTool& entry : std::as_const(d->manifest.tools)) {
        if (!targets.contains(entry.target)) {
            targets.append(entry.target);
        }
    }
    d->waiting = int(targets.size());
    for (const QString& target : std::as_const(targets)) {
        d->client.schema(target, [this, target](const IpcReply& reply) {
            d->onSchema(target, reply);
        });
    }
}

void McpIpcBridge::Private::onSchema(const QString& target, const IpcReply& reply)
{
    if (phase != Phase::Preparing) {
        return;
    }
    using namespace PhosphorIpc;
    if (reply.fromClient && reply.errorCode == QLatin1StringView(ClientErrorCode::MalformedReply)) {
        // The socket answered, with an error that has no code. The message
        // is the socket's own text.
        finish(Phase::Failed,
               QStringLiteral("the application's socket at %1 gave no schema for the target \"%2\" (an error "
                              "without a code: %3)")
                   .arg(socketPath, target, peerText(reply.errorMessage)));
        return;
    }
    if (reply.fromClient) {
        // The connection ended, or the client could not send the request.
        // The texts are the client's own.
        if (reply.errorCode == QLatin1StringView(ClientErrorCode::ConnectionClosed)) {
            finish(Phase::Failed, connectionProblem(reply.errorMessage));
        } else if (reply.errorCode == QLatin1StringView(ClientErrorCode::NotConnected)) {
            finish(Phase::Failed,
                   connectionProblem(closeReason.isEmpty() ? QStringLiteral("no connection") : closeReason));
        } else {
            finish(Phase::Failed,
                   QStringLiteral("cannot ask the application's socket at %1 for the schema of the target \"%2\" "
                                  "(%3: %4)")
                       .arg(socketPath, target, reply.errorCode, reply.errorMessage));
        }
        return;
    }
    if (reply.isError()) {
        if (reply.errorCode != QLatin1StringView(ErrorCode::NoSuchTarget)) {
            // Any other answer tells nothing about the target, so the
            // tools cannot be built and cannot be left out either.
            finish(Phase::Failed,
                   QStringLiteral("the application's socket at %1 gave no schema for the target \"%2\" (%3: %4)")
                       .arg(socketPath, target, peerText(reply.errorCode), peerText(reply.errorMessage)));
            return;
        }
        QStringList names;
        for (const McpIpcManifestTool& entry : std::as_const(manifest.tools)) {
            if (entry.target == target) {
                names.append(entry.name);
            }
        }
        warnings.append((names.size() == 1 ? QStringLiteral("the socket has no target \"%1\" (%2: %3), so the tool "
                                                            "%4 is left out")
                                           : QStringLiteral("the socket has no target \"%1\" (%2: %3), so the tools "
                                                            "%4 are left out"))
                            .arg(target, peerText(reply.errorCode), peerText(reply.errorMessage),
                                 names.join(QStringLiteral(", "))));
    } else {
        const QJsonObject schema = reply.result.toObject();
        if (!schema.value(QLatin1StringView("functions")).isArray()) {
            finish(Phase::Failed,
                   QStringLiteral("the application's socket at %1 did not answer with a schema for "
                                  "the target \"%2\"")
                       .arg(socketPath, target));
            return;
        }
        schemas.insert(target, schema);
    }
    if (--waiting > 0) {
        return;
    }
    Built built = buildTools(manifest, schemas);
    warnings.append(built.warnings);
    if (!built.error.isEmpty()) {
        finish(Phase::Failed, built.error);
        return;
    }
    tools = std::move(built.tools);
    finish(Phase::Prepared, {});
}

QString McpIpcBridge::Private::connectionProblem(const QString& reason) const
{
    // The reason of a connection that was not made names the path itself.
    if (!connected && reason.contains(socketPath)) {
        return reason;
    }
    return QStringLiteral("the application's socket at %1 ended the connection before the tools were built: %2")
        .arg(socketPath, reason);
}

void McpIpcBridge::Private::finish(Phase outcome, const QString& error)
{
    phase = outcome;
    timeout.stop();
    schemas.clear();
    if (outcome == Phase::Failed) {
        tools.clear();
    }
    // A connection that no tool uses.
    if (tools.isEmpty()) {
        client.close();
    }
    // From the event loop on every path, also for a refusal inside
    // prepare(): a program that calls prepare() before its event loop runs
    // could not act on a signal that came inside the call.
    QMetaObject::invokeMethod(
        q,
        [this, outcome, error] {
            if (outcome == Phase::Prepared) {
                Q_EMIT q->prepared();
            } else {
                Q_EMIT q->failed(error);
            }
        },
        Qt::QueuedConnection);
}

QList<PhosphorMcp::McpTool> McpIpcBridge::tools() const
{
    QList<PhosphorMcp::McpTool> tools;
    for (const Tool& tool : std::as_const(d->tools)) {
        tools.append(tool.tool);
    }
    return tools;
}

QStringList McpIpcBridge::warnings() const
{
    return d->warnings;
}

bool McpIpcBridge::addTools(PhosphorMcp::McpServer& server, QString* error)
{
    const auto refuse = [&](const QString& reason) {
        if (error) {
            *error = reason;
        }
        return false;
    };
    if (d->phase != Private::Phase::Prepared) {
        return refuse(QStringLiteral("prepare() has not finished with prepared()"));
    }
    // The server cannot take a tool back, so the one refusal that depends
    // on the other tools is checked before the first one goes in.
    QSet<QString> taken;
    const QList<PhosphorMcp::McpTool> present = server.tools();
    for (const PhosphorMcp::McpTool& tool : present) {
        taken.insert(tool.name());
    }
    for (const Tool& tool : std::as_const(d->tools)) {
        if (taken.contains(tool.tool.name())) {
            return refuse(
                QStringLiteral("tool \"%1\": the server has a tool of that name already").arg(tool.tool.name()));
        }
    }
    for (const Tool& tool : std::as_const(d->tools)) {
        const auto handler = [this, target = tool.target, method = tool.method,
                              params = tool.params](PhosphorMcp::McpCall call) {
            const QJsonObject arguments = call.arguments();
            QJsonArray args;
            for (const QString& param : params) {
                args.append(arguments.value(param));
            }
            // The application may have restarted since its socket closed.
            // A connection that cannot be made fails the call below, after
            // closed() gave the reason.
            if (d->client.state() == IpcClient::State::Unconnected) {
                d->client.connectToServer(d->socketPath);
            }
            // McpCall moves and does not copy, and the client's callback is a
            // std::function, which must copy.
            const auto pending = std::make_shared<PhosphorMcp::McpCall>(std::move(call));
            d->client.call(target, method, args, [this, pending](const IpcReply& reply) {
                pending->finish(resultOf(reply, d->closeReason));
            });
        };
        QString reason;
        if (!server.addTool(tool.tool, handler, &reason)) {
            return refuse(QStringLiteral("tool \"%1\": %2").arg(tool.tool.name(), reason));
        }
    }
    if (error) {
        error->clear();
    }
    return true;
}

} // namespace PhosphorMcpIpc
