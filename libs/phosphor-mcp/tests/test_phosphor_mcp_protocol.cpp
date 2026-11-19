// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// The protocol around the tools: JSON-RPC framing and its errors, the checks
// of every request's _meta, server/discover, tools/list, the refusal of
// initialize and of the methods that 2026-07-28 removed, and the end of the
// input.

#include "mcptesthelpers.h"

#include <PhosphorMcp/McpProtocol.h>
#include <PhosphorMcp/McpServer.h>

#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTest>

#include <memory>

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

// A server with two tools on a socket pair.
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

const QJsonValue NullId(QJsonValue::Null);

} // namespace

class TestPhosphorMcpProtocol : public QObject
{
    Q_OBJECT

private Q_SLOTS:
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

    void serverInfoDefaultsToTheApplication()
    {
        Peer peer;
        McpServer server;
        QCoreApplication::setApplicationName(QStringLiteral("app-name"));
        QCoreApplication::setApplicationVersion(QStringLiteral("9.9"));
        QVERIFY(server.start(peer.serverFd(), peer.serverFd()));
        QVERIFY(peer.send(request(1, QStringLiteral("server/discover"))));
        const QJsonObject info =
            resultOf(peer.readOne()).value(QLatin1StringView("_meta")).toObject().value(MetaServerInfo).toObject();
        QCOMPARE(info.value(QLatin1StringView("name")).toString(), QStringLiteral("app-name"));
        QCOMPARE(info.value(QLatin1StringView("version")).toString(), QStringLiteral("9.9"));
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
        QVERIFY(reply.value(QLatin1StringView("error"))
                    .toObject()
                    .value(QLatin1StringView("message"))
                    .toString()
                    .contains(QLatin1StringView("cursor")));
    }

    void metaIsRequired_data()
    {
        QTest::addColumn<QJsonObject>("message");
        const auto bare = [](const QJsonValue& params) {
            QJsonObject message{
                {QLatin1StringView("jsonrpc"), QStringLiteral("2.0")},
                {QLatin1StringView("id"), 7},
                {QLatin1StringView("method"), QStringLiteral("server/discover")},
            };
            if (!params.isUndefined()) {
                message.insert(QLatin1StringView("params"), params);
            }
            return message;
        };
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

        QTest::newRow("no params") << bare(QJsonValue(QJsonValue::Undefined));
        QTest::newRow("params not an object") << bare(QJsonArray{1});
        QTest::newRow("no _meta") << bare(QJsonObject());
        QTest::newRow("_meta not an object") << bare(QJsonObject{{QLatin1StringView("_meta"), 1}});
        QTest::newRow("no protocolVersion") << bare(QJsonObject{{QLatin1StringView("_meta"), noVersion}});
        QTest::newRow("protocolVersion a number") << bare(QJsonObject{{QLatin1StringView("_meta"), numberVersion}});
        QTest::newRow("no clientCapabilities") << bare(QJsonObject{{QLatin1StringView("_meta"), noCapabilities}});
        QTest::newRow("clientCapabilities an array")
            << bare(QJsonObject{{QLatin1StringView("_meta"), arrayCapabilities}});
        QTest::newRow("progressToken an object") << bare(QJsonObject{{QLatin1StringView("_meta"), objectToken}});
        QTest::newRow("progressToken a fraction") << bare(QJsonObject{{QLatin1StringView("_meta"), fractionToken}});
    }

    void metaIsRequired()
    {
        QFETCH(QJsonObject, message);
        Fixture f;
        QVERIFY(f.start());
        QVERIFY(f.peer.send(message));
        checkError(f.peer.readOne(), 7, ErrorCode::InvalidParams);
        // The same holds for tools/list and tools/call.
        message.insert(QLatin1StringView("method"), QStringLiteral("tools/call"));
        QVERIFY(f.peer.send(message));
        checkError(f.peer.readOne(), 7, ErrorCode::InvalidParams);
    }

