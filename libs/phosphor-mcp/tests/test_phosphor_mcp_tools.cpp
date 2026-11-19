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

// Why pattern and patternProperties are refused, after the keyword.
QString patternReason()
{
    return QStringLiteral(
        " is not supported, because the server cannot match regular expressions safely. Use \"enum\", or check the "
        "value in the handler");
}

const QString CountRefusal = QStringLiteral(": must be an integer from 0 to 9007199254740991");
const QString TooLargeToList =
    QStringLiteral("- arguments: does not match the schema, and is too large for the server to list each error");
const QString TooLargeToCheck = QStringLiteral("- arguments: is too large for the server to check against the schema");

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

        // The keywords the server cannot check as 2020-12 says, at the root
        // and nested, each with its reason.
        const QList<std::pair<const char*, QString>> refused{
            {"pattern", patternReason()},
            {"patternProperties", patternReason()},
            {"uniqueItems",
             QStringLiteral(" is not supported, because the server would compare every pair of items. Check for "
                            "repeated items in the handler")},
            {"dependentSchemas",
             QStringLiteral(" is not supported, because the server's validator reports its errors at the wrong "
                            "place. Use \"if\" with \"required\", and \"then\"")},
            {"$id",
             QStringLiteral(" is not supported, because the server resolves references within the tool's "
                            "schema only")},
            {"$dynamicRef",
             QStringLiteral(" is not supported, because the server does not resolve dynamic references. Use \"$ref\"")},
            {"$dynamicAnchor",
             QStringLiteral(" is not supported, because the server does not resolve dynamic references. Use \"$ref\"")},
            {"$vocabulary", QStringLiteral(" is not supported, because it belongs in a meta-schema")},
            {"definitions", QStringLiteral(" is not a keyword of JSON Schema 2020-12. Use \"$defs\"")},
            {"dependencies", QStringLiteral(" is not a keyword of JSON Schema 2020-12. Use \"dependentRequired\"")},
            {"additionalItems",
             QStringLiteral(" is not a keyword of JSON Schema 2020-12. Use \"items\" beside \"prefixItems\"")},
            {"$recursiveRef", QStringLiteral(" is not a keyword of JSON Schema 2020-12. Use \"$ref\"")},
            {"$recursiveAnchor", QStringLiteral(" is not a keyword of JSON Schema 2020-12")},
            {"contentSchema", QStringLiteral(" is not supported in a tool schema")},
            {"colour", QStringLiteral(" is not supported in a tool schema")},
        };
        for (const auto& [keyword, reason] : refused) {
            const QString name = QString::fromLatin1(keyword);
            const QString quotedName = QLatin1Char('"') + name + QLatin1Char('"');
            QTest::addRow("root %s", keyword)
                << QByteArray("{'type': 'object', '") + keyword + "': {}}"
                << QLatin1Char('/') + name + QStringLiteral(": ") + quotedName + reason << true;
            QTest::addRow("nested %s", keyword)
                << QByteArray("{'type': 'object', 'properties': {'a': {'type': 'object', '") + keyword + "': {}}}}"
                << QStringLiteral("/properties/a/") + name + QStringLiteral(": ") + quotedName + reason << true;
        }
        // Below every kind of subschema.
        row("pattern deep in items",
            "{'type': 'object', 'properties': {'a': {'type': 'array', 'items': {'pattern': 'x'}}}}",
            QStringLiteral("/properties/a/items/pattern: \"pattern\"") + patternReason());
        row("pattern in propertyNames", "{'type': 'object', 'propertyNames': {'pattern': 'x'}}",
            QStringLiteral("/propertyNames/pattern: \"pattern\"") + patternReason());
        row("$id deep in anyOf", "{'type': 'object', 'anyOf': [{'properties': {'b': {'$id': 'x'}}}]}",
            QStringLiteral("/anyOf/0/properties/b/$id: \"$id\" is not supported, because the server resolves "
                           "references within the tool's schema only"));
        row("uniqueItems in $defs", "{'type': 'object', '$defs': {'d': {'uniqueItems': true}}}",
            QStringLiteral("/$defs/d/uniqueItems: \"uniqueItems\" is not supported, because the server would "
                           "compare every pair of items. Check for repeated items in the handler"));
        row("colour in prefixItems", "{'type': 'object', 'prefixItems': [true, {'colour': 1}]}",
            QStringLiteral("/prefixItems/1/colour: \"colour\" is not supported in a tool schema"));
        row("colour in then", "{'type': 'object', 'if': true, 'then': {'colour': 1}}",
            QStringLiteral("/then/colour: \"colour\" is not supported in a tool schema"));
        const QString rootType = QStringLiteral("/type: the root of a tool schema must have \"type\": \"object\"");
        row("root without type", "{'properties': {}}", rootType, false);
        row("root an array", "{'type': 'array'}", rootType, false);
        row("root type list", "{'type': ['object', 'null']}", rootType, false);

        // MCP clients read 2020-12, and need not read another dialect.
        const QString dialect = QStringLiteral(
            "/$schema: the schema must be JSON Schema 2020-12, the dialect that MCP clients read, so \"$schema\" may "
            "name https://json-schema.org/draft/2020-12/schema only");
        row("$schema draft-07", "{'type': 'object', '$schema': 'http://json-schema.org/draft-07/schema#'}", dialect);
        row("$schema 2019-09", "{'type': 'object', '$schema': 'https://json-schema.org/draft/2019-09/schema'}",
            dialect);
        row("$schema elsewhere", "{'type': 'object', '$schema': 'https://example.com/schema'}", dialect);
        row("$schema not a string", "{'type': 'object', '$schema': 2020}", dialect);
        row("$schema nested",
            "{'type': 'object', 'properties': {'a': {'$schema': 'https://json-schema.org/draft/2020-12/schema'}}}",
            QStringLiteral("/properties/a/$schema: \"$schema\" is supported at the root of the schema only"));

        // References stay inside the schema, and lead to a schema.
        row("$ref remote", "{'type': 'object', 'properties': {'a': {'$ref': 'https://example.com/a.json'}}}",
            QStringLiteral("/properties/a/$ref: \"$ref\" may refer only to a place in the same schema (\"#...\"), "
                           "because the server fetches no other schema"));
        row("$ref relative", "{'type': 'object', 'properties': {'a': {'$ref': 'a.json#/x'}}}",
            QStringLiteral("/properties/a/$ref: \"$ref\" may refer only to a place in the same schema (\"#...\"), "
                           "because the server fetches no other schema"));
        row("$ref to the meta-schema",
            "{'type': 'object', 'properties': {'a': {'$ref': 'https://json-schema.org/draft/2020-12/schema'}}}",
            QStringLiteral("/properties/a/$ref: \"$ref\" may refer only to a place in the same schema (\"#...\"), "
                           "because the server fetches no other schema"));
        row("$ref not a string", "{'type': 'object', '$ref': 1}", QStringLiteral("/$ref: must be a string"));
        row("$ref with a space",
            "{'type': 'object', '$defs': {'a b': {}}, 'properties': {'a': {'$ref': '#/$defs/a b'}}}",
            QStringLiteral("/properties/a/$ref: is not a valid URI fragment. Percent-encode the characters that a URI "
                           "cannot hold"));
        row("$ref with a broken escape", "{'type': 'object', 'properties': {'a': {'$ref': '#/$defs/%zz'}}}",
            QStringLiteral("/properties/a/$ref: is not a valid URI fragment. Percent-encode the characters that a URI "
                           "cannot hold"));
        row("$ref to nothing", "{'type': 'object', 'properties': {'a': {'$ref': '#/$defs/x'}}}",
            QStringLiteral("/properties/a/$ref: refers to no schema in the tool's schema"));
        row("$ref to a keyword's value",
            "{'type': 'object', 'properties': {'a': {'$ref': '#/required'}}, 'required': []}",
            QStringLiteral("/properties/a/$ref: refers to no schema in the tool's schema"));
        row("$ref to an unknown anchor", "{'type': 'object', 'properties': {'a': {'$ref': '#nowhere'}}}",
            QStringLiteral("/properties/a/$ref: refers to no schema in the tool's schema"));
        row("$ref to a boolean schema",
            "{'type': 'object', '$defs': {'f': false}, 'properties': {'a': {'$ref': '#/$defs/f'}}}",
            QStringLiteral("/properties/a/$ref: \"$ref\" to a boolean schema is not supported. Use {} for true, or "
                           "{\"not\": {}} for false"));
        row("$ref into items beside prefixItems",
            "{'type': 'object', 'prefixItems': [{}], 'items': {'type': 'string'}, 'properties': {'a': {'$ref': "
            "'#/items'}}}",
            QStringLiteral("/properties/a/$ref: \"$ref\" into \"items\" beside \"prefixItems\" is not supported, "
                           "because the server's validator files that schema under another name. Move it to "
                           "\"$defs\""));
        row("$anchor starting with _", "{'type': 'object', '$defs': {'a': {'$anchor': '_a'}}}",
            QStringLiteral("/$defs/a/$anchor: must be a name of letters, digits, \"-\", \"_\" and \".\" that starts "
                           "with a letter"));
        row("$anchor twice", "{'type': 'object', '$defs': {'a': {'$anchor': 'x'}, 'b': {'$anchor': 'x'}}}",
            QStringLiteral("/$defs/b/$anchor: another schema has this $anchor"));
        row("$defs an array", "{'type': 'object', '$defs': []}",
            QStringLiteral("/$defs: must be an object of schemas"));

        // A $ref that comes back to its schema on the same value recurses
        // without end (jsoncons overflows the stack).
        const auto loop = [](const QString& pointer) {
            return pointer
                + QStringLiteral(
                       ": \"$ref\" leads back to the same schema without descending into the value, so "
                       "checking it would never end");
        };
        row("$ref to itself", "{'type': 'object', '$ref': '#'}", loop(QStringLiteral("/$ref")));
        row("$ref to its parent", "{'type': 'object', 'allOf': [{'$ref': '#'}]}",
            loop(QStringLiteral("/allOf/0/$ref")));
        row("$ref loop in $defs",
            "{'type': 'object', '$defs': {'a': {'$ref': '#/$defs/b'}, 'b': {'anyOf': [{'$ref': '#/$defs/a'}]}}}",
            loop(QStringLiteral("/$defs/a/$ref")));
        row("$ref loop through not", "{'type': 'object', '$defs': {'a': {'not': {'$ref': '#/$defs/a'}}}}",
            loop(QStringLiteral("/$defs/a/not/$ref")));
        row("$ref loop through if and else",
            "{'type': 'object', '$defs': {'a': {'if': {'$ref': '#a2'}, 'then': true}, 'b': {'$anchor': 'a2', 'else': "
            "{'$ref': '#/$defs/a'}, 'if': true}}}",
            loop(QStringLiteral("/$defs/a/if/$ref")));
        row("$ref loop through an anchor", "{'type': 'object', '$anchor': 'top', 'oneOf': [true, {'$ref': '#top'}]}",
            loop(QStringLiteral("/oneOf/1/$ref")));

        // unevaluatedProperties and unevaluatedItems where jsoncons counts
        // what 2020-12 does not.
        row("unevaluatedProperties a schema", "{'type': 'object', 'unevaluatedProperties': {'type': 'string'}}",
            QStringLiteral("/unevaluatedProperties: \"unevaluatedProperties\" is supported as true or false only, "
                           "because the server's validator checks its schema at the wrong place"));
        row("unevaluatedProperties beside not",
            "{'type': 'object', 'not': {'required': ['a']}, 'unevaluatedProperties': false}",
            QStringLiteral("/unevaluatedProperties: \"unevaluatedProperties\" is not supported beside a \"not\" that "
                           "applies to the same value, because the server's validator counts what the subschema of not "
                           "evaluated"));
        row("unevaluatedItems beside a not through $ref",
            "{'type': 'object', 'properties': {'a': {'allOf': [{'$ref': '#/$defs/n'}], 'unevaluatedItems': false}}, "
            "'$defs': {'n': {'not': {'type': 'string'}}}}",
            QStringLiteral(
                "/properties/a/unevaluatedItems: \"unevaluatedItems\" is not supported beside a \"not\" "
                "that applies to the same value, because the server's validator counts what the subschema of "
                "not evaluated"));
        row("unevaluatedItems beside contains",
            "{'type': 'object', 'properties': {'a': {'contains': {}, 'unevaluatedItems': false}}}",
            QStringLiteral(
                "/properties/a/unevaluatedItems: \"unevaluatedItems\" is not supported beside a \"contains\" "
                "that applies to the same value, because the server's validator counts the items of nested "
                "arrays that contains matched"));

        // Names the server cannot hold in UTF-8.
        row("property name with a surrogate", "{'type': 'object', 'properties': {'a\\ud800': {}}}",
            QStringLiteral("/properties/a") + QChar(0xd800)
                + QStringLiteral(": a property name with an unpaired UTF-16 surrogate is not supported"));
        row("required name with a surrogate", "{'type': 'object', 'required': ['a', 'b\\udc00']}",
            QStringLiteral("/required/1: a property name with an unpaired UTF-16 surrogate is not supported"));
        row("enum string with a surrogate", "{'type': 'object', 'properties': {'a': {'enum': ['t\\ud800ab']}}}",
            QStringLiteral("/properties/a/enum/0: a string with an unpaired UTF-16 surrogate is not supported"));
        row("dependentRequired name with a surrogate", "{'type': 'object', 'dependentRequired': {'a': ['\\ud800']}}",
            QStringLiteral(
                "/dependentRequired/a/0: a property name with an unpaired UTF-16 surrogate is not supported"));

        // The value of each keyword.
        row("required not an array", "{'type': 'object', 'required': 'a'}",
            QStringLiteral("/required: must be an array of distinct property names"));
        row("required not names", "{'type': 'object', 'required': ['a', 1]}",
            QStringLiteral("/required: must be an array of distinct property names"));
        row("required twice", "{'type': 'object', 'required': ['a', 'a']}",
            QStringLiteral("/required: must be an array of distinct property names"));
        row("required twice nested", "{'type': 'object', 'properties': {'a': {'required': ['x', 'y', 'x']}}}",
            QStringLiteral("/properties/a/required: must be an array of distinct property names"));
        row("dependentRequired an array", "{'type': 'object', 'dependentRequired': []}",
            QStringLiteral("/dependentRequired: must be an object of arrays of distinct property names"));
        row("dependentRequired twice", "{'type': 'object', 'dependentRequired': {'a': ['b', 'b']}}",
            QStringLiteral("/dependentRequired/a: must be an array of distinct property names"));
        row("schema a number", "{'type': 'object', 'properties': {'a': 3}}",
            QStringLiteral("/properties/a: a schema must be an object or a boolean"));
        row("additionalProperties a number", "{'type': 'object', 'additionalProperties': 3}",
            QStringLiteral("/additionalProperties: a schema must be an object or a boolean"));
        row("contains a string", "{'type': 'object', 'contains': 'x'}",
            QStringLiteral("/contains: a schema must be an object or a boolean"));
        row("properties an array", "{'type': 'object', 'properties': []}",
            QStringLiteral("/properties: must be an object of schemas"));
        row("empty anyOf", "{'type': 'object', 'anyOf': []}",
            QStringLiteral("/anyOf: must be a non-empty array of schemas"));
        row("oneOf an object", "{'type': 'object', 'oneOf': {}}",
            QStringLiteral("/oneOf: must be a non-empty array of schemas"));
        row("empty prefixItems", "{'type': 'object', 'prefixItems': []}",
            QStringLiteral("/prefixItems: must be a non-empty array of schemas"));
        row("items as an array",
            "{'type': 'object', 'properties': {'a': {'type': 'array', 'items': [{'type': 'string'}]}}}",
            QStringLiteral("/properties/a/items: must be a schema. A list of schemas for the first items is "
                           "\"prefixItems\" in JSON Schema 2020-12"));
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
        row("multipleOf 0", "{'type': 'object', 'properties': {'a': {'multipleOf': 0}}}",
            QStringLiteral("/properties/a/multipleOf: must be a number greater than 0"));
        row("multipleOf negative", "{'type': 'object', 'properties': {'a': {'multipleOf': -2}}}",
            QStringLiteral("/properties/a/multipleOf: must be a number greater than 0"));
        // jsoncons compares an argument, a qint64, with a double as doubles,
        // and the largest qint64 values round to 2^63.
        const QString twoTo63 = QStringLiteral(
            ": the number 2^63 is not supported, because the server would take the largest 64-bit integers for it");
        row("maximum 2^63", "{'type': 'object', 'properties': {'a': {'maximum': 9223372036854775808}}}",
            QStringLiteral("/properties/a/maximum") + twoTo63);
        row("enum 2^63", "{'type': 'object', 'properties': {'a': {'enum': [1, 9223372036854775808]}}}",
            QStringLiteral("/properties/a/enum/1") + twoTo63);
        row("const 2^63 deep", "{'type': 'object', 'properties': {'a': {'const': {'x': [9223372036854775808]}}}}",
            QStringLiteral("/properties/a/const/x/0") + twoTo63);
        row("maxLength a string", "{'type': 'object', 'properties': {'a': {'maxLength': '3'}}}",
            QStringLiteral("/properties/a/maxLength") + CountRefusal);
        row("maxLength negative", "{'type': 'object', 'properties': {'a': {'maxLength': -1}}}",
            QStringLiteral("/properties/a/maxLength") + CountRefusal);
        row("maxLength fractional", "{'type': 'object', 'properties': {'a': {'maxLength': 1.5}}}",
            QStringLiteral("/properties/a/maxLength") + CountRefusal);
        row("maxLength 1e300", "{'type': 'object', 'properties': {'a': {'maxLength': 1e300}}}",
            QStringLiteral("/properties/a/maxLength") + CountRefusal);
        row("minItems 2^53", "{'type': 'object', 'properties': {'a': {'minItems': 9007199254740992}}}",
            QStringLiteral("/properties/a/minItems") + CountRefusal);
        row("minContains a string", "{'type': 'object', 'properties': {'a': {'contains': {}, 'minContains': '1'}}}",
            QStringLiteral("/properties/a/minContains") + CountRefusal);
        row("minProperties a boolean", "{'type': 'object', 'minProperties': true}",
            QStringLiteral("/minProperties") + CountRefusal);
        row("description a number", "{'type': 'object', 'properties': {'a': {'description': 3}}}",
            QStringLiteral("/properties/a/description: must be a string"));
        row("title an object", "{'type': 'object', 'title': {}}", QStringLiteral("/title: must be a string"));
        row("$comment a number", "{'type': 'object', '$comment': 1}", QStringLiteral("/$comment: must be a string"));
        row("format a number", "{'type': 'object', 'properties': {'a': {'format': 1}}}",
            QStringLiteral("/properties/a/format: must be a string"));
        row("contentMediaType a number", "{'type': 'object', 'contentMediaType': 1}",
            QStringLiteral("/contentMediaType: must be a string"));
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
        // Every keyword the server checks, names and values that look like
        // refused keywords and are neither, references of each form, and a
        // schema that recurses through properties.
        const QJsonObject schema = parse(
            "{ "
            "'$schema': 'https://json-schema.org/draft/2020-12/schema', "
            "'type': 'object', "
            "'title': 't', 'description': 'd', '$comment': 'c', "
            "'propertyNames': {'maxLength': 20}, "
            "'$defs': {'name': {'$anchor': 'name', 'type': 'string', 'minLength': 1}, "
            "'a b': {'type': 'integer'}, 'node': {'type': 'object', 'properties': {'next': {'$ref': "
            "'#/$defs/node'}}}}, "
            "'properties': { "
            "'$ref': {'type': 'string', 'enum': ['$ref', '$defs'], 'default': '$ref'}, "
            "'items': {'type': 'array', 'items': {'type': ['string', 'null']}, 'minItems': 0}, "
            "'tuple': {'type': 'array', 'prefixItems': [{'type': 'string'}, {'$ref': '#name'}], 'items': false, "
            "'contains': {'const': 'x'}, 'minContains': 1, 'maxContains': 2}, "
            "'rest': {'prefixItems': [true], 'unevaluatedItems': {'type': 'integer'}}, "
            "'n': {'type': 'number', 'exclusiveMinimum': 0, 'exclusiveMaximum': 10, 'multipleOf': 0.5}, "
            "'big': {'type': 'integer', 'minimum': -9223372036854775808, 'maximum': 9223372036854775807, "
            "'enum': [9007199254740993, 1]}, "
            "'huge': {'type': 'number', 'maximum': 1e300, 'enum': [1e300, 4294967295]}, "
            "'c': {'const': {'$schema': 'x', 'pattern': 1}, 'examples': [{'$id': 1}]}, "
            "'s': {'type': 'string', 'minLength': 1, 'readOnly': true, 'writeOnly': false, 'deprecated': false, "
            "'format': 'email', 'contentMediaType': 'text/plain', 'contentEncoding': 'base64'}, "
            "'pattern': {'type': 'string'}, 'patternProperties': {'type': 'object'}, "
            "'uniqueItems': {'type': 'boolean'}, 'a/b~c': {'$ref': '#/$defs/a%20b'}, "
            "'o': {'type': 'object', 'minProperties': 1, 'maxProperties': 2, "
            "'propertyNames': {'maxLength': 3}, "
            "'properties': {'deep': {'propertyNames': false}}, "
            "'additionalProperties': {'type': 'boolean', 'propertyNames': {'enum': ['x']}}}, "
            "'list': {'items': {'propertyNames': {'not': {'const': 'x'}}}}, "
            "'t': {'propertyNames': true}, "
            "'u': {'oneOf': [{'type': 'string'}, {'type': 'integer'}]}, "
            "'v': {'allOf': [true, {'not': {'type': 'null'}}]}, "
            "'w': {'if': {'required': ['a']}, 'then': {'required': ['b']}, 'else': {'maxProperties': 0}}, "
            "'tree': {'$ref': '#/$defs/node'}, "
            "'closed': {'allOf': [{'properties': {'a': true}}], 'unevaluatedProperties': false} "
            "}, "
            "'required': ['$ref', 'a/b~c'], "
            "'dependentRequired': {'tuple': ['rest'], 'rest': []}, "
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
        // The $schema of 2020-12 with an empty fragment, as some generators
        // write it.
        QVERIFY2(
            server.addTool(McpTool(QStringLiteral("hash"))
                               .setInputSchema(parse("{'$schema': 'https://json-schema.org/draft/2020-12/schema#', "
                                                     "'type': 'object'}")),
                           noop(), &error),
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
        // Every line after the first, exactly: no English of jsoncons's
        // ("Expected integer, found string"), and each error once.
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
        // Each one is listed.
        row("two extra properties", "{'pane': 1, 'colour': 'red', 'size': 2}",
            "- arguments/colour: is not a known property\n- arguments/size: is not a known property");
        // A false subschema fails without a reason of its own, which stays
        // even when another property has an error of its own.
        row("false subschema", "{'pane': 1, 'gone': 1}", "- arguments/gone: does not match the schema");
        row("false subschema and another error", "{'pane': 0, 'gone': 1}",
            "- arguments/gone: does not match the schema\n- arguments/pane: must be at least 1");
        row("several", "{'text': 'toolong', 'tags': [1], 'colour': 'red'}",
            "- arguments/tags/0: must be of type string\n- arguments/text: must be at most 5 characters long\n"
            "- arguments/colour: is not a known property\n- arguments/pane: is required");
        // What jsoncons would read differently from the handler, refused
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
        // The errors of large arguments are listed while they are few, and
        // the check stops once there are too many to list. A check that would
        // cost too much is not made at all.
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
        // Each answer takes well under a second, about ten under valgrind.
        // valijson took 152 s for the first with every error collected, and
        // jsoncons would collect 150,000 errors of the anyOf (about 200 MB).
        const int bound = 30000;
        const QJsonObject reply = peer.readOne(bound);
        QVERIFY2(timer.elapsed() < bound, qPrintable(QString::number(timer.elapsed())));
        QCOMPARE(errorLines(reply), expected);
        QCOMPARE(calls, 0);
    }

    void hugeArgumentsAnswerFast_data()
    {
        QTest::addColumn<QByteArray>("arguments");
        QTest::addColumn<QString>("expected");
        const auto repeat = [](const QByteArray& item, int count) {
            QByteArray list;
            for (int i = 0; i < count; ++i) {
                list += (i ? "," : "") + item;
            }
            return list;
        };
        const auto variables = [](int count) {
            QByteArray env;
            for (int i = 0; i < count; ++i) {
                env += (i ? ",'V" : "'V") + QByteArray::number(i) + "':" + QByteArray::number(i);
            }
            return env;
        };

        // Each wrong twice (anyOf), and maxProperties.
        QTest::newRow("50000 wrong variables")
            << "{'where': 'tab', 'env': {" + variables(50000) + "}}" << TooLargeToCheck;
        QTest::newRow("5000 wrong variables") << "{'where': 'tab', 'env': {" + variables(5000) + "}}" << TooLargeToList;
        // One wrong value, however large.
        QTest::newRow("400000 items for a string") << "{'where': [" + repeat("1", 400000) + "]}"
                                                   << QStringLiteral(
                                                          "- arguments/where: must be one of the values of enum\n"
                                                          "- arguments/where: must be of type string");
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

    void combinatorSchemasAnswerFast()
    {
        // jsoncons collects every error inside an allOf, anyOf or oneOf, and
        // copies them into the combinator's own error, again for each one
        // around it. So the schema counts as well as the size of the
        // arguments, and a value that would make too many errors there is
        // not checked.
        QFETCH(QJsonObject, schema);
        QFETCH(QJsonObject, arguments);
        QFETCH(QString, expected);
        Peer peer;
        McpServer server;
        int calls = 0;
        QString error;
        QVERIFY2(server.addTool(
                     McpTool(QStringLiteral("heavy")).setInputSchema(schema),
                     [&](McpCall call) {
                         ++calls;
                         call.finish(McpResult::text(QStringLiteral("ran")));
                     },
                     &error),
                 qPrintable(error));
        QVERIFY(server.start(peer.serverFd(), peer.serverFd()));
        QElapsedTimer timer;
        timer.start();
        QVERIFY(peer.send(callRequest(1, QStringLiteral("heavy"), arguments)));
        // Each answer takes milliseconds. valijson took from seconds to
        // minutes with every error listed (44 s for the first in a release
        // build), and jsoncons would copy gigabytes of errors for the third.
        const int bound = 30000;
        const QJsonObject reply = peer.readOne(bound);
        QVERIFY2(timer.elapsed() < bound, qPrintable(QString::number(timer.elapsed())));
        QCOMPARE(errorLines(reply), expected);
        QCOMPARE(calls, 0);
    }

    void combinatorSchemasAnswerFast_data()
    {
        QTest::addColumn<QJsonObject>("schema");
        QTest::addColumn<QJsonObject>("arguments");
        QTest::addColumn<QString>("expected");
        const auto object = [](const QString& name, const QJsonObject& property) {
            return QJsonObject{{QLatin1StringView("type"), QStringLiteral("object")},
                               {QLatin1StringView("properties"), QJsonObject{{name, property}}}};
        };
        const auto items = [](const QJsonValue& item, int count) {
            QJsonArray array;
            for (int i = 0; i < count; ++i) {
                array.append(item);
            }
            return array;
        };

        // A oneOf of 15 arrays, each with items that are an anyOf of 15
        // objects, and items that match none of them.
        QJsonArray objects;
        QJsonArray arrays;
        for (int i = 0; i < 15; ++i) {
            QJsonObject inner = object(QStringLiteral("x"), QJsonObject{{QLatin1StringView("maximum"), i}});
            inner.insert(QLatin1StringView("required"), QJsonArray{QStringLiteral("y%1").arg(i)});
            objects.append(inner);
        }
        for (int i = 0; i < 15; ++i) {
            arrays.append(
                QJsonObject{{QLatin1StringView("type"), QStringLiteral("array")},
                            {QLatin1StringView("maxItems"), i},
                            {QLatin1StringView("items"), QJsonObject{{QLatin1StringView("anyOf"), objects}}}});
        }
        const QJsonObject nested = object(QStringLiteral("v"), QJsonObject{{QLatin1StringView("oneOf"), arrays}});
        const QJsonObject wrongItem{{QLatin1StringView("x"), 1000}};
        QTest::newRow("oneOf of anyOf, 400 items")
            << nested << QJsonObject{{QLatin1StringView("v"), items(wrongItem, 400)}} << TooLargeToCheck;
        // The same schema lists the errors of a small value.
        QTest::newRow("oneOf of anyOf, 1 item")
            << nested << QJsonObject{{QLatin1StringView("v"), items(wrongItem, 1)}}
            << QStringLiteral(
                   "- arguments/v/0/x: must be at most 0\n- arguments/v/0/y0: is required\n"
                   "- arguments/v/0/x: must be at most 1\n- arguments/v/0/y1: is required\n"
                   "- arguments/v/0/x: must be at most 2\n- arguments/v/0/y2: is required\n"
                   "- arguments/v/0/x: must be at most 3\n- arguments/v/0/y3: is required\n"
                   "- arguments/v/0/x: must be at most 4\n- arguments/v/0/y4: is required\n"
                   "- arguments: and 23 more errors");

        // anyOf in anyOf, 50 deep, for each of 990 items, and for 1.
        QJsonObject chain{{QLatin1StringView("type"), QStringLiteral("integer")}, {QLatin1StringView("maximum"), 0}};
        for (int i = 0; i < 50; ++i) {
            const QJsonObject other{{QLatin1StringView("type"), QStringLiteral("string")},
                                    {QLatin1StringView("maxLength"), 0}};
            chain = QJsonObject{{QLatin1StringView("anyOf"), QJsonArray{chain, other}}};
        }
        const QJsonObject deep = object(
            QStringLiteral("v"),
            QJsonObject{{QLatin1StringView("type"), QStringLiteral("array")}, {QLatin1StringView("items"), chain}});
        QTest::newRow("anyOf 50 deep, 990 items")
            << deep << QJsonObject{{QLatin1StringView("v"), items(5, 990)}} << TooLargeToCheck;
        QTest::newRow("anyOf 50 deep, 1 item") << deep << QJsonObject{{QLatin1StringView("v"), items(5, 1)}}
                                               << QStringLiteral(
                                                      "- arguments/v/0: must be at most 0\n"
                                                      "- arguments/v/0: must be of type string\n"
                                                      "- arguments/v/0: must match at least one schema of anyOf");

        // An allOf of 200 anyOfs for each of 990 items, and for 10, with too
        // many errors to list.
        QJsonArray all;
        for (int i = 0; i < 200; ++i) {
            all.append(QJsonObject{{QLatin1StringView("anyOf"),
                                    QJsonArray{QJsonObject{{QLatin1StringView("maximum"), i}},
                                               QJsonObject{{QLatin1StringView("type"), QStringLiteral("string")}}}}});
        }
        const QJsonObject wide =
            object(QStringLiteral("v"),
                   QJsonObject{{QLatin1StringView("type"), QStringLiteral("array")},
                               {QLatin1StringView("items"), QJsonObject{{QLatin1StringView("allOf"), all}}}});
        QTest::newRow("allOf of 200 anyOfs, 990 items")
            << wide << QJsonObject{{QLatin1StringView("v"), items(1000, 990)}} << TooLargeToCheck;
        QTest::newRow("allOf of 200 anyOfs, 10 items")
            << wide << QJsonObject{{QLatin1StringView("v"), items(1000, 10)}} << TooLargeToList;
    }

    void longStringsAnswerFast()
    {
        // Each string of the arguments is converted to UTF-8 once, however
        // many values of an enum it is compared with. jsoncons counts the
        // characters of a string for each minLength and maxLength, so the
        // length of the strings counts in whether the check is made.
        QFETCH(QJsonObject, schema);
        QFETCH(QJsonObject, arguments);
        QFETCH(QString, expected);
        Peer peer;
        McpServer server;
        int calls = 0;
        QString error;
        QVERIFY2(server.addTool(
                     McpTool(QStringLiteral("long")).setInputSchema(schema),
                     [&](McpCall call) {
                         ++calls;
                         call.finish(McpResult::text(QStringLiteral("ran")));
                     },
                     &error),
                 qPrintable(error));
        QVERIFY(server.start(peer.serverFd(), peer.serverFd()));
        QElapsedTimer timer;
        timer.start();
        QVERIFY(peer.send(callRequest(1, QStringLiteral("long"), arguments)));
        // Each answer takes well under a second, in a debug build too. With
        // the string converted for each comparison the first took 81 s in a
        // debug build.
        const int bound = 30000;
        const QJsonObject reply = peer.readOne(bound);
        QVERIFY2(timer.elapsed() < bound, qPrintable(QString::number(timer.elapsed())));
        QCOMPARE(errorLines(reply), expected);
        QCOMPARE(calls, 0);
    }

    void longStringsAnswerFast_data()
    {
        QTest::addColumn<QJsonObject>("schema");
        QTest::addColumn<QJsonObject>("arguments");
        QTest::addColumn<QString>("expected");
        const auto object = [](const QJsonObject& property) {
            return QJsonObject{{QLatin1StringView("type"), QStringLiteral("object")},
                               {QLatin1StringView("properties"), QJsonObject{{QLatin1StringView("a"), property}}}};
        };
        const auto arrayOf = [](const QJsonObject& item) {
            return QJsonObject{{QLatin1StringView("type"), QStringLiteral("array")},
                               {QLatin1StringView("items"), item}};
        };
        // U+4E2D, three bytes in UTF-8.
        const QChar cjk(0x4E2D);

        // 15 MB of UTF-8 against each of 3000 values.
        QJsonArray values;
        for (int i = 0; i < 3000; ++i) {
            values.append(QStringLiteral("v%1").arg(i));
        }
        QTest::newRow("CJK string against an enum")
            << object(QJsonObject{{QLatin1StringView("enum"), values}})
            << QJsonObject{{QLatin1StringView("a"), QString(5000000, cjk)}}
            << QStringLiteral("- arguments/a: must be one of the values of enum");

        // 7.2 MB of UTF-8 counted for each of 35 maxLength.
        QJsonArray lengths;
        for (int i = 1; i <= 35; ++i) {
            lengths.append(QJsonObject{{QLatin1StringView("maxLength"), i}});
        }
        QJsonArray strings;
        for (int i = 0; i < 24; ++i) {
            strings.append(QString(100000, cjk));
        }
        QTest::newRow("CJK strings against maxLength in an anyOf")
            << object(arrayOf(QJsonObject{{QLatin1StringView("anyOf"), lengths}}))
            << QJsonObject{{QLatin1StringView("a"), strings}} << TooLargeToCheck;

        // Each error that says a property is missing names it in full in its
        // place, which the server keeps to tell it from the others.
        QJsonArray names;
        for (int i = 0; i < 10; ++i) {
            names.append(QString(QStringLiteral("r%1").arg(i) + QString(65536, QLatin1Char('x'))));
        }
        QJsonArray empties;
        for (int i = 0; i < 100; ++i) {
            empties.append(QJsonObject());
        }
        QTest::newRow("long required names") << object(arrayOf(
            QJsonObject{{QLatin1StringView("type"), QStringLiteral("object")}, {QLatin1StringView("required"), names}}))
                                             << QJsonObject{{QLatin1StringView("a"), empties}} << TooLargeToList;
        // The same schema lists the errors of a small value.
        QTest::newRow("long required names, 1 item")
            << object(arrayOf(QJsonObject{{QLatin1StringView("type"), QStringLiteral("object")},
                                          {QLatin1StringView("required"), QJsonArray{names.at(0)}}}))
            << QJsonObject{{QLatin1StringView("a"), QJsonArray{QJsonObject()}}}
            << QString(QStringLiteral("- arguments/a/0/r0") + QString(59, QLatin1Char('x'))
                       + QStringLiteral("...: is required"));

        // A tool that takes a long text, with a dozen other properties, still
        // says why 4.4 MB of emoji is refused.
        QJsonObject properties{
            {QLatin1StringView("text"),
             QJsonObject{{QLatin1StringView("type"), QStringLiteral("string")},
                         {QLatin1StringView("maxLength"), 1048576}}},
            {QLatin1StringView("pane"), QJsonObject{{QLatin1StringView("type"), QStringLiteral("integer")}}}};
        for (int i = 0; i < 12; ++i) {
            properties.insert(QStringLiteral("p%1").arg(i),
                              QJsonObject{{QLatin1StringView("type"), QStringLiteral("string")}});
        }
        QString emoji;
        emoji.reserve(2 * 1100000);
        for (int i = 0; i < 1100000; ++i) {
            emoji += QString::fromUcs4(U"\U0001F600", 1);
        }
        QTest::newRow("a few megabytes of text still list")
            << QJsonObject{{QLatin1StringView("type"), QStringLiteral("object")},
                           {QLatin1StringView("properties"), properties}}
            << QJsonObject{{QLatin1StringView("text"), emoji}}
            << QStringLiteral("- arguments/text: must be at most 1048576 characters long");
    }

    void checksHaveABound()
    {
        // A check whose work the server cannot bound is not made: a value
        // nested deeper under a recursive schema than jsoncons can walk
        // without growing its memory with the square of the depth, a schema
        // whose references double the work at each level, and a large value
        // under a schema of many checks. A number that multipleOf cannot
        // check is refused by name.
        QFETCH(QJsonObject, schema);
        QFETCH(QJsonValue, arguments);
        QFETCH(QString, expected);
        Peer peer;
        McpServer server;
        QString error;
        QVERIFY2(server.addTool(McpTool(QStringLiteral("bound")).setInputSchema(schema), noop(), &error),
                 qPrintable(error));
        QVERIFY(server.start(peer.serverFd(), peer.serverFd()));
        QElapsedTimer timer;
        timer.start();
        QVERIFY(peer.send(callRequest(1, QStringLiteral("bound"), arguments.toObject())));
        const int bound = 30000;
        const QJsonObject reply = peer.readOne(bound);
        QVERIFY2(timer.elapsed() < bound, qPrintable(QString::number(timer.elapsed())));
        if (expected.isEmpty()) {
            QCOMPARE(firstText(reply), QStringLiteral("ok"));
        } else {
            QCOMPARE(errorLines(reply), expected);
        }
    }

    void checksHaveABound_data()
    {
        QTest::addColumn<QJsonObject>("schema");
        QTest::addColumn<QJsonValue>("arguments");
        // Empty: the handler runs.
        QTest::addColumn<QString>("expected");
        const auto nest = [](int depth, const QString& key) {
            QJsonValue value = 1;
            for (int i = 0; i < depth; ++i) {
                value = key.isEmpty() ? QJsonValue(QJsonArray{value}) : QJsonValue(QJsonObject{{key, value}});
            }
            return value;
        };
        // A list of lists, which recurses through items and $ref: two tokens
        // of path for each level of the value.
        const QJsonObject lists = parse(
            "{'type': 'object', 'properties': {'a': {'$ref': '#/$defs/n'}}, '$defs': {'n': {'type': ['array', "
            "'integer'], 'items': {'$ref': '#/$defs/n'}}}}");
        QTest::newRow("lists 300 deep") << lists << QJsonValue(QJsonObject{{QLatin1StringView("a"), nest(300, {})}})
                                        << QString();
        QTest::newRow("lists 600 deep") << lists << QJsonValue(QJsonObject{{QLatin1StringView("a"), nest(600, {})}})
                                        << TooLargeToCheck;
        // Two references to the schema at each level: 2^depth checks.
        const QJsonObject doubling = parse(
            "{'type': 'object', 'properties': {'a': {'$ref': '#/$defs/n'}}, '$defs': {'n': {'properties': {'x': "
            "{'allOf': [{'$ref': '#/$defs/n'}, {'$ref': '#/$defs/n'}]}}}}}");
        QTest::newRow("doubling 5 deep") << doubling
                                         << QJsonValue(
                                                QJsonObject{{QLatin1StringView("a"), nest(5, QStringLiteral("x"))}})
                                         << QString();
        QTest::newRow("doubling 40 deep")
            << doubling << QJsonValue(QJsonObject{{QLatin1StringView("a"), nest(40, QStringLiteral("x"))}})
            << TooLargeToCheck;
        // Three million integers, which jsoncons would check in about a
        // second, refused, and a hundred thousand checked.
        const QJsonObject integers =
            parse("{'type': 'object', 'properties': {'a': {'type': 'array', 'items': {'type': 'integer'}}}}");
        QJsonArray many;
        for (int i = 0; i < 3000000; ++i) {
            many.append(1);
        }
        QTest::newRow("3000000 integers")
            << integers << QJsonValue(QJsonObject{{QLatin1StringView("a"), many}}) << TooLargeToCheck;
        QTest::newRow("100000 integers")
            << integers
            << QJsonValue(QJsonObject{{QLatin1StringView("a"), QJsonArray::fromVariantList(QVariantList(100000, 1))}})
            << QString();
        // jsoncons checks multipleOf with doubles, which do not tell 10^17
        // from a multiple of 3.
        const QJsonObject multiple = parse("{'type': 'object', 'properties': {'a': {'multipleOf': 3}}}");
        QTest::newRow("multipleOf past 2^53")
            << multiple << QJsonValue(parse("{'a': 100000000000000000}"))
            << QStringLiteral(
                   "- arguments/a: must be less than 2^53 in magnitude, because the server checks "
                   "multipleOf with doubles");
        QTest::newRow("multipleOf below 2^53") << multiple << QJsonValue(parse("{'a': 9007199254740989}"))
                                               << QStringLiteral("- arguments/a: must be a multiple of 3");
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

        // Names that differ only past the cut are as many errors, though
        // their lines look alike: ten, and a count of the rest.
        QJsonObject arguments{{QLatin1StringView("pane"), 1}};
        for (int i = 0; i < 200; ++i) {
            arguments.insert(QString(100, QLatin1Char('n')) + QString::number(i), 1);
        }
        QVERIFY(peer.send(callRequest(2, QStringLiteral("pane_tool"), arguments)));
        const QStringList lines = errorLines(peer.readOne()).split(QLatin1Char('\n'));
        QCOMPARE(lines.size(), 11);
        QCOMPARE(lines.first(),
                 QStringLiteral("- arguments/") + QString(61, QLatin1Char('n'))
                     + QStringLiteral("...: is not a known property"));
        QCOMPARE(lines.at(9), lines.first());
        QCOMPARE(lines.last(), QStringLiteral("- arguments: and 190 more errors"));
    }

    void boundsAreTheSameInEveryLocale()
    {
        // QCoreApplication sets the locale of the environment, and jsoncons
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
        // For each keyword addTool allows, a value that JSON Schema 2020-12
        // accepts passes and one it refuses fails. The annotations, format
        // among them, never refuse.
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
        // jsoncons would take 2^63 for an integer of at least 1, and
        // toInteger() gives the handler 0: refused before jsoncons.
        row("integer 2^63 with a minimum", "{'type': 'integer', 'minimum': 1}", "9223372036854775808", false);
        row("integer 1e19", "{'type': 'integer'}", "1e19", false);
        // Qt's parser rounds these to a double, as the README says: the
        // first arrives as qint64's minimum, the second as 2^63.
        row("integer rounded to qint64 min", "{'type': 'integer'}", "-9223372036854775809", true);
        row("integer qint64 max with a fraction", "{'type': 'integer'}", "9223372036854775807.0", false);
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
        // UTF-8 cannot hold 't\ud800ab', which the handler reads.
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
        // UTF-8 cannot hold the surrogate, so the length is unknown.
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
        // A nested schema of propertyNames is its own: not, anyOf and allOf
        // inside it apply to the name.
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
            "'writeOnly': false, 'deprecated': true, '$comment': 'c', 'contentMediaType': 'image/png', "
            "'contentEncoding': 'base64'}",
            "1", true);
        // format is an annotation in 2020-12, as a client reads it.
        row("format", "{'type': 'string', 'format': 'email'}", "'not an address'", true);
        // Integers compare exactly, past 2^53 too.
        row("maximum past 2^53", "{'type': 'integer', 'maximum': 9007199254740993}", "9007199254740994", false);
        row("maximum past 2^53 accepts", "{'type': 'integer', 'maximum': 9007199254740993}", "9007199254740993", true);
        row("enum past 2^53", "{'enum': [9007199254740993]}", "9007199254740992", false);
        row("enum past 2^53 accepts", "{'enum': [9007199254740993]}", "9007199254740993", true);
        row("const 1 and 1.0", "{'const': 1}", "1.0", true);
        // Members compare by name, whatever their order.
        row("const object", "{'const': {'a': 1, 'b': [true]}}", "{'b': [true], 'a': 1}", true);
        row("const object refuses", "{'const': {'a': 1, 'b': [true]}}", "{'b': [false], 'a': 1}", false);
        row("multipleOf", "{'multipleOf': 2}", "4", true);
        row("multipleOf refuses", "{'multipleOf': 2}", "3", false);
        row("multipleOf a fraction", "{'multipleOf': 1}", "2.5", false);
        row("multipleOf of a fraction", "{'multipleOf': 2.5}", "7.5", true);
        // jsoncons allows an error of one unit in the last place, so the
        // decimal 0.3 passes where (0.3 / 0.1) % 1 in doubles does not.
        row("multipleOf 0.1", "{'multipleOf': 0.1}", "0.3", true);
        // Past 2^53 doubles do not tell 10^17 from a multiple of 3: refused.
        row("multipleOf past 2^53", "{'multipleOf': 3}", "100000000000000000", false);
        row("multipleOf past 2^53 of a multiple", "{'multipleOf': 1}", "100000000000000000", false);
        row("contains", "{'contains': {'type': 'string'}}", "[1, 'x']", true);
        row("contains refuses", "{'contains': {'type': 'string'}}", "[1, 2]", false);
        row("contains not an array", "{'contains': {'type': 'string'}}", "{'a': 1}", true);
        row("minContains", "{'contains': {'type': 'string'}, 'minContains': 2}", "['x', 1]", false);
        row("minContains accepts", "{'contains': {'type': 'string'}, 'minContains': 2}", "['x', 1, 'y']", true);
        row("minContains 0", "{'contains': {'type': 'string'}, 'minContains': 0}", "[]", true);
        row("maxContains", "{'contains': {'type': 'string'}, 'maxContains': 1}", "['x', 'y']", false);
        row("maxContains accepts", "{'contains': {'type': 'string'}, 'maxContains': 1}", "['x', 1]", true);
        row("prefixItems", "{'prefixItems': [{'type': 'string'}, {'type': 'integer'}]}", "['a', 1, null]", true);
        row("prefixItems refuses", "{'prefixItems': [{'type': 'string'}, {'type': 'integer'}]}", "[1]", false);
        row("prefixItems and items", "{'prefixItems': [{'type': 'string'}], 'items': {'type': 'integer'}}",
            "['a', 1, 2]", true);
        row("prefixItems and items refuses", "{'prefixItems': [{'type': 'string'}], 'items': {'type': 'integer'}}",
            "['a', 1, 'b']", false);
        row("prefixItems and items false", "{'prefixItems': [{}], 'items': false}", "[1, 2]", false);
        row("dependentRequired", "{'dependentRequired': {'a': ['b']}}", "{'a': 1}", false);
        row("dependentRequired accepts", "{'dependentRequired': {'a': ['b']}}", "{'a': 1, 'b': 2}", true);
        row("dependentRequired absent", "{'dependentRequired': {'a': ['b']}}", "{'c': 1}", true);
        row("if then", "{'if': {'required': ['a']}, 'then': {'required': ['b']}, 'else': {'required': ['c']}}",
            "{'a': 1}", false);
        row("if then accepts", "{'if': {'required': ['a']}, 'then': {'required': ['b']}, 'else': {'required': ['c']}}",
            "{'a': 1, 'b': 1}", true);
        row("if else", "{'if': {'required': ['a']}, 'then': {'required': ['b']}, 'else': {'required': ['c']}}", "{}",
            false);
        row("if else accepts", "{'if': {'required': ['a']}, 'then': {'required': ['b']}, 'else': {'required': ['c']}}",
            "{'c': 1}", true);
        row("then without if", "{'then': false}", "1", true);
        row("unevaluatedProperties", "{'allOf': [{'properties': {'a': true}}], 'unevaluatedProperties': false}",
            "{'a': 1, 'b': 2}", false);
        row("unevaluatedProperties accepts", "{'allOf': [{'properties': {'a': true}}], 'unevaluatedProperties': false}",
            "{'a': 1}", true);
        // A branch of anyOf that failed evaluates nothing.
        row("unevaluatedProperties failed branch",
            "{'anyOf': [{'properties': {'a': true}, 'required': ['z']}, true], 'unevaluatedProperties': false}",
            "{'a': 1}", false);
        row("unevaluatedProperties through $ref",
            "{'$defs': {'p': {'properties': {'a': true}}}, '$ref': '#/properties/v/$defs/p', "
            "'unevaluatedProperties': false}",
            "{'a': 1}", true);
        row("unevaluatedItems", "{'prefixItems': [true], 'unevaluatedItems': false}", "[1, 2]", false);
        row("unevaluatedItems accepts", "{'prefixItems': [true], 'unevaluatedItems': false}", "[1]", true);
        row("unevaluatedItems schema", "{'prefixItems': [true], 'unevaluatedItems': {'type': 'string'}}", "[1, 'x', 2]",
            false);
        row("$ref", "{'$defs': {'s': {'type': 'string'}}, '$ref': '#/properties/v/$defs/s'}", "1", false);
        row("$ref accepts", "{'$defs': {'s': {'type': 'string'}}, '$ref': '#/properties/v/$defs/s'}", "'x'", true);
        row("$ref beside a keyword",
            "{'$defs': {'s': {'type': 'string'}}, '$ref': '#/properties/v/$defs/s', "
            "'maxLength': 1}",
            "'xy'", false);
        row("$ref to an anchor", "{'$defs': {'s': {'$anchor': 'str', 'type': 'string'}}, '$ref': '#str'}", "1", false);
        row("$ref recursion",
            "{'$defs': {'n': {'type': 'object', 'properties': {'next': {'$ref': '#/properties/v/$defs/n'}, "
            "'id': {'type': 'integer'}}}}, '$ref': '#/properties/v/$defs/n'}",
            "{'next': {'next': {'id': 3}}}", true);
        row("$ref recursion refuses",
            "{'$defs': {'n': {'type': 'object', 'properties': {'next': {'$ref': '#/properties/v/$defs/n'}, "
            "'id': {'type': 'integer'}}}}, '$ref': '#/properties/v/$defs/n'}",
            "{'next': {'next': {'id': 'x'}}}", false);
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
        // "properties" names members, and so does the chain: a name that is
        // a number does not step into an array.
        const QJsonObject numbered =
            parse("{'type': 'object', 'properties': {'a': {'properties': {'0': {'propertyNames': false}}}}}");
        QTest::newRow("chain through an array") << numbered << QByteArray("{'a': [{'x': 1}]}") << QString();
        QTest::newRow("chain with a number") << numbered << QByteArray("{'a': {'0': {'x': 1}}}")
                                             << QStringLiteral(
                                                    "- arguments/a/0/x: is not allowed, because the schema allows no "
                                                    "property here");
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
                                              "- arguments/env/: the property name must be at least 1 character long\n"
                                              "- arguments/env/: must be of type string\n"
                                              "- arguments/env/: must be the value of const\n"
                                              "- arguments/env/: must match at least one schema of anyOf\n"
                                              "- arguments/where: must be one of the values of enum");

        // propertyNames applies wherever the schema that holds it applies:
        // to each item, each other member, a branch of a combinator.
        const QJsonObject inItems =
            parse("{'type': 'object', 'properties': {'a': {'items': {'propertyNames': {'maxLength': 1}}}}}");
        QTest::newRow("in items") << inItems << QByteArray("{'a': [{'x': 1}, {'y': 2}]}") << QString();
        QTest::newRow("in items refuses")
            << inItems << QByteArray("{'a': [{'x': 1}, {'yz': 2, 'w': 3}]}")
            << QStringLiteral("- arguments/a/1/yz: the property name must be at most 1 character long");
        const QJsonObject inAdditional =
            parse("{'type': 'object', 'additionalProperties': {'propertyNames': {'enum': ['k']}}}");
        QTest::newRow("in additionalProperties") << inAdditional << QByteArray("{'a': {'k': 1}}") << QString();
        QTest::newRow("in additionalProperties refuses")
            << inAdditional << QByteArray("{'a': {'k': 1}, 'b': {'j': 2}}")
            << QStringLiteral("- arguments/b/j: the property name must be one of the values of enum");
        const QJsonObject inAnyOf = parse(
            "{'type': 'object', 'properties': {'a': {'anyOf': [{'type': 'array'}, {'propertyNames': {'const': "
            "'k'}}]}}}");
        QTest::newRow("in anyOf") << inAnyOf << QByteArray("{'a': {'k': 1}}") << QString();
        QTest::newRow("in anyOf refuses") << inAnyOf << QByteArray("{'a': {'j': 1}}")
                                          << QStringLiteral(
                                                 "- arguments/a: must be of type array\n"
                                                 "- arguments/a/j: the property name must be the value of const\n"
                                                 "- arguments/a: must match at least one schema of anyOf");
        // A name checked against a schema with a reference.
        const QJsonObject withRef = parse(
            "{'type': 'object', '$defs': {'short': {'maxLength': 2}}, 'propertyNames': {'$ref': '#/$defs/short'}}");
        QTest::newRow("through $ref") << withRef << QByteArray("{'abc': 1}")
                                      << QStringLiteral(
                                             "- arguments/abc: the property name must be at most 2 characters long");
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

    void errorsNameWhatRejects_data()
    {
        QTest::addColumn<QByteArray>("schema");
        QTest::addColumn<QByteArray>("arguments");
        QTest::addColumn<QString>("expected");
        const auto row = [](const char* name, const char* schema, const char* arguments, const char* expected) {
            QTest::newRow(name) << QByteArray(schema) << QByteArray(arguments) << QString::fromUtf8(expected);
        };
        // A branch of anyOf or oneOf that failed beside one that matched
        // binds nothing, so its constraint is not named.
        row("anyOf passed, type",
            "{'type': 'object', 'properties': {'v': {'type': 'string', 'anyOf': [{'type': 'integer'}, {'type': "
            "'number'}]}}}",
            "{'v': 1.5}", "- arguments/v: must be of type string");
        row("anyOf passed, bound",
            "{'type': 'object', 'properties': {'v': {'maximum': 5, 'anyOf': [{'maximum': 0}, {'minimum': 6}]}}}",
            "{'v': 7}", "- arguments/v: must be at most 5");
        row("anyOf passed, unknown property",
            "{'type': 'object', 'properties': {'a': {}}, 'additionalProperties': false, 'anyOf': [{'properties': "
            "{'b': {}}, 'additionalProperties': false}, true]}",
            "{'a': 1, 'z': 1}", "- arguments/z: is not a known property");
        row("oneOf passed",
            "{'type': 'object', 'properties': {'v': {'type': 'integer', 'oneOf': [{'type': 'string'}, {'type': "
            "'number'}]}}}",
            "{'v': 1.5}", "- arguments/v: must be of type integer");
        // The anyOf is above the error, and passed there.
        row("anyOf passed above",
            "{'type': 'object', 'properties': {'v': {'properties': {'a': {'maximum': 5}}, 'anyOf': [{'properties': "
            "{'a': {'maximum': 0}}}, {'type': 'object'}]}}}",
            "{'v': {'a': 7}}", "- arguments/v/a: must be at most 5");
        // An anyOf in a branch of allOf, which every value must match.
        row("anyOf passed in allOf",
            "{'type': 'object', 'properties': {'v': {'type': 'string', 'allOf': [{'anyOf': [{'type': 'boolean'}, "
            "{'type': 'integer'}]}]}}}",
            "{'v': 3}", "- arguments/v: must be of type string");
        // Every branch failed: each is named, and the anyOf.
        row("anyOf failed",
            "{'type': 'object', 'properties': {'v': {'maximum': 5, 'anyOf': [{'maximum': 0}, {'minimum': 9}]}}}",
            "{'v': 7}",
            "- arguments/v: must be at most 0\n- arguments/v: must be at least 9\n"
            "- arguments/v: must match at least one schema of anyOf\n- arguments/v: must be at most 5");
        row("oneOf failed",
            "{'type': 'object', 'properties': {'v': {'oneOf': [{'type': 'string'}, {'type': 'null'}]}}}", "{'v': 7}",
            "- arguments/v: must be of type string\n- arguments/v: must be of type null\n"
            "- arguments/v: must match exactly one schema of oneOf, and matches none");
        // Two branches matched: the one that failed is no reason.
        row("oneOf matched twice",
            "{'type': 'object', 'properties': {'v': {'oneOf': [{'type': 'integer'}, {'minimum': 0}, {'type': "
            "'string'}]}}}",
            "{'v': 7}", "- arguments/v: must match exactly one schema of oneOf, and matches more than one");
        row("allOf failed",
            "{'type': 'object', 'properties': {'v': {'allOf': [{'type': 'integer'}, {'minimum': 3}, true]}}}",
            "{'v': 1.5}", "- arguments/v: must be of type integer\n- arguments/v: must be at least 3");
        row("not", "{'type': 'object', 'properties': {'v': {'not': {'type': 'integer'}}}}", "{'v': 1}",
            "- arguments/v: must not match the schema of not");
        // A false subschema has no reason of its own.
        row("false below items", "{'type': 'object', 'properties': {'v': {'items': {'properties': {'a': false}}}}}",
            "{'v': [{'a': 1}]}", "- arguments/v/0/a: does not match the schema");
        // jsoncons names the first item that a false items refuses only.
        row("items false", "{'type': 'object', 'properties': {'v': {'items': false}}}", "{'v': [1, 2]}",
            "- arguments/v/0: is not allowed, because the schema allows no items");
        row("items false after prefixItems",
            "{'type': 'object', 'properties': {'v': {'prefixItems': [{}, {}], 'items': false}}}", "{'v': [1, 2, 3, 4]}",
            "- arguments/v/2: is not allowed, because the schema allows at most 2 items");
        row("items after prefixItems",
            "{'type': 'object', 'properties': {'v': {'prefixItems': [{'type': 'string'}], 'items': {'type': "
            "'integer'}}}}",
            "{'v': [1, 'a']}", "- arguments/v/0: must be of type string\n- arguments/v/1: must be of type integer");
        // jsoncons collects the errors of additionalProperties and gives
        // none: each member is checked again to name them.
        row("false below additionalProperties",
            "{'type': 'object', 'additionalProperties': {'properties': {'x': false}}}", "{'c': {'x': 1}}",
            "- arguments/c/x: does not match the schema");
        row("additionalProperties schema",
            "{'type': 'object', 'properties': {'a': {}}, 'additionalProperties': {'type': 'integer', 'maximum': 3}}",
            "{'a': 'x', 'b': 9, 'c': 'y', 'd': 1}",
            "- arguments/b: must be at most 3\n- arguments/c: must be of type integer");
        row("additionalProperties nested",
            "{'type': 'object', 'additionalProperties': {'additionalProperties': {'type': 'integer'}}}",
            "{'o': {'k': 'z', 'l': 2}}", "- arguments/o/k: must be of type integer");
        row("contains", "{'type': 'object', 'properties': {'v': {'contains': {'type': 'string'}}}}", "{'v': [1]}",
            "- arguments/v: must have an item that matches contains");
        row("minContains",
            "{'type': 'object', 'properties': {'v': {'contains': {'type': 'string'}, 'minContains': 3}}}",
            "{'v': ['a', 'b']}", "- arguments/v: must have at least 3 items that match contains");
        row("maxContains",
            "{'type': 'object', 'properties': {'v': {'contains': {'type': 'string'}, 'maxContains': 1}}}",
            "{'v': ['a', 'b']}", "- arguments/v: must have at most 1 item that matches contains");
        row("dependentRequired", "{'type': 'object', 'dependentRequired': {'card': ['name', 'expiry']}}",
            "{'card': 1, 'expiry': 2}", "- arguments/name: is required when \"card\" is present");
        row("if then else",
            "{'type': 'object', 'if': {'properties': {'kind': {'const': 'file'}}, 'required': ['kind']}, 'then': "
            "{'required': ['path']}, 'else': {'required': ['url']}}",
            "{'kind': 'file'}", "- arguments/path: is required");
        row("unevaluatedProperties",
            "{'type': 'object', 'allOf': [{'properties': {'a': {}}}], 'unevaluatedProperties': false}",
            "{'a': 1, 'b': 2}", "- arguments/b: is not a known property");
        row("unevaluatedItems",
            "{'type': 'object', 'properties': {'v': {'prefixItems': [{}], 'unevaluatedItems': false}}}",
            "{'v': [1, 2]}", "- arguments/v/1: is not allowed, because the schema allows no more items");
        row("unevaluatedItems schema",
            "{'type': 'object', 'properties': {'v': {'prefixItems': [{}], 'unevaluatedItems': {'type': 'string'}}}}",
            "{'v': [1, 2]}", "- arguments/v/1: must be of type string");
        row("multipleOf", "{'type': 'object', 'properties': {'v': {'multipleOf': 0.5}}}", "{'v': 0.7}",
            "- arguments/v: must be a multiple of 0.5");
        // The place in the value, whatever the references on the way.
        row("$ref",
            "{'type': 'object', '$defs': {'item': {'$anchor': 'item', 'properties': {'n': {'type': 'integer'}}}}, "
            "'properties': {'v': {'items': {'$ref': '#item'}}}}",
            "{'v': [{'n': 1}, {'n': 'x'}]}", "- arguments/v/1/n: must be of type integer");
        row("$ref recursion",
            "{'type': 'object', 'properties': {'next': {'$ref': '#'}, 'n': {'type': 'integer', 'maximum': 9}}}",
            "{'next': {'next': {'n': 10}}}", "- arguments/next/next/n: must be at most 9");
        row("big bounds",
            "{'type': 'object', 'properties': {'v': {'minimum': 9007199254740993, 'exclusiveMaximum': 1e300}}}",
            "{'v': 9007199254740992}", "- arguments/v: must be at least 9007199254740993");
    }

    void errorsNameWhatRejects()
    {
        QFETCH(QByteArray, schema);
        QFETCH(QByteArray, arguments);
        QFETCH(QString, expected);
        Peer peer;
        McpServer server;
        QString error;
        QVERIFY2(server.addTool(McpTool(QStringLiteral("t")).setInputSchema(parse(schema)), noop(), &error),
                 qPrintable(error));
        QVERIFY(server.start(peer.serverFd(), peer.serverFd()));
        QVERIFY(sendCall(peer, 1, QStringLiteral("t"), arguments));
        QCOMPARE(errorLines(peer.readOne()), expected);
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

    void deepStructuredContentIsRefused()
    {
        // A handler can build a value nested deeper than Qt's parser takes
        // from the wire (1024 levels). The checks walk it recursively, so it
        // is refused before them.
        Peer peer;
        McpServer server;
        McpTool tool(QStringLiteral("deep"));
        tool.setOutputSchema(parse("{'type': 'array'}"));
        QVERIFY(server.addTool(tool, [](McpCall call) {
            QJsonValue value = QJsonArray();
            for (int i = 0; i < 1100; ++i) {
                value = QJsonArray{value};
            }
            call.finish(McpResult::structured(value));
        }));
        QVERIFY(server.start(peer.serverFd(), peer.serverFd()));
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("does not match its output schema")));
        QVERIFY(peer.send(callRequest(1, QStringLiteral("deep"))));
        const QJsonObject reply = peer.readOne();
        QCOMPARE(errorCode(reply), ErrorCode::InternalError);
        QString pointer;
        for (int i = 0; i <= 1024; ++i) {
            pointer += QStringLiteral("/0");
        }
        const QString expected = QStringLiteral("structuredContent") + pointer
            + QStringLiteral(": is nested more than 1024 levels deep, which the server does not check");
        QVERIFY2(QJsonDocument(reply).toJson().contains(expected.toUtf8()),
                 QJsonDocument(reply).toJson().left(400).constData());
    }

    void outputPropertyNamesSkipArrays()
    {
        // An output schema may have any root. Its propertyNames under
        // "properties" applies to an object's member called "0", not to the
        // first item of an array.
        Peer peer;
        McpServer server;
        McpTool tool(QStringLiteral("list"));
        tool.setInputSchema(parse("{'type': 'object', 'properties': {'kind': {'type': 'string'}}}"));
        tool.setOutputSchema(parse("{'properties': {'0': {'propertyNames': false}}}"));
        const QJsonObject member{{QLatin1StringView("x"), 1}};
        QVERIFY(server.addTool(tool, [&](McpCall call) {
            if (call.arguments().value(QLatin1StringView("kind")) == QJsonValue(QStringLiteral("array"))) {
                call.finish(McpResult::structured(QJsonArray{member}));
            } else {
                call.finish(McpResult::structured(QJsonObject{{QLatin1StringView("0"), member}}));
            }
        }));
        QVERIFY(server.start(peer.serverFd(), peer.serverFd()));

        QVERIFY(peer.send(
            callRequest(1, QStringLiteral("list"), QJsonObject{{QLatin1StringView("kind"), QStringLiteral("array")}})));
        QJsonObject reply = peer.readOne();
        QCOMPARE(resultOf(reply).value(QLatin1StringView("structuredContent")), QJsonValue(QJsonArray{member}));

        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("does not match its output schema")));
        QVERIFY(peer.send(callRequest(2, QStringLiteral("list"),
                                      QJsonObject{{QLatin1StringView("kind"), QStringLiteral("object")}})));
        reply = peer.readOne();
        QCOMPARE(errorCode(reply), ErrorCode::InternalError);
        QVERIFY2(QJsonDocument(reply).toJson().contains(
                     "structuredContent/0/x: is not allowed, because the schema allows no property here"),
                 QJsonDocument(reply).toJson().constData());
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
