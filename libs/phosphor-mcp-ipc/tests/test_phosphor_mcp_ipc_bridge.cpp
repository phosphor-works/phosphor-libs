// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// McpIpcBridge end to end in one thread: an application's router with its
// targets on a socket in a temporary directory, the bridge as its client, and
// an McpServer on one end of a socket pair that the test drives as an MCP
// client.

#include "mcpipctesthelpers.h"
#include "mcptesthelpers.h"

#include <PhosphorMcp/McpServer.h>
#include <PhosphorMcp/McpTool.h>
#include <PhosphorMcpIpc/McpIpcBridge.h>
#include <PhosphorMcpIpc/McpIpcManifest.h>

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

using namespace PhosphorMcpIpc;
using namespace PhosphorMcpIpcTests;
using namespace PhosphorMcpTests;

namespace {

QJsonObject parse(const char* json)
{
    return QJsonDocument::fromJson(json).object();
}

McpIpcManifest manifestOf(const QByteArray& json)
{
    QString error;
    const std::optional<McpIpcManifest> manifest = McpIpcManifest::parse(json, &error);
    if (!manifest) {
        qWarning().noquote() << error;
        return {};
    }
    return *manifest;
}

// A bridge prepared against an application, and an MCP server with its
// tools on a socket pair.
struct Session
{
    explicit Session(const QByteArray& manifest = manifestJson())
        : bridge(manifestOf(manifest))
    {
    }

    // Starts the application and prepares the bridge. False on any failure.
    bool prepare()
    {
        if (!application.start()) {
            return false;
        }
        QSignalSpy prepared(&bridge, &McpIpcBridge::prepared);
        QSignalSpy failed(&bridge, &McpIpcBridge::failed);
        bridge.prepare(application.socketPath());
        if (!prepared.wait(5000)) {
            qWarning() << "the bridge failed:" << failed;
            return false;
        }
        return true;
    }

    // Adds the tools to the server and starts it on the socket pair.
    bool serve()
    {
        QString error;
        if (!peer.isValid() || !bridge.addTools(server, &error)) {
            qWarning().noquote() << error;
            return false;
        }
        return server.start(peer.serverFd(), peer.serverFd());
    }

    QJsonObject call(int id, const QString& tool, const QJsonObject& arguments)
    {
        peer.send(callRequest(id, tool, arguments));
        return peer.readOne();
    }

    Application application;
    McpIpcBridge bridge;
    PhosphorMcp::McpServer server;
    Peer peer;
};

QJsonObject structured(const QJsonObject& reply)
{
    return resultOf(reply).value(QLatin1StringView("structuredContent")).toObject();
}

bool isError(const QJsonObject& reply)
{
    return resultOf(reply).value(QLatin1StringView("isError")).toBool();
}

} // namespace

