// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: LGPL-2.1-or-later

// phosphor-mcp-ipc: an MCP server on stdio that offers the calls of an
// application's phosphor-ipc socket that the application's manifest allows.
// An MCP client starts it:
//
//     {"mcpServers": {"app": {"command": "phosphor-mcp-ipc",
//                             "args": ["--socket", "/run/user/1000/app.sock",
//                                      "--manifest", "/usr/share/app/mcp.json"]}}}
//
// Exit codes: 0 when the client ends the server, 1 when the server cannot
// run or its input or output fails, 2 for a wrong command line.

#include <PhosphorMcp/McpServer.h>
#include <PhosphorMcpIpc/McpIpcBridge.h>
#include <PhosphorMcpIpc/McpIpcManifest.h>

#include <QByteArrayView>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QString>
#include <QStringList>
#include <QTextStream>
#include <QtEnvironmentVariables>

#include <cstdio>
#include <optional>

using namespace PhosphorMcpIpc;

namespace {

// To stderr whatever the logging rules say: the reason the server does not
// run is for the person who reads the client's log.
void report(const QString& message)
{
    QTextStream(stderr) << "phosphor-mcp-ipc: " << message << '\n';
}

// A wrong command line: the reason, the usage line, and exit code 2.
int usageError(const QString& message)
{
    report(message);
    QTextStream(stderr) << "usage: phosphor-mcp-ipc --socket PATH --manifest FILE\n";
    return 2;
}

} // namespace

int main(int argc, char** argv)
{
    // A client keeps the server's stderr as its log, and Qt writes there only
    // when the process has a terminal unless it is asked to.
    if (!qEnvironmentVariableIsSet("QT_FORCE_STDERR_LOGGING")) {
        qputenv("QT_FORCE_STDERR_LOGGING", QByteArrayView("1"));
    }
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("phosphor-mcp-ipc"));
    QCoreApplication::setApplicationVersion(QStringLiteral(PHOSPHORMCPIPC_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "An MCP server on stdio that offers the calls of a phosphor-ipc socket that a manifest allows."));
    const QCommandLineOption helpOption = parser.addHelpOption();
    const QCommandLineOption versionOption = parser.addVersionOption();
    const QCommandLineOption socketOption(QStringLiteral("socket"),
                                          QStringLiteral("The application's socket, as an absolute path (required)."),
                                          QStringLiteral("path"));
    const QCommandLineOption manifestOption(QStringLiteral("manifest"),
                                            QStringLiteral("The manifest of the calls to offer (required)."),
                                            QStringLiteral("file"));
    parser.addOption(socketOption);
    parser.addOption(manifestOption);
    // parse(), not process(): process() ends the program with exit code 1
    // for an unknown option, which is the code of a server that cannot run.
    if (!parser.parse(QCoreApplication::arguments())) {
        return usageError(parser.errorText());
    }
    // addHelpOption() also makes --help-all, which has no option object.
    if (parser.isSet(helpOption) || parser.isSet(QStringLiteral("help-all"))) {
        parser.showHelp(0);
    }
    if (parser.isSet(versionOption)) {
        parser.showVersion();
    }
    if (!parser.positionalArguments().isEmpty()) {
        return usageError(QStringLiteral("unexpected argument \"%1\"").arg(parser.positionalArguments().constFirst()));
    }
    for (const QCommandLineOption& option : {socketOption, manifestOption}) {
        const QString name = option.names().constFirst();
        if (!parser.isSet(option)) {
            return usageError(QStringLiteral("--%1 is missing").arg(name));
        }
        if (parser.values(option).size() > 1) {
            return usageError(QStringLiteral("--%1 is given more than one time").arg(name));
        }
    }
    const QString socketPath = parser.value(socketOption);
    const QString manifestPath = parser.value(manifestOption);
    // QLocalSocket takes a name that is not absolute as a file in the
    // temporary directory, where another user can listen.
    if (!QDir::isAbsolutePath(socketPath)) {
        return usageError(QStringLiteral("--socket needs an absolute path, and got \"%1\"").arg(socketPath));
    }
    if (manifestPath.isEmpty()) {
        return usageError(QStringLiteral("--manifest needs a path"));
    }

    QString error;
    bool missing = false;
    const std::optional<McpIpcManifest> manifest = McpIpcManifest::load(manifestPath, &error, &missing);
    if (!manifest) {
        report(error);
        return 1;
    }
    if (missing) {
        report(QStringLiteral("no manifest at %1, so the server offers no tools").arg(manifestPath));
    } else if (manifest->tools.isEmpty()) {
        report(QStringLiteral("the manifest at %1 lists no tools, so the server offers none").arg(manifestPath));
    }

    // The bridge first, so that it outlives the server: each tool handler
    // of the server calls the bridge.
    McpIpcBridge bridge(*manifest);
    PhosphorMcp::McpServer server;
    // What the bridge left out, also when the start fails after it.
    const auto reportWarnings = [&bridge] {
        const QStringList warnings = bridge.warnings();
        for (const QString& warning : warnings) {
            report(warning);
        }
    };
    QObject::connect(&bridge, &McpIpcBridge::failed, &app, [&](const QString& reason) {
        reportWarnings();
        report(reason);
        QCoreApplication::exit(1);
    });
    QObject::connect(&bridge, &McpIpcBridge::prepared, &app, [&] {
        reportWarnings();
        QString reason;
        if (!bridge.addTools(server, &reason)) {
            report(reason);
            QCoreApplication::exit(1);
            return;
        }
        // SIGTERM from the client ends the server immediately. The end of
        // stdin sends the replies that are written, and drops the answers
        // of the tool calls that still wait.
        if (!server.handleTerminationSignals()) {
            report(
                QStringLiteral("cannot handle the signals that end the server, so a signal ends it without "
                               "the replies that wait"));
        }
        using Reason = PhosphorMcp::McpServer::FinishReason;
        QObject::connect(&server, &PhosphorMcp::McpServer::finished, &app, [](Reason finish) {
            QCoreApplication::exit(finish == Reason::InputFailed || finish == Reason::OutputFailed ? 1 : 0);
        });
        if (!server.start()) {
            report(QStringLiteral("cannot start the MCP server on stdin and stdout"));
            QCoreApplication::exit(1);
        }
    });
    bridge.prepare(socketPath);
    return app.exec();
}
