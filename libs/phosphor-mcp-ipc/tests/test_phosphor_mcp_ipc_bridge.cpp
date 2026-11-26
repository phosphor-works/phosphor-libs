// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: LGPL-2.1-or-later

// McpIpcBridge end to end in one thread. An application's router has its
// targets on a socket in a temporary directory, and the bridge is its
// client. An McpServer has one end of a socket pair, and the test drives the
// other end as an MCP client. What a router never sends comes from a
// RawServer in its place.

#include "mcpipctesthelpers.h"
#include "mcptesthelpers.h"

#include <PhosphorIpc/IpcProtocol.h>
#include <PhosphorMcp/McpServer.h>
#include <PhosphorMcp/McpTool.h>
#include <PhosphorMcpIpc/McpIpcBridge.h>
#include <PhosphorMcpIpc/McpIpcManifest.h>

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QPointer>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>

#include <memory>
#include <optional>

using namespace PhosphorMcpIpc;
using namespace PhosphorMcpIpcTests;
using namespace PhosphorMcpTests;

namespace {

QJsonObject structured(const QJsonObject& reply)
{
    return resultOf(reply).value(QLatin1StringView("structuredContent")).toObject();
}

bool isError(const QJsonObject& reply)
{
    return resultOf(reply).value(QLatin1StringView("isError")).toBool();
}

// An MCP server on one end of a socket pair, and the test as its client.
struct Served
{
    // The peer first: it owns the descriptors that the server reads.
    Peer peer;
    PhosphorMcp::McpServer server;
    QString problem;

    // Adds the tools of `bridge` to the server and starts it.
    bool serve(McpIpcBridge& bridge)
    {
        if (!peer.isValid()) {
            problem = QStringLiteral("no socket pair");
            return false;
        }
        if (!bridge.addTools(server, &problem)) {
            return false;
        }
        if (!server.start(peer.serverFd(), peer.serverFd())) {
            problem = QStringLiteral("the MCP server did not start");
            return false;
        }
        return true;
    }

    // Makes a tool call and reads its answer. False when the request
    // cannot be sent, or when the next message is not the answer to it.
    bool call(int id, const QString& tool, const QJsonObject& arguments, QJsonObject* reply)
    {
        if (!peer.send(callRequest(id, tool, arguments))) {
            problem = QStringLiteral("the request was not sent");
            return false;
        }
        return read(id, reply);
    }

    // Waits for prepared() or failed(). False for failed(), with its
    // reason in `problem`, and when neither comes within 5 s.
    bool waitForPrepared(const QSignalSpy& prepared, const QSignalSpy& failed)
    {
        spinUntil([&] {
            return !prepared.isEmpty() || !failed.isEmpty();
        });
        if (!prepared.isEmpty()) {
            return true;
        }
        problem = failed.isEmpty() ? QStringLiteral("neither prepared() nor failed() within 5 s")
                                   : failed.first().first().toString();
        return false;
    }

    bool read(int id, QJsonObject* reply)
    {
        *reply = peer.readOne();
        if (reply->value(QLatin1StringView("id")) != QJsonValue(id)) {
            problem =
                QStringLiteral("no answer to request %1, but: %2")
                    .arg(QString::number(id), QString::fromUtf8(QJsonDocument(*reply).toJson(QJsonDocument::Compact)));
            return false;
        }
        return true;
    }
};

// A bridge prepared against an application, with an MCP server.
struct Session : Served
{
    explicit Session(const QByteArray& json = manifestJson())
        : manifest(McpIpcManifest::parse(json, &problem))
        , bridge(manifest.value_or(McpIpcManifest()))
    {
    }

    // False for a manifest that the test wrote wrong.
    bool valid() const
    {
        return manifest.has_value();
    }

    // Starts the application and prepares the bridge. False on any failure.
    bool prepare()
    {
        if (!valid()) {
            return false;
        }
        if (!application.start()) {
            problem = QStringLiteral("the application did not start");
            return false;
        }
        QSignalSpy prepared(&bridge, &McpIpcBridge::prepared);
        QSignalSpy failed(&bridge, &McpIpcBridge::failed);
        bridge.prepare(application.socketPath());
        return waitForPrepared(prepared, failed);
    }

    bool serve()
    {
        return Served::serve(bridge);
    }

    std::optional<McpIpcManifest> manifest;
    Application application;
    McpIpcBridge bridge;
};

// A bridge whose socket is a RawServer that the test answers by hand.
struct RawSession : Served
{
    PhosphorIpcTests::RawServer raw;
    std::unique_ptr<McpIpcBridge> bridge;
    // On the bridge from before prepare(), so that no outcome is lost.
    std::unique_ptr<QSignalSpy> prepared;
    std::unique_ptr<QSignalSpy> failed;
    // False for a test that only needs the connection: connect() then
    // does not wait for the schema request.
    bool readsTheRequest = true;

    // One tool for each name, each a call of target "t".
    static McpIpcManifest manifestOf(const QStringList& methods)
    {
        McpIpcManifest manifest;
        for (const QString& method : methods) {
            McpIpcManifestTool tool;
            tool.target = QStringLiteral("t");
            tool.method = method;
            tool.name = method;
            tool.description = QStringLiteral("d");
            manifest.tools.append(tool);
        }
        return manifest;
    }

    // Makes the bridge, connects it, and reads its schema request. The
    // id of the request goes to `schemaId`. The test then answers it.
    bool connect(const McpIpcManifest& manifest, qint64* schemaId,
                 int timeoutMs = McpIpcBridge::DefaultPrepareTimeoutMs)
    {
        if (!raw.listen()) {
            problem = QStringLiteral("the raw server does not listen");
            return false;
        }
        bridge = std::make_unique<McpIpcBridge>(manifest);
        prepared = std::make_unique<QSignalSpy>(bridge.get(), &McpIpcBridge::prepared);
        failed = std::make_unique<QSignalSpy>(bridge.get(), &McpIpcBridge::failed);
        bridge->prepare(raw.path(), timeoutMs);
        if (!raw.accept()) {
            problem = QStringLiteral("the bridge did not connect");
            return false;
        }
        if (!readsTheRequest) {
            return true;
        }
        const std::optional<QJsonObject> request = raw.readRequest();
        if (!request || request->value(QLatin1StringView("type")).toString() != QLatin1StringView("schema")) {
            problem = QStringLiteral("no schema request");
            return false;
        }
        *schemaId = request->value(QLatin1StringView("id")).toInteger();
        return true;
    }

