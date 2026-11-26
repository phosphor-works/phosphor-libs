// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorMcpIpc/McpIpcManifest.h>

#include <PhosphorMcp/McpTool.h>

#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QLatin1StringView>
#include <QSet>

#include <cerrno>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

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

QString errnoText(int error)
{
    return QString::fromStdString(std::generic_category().message(error));
}

// What is wrong with the values of one tool, or nothing. parse() and
// McpIpcManifest::problem() share it, so that a manifest from a file and
// one that code built have one set of rules.
QString toolProblem(const McpIpcManifestTool& tool)
{
    if (tool.target.isEmpty() || tool.method.isEmpty() || tool.method.contains(QLatin1Char('.'))) {
        return QStringLiteral("\"call\" is not of the form TARGET.METHOD");
    }
    if (!PhosphorMcp::McpTool::isValidName(tool.name)) {
        return QStringLiteral("\"name\" must be 1 to 128 of A-Z, a-z, 0-9, '_', '-' and '.'");
    }
    // The model reads only the description to choose a tool, so a tool
    // without one is a mistake of the manifest.
    if (tool.description.isEmpty()) {
        return QStringLiteral("\"description\" must not be empty");
    }
    if (tool.params) {
        QStringList names;
        for (const QString& name : *tool.params) {
            if (name.isEmpty()) {
                return QStringLiteral("\"params\" holds an empty name");
            }
            if (names.contains(name)) {
                return QStringLiteral("\"params\" names \"%1\" twice").arg(name);
            }
            names.append(name);
        }
    }
    return {};
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
    if (call.isUndefined()) {
        *error = QStringLiteral("there is no \"call\"");
        return false;
    }
    const QString callText = call.toString();
    const qsizetype dot = callText.lastIndexOf(QLatin1Char('.'));
    if (!call.isString() || dot <= 0 || dot == callText.size() - 1) {
        *error = QStringLiteral("\"call\" is not a string of the form TARGET.METHOD");
        return false;
    }
    tool->target = callText.left(dot);
    tool->method = callText.mid(dot + 1);

    const QJsonValue name = object.value(QLatin1StringView("name"));
    if (name.isUndefined()) {
        *error = QStringLiteral("there is no \"name\"");
        return false;
    }
    if (!name.isString()) {
        *error = QStringLiteral("\"name\" is not a string");
        return false;
    }
    tool->name = name.toString();

    const QJsonValue title = object.value(QLatin1StringView("title"));
    if (!title.isUndefined()) {
        if (!title.isString()) {
            *error = QStringLiteral("\"title\" is not a string");
            return false;
        }
        if (title.toString().isEmpty()) {
            *error = QStringLiteral("\"title\" must not be empty");
            return false;
        }
    }
    tool->title = title.toString();

    const QJsonValue description = object.value(QLatin1StringView("description"));
    if (description.isUndefined()) {
        *error = QStringLiteral("there is no \"description\"");
        return false;
    }
    if (!description.isString()) {
        *error = QStringLiteral("\"description\" is not a string");
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
        const QJsonArray entries = params.toArray();
        for (const QJsonValue param : entries) {
            if (!param.isString()) {
                *error = QStringLiteral("\"params\" holds a value that is not a string");
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

    *error = toolProblem(*tool);
    return error->isEmpty();
}

} // namespace

QString McpIpcManifest::problem() const
{
    QSet<QString> names;
    for (qsizetype i = 0; i < tools.size(); ++i) {
        const QString reason = toolProblem(tools.at(i));
        if (!reason.isEmpty()) {
            return QStringLiteral("tools[%1]: %2").arg(QString::number(i), reason);
        }
        if (names.contains(tools.at(i).name)) {
            return QStringLiteral("tools[%1]: another tool is named \"%2\"").arg(QString::number(i), tools.at(i).name);
        }
        names.insert(tools.at(i).name);
    }
    return {};
}

std::optional<McpIpcManifest> McpIpcManifest::parse(const QByteArray& json, QString* error)
{
    const auto refuse = [&](const QString& message) -> std::optional<McpIpcManifest> {
        if (error) {
            *error = message;
        }
        return std::nullopt;
    };

    // QJsonValue reads any JSON text, where QJsonDocument takes only an
    // object or an array and would call a number at the root "not JSON".
    QJsonParseError parseError{};
    const QJsonValue value = QJsonValue::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        return refuse(QStringLiteral("not JSON at offset %1: %2")
                          .arg(QString::number(parseError.offset), parseError.errorString()));
    }
    if (!value.isObject()) {
        return refuse(QStringLiteral("the root is not an object"));
    }
    const QJsonObject root = value.toObject();
    for (auto it = root.begin(); it != root.end(); ++it) {
        if (it.key() != QLatin1StringView("tools")) {
            return refuse(QStringLiteral("unknown member \"%1\" at the root").arg(it.key()));
        }
    }
    const QJsonValue tools = root.value(QLatin1StringView("tools"));
    if (tools.isUndefined()) {
        return refuse(QStringLiteral("the root has no \"tools\""));
    }
    if (!tools.isArray()) {
        return refuse(QStringLiteral("\"tools\" is not an array"));
    }

    McpIpcManifest manifest;
    QSet<QString> names;
    const QJsonArray entries = tools.toArray();
    for (qsizetype i = 0; i < entries.size(); ++i) {
        const QString index = QString::number(i);
        if (!entries.at(i).isObject()) {
            return refuse(QStringLiteral("tools[%1] is not an object").arg(index));
        }
        McpIpcManifestTool tool;
        QString reason;
        if (!readTool(entries.at(i).toObject(), &tool, &reason)) {
            return refuse(QStringLiteral("tools[%1]: %2").arg(index, reason));
        }
        if (names.contains(tool.name)) {
            return refuse(QStringLiteral("tools[%1]: another tool is named \"%2\"").arg(index, tool.name));
        }
        names.insert(tool.name);
        manifest.tools.append(tool);
    }
    if (error) {
        error->clear();
    }
    return manifest;
}

std::optional<McpIpcManifest> McpIpcManifest::load(const QString& path, QString* error, bool* missing)
{
    if (missing) {
        *missing = false;
    }
    const auto refuse = [&](const QString& message) -> std::optional<McpIpcManifest> {
        if (error) {
            *error = message;
        }
        return std::nullopt;
    };
    if (path.isEmpty()) {
        return refuse(QStringLiteral("the path of the manifest is empty"));
    }

    // Without waiting: a plain open() of a FIFO waits for a writer, and
    // the server would never start. The flag changes nothing for a file.
    // O_NOCTTY, so that a terminal device at the path does not become the
    // terminal of this process before the check below refuses it.
    const QByteArray name = QFile::encodeName(path);
    const int descriptor = ::open(name.constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOCTTY);
    if (descriptor < 0) {
        const int openError = errno;
        if (openError == ENOENT) {
            // No file, or a link to no file. Only the first is "no manifest":
            // a link shows that someone meant a manifest to be here.
            struct stat link = {};
            if (::lstat(name.constData(), &link) != 0) {
                if (missing) {
                    *missing = true;
                }
                if (error) {
                    error->clear();
                }
                return McpIpcManifest();
            }
            return refuse(QStringLiteral("cannot read %1: it is a link to a file that does not exist").arg(path));
        }
        return refuse(QStringLiteral("cannot read %1: %2").arg(path, errnoText(openError)));
    }
    struct stat status = {};
    if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode)) {
        ::close(descriptor);
        return refuse(QStringLiteral("%1 is not a regular file").arg(path));
    }
    QFile file;
    if (!file.open(descriptor, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        ::close(descriptor);
        return refuse(QStringLiteral("cannot read %1: %2").arg(path, file.errorString()));
    }
    const QByteArray json = file.read(MaxManifestBytes + 1);
    if (file.error() != QFileDevice::NoError) {
        return refuse(QStringLiteral("cannot read %1: %2").arg(path, file.errorString()));
    }
    if (json.size() > MaxManifestBytes) {
        return refuse(QStringLiteral("%1 is larger than %2 bytes").arg(path, QString::number(MaxManifestBytes)));
    }
    QString reason;
    std::optional<McpIpcManifest> manifest = parse(json, &reason);
    if (!manifest) {
        return refuse(QStringLiteral("%1: %2").arg(path, reason));
    }
    if (error) {
        error->clear();
    }
    return manifest;
}

} // namespace PhosphorMcpIpc
