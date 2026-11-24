// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorMcpIpc/McpIpcBridge.h>

#include <PhosphorIpc/IpcClient.h>
#include <PhosphorIpc/IpcProtocol.h>
#include <PhosphorMcp/McpCall.h>
#include <PhosphorMcp/McpResult.h>
#include <PhosphorMcp/McpServer.h>
#include <PhosphorMcp/McpTool.h>

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLatin1StringView>
#include <QLoggingCategory>
#include <QMetaObject>
#include <QTimer>

#include <memory>
#include <optional>
#include <utility>

namespace PhosphorMcpIpc {

namespace {

Q_LOGGING_CATEGORY(lcMcpIpc, "phosphormcpipc")

using PhosphorIpc::IpcClient;
using PhosphorIpc::IpcReply;

// A tool that the bridge built, and the call it makes.
struct Tool
{
    PhosphorMcp::McpTool tool;
    QString target;
    QString function;
    // The parameter names, in the order of the call's positional arguments.
    QStringList params;
};

// The names of a function's parameters in the phosphor-ipc schema, in
// order, or nothing when one has no name (a C++ declaration without one).
std::optional<QStringList> parameterNames(const QJsonObject& function)
{
    QStringList names;
    for (const QJsonValue param : function.value(QLatin1StringView("params")).toArray()) {
        const QString name = param.toObject().value(QLatin1StringView("name")).toString();
        if (name.isEmpty()) {
            return std::nullopt;
        }
        names.append(name);
    }
    return names;
}

// "(pane, title)", for messages. An unnamed parameter shows as "?".
QString signature(const QJsonObject& function)
{
    QStringList names;
    for (const QJsonValue param : function.value(QLatin1StringView("params")).toArray()) {
        const QString name = param.toObject().value(QLatin1StringView("name")).toString();
        names.append(name.isEmpty() ? QStringLiteral("?") : name);
    }
    return QStringLiteral("(%1)").arg(names.join(QStringLiteral(", ")));
}

// An object with one required property for each parameter, and no other.
// Each property is the parameter's fragment of the phosphor-ipc schema
// without its name, which is no JSON Schema keyword: a type, the items of a
// string list, or a description alone for a type that has no JSON form, and
// {} for a QVariant (an untyped QML parameter), which takes any value.
QJsonObject inputSchema(const QJsonObject& function)
{
    QJsonObject schema{
        {QLatin1StringView("type"), QStringLiteral("object")},
        {QLatin1StringView("additionalProperties"), false},
    };
    QJsonObject properties;
    QJsonArray required;
    for (const QJsonValue param : function.value(QLatin1StringView("params")).toArray()) {
        QJsonObject fragment = param.toObject();
        const QString name = fragment.take(QLatin1StringView("name")).toString();
        properties.insert(name, fragment);
        required.append(name);
    }
    if (!required.isEmpty()) {
        schema.insert(QLatin1StringView("properties"), properties);
        schema.insert(QLatin1StringView("required"), required);
    }
    return schema;
}

// The tool result of a reply. An object is structured content, which the
// library also sends as JSON text. A string is its text, so that the model
// reads it without quotes and escapes. Any other value (a number, a
// boolean, an array, null for a method that returns nothing) is its
// compact JSON as text. An error is a failed call with its code and
// message, and its detail as JSON when it has one.
PhosphorMcp::McpResult resultOf(const IpcReply& reply)
{
    if (reply.isError()) {
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
    // QJsonDocument holds only an object or an array, so the value goes in
    // an array whose brackets are then cut off.
    const QByteArray json = QJsonDocument(QJsonArray{reply.result}).toJson(QJsonDocument::Compact);
    return PhosphorMcp::McpResult::text(QString::fromUtf8(json.mid(1, json.size() - 2)));
}

} // namespace

struct McpIpcBridge::Private
{
    explicit Private(McpIpcManifest manifest)
        : manifest(std::move(manifest))
    {
    }