class TestPhosphorMcpIpcBridge : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void listShowsOnlyTheAllowedCalls()
    {
        Session session;
        QVERIFY(session.prepare());
        QVERIFY(session.serve());

        // The target and the function that the socket lacks left out, each
        // with a warning.
        const QStringList warnings = session.bridge.warnings();
        QCOMPARE(warnings,
                 (QStringList{QStringLiteral("the socket has no target \"ghost\" (NO_SUCH_TARGET: unknown target "
                                             "'ghost'), so the tools haunt are left out"),
                              QStringLiteral("tool \"nothing\": the target \"panes\" has no function \"nothing\", so "
                                             "the tool is left out")}));

        session.peer.send(request(1, QStringLiteral("tools/list")));
        const QJsonArray tools = resultOf(session.peer.readOne()).value(QLatin1StringView("tools")).toArray();
        QStringList names;
        QHash<QString, QJsonObject> byName;
        for (const QJsonValue tool : tools) {
            names.append(tool.toObject().value(QLatin1StringView("name")).toString());
            byName.insert(names.last(), tool.toObject());
        }
        QCOMPARE(names,
                 (QStringList{QStringLiteral("describe_pane"), QStringLiteral("rename_pane"),
                              QStringLiteral("split_pane"), QStringLiteral("close_pane"), QStringLiteral("pane_names"),
                              QStringLiteral("wait_pane"), QStringLiteral("run_script"), QStringLiteral("greet")}));

        QCOMPARE(byName.value(QStringLiteral("describe_pane")), parse(R"({
            "name": "describe_pane",
            "title": "Describe a pane",
            "description": "Describes a pane.",
            "inputSchema": {
                "type": "object",
                "properties": {"pane": {"type": "integer"}},
                "required": ["pane"],
                "additionalProperties": false
            },
            "annotations": {"readOnlyHint": true, "destructiveHint": false, "idempotentHint": true, "openWorldHint": false}
        })"));
        // No annotations in the manifest, none in the list.
        QCOMPARE(byName.value(QStringLiteral("rename_pane")), parse(R"({
            "name": "rename_pane",
            "description": "Renames a pane.",
            "inputSchema": {
                "type": "object",
                "properties": {"pane": {"type": "integer"}, "title": {"type": "string"}},
                "required": ["pane", "title"],
                "additionalProperties": false
            }
        })"));
        // The overload that "params" chose.
        QCOMPARE(byName.value(QStringLiteral("split_pane")).value(QLatin1StringView("inputSchema")).toObject(),
                 parse(R"({
            "type": "object",
            "properties": {"pane": {"type": "integer"}, "direction": {"type": "string"}},
            "required": ["pane", "direction"],
            "additionalProperties": false
        })"));
        // A function without parameters takes no arguments.
        QCOMPARE(byName.value(QStringLiteral("pane_names")).value(QLatin1StringView("inputSchema")).toObject(),
                 parse(R"({"type": "object", "additionalProperties": false})"));
        // An untyped QML parameter takes any value, a typed one its type.
        QCOMPARE(byName.value(QStringLiteral("run_script")).value(QLatin1StringView("inputSchema")).toObject(),
                 parse(R"({
            "type": "object",
            "properties": {"value": {}},
            "required": ["value"],
            "additionalProperties": false
        })"));
        QCOMPARE(byName.value(QStringLiteral("greet"))
                     .value(QLatin1StringView("inputSchema"))
                     .toObject()
                     .value(QLatin1StringView("properties"))
                     .toObject(),
                 parse(R"({"name": {"type": "string"}})"));
    }

    void argumentsReachTheMethodInOrder()
    {
        Session session;
        QVERIFY(session.prepare());
        QVERIFY(session.serve());

        // Named arguments, in another order than the parameters'.
        QJsonObject reply = session.call(
            1, QStringLiteral("rename_pane"),
            QJsonObject{{QLatin1StringView("title"), QStringLiteral("logs")}, {QLatin1StringView("pane"), 3}});
        QVERIFY(!isError(reply));
        QCOMPARE(firstText(reply), QStringLiteral("pane 3 is now logs"));

        reply = session.call(
            2, QStringLiteral("split_pane"),
            QJsonObject{{QLatin1StringView("direction"), QStringLiteral("right")}, {QLatin1StringView("pane"), 3}});
        QCOMPARE(firstText(reply), QStringLiteral("split 3 right"));

        // An object result is structured content.
        reply = session.call(3, QStringLiteral("describe_pane"), QJsonObject{{QLatin1StringView("pane"), 3}});
        QCOMPARE(structured(reply), parse(R"({"pane": 3, "title": "logs"})"));

        // Any other result is text: an array as its JSON.
        reply = session.call(4, QStringLiteral("pane_names"), QJsonObject());
        QCOMPARE(firstText(reply), QStringLiteral(R"(["a","b"])"));
        QVERIFY(!resultOf(reply).contains(QLatin1StringView("structuredContent")));

        // Any JSON reaches an untyped QML parameter as it is.
        reply = session.call(5, QStringLiteral("run_script"),
                             QJsonObject{{QLatin1StringView("value"), parse(R"({"x": [1, "two", null]})")}});
        QCOMPARE(structured(reply), parse(R"({"got": {"x": [1, "two", null]}})"));
        reply = session.call(6, QStringLiteral("run_script"), QJsonObject{{QLatin1StringView("value"), 7}});
        QCOMPARE(structured(reply), parse(R"({"got": 7})"));

        reply =
            session.call(7, QStringLiteral("greet"), QJsonObject{{QLatin1StringView("name"), QStringLiteral("you")}});
        QCOMPARE(firstText(reply), QStringLiteral("hi you"));
    }

    void refusedAndFailedCalls()
    {
        Session session;
        QVERIFY(session.prepare());
        QVERIFY(session.serve());

        // A call that the manifest does not list is no tool at all.
        QJsonObject reply = session.call(1, QStringLiteral("secret"), QJsonObject());
        QCOMPARE(errorCode(reply), int(PhosphorMcp::ErrorCode::InvalidParams));
        reply = session.call(2, QStringLiteral("focus"), QJsonObject{{QLatin1StringView("pane"), 1}});
        QCOMPARE(errorCode(reply), int(PhosphorMcp::ErrorCode::InvalidParams));
        QCOMPARE(session.application.panes.secretCalls, 0);

        // Arguments beyond the parameters, and of the wrong type, are tool
        // errors before anything reaches the socket.
        reply = session.call(3, QStringLiteral("rename_pane"),
                             QJsonObject{{QLatin1StringView("pane"), 1},
                                         {QLatin1StringView("title"), QStringLiteral("x")},
                                         {QLatin1StringView("fn"), QStringLiteral("secret")}});
        QVERIFY(isError(reply));
        QVERIFY(firstText(reply).contains(QLatin1StringView("arguments/fn: is not a known property")));
        reply = session.call(4, QStringLiteral("describe_pane"),
                             QJsonObject{{QLatin1StringView("pane"), QStringLiteral("one")}});
        QVERIFY(isError(reply));

        // A call that the method fails is a tool error with its code.
        reply = session.call(5, QStringLiteral("close_pane"), QJsonObject{{QLatin1StringView("pane"), 2}});
        QVERIFY(isError(reply));
        QCOMPARE(firstText(reply), QStringLiteral("CALL_FAILED: no pane 2"));
        reply = session.call(6, QStringLiteral("close_pane"), QJsonObject{{QLatin1StringView("pane"), 1}});
        QVERIFY(!isError(reply));
        QCOMPARE(firstText(reply), QStringLiteral("1"));
    }

    void socketGoingAwayGivesToolErrors()
    {
        Session session;
        QVERIFY(session.prepare());
        QVERIFY(session.serve());

        // A call that waits when the socket goes.
        session.peer.send(callRequest(1, QStringLiteral("wait_pane"), QJsonObject{{QLatin1StringView("pane"), 1}}));
        QVERIFY(spinUntil([&] {
            return session.application.panes.waiting.size() == 1;
        }));
        session.application.router().stop();
        QJsonObject reply = session.peer.readOne();
        QCOMPARE(reply.value(QLatin1StringView("id")), QJsonValue(1));
        QVERIFY(isError(reply));
        QVERIFY(firstText(reply).startsWith(QLatin1StringView("CONNECTION_CLOSED: ")));

        // No socket at all: the next call tries it again, and fails.
        reply = session.call(2, QStringLiteral("describe_pane"), QJsonObject{{QLatin1StringView("pane"), 1}});
        QVERIFY(isError(reply));
        QVERIFY(firstText(reply).startsWith(QLatin1StringView("NOT_CONNECTED: ")));

        // The application is back, and so are the calls.
        QVERIFY(session.application.router().start(session.application.socketPath()));
        reply =
            session.call(3, QStringLiteral("greet"), QJsonObject{{QLatin1StringView("name"), QStringLiteral("again")}});
        QVERIFY(!isError(reply));
        QCOMPARE(firstText(reply), QStringLiteral("hi again"));
    }

    void noManifestGivesNoTools()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QString error;
        bool missing = false;
        const std::optional<McpIpcManifest> manifest =
            McpIpcManifest::load(QDir(dir.path()).filePath(QStringLiteral("none.json")), &error, &missing);
        QVERIFY(manifest);
        QVERIFY(missing);
        QVERIFY(manifest->tools.isEmpty());

        // No tools, so nothing connects: the socket need not exist.
        McpIpcBridge bridge(*manifest);
        QSignalSpy prepared(&bridge, &McpIpcBridge::prepared);
        bridge.prepare(QDir(dir.path()).filePath(QStringLiteral("none.sock")));
        QVERIFY(prepared.wait(2000));
        QVERIFY(bridge.tools().isEmpty());

        PhosphorMcp::McpServer server;
        Peer peer;
        QVERIFY(bridge.addTools(server));
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("no tool")));
        QVERIFY(server.start(peer.serverFd(), peer.serverFd()));
        peer.send(request(1, QStringLiteral("tools/list")));
        QCOMPARE(resultOf(peer.readOne()).value(QLatin1StringView("tools")).toArray(), QJsonArray());

        // An empty list of tools is the same.
        McpIpcBridge empty(manifestOf(QByteArrayLiteral(R"({"tools": []})")));
        QSignalSpy emptyPrepared(&empty, &McpIpcBridge::prepared);
        empty.prepare(QDir(dir.path()).filePath(QStringLiteral("none.sock")));
        QVERIFY(emptyPrepared.wait(2000));
        QVERIFY(empty.tools().isEmpty());
    }

    void unchoosableOverloadsFailAtStart_data()
    {
        QTest::addColumn<QByteArray>("tool");
        QTest::addColumn<QString>("message");
        QTest::newRow("overload without params")
            << QByteArray(R"({"call": "panes.split", "name": "split", "description": "d"})")
            << QStringLiteral(
                   "tool \"split\": panes.split has 2 overloads, (pane), (pane, direction), so \"params\" "
                   "must name the parameters of one");
        QTest::newRow("params that match no overload")
            << QByteArray(
                   R"({"call": "panes.split", "name": "split", "description": "d", "params": ["direction", "pane"]})")
            << QStringLiteral(
                   "tool \"split\": panes.split has no overload with the parameters (direction, pane). It "
                   "has (pane), (pane, direction)");
        QTest::newRow("params of the only overload, wrong")
            << QByteArray(R"({"call": "panes.rename", "name": "rename", "description": "d", "params": ["pane"]})")
            << QStringLiteral(
                   "tool \"rename\": panes.rename has no overload with the parameters (pane). It has "
                   "(pane, title)");
        QTest::newRow("overloads with as many parameters")
            << QByteArray(R"({"call": "panes.focus", "name": "focus", "description": "d", "params": ["title"]})")
            << QStringLiteral(
                   "tool \"focus\": panes.focus(title) has as many parameters as the overload "
                   "panes.focus(pane), and the socket chooses an overload by its number of arguments alone");
    }

    void unchoosableOverloadsFailAtStart()
    {
        QFETCH(QByteArray, tool);
        QFETCH(QString, message);
        Session session(QByteArrayLiteral(R"({"tools": [)") + tool + QByteArrayLiteral("]}"));
        QVERIFY(session.application.start());
        QSignalSpy prepared(&session.bridge, &McpIpcBridge::prepared);
        QSignalSpy failed(&session.bridge, &McpIpcBridge::failed);
        session.bridge.prepare(session.application.socketPath());
        QVERIFY(failed.wait(5000));
        QCOMPARE(failed.at(0).at(0).toString(), message);
        QCOMPARE(prepared.size(), 0);
        QVERIFY(session.bridge.tools().isEmpty());
    }

    void unreachableSocketFails()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        McpIpcBridge bridge(manifestOf(manifestJson()));
        QSignalSpy failed(&bridge, &McpIpcBridge::failed);
        const QString path = QDir(dir.path()).filePath(QStringLiteral("none.sock"));
        bridge.prepare(path);
        QVERIFY(failed.wait(5000));
        QVERIFY(failed.at(0).at(0).toString().startsWith(
            QStringLiteral("cannot reach the application's socket at %1: ").arg(path)));
        QTest::qWait(50);
        QCOMPARE(failed.size(), 1);
    }
};

QTEST_GUILESS_MAIN(TestPhosphorMcpIpcBridge)
#include "test_phosphor_mcp_ipc_bridge.moc"
