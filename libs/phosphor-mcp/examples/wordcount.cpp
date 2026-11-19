// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// The smallest useful MCP server: one tool, count_words, that counts the
// words and characters of a text. An MCP client starts it as a stdio server:
//
//     {"mcpServers": {"wordcount": {"command": "/path/to/phosphor-mcp-example"}}}
//
// It shows what an application gives the library. That is its name and
// version, the cache rules, a tool with its schemas and hints, and a handler
// that answers. The handler here answers at once. One that waits keeps the
// McpCall and answers later.

#include <PhosphorMcp/McpServer.h>

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QLatin1StringView>
#include <QtEnvironmentVariables>

using namespace PhosphorMcp;

int main(int argc, char** argv)
{
    // A client keeps the server's stderr as its log. Qt writes its messages
    // there only when the process has a terminal, which a server that a
    // client starts has not, and otherwise to the systemd journal when Qt is
    // built with it. So the example asks for stderr, before Qt writes
    // anything, unless the environment says otherwise.
    if (!qEnvironmentVariableIsSet("QT_FORCE_STDERR_LOGGING")) {
        qputenv("QT_FORCE_STDERR_LOGGING", QByteArrayView("1"));
    }
    QCoreApplication app(argc, argv);

    McpServer server;
    // The name a client shows. Without it the server takes the executable's
    // name. The version is the library's, which the build passes in.
    server.setServerInfo(QStringLiteral("wordcount"), QStringLiteral(PHOSPHOR_MCP_EXAMPLE_VERSION));
    server.setInstructions(QStringLiteral("count_words counts the words and characters of a text."));
    // The tool list never changes and is the same for every client.
    server.setCache(60 * 60 * 1000, McpServer::CacheScope::Public);

    McpTool tool(QStringLiteral("count_words"));
    tool.setTitle(QStringLiteral("Count words"));
    tool.setDescription(QStringLiteral(
        "Counts the words (runs of characters that are not spaces) and the characters (Unicode code points) of a "
        "text."));
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

    const bool added = server.addTool(tool, [](McpCall call) {
        // The arguments match the schema here, so "text" is there, and a
        // string.
        const QString text = call.arguments().value(QLatin1StringView("text")).toString();
        // A word is a run of characters that are not spaces, and every
        // Unicode space separates words (U+00A0 and U+3000 too). A character
        // is a code point, so a surrogate pair counts once. The server
        // refuses a string with an unpaired surrogate before the handler
        // runs, so each low surrogate here ends a pair.
        qsizetype words = 0;
        qsizetype characters = 0;
        bool inWord = false;
        for (const QChar c : text) {
            if (!c.isLowSurrogate()) {
                ++characters;
            }
            const bool space = c.isSpace();
            if (!space && !inWord) {
                ++words;
            }
            inWord = !space;
        }
        call.finish(McpResult::structured(QJsonObject{
            {QLatin1StringView("words"), words},
            {QLatin1StringView("characters"), characters},
        }));
    });
    if (!added) {
        // addTool() gave the reason in a warning.
        return 1;
    }

    // SIGTERM from the client ends the server at once, and the replies still
    // queued are dropped. The end of stdin sends them first.
    server.handleTerminationSignals();
    using Reason = McpServer::FinishReason;
    QObject::connect(&server, &McpServer::finished, &app, [&app](Reason reason) {
        // A failed read or write is an error. The other reasons are ends.
        app.exit(reason == Reason::InputFailed || reason == Reason::OutputFailed ? 1 : 0);
    });
    if (!server.start()) {
        return 1;
    }
    return app.exec();
}
