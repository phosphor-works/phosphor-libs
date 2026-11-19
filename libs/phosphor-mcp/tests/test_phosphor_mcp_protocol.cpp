// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// The protocol around the tools: JSON-RPC framing and its errors (bad JSON,
// JSON that is not an object, batches, ids, the line limit), the checks of
// every request's _meta, server/discover with and without tools, the server's
// name and version, tools/list, the protocol errors of tools/call and the
// envelope of its result, the refusal of initialize, of the methods that
// 2026-07-28 removed (ping, logging/setLevel) and of those the server does not
// offer, responses, notifications and blank lines that get no answer, and the
// end of the input and of the output.

#include "mcptesthelpers.h"

#include <PhosphorMcp/McpProtocol.h>
#include <PhosphorMcp/McpServer.h>
#include <PhosphorMcp/McpTransport.h>

#include <QJsonArray>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTest>

#include <limits>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace PhosphorMcp;
using namespace PhosphorMcpTests;

namespace {

QJsonObject objectSchema()
{
    return QJsonObject{{QLatin1StringView("type"), QStringLiteral("object")}};
}

void answerAtOnce(McpCall call)
{
    call.finish(McpResult::text(QStringLiteral("done")));
}

// A server with two tools, or none, on a socket pair.
struct Fixture
{
    Peer peer;
    McpServer server;

    explicit Fixture(bool withTools = true)
    {
        server.setServerInfo(QStringLiteral("test-server"), QStringLiteral("1.2.3"));
        if (withTools) {
            McpTool first(QStringLiteral("zeta"));
            first.setTitle(QStringLiteral("Zeta"))
                .setDescription(QStringLiteral("The first tool"))
                .setInputSchema(objectSchema())
                .setHints({.readOnly = true, .destructive = false, .idempotent = true, .openWorld = false});
            server.addTool(first, answerAtOnce);
            McpTool second(QStringLiteral("alpha"));
            second.setReadOnlyHint(false);
            server.addTool(second, answerAtOnce);
        }
    }
    bool start()
    {
        return server.start(peer.serverFd(), peer.serverFd());
    }
};

void checkEnvelope(const QJsonObject& reply, const QJsonValue& id)
{
    QCOMPARE(reply.value(QLatin1StringView("jsonrpc")).toString(), QStringLiteral("2.0"));
    QCOMPARE(reply.value(QLatin1StringView("id")), id);
}

void checkResult(const QJsonObject& reply, const QJsonValue& id)
{
    checkEnvelope(reply, id);
    QVERIFY2(reply.contains(QLatin1StringView("result")), QJsonDocument(reply).toJson().constData());
    const QJsonObject result = resultOf(reply);
    QCOMPARE(result.value(QLatin1StringView("resultType")).toString(), QStringLiteral("complete"));
    const QJsonObject info = result.value(QLatin1StringView("_meta")).toObject().value(MetaServerInfo).toObject();
    QCOMPARE(info.value(QLatin1StringView("name")).toString(), QStringLiteral("test-server"));
    QCOMPARE(info.value(QLatin1StringView("version")).toString(), QStringLiteral("1.2.3"));
}

void checkError(const QJsonObject& reply, const QJsonValue& id, int code)
{
    checkEnvelope(reply, id);
    QVERIFY2(reply.contains(QLatin1StringView("error")), QJsonDocument(reply).toJson().constData());
    QCOMPARE(errorCode(reply), code);
    QVERIFY(
        !reply.value(QLatin1StringView("error")).toObject().value(QLatin1StringView("message")).toString().isEmpty());
}

QString errorMessage(const QJsonObject& reply)
{
    return reply.value(QLatin1StringView("error")).toObject().value(QLatin1StringView("message")).toString();
}

// The answer to a message whose id cannot be read leaves the id out, as MCP's
// schema has an error's id be a string or a number, or absent.
const QJsonValue NoId(QJsonValue::Undefined);

} // namespace

