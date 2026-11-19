// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// Tool definitions: the checks of addTool (names, schemas), the argument
// checks before a handler runs, the client capabilities a tool needs, and the
// shape of results with structured content and images.

#include "mcptesthelpers.h"

#include <PhosphorMcp/McpProtocol.h>
#include <PhosphorMcp/McpServer.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTest>

using namespace PhosphorMcp;
using namespace PhosphorMcpTests;

namespace {

// JSON with single quotes for double ones, which keeps the schemas below
// readable without raw strings (moc does not parse them here).
QByteArray json(QByteArray text)
{
    return text.replace('\'', '"');
}

QJsonObject parse(const QByteArray& text)
{
    return QJsonDocument::fromJson(json(text)).object();
}

McpServer::Handler noop()
{
    return [](McpCall call) {
        call.finish(McpResult::text(QStringLiteral("ok")));
    };
}

// addTool with the warning it gives on a refusal expected and swallowed.
bool addQuietly(McpServer& server, const McpTool& tool, McpServer::Handler handler, QString* error)
{
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("McpServer::addTool")));
    return server.addTool(tool, std::move(handler), error);
}

// The schema of the argument tests.
QJsonObject paneSchema()
{
    return parse(
        "{ "
        "'type': 'object', "
        "'properties': { "
        "'pane': {'type': 'integer', 'minimum': 1}, "
        "'text': {'type': 'string', 'maxLength': 5}, "
        "'mode': {'enum': ['fast', 'slow']}, "
        "'options': { "
        "'type': 'object', "
        "'properties': {'depth': {'type': 'integer', 'maximum': 3}}, "
        "'additionalProperties': false "
        "}, "
        "'tags': {'type': 'array', 'items': {'type': 'string'}, 'maxItems': 2} "
        "}, "
        "'required': ['pane'], "
        "'additionalProperties': false "
        "}");
}

} // namespace

