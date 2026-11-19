// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// The smallest useful MCP server: one tool, count_words, that counts the
// words and characters of a text. An MCP client starts it as a stdio server:
//
//     {"mcpServers": {"wordcount": {"command": "/path/to/phosphor-mcp-example"}}}
//
// It shows what an application gives the library: its name, the cache rules,
// a tool with its schemas and hints, and a handler that answers. The handler
// here answers at once; one that waits keeps the McpCall and answers later.

#include <PhosphorMcp/McpServer.h>

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QLatin1StringView>
#include <QRegularExpression>

using namespace PhosphorMcp;

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("phosphor-mcp-example"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    McpServer server;
    server.setInstructions(QStringLiteral("count_words counts the words and characters of a text."));
    // The tool list never changes and is the same for every client.
    server.setCache(3600 * 1000, McpServer::CacheScope::Public);

    McpTool tool(QStringLiteral("count_words"));
    tool.setTitle(QStringLiteral("Count words"));
    tool.setDescription(
        QStringLiteral("Counts the words (runs of non-space characters) and the characters of a text."));
    tool.setInputSchema(QJsonObject{
        {QLatin1StringView("type"), QStringLiteral("object")},
        {QLatin1StringView("properties"),
         QJsonObject{
             {QLatin1StringView("text"),
              QJsonObject{
                  {QLatin1StringView("type"), QStringLiteral("string")},
                  {QLatin1StringView("description"), QStringLiteral("The text to count")},
                  {QLatin1StringView("maxLength"), 1000000},
              }},
         }},
        {QLatin1StringView("required"), QJsonArray{QStringLiteral("text")}},
        {QLatin1StringView("additionalProperties"), false},
    });
    tool.setOutputSchema(QJsonObject{
        {QLatin1StringView("type"), QStringLiteral("object")},
        {QLatin1StringView("properties"),
         QJsonObject{
             {QLatin1StringView("words"), QJsonObject{{QLatin1StringView("type"), QStringLiteral("integer")}}},
             {QLatin1StringView("characters"), QJsonObject{{QLatin1StringView("type"), QStringLiteral("integer")}}},
         }},
        {QLatin1StringView("required"), QJsonArray{QStringLiteral("words"), QStringLiteral("characters")}},
    });
    tool.setHints({.readOnly = true, .destructive = false, .idempotent = true, .openWorld = false});

    server.addTool(tool, [](McpCall call) {
        // The schema has been checked: "text" is there, and a string.
        const QString text = call.arguments().value(QLatin1StringView("text")).toString();
        static const QRegularExpression space(QStringLiteral("\\s+"));
        const qsizetype words = text.split(space, Qt::SkipEmptyParts).size();
        call.finish(McpResult::structured(QJsonObject{
            {QLatin1StringView("words"), words},
            {QLatin1StringView("characters"), text.size()},
        }));
    });

    // SIGTERM from the client ends the server as the end of stdin does.
    server.handleTerminationSignals();
    QObject::connect(&server, &McpServer::finished, &app, [&app](McpServer::FinishReason reason) {
        app.exit(reason == McpServer::FinishReason::InputFailed ? 1 : 0);
    });
    if (!server.start()) {
        return 1;
    }
    return app.exec();
}
