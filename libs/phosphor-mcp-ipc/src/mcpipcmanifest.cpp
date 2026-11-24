// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorMcpIpc/McpIpcManifest.h>

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QLatin1StringView>
#include <QSet>

namespace PhosphorMcpIpc {

namespace {

// Larger than any manifest an application writes by hand, and small enough
// that a wrong path (a log, an image) is refused before it is read whole.
constexpr qint64 MaxManifestBytes = 4 * 1024 * 1024;

const QSet<QString>& toolKeys()
{
    static const QSet<QString> keys{
        QStringLiteral("call"),        QStringLiteral("name"),   QStringLiteral("title"),
        QStringLiteral("description"), QStringLiteral("params"), QStringLiteral("annotations"),
    };
    return keys;
}

// Reads one optional boolean hint of "annotations".
bool readHint(const QJsonObject& annotations, QLatin1StringView key, std::optional<bool>* hint, QString* error)
{
    const QJsonValue value = annotations.value(key);
    if (value.isUndefined()) {
        return true;
    }
    if (!value.isBool()) {
        *error = QStringLiteral("annotations.%1 is not a boolean").arg(key);
        return false;
    }
    *hint = value.toBool();
    return true;
}

bool readTool(const QJsonObject& object, McpIpcManifestTool* tool, QString* error)
{
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (!toolKeys().contains(it.key())) {
            *error = QStringLiteral("unknown member \"%1\"").arg(it.key());
            return false;
        }
    }

    const QJsonValue call = object.value(QLatin1StringView("call"));
    const QString callText = call.toString();
    const qsizetype dot = callText.lastIndexOf(QLatin1Char('.'));
    if (!call.isString() || dot <= 0 || dot == callText.size() - 1) {
        *error = QStringLiteral("\"call\" is not a string of the form TARGET.METHOD");
        return false;
    }
    tool->target = callText.left(dot);
    tool->function = callText.mid(dot + 1);

    const QJsonValue name = object.value(QLatin1StringView("name"));
    if (!name.isString() || name.toString().isEmpty()) {
        *error = QStringLiteral("\"name\" is not a string that is not empty");
        return false;
    }
    tool->name = name.toString();

    const QJsonValue title = object.value(QLatin1StringView("title"));
    if (!title.isUndefined() && !title.isString()) {
        *error = QStringLiteral("\"title\" is not a string");
        return false;
    }
    tool->title = title.toString();

    // The model reads only the description to choose a tool, so a tool
    // without one is a mistake of the manifest.
    const QJsonValue description = object.value(QLatin1StringView("description"));
    if (!description.isString() || description.toString().isEmpty()) {
        *error = QStringLiteral("\"description\" is not a string that is not empty");
        return false;
    }
    tool->description = description.toString();

    const QJsonValue params = object.value(QLatin1StringView("params"));
    if (!params.isUndefined()) {
        if (!params.isArray()) {
            *error = QStringLiteral("\"params\" is not an array");
            return false;
        }
        QStringList names;
        for (const QJsonValue param : params.toArray()) {
            if (!param.isString() || param.toString().isEmpty()) {
                *error = QStringLiteral("\"params\" holds a value that is not a string that is not empty");
                return false;
            }
            if (names.contains(param.toString())) {
                *error = QStringLiteral("\"params\" names \"%1\" twice").arg(param.toString());
                return false;
            }
            names.append(param.toString());
        }
        tool->params = names;
    }

    const QJsonValue annotations = object.value(QLatin1StringView("annotations"));
    if (!annotations.isUndefined()) {
        if (!annotations.isObject()) {
            *error = QStringLiteral("\"annotations\" is not an object");
            return false;
        }
        const QJsonObject hints = annotations.toObject();
        for (auto it = hints.begin(); it != hints.end(); ++it) {
            if (it.key() != QLatin1StringView("readOnly") && it.key() != QLatin1StringView("destructive")
                && it.key() != QLatin1StringView("idempotent") && it.key() != QLatin1StringView("openWorld")) {
                *error = QStringLiteral("unknown member \"annotations.%1\"").arg(it.key());
                return false;
            }
        }
        if (!readHint(hints, QLatin1StringView("readOnly"), &tool->readOnly, error)
            || !readHint(hints, QLatin1StringView("destructive"), &tool->destructive, error)
            || !readHint(hints, QLatin1StringView("idempotent"), &tool->idempotent, error)
            || !readHint(hints, QLatin1StringView("openWorld"), &tool->openWorld, error)) {
            return false;
        }
    }
    return true;
}

} // namespace

std::optional<McpIpcManifest> McpIpcManifest::parse(const QByteArray& json, QString* error)
{
    QString reason;
    const auto refuse = [&](const QString& message) -> std::optional<McpIpcManifest> {
        if (error) {
            *error = message;
        }
        return std::nullopt;
    };

    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        return refuse(QStringLiteral("not JSON at offset %1: %2").arg(parseError.offset).arg(parseError.errorString()));
    }
    if (!document.isObject()) {
        return refuse(QStringLiteral("the root is not an object"));
    }
    const QJsonObject root = document.object();
    for (auto it = root.begin(); it != root.end(); ++it) {
        if (it.key() != QLatin1StringView("tools")) {
            return refuse(QStringLiteral("unknown member \"%1\" at the root").arg(it.key()));
        }
    }
    const QJsonValue tools = root.value(QLatin1StringView("tools"));
    if (!tools.isArray()) {
        return refuse(QStringLiteral("\"tools\" is not an array"));
    }

    McpIpcManifest manifest;
    QSet<QString> names;
    const QJsonArray entries = tools.toArray();
    for (qsizetype i = 0; i < entries.size(); ++i) {
        if (!entries.at(i).isObject()) {
            return refuse(QStringLiteral("tools[%1] is not an object").arg(i));
        }
        McpIpcManifestTool tool;
        if (!readTool(entries.at(i).toObject(), &tool, &reason)) {
            return refuse(QStringLiteral("tools[%1]: %2").arg(i).arg(reason));
        }
        if (names.contains(tool.name)) {
            return refuse(QStringLiteral("tools[%1]: another tool is named \"%2\"").arg(i).arg(tool.name));
        }
        names.insert(tool.name);
        manifest.tools.append(tool);
    }
    return manifest;
}

std::optional<McpIpcManifest> McpIpcManifest::load(const QString& path, QString* error, bool* missing)
{
    if (missing) {
        *missing = false;
    }
    if (!QFileInfo::exists(path)) {
        if (missing) {
            *missing = true;
        }
        return McpIpcManifest();
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = QStringLiteral("cannot read %1: %2").arg(path, file.errorString());
        }
        return std::nullopt;
    }
    const QByteArray json = file.read(MaxManifestBytes + 1);
    if (json.size() > MaxManifestBytes) {
        if (error) {
            *error = QStringLiteral("%1 is larger than %2 bytes").arg(path).arg(MaxManifestBytes);
        }
        return std::nullopt;
    }
    QString reason;
    std::optional<McpIpcManifest> manifest = parse(json, &reason);
    if (!manifest && error) {
        *error = QStringLiteral("%1: %2").arg(path, reason);
    }
    return manifest;
}

} // namespace PhosphorMcpIpc
