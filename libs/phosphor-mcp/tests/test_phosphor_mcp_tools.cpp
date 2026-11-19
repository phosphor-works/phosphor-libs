// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// Tool definitions: the checks of addTool (names, schemas), the argument
// checks before a handler runs, the client capabilities a tool needs, and the
// shape of results with structured content and images.

#include "mcptesthelpers.h"

#include <PhosphorMcp/McpProtocol.h>
#include <PhosphorMcp/McpServer.h>

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTest>

#include <clocale>
#include <optional>
#include <string>

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

// A tools/call whose arguments are `arguments` exactly as written, so that
// a test can put on the wire what QJsonDocument would not write: 3.0,
// 9223372036854775808, an unpaired surrogate.
bool sendCall(Peer& peer, int id, const QString& tool, const QByteArray& arguments)
{
    QByteArray line = QJsonDocument(callRequest(id, tool, QJsonObject{{QLatin1StringView("a"), QStringLiteral("@@")}}))
                          .toJson(QJsonDocument::Compact);
    line.replace("{\"a\":\"@@\"}", json(arguments));
    return peer.sendRaw(line + '\n');
}

// The lines of an argument error after its first ("Invalid arguments for
// tool x:").
QString errorLines(const QJsonObject& reply)
{
    return firstText(reply).section(QLatin1Char('\n'), 1);
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
        "'tags': {'type': 'array', 'items': {'type': 'string'}, 'maxItems': 2}, "
        "'gone': false "
        "}, "
        "'required': ['pane'], "
        "'additionalProperties': false "
        "}");
}

// The shape of Afterglow's open_pane, whose env has propertyNames.
QJsonObject envSchema()
{
    return parse(
        "{ "
        "'type': 'object', "
        "'properties': { "
        "'where': {'type': 'string', 'enum': ['tab', 'split']}, "
        "'env': { "
        "'type': 'object', "
        "'additionalProperties': {'anyOf': [{'type': 'string'}, {'const': false}]}, "
        "'propertyNames': {'minLength': 1, 'maxLength': 256}, "
        "'maxProperties': 256 "
        "} "
        "}, "
        "'required': ['where'], "
        "'additionalProperties': false "
        "}");
}

QString dialectRefusal(const QString& pointer, const QString& keyword)
{
    return QStringLiteral(
               "%1: \"%2\" is not supported, because it means something else, or nothing, in one of JSON Schema "
               "Draft 7 and 2020-12")
        .arg(pointer, keyword);
}

QString unsupported(const QString& pointer, const QString& keyword)
{
    return QStringLiteral("%1: \"%2\" is not supported in a tool schema").arg(pointer, keyword);
}

QString patternRefusal(const QString& pointer, const QString& keyword)
{
    return QStringLiteral(
               "%1: \"%2\" is not supported, because the server cannot match regular expressions safely. Use "
               "\"enum\", or check the value in the handler")
        .arg(pointer, keyword);
}

QString propertyNamesRefusal(const QString& pointer)
{
    return pointer
        + QStringLiteral(
               ": \"propertyNames\" is supported only at the root and under a chain of \"properties\" from it");
}

QString inexactRefusal(const QString& pointer)
{
    return pointer
        + QStringLiteral(
               ": a number from 2^53 to 2^63 in magnitude is not supported here, because the server compares "
               "numbers as doubles, which round the integers there");
}