    McpIpcManifest manifest;
    IpcClient client;
    QString socketPath;
    QTimer timeout;
    bool started = false;
    bool preparing = false;
    // The schema of each target the socket has, and the targets still to
    // answer.
    QHash<QString, QJsonObject> schemas;
    int waiting = 0;
    QList<Tool> tools;
    QStringList warnings;
};

McpIpcBridge::McpIpcBridge(McpIpcManifest manifest, QObject* parent)
    : QObject(parent)
    , d(std::make_unique<Private>(std::move(manifest)))
{
}

McpIpcBridge::~McpIpcBridge() = default;

void McpIpcBridge::prepare(const QString& socketPath, int timeoutMs)
{
    if (d->started) {
        qCWarning(lcMcpIpc, "McpIpcBridge::prepare runs once");
        return;
    }
    d->started = true;
    d->socketPath = socketPath;

    if (d->manifest.tools.isEmpty()) {
        QMetaObject::invokeMethod(
            this,
            [this] {
                Q_EMIT prepared();
            },
            Qt::QueuedConnection);
        return;
    }

    const auto fail = [this](const QString& error) {
        if (!d->preparing) {
            return;
        }
        d->preparing = false;
        d->timeout.stop();
        d->client.close();
        Q_EMIT failed(error);
    };

    const auto build = [this, fail] {
        for (const McpIpcManifestTool& entry : std::as_const(d->manifest.tools)) {
            const auto schema = d->schemas.constFind(entry.target);
            if (schema == d->schemas.cend()) {
                // Warned once for the target.
                continue;
            }
            const QString call = QStringLiteral("%1.%2").arg(entry.target, entry.function);
            QList<QJsonObject> overloads;
            for (const QJsonValue function : schema->value(QLatin1StringView("functions")).toArray()) {
                if (function.toObject().value(QLatin1StringView("name")).toString() == entry.function) {
                    overloads.append(function.toObject());
                }
            }
            if (overloads.isEmpty()) {
                const QString warning = QStringLiteral(
                                            "tool \"%1\": the target \"%2\" has no function \"%3\", so the "
                                            "tool is left out")
                                            .arg(entry.name, entry.target, entry.function);
                qCWarning(lcMcpIpc).noquote() << warning;
                d->warnings.append(warning);
                continue;
            }
            QStringList signatures;
            for (const QJsonObject& overload : std::as_const(overloads)) {
                signatures.append(signature(overload));
            }

            qsizetype chosen = -1;
            if (!entry.params) {
                if (overloads.size() > 1) {
                    fail(QStringLiteral("tool \"%1\": %2 has %3 overloads, %4, so \"params\" must name the "
                                        "parameters of one")
                             .arg(entry.name, call)
                             .arg(overloads.size())
                             .arg(signatures.join(QStringLiteral(", "))));
                    return;
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
                    fail(QStringLiteral("tool \"%1\": %2 has no overload with the parameters (%3). It has %4")
                             .arg(entry.name, call, entry.params->join(QStringLiteral(", ")),
                                  signatures.join(QStringLiteral(", "))));
                    return;
                }
            }
            const QJsonObject& function = overloads.at(chosen);
            const std::optional<QStringList> names = parameterNames(function);
            if (!names) {
                fail(QStringLiteral("tool \"%1\": a parameter of %2%3 has no name, so no argument can name it")
                         .arg(entry.name, call, signature(function)));
                return;
            }
            // The router picks an overload by the number of arguments alone,
            // so one of two overloads with the same count is never called.
            for (qsizetype i = 0; i < overloads.size(); ++i) {
                if (i != chosen
                    && overloads.at(i).value(QLatin1StringView("params")).toArray().size() == names->size()) {
                    fail(QStringLiteral("tool \"%1\": %2%3 has as many parameters as the overload %2%4, and the "
                                        "socket chooses an overload by its number of arguments alone")
                             .arg(entry.name, call, signature(function), signature(overloads.at(i))));
                    return;
                }
            }

            PhosphorMcp::McpTool tool(entry.name);
            tool.setDescription(entry.description).setInputSchema(inputSchema(function));
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
            d->tools.append(Tool{tool, entry.target, entry.function, *names});
        }
        d->preparing = false;
        d->timeout.stop();
        Q_EMIT prepared();
    };

    d->preparing = true;
    connect(&d->client, &IpcClient::closed, this, [this, fail](const QString& reason) {
        fail(QStringLiteral("cannot reach the application's socket at %1: %2").arg(d->socketPath, reason));
    });
    d->timeout.setSingleShot(true);
    connect(&d->timeout, &QTimer::timeout, this, [this, fail, timeoutMs] {
        fail(QStringLiteral("the application's socket at %1 did not answer within %2 ms")
                 .arg(d->socketPath)
                 .arg(timeoutMs));
    });
    d->timeout.start(timeoutMs);
    d->client.connectToServer(socketPath);

    QStringList targets;
    for (const McpIpcManifestTool& entry : std::as_const(d->manifest.tools)) {
        if (!targets.contains(entry.target)) {
            targets.append(entry.target);
        }
    }
    d->waiting = int(targets.size());
    for (const QString& target : std::as_const(targets)) {
        d->client.schema(target, [this, target, fail, build](const IpcReply& reply) {
            if (!d->preparing) {
                return;
            }
            if (reply.errorCode == QLatin1StringView(PhosphorIpc::ClientErrorCode::ConnectionClosed)
                || reply.errorCode == QLatin1StringView(PhosphorIpc::ClientErrorCode::NotConnected)) {
                fail(QStringLiteral("cannot reach the application's socket at %1: %2")
                         .arg(d->socketPath, reply.errorMessage));
                return;
            }
            if (reply.isError()) {
                QStringList names;
                for (const McpIpcManifestTool& entry : std::as_const(d->manifest.tools)) {
                    if (entry.target == target) {
                        names.append(entry.name);
                    }
                }
                const QString warning =
                    QStringLiteral(
                        "the socket has no target \"%1\" (%2: %3), so the tools %4 are "
                        "left out")
                        .arg(target, reply.errorCode, reply.errorMessage, names.join(QStringLiteral(", ")));
                qCWarning(lcMcpIpc).noquote() << warning;
                d->warnings.append(warning);
            } else {
                d->schemas.insert(target, reply.result.toObject());
            }
            if (--d->waiting == 0) {
                build();
            }
        });
    }
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
    for (const Tool& tool : std::as_const(d->tools)) {
        const auto handler = [this, target = tool.target, function = tool.function,
                              params = tool.params](PhosphorMcp::McpCall call) {
            const QJsonObject arguments = call.arguments();
            QJsonArray args;
            for (const QString& param : params) {
                args.append(arguments.value(param));
            }
            // The application may have restarted since its socket closed.
            if (d->client.state() == IpcClient::State::Unconnected) {
                d->client.connectToServer(d->socketPath);
                if (d->client.state() == IpcClient::State::Unconnected) {
                    call.finish(PhosphorMcp::McpResult::error(
                        QStringLiteral("%1: cannot reach the application's socket at %2")
                            .arg(QLatin1StringView(PhosphorIpc::ClientErrorCode::NotConnected), d->socketPath)));
                    return;
                }
            }
            // McpCall moves and does not copy, and the client's callback is a
            // std::function, which must copy.
            const auto pending = std::make_shared<PhosphorMcp::McpCall>(std::move(call));
            d->client.call(target, function, args, [pending](const IpcReply& reply) {
                pending->finish(resultOf(reply));
            });
        };
        QString reason;
        if (!server.addTool(tool.tool, handler, &reason)) {
            if (error) {
                *error = QStringLiteral("tool \"%1\": %2").arg(tool.tool.name(), reason);
            }
            return false;
        }
    }
    return true;
}

} // namespace PhosphorMcpIpc