    void unsupportedVersion()
    {
        Fixture f;
        QVERIFY(f.start());
        for (const QString& method :
             {QStringLiteral("server/discover"), QStringLiteral("tools/list"), QStringLiteral("tools/call")}) {
            QVERIFY(f.peer.send(request(4, method, QJsonObject{{QLatin1StringView("name"), QStringLiteral("zeta")}},
                                        meta(QStringLiteral("2025-06-18")))));
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
        QVERIFY(error.value(QLatin1StringView("message")).toString().contains(QLatin1StringView("2026-07-28")));
        QCOMPARE(error.value(QLatin1StringView("data")).toObject(),
                 (QJsonObject{{QLatin1StringView("supported"), QJsonArray{QStringLiteral("2026-07-28")}},
                              {QLatin1StringView("requested"), QStringLiteral("2025-06-18")}}));
        checkResult(replies.at(1), 1);
    }

    void removedAndUnknownMethods_data()
    {
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
        // Without _meta too: the method is what is wrong.
        QVERIFY(f.peer.send(QJsonObject{{QLatin1StringView("jsonrpc"), QStringLiteral("2.0")},
                                        {QLatin1StringView("id"), 6},
                                        {QLatin1StringView("method"), method}}));
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
        checkError(replies.at(0), NullId, ErrorCode::ParseError);
        checkError(replies.at(1), NullId, ErrorCode::ParseError);
        checkResult(replies.at(2), 2);
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
        checkError(replies.at(0), NullId, ErrorCode::InvalidRequest);
        checkResult(replies.at(1), 3);
    }

    void lineOverTheLimit()
    {
        Fixture f;
        // Room for an ordinary request (about 230 bytes), not for one padded
        // past the limit.
        f.server.setMaxLineBytes(400);
        QVERIFY(f.start());
        QJsonObject big = request(1, QStringLiteral("server/discover"));
        big.insert(QLatin1StringView("padding"), QString(1000, QLatin1Char('p')));
        QVERIFY(f.peer.send(big));
        QVERIFY(f.peer.send(request(2, QStringLiteral("server/discover"))));
        const QList<QJsonObject> replies = f.peer.read(2);
        QCOMPARE(replies.size(), 2);
        checkError(replies.at(0), NullId, ErrorCode::InvalidRequest);
        checkResult(replies.at(1), 2);
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
        QTest::newRow("id null") << with("id", QJsonValue::Null) << NullId;
        QTest::newRow("id an object") << with("id", QJsonObject()) << NullId;
        QTest::newRow("id a fraction") << with("id", 1.5) << NullId;
        QTest::newRow("id a boolean") << with("id", true) << NullId;
        QTest::newRow("no jsonrpc") << with("jsonrpc", QJsonValue(QJsonValue::Undefined)) << QJsonValue(1);
        QTest::newRow("jsonrpc 1.0") << with("jsonrpc", QStringLiteral("1.0")) << QJsonValue(1);
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
        checkError(f.peer.readOne(), replyId, ErrorCode::InvalidRequest);
    }

    void responsesAndBlankLinesAreIgnored()
    {
        Fixture f;
        QVERIFY(f.start());
        QVERIFY(f.peer.send(QJsonObject{{QLatin1StringView("jsonrpc"), QStringLiteral("2.0")},
                                        {QLatin1StringView("id"), 9},
                                        {QLatin1StringView("result"), QJsonObject()}}));
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
        const QJsonObject reply = f.peer.readOne();
        checkError(reply, 1, ErrorCode::InvalidParams);
        QVERIFY(reply.value(QLatin1StringView("error"))
                    .toObject()
                    .value(QLatin1StringView("message"))
                    .toString()
                    .contains(QLatin1StringView("nosuchtool")));
        // No name, and arguments that are not an object.
        QVERIFY(f.peer.send(request(2, QStringLiteral("tools/call"))));
        checkError(f.peer.readOne(), 2, ErrorCode::InvalidParams);
        QVERIFY(f.peer.send(request(3, QStringLiteral("tools/call"),
                                    QJsonObject{{QLatin1StringView("name"), QStringLiteral("zeta")},
                                                {QLatin1StringView("arguments"), QJsonArray()}})));
        checkError(f.peer.readOne(), 3, ErrorCode::InvalidParams);
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
        checkResult(f.peer.readOne(), 1);
        // Once only, and nothing read after it.
        spinFor(50);
        QCOMPARE(finished.count(), 1);
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("started or ended before")));
        QVERIFY(!f.server.start(f.peer.serverFd(), f.peer.serverFd()));
    }

    void outputClosedFinishes()
    {
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
};

QTEST_GUILESS_MAIN(TestPhosphorMcpProtocol)
#include "test_phosphor_mcp_protocol.moc"