const QString CountRefusal = QStringLiteral(": must be an integer from 0 to 9007199254740991");
const QString Int64Range = QStringLiteral(": must be from -9223372036854775808 to 9223372036854775807");

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
        // "$" matches before a last newline: the pattern must not.
        QTest::newRow("trailing newline") << QStringLiteral("read\n") << false;
        QTest::newRow("128 and a newline") << QString(128, QLatin1Char('n')) + QLatin1Char('\n') << false;
    }

    void toolNames()
    {
        QFETCH(QString, name);
        QFETCH(bool, valid);
        McpServer server;
        // A sentinel: success must clear it.
        QString error = QStringLiteral("unset");
        if (valid) {
            QVERIFY(server.addTool(McpTool(name), noop(), &error));
            QVERIFY(error.isEmpty());
        } else {
            QVERIFY(!addQuietly(server, McpTool(name), noop(), &error));
            QVERIFY(error.contains(QLatin1StringView("128")));
        }
    }

    void toolDefinition()
    {
        McpTool tool;
        QVERIFY(tool.name().isEmpty());
        QVERIFY(!tool.readOnlyHint() && !tool.destructiveHint() && !tool.idempotentHint() && !tool.openWorldHint());
        QVERIFY(tool.outputSchema().isEmpty());
        QVERIFY(tool.requiredClientCapabilities().isEmpty());
        // A tool without arguments.
        QCOMPARE(tool.inputSchema(), parse("{'type': 'object', 'additionalProperties': false}"));

        const QJsonObject output = parse("{'type': 'object'}");
        const QJsonObject capabilities = parse("{'roots': {}}");
        tool.setName(QStringLiteral("n"))
            .setTitle(QStringLiteral("T"))
            .setDescription(QStringLiteral("D"))
            .setOutputSchema(output)
            .setReadOnlyHint(true)
            .setDestructiveHint(false)
            .setIdempotentHint(true)
            .setOpenWorldHint(false)
            .setRequiredClientCapabilities(capabilities);
        QCOMPARE(tool.name(), QStringLiteral("n"));
        QCOMPARE(tool.title(), QStringLiteral("T"));
        QCOMPARE(tool.description(), QStringLiteral("D"));
        QCOMPARE(tool.outputSchema(), output);
        QCOMPARE(tool.readOnlyHint(), std::optional<bool>(true));
        QCOMPARE(tool.destructiveHint(), std::optional<bool>(false));
        QCOMPARE(tool.idempotentHint(), std::optional<bool>(true));
        QCOMPARE(tool.openWorldHint(), std::optional<bool>(false));
        QCOMPARE(tool.requiredClientCapabilities(), capabilities);
        // The required capabilities are the server's alone.
        QCOMPARE(tool.toJson(),
                 parse("{'name': 'n', 'title': 'T', 'description': 'D', "
                       "'inputSchema': {'type': 'object', 'additionalProperties': false}, "
                       "'outputSchema': {'type': 'object'}, "
                       "'annotations': {'readOnlyHint': true, 'destructiveHint': false, "
                       "'idempotentHint': true, 'openWorldHint': false}}"));

        // Only the hints that are set, and no empty title or description.
        McpTool bare(QStringLiteral("b"));
        bare.setIdempotentHint(false);
        QCOMPARE(bare.toJson(),
                 parse("{'name': 'b', 'inputSchema': {'type': 'object', 'additionalProperties': false}, "
                       "'annotations': {'idempotentHint': false}}"));
        QCOMPARE(McpTool(QStringLiteral("c")).toJson(),
                 parse("{'name': 'c', 'inputSchema': {'type': 'object', 'additionalProperties': false}}"));
    }

    void refusesDuplicatesEmptyHandlersAndLateTools()
    {
        // The peer outlives the server, whose transport restores the flags
        // of its descriptors when it goes.
        Peer peer;
        McpServer server;
        QString error;
        QVERIFY(server.addTool(McpTool(QStringLiteral("one")), noop()));
        QVERIFY(!addQuietly(server, McpTool(QStringLiteral("one")), noop(), &error));
        QVERIFY(error.contains(QLatin1StringView("name")));
        QVERIFY(!addQuietly(server, McpTool(QStringLiteral("two")), McpServer::Handler(), &error));
        QVERIFY(error.contains(QLatin1StringView("handler")));

        QVERIFY(server.start(peer.serverFd(), peer.serverFd()));
        QVERIFY(!addQuietly(server, McpTool(QStringLiteral("three")), noop(), &error));
        QVERIFY(error.contains(QLatin1StringView("started")));
        QCOMPARE(server.tools().size(), 1);
    }

    void refusedSchemas_data()
    {
        QTest::addColumn<QByteArray>("schema");
        QTest::addColumn<QString>("expected");
        // False for the rows about the root's type, which only an input
        // schema has.
        QTest::addColumn<bool>("outputToo");
        const auto row = [](const char* name, const QByteArray& schema, const QString& expected,
                            bool outputToo = true) {
            QTest::newRow(name) << schema << expected << outputToo;
        };

        // The keywords whose meaning differs between Draft 7 and 2020-12, at
        // the root and nested.
        for (const char* keyword :
             {"$schema", "$ref", "$defs", "definitions", "prefixItems", "additionalItems", "dependentRequired",
              "dependentSchemas", "dependencies", "unevaluatedProperties", "unevaluatedItems", "$dynamicRef",
              "$dynamicAnchor", "$anchor", "$id", "$recursiveRef", "$recursiveAnchor", "$vocabulary"}) {
            const QString name = QString::fromLatin1(keyword);
            QTest::addRow("root %s", keyword) << QByteArray("{'type': 'object', '") + keyword + "': {}}"
                                              << dialectRefusal(QLatin1Char('/') + name, name) << true;
            QTest::addRow("nested %s", keyword)
                << QByteArray("{'type': 'object', 'properties': {'a': {'type': 'object', '") + keyword + "': {}}}}"
                << dialectRefusal(QStringLiteral("/properties/a/") + name, name) << true;
        }
        row("$ref to a definition", "{'type': 'object', 'properties': {'a': {'$ref': '#/definitions/x'}}}",
            dialectRefusal(QStringLiteral("/properties/a/$ref"), QStringLiteral("$ref")));
        row("items as an array",
            "{'type': 'object', 'properties': {'a': {'type': 'array', 'items': [{'type': 'string'}]}}}",
            QStringLiteral("/properties/a/items: \"items\" as an array is not supported, because it is a tuple in "
                           "Draft 7 and an error in 2020-12 (prefixItems)"));
        row("format", "{'type': 'object', 'properties': {'a': {'format': 'uri'}}}",
            unsupported(QStringLiteral("/properties/a/format"), QStringLiteral("format")));
        row("multipleOf", "{'type': 'object', 'properties': {'a': {'multipleOf': 1}}}",
            unsupported(QStringLiteral("/properties/a/multipleOf"), QStringLiteral("multipleOf")));
        row("contains", "{'type': 'object', 'properties': {'a': {'contains': {}}}}",
            unsupported(QStringLiteral("/properties/a/contains"), QStringLiteral("contains")));
        row("unknown keyword", "{'type': 'object', 'colour': 'red'}",
            unsupported(QStringLiteral("/colour"), QStringLiteral("colour")));
        const QString rootType = QStringLiteral("/type: the root of a tool schema must have \"type\": \"object\"");
        row("root without type", "{'properties': {}}", rootType, false);
        row("root an array", "{'type': 'array'}", rootType, false);
        row("root type list", "{'type': ['object', 'null']}", rootType, false);
        // std::regex, which valijson matches them with, overflows the stack
        // on a long string: a model's argument must never reach it.
        row("pattern", "{'type': 'object', 'properties': {'a': {'type': 'string', 'pattern': '^[a-z ]*$'}}}",
            patternRefusal(QStringLiteral("/properties/a/pattern"), QStringLiteral("pattern")));
        row("patternProperties", "{'type': 'object', 'patternProperties': {'^x': {'type': 'integer'}}}",
            patternRefusal(QStringLiteral("/patternProperties"), QStringLiteral("patternProperties")));
        row("pattern deep in items",
            "{'type': 'object', 'properties': {'a': {'type': 'array', 'items': {'pattern': 'x'}}}}",
            patternRefusal(QStringLiteral("/properties/a/items/pattern"), QStringLiteral("pattern")));
        // valijson compares every pair of items, even when they all differ.
        const QString uniqueItems = QStringLiteral(
            "/properties/a/uniqueItems: \"uniqueItems\" is not supported, because the server would compare every "
            "pair of items. Check for repeated items in the handler");
        row("uniqueItems", "{'type': 'object', 'properties': {'a': {'type': 'array', 'uniqueItems': true}}}",
            uniqueItems);
        row("uniqueItems false", "{'type': 'object', 'properties': {'a': {'uniqueItems': false}}}", uniqueItems);

        // Refused keywords below every kind of subschema.
        row("$id deep in anyOf", "{'type': 'object', 'anyOf': [{'properties': {'b': {'$id': 'x'}}}]}",
            dialectRefusal(QStringLiteral("/anyOf/0/properties/b/$id"), QStringLiteral("$id")));
        row("format in additionalProperties", "{'type': 'object', 'additionalProperties': {'format': 'uri'}}",
            unsupported(QStringLiteral("/additionalProperties/format"), QStringLiteral("format")));
        row("pattern in propertyNames", "{'type': 'object', 'propertyNames': {'pattern': 'x'}}",
            patternRefusal(QStringLiteral("/propertyNames/pattern"), QStringLiteral("pattern")));
        row("$ref in not", "{'type': 'object', 'not': {'$ref': '#'}}",
            dialectRefusal(QStringLiteral("/not/$ref"), QStringLiteral("$ref")));
        row("contains in oneOf", "{'type': 'object', 'oneOf': [{'contains': {}}]}",
            unsupported(QStringLiteral("/oneOf/0/contains"), QStringLiteral("contains")));
        row("multipleOf in allOf", "{'type': 'object', 'allOf': [true, {'multipleOf': 2}]}",
            unsupported(QStringLiteral("/allOf/1/multipleOf"), QStringLiteral("multipleOf")));

        // propertyNames anywhere the object it applies to is not fixed by
        // the schema (valijson#254).
        row("propertyNames in items",
            "{'type': 'object', 'properties': {'a': {'type': 'array', 'items': {'propertyNames': {'maxLength': 2}}}}}",
            propertyNamesRefusal(QStringLiteral("/properties/a/items/propertyNames")));
        row("propertyNames in additionalProperties",
            "{'type': 'object', 'additionalProperties': {'propertyNames': {'maxLength': 2}}}",
            propertyNamesRefusal(QStringLiteral("/additionalProperties/propertyNames")));
        row("propertyNames in anyOf", "{'type': 'object', 'anyOf': [{'propertyNames': {'maxLength': 2}}]}",
            propertyNamesRefusal(QStringLiteral("/anyOf/0/propertyNames")));
        row("propertyNames in not", "{'type': 'object', 'not': {'propertyNames': {'maxLength': 2}}}",
            propertyNamesRefusal(QStringLiteral("/not/propertyNames")));
        row("propertyNames in propertyNames", "{'type': 'object', 'propertyNames': {'propertyNames': {}}}",
            propertyNamesRefusal(QStringLiteral("/propertyNames/propertyNames")));

        // Names valijson cannot hold.
        row("property name with a slash", "{'type': 'object', 'properties': {'a/b': {'type': 'string'}}}",
            QStringLiteral("/properties/a~1b: a property name with \"/\" is not supported, because the validator "
                           "would confuse it with another place in the schema"));
        row("property name with a surrogate", "{'type': 'object', 'properties': {'a\\ud800': {}}}",
            QStringLiteral("/properties/a") + QChar(0xd800)
                + QStringLiteral(": a property name with an unpaired UTF-16 surrogate is not supported"));
        row("required name with a surrogate", "{'type': 'object', 'required': ['a', 'b\\udc00']}",
            QStringLiteral("/required/1: a property name with an unpaired UTF-16 surrogate is not supported"));
        row("enum string with a surrogate", "{'type': 'object', 'properties': {'a': {'enum': ['t\\ud800ab']}}}",
            QStringLiteral("/properties/a/enum/0: a string with an unpaired UTF-16 surrogate is not supported"));

        // The value of each keyword.
        row("required not an array", "{'type': 'object', 'required': 'a'}",
            QStringLiteral("/required: must be an array of distinct property names"));
        row("required not names", "{'type': 'object', 'required': ['a', 1]}",
            QStringLiteral("/required: must be an array of distinct property names"));
        row("required twice", "{'type': 'object', 'required': ['a', 'a']}",
            QStringLiteral("/required: must be an array of distinct property names"));
        row("required twice nested", "{'type': 'object', 'properties': {'a': {'required': ['x', 'y', 'x']}}}",
            QStringLiteral("/properties/a/required: must be an array of distinct property names"));
        row("schema a number", "{'type': 'object', 'properties': {'a': 3}}",
            QStringLiteral("/properties/a: a schema must be an object or a boolean"));
        row("additionalProperties a number", "{'type': 'object', 'additionalProperties': 3}",
            QStringLiteral("/additionalProperties: a schema must be an object or a boolean"));
        row("properties an array", "{'type': 'object', 'properties': []}",
            QStringLiteral("/properties: must be an object of schemas"));
        row("empty anyOf", "{'type': 'object', 'anyOf': []}",
            QStringLiteral("/anyOf: must be a non-empty array of schemas"));
        row("oneOf an object", "{'type': 'object', 'oneOf': {}}",
            QStringLiteral("/oneOf: must be a non-empty array of schemas"));
        row("unknown type", "{'type': 'object', 'properties': {'a': {'type': 'text'}}}",
            QStringLiteral("/properties/a/type: unknown type \"text\""));
        row("empty type list", "{'type': 'object', 'properties': {'a': {'type': []}}}",
            QStringLiteral("/properties/a/type: must be a type name or a non-empty array of them"));
        row("type twice", "{'type': 'object', 'properties': {'a': {'type': ['string', 'string']}}}",
            QStringLiteral("/properties/a/type: must be a type name or an array of distinct type names"));
        row("Draft 4 exclusiveMinimum",
            "{'type': 'object', 'properties': {'a': {'minimum': 1, 'exclusiveMinimum': true}}}",
            QStringLiteral("/properties/a/exclusiveMinimum: must be a number"));
        row("minimum a string", "{'type': 'object', 'properties': {'a': {'minimum': '1'}}}",
            QStringLiteral("/properties/a/minimum: must be a number"));
        // valijson compares numbers as doubles: maximum 2^53 lets 2^53 + 1
        // through to a handler that reads it whole.
        row("maximum 2^53", "{'type': 'object', 'properties': {'a': {'maximum': 9007199254740992}}}",
            inexactRefusal(QStringLiteral("/properties/a/maximum")));
        row("exclusiveMinimum -2^63",
            "{'type': 'object', 'properties': {'a': {'exclusiveMinimum': -9223372036854775808}}}",
            inexactRefusal(QStringLiteral("/properties/a/exclusiveMinimum")));
        row("enum past 2^53", "{'type': 'object', 'properties': {'a': {'enum': [1, 9007199254740993]}}}",
            inexactRefusal(QStringLiteral("/properties/a/enum/1")));
        row("const past 2^53", "{'type': 'object', 'properties': {'a': {'const': {'x': [9007199254740992]}}}}",
            inexactRefusal(QStringLiteral("/properties/a/const/x/0")));
        row("maxLength a string", "{'type': 'object', 'properties': {'a': {'maxLength': '3'}}}",
            QStringLiteral("/properties/a/maxLength") + CountRefusal);
        row("maxLength negative", "{'type': 'object', 'properties': {'a': {'maxLength': -1}}}",
            QStringLiteral("/properties/a/maxLength") + CountRefusal);
        row("maxLength fractional", "{'type': 'object', 'properties': {'a': {'maxLength': 1.5}}}",
            QStringLiteral("/properties/a/maxLength") + CountRefusal);
        // valijson casts it to int64_t, undefined from 2^63.
        row("maxLength 1e300", "{'type': 'object', 'properties': {'a': {'maxLength': 1e300}}}",
            QStringLiteral("/properties/a/maxLength") + CountRefusal);
        row("minItems 2^53", "{'type': 'object', 'properties': {'a': {'minItems': 9007199254740992}}}",
            QStringLiteral("/properties/a/minItems") + CountRefusal);
        row("minProperties a boolean", "{'type': 'object', 'minProperties': true}",
            QStringLiteral("/minProperties") + CountRefusal);
        row("description a number", "{'type': 'object', 'properties': {'a': {'description': 3}}}",
            QStringLiteral("/properties/a/description: must be a string"));
        row("title an object", "{'type': 'object', 'title': {}}", QStringLiteral("/title: must be a string"));
        row("$comment a number", "{'type': 'object', '$comment': 1}", QStringLiteral("/$comment: must be a string"));
        row("readOnly a string", "{'type': 'object', 'properties': {'a': {'readOnly': 'yes'}}}",
            QStringLiteral("/properties/a/readOnly: must be a boolean"));
        row("enum a string", "{'type': 'object', 'properties': {'a': {'enum': 'x'}}}",
            QStringLiteral("/properties/a/enum: must be an array"));
        row("examples a number", "{'type': 'object', 'examples': 1}", QStringLiteral("/examples: must be an array"));
    }

    void refusedSchemas()
    {
        QFETCH(QByteArray, schema);
        QFETCH(QString, expected);
        QFETCH(bool, outputToo);
        const QJsonObject object = QJsonDocument::fromJson(json(schema)).object();
        QVERIFY(!object.isEmpty());

        McpServer server;
        QString error;
        QVERIFY(!addQuietly(server, McpTool(QStringLiteral("t")).setInputSchema(object), noop(), &error));
        QCOMPARE(error, QStringLiteral("inputSchema") + expected);

        // The same rules for the output schema.
        if (outputToo) {
            QVERIFY(!addQuietly(server, McpTool(QStringLiteral("t")).setOutputSchema(object), noop(), &error));
            QCOMPARE(error, QStringLiteral("outputSchema") + expected);
        }
        QVERIFY(server.tools().isEmpty());
    }

    void acceptedSchemas()
    {
        // Names and values that look like refused keywords are neither, and
        // propertyNames at the root and down a chain of properties.
        const QJsonObject schema = parse(
            "{ "
            "'type': 'object', "
            "'title': 't', 'description': 'd', '$comment': 'c', "
            "'propertyNames': {'maxLength': 20}, "
            "'properties': { "
            "'$ref': {'type': 'string', 'enum': ['$ref', '$defs'], 'default': '$ref'}, "
            "'items': {'type': 'array', 'items': {'type': ['string', 'null']}, 'minItems': 0}, "
            "'n': {'type': 'number', 'exclusiveMinimum': 0, 'exclusiveMaximum': 10}, "
            "'big': {'type': 'integer', 'minimum': -9007199254740991, 'maximum': 9007199254740991}, "
            "'huge': {'type': 'number', 'maximum': 1e300, 'enum': [1e300, 4294967295]}, "
            "'c': {'const': {'$schema': 'x'}, 'examples': [{'$id': 1}]}, "
            "'s': {'type': 'string', 'minLength': 1, 'readOnly': true, 'deprecated': false}, "
            "'pattern': {'type': 'string'}, 'patternProperties': {'type': 'object'}, "
            "'uniqueItems': {'type': 'boolean'}, "
            "'o': {'type': 'object', 'minProperties': 1, 'maxProperties': 2, "
            "'propertyNames': {'maxLength': 3}, "
            "'properties': {'deep': {'propertyNames': false}}, "
            "'additionalProperties': {'type': 'boolean'}}, "
            "'t': {'propertyNames': true}, "
            "'u': {'oneOf': [{'type': 'string'}, {'type': 'integer'}]}, "
            "'v': {'allOf': [true, {'not': {'type': 'null'}}]} "
            "}, "
            "'required': ['$ref', 'a/b'], "
            "'additionalProperties': false "
            "}");
        QVERIFY(!schema.isEmpty());
        McpServer server;
        QString error;
        QVERIFY2(
            server.addTool(McpTool(QStringLiteral("t")).setInputSchema(schema).setOutputSchema(schema), noop(), &error),
            qPrintable(error));
        QVERIFY2(server.addTool(McpTool(QStringLiteral("env")).setInputSchema(envSchema()), noop(), &error),
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
        const QJsonObject reply = peer.readOne();
        QCOMPARE(resultOf(reply).value(QLatin1StringView("isError")).toBool(), true);
        QCOMPARE(errorLines(reply), QStringLiteral("- arguments/s: must be at most 1000000 characters long"));
    }

    void badArgumentsAreToolErrors_data()
    {
        QTest::addColumn<QByteArray>("arguments");
        // Every line after the first, exactly: no wrapper of valijson's
        // ("Failed to validate against ..."), and each error once.
        QTest::addColumn<QString>("expected");
        const auto row = [](const char* name, const char* arguments, const char* expected) {
            QTest::newRow(name) << QByteArray(arguments) << QString::fromUtf8(expected);
        };
        row("missing required", "{}", "- arguments/pane: is required");
        row("wrong type", "{'pane': 'one'}", "- arguments/pane: must be of type integer");
        row("not an integer", "{'pane': 1.5}", "- arguments/pane: must be of type integer");
        row("below minimum", "{'pane': 0}", "- arguments/pane: must be at least 1");
        row("too long", "{'pane': 1, 'text': 'toolong'}", "- arguments/text: must be at most 5 characters long");
        row("not in enum", "{'pane': 1, 'mode': 'medium'}", "- arguments/mode: must be one of the values of enum");
        // The bound as the schema has it, not "3.000000".
        row("nested", "{'pane': 1, 'options': {'depth': 9}}", "- arguments/options/depth: must be at most 3");
        row("nested extra", "{'pane': 1, 'options': {'width': 1}}",
            "- arguments/options/width: is not a known property");
        row("array item", "{'pane': 1, 'tags': ['a', 2]}", "- arguments/tags/1: must be of type string");
        row("too many items", "{'pane': 1, 'tags': ['a', 'b', 'c']}", "- arguments/tags: must have at most 2 items");
        row("extra property", "{'pane': 1, 'colour': 'red'}", "- arguments/colour: is not a known property");
        // valijson names only the first: each one is listed.
        row("two extra properties", "{'pane': 1, 'colour': 'red', 'size': 2}",
            "- arguments/colour: is not a known property\n- arguments/size: is not a known property");
        // A false subschema fails with only a wrapper, which stays even when
        // another property has an error of its own.
        row("false subschema", "{'pane': 1, 'gone': 1}", "- arguments/gone: does not match the schema");
        row("false subschema and another error", "{'pane': 0, 'gone': 1}",
            "- arguments/gone: does not match the schema\n- arguments/pane: must be at least 1");
        row("several", "{'text': 'toolong', 'tags': [1], 'colour': 'red'}",
            "- arguments/tags/0: must be of type string\n- arguments/text: must be at most 5 characters long\n"
            "- arguments/colour: is not a known property\n- arguments/pane: is required");
        // What valijson would read differently from the handler, refused
        // before it: an integer past qint64 (2^63 passes minimum 1 there,
        // and the handler's toInteger() reads 0), an unpaired surrogate.
        row("integer 2^63", "{'pane': 9223372036854775808}",
            "- arguments/pane: must be from -9223372036854775808 to 9223372036854775807");
        row("unpaired surrogate", "{'pane': 1, 'text': 'ab\\ud800'}",
            "- arguments/text: contains an unpaired UTF-16 surrogate");
        row("surrogate in a name", "{'pane': 1, 'x\\udc00': 1}",
            "- arguments/x\xef\xbf\xbd: the property name contains an unpaired UTF-16 surrogate");
        row("surrogates everywhere", "{'pane': 1e300, 'text': '\\ud800', 'tags': ['\\udc00']}",
            "- arguments/pane: must be from -9223372036854775808 to 9223372036854775807\n"
            "- arguments/tags/0: contains an unpaired UTF-16 surrogate\n"
            "- arguments/text: contains an unpaired UTF-16 surrogate");
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
        QVERIFY(sendCall(peer, 1, QStringLiteral("pane_tool"), arguments));
        const QJsonObject reply = peer.readOne();
        // A result the model reads, not a protocol error.
        QVERIFY2(reply.contains(QLatin1StringView("result")), QJsonDocument(reply).toJson().constData());
        const QJsonObject result = resultOf(reply);
        QCOMPARE(result.value(QLatin1StringView("isError")).toBool(), true);
        QCOMPARE(result.value(QLatin1StringView("resultType")).toString(), QStringLiteral("complete"));
        QVERIFY2(firstText(reply).startsWith(QLatin1StringView("Invalid arguments for tool pane_tool:\n")),
                 qPrintable(firstText(reply)));
        QCOMPARE(errorLines(reply), expected);
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

    void manyErrorsAreCapped()
    {
        // 30 wrong items: ten lines, and one that counts the rest.
        Peer peer;
        McpServer server;
        QVERIFY(server.addTool(McpTool(QStringLiteral("pane_tool")).setInputSchema(paneSchema()), noop()));
        QVERIFY(server.start(peer.serverFd(), peer.serverFd()));
        QJsonArray tags;
        for (int i = 0; i < 30; ++i) {
            tags.append(i);
        }
        QVERIFY(peer.send(callRequest(1, QStringLiteral("pane_tool"),
                                      QJsonObject{{QLatin1StringView("pane"), 1}, {QLatin1StringView("tags"), tags}})));
        const QStringList lines = errorLines(peer.readOne()).split(QLatin1Char('\n'));
        QCOMPARE(lines.size(), 11);
        QCOMPARE(lines.first(), QStringLiteral("- arguments/tags/0: must be of type string"));
        QCOMPARE(lines.at(9), QStringLiteral("- arguments/tags/9: must be of type string"));
        // 30 items and maxItems.
        QCOMPARE(lines.last(), QStringLiteral("- arguments: and 21 more errors"));
    }

    void hugeArgumentsAnswerFast()
    {
        // Errors are collected only for small arguments: valijson keeps each
        // in a queue without bound, and these would freeze the server.
        QFETCH(QByteArray, arguments);
        QFETCH(QString, expected);
        Peer peer;
        McpServer server;
        int calls = 0;
        QVERIFY(server.addTool(McpTool(QStringLiteral("env")).setInputSchema(envSchema()), [&](McpCall call) {
            ++calls;
            call.finish(McpResult::text(QStringLiteral("ran")));
        }));
        QVERIFY(server.start(peer.serverFd(), peer.serverFd()));
        QElapsedTimer timer;
        timer.start();
        QVERIFY(sendCall(peer, 1, QStringLiteral("env"), arguments));
        const QJsonObject reply = peer.readOne(30000);
        QVERIFY2(timer.elapsed() < 5000, qPrintable(QString::number(timer.elapsed())));
        QCOMPARE(errorLines(reply), expected);
        QCOMPARE(calls, 0);
    }

    void hugeArgumentsAnswerFast_data()
    {
        QTest::addColumn<QByteArray>("arguments");
        QTest::addColumn<QString>("expected");
        const QString tooLarge = QStringLiteral(
            "- arguments: does not match the schema, and is too large for the server to list each error");
        const auto repeat = [](const QByteArray& item, int count) {
            QByteArray list;
            for (int i = 0; i < count; ++i) {
                list += (i ? "," : "") + item;
            }
            return list;
        };

        // Each wrong twice (anyOf), and maxProperties: 152 s with every
        // error collected.
        QByteArray env;
        for (int i = 0; i < 50000; ++i) {
            env += (i ? ",'V" : "'V") + QByteArray::number(i) + "':" + QByteArray::number(i);
        }
        QTest::newRow("50000 wrong variables") << "{'where': 'tab', 'env': {" + env + "}}" << tooLarge;
        QTest::newRow("400000 wrong items") << "{'where': [" + repeat("1", 400000) + "]}" << tooLarge;
        // An unpaired surrogate in each: listed whatever the size, cheaply.
        QTest::newRow("400000 surrogates") << "{'where': [" + repeat("'\\ud800'", 400000) + "]}"
                                           << QStringLiteral(
                                                  "- arguments/where/0: contains an unpaired UTF-16 surrogate\n"
                                                  "- arguments/where/1: contains an unpaired UTF-16 surrogate\n"
                                                  "- arguments/where/2: contains an unpaired UTF-16 surrogate\n"
                                                  "- arguments/where/3: contains an unpaired UTF-16 surrogate\n"
                                                  "- arguments/where/4: contains an unpaired UTF-16 surrogate\n"
                                                  "- arguments/where/5: contains an unpaired UTF-16 surrogate\n"
                                                  "- arguments/where/6: contains an unpaired UTF-16 surrogate\n"
                                                  "- arguments/where/7: contains an unpaired UTF-16 surrogate\n"
                                                  "- arguments/where/8: contains an unpaired UTF-16 surrogate\n"
                                                  "- arguments/where/9: contains an unpaired UTF-16 surrogate\n"
                                                  "- arguments: and 399990 more errors");
    }

    void longNamesAreShortInErrors()
    {
        // The model reads its errors back: a name of any length is cut.
        Peer peer;
        McpServer server;
        QVERIFY(server.addTool(McpTool(QStringLiteral("pane_tool")).setInputSchema(paneSchema()), noop()));
        QVERIFY(server.start(peer.serverFd(), peer.serverFd()));
        const QString name(5000, QLatin1Char('n'));
        QVERIFY(peer.send(
            callRequest(1, QStringLiteral("pane_tool"), QJsonObject{{QLatin1StringView("pane"), 1}, {name, 1}})));
        QCOMPARE(errorLines(peer.readOne()),
                 QStringLiteral("- arguments/") + QString(61, QLatin1Char('n'))
                     + QStringLiteral("...: is not a known property"));
    }

    void boundsAreTheSameInEveryLocale()
    {
        // QCoreApplication sets the locale of the environment, and valijson
        // writes bounds with std::to_string: "2,500000" under de_DE.
        const std::string saved = std::setlocale(LC_NUMERIC, nullptr);
        bool commaLocale = false;
        for (const char* name : {"de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8", "fr_FR.utf8"}) {
            if (!commaLocale) {
                commaLocale = std::setlocale(LC_NUMERIC, name) != nullptr;
            }
        }
        Peer peer;
        McpServer server;
        QVERIFY(server.addTool(McpTool(QStringLiteral("v"))
                                   .setInputSchema(parse("{'type': 'object', 'properties': {'v': {'type': 'number', "
                                                         "'maximum': 2.5, 'exclusiveMinimum': 0.0000001}}}")),
                               noop()));
        QVERIFY(server.start(peer.serverFd(), peer.serverFd()));
        QVERIFY(sendCall(peer, 1, QStringLiteral("v"), "{'v': 3}"));
        const QString high = errorLines(peer.readOne());
        QVERIFY(sendCall(peer, 2, QStringLiteral("v"), "{'v': 0}"));
        const QString low = errorLines(peer.readOne());
        std::setlocale(LC_NUMERIC, saved.c_str());
        QCOMPARE(high, QStringLiteral("- arguments/v: must be at most 2.5"));
        QCOMPARE(low, QStringLiteral("- arguments/v: must be greater than 1e-07"));
        if (!commaLocale) {
            qInfo("No locale with a decimal comma is installed: checked in the C locale only");
        }
    }

    void keywordSemantics_data()
    {
        // valijson checks Draft 7 and the client reads 2020-12: for each
        // keyword addTool allows, a value both dialects accept passes and one
        // both refuse fails. The annotations never refuse.
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
        row("integer qint64 max", "{'type': 'integer'}", "9223372036854775807", true);
        row("integer qint64 min", "{'type': 'integer'}", "-9223372036854775808", true);
        // valijson takes 2^63 for an integer of at least 1, and toInteger()
        // gives the handler 0: refused before valijson.
        row("integer 2^63 with a minimum", "{'type': 'integer', 'minimum': 1}", "9223372036854775808", false);
        row("integer 1e19", "{'type': 'integer'}", "1e19", false);
        row("number", "{'type': 'number'}", "2.5", true);
        row("number not a string", "{'type': 'number'}", "'2'", false);
        row("number past qint64", "{'type': 'number'}", "1e300", false);
        row("type list", "{'type': ['string', 'null']}", "null", true);
        row("type list refuses", "{'type': ['string', 'null']}", "1", false);
        row("const", "{'const': 'x'}", "'x'", true);
        row("const refuses", "{'const': 'x'}", "'y'", false);
        row("enum number", "{'enum': [1, 2]}", "2", true);
        row("enum refuses", "{'enum': [1, 2]}", "3", false);
        row("enum string", "{'enum': ['tab', 'split']}", "'tab'", true);
        // valijson reads 't\ud800ab' as 'tab'.
        row("enum unpaired surrogate", "{'enum': ['tab', 'split']}", "'t\\ud800ab'", false);
        row("minimum", "{'minimum': 1}", "1", true);
        row("minimum refuses", "{'minimum': 1}", "0.5", false);
        row("maximum", "{'maximum': 1}", "1", true);
        row("maximum large", "{'type': 'integer', 'maximum': 4000000000}", "4000000001", false);
        row("exclusiveMinimum", "{'exclusiveMinimum': 1}", "1", false);
        row("exclusiveMinimum accepts", "{'exclusiveMinimum': 1}", "1.5", true);
        row("exclusiveMaximum", "{'exclusiveMaximum': 1}", "0.5", true);
        row("exclusiveMaximum refuses", "{'exclusiveMaximum': 1}", "1", false);
        row("minLength", "{'type': 'string', 'minLength': 2}", "'a'", false);
        row("minLength accepts", "{'type': 'string', 'minLength': 2}", "'ab'", true);
        row("minLength unpaired surrogate", "{'type': 'string', 'minLength': 1}", "'\\ud800'", false);
        // Characters are code points in both dialects.
        row("maxLength code points", "{'type': 'string', 'maxLength': 5}", "'na\\u00efve'", true);
        row("maxLength refuses", "{'type': 'string', 'maxLength': 4}", "'na\\u00efve'", false);
        // valijson would count 'ab' and pass it.
        row("maxLength unpaired surrogate", "{'type': 'string', 'maxLength': 2}", "'ab\\udc00'", false);
        row("minItems", "{'type': 'array', 'minItems': 1}", "[]", false);
        row("minItems accepts", "{'type': 'array', 'minItems': 1}", "[1]", true);
        row("maxItems", "{'type': 'array', 'maxItems': 1}", "[1]", true);
        row("maxItems refuses", "{'type': 'array', 'maxItems': 1}", "[1, 2]", false);
        row("items", "{'type': 'array', 'items': {'type': 'integer'}}", "[1, 'a']", false);
        row("items accepts", "{'type': 'array', 'items': {'type': 'integer'}}", "[1, 2]", true);
        row("items false", "{'type': 'array', 'items': false}", "[1]", false);
        row("required", "{'type': 'object', 'required': ['a']}", "{}", false);
        row("required accepts", "{'type': 'object', 'required': ['a']}", "{'a': null}", true);
        row("properties", "{'type': 'object', 'properties': {'a': {'type': 'string'}}}", "{'a': 1}", false);
        row("properties accepts", "{'type': 'object', 'properties': {'a': {'type': 'string'}}}", "{'a': 'x', 'b': 1}",
            true);
        row("minProperties", "{'type': 'object', 'minProperties': 1}", "{}", false);
        row("minProperties accepts", "{'type': 'object', 'minProperties': 1}", "{'a': 1}", true);
        row("maxProperties", "{'type': 'object', 'maxProperties': 1}", "{'a': 1, 'b': 2}", false);
        row("maxProperties accepts", "{'type': 'object', 'maxProperties': 1}", "{'a': 1}", true);
        row("member name unpaired surrogate", "{'type': 'object'}", "{'\\ud800': 1}", false);
        row("additionalProperties schema", "{'type': 'object', 'additionalProperties': {'type': 'boolean'}}",
            "{'a': 1}", false);
        row("additionalProperties schema accepts", "{'type': 'object', 'additionalProperties': {'type': 'boolean'}}",
            "{'a': true}", true);
        row("additionalProperties false", "{'type': 'object', 'additionalProperties': false}", "{'a': 1}", false);
        row("additionalProperties false accepts", "{'type': 'object', 'additionalProperties': false}", "{}", true);
        row("propertyNames", "{'type': 'object', 'propertyNames': {'maxLength': 2}}", "{'abc': 1}", false);
        row("propertyNames accepts", "{'type': 'object', 'propertyNames': {'maxLength': 2}}", "{'ab': 1, 'c': 2}",
            true);
        row("propertyNames enum", "{'propertyNames': {'enum': ['x', 'y']}}", "{'x': 1, 'y': 2}", true);
        row("propertyNames enum refuses", "{'propertyNames': {'enum': ['x', 'y']}}", "{'x': 1, 'z': 2}", false);
        // valijson#254 mixed a nested schema of propertyNames with its
        // parent's: not, anyOf and allOf inside it are its own.
        row("propertyNames not", "{'type': 'object', 'propertyNames': {'not': {'const': 'x'}}}", "{'y': 1}", true);
        row("propertyNames not refuses", "{'type': 'object', 'propertyNames': {'not': {'const': 'x'}}}", "{'x': 1}",
            false);
        row("propertyNames anyOf", "{'type': 'object', 'propertyNames': {'anyOf': [{'const': 'a'}, {'maxLength': 1}]}}",
            "{'a': 1, 'b': 2}", true);
        row("propertyNames anyOf refuses",
            "{'type': 'object', 'propertyNames': {'anyOf': [{'const': 'a'}, {'maxLength': 1}]}}", "{'bc': 1}", false);
        row("propertyNames false", "{'propertyNames': false}", "{}", true);
        row("propertyNames false refuses", "{'propertyNames': false}", "{'a': 1}", false);
        row("propertyNames not an object", "{'propertyNames': {'maxLength': 1}}", "'abc'", true);
        row("oneOf both", "{'oneOf': [{'type': 'integer'}, {'type': 'number'}]}", "1", false);
        row("oneOf one", "{'oneOf': [{'type': 'integer'}, {'type': 'number'}]}", "1.5", true);
        row("oneOf none", "{'oneOf': [{'type': 'integer'}, {'type': 'number'}]}", "'1'", false);
        row("anyOf", "{'anyOf': [{'type': 'string'}, {'type': 'integer'}]}", "true", false);
        row("anyOf accepts", "{'anyOf': [{'type': 'string'}, {'type': 'integer'}]}", "1", true);
        row("allOf", "{'allOf': [{'minimum': 1}, {'maximum': 3}]}", "4", false);
        row("allOf accepts", "{'allOf': [{'minimum': 1}, {'maximum': 3}]}", "2", true);
        row("not", "{'not': {'type': 'null'}}", "null", false);
        row("not accepts", "{'not': {'type': 'null'}}", "1", true);
        row("false schema", "false", "1", false);
        row("true schema", "true", "1", true);
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
        Peer peer;
        McpServer server;
        QString error;
        QVERIFY2(server.addTool(McpTool(QStringLiteral("v")).setInputSchema(schema), noop(), &error),
                 qPrintable(error));
        QVERIFY(server.start(peer.serverFd(), peer.serverFd()));
        // The value goes on the wire as written.
        QVERIFY(sendCall(peer, 1, QStringLiteral("v"), "{'v': " + value + "}"));
        const QJsonObject reply = peer.readOne();
        const QByteArray shown = QJsonDocument(reply).toJson();
        QVERIFY2(reply.contains(QLatin1StringView("result")), shown.constData());
        if (accepted) {
            // The handler ran.
            QVERIFY2(!resultOf(reply).contains(QLatin1StringView("isError")), shown.constData());
            QCOMPARE(firstText(reply), QStringLiteral("ok"));
        } else {
            QVERIFY2(resultOf(reply).value(QLatin1StringView("isError")) == QJsonValue(true), shown.constData());
            QVERIFY2(firstText(reply).startsWith(QLatin1StringView("Invalid arguments for tool v:\n- arguments")),
                     shown.constData());
        }
    }

    void propertyNamesWhereTheyApply_data()
    {
        QTest::addColumn<QJsonObject>("schema");
        QTest::addColumn<QByteArray>("arguments");
        // Empty: the handler runs.
        QTest::addColumn<QString>("expected");
        const QJsonObject root = parse("{'type': 'object', 'propertyNames': {'maxLength': 2}}");
        QTest::newRow("root") << root << QByteArray("{'ab': 1}") << QString();
        QTest::newRow("root refuses") << root << QByteArray("{'ab': 1, 'abc': 2}")
                                      << QStringLiteral(
                                             "- arguments/abc: the property name must be at most 2 "
                                             "characters long");
        const QJsonObject chain = parse(
            "{'type': 'object', 'properties': {'a': {'type': 'object', 'properties': "
            "{'b': {'propertyNames': {'enum': ['x']}}}}}}");
        QTest::newRow("chain") << chain << QByteArray("{'a': {'b': {'x': 1}}, 'y': 2}") << QString();
        QTest::newRow("chain refuses") << chain << QByteArray("{'a': {'b': {'x': 1, 'z': 2}}}")
                                       << QStringLiteral(
                                              "- arguments/a/b/z: the property name must be one of the values of enum");
        QTest::newRow("chain not an object") << chain << QByteArray("{'a': {'b': 3}}") << QString();
        QTest::newRow("chain absent") << chain << QByteArray("{'a': {}}") << QString();
        QTest::newRow("false") << parse("{'type': 'object', 'properties': {'a': {'propertyNames': false}}}")
                               << QByteArray("{'a': {'k': 1}}")
                               << QStringLiteral(
                                      "- arguments/a/k: is not allowed, because the schema allows no property here");

        // Afterglow's env.
        QTest::newRow("env") << envSchema() << QByteArray("{'where': 'tab', 'env': {'PATH': '/bin', 'HOME': false}}")
                             << QString();
        QTest::newRow("env empty name") << envSchema() << QByteArray("{'where': 'tab', 'env': {'': 'x'}}")
                                        << QStringLiteral(
                                               "- arguments/env/: the property name must be at least 1 character long");
        QTest::newRow("env long name") << envSchema()
                                       << QByteArray("{'where': 'tab', 'env': {'") + QByteArray(257, 'N') + "': 'x'}}"
                                       << QStringLiteral("- arguments/env/") + QString(61, QLatin1Char('N'))
                + QStringLiteral("...: the property name must be at most 256 characters long");
        QTest::newRow("env wrong value") << envSchema() << QByteArray("{'where': 'tab', 'env': {'A': 5}}")
                                         << QStringLiteral(
                                                "- arguments/env/A: must be of type string\n"
                                                "- arguments/env/A: must be the value of const\n"
                                                "- arguments/env/A: must match at least one schema of anyOf");
        QTest::newRow("env and where") << envSchema() << QByteArray("{'where': 'up', 'env': {'': true}}")
                                       << QStringLiteral(
                                              "- arguments/env/: must be of type string\n"
                                              "- arguments/env/: must be the value of const\n"
                                              "- arguments/env/: must match at least one schema of anyOf\n"
                                              "- arguments/where: must be one of the values of enum\n"
                                              "- arguments/env/: the property name must be at least 1 character long");
    }

    void propertyNamesWhereTheyApply()
    {
        QFETCH(QJsonObject, schema);
        QFETCH(QByteArray, arguments);
        QFETCH(QString, expected);
        Peer peer;
        McpServer server;
        QString error;
        QVERIFY2(server.addTool(McpTool(QStringLiteral("t")).setInputSchema(schema), noop(), &error),
                 qPrintable(error));
        QVERIFY(server.start(peer.serverFd(), peer.serverFd()));
        QVERIFY(sendCall(peer, 1, QStringLiteral("t"), arguments));
        const QJsonObject reply = peer.readOne();
        if (expected.isEmpty()) {
            QCOMPARE(firstText(reply), QStringLiteral("ok"));
        } else {
            QCOMPARE(resultOf(reply).value(QLatin1StringView("isError")), QJsonValue(true));
            QCOMPARE(errorLines(reply), expected);
        }
    }

    void errorsAreListedOnce()
    {
        // Two branches of anyOf that fail alike: one line.
        Peer peer;
        McpServer server;
        QVERIFY(server.addTool(
            McpTool(QStringLiteral("t"))
                .setInputSchema(parse("{'type': 'object', 'properties': {'v': {'anyOf': [{'type': 'string'}, "
                                      "{'type': 'string', 'minLength': 1}]}}}")),
            noop()));
        QVERIFY(server.start(peer.serverFd(), peer.serverFd()));
        QVERIFY(sendCall(peer, 1, QStringLiteral("t"), "{'v': 1}"));
        QCOMPARE(errorLines(peer.readOne()),
                 QStringLiteral("- arguments/v: must be of type string\n"
                                "- arguments/v: must match at least one schema of anyOf"));
    }

    void requiredClientCapabilities()
    {
        Peer peer;
        McpServer server;
        const QJsonObject required = parse("{'elicitation': {'form': {}}, 'roots': {}}");
        QVERIFY(server.addTool(McpTool(QStringLiteral("asks")).setRequiredClientCapabilities(required), noop()));
        QVERIFY(server.addTool(McpTool(QStringLiteral("plain")), noop()));
        QVERIFY(server.start(peer.serverFd(), peer.serverFd()));

        // None declared, one missing, the nested one missing, and ones
        // declared as something else than an object: refused.
        for (const char* declared :
             {"{}", "{'elicitation': {'form': {}}}", "{'elicitation': {}, 'roots': {}}",
              "{'elicitation': {'form': {}}, 'roots': false}", "{'elicitation': {'form': {}}, 'roots': null}",
              "{'elicitation': {'form': {}}, 'roots': 'yes'}", "{'elicitation': {'form': true}, 'roots': {}}",
              "{'elicitation': [], 'roots': {}}"}) {
            QVERIFY(peer.send(callRequest(1, QStringLiteral("asks"), QJsonObject(),
                                          meta(QString(ProtocolVersion), parse(declared)))));
            const QJsonObject reply = peer.readOne();
            QVERIFY2(errorCode(reply) == ErrorCode::MissingRequiredClientCapability, declared);
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
        QVERIFY(QJsonDocument(reply).toJson().contains("structuredContent/count: must be of type integer"));

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