class TestPhosphorMcpTools : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void toolNames_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<bool>("valid");
        QTest::newRow("plain") << QStringLiteral("read") << true;
        QTest::newRow("every class") << QStringLiteral("a.B-c_9") << true;
        QTest::newRow("128") << QString(128, QLatin1Char('n')) << true;
        QTest::newRow("empty") << QString() << false;
        QTest::newRow("129") << QString(129, QLatin1Char('n')) << false;
        QTest::newRow("space") << QStringLiteral("two words") << false;
        QTest::newRow("slash") << QStringLiteral("a/b") << false;
        QTest::newRow("non-ASCII") << QStringLiteral("café") << false;
    }

    void toolNames()
    {
        QFETCH(QString, name);
        QFETCH(bool, valid);
        McpServer server;
        QString error;
        if (valid) {
            QVERIFY(server.addTool(McpTool(name), noop(), &error));
            QVERIFY(error.isEmpty());
        } else {
            QVERIFY(!addQuietly(server, McpTool(name), noop(), &error));
            QVERIFY(error.contains(QLatin1StringView("128")));
        }
    }

    void refusesDuplicatesEmptyHandlersAndLateTools()
    {
        McpServer server;
        QString error;
        QVERIFY(server.addTool(McpTool(QStringLiteral("one")), noop()));
        QVERIFY(!addQuietly(server, McpTool(QStringLiteral("one")), noop(), &error));
        QVERIFY(error.contains(QLatin1StringView("name")));
        QVERIFY(!addQuietly(server, McpTool(QStringLiteral("two")), McpServer::Handler(), &error));
        QVERIFY(error.contains(QLatin1StringView("handler")));

        Peer peer;
        QVERIFY(server.start(peer.serverFd(), peer.serverFd()));
        QVERIFY(!addQuietly(server, McpTool(QStringLiteral("three")), noop(), &error));
        QVERIFY(error.contains(QLatin1StringView("started")));
        QCOMPARE(server.tools().size(), 1);
    }

    void refusedSchemas_data()
    {
        QTest::addColumn<QByteArray>("schema");
        QTest::addColumn<QString>("expected");

        // The keywords whose meaning differs between Draft 7 and 2020-12, at
        // the root and nested.
        for (const char* keyword : {"$schema", "$ref", "$defs", "definitions", "prefixItems", "additionalItems",
                                    "dependentRequired", "dependentSchemas", "dependencies", "unevaluatedProperties",
                                    "unevaluatedItems", "$dynamicRef", "$anchor", "$id"}) {
            QTest::addRow("root %s", keyword)
                << QByteArray("{'type': 'object', '") + keyword + "': {}}" << QString::fromLatin1(keyword);
            QTest::addRow("nested %s", keyword)
                << QByteArray("{'type': 'object', 'properties': {'a': {'type': 'object', '") + keyword + "': {}}}}"
                << QStringLiteral("/properties/a/") + QString::fromLatin1(keyword);
        }
        QTest::newRow("$ref to a definition")
            << QByteArray("{'type': 'object', 'properties': {'a': {'$ref': '#/definitions/x'}}}")
            << QStringLiteral("/properties/a/$ref");
        QTest::newRow("items as an array")
            << QByteArray("{'type': 'object', 'properties': {'a': {'type': 'array', 'items': [{'type': 'string'}]}}}")
            << QStringLiteral("/properties/a/items");
        QTest::newRow("$id deep in anyOf")
            << QByteArray("{'type': 'object', 'anyOf': [{'properties': {'b': {'$id': 'x'}}}]}")
            << QStringLiteral("/anyOf/0/properties/b/$id");
        QTest::newRow("format") << QByteArray("{'type': 'object', 'properties': {'a': {'format': 'uri'}}}")
                                << QStringLiteral("format");
        QTest::newRow("multipleOf") << QByteArray("{'type': 'object', 'properties': {'a': {'multipleOf': 1}}}")
                                    << QStringLiteral("multipleOf");
        QTest::newRow("contains") << QByteArray("{'type': 'object', 'properties': {'a': {'contains': {}}}}")
                                  << QStringLiteral("contains");
        QTest::newRow("unknown keyword") << QByteArray("{'type': 'object', 'colour': 'red'}")
                                         << QStringLiteral("colour");
        QTest::newRow("root without type") << QByteArray("{'properties': {}}") << QStringLiteral("/type");
        QTest::newRow("root an array") << QByteArray("{'type': 'array'}") << QStringLiteral("/type");
        QTest::newRow("root type list") << QByteArray("{'type': ['object', 'null']}") << QStringLiteral("/type");
        // std::regex, which valijson matches them with, overflows the stack
        // on a long string: a model's argument must never reach it.
        QTest::newRow("pattern") << QByteArray(
            "{'type': 'object', 'properties': {'a': {'type': 'string', 'pattern': '^[a-z ]*$'}}}")
                                 << QStringLiteral("/properties/a/pattern: \"pattern\" is not supported");
        QTest::newRow("pattern says what to do")
            << QByteArray("{'type': 'object', 'properties': {'a': {'type': 'string', 'pattern': 'x'}}}")
            << QStringLiteral("Use \"enum\", or check the value in the handler");
        QTest::newRow("patternProperties")
            << QByteArray("{'type': 'object', 'patternProperties': {'^x': {'type': 'integer'}}}")
            << QStringLiteral("/patternProperties: \"patternProperties\" is not supported");
        QTest::newRow("pattern deep in items")
            << QByteArray("{'type': 'object', 'properties': {'a': {'type': 'array', 'items': {'pattern': 'x'}}}}")
            << QStringLiteral("/properties/a/items/pattern");
        QTest::newRow("unknown type") << QByteArray("{'type': 'object', 'properties': {'a': {'type': 'text'}}}")
                                      << QStringLiteral("text");
        QTest::newRow("Draft 4 exclusiveMinimum")
            << QByteArray("{'type': 'object', 'properties': {'a': {'minimum': 1, 'exclusiveMinimum': true}}}")
            << QStringLiteral("exclusiveMinimum");
        QTest::newRow("required not an array")
            << QByteArray("{'type': 'object', 'required': 'a'}") << QStringLiteral("/required");
        QTest::newRow("schema a number") << QByteArray("{'type': 'object', 'properties': {'a': 3}}")
                                         << QStringLiteral("/properties/a");
        QTest::newRow("empty anyOf") << QByteArray("{'type': 'object', 'anyOf': []}") << QStringLiteral("/anyOf");
    }

    void refusedSchemas()
    {
        QFETCH(QByteArray, schema);
        QFETCH(QString, expected);
        const QJsonObject object = QJsonDocument::fromJson(json(schema)).object();
        QVERIFY(!object.isEmpty());

        McpServer server;
        QString error;
        QVERIFY(!addQuietly(server, McpTool(QStringLiteral("t")).setInputSchema(object), noop(), &error));
        QVERIFY2(error.startsWith(QLatin1StringView("inputSchema")), qPrintable(error));
        QVERIFY2(error.contains(expected), qPrintable(error));

        // The same rules for the output schema.
        QVERIFY(!addQuietly(server, McpTool(QStringLiteral("t")).setOutputSchema(object), noop(), &error));
        QVERIFY2(error.startsWith(QLatin1StringView("outputSchema")), qPrintable(error));
        QVERIFY(server.tools().isEmpty());
    }

    void acceptedSchemas()
    {
        // Names and values that look like refused keywords are neither.
        const QJsonObject schema = parse(
            "{ "
            "'type': 'object', "
            "'title': 't', 'description': 'd', '$comment': 'c', "
            "'properties': { "
            "'$ref': {'type': 'string', 'enum': ['$ref', '$defs'], 'default': '$ref'}, "
            "'items': {'type': 'array', 'items': {'type': ['string', 'null']}, 'minItems': 0, 'uniqueItems': true}, "
            "'n': {'type': 'number', 'exclusiveMinimum': 0, 'exclusiveMaximum': 10}, "
            "'c': {'const': {'$schema': 'x'}, 'examples': [{'$id': 1}]}, "
            "'s': {'type': 'string', 'minLength': 1, 'readOnly': true, 'deprecated': false}, "
            "'pattern': {'type': 'string'}, 'patternProperties': {'type': 'object'}, "
            "'o': {'type': 'object', 'minProperties': 1, 'maxProperties': 2, "
            "'propertyNames': {'maxLength': 3}, "
            "'additionalProperties': {'type': 'boolean'}}, "
            "'u': {'oneOf': [{'type': 'string'}, {'type': 'integer'}]}, "
            "'v': {'allOf': [true, {'not': {'type': 'null'}}]} "
            "}, "
            "'required': ['$ref'], "
            "'additionalProperties': false "
            "}");
        QVERIFY(!schema.isEmpty());
        McpServer server;
        QString error;
        QVERIFY2(
            server.addTool(McpTool(QStringLiteral("t")).setInputSchema(schema).setOutputSchema(schema), noop(), &error),
            qPrintable(error));
    }

    void longStringArgument()
    {
        // The arguments of a model may be long. Nothing in a schema that
        // addTool takes walks a string character by character on the stack:
        // a pattern did (std::regex), and is refused.
        Peer peer;
        McpServer server;
        QString seen;
        QVERIFY(server.addTool(
            McpTool(QStringLiteral("long"))
                .setInputSchema(parse("{'type': 'object', 'properties': {'s': {'type': 'string', 'minLength': 1, "
                                      "'maxLength': 1000000}}, 'required': ['s']}")),
            [&](McpCall call) {
                seen = call.arguments().value(QLatin1StringView("s")).toString();
                call.finish(McpResult::text(QStringLiteral("ok")));
            }));
        QVERIFY(server.start(peer.serverFd(), peer.serverFd()));
        const QString text(600000, QLatin1Char('a'));
        QVERIFY(peer.send(callRequest(1, QStringLiteral("long"), QJsonObject{{QLatin1StringView("s"), text}})));
        QCOMPARE(firstText(peer.readOne()), QStringLiteral("ok"));
        QCOMPARE(seen.size(), text.size());
        // One over the limit: refused, not crashed.
        QVERIFY(peer.send(callRequest(2, QStringLiteral("long"),
                                      QJsonObject{{QLatin1StringView("s"), QString(1000001, QLatin1Char('a'))}})));
        QCOMPARE(resultOf(peer.readOne()).value(QLatin1StringView("isError")).toBool(), true);
    }

    void badArgumentsAreToolErrors_data()
    {
        QTest::addColumn<QByteArray>("arguments");
        QTest::addColumn<QString>("expected");
        QTest::newRow("missing required") << QByteArray("{}") << QStringLiteral("pane");
        QTest::newRow("wrong type") << QByteArray("{'pane': 'one'}") << QStringLiteral("arguments/pane");
        QTest::newRow("not an integer") << QByteArray("{'pane': 1.5}") << QStringLiteral("arguments/pane");
        QTest::newRow("below minimum") << QByteArray("{'pane': 0}") << QStringLiteral("arguments/pane");
        QTest::newRow("too long") << QByteArray("{'pane': 1, 'text': 'toolong'}") << QStringLiteral("arguments/text");
        QTest::newRow("not in enum") << QByteArray("{'pane': 1, 'mode': 'medium'}") << QStringLiteral("arguments/mode");
        QTest::newRow("nested") << QByteArray("{'pane': 1, 'options': {'depth': 9}}")
                                << QStringLiteral("arguments/options/depth");
        QTest::newRow("nested extra") << QByteArray("{'pane': 1, 'options': {'width': 1}}")
                                      << QStringLiteral("arguments/options");
        QTest::newRow("array item") << QByteArray("{'pane': 1, 'tags': ['a', 2]}")
                                    << QStringLiteral("arguments/tags/1");
        QTest::newRow("extra property") << QByteArray("{'pane': 1, 'colour': 'red'}") << QStringLiteral("colour");
    }

    void badArgumentsAreToolErrors()
    {
        QFETCH(QByteArray, arguments);
        QFETCH(QString, expected);
        Peer peer;
        McpServer server;
        int calls = 0;
        QVERIFY(server.addTool(McpTool(QStringLiteral("pane_tool")).setInputSchema(paneSchema()), [&](McpCall call) {
            ++calls;
            call.finish(McpResult::text(QStringLiteral("ran")));
        }));
        QVERIFY(server.start(peer.serverFd(), peer.serverFd()));
        QVERIFY(
            peer.send(callRequest(1, QStringLiteral("pane_tool"), QJsonDocument::fromJson(json(arguments)).object())));
        const QJsonObject reply = peer.readOne();
        // A result the model reads, not a protocol error.
        QVERIFY2(reply.contains(QLatin1StringView("result")), QJsonDocument(reply).toJson().constData());
        const QJsonObject result = resultOf(reply);
        QCOMPARE(result.value(QLatin1StringView("isError")).toBool(), true);
        QCOMPARE(result.value(QLatin1StringView("resultType")).toString(), QStringLiteral("complete"));
        const QString text = firstText(reply);
        QVERIFY2(text.contains(QLatin1StringView("pane_tool")), qPrintable(text));
        QVERIFY2(text.contains(expected), qPrintable(text));
        QCOMPARE(calls, 0);
    }

    void goodArgumentsReachTheHandler()
    {
        Peer peer;
        McpServer server;
        QJsonObject seen;
        QVERIFY(server.addTool(McpTool(QStringLiteral("pane_tool")).setInputSchema(paneSchema()), [&](McpCall call) {
            seen = call.arguments();
            QCOMPARE(call.toolName(), QStringLiteral("pane_tool"));
            QCOMPARE(call.requestId(), QJsonValue(4));
            QCOMPARE(call.requestMeta().value(MetaClientInfo).toObject().value(QLatin1StringView("name")).toString(),
                     QStringLiteral("test"));
            call.finish(McpResult::text(QStringLiteral("ran")));
        }));
        QVERIFY(server.start(peer.serverFd(), peer.serverFd()));
        const QJsonObject arguments =
            parse("{'pane': 3, 'text': 'hi', 'mode': 'fast', 'options': {'depth': 2}, 'tags': ['a']}");
        QVERIFY(peer.send(callRequest(4, QStringLiteral("pane_tool"), arguments)));
        const QJsonObject reply = peer.readOne();
        QCOMPARE(firstText(reply), QStringLiteral("ran"));
        QCOMPARE(seen, arguments);
    }

    void keywordSemantics_data()
    {
        // valijson checks Draft 7 and the client reads 2020-12: for each
        // keyword addTool allows, a value both dialects accept passes and one
        // both refuse fails.
        QTest::addColumn<QByteArray>("property");
        QTest::addColumn<QByteArray>("value");
        QTest::addColumn<bool>("accepted");
        const auto row = [](const char* name, const char* property, const char* value, bool accepted) {
            QTest::newRow(name) << QByteArray(property) << QByteArray(value) << accepted;
        };
        row("integer", "{'type': 'integer'}", "7", true);
        row("integer 3.0", "{'type': 'integer'}", "3.0", true);
        row("integer fraction", "{'type': 'integer'}", "3.5", false);
        row("integer past int", "{'type': 'integer'}", "3000000000", true);
        row("integer past int, negative", "{'type': 'integer'}", "-3000000000", true);
        row("integer 2^53", "{'type': 'integer'}", "9007199254740992", true);
        row("integer fraction past int", "{'type': 'integer'}", "3000000000.5", false);
        row("number", "{'type': 'number'}", "2.5", true);
        row("number not a string", "{'type': 'number'}", "'2'", false);
        row("type list", "{'type': ['string', 'null']}", "null", true);
        row("type list refuses", "{'type': ['string', 'null']}", "1", false);
        row("const", "{'const': 'x'}", "'x'", true);
        row("const refuses", "{'const': 'x'}", "'y'", false);
        row("enum number", "{'enum': [1, 2]}", "2", true);
        row("enum refuses", "{'enum': [1, 2]}", "3", false);
        row("exclusiveMinimum", "{'exclusiveMinimum': 1}", "1", false);
        row("exclusiveMaximum", "{'exclusiveMaximum': 1}", "0.5", true);
        row("maximum large", "{'type': 'integer', 'maximum': 4000000000}", "4000000001", false);
        row("minLength", "{'type': 'string', 'minLength': 2}", "'a'", false);
        row("minItems", "{'type': 'array', 'minItems': 1}", "[]", false);
        row("uniqueItems", "{'type': 'array', 'uniqueItems': true}", "[1, 2]", true);
        row("uniqueItems refuses", "{'type': 'array', 'uniqueItems': true}", "[1, 1]", false);
        row("items", "{'type': 'array', 'items': {'type': 'integer'}}", "[1, 'a']", false);
        row("minProperties", "{'type': 'object', 'minProperties': 1}", "{}", false);
        row("maxProperties", "{'type': 'object', 'maxProperties': 1}", "{'a': 1, 'b': 2}", false);
        row("propertyNames", "{'type': 'object', 'propertyNames': {'maxLength': 2}}", "{'abc': 1}", false);
        row("additionalProperties schema", "{'type': 'object', 'additionalProperties': {'type': 'boolean'}}",
            "{'a': 1}", false);
        row("oneOf both", "{'oneOf': [{'type': 'integer'}, {'type': 'number'}]}", "1", false);
        row("oneOf one", "{'oneOf': [{'type': 'integer'}, {'type': 'number'}]}", "1.5", true);
        row("anyOf", "{'anyOf': [{'type': 'string'}, {'type': 'integer'}]}", "true", false);
        row("allOf", "{'allOf': [{'minimum': 1}, {'maximum': 3}]}", "4", false);
        row("not", "{'not': {'type': 'null'}}", "null", false);
        row("false schema", "false", "1", false);
        row("annotations",
            "{'title': 't', 'description': 'd', 'default': 1, 'examples': [1], 'readOnly': true, "
            "'writeOnly': false, 'deprecated': true, '$comment': 'c'}",
            "1", true);
    }

    void keywordSemantics()
    {
        QFETCH(QByteArray, property);
        QFETCH(QByteArray, value);
        QFETCH(bool, accepted);
        const QJsonObject schema = parse(QByteArray("{'type': 'object', 'properties': {'v': " + property + "}}"));
        QVERIFY(!schema.isEmpty());
        const QJsonObject arguments = parse(QByteArray("{'v': " + value + "}"));
        QVERIFY(!arguments.isEmpty());
        Peer peer;
        McpServer server;
        QString error;
        QVERIFY2(server.addTool(McpTool(QStringLiteral("v")).setInputSchema(schema), noop(), &error),
                 qPrintable(error));
        QVERIFY(server.start(peer.serverFd(), peer.serverFd()));
        QVERIFY(peer.send(callRequest(1, QStringLiteral("v"), arguments)));
        const QJsonObject reply = peer.readOne();
        QVERIFY2(resultOf(reply).value(QLatin1StringView("isError")).toBool() == !accepted,
                 QJsonDocument(reply).toJson().constData());
    }

    void requiredClientCapabilities()
    {
        Peer peer;
        McpServer server;
        const QJsonObject required = parse("{'elicitation': {'form': {}}, 'roots': {}}");
        QVERIFY(server.addTool(McpTool(QStringLiteral("asks")).setRequiredClientCapabilities(required), noop()));
        QVERIFY(server.addTool(McpTool(QStringLiteral("plain")), noop()));
        QVERIFY(server.start(peer.serverFd(), peer.serverFd()));

        // None declared, one missing, the nested one missing: refused.
        for (const char* declared : {"{}", "{'elicitation': {'form': {}}}", "{'elicitation': {}, 'roots': {}}"}) {
            QVERIFY(peer.send(callRequest(1, QStringLiteral("asks"), QJsonObject(),
                                          meta(QString(ProtocolVersion), parse(declared)))));
            const QJsonObject reply = peer.readOne();
            QCOMPARE(errorCode(reply), ErrorCode::MissingRequiredClientCapability);
            QCOMPARE(reply.value(QLatin1StringView("error"))
                         .toObject()
                         .value(QLatin1StringView("data"))
                         .toObject()
                         .value(QLatin1StringView("requiredCapabilities"))
                         .toObject(),
                     required);
        }
        QVERIFY(peer.send(callRequest(
            2, QStringLiteral("asks"), QJsonObject(),
            meta(QString(ProtocolVersion), parse("{'elicitation': {'form': {}, 'url': {}}, 'roots': {'x': 1}}")))));
        QCOMPARE(firstText(peer.readOne()), QStringLiteral("ok"));
        // A tool that needs nothing runs with nothing declared.
        QVERIFY(peer.send(callRequest(3, QStringLiteral("plain"))));
        QCOMPARE(firstText(peer.readOne()), QStringLiteral("ok"));
    }

    void structuredContent()
    {
        Peer peer;
        McpServer server;
        const QJsonObject output =
            parse("{'type': 'object', 'properties': {'count': {'type': 'integer'}}, 'required': ['count']}");
        QVERIFY(server.addTool(McpTool(QStringLiteral("count")).setOutputSchema(output), [](McpCall call) {
            call.finish(
                McpResult::structured(QJsonObject{{QLatin1StringView("count"), 3}}).addText(QStringLiteral("three")));
        }));
        QVERIFY(server.start(peer.serverFd(), peer.serverFd()));
        QVERIFY(peer.send(callRequest(1, QStringLiteral("count"))));
        const QJsonObject result = resultOf(peer.readOne());
        QCOMPARE(result.value(QLatin1StringView("structuredContent")).toObject(),
                 (QJsonObject{{QLatin1StringView("count"), 3}}));
        // The tool's own text, then the JSON as text, last.
        const QJsonArray content = result.value(QLatin1StringView("content")).toArray();
        QCOMPARE(content.size(), 2);
        QCOMPARE(content.at(0).toObject().value(QLatin1StringView("text")).toString(), QStringLiteral("three"));
        QCOMPARE(content.at(1).toObject(),
                 (QJsonObject{{QLatin1StringView("type"), QStringLiteral("text")},
                              {QLatin1StringView("text"), QStringLiteral("{\"count\":3}")}}));
    }

    void structuredContentMustMatch()
    {
        Peer peer;
        McpServer server;
        const QJsonObject output =
            parse("{'type': 'object', 'properties': {'count': {'type': 'integer'}}, 'required': ['count']}");
        QList<bool> finished;
        McpTool tool(QStringLiteral("count"));
        tool.setInputSchema(parse("{'type': 'object', 'properties': {'kind': {'type': 'string'}}}"));
        tool.setOutputSchema(output);
        QVERIFY(server.addTool(tool, [&](McpCall call) {
            const QString kind = call.arguments().value(QLatin1StringView("kind")).toString();
            if (kind == QLatin1StringView("wrong")) {
                finished.append(call.finish(McpResult::structured(QJsonObject{{QLatin1StringView("count"), true}})));
            } else if (kind == QLatin1StringView("none")) {
                finished.append(call.finish(McpResult::text(QStringLiteral("no value"))));
            } else {
                // A failed call need not carry the value.
                finished.append(call.fail(QStringLiteral("could not count")));
            }
        }));
        QVERIFY(server.start(peer.serverFd(), peer.serverFd()));

        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("does not match its output schema")));
        QVERIFY(peer.send(callRequest(1, QStringLiteral("count"),
                                      QJsonObject{{QLatin1StringView("kind"), QStringLiteral("wrong")}})));
        QJsonObject reply = peer.readOne();
        QCOMPARE(errorCode(reply), ErrorCode::InternalError);
        QVERIFY(QJsonDocument(reply).toJson().contains("structuredContent/count"));

        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("does not match its output schema")));
        QVERIFY(peer.send(
            callRequest(2, QStringLiteral("count"), QJsonObject{{QLatin1StringView("kind"), QStringLiteral("none")}})));
        QCOMPARE(errorCode(peer.readOne()), ErrorCode::InternalError);

        QVERIFY(peer.send(callRequest(3, QStringLiteral("count"),
                                      QJsonObject{{QLatin1StringView("kind"), QStringLiteral("error")}})));
        reply = peer.readOne();
        QCOMPARE(resultOf(reply).value(QLatin1StringView("isError")).toBool(), true);
        QCOMPARE(firstText(reply), QStringLiteral("could not count"));
        QCOMPARE(finished, (QList<bool>{false, false, true}));
    }

    void imageContent()
    {
        Peer peer;
        McpServer server;
        const QByteArray png("\x89PNG\r\n\x1a\nnot really", 18);
        QVERIFY(server.addTool(McpTool(QStringLiteral("shot")), [&](McpCall call) {
            call.finish(McpResult().addImage(png, QStringLiteral("image/png")));
        }));
        QVERIFY(server.start(peer.serverFd(), peer.serverFd()));
        QVERIFY(peer.send(callRequest(1, QStringLiteral("shot"))));
        const QJsonObject block =
            resultOf(peer.readOne()).value(QLatin1StringView("content")).toArray().first().toObject();
        QCOMPARE(block.value(QLatin1StringView("type")).toString(), QStringLiteral("image"));
        QCOMPARE(block.value(QLatin1StringView("mimeType")).toString(), QStringLiteral("image/png"));
        QCOMPARE(QByteArray::fromBase64(block.value(QLatin1StringView("data")).toString().toLatin1()), png);
    }
};

QTEST_GUILESS_MAIN(TestPhosphorMcpTools)
#include "test_phosphor_mcp_tools.moc"