class TestPhosphorMcpProtocol : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init()
    {
        // The silent paths stay silent: a closed output (EPIPE), a dropped
        // response, a notification. Every warning a test expects, it names.
        QTest::failOnWarning();
    }

    void discover()
    {
        Fixture f;
        f.server.setInstructions(QStringLiteral("Use the tools."));
        QVERIFY(f.server.setCache(60000, McpServer::CacheScope::Public));
        QVERIFY(f.start());
        QVERIFY(f.peer.send(request(1, QStringLiteral("server/discover"))));
        const QJsonObject reply = f.peer.readOne();
        checkResult(reply, 1);
        const QJsonObject result = resultOf(reply);
        QCOMPARE(result.value(QLatin1StringView("supportedVersions")).toArray(),
                 QJsonArray{QStringLiteral("2026-07-28")});
        QCOMPARE(result.value(QLatin1StringView("capabilities")).toObject(),
                 (QJsonObject{{QLatin1StringView("tools"), QJsonObject()}}));
        QCOMPARE(result.value(QLatin1StringView("instructions")).toString(), QStringLiteral("Use the tools."));
        QCOMPARE(result.value(QLatin1StringView("ttlMs")).toInteger(), Q_INT64_C(60000));
        QCOMPARE(result.value(QLatin1StringView("cacheScope")).toString(), QStringLiteral("public"));
    }

    void discoverDefaults()
    {
        // No instructions: none sent. The cache fields are required, so they
        // go out with their defaults.
        Fixture f;
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("negative ttl")));
        QVERIFY(!f.server.setCache(-1, McpServer::CacheScope::Public));
        QVERIFY(f.start());
        QVERIFY(f.peer.send(request(QStringLiteral("a"), QStringLiteral("server/discover"))));
        const QJsonObject reply = f.peer.readOne();
        checkResult(reply, QStringLiteral("a"));
        const QJsonObject result = resultOf(reply);
        QVERIFY(!result.contains(QLatin1StringView("instructions")));
        QCOMPARE(result.value(QLatin1StringView("ttlMs")).toInteger(-1), Q_INT64_C(0));
        QCOMPARE(result.value(QLatin1StringView("cacheScope")).toString(), QStringLiteral("private"));
    }

    void serverWithoutTools()
    {
        // It starts, with a warning, and does not claim the tools capability.
        Fixture f(false);
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("has no tools")));
        QVERIFY(f.start());
        QVERIFY(f.peer.send(request(1, QStringLiteral("server/discover"))));
        const QJsonObject reply = f.peer.readOne();
        checkResult(reply, 1);
        QCOMPARE(resultOf(reply).value(QLatin1StringView("capabilities")).toObject(), QJsonObject());
        QVERIFY(f.peer.send(request(2, QStringLiteral("tools/list"))));
        QCOMPARE(resultOf(f.peer.readOne()).value(QLatin1StringView("tools")).toArray(), QJsonArray());
    }

    void serverInfoDefaultsToTheApplication()
    {
        const QString name = QCoreApplication::applicationName();
        const QString version = QCoreApplication::applicationVersion();
        const auto restore = qScopeGuard([&] {
            QCoreApplication::setApplicationName(name);
            QCoreApplication::setApplicationVersion(version);
        });
        // The application's name, and its version, or "0" when it has none.
        for (const QString& appVersion : {QStringLiteral("9.9"), QString()}) {
            Peer peer;
            McpServer server;
            QVERIFY(server.addTool(McpTool(QStringLiteral("t")), answerAtOnce));
            QCoreApplication::setApplicationName(QStringLiteral("app-name"));
            QCoreApplication::setApplicationVersion(appVersion);
            QVERIFY(server.start(peer.serverFd(), peer.serverFd()));
            QVERIFY(peer.send(request(1, QStringLiteral("server/discover"))));
            const QJsonObject info =
                resultOf(peer.readOne()).value(QLatin1StringView("_meta")).toObject().value(MetaServerInfo).toObject();
            QCOMPARE(info.value(QLatin1StringView("name")).toString(), QStringLiteral("app-name"));
            QCOMPARE(info.value(QLatin1StringView("version")).toString(),
                     appVersion.isEmpty() ? QStringLiteral("0") : appVersion);
        }
        // Without a name of its own, the application's is its executable's.
        Peer peer;
        McpServer server;
        QVERIFY(server.addTool(McpTool(QStringLiteral("t")), answerAtOnce));
        QCoreApplication::setApplicationName(QString());
        QVERIFY(server.start(peer.serverFd(), peer.serverFd()));
        QVERIFY(peer.send(request(1, QStringLiteral("server/discover"))));
        const QJsonObject info =
            resultOf(peer.readOne()).value(QLatin1StringView("_meta")).toObject().value(MetaServerInfo).toObject();
        QCOMPARE(info.value(QLatin1StringView("name")).toString(), QStringLiteral("test_phosphor_mcp_protocol"));
    }

    void toolsList()
    {
        Fixture f;
        QVERIFY(f.server.setCache(1000, McpServer::CacheScope::Public));
        QVERIFY(f.start());
        QVERIFY(f.peer.send(request(1, QStringLiteral("tools/list"))));
        QVERIFY(f.peer.send(request(2, QStringLiteral("tools/list"))));
        const QList<QJsonObject> replies = f.peer.read(2);
        QCOMPARE(replies.size(), 2);
        checkResult(replies.at(0), 1);
        checkResult(replies.at(1), 2);
        const QJsonObject result = resultOf(replies.at(0));
        QCOMPARE(result.value(QLatin1StringView("ttlMs")).toInteger(), Q_INT64_C(1000));
        QCOMPARE(result.value(QLatin1StringView("cacheScope")).toString(), QStringLiteral("public"));
        QVERIFY(!result.contains(QLatin1StringView("nextCursor")));

        // Registration order, and the same list twice.
        const QJsonArray tools = result.value(QLatin1StringView("tools")).toArray();
        QCOMPARE(tools, resultOf(replies.at(1)).value(QLatin1StringView("tools")).toArray());
        QCOMPARE(tools.size(), 2);
        const QJsonObject zeta = tools.at(0).toObject();
        QCOMPARE(zeta.value(QLatin1StringView("name")).toString(), QStringLiteral("zeta"));
        QCOMPARE(zeta.value(QLatin1StringView("title")).toString(), QStringLiteral("Zeta"));
        QCOMPARE(zeta.value(QLatin1StringView("description")).toString(), QStringLiteral("The first tool"));
        QCOMPARE(zeta.value(QLatin1StringView("inputSchema")).toObject(), objectSchema());
        QVERIFY(!zeta.contains(QLatin1StringView("outputSchema")));
        QCOMPARE(zeta.value(QLatin1StringView("annotations")).toObject(),
                 (QJsonObject{
                     {QLatin1StringView("readOnlyHint"), true},
                     {QLatin1StringView("destructiveHint"), false},
                     {QLatin1StringView("idempotentHint"), true},
                     {QLatin1StringView("openWorldHint"), false},
                 }));

        // A tool without a schema takes no arguments, and sends only the
        // hints it set.
        const QJsonObject alpha = tools.at(1).toObject();
        QCOMPARE(alpha.value(QLatin1StringView("name")).toString(), QStringLiteral("alpha"));
        QCOMPARE(alpha.value(QLatin1StringView("inputSchema")).toObject(),
                 (QJsonObject{{QLatin1StringView("type"), QStringLiteral("object")},
                              {QLatin1StringView("additionalProperties"), false}}));
        QVERIFY(!alpha.contains(QLatin1StringView("title")));
        QVERIFY(!alpha.contains(QLatin1StringView("description")));
        QCOMPARE(alpha.value(QLatin1StringView("annotations")).toObject(),
                 (QJsonObject{{QLatin1StringView("readOnlyHint"), false}}));
    }

    void toolsListRefusesACursor()
    {
        Fixture f;
        QVERIFY(f.start());
        QVERIFY(f.peer.send(request(3, QStringLiteral("tools/list"),
                                    QJsonObject{{QLatin1StringView("cursor"), QStringLiteral("page2")},
                                                {QLatin1StringView("_meta"), meta()}})));
        const QJsonObject reply = f.peer.readOne();
        checkError(reply, 3, ErrorCode::InvalidParams);
        QCOMPARE(errorMessage(reply), QStringLiteral("Invalid params: no such cursor"));
    }

    void metaIsRequired_data()
    {
        QTest::addColumn<QJsonValue>("params");
        QTest::addColumn<QString>("expected");
        QJsonObject noVersion = meta();
        noVersion.remove(MetaProtocolVersion);
        QJsonObject noCapabilities = meta();
        noCapabilities.remove(MetaClientCapabilities);
        QJsonObject numberVersion = meta();
        numberVersion.insert(MetaProtocolVersion, 20260728);
        QJsonObject arrayCapabilities = meta();
        arrayCapabilities.insert(MetaClientCapabilities, QJsonArray());
        QJsonObject objectToken = meta();
        objectToken.insert(MetaProgressToken, QJsonObject());
        QJsonObject fractionToken = meta();
        fractionToken.insert(MetaProgressToken, 1.5);
        QJsonObject hugeToken = meta();
        hugeToken.insert(MetaProgressToken, 9223372036854775808.0);
        const auto withMeta = [](const QJsonValue& m) {
            return QJsonValue(QJsonObject{{QLatin1StringView("_meta"), m}});
        };
        const QString metaRequired = QStringLiteral("Invalid params: params._meta is required");
        const QString versionRequired =
            QStringLiteral("Invalid params: params._meta[\"%1\"] is required").arg(MetaProtocolVersion);
        const QString capabilitiesRequired =
            QStringLiteral("Invalid params: params._meta[\"%1\"] is required").arg(MetaClientCapabilities);
        const QString badToken = QStringLiteral(
            "Invalid params: params._meta.progressToken must be a string or an integer of at most 64 bits");

        QTest::newRow("no params") << QJsonValue(QJsonValue::Undefined) << metaRequired;
        QTest::newRow("params null") << QJsonValue(QJsonValue::Null)
                                     << QStringLiteral("Invalid params: params must be an object");
        QTest::newRow("params not an object")
            << QJsonValue(QJsonArray{1}) << QStringLiteral("Invalid params: params must be an object");
        QTest::newRow("no _meta") << QJsonValue(QJsonObject()) << metaRequired;
        QTest::newRow("_meta not an object") << withMeta(1) << metaRequired;
        QTest::newRow("no protocolVersion") << withMeta(noVersion) << versionRequired;
        QTest::newRow("protocolVersion a number") << withMeta(numberVersion) << versionRequired;
        QTest::newRow("no clientCapabilities") << withMeta(noCapabilities) << capabilitiesRequired;
        QTest::newRow("clientCapabilities an array") << withMeta(arrayCapabilities) << capabilitiesRequired;
        QTest::newRow("progressToken an object") << withMeta(objectToken) << badToken;
        QTest::newRow("progressToken a fraction") << withMeta(fractionToken) << badToken;
        QTest::newRow("progressToken past 2^63") << withMeta(hugeToken) << badToken;
    }

    void metaIsRequired()
    {
        // For each of the three methods, with a tools/call that names a tool,
        // so that only the check of _meta can give this message.
        QFETCH(QJsonValue, params);
        QFETCH(QString, expected);
        Fixture f;
        QVERIFY(f.start());
        int id = 7;
        for (const char* method : {"server/discover", "tools/list", "tools/call"}) {
            QJsonObject message{
                {QLatin1StringView("jsonrpc"), QStringLiteral("2.0")},
                {QLatin1StringView("id"), id},
                {QLatin1StringView("method"), QString::fromLatin1(method)},
            };
            if (!params.isUndefined()) {
                QJsonValue sent = params;
                if (params.isObject() && QLatin1StringView(method) == QLatin1StringView("tools/call")) {
                    QJsonObject withName = params.toObject();
                    withName.insert(QLatin1StringView("name"), QStringLiteral("zeta"));
                    sent = withName;
                }
                message.insert(QLatin1StringView("params"), sent);
            }
            QVERIFY(f.peer.send(message));
            const QJsonObject reply = f.peer.readOne();
            checkError(reply, id, ErrorCode::InvalidParams);
            QCOMPARE(errorMessage(reply), expected);
            ++id;
        }
    }

    void unsupportedVersion()
    {
        // The version is checked before the other keys of _meta, which
        // another revision may not have.
        QJsonObject withoutCapabilities = meta(QStringLiteral("2025-06-18"));
        withoutCapabilities.remove(MetaClientCapabilities);
        withoutCapabilities.insert(MetaProgressToken, QJsonObject());
        Fixture f;
        QVERIFY(f.start());
        for (const QJsonObject& requestMeta : {meta(QStringLiteral("2025-06-18")), withoutCapabilities}) {
            for (const QString& method :
                 {QStringLiteral("server/discover"), QStringLiteral("tools/list"), QStringLiteral("tools/call")}) {
                QVERIFY(f.peer.send(
                    request(4, method, QJsonObject{{QLatin1StringView("name"), QStringLiteral("zeta")}}, requestMeta)));
                const QJsonObject reply = f.peer.readOne();
                checkError(reply, 4, ErrorCode::UnsupportedProtocolVersion);
                const QJsonObject error = reply.value(QLatin1StringView("error")).toObject();
                QCOMPARE(error.value(QLatin1StringView("message")).toString(),
                         QStringLiteral("Unsupported protocol version"));
                QCOMPARE(error.value(QLatin1StringView("data")).toObject(),
                         (QJsonObject{{QLatin1StringView("supported"), QJsonArray{QStringLiteral("2026-07-28")}},
                                      {QLatin1StringView("requested"), QStringLiteral("2025-06-18")}}));
            }
        }
    }

    void initializeIsRefused()
    {
        Fixture f;
        QVERIFY(f.start());
        QVERIFY(f.peer.send(QJsonObject{
            {QLatin1StringView("jsonrpc"), QStringLiteral("2.0")},
            {QLatin1StringView("id"), 0},
            {QLatin1StringView("method"), QStringLiteral("initialize")},
            {QLatin1StringView("params"),
             QJsonObject{{QLatin1StringView("protocolVersion"), QStringLiteral("2025-06-18")},
                         {QLatin1StringView("capabilities"), QJsonObject()},
                         {QLatin1StringView("clientInfo"), QJsonObject()}}},
        }));
        // The legacy client then sends this notification, which gets no
        // answer, as no notification does.
        QVERIFY(f.peer.send(notification(QStringLiteral("notifications/initialized"))));
        QVERIFY(f.peer.send(notification(QStringLiteral("notifications/whatever"))));
        QVERIFY(f.peer.send(request(1, QStringLiteral("server/discover"))));
        const QList<QJsonObject> replies = f.peer.read(3, 1000);
        QCOMPARE(replies.size(), 2);
        checkError(replies.at(0), 0, ErrorCode::MethodNotFound);
        const QJsonObject error = replies.at(0).value(QLatin1StringView("error")).toObject();
        QCOMPARE(error.value(QLatin1StringView("message")).toString(),
                 QStringLiteral("This server supports MCP 2026-07-28 only, which has no initialize. Use "
                                "server/discover."));
        QCOMPARE(error.value(QLatin1StringView("data")).toObject(),
                 (QJsonObject{{QLatin1StringView("supported"), QJsonArray{QStringLiteral("2026-07-28")}},
                              {QLatin1StringView("requested"), QStringLiteral("2025-06-18")}}));
        checkResult(replies.at(1), 1);
    }

    void removedAndUnknownMethods_data()
    {
        // ping and logging/setLevel, which 2026-07-28 removed, methods that
        // it has and this server does not offer, and one that no revision
        // has.
        QTest::addColumn<QString>("method");
        for (const char* method : {"ping", "resources/list", "resources/read", "prompts/list", "logging/setLevel",
                                   "subscriptions/listen", "completion/complete", "nope"}) {
            QTest::newRow(method) << QString::fromLatin1(method);
        }
    }

    void removedAndUnknownMethods()
    {
        QFETCH(QString, method);
        Fixture f;
        QVERIFY(f.start());
        QVERIFY(f.peer.send(request(5, method)));
        checkError(f.peer.readOne(), 5, ErrorCode::MethodNotFound);
        // Without _meta too, and with params that are not an object: the
        // method is what is wrong.
        QVERIFY(f.peer.send(QJsonObject{{QLatin1StringView("jsonrpc"), QStringLiteral("2.0")},
                                        {QLatin1StringView("id"), 6},
                                        {QLatin1StringView("method"), method},
                                        {QLatin1StringView("params"), QJsonArray()}}));
        checkError(f.peer.readOne(), 6, ErrorCode::MethodNotFound);
    }

    void badJson()
    {
        Fixture f;
        QVERIFY(f.start());
        QVERIFY(f.peer.sendRaw("{\"jsonrpc\": \"2.0\", \"id\": 1, \"method\": \n"));
        QVERIFY(f.peer.sendRaw("not json at all\n"));
        QVERIFY(f.peer.send(request(2, QStringLiteral("server/discover"))));
        const QList<QJsonObject> replies = f.peer.read(3);
        QCOMPARE(replies.size(), 3);
        checkError(replies.at(0), NoId, ErrorCode::ParseError);
        checkError(replies.at(1), NoId, ErrorCode::ParseError);
        checkResult(replies.at(2), 2);
    }

    void jsonThatIsNotAnObject_data()
    {
        QTest::addColumn<QByteArray>("line");
        QTest::newRow("a number") << QByteArray("3");
        QTest::newRow("a string") << QByteArray("\"server/discover\"");
        QTest::newRow("null") << QByteArray("null");
        QTest::newRow("true") << QByteArray("true");
    }

    void jsonThatIsNotAnObject()
    {
        // Valid JSON, so not a Parse error: an Invalid Request.
        QFETCH(QByteArray, line);
        Fixture f;
        QVERIFY(f.start());
        QVERIFY(f.peer.sendRaw(line + '\n'));
        const QJsonObject reply = f.peer.readOne();
        checkError(reply, NoId, ErrorCode::InvalidRequest);
        QVERIFY(!reply.contains(QLatin1StringView("id")));
    }

    void batchIsRefused()
    {
        Fixture f;
        QVERIFY(f.start());
        const QJsonArray batch{request(1, QStringLiteral("server/discover")), request(2, QStringLiteral("tools/list"))};
        QVERIFY(f.peer.sendRaw(QJsonDocument(batch).toJson(QJsonDocument::Compact) + '\n'));
        QVERIFY(f.peer.send(request(3, QStringLiteral("server/discover"))));
        const QList<QJsonObject> replies = f.peer.read(3, 1000);
        QCOMPARE(replies.size(), 2);
        checkError(replies.at(0), NoId, ErrorCode::InvalidRequest);
        checkResult(replies.at(1), 3);
    }

    void lineOverTheLimit_data()
    {
        QTest::addColumn<bool>("afterStart");
        QTest::newRow("set before start") << false;
        QTest::newRow("set while running") << true;
    }

    void lineOverTheLimit()
    {
        QFETCH(bool, afterStart);
        Fixture f;
        // Room for an ordinary request (about 230 bytes), not for one padded
        // past the limit.
        if (!afterStart) {
            f.server.setMaxLineBytes(400);
        }
        QVERIFY(f.start());
        if (afterStart) {
            f.server.setMaxLineBytes(400);
        }
        QJsonObject big = request(1, QStringLiteral("server/discover"));
        big.insert(QLatin1StringView("padding"), QString(1000, QLatin1Char('p')));
        QVERIFY(f.peer.send(big));
        QVERIFY(f.peer.send(request(2, QStringLiteral("server/discover"))));
        const QList<QJsonObject> replies = f.peer.read(2);
        QCOMPARE(replies.size(), 2);
        checkError(replies.at(0), NoId, ErrorCode::InvalidRequest);
        checkResult(replies.at(1), 2);
    }

    void pendingWriteMarkSetWhileRunning()
    {
        Fixture f;
        QVERIFY(f.start());
        f.server.setMaxPendingWriteBytes(0);
        // Taken as 1, and in force at once.
        QCOMPARE(f.server.transport()->maxPendingWriteBytes(), Q_INT64_C(1));
    }

    void invalidRequests_data()
    {
        QTest::addColumn<QJsonObject>("message");
        QTest::addColumn<QJsonValue>("replyId");
        const auto with = [](const char* key, const QJsonValue& value) {
            QJsonObject message = request(1, QStringLiteral("server/discover"));
            if (value.isUndefined()) {
                message.remove(QLatin1StringView(key));
            } else {
                message.insert(QLatin1StringView(key), value);
            }
            return message;
        };
        QJsonObject notificationWithBadJsonrpc = notification(QStringLiteral("notifications/initialized"));
        notificationWithBadJsonrpc.insert(QLatin1StringView("jsonrpc"), QStringLiteral("1.0"));
        QTest::newRow("id null") << with("id", QJsonValue::Null) << NoId;
        QTest::newRow("id an object") << with("id", QJsonObject()) << NoId;
        QTest::newRow("id an array") << with("id", QJsonArray{1}) << NoId;
        QTest::newRow("id a fraction") << with("id", 1.5) << NoId;
        QTest::newRow("id a boolean") << with("id", true) << NoId;
        // Echoed, it would come back as another number.
        QTest::newRow("id past 2^63") << with("id", 9223372036854775808.0) << NoId;
        QTest::newRow("no jsonrpc") << with("jsonrpc", QJsonValue(QJsonValue::Undefined)) << QJsonValue(1);
        QTest::newRow("jsonrpc 1.0") << with("jsonrpc", QStringLiteral("1.0")) << QJsonValue(1);
        QTest::newRow("jsonrpc 1.0 without an id") << notificationWithBadJsonrpc << NoId;
        QTest::newRow("method a number") << with("method", 3) << QJsonValue(1);
        QTest::newRow("no method") << with("method", QJsonValue(QJsonValue::Undefined)) << QJsonValue(1);
    }

    void invalidRequests()
    {
        QFETCH(QJsonObject, message);
        QFETCH(QJsonValue, replyId);
        Fixture f;
        QVERIFY(f.start());
        QVERIFY(f.peer.send(message));
        const QJsonObject reply = f.peer.readOne();
        checkError(reply, replyId, ErrorCode::InvalidRequest);
        QCOMPARE(reply.contains(QLatin1StringView("id")), !replyId.isUndefined());
    }

    void integerIdsAtTheEdgesOf64Bits()
    {
        // The largest and the smallest qint64 are ids, and come back as they
        // went.
        Fixture f;
        QVERIFY(f.start());
        QVERIFY(
            f.peer.sendRaw("{\"jsonrpc\":\"2.0\",\"id\":9223372036854775807,\"method\":\"server/discover\","
                           "\"params\":{\"_meta\":"
                           + QJsonDocument(meta()).toJson(QJsonDocument::Compact) + "}}\n"));
        QCOMPARE(f.peer.readOne().value(QLatin1StringView("id")).toInteger(), std::numeric_limits<qint64>::max());
        QVERIFY(
            f.peer.sendRaw("{\"jsonrpc\":\"2.0\",\"id\":-9223372036854775808,\"method\":\"server/discover\","
                           "\"params\":{\"_meta\":"
                           + QJsonDocument(meta()).toJson(QJsonDocument::Compact) + "}}\n"));
        QCOMPARE(f.peer.readOne().value(QLatin1StringView("id")).toInteger(), std::numeric_limits<qint64>::min());
    }

    void responsesNotificationsAndBlankLinesAreIgnored()
    {
        Fixture f;
        QVERIFY(f.start());
        QVERIFY(f.peer.send(QJsonObject{{QLatin1StringView("jsonrpc"), QStringLiteral("2.0")},
                                        {QLatin1StringView("id"), 9},
                                        {QLatin1StringView("result"), QJsonObject()}}));
        QVERIFY(f.peer.send(QJsonObject{
            {QLatin1StringView("jsonrpc"), QStringLiteral("2.0")},
            {QLatin1StringView("id"), 10},
            {QLatin1StringView("error"),
             QJsonObject{{QLatin1StringView("code"), -1}, {QLatin1StringView("message"), QStringLiteral("x")}}}}));
        QJsonObject arrayParams = notification(QStringLiteral("notifications/initialized"));
        arrayParams.insert(QLatin1StringView("params"), QJsonArray{1});
        QVERIFY(f.peer.send(arrayParams));
        QVERIFY(f.peer.sendRaw("\n   \n"));
        QVERIFY(f.peer.send(request(1, QStringLiteral("server/discover"))));
        const QList<QJsonObject> replies = f.peer.read(2, 1000);
        QCOMPARE(replies.size(), 1);
        checkResult(replies.at(0), 1);
    }

    void unknownToolIsAProtocolError()
    {
        Fixture f;
        QVERIFY(f.start());
        QVERIFY(f.peer.send(callRequest(1, QStringLiteral("nosuchtool"))));
        QJsonObject reply = f.peer.readOne();
        checkError(reply, 1, ErrorCode::InvalidParams);
        QCOMPARE(errorMessage(reply), QStringLiteral("Unknown tool: nosuchtool"));
        // No name, and arguments that are not an object.
        QVERIFY(f.peer.send(request(2, QStringLiteral("tools/call"))));
        reply = f.peer.readOne();
        checkError(reply, 2, ErrorCode::InvalidParams);
        QCOMPARE(errorMessage(reply), QStringLiteral("Invalid params: name must be a string"));
        QVERIFY(f.peer.send(request(3, QStringLiteral("tools/call"),
                                    QJsonObject{{QLatin1StringView("name"), QStringLiteral("zeta")},
                                                {QLatin1StringView("arguments"), QJsonArray()}})));
        reply = f.peer.readOne();
        checkError(reply, 3, ErrorCode::InvalidParams);
        QCOMPARE(errorMessage(reply), QStringLiteral("Invalid params: arguments must be an object"));
    }

    void callResultEnvelope()
    {
        Fixture f;
        QVERIFY(f.start());
        // arguments may be left out.
        QVERIFY(f.peer.send(request(QStringLiteral("call-1"), QStringLiteral("tools/call"),
                                    QJsonObject{{QLatin1StringView("name"), QStringLiteral("alpha")}})));
        const QJsonObject reply = f.peer.readOne();
        checkResult(reply, QStringLiteral("call-1"));
        QCOMPARE(firstText(reply), QStringLiteral("done"));
        QVERIFY(!resultOf(reply).contains(QLatin1StringView("isError")));
    }

    void endOfInputFinishes()
    {
        Fixture f;
        QSignalSpy finished(&f.server, &McpServer::finished);
        QVERIFY(f.start());
        QVERIFY(f.server.isRunning());
        // The request on the last line, without its newline, is answered.
        QVERIFY(f.peer.sendRaw(
            QJsonDocument(request(1, QStringLiteral("server/discover"))).toJson(QJsonDocument::Compact)));
        f.peer.closeWrite();
        QVERIFY(spinUntil([&] {
            return finished.count() == 1;
        }));
        QCOMPARE(finished.first().first().value<McpServer::FinishReason>(), McpServer::FinishReason::EndOfInput);
        QVERIFY(!f.server.isRunning());
        QVERIFY(!f.server.transport());
        checkResult(f.peer.readOne(), 1);
        // Once only, and an ended server does not start again.
        spinFor(50);
        QCOMPARE(finished.count(), 1);
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("started or ended before")));
        QVERIFY(!f.server.start(f.peer.serverFd(), f.peer.serverFd()));
    }

    void outputClosedFinishes()
    {
        // A client that closes stdout ends the session: no warning.
        Fixture f;
        QSignalSpy finished(&f.server, &McpServer::finished);
        QVERIFY(f.start());
        // The client reads nothing more and keeps writing: the reply to the
        // next request finds no reader.
        ::shutdown(f.peer.clientFd(), SHUT_RD);
        QVERIFY(f.peer.send(request(1, QStringLiteral("server/discover"))));
        QVERIFY(spinUntil([&] {
            return finished.count() == 1;
        }));
        QCOMPARE(finished.first().first().value<McpServer::FinishReason>(), McpServer::FinishReason::OutputClosed);
    }

    void outputFailedFinishes()
    {
        // Any other failed write is an error, which the application can tell
        // apart, with a warning that names it.
        const int full = ::open("/dev/full", O_WRONLY | O_CLOEXEC);
        if (full < 0) {
            QSKIP("no /dev/full");
        }
        const auto closeFull = qScopeGuard([full] {
            ::close(full);
        });
        Fixture f;
        QSignalSpy finished(&f.server, &McpServer::finished);
        QVERIFY(f.server.start(f.peer.serverFd(), full));
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("writing the output failed")));
        QVERIFY(f.peer.send(request(1, QStringLiteral("server/discover"))));
        QVERIFY(spinUntil([&] {
            return finished.count() == 1;
        }));
        QCOMPARE(finished.first().first().value<McpServer::FinishReason>(), McpServer::FinishReason::OutputFailed);
    }

    void cancelReasonIsLogged()
    {
        // The spec has the reason logged, for debugging. It is the client's
        // text, so it is quoted, with its control characters escaped.
        QLoggingCategory::setFilterRules(QStringLiteral("phosphormcp.debug=true"));
        const auto restore = qScopeGuard([] {
            QLoggingCategory::setFilterRules(QString());
        });
        Peer peer;
        McpServer server;
        McpCall held;
        QVERIFY(server.addTool(McpTool(QStringLiteral("hold")), [&held](McpCall call) {
            held = std::move(call);
        }));
        QVERIFY(server.start(peer.serverFd(), peer.serverFd()));
        QVERIFY(peer.send(callRequest(1, QStringLiteral("hold"))));
        QVERIFY(spinUntil([&] {
            return held.isValid();
        }));
        QTest::ignoreMessage(QtDebugMsg,
                             QRegularExpression(QStringLiteral("cancelled a call of tool \"hold\" with the reason "
                                                               "\"too slow\\\\nFORGED\"")));
        QVERIFY(
            peer.send(notification(QStringLiteral("notifications/cancelled"),
                                   QJsonObject{{QLatin1StringView("requestId"), 1},
                                               {QLatin1StringView("reason"), QStringLiteral("too slow\nFORGED")}})));
        QVERIFY(spinUntil([&] {
            return held.isCancelled();
        }));
    }
};

QTEST_GUILESS_MAIN(TestPhosphorMcpProtocol)
#include "test_phosphor_mcp_protocol.moc"