    // connect(), then the schema of target "t" with `functions`, then the
    // wait for prepared().
    bool prepare(const McpIpcManifest& manifest, const QJsonArray& functions)
    {
        qint64 schemaId = 0;
        if (!connect(manifest, &schemaId)) {
            return false;
        }
        if (!raw.reply(schemaId,
                       QJsonObject{{QStringLiteral("target"), QStringLiteral("t")},
                                   {QStringLiteral("functions"), functions},
                                   {QStringLiteral("signals"), QJsonArray()}})) {
            problem = QStringLiteral("the schema was not sent");
            return false;
        }
        return waitForPrepared(*prepared, *failed);
    }

    // Sends a tool call, and reads the call that the bridge makes of it.
    // Its id goes to `callId`.
    bool callReachesTheSocket(int id, const QString& tool, qint64* callId)
    {
        if (!peer.send(callRequest(id, tool, QJsonObject()))) {
            problem = QStringLiteral("the request was not sent");
            return false;
        }
        const std::optional<QJsonObject> request = raw.readRequest();
        if (!request || request->value(QLatin1StringView("fn")).toString() != tool) {
            problem = QStringLiteral("the call did not reach the socket");
            return false;
        }
        *callId = request->value(QLatin1StringView("id")).toInteger();
        return true;
    }
};

// A method of the schema, with the parameters `params`.
QJsonObject method(const QString& name, const QJsonArray& params = {})
{
    return QJsonObject{{QStringLiteral("name"), name}, {QStringLiteral("params"), params}};
}

} // namespace

