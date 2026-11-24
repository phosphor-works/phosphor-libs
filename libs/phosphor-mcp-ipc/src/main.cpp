// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// phosphor-mcp-ipc: an MCP server on stdio that offers the calls of an
// application's phosphor-ipc socket that the application's manifest allows.
// An MCP client starts it:
//
//     {"mcpServers": {"app": {"command": "phosphor-mcp-ipc",
//                             "args": ["--socket", "/run/user/1000/app.sock",
//                                      "--manifest", "/usr/share/app/mcp.json"]}}}

#include <PhosphorMcp/McpServer.h>
#include <PhosphorMcpIpc/McpIpcBridge.h>
#include <PhosphorMcpIpc/McpIpcManifest.h>

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QTextStream>
#include <QtEnvironmentVariables>

#include <cstdio>

using namespace PhosphorMcpIpc;

namespace {

// To stderr whatever the logging rules say: the reason the server does not
// run is for the person who reads the client's log.
void report(const QString& message)
{
    QTextStream(stderr) << "phosphor-mcp-ipc: " << message << '\n';
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
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption socketOption(QStringLiteral("socket"), QStringLiteral("The application's socket."),
                                          QStringLiteral("path"));
    const QCommandLineOption manifestOption(
        QStringLiteral("manifest"), QStringLiteral("The manifest of the calls to offer."), QStringLiteral("file"));
    parser.addOption(socketOption);
    parser.addOption(manifestOption);
    parser.process(app);
    if (!parser.isSet(socketOption) || !parser.isSet(manifestOption) || !parser.positionalArguments().isEmpty()) {
        report(QStringLiteral("usage: phosphor-mcp-ipc --socket PATH --manifest FILE"));
        return 2;
    }
    const QString socketPath = parser.value(socketOption);
    const QString manifestPath = parser.value(manifestOption);

    QString error;
    bool missing = false;
    const std::optional<McpIpcManifest> manifest = McpIpcManifest::load(manifestPath, &error, &missing);
    if (!manifest) {
        report(error);
        return 1;
    }
    if (missing) {
        report(QStringLiteral("no manifest at %1, so the server offers no tools").arg(manifestPath));
    }

    PhosphorMcp::McpServer server;
    McpIpcBridge bridge(*manifest);
    QObject::connect(&bridge, &McpIpcBridge::failed, &app, [](const QString& reason) {
        report(reason);
        QCoreApplication::exit(1);
    });
    QObject::connect(&bridge, &McpIpcBridge::prepared, &app, [&] {
        QString reason;
        if (!bridge.addTools(server, &reason)) {
            report(reason);
            QCoreApplication::exit(1);
            return;
        }
        // SIGTERM from the client ends the server at once. The end of stdin
        // sends the replies still queued first.
        server.handleTerminationSignals();
        using Reason = PhosphorMcp::McpServer::FinishReason;
        QObject::connect(&server, &PhosphorMcp::McpServer::finished, &app, [](Reason finish) {
            QCoreApplication::exit(finish == Reason::InputFailed || finish == Reason::OutputFailed ? 1 : 0);
        });
        if (!server.start()) {
            QCoreApplication::exit(1);
        }
    });
    bridge.prepare(socketPath);
    return app.exec();
}