class TestPhosphorMcpIpcBridge : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void listShowsOnlyTheAllowedCalls()
    {
        Session session;
        QVERIFY2(session.prepare(), qPrintable(session.problem));
        QVERIFY2(session.serve(), qPrintable(session.problem));

        // The list leaves out the target and the method that the socket
        // lacks. Each gives a line.
        const QStringList warnings = session.bridge.warnings();
        QCOMPARE(warnings,
                 (QStringList{QStringLiteral("the socket has no target \"ghost\" (NO_SUCH_TARGET: unknown target "
                                             "'ghost'), so the tool haunt is left out"),
                              QStringLiteral("tool \"missing\": the target \"panes\" has no method \"missing\", so "
                                             "the tool is left out")}));

        QVERIFY(session.peer.send(request(1, QStringLiteral("tools/list"))));
        QJsonObject reply;
        QVERIFY2(session.read(1, &reply), qPrintable(session.problem));
        const QJsonArray tools = resultOf(reply).value(QLatin1StringView("tools")).toArray();
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
        // tools() has the same tools.
        QStringList built;
        const QList<PhosphorMcp::McpTool> bridgeTools = session.bridge.tools();
        for (const PhosphorMcp::McpTool& tool : bridgeTools) {
            built.append(tool.name());
        }
        QCOMPARE(built, names);

        QCOMPARE(byName.value(QStringLiteral("describe_pane")), parseObject(R"({
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
        QCOMPARE(byName.value(QStringLiteral("rename_pane")), parseObject(R"({
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
                 parseObject(R"({
            "type": "object",
            "properties": {"pane": {"type": "integer"}, "direction": {"type": "string"}},
            "required": ["pane", "direction"],
            "additionalProperties": false
        })"));
        // A method without parameters takes no arguments.
        QCOMPARE(byName.value(QStringLiteral("pane_names")).value(QLatin1StringView("inputSchema")).toObject(),
                 parseObject(R"({"type": "object", "additionalProperties": false})"));
        // An untyped QML parameter takes any value, a typed one its type.
        QCOMPARE(byName.value(QStringLiteral("run_script")).value(QLatin1StringView("inputSchema")).toObject(),
                 parseObject(R"({
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
                 parseObject(R"({"name": {"type": "string"}})"));
    }

    void argumentsReachTheMethodInOrder()
    {
        Session session;
        QVERIFY2(session.prepare(), qPrintable(session.problem));
        QVERIFY2(session.serve(), qPrintable(session.problem));

        QJsonObject reply;
        QVERIFY2(session.call(
                     1, QStringLiteral("rename_pane"),
                     QJsonObject{{QLatin1StringView("title"), QStringLiteral("logs")}, {QLatin1StringView("pane"), 3}},
                     &reply),
                 qPrintable(session.problem));
        QVERIFY(!isError(reply));
        QCOMPARE(firstText(reply), QStringLiteral("pane 3 is now logs"));

        // A JSON object has its members in the order of their names. Here
        // that is another order than the parameters': "direction" comes
        // before "pane". A bridge that took the arguments as they come
        // would pass them the wrong way round.
        QVERIFY2(session.call(2, QStringLiteral("split_pane"),
                              QJsonObject{{QLatin1StringView("direction"), QStringLiteral("right")},
                                          {QLatin1StringView("pane"), 3}},
                              &reply),
                 qPrintable(session.problem));
        QCOMPARE(firstText(reply), QStringLiteral("split 3 right"));

        // An object result is structured content.
        QVERIFY2(session.call(3, QStringLiteral("describe_pane"), QJsonObject{{QLatin1StringView("pane"), 3}}, &reply),
                 qPrintable(session.problem));
        QCOMPARE(structured(reply), parseObject(R"({"pane": 3, "title": "logs"})"));

        // Any other result is text: an array as its JSON.
        QVERIFY2(session.call(4, QStringLiteral("pane_names"), QJsonObject(), &reply), qPrintable(session.problem));
        QCOMPARE(firstText(reply), QStringLiteral(R"(["a","b"])"));
        QVERIFY(!resultOf(reply).contains(QLatin1StringView("structuredContent")));

        // Any JSON reaches an untyped QML parameter as it is.
        const QJsonObject nested = parseObject(R"({"x": [1, "two", null]})");
        QVERIFY2(
            session.call(5, QStringLiteral("run_script"), QJsonObject{{QLatin1StringView("value"), nested}}, &reply),
            qPrintable(session.problem));
        QCOMPARE(structured(reply), QJsonObject({{QLatin1StringView("got"), nested}}));
        QVERIFY2(session.call(6, QStringLiteral("run_script"), QJsonObject{{QLatin1StringView("value"), 7}}, &reply),
                 qPrintable(session.problem));
        QCOMPARE(structured(reply), parseObject(R"({"got": 7})"));

        QVERIFY2(session.call(7, QStringLiteral("greet"),
                              QJsonObject{{QLatin1StringView("name"), QStringLiteral("you")}}, &reply),
                 qPrintable(session.problem));
        QCOMPARE(firstText(reply), QStringLiteral("hi you"));
    }

    void parameterKindsAndResults()
    {
        // One parameter of each kind that the schema of a socket describes,
        // and the results that are neither an object nor a string.
        Session session(QByteArrayLiteral(R"({"tools": [
            {"call": "panes.kinds", "name": "kinds", "description": "d"},
            {"call": "panes.nothing", "name": "nothing", "description": "d"},
            {"call": "panes.install", "name": "install", "description": "d"}
        ]})"));
        QVERIFY2(session.prepare(), qPrintable(session.problem));
        QVERIFY2(session.serve(), qPrintable(session.problem));
        QCOMPARE(session.bridge.warnings(), QStringList());

        QVERIFY(session.peer.send(request(1, QStringLiteral("tools/list"))));
        QJsonObject reply;
        QVERIFY2(session.read(1, &reply), qPrintable(session.problem));
        const QJsonArray tools = resultOf(reply).value(QLatin1StringView("tools")).toArray();
        QCOMPARE(tools.size(), 3);
        QCOMPARE(tools.at(0).toObject().value(QLatin1StringView("inputSchema")).toObject(), parseObject(R"({
            "type": "object",
            "properties": {
                "flag": {"type": "boolean"},
                "ratio": {"type": "number"},
                "tags": {"type": "array", "items": {"type": "string"}},
                "items": {"type": "array"},
                "options": {"type": "object"},
                "link": {"description": "custom QMetaType: QUrl"}
            },
            "required": ["flag", "ratio", "tags", "items", "options", "link"],
            "additionalProperties": false
        })"));

        // A boolean result is its JSON as text.
        // Not a raw string: moc reads the two slashes of a URL in one as
        // the start of a comment, and then finds no class in this file.
        const QJsonObject arguments{
            {QLatin1StringView("flag"), true},
            {QLatin1StringView("ratio"), 0.5},
            {QLatin1StringView("tags"), QJsonArray{QStringLiteral("a"), QStringLiteral("b")}},
            {QLatin1StringView("items"), QJsonArray{1, QStringLiteral("x")}},
            {QLatin1StringView("options"), QJsonObject{{QLatin1StringView("k"), 1}}},
            {QLatin1StringView("link"), QStringLiteral("https://example.org/x")},
        };
        QVERIFY2(session.call(2, QStringLiteral("kinds"), arguments, &reply), qPrintable(session.problem));
        QVERIFY2(!isError(reply), qPrintable(firstText(reply)));
        QCOMPARE(firstText(reply), QStringLiteral("true"));

        // The schema of the QUrl parameter has no type, so the MCP server
        // takes an object for it. The router has no conversion of an
        // object to a QUrl, and refuses the call.
        QJsonObject wrong = arguments;
        wrong.insert(QLatin1StringView("link"), QJsonObject{{QLatin1StringView("host"), QStringLiteral("x")}});
        QVERIFY2(session.call(20, QStringLiteral("kinds"), wrong, &reply), qPrintable(session.problem));
        QVERIFY(isError(reply));
        QVERIFY2(firstText(reply).startsWith(QLatin1StringView("INVALID_ARG: ")), qPrintable(firstText(reply)));

        // A method that returns nothing answers null.
        QVERIFY2(session.call(3, QStringLiteral("nothing"), QJsonObject(), &reply), qPrintable(session.problem));
        QVERIFY(!isError(reply));
        QCOMPARE(firstText(reply), QStringLiteral("null"));

        // An event that the target sends to the caller is dropped. The
        // result still comes.
        QVERIFY2(session.call(4, QStringLiteral("install"),
                              QJsonObject{{QLatin1StringView("name"), QStringLiteral("foo")}}, &reply),
                 qPrintable(session.problem));
        QCOMPARE(firstText(reply), QStringLiteral("installed foo"));
    }

    void paramsChooseTheOverload()
    {
        // A default argument makes two overloads. An empty list names the
        // overload without parameters.
        Session session(QByteArrayLiteral(R"({"tools": [
            {"call": "panes.zoom", "name": "zoom", "description": "d", "params": ["pane", "factor"]},
            {"call": "panes.zoom", "name": "zoom_default", "description": "d", "params": ["pane"]},
            {"call": "panes.layout", "name": "layout", "description": "d", "params": []},
            {"call": "panes.layout", "name": "layout_named", "description": "d", "params": ["name"]}
        ]})"));
        QVERIFY2(session.prepare(), qPrintable(session.problem));
        QVERIFY2(session.serve(), qPrintable(session.problem));

        QJsonObject reply;
        QVERIFY2(session.call(1, QStringLiteral("zoom"),
                              QJsonObject{{QLatin1StringView("pane"), 1}, {QLatin1StringView("factor"), 4}}, &reply),
                 qPrintable(session.problem));
        QCOMPARE(firstText(reply), QStringLiteral("zoom 1 by 4"));
        QVERIFY2(session.call(2, QStringLiteral("zoom_default"), QJsonObject{{QLatin1StringView("pane"), 1}}, &reply),
                 qPrintable(session.problem));
        QCOMPARE(firstText(reply), QStringLiteral("zoom 1 by 2"));
        QVERIFY2(session.call(3, QStringLiteral("layout"), QJsonObject(), &reply), qPrintable(session.problem));
        QCOMPARE(firstText(reply), QStringLiteral("the layout"));
        QVERIFY2(session.call(4, QStringLiteral("layout_named"),
                              QJsonObject{{QLatin1StringView("name"), QStringLiteral("tall")}}, &reply),
                 qPrintable(session.problem));
        QCOMPARE(firstText(reply), QStringLiteral("the layout tall"));
    }

    void refusedAndFailedCalls()
    {
        Session session;
        QVERIFY2(session.prepare(), qPrintable(session.problem));
        QVERIFY2(session.serve(), qPrintable(session.problem));

        // A call that the manifest does not list is no tool at all.
        QJsonObject reply;
        QVERIFY2(session.call(1, QStringLiteral("secret"), QJsonObject(), &reply), qPrintable(session.problem));
        QCOMPARE(errorCode(reply), int(PhosphorMcp::ErrorCode::InvalidParams));
        QVERIFY2(session.call(2, QStringLiteral("focus"), QJsonObject{{QLatin1StringView("pane"), 1}}, &reply),
                 qPrintable(session.problem));
        QCOMPARE(errorCode(reply), int(PhosphorMcp::ErrorCode::InvalidParams));

        // The MCP server checks the arguments against the schema. An
        // argument beyond the parameters, one of the wrong type and a
        // missing one are tool errors of that check, and nothing reaches
        // the socket: the router would answer with INVALID_ARG.
        QVERIFY2(session.call(3, QStringLiteral("rename_pane"),
                              QJsonObject{{QLatin1StringView("pane"), 1},
                                          {QLatin1StringView("title"), QStringLiteral("x")},
                                          {QLatin1StringView("fn"), QStringLiteral("secret")}},
                              &reply),
                 qPrintable(session.problem));
        QVERIFY(isError(reply));
        QVERIFY2(firstText(reply).contains(QLatin1StringView("arguments/fn: is not a known property")),
                 qPrintable(firstText(reply)));
        QVERIFY2(session.call(4, QStringLiteral("describe_pane"),
                              QJsonObject{{QLatin1StringView("pane"), QStringLiteral("one")}}, &reply),
                 qPrintable(session.problem));
        QVERIFY(isError(reply));
        QVERIFY2(firstText(reply).contains(QLatin1StringView("arguments/pane")), qPrintable(firstText(reply)));
        QVERIFY2(!firstText(reply).contains(QLatin1StringView("INVALID_ARG")), qPrintable(firstText(reply)));
        QVERIFY2(session.call(5, QStringLiteral("rename_pane"), QJsonObject{{QLatin1StringView("pane"), 1}}, &reply),
                 qPrintable(session.problem));
        QVERIFY(isError(reply));
        QVERIFY2(firstText(reply).contains(QLatin1StringView("title")), qPrintable(firstText(reply)));
        QVERIFY2(!firstText(reply).contains(QLatin1StringView("INVALID_ARG")), qPrintable(firstText(reply)));

        // A call that the method fails is a tool error with its code.
        QVERIFY2(session.call(6, QStringLiteral("close_pane"), QJsonObject{{QLatin1StringView("pane"), 2}}, &reply),
                 qPrintable(session.problem));
        QVERIFY(isError(reply));
        QCOMPARE(firstText(reply), QStringLiteral("CALL_FAILED: no pane 2"));
        QVERIFY2(session.call(7, QStringLiteral("close_pane"), QJsonObject{{QLatin1StringView("pane"), 1}}, &reply),
                 qPrintable(session.problem));
        QVERIFY(!isError(reply));
        QCOMPARE(firstText(reply), QStringLiteral("1"));

        // A call that is larger than the socket takes. The MCP server takes
        // 16 MiB for a line, and the socket 1 MiB. The client refuses the
        // call before it sends it.
        QVERIFY2(session.call(8, QStringLiteral("greet"),
                              QJsonObject{{QLatin1StringView("name"), QString(1100 * 1024, QLatin1Char('x'))}}, &reply),
                 qPrintable(session.problem));
        QVERIFY(isError(reply));
        QCOMPARE(firstText(reply),
                 QStringLiteral("REQUEST_TOO_LARGE: the arguments are too large. The application's socket takes "
                                "1048576 bytes for one call. Send less."));

        // Nothing above reached the method that no tool offers, the
        // argument named "fn" included.
        QCOMPARE(session.application.panes().secretCalls(), 0);
    }

    void deferredCallAnswersLater()
    {
        // A method that answers later holds its tool call open. The server
        // serves other calls meanwhile.
        Session session;
        QVERIFY2(session.prepare(), qPrintable(session.problem));
        QVERIFY2(session.serve(), qPrintable(session.problem));

        QVERIFY(session.peer.send(
            callRequest(1, QStringLiteral("wait_pane"), QJsonObject{{QLatin1StringView("pane"), 1}})));
        QVERIFY(spinUntil([&] {
            return session.application.panes().waiting().size() == 1;
        }));
        QJsonObject reply;
        QVERIFY2(session.call(2, QStringLiteral("greet"),
                              QJsonObject{{QLatin1StringView("name"), QStringLiteral("you")}}, &reply),
                 qPrintable(session.problem));
        QCOMPARE(firstText(reply), QStringLiteral("hi you"));

        QVERIFY(session.application.panes().waiting().front().finish(QJsonValue(QStringLiteral("done"))));
        QVERIFY2(session.read(1, &reply), qPrintable(session.problem));
        QVERIFY(!isError(reply));
        QCOMPARE(firstText(reply), QStringLiteral("done"));
    }

    void cancelledCallGetsNoAnswer()
    {
        // An MCP cancel does not reach the method. The answer that the
        // method gives later is dropped.
        Session session;
        QVERIFY2(session.prepare(), qPrintable(session.problem));
        QVERIFY2(session.serve(), qPrintable(session.problem));

        QVERIFY(session.peer.send(
            callRequest(1, QStringLiteral("wait_pane"), QJsonObject{{QLatin1StringView("pane"), 1}})));
        QVERIFY(spinUntil([&] {
            return session.application.panes().waiting().size() == 1;
        }));
        QVERIFY(session.peer.send(cancelNotification(1)));
        // The answer to a later call shows that the server read the cancel.
        QJsonObject reply;
        QVERIFY2(session.call(2, QStringLiteral("greet"),
                              QJsonObject{{QLatin1StringView("name"), QStringLiteral("you")}}, &reply),
                 qPrintable(session.problem));

        QVERIFY(session.application.panes().waiting().front().finish(QJsonValue(QStringLiteral("late"))));
        QVERIFY2(session.call(3, QStringLiteral("greet"),
                              QJsonObject{{QLatin1StringView("name"), QStringLiteral("again")}}, &reply),
                 qPrintable(session.problem));
        QCOMPARE(firstText(reply), QStringLiteral("hi again"));
        QCOMPARE(session.peer.readFor(100).size(), 0);
    }

    void socketGoingAwayGivesToolErrors()
    {
        Session session;
        QVERIFY2(session.prepare(), qPrintable(session.problem));
        QVERIFY2(session.serve(), qPrintable(session.problem));

        // A call that waits when the socket goes.
        QVERIFY(session.peer.send(
            callRequest(1, QStringLiteral("wait_pane"), QJsonObject{{QLatin1StringView("pane"), 1}})));
        QVERIFY(spinUntil([&] {
            return session.application.panes().waiting().size() == 1;
        }));
        session.application.router().stop();
        QJsonObject reply;
        QVERIFY2(session.read(1, &reply), qPrintable(session.problem));
        QVERIFY(isError(reply));
        // The words are for the model, which knows no "server" but this
        // one: the application, that the call may have run, what comes next.
        QCOMPARE(firstText(reply),
                 QStringLiteral("CONNECTION_CLOSED: the connection to the application ended before the answer came "
                                "(the server closed the connection). The call may have run all the same. The next "
                                "call connects again."));

        // No socket at all: the next call tries it again, and fails with
        // the reason of that attempt.
        QVERIFY2(session.call(2, QStringLiteral("describe_pane"), QJsonObject{{QLatin1StringView("pane"), 1}}, &reply),
                 qPrintable(session.problem));
        QVERIFY(isError(reply));
        const QString expected = QStringLiteral(
                                     "NOT_CONNECTED: the application's socket is not reachable (cannot "
                                     "connect to %1: ")
                                     .arg(session.application.socketPath());
        QVERIFY2(firstText(reply).startsWith(expected), qPrintable(firstText(reply)));
        QVERIFY2(firstText(reply).endsWith(QLatin1StringView("). The next call tries again.")),
                 qPrintable(firstText(reply)));

        // The application is back, and so are the calls.
        QVERIFY(session.application.router().start(session.application.socketPath()));
        QVERIFY2(session.call(3, QStringLiteral("greet"),
                              QJsonObject{{QLatin1StringView("name"), QStringLiteral("again")}}, &reply),
                 qPrintable(session.problem));
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
        QVERIFY2(manifest.has_value(), qPrintable(error));
        QVERIFY(missing);
        QVERIFY(manifest->tools.isEmpty());

        // No tools, so nothing connects: the socket need not exist.
        McpIpcBridge bridge(*manifest);
        QSignalSpy prepared(&bridge, &McpIpcBridge::prepared);
        bridge.prepare(QDir(dir.path()).filePath(QStringLiteral("none.sock")));
        // From the event loop, as every outcome.
        QCOMPARE(prepared.size(), 0);
        QVERIFY(prepared.wait(2000));
        QVERIFY(bridge.tools().isEmpty());

        Served served;
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("no tool")));
        QVERIFY2(served.serve(bridge), qPrintable(served.problem));
        QVERIFY(served.peer.send(request(1, QStringLiteral("tools/list"))));
        QJsonObject reply;
        QVERIFY2(served.read(1, &reply), qPrintable(served.problem));
        // The member itself: a reply without it would convert to an empty
        // array too.
        QCOMPARE(resultOf(reply).value(QLatin1StringView("tools")), QJsonValue(QJsonArray()));

        // An empty list of tools is the same.
        const std::optional<McpIpcManifest> listed =
            McpIpcManifest::parse(QByteArrayLiteral(R"({"tools": []})"), &error);
        QVERIFY2(listed.has_value(), qPrintable(error));
        McpIpcBridge empty(*listed);
        QSignalSpy emptyPrepared(&empty, &McpIpcBridge::prepared);
        empty.prepare(QDir(dir.path()).filePath(QStringLiteral("none.sock")));
        QVERIFY(emptyPrepared.wait(2000));
        QVERIFY(empty.tools().isEmpty());
        QString addError = QStringLiteral("stale");
        PhosphorMcp::McpServer server;
        QVERIFY(empty.addTools(server, &addError));
        QVERIFY(addError.isEmpty());
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
        QTest::newRow("a default argument without params")
            << QByteArray(R"({"call": "panes.zoom", "name": "zoom", "description": "d"})")
            << QStringLiteral(
                   "tool \"zoom\": panes.zoom has 2 overloads, (pane, factor), (pane), so \"params\" must name "
                   "the parameters of one");
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
        QTest::newRow("a parameter without a name")
            << QByteArray(R"({"call": "panes.unnamed", "name": "unnamed", "description": "d"})")
            << QStringLiteral(
                   "tool \"unnamed\": a parameter of panes.unnamed(?) has no name, so no argument can name it");
    }

    void unchoosableOverloadsFailAtStart()
    {
        QFETCH(QByteArray, tool);
        QFETCH(QString, message);
        // A tool that the bridge can build comes first. It must not stay
        // when the next one fails the start.
        Session session(
            QByteArrayLiteral(R"({"tools": [{"call": "panes.names", "name": "pane_names", "description": "d"},)") + tool
            + QByteArrayLiteral("]}"));
        QVERIFY2(session.valid(), qPrintable(session.problem));
        QVERIFY(session.application.start());
        QSignalSpy prepared(&session.bridge, &McpIpcBridge::prepared);
        QSignalSpy failed(&session.bridge, &McpIpcBridge::failed);
        session.bridge.prepare(session.application.socketPath());
        QVERIFY(failed.wait(5000));
        QCOMPARE(failed.at(0).at(0).toString(), message);
        QCOMPARE(prepared.size(), 0);
        QVERIFY(session.bridge.tools().isEmpty());

        // A bridge whose start failed adds nothing.
        QString error;
        QVERIFY(!session.bridge.addTools(session.server, &error));
        QCOMPARE(error, QStringLiteral("prepare() has not finished with prepared()"));
        QVERIFY(session.server.tools().isEmpty());
    }

    void methodThatCannotBeCalledIsLeftOut()
    {
        // A call takes at most 10 arguments. The method is a fact about
        // the application, as a missing one is: its tool is left out, and
        // the others stay.
        Session session(QByteArrayLiteral(R"({"tools": [
            {"call": "panes.eleven", "name": "eleven", "description": "d"},
            {"call": "panes.names", "name": "pane_names", "description": "d"}
        ]})"));
        QVERIFY2(session.prepare(), qPrintable(session.problem));
        QCOMPARE(session.bridge.warnings(),
                 QStringList{QStringLiteral("tool \"eleven\": panes.eleven(a, b, c, d, e, f, g, h, i, j, k) has 11 "
                                            "parameters, and a call takes at most 10, so the tool is left out")});
        const QList<PhosphorMcp::McpTool> tools = session.bridge.tools();
        QCOMPARE(tools.size(), 1);
        QCOMPARE(tools.at(0).name(), QStringLiteral("pane_names"));
    }

    void unreachableSocketFails()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QString error;
        const std::optional<McpIpcManifest> manifest = McpIpcManifest::parse(manifestJson(), &error);
        QVERIFY2(manifest.has_value(), qPrintable(error));
        McpIpcBridge bridge(*manifest);
        QSignalSpy failed(&bridge, &McpIpcBridge::failed);
        const QString path = QDir(dir.path()).filePath(QStringLiteral("none.sock"));
        bridge.prepare(path);
        QVERIFY(failed.wait(5000));
        const QString reason = failed.at(0).at(0).toString();
        QVERIFY2(reason.startsWith(QStringLiteral("cannot connect to %1: ").arg(path)), qPrintable(reason));
        // A second failed() would be a queued call of this thread.
        QCoreApplication::processEvents();
        QCOMPARE(failed.size(), 1);
        QVERIFY(bridge.tools().isEmpty());
    }

    void prepareRefusals_data()
    {
        QTest::addColumn<QString>("socketPath");
        QTest::addColumn<int>("timeoutMs");
        QTest::addColumn<QString>("target");
        QTest::addColumn<QString>("message");

        QTest::newRow("a socket path that is not absolute")
            << QStringLiteral("app.sock") << 1000 << QStringLiteral("t")
            << QStringLiteral("the socket path \"app.sock\" is not absolute");
        QTest::newRow("a timeout of 0") << QStringLiteral("/nonexistent/app.sock") << 0 << QStringLiteral("t")
                                        << QStringLiteral("the timeout of 0 ms is under 1 ms");
        QTest::newRow("a negative timeout") << QStringLiteral("/nonexistent/app.sock") << -5 << QStringLiteral("t")
                                            << QStringLiteral("the timeout of -5 ms is under 1 ms");
        // A manifest that code built, with a tool that the parser refuses.
        QTest::newRow("a manifest with an empty target")
            << QStringLiteral("/nonexistent/app.sock") << 1000 << QString()
            << QStringLiteral("the manifest is not valid: tools[0]: \"call\" is not of the form TARGET.METHOD");
    }

    void prepareRefusals()
    {
        QFETCH(QString, socketPath);
        QFETCH(int, timeoutMs);
        QFETCH(QString, target);
        QFETCH(QString, message);

        McpIpcManifest manifest = RawSession::manifestOf({QStringLiteral("f")});
        manifest.tools[0].target = target;
        McpIpcBridge bridge(manifest);
        QSignalSpy prepared(&bridge, &McpIpcBridge::prepared);
        QSignalSpy failed(&bridge, &McpIpcBridge::failed);
        bridge.prepare(socketPath, timeoutMs);
        // Never from inside prepare(): a program that calls it before its
        // event loop runs could not act on the signal.
        QCOMPARE(failed.size(), 0);
        QVERIFY(failed.wait(2000));
        QCOMPARE(failed.at(0).at(0).toString(), message);
        QCOMPARE(prepared.size(), 0);

        // prepare() runs one time.
        QTest::ignoreMessage(QtWarningMsg, "McpIpcBridge::prepare runs one time");
        bridge.prepare(socketPath, timeoutMs);
        QCoreApplication::processEvents();
        QCOMPARE(failed.size(), 1);
    }

    void addToolsAddsAllOrNone()
    {
        Session session;
        // Before prepared(), nothing is there to add.
        QString error;
        QVERIFY(!session.bridge.addTools(session.server, &error));
        QCOMPARE(error, QStringLiteral("prepare() has not finished with prepared()"));

        QVERIFY2(session.prepare(), qPrintable(session.problem));
        // The server has a tool with the name of the last tool of the
        // manifest. The tools before it must not go in.
        PhosphorMcp::McpTool own(QStringLiteral("greet"));
        own.setDescription(QStringLiteral("The server's own."));
        QVERIFY(session.server.addTool(own, [](PhosphorMcp::McpCall call) {
            call.finish(PhosphorMcp::McpResult::text(QStringLiteral("own")));
        }));
        QVERIFY(!session.bridge.addTools(session.server, &error));
        QCOMPARE(error, QStringLiteral("tool \"greet\": the server has a tool of that name already"));
        QCOMPARE(session.server.tools().size(), 1);
    }

    void socketThatDoesNotAnswerFails()
    {
        // The test does not read the schema request: the bridge writes it
        // from the event loop, and its timer of 100 ms may run first.
        RawSession session;
        session.readsTheRequest = false;
        qint64 schemaId = 0;
        QVERIFY2(session.connect(RawSession::manifestOf({QStringLiteral("f")}), &schemaId, 100),
                 qPrintable(session.problem));
        QSignalSpy& failed = *session.failed;
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(failed.at(0).at(0).toString(),
                 QStringLiteral("the application's socket at %1 did not answer within 100 ms").arg(session.raw.path()));
        // The bridge gave the connection up.
        QVERIFY(session.raw.clientClosed());
    }

    void schemaAnswersThatAreOfNoUse_data()
    {
        QTest::addColumn<QJsonObject>("answer");
        QTest::addColumn<QString>("message");

        QTest::newRow("a result that is no schema")
            << QJsonObject{{QStringLiteral("type"), QStringLiteral("reply")}, {QStringLiteral("result"), 5}}
            << QStringLiteral("the application's socket at %1 did not answer with a schema for the target \"t\"");
        QTest::newRow("an object without functions")
            << QJsonObject{{QStringLiteral("type"), QStringLiteral("reply")},
                           {QStringLiteral("result"), QJsonObject{{QStringLiteral("target"), QStringLiteral("t")}}}}
            << QStringLiteral("the application's socket at %1 did not answer with a schema for the target \"t\"");
        // Only NO_SUCH_TARGET tells that the target is not there. A text
        // of the socket loses its control characters on the way.
        QTest::newRow("an error without a code")
            << QJsonObject{{QStringLiteral("type"), QStringLiteral("error")},
                           {QStringLiteral("message"), QStringLiteral("no\ncode")}}
            << QStringLiteral(
                   "the application's socket at %1 gave no schema for the target \"t\" (an error "
                   "without a code: no code)");
        QTest::newRow("an error that is not about the target")
            << QJsonObject{{QStringLiteral("type"), QStringLiteral("error")},
                           {QStringLiteral("code"), QStringLiteral("BUSY")},
                           {QStringLiteral("message"), QStringLiteral("try\nlater")}}
            << QStringLiteral("the application's socket at %1 gave no schema for the target \"t\" (BUSY: try later)");
    }

    void schemaAnswersThatAreOfNoUse()
    {
        QFETCH(QJsonObject, answer);
        QFETCH(QString, message);

        RawSession session;
        qint64 schemaId = 0;
        QVERIFY2(session.connect(RawSession::manifestOf({QStringLiteral("f")}), &schemaId),
                 qPrintable(session.problem));
        QSignalSpy& prepared = *session.prepared;
        QSignalSpy& failed = *session.failed;
        answer.insert(QStringLiteral("id"), schemaId);
        QVERIFY(session.raw.write(answer));
        QVERIFY(failed.wait(5000));
        QCOMPARE(failed.at(0).at(0).toString(), message.arg(session.raw.path()));
        QCOMPARE(prepared.size(), 0);
    }

    void socketThatClosesDuringTheStartFails()
    {
        RawSession session;
        qint64 schemaId = 0;
        QVERIFY2(session.connect(RawSession::manifestOf({QStringLiteral("f")}), &schemaId),
                 qPrintable(session.problem));
        QSignalSpy& failed = *session.failed;
        session.raw.closeConnection();
        QVERIFY(failed.wait(5000));
        QCOMPARE(failed.at(0).at(0).toString(),
                 QStringLiteral("the application's socket at %1 ended the connection before the tools were built: "
                                "the server closed the connection")
                     .arg(session.raw.path()));
        QCoreApplication::processEvents();
        QCOMPARE(failed.size(), 1);
    }

    void schemaOfALaterVersion()
    {
        // The application may run a later phosphor-ipc. A member of a
        // parameter that this bridge does not know is left out of the
        // input schema, since the MCP server refuses a keyword that it
        // does not know. A type that it does not know, and two parameters
        // of one name, make a tool that it cannot offer.
        RawSession session;
        const auto parameter = [](const QString& name, const QString& type) {
            return QJsonObject{{QStringLiteral("name"), name}, {QStringLiteral("type"), type}};
        };
        QJsonObject extended = parameter(QStringLiteral("count"), QStringLiteral("integer"));
        extended.insert(QStringLiteral("x-later"), 1);
        extended.insert(QStringLiteral("minimum"), 0);
        QVERIFY2(session.prepare(RawSession::manifestOf(
                                     {QStringLiteral("extended"), QStringLiteral("typed"), QStringLiteral("twice")}),
                                 QJsonArray{
                                     method(QStringLiteral("extended"), QJsonArray{extended}),
                                     method(QStringLiteral("typed"),
                                            QJsonArray{parameter(QStringLiteral("when"), QStringLiteral("tuple"))}),
                                     method(QStringLiteral("twice"),
                                            QJsonArray{parameter(QStringLiteral("a"), QStringLiteral("integer")),
                                                       parameter(QStringLiteral("a"), QStringLiteral("string"))}),
                                 }),
                 qPrintable(session.problem));
        QCOMPARE(session.bridge->warnings(),
                 (QStringList{QStringLiteral("tool \"typed\": t.typed(when) has the parameter \"when\" of the type "
                                             "\"tuple\", which this version does not know, so the tool is left out"),
                              QStringLiteral("tool \"twice\": t.twice(a, a) has two parameters named \"a\", so the "
                                             "tool is left out")}));
        const QList<PhosphorMcp::McpTool> tools = session.bridge->tools();
        QCOMPARE(tools.size(), 1);
        QCOMPARE(tools.at(0).name(), QStringLiteral("extended"));
        QCOMPARE(tools.at(0).inputSchema(), parseObject(R"({
            "type": "object",
            "properties": {"count": {"type": "integer"}},
            "required": ["count"],
            "additionalProperties": false
        })"));
        // The server takes what the bridge built.
        QVERIFY2(session.serve(*session.bridge), qPrintable(session.problem));
    }

    void answersOfTheSocketAsToolResults()
    {
        RawSession session;
        QVERIFY2(
            session.prepare(RawSession::manifestOf({QStringLiteral("f")}), QJsonArray{method(QStringLiteral("f"))}),
            qPrintable(session.problem));
        QVERIFY2(session.serve(*session.bridge), qPrintable(session.problem));
        using PhosphorIpc::buildError;

        qint64 callId = 0;
        QJsonObject reply;
        // An error with a detail.
        QVERIFY2(session.callReachesTheSocket(1, QStringLiteral("f"), &callId), qPrintable(session.problem));
        QVERIFY(session.raw.write(buildError(callId, QStringLiteral("PARSE"), QStringLiteral("bad input"),
                                             QJsonObject{{QStringLiteral("line"), 3}})));
        QVERIFY2(session.read(1, &reply), qPrintable(session.problem));
        QVERIFY(isError(reply));
        QCOMPARE(firstText(reply), QStringLiteral("PARSE: bad input\ndetail: {\"line\":3}"));

        // An error with a code of the application's own that reads like a
        // code of the client. It keeps the application's words.
        QVERIFY2(session.callReachesTheSocket(2, QStringLiteral("f"), &callId), qPrintable(session.problem));
        QVERIFY(session.raw.write(
            buildError(callId, QStringLiteral("CONNECTION_CLOSED"), QStringLiteral("the remote pane hung up"))));
        QVERIFY2(session.read(2, &reply), qPrintable(session.problem));
        QVERIFY(isError(reply));
        QCOMPARE(firstText(reply), QStringLiteral("CONNECTION_CLOSED: the remote pane hung up"));

        // An error without a code.
        QVERIFY2(session.callReachesTheSocket(3, QStringLiteral("f"), &callId), qPrintable(session.problem));
        QVERIFY(session.raw.write(QJsonObject{{QStringLiteral("type"), QStringLiteral("error")},
                                              {QStringLiteral("id"), callId},
                                              {QStringLiteral("message"), QStringLiteral("no code")}}));
        QVERIFY2(session.read(3, &reply), qPrintable(session.problem));
        QVERIFY(isError(reply));
        QCOMPARE(firstText(reply),
                 QStringLiteral("MALFORMED_REPLY: the application answered with an error that has no code: no code"));

        // A number and an empty string.
        QVERIFY2(session.callReachesTheSocket(4, QStringLiteral("f"), &callId), qPrintable(session.problem));
        QVERIFY(session.raw.reply(callId, 1.5));
        QVERIFY2(session.read(4, &reply), qPrintable(session.problem));
        QVERIFY(!isError(reply));
        QCOMPARE(firstText(reply), QStringLiteral("1.5"));
        QVERIFY2(session.callReachesTheSocket(5, QStringLiteral("f"), &callId), qPrintable(session.problem));
        QVERIFY(session.raw.reply(callId, QString()));
        QVERIFY2(session.read(5, &reply), qPrintable(session.problem));
        // The member itself: firstText() is empty also for a result
        // without content.
        QCOMPARE(resultOf(reply).value(QLatin1StringView("content")),
                 QJsonValue(QJsonArray{QJsonObject{{QLatin1StringView("type"), QStringLiteral("text")},
                                                   {QLatin1StringView("text"), QString()}}}));
        QVERIFY(!isError(reply));
    }

    void toolThatTheServerRefusesIsLeftOut()
    {
        // A parameter name that is not valid UTF-16, which a socket can
        // send and the MCP server refuses as a property name. The bridge
        // leaves the tool out, so that addTools() adds the others.
        RawSession session;
        qint64 schemaId = 0;
        QVERIFY2(session.connect(RawSession::manifestOf({QStringLiteral("odd"), QStringLiteral("plain")}), &schemaId),
                 qPrintable(session.problem));
        const QByteArray schema = QByteArrayLiteral(R"({"type":"reply","id":)") + QByteArray::number(schemaId)
            + QByteArrayLiteral(R"(,"result":{"target":"t","functions":[)"
                                R"({"name":"odd","params":[{"name":"\ud800","type":"integer"}]},)"
                                R"({"name":"plain","params":[]}],"signals":[]}})"
                                "\n");
        QVERIFY(session.raw.write(schema));
        // The bridge writes no warning of its own. The line is in warnings().
        QTest::failOnWarning();
        QVERIFY2(session.waitForPrepared(*session.prepared, *session.failed), qPrintable(session.problem));

        const QStringList warnings = session.bridge->warnings();
        QCOMPARE(warnings.size(), 1);
        QVERIFY2(warnings.at(0).startsWith(QLatin1StringView("tool \"odd\": the MCP server does not take it (")),
                 qPrintable(warnings.at(0)));
        QVERIFY2(warnings.at(0).endsWith(QLatin1StringView("), so the tool is left out")), qPrintable(warnings.at(0)));
        const QList<PhosphorMcp::McpTool> tools = session.bridge->tools();
        QCOMPARE(tools.size(), 1);
        QCOMPARE(tools.at(0).name(), QStringLiteral("plain"));
        QVERIFY2(session.serve(*session.bridge), qPrintable(session.problem));
    }

    void messagesCutWhatTheSocketNames()
    {
        // The names of methods and parameters come from the socket. A
        // message shows at most 16 parameter names and 8 overloads, and no
        // control character of a name.
        const auto parameter = [](const QString& name) {
            return QJsonObject{{QStringLiteral("name"), name}, {QStringLiteral("type"), QStringLiteral("integer")}};
        };
        QJsonArray seventeen;
        QStringList shown;
        for (int i = 0; i < 17; ++i) {
            seventeen.append(parameter(QStringLiteral("p%1").arg(i)));
            if (i < 16) {
                shown.append(QStringLiteral("p%1").arg(i));
            }
        }
        {
            RawSession session;
            QVERIFY2(session.prepare(RawSession::manifestOf({QStringLiteral("many"), QStringLiteral("odd")}),
                                     QJsonArray{method(QStringLiteral("many"), seventeen),
                                                method(QStringLiteral("odd"),
                                                       QJsonArray{parameter(QStringLiteral("a\nb\x1b[31m")),
                                                                  parameter(QStringLiteral("a\nb\x1b[31m"))})}),
                     qPrintable(session.problem));
            QCOMPARE(session.bridge->warnings(),
                     (QStringList{QStringLiteral("tool \"many\": t.many(%1, ...) has 17 parameters, and a call takes "
                                                 "at most 10, so the tool is left out")
                                      .arg(shown.join(QStringLiteral(", "))),
                                  QStringLiteral("tool \"odd\": t.odd(a b [31m, a b [31m) has two parameters named "
                                                 "\"a b [31m\", so the tool is left out")}));
        }

        // Nine overloads, with 0 to 8 parameters. The message lists eight.
        QJsonArray overloads;
        QStringList listed;
        for (int count = 0; count < 9; ++count) {
            QJsonArray params;
            QStringList names;
            for (int i = 0; i < count; ++i) {
                params.append(parameter(QStringLiteral("q%1").arg(i)));
                names.append(QStringLiteral("q%1").arg(i));
            }
            overloads.append(method(QStringLiteral("over"), params));
            if (count < 8) {
                listed.append(QStringLiteral("(%1)").arg(names.join(QStringLiteral(", "))));
            }
        }
        RawSession session;
        qint64 schemaId = 0;
        QVERIFY2(session.connect(RawSession::manifestOf({QStringLiteral("over")}), &schemaId),
                 qPrintable(session.problem));
        QVERIFY(session.raw.reply(schemaId,
                                  QJsonObject{{QStringLiteral("target"), QStringLiteral("t")},
                                              {QStringLiteral("functions"), overloads},
                                              {QStringLiteral("signals"), QJsonArray()}}));
        QTRY_COMPARE(session.failed->size(), 1);
        QCOMPARE(session.failed->at(0).at(0).toString(),
                 QStringLiteral("tool \"over\": t.over has 9 overloads, %1, and 1 more, so \"params\" must name the "
                                "parameters of one")
                     .arg(listed.join(QStringLiteral(", "))));
    }

    void addToolsAfterTheServerStartedAddsNone()
    {
        // The other refusal of the server: it takes no tool while it runs.
        Session session;
        QVERIFY2(session.prepare(), qPrintable(session.problem));
        QVERIFY(session.peer.isValid());
        PhosphorMcp::McpTool own(QStringLiteral("own"));
        own.setDescription(QStringLiteral("The server's own."));
        QVERIFY(session.server.addTool(own, [](PhosphorMcp::McpCall call) {
            call.finish(PhosphorMcp::McpResult::text(QStringLiteral("own")));
        }));
        QVERIFY(session.server.start(session.peer.serverFd(), session.peer.serverFd()));
        QString error;
        QTest::ignoreMessage(QtWarningMsg,
                             QRegularExpression(QStringLiteral("McpServer::addTool: .*the server has started")));
        QVERIFY(!session.bridge.addTools(session.server, &error));
        QCOMPARE(error,
                 QStringLiteral("tool \"describe_pane\": the server has started, and its tool list does not "
                                "change"));
        QCOMPARE(session.server.tools().size(), 1);
    }

    void outcomeSlotCanDeleteTheBridgeLater_data()
    {
        QTest::addColumn<bool>("fails");
        QTest::newRow("prepared") << false;
        QTest::newRow("failed") << true;
    }

    void outcomeSlotCanDeleteTheBridgeLater()
    {
        QFETCH(bool, fails);
        McpIpcManifest manifest;
        if (fails) {
            manifest = RawSession::manifestOf({QStringLiteral("f")});
        }
        // The owner deletes the bridge when a check below fails.
        QObject owner;
        QPointer<McpIpcBridge> bridge(new McpIpcBridge(manifest, &owner));
        int outcomes = 0;
        const auto slot = [&] {
            ++outcomes;
            bridge->deleteLater();
        };
        connect(bridge, &McpIpcBridge::prepared, this, slot);
        connect(bridge, &McpIpcBridge::failed, this, slot);
        // No tools, so nothing connects. With a tool, no socket is there.
        bridge->prepare(QStringLiteral("/nonexistent/phosphor-mcp-ipc-test.sock"));
        QTRY_VERIFY(bridge.isNull());
        QCOMPARE(outcomes, 1);
    }
};

QTEST_GUILESS_MAIN(TestPhosphorMcpIpcBridge)
#include "test_phosphor_mcp_ipc_bridge.moc"
