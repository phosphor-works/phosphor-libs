// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// The bound of the argument check: large arguments and costly schemas answer
// fast, with their errors while they are few, one line once there are too
// many to list, and one line that says so when the check would take too
// long or hold too much. Each shape that the audits of the checker found
// costly has a row. The rows that pass the bound burn all of its work, about
// half a second of a release build, so they are a test of their own.
// test_phosphor_mcp_checkwork counts what each step is charged.

#include "mcptesthelpers.h"

#include <PhosphorMcp/McpServer.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTest>

#include <ctime>

using namespace PhosphorMcp;
using namespace PhosphorMcpTests;

namespace {

// AddressSanitizer grows each frame of the check's recursion, more when it
// is optimized: a build at -O1 refuses lists 500 levels deep at the bound of
// the check's own stack, and one at -O0, as CI's sanitizer build is, passes
// them. Such a build checks the lists that pass below half as deep.
#if defined(__SANITIZE_ADDRESS__)
constexpr bool Sanitized = true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
constexpr bool Sanitized = true;
#else
constexpr bool Sanitized = false;
#endif
#else
constexpr bool Sanitized = false;
#endif
constexpr int ListLevels = Sanitized ? 250 : 500;
constexpr int ChainedLevels = Sanitized ? 75 : 150;

// JSON with single quotes for double ones.
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

// A tools/call whose arguments are `arguments` exactly as written.
bool sendCall(Peer& peer, int id, const QString& tool, const QByteArray& arguments)
{
    QByteArray line = QJsonDocument(callRequest(id, tool, QJsonObject{{QLatin1StringView("a"), QStringLiteral("@@")}}))
                          .toJson(QJsonDocument::Compact);
    line.replace("{\"a\":\"@@\"}", json(arguments));
    return peer.sendRaw(line + '\n');
}

// The lines of an argument error after its first.
QString errorLines(const QJsonObject& reply)
{
    return firstText(reply).section(QLatin1Char('\n'), 1);
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

// {"type": "object", "properties": {"v": property}}
QJsonObject withV(const QJsonObject& property)
{
    return QJsonObject{{QLatin1StringView("type"), QStringLiteral("object")},
                       {QLatin1StringView("properties"), QJsonObject{{QLatin1StringView("v"), property}}}};
}

QJsonArray repeated(const QJsonValue& item, int count)
{
    QJsonArray array;
    for (int i = 0; i < count; ++i) {
        array.append(item);
    }
    return array;
}

// A hang guard on the CPU time of the thread, where the server runs too,
// which a release build stays far below (the rows take milliseconds, half a
// second for those that spend the whole bound): a debug build with
// sanitizers is some twenty times slower. A loaded machine stretches the
// wall-clock time, not this.
constexpr double BoundSeconds = 60;
// How long a row waits for its answer on the wall clock. The test's own
// timeout ends a hang.
constexpr int WaitMs = 600000;

double threadSeconds()
{
    timespec now{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
    return double(now.tv_sec) + double(now.tv_nsec) / 1e9;
}

const QString TooLargeToList =
    QStringLiteral("- arguments: does not match the schema, and is too large for the server to list each error");
const QString TooLargeToCheck = QStringLiteral("- arguments: is too large for the server to check against the schema");
const QString TooManyRefused =
    QStringLiteral("- arguments: is refused before the check, with too many errors for the server to list each one");

} // namespace

class TestPhosphorMcpBounds : public QObject
{
    Q_OBJECT

private:
    // Calls a tool of `schema` with `arguments` as written, and compares the
    // lines of the answer (empty: the handler ran).
    static void check(const QJsonObject& schema, const QByteArray& arguments, const QString& expected)
    {
        Peer peer;
        McpServer server;
        QString error;
        QVERIFY2(server.addTool(McpTool(QStringLiteral("t")).setInputSchema(schema), noop(), &error),
                 qPrintable(error));
        QVERIFY(server.start(peer.serverFd(), peer.serverFd()));
        const double start = threadSeconds();
        QVERIFY(sendCall(peer, 1, QStringLiteral("t"), arguments));
        const QJsonObject reply = peer.readOne(WaitMs);
        const double spent = threadSeconds() - start;
        QVERIFY2(spent < BoundSeconds, qPrintable(QString::number(spent)));
        if (expected.isEmpty()) {
            QCOMPARE(firstText(reply), QStringLiteral("ok"));
        } else {
            QCOMPARE(errorLines(reply), expected);
        }
    }

private Q_SLOTS:
    void boundedChecks_data()
    {
        QTest::addColumn<QJsonObject>("schema");
        QTest::addColumn<QByteArray>("arguments");
        // Empty: the handler runs.
        QTest::addColumn<QString>("expected");
        const auto repeat = [](const QByteArray& item, int count) -> QByteArray {
            QByteArray list;
            list.reserve(qsizetype(item.size() + 1) * count);
            for (int i = 0; i < count; ++i) {
                list += (i ? "," : "") + item;
            }
            return list;
        };
        const auto nest = [](int depth, const QByteArray& open, const QByteArray& close) -> QByteArray {
            return open.repeated(depth) + "1" + close.repeated(depth);
        };

        // Afterglow's env, every value wrong twice (anyOf): one line for
        // more than a thousand errors, however large.
        const auto variables = [](int count) {
            QByteArray env;
            for (int i = 0; i < count; ++i) {
                env += (i ? ",'V" : "'V") + QByteArray::number(i) + "':" + QByteArray::number(i);
            }
            return env;
        };
        QTest::newRow("50000 wrong variables")
            << envSchema() << "{'where': 'tab', 'env': {" + variables(50000) + "}}" << TooLargeToList;
        QByteArray right = "{";
        for (int i = 0; i < 300000; ++i) {
            right += (i ? ",'V" : "'V") + QByteArray::number(i) + "':'x'";
        }
        QTest::newRow("300000 right variables") << parse(
            "{'type': 'object', 'additionalProperties': {'anyOf': [{'type': 'string'}, {'const': false}]}, "
            "'propertyNames': {'minLength': 1, 'maxLength': 256}}")
                                                << right + "}" << QString();
        // One wrong value, however large.
        QTest::newRow("400000 items for a string")
            << envSchema() << "{'where': [" + repeat("1", 400000) + "]}"
            << QStringLiteral(
                   "- arguments/where: must be of type string\n- arguments/where: must be one of the values of enum");
        // The checks before the schema's are capped as the others.
        QTest::newRow("400000 surrogates")
            << envSchema() << "{'where': [" + repeat("'\\ud800'", 400000) + "]}" << TooManyRefused;

        // Lists of lists, which recurse through items and $ref, two
        // subschemas deep for each level...
        const QJsonObject lists = parse(
            "{'type': 'object', 'properties': {'a': {'$ref': '#/$defs/n'}}, '$defs': {'n': {'type': ['array', "
            "'integer'], 'items': {'$ref': '#/$defs/n'}}}}");
        QTest::newRow("lists deep") << lists << "{'a': " + nest(ListLevels, "[", "]") + "}" << QString();
        QTest::newRow("lists deep, wrong") << lists << "{'a': " + nest(ListLevels, "[", "]").replace("1", "'x'") + "}"
                                           << QStringLiteral("- arguments/a")
                + QStringLiteral("/0").repeated(ListLevels) + QStringLiteral(": must be of type array or integer");
        // ...and through three $ref more, five subschemas for each level, so
        // that 3000 subschemas deep are 600 levels, with the root and "a".
        // The stack bound stops a build with sanitizers sooner (see
        // Sanitized).
        const QJsonObject chained = parse(
            "{'type': 'object', 'properties': {'a': {'$ref': '#/$defs/n'}}, '$defs': {'n': {'type': ['array', "
            "'integer'], 'items': {'$ref': '#/$defs/b'}}, 'b': {'$ref': '#/$defs/c'}, 'c': {'$ref': '#/$defs/d'}, "
            "'d': {'$ref': '#/$defs/n'}}}");
        QTest::newRow("lists through $ref, deep")
            << chained << "{'a': " + nest(ChainedLevels, "[", "]") + "}" << QString();
        QTest::newRow("lists through $ref, 700 deep")
            << chained << "{'a': " + nest(700, "[", "]") + "}" << TooLargeToCheck;

        // Two references to the schema at each level: 2^depth checks.
        const QJsonObject doubling = parse(
            "{'type': 'object', 'properties': {'a': {'$ref': '#/$defs/n'}}, '$defs': {'n': {'properties': {'x': "
            "{'allOf': [{'$ref': '#/$defs/n'}, {'$ref': '#/$defs/n'}]}}}}}");
        QTest::newRow("doubling 5 deep") << doubling << "{'a': " + nest(5, "{'x': ", "}") + "}" << QString();
        QTest::newRow("doubling 40 deep") << doubling << "{'a': " + nest(40, "{'x': ", "}") + "}" << TooLargeToCheck;

        // Many keywords for each of many items: checked to the bound.
        QJsonArray minimums;
        for (int i = 0; i < 20; ++i) {
            minimums.append(QJsonObject{{QLatin1StringView("minimum"), 0}});
        }
        const QJsonObject heavy =
            withV(QJsonObject{{QLatin1StringView("items"), QJsonObject{{QLatin1StringView("allOf"), minimums}}}});
        QTest::newRow("20 allOf, 100000 items") << heavy << "{'v': [" + repeat("1", 100000) + "]}" << QString();
        QTest::newRow("20 allOf, 1000000 items") << heavy << "{'v': [" + repeat("1", 1000000) + "]}" << TooLargeToCheck;

        // uniqueItems keeps 32 bytes for each item while it sorts them.
        const QJsonObject unique = withV(parse("{'uniqueItems': true}"));
        QTest::newRow("uniqueItems, 2000000 items")
            << unique << "{'v': [" + repeat("1", 2000000) + "]}"
            << QStringLiteral("- arguments/v: must have unique items, and items 0 and 1 are equal");
        QTest::newRow("uniqueItems, 2100000 items")
            << unique << "{'v': [" + repeat("1", 2100000) + "]}" << TooLargeToCheck;
        QTest::newRow("uniqueItems, 200000 distinct objects") << unique << "{'v': [" + [] {
            QByteArray list;
            for (int i = 0; i < 200000; ++i) {
                list += (i ? "," : "") + QByteArray("{'a': ") + QByteArray::number(i) + ", 'b': [1, 'x']}";
            }
            return list;
        }() + "]}" << QString();

        // The shapes that the audits of the jsoncons checker found costly,
        // each cheap now. dependentRequired with 2000 triggers looked each up
        // in each object.
        QJsonObject triggers;
        for (int i = 0; i < 2000; ++i) {
            triggers.insert(QStringLiteral("t%1").arg(i), QJsonArray{QStringLiteral("x")});
        }
        QTest::newRow("dependentRequired of 2000 triggers, 1000000 objects") << withV(
            QJsonObject{{QLatin1StringView("items"), QJsonObject{{QLatin1StringView("dependentRequired"), triggers}}}})
                                                                             << "{'v': [" + repeat("{}", 1000000) + "]}"
                                                                             << QString();
        // additionalProperties that fails for many members.
        QJsonObject maps;
        QByteArray mapArguments = "{";
        for (int i = 0; i < 300; ++i) {
            maps.insert(QStringLiteral("p%1").arg(i),
                        parse("{'type': 'object', 'description': 'A map of labels', 'additionalProperties': {'type': "
                              "'string'}}"));
            mapArguments += (i ? ",'p" : "'p") + QByteArray::number(i) + "': {'z': 1}";
        }
        QTest::newRow("additionalProperties of 300 maps")
            << QJsonObject{{QLatin1StringView("type"), QStringLiteral("object")},
                           {QLatin1StringView("properties"), maps}}
            << mapArguments + "}"
            << QStringLiteral(
                   "- arguments/p0/z: must be of type string\n- arguments/p1/z: must be of type string\n"
                   "- arguments/p10/z: must be of type string\n- arguments/p100/z: must be of type string\n"
                   "- arguments/p101/z: must be of type string\n- arguments/p102/z: must be of type string\n"
                   "- arguments/p103/z: must be of type string\n- arguments/p104/z: must be of type string\n"
                   "- arguments/p105/z: must be of type string\n- arguments/p106/z: must be of type string\n"
                   "- arguments: and 290 more errors");
        // required inside not and anyOf, every name missing.
        QJsonArray fields;
        for (int i = 0; i < 1000; ++i) {
            fields.append(QStringLiteral("field%1").arg(i));
        }
        const QJsonObject requiredItems{
            {QLatin1StringView("items"), QJsonObject{{QLatin1StringView("required"), fields}}}};
        QTest::newRow("required of 1000 inside not, 200000 objects")
            << withV(QJsonObject{{QLatin1StringView("not"), requiredItems}}) << "{'v': [" + repeat("{}", 200000) + "]}"
            << QString();
        QTest::newRow("required of 1000 inside anyOf, 200000 objects")
            << withV(QJsonObject{{QLatin1StringView("anyOf"), QJsonArray{requiredItems, parse("{'type': 'null'}")}}})
            << "{'v': [" + repeat("{}", 200000) + "]}" << TooLargeToList;
        // 9000 properties against objects of one other member.
        QJsonObject declared;
        for (int i = 0; i < 9000; ++i) {
            declared.insert(QStringLiteral("%1").arg(i, 6, 10, QLatin1Char('0')), QJsonObject());
        }
        QTest::newRow("9000 properties, 500000 objects")
            << withV(
                   QJsonObject{{QLatin1StringView("items"), QJsonObject{{QLatin1StringView("properties"), declared}}}})
            << "{'v': [" + repeat("{'000000z': 0}", 500000) + "]}" << QString();
        // unevaluatedItems beside an if whose items fail every other one.
        QTest::newRow("unevaluatedItems, 400000 items")
            << withV(parse("{'if': {'items': {'type': 'integer'}, 'unevaluatedItems': {'type': 'integer'}}, 'then': "
                           "true, 'unevaluatedItems': {'type': ['integer', 'string']}}"))
            << "{'v': [" + repeat("1, 'a'", 200000) + "]}" << QString();
        // propertyNames of many members, and a default, which the checker
        // never copies.
        QTest::newRow("propertyNames, 500000 members")
            << withV(parse("{'propertyNames': {'anyOf': [{'maxLength': 1}, {'minLength': 2, 'maxLength': 8}]}}"))
            << "{'v': {" +
                [] {
                    QByteArray members;
                    for (int i = 0; i < 500000; ++i) {
                        members += (i ? ",'k" : "'k") + QByteArray::number(i) + "': 1";
                    }
                    return members;
                }()
                + "}}"
            << QString();
        QJsonObject large;
        for (int i = 0; i < 2000; ++i) {
            large.insert(QStringLiteral("d%1").arg(i), i);
        }
        QTest::newRow("a default of 2000 members, 500000 objects") << withV(QJsonObject{
            {QLatin1StringView("items"),
             QJsonObject{{QLatin1StringView("properties"),
                          QJsonObject{{QLatin1StringView("p"), QJsonObject{{QLatin1StringView("default"), large}}}}}}}})
                                                                   << "{'v': [" + repeat("{}", 500000) + "]}"
                                                                   << QString();
        // enum against many strings in an anyOf, and contains that no item
        // matches.
        QTest::newRow("enum in an anyOf, 566000 strings")
            << withV(parse("{'items': {'anyOf': [{'enum': ['a', 'b']}, {'enum': ['c', 'd']}, {'enum': ['e']}, "
                           "{'enum': ['f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n']}]}}"))
            << "{'v': [" + repeat("'zz'", 566000) + "]}" << TooLargeToList;
        QTest::newRow("contains, 2000000 integers")
            << withV(parse("{'contains': {'type': 'string'}}")) << "{'v': [" + repeat("1", 2000000) + "]}"
            << QStringLiteral("- arguments/v: must have an item that matches contains");
        // Many small objects, which jsoncons' conversion made costly.
        QTest::newRow("2000000 empty objects")
            << withV(parse("{'items': {'type': 'object'}}")) << "{'v': [" + repeat("{}", 2000000) + "]}" << QString();
    }

    void boundedChecks()
    {
        QFETCH(QJsonObject, schema);
        QFETCH(QByteArray, arguments);
        QFETCH(QString, expected);
        check(schema, arguments, expected);
    }

    void combinatorsAnswerFast_data()
    {
        QTest::addColumn<QJsonObject>("schema");
        QTest::addColumn<QByteArray>("arguments");
        QTest::addColumn<QString>("expected");
        const auto items = [](const QByteArray& item, int count) -> QByteArray {
            QByteArray list = "{'v': [";
            for (int i = 0; i < count; ++i) {
                list += (i ? "," : "") + item;
            }
            return list + "]}";
        };

        // A oneOf of 15 arrays, each with items that are an anyOf of 15
        // objects, and items that match none of them.
        QJsonArray objects;
        QJsonArray arrays;
        for (int i = 0; i < 15; ++i) {
            objects.append(
                QJsonObject{{QLatin1StringView("properties"),
                             QJsonObject{{QLatin1StringView("x"), QJsonObject{{QLatin1StringView("maximum"), i}}}}},
                            {QLatin1StringView("required"), QJsonArray{QStringLiteral("y%1").arg(i)}}});
        }
        for (int i = 0; i < 15; ++i) {
            arrays.append(
                QJsonObject{{QLatin1StringView("type"), QStringLiteral("array")},
                            {QLatin1StringView("maxItems"), i},
                            {QLatin1StringView("items"), QJsonObject{{QLatin1StringView("anyOf"), objects}}}});
        }
        const QJsonObject nested = withV(QJsonObject{{QLatin1StringView("oneOf"), arrays}});
        QTest::newRow("oneOf of anyOf, 400 items") << nested << items("{'x': 1000}", 400) << TooLargeToList;
        // The same schema lists the errors of a small value.
        QTest::newRow("oneOf of anyOf, 1 item")
            << nested << items("{'x': 1000}", 1)
            << QStringLiteral(
                   "- arguments/v: must have at most 0 items\n"
                   "- arguments/v/0/x: must be at most 0\n- arguments/v/0/y0: is required\n"
                   "- arguments/v/0/x: must be at most 1\n- arguments/v/0/y1: is required\n"
                   "- arguments/v/0/x: must be at most 2\n- arguments/v/0/y2: is required\n"
                   "- arguments/v/0/x: must be at most 3\n- arguments/v/0/y3: is required\n"
                   "- arguments/v/0/x: must be at most 4\n"
                   "- arguments: and 23 more errors");

        // anyOf in anyOf, 50 deep, for each of 990 items, and for 1.
        QJsonObject chain{{QLatin1StringView("type"), QStringLiteral("integer")}, {QLatin1StringView("maximum"), 0}};
        for (int i = 0; i < 50; ++i) {
            chain = QJsonObject{
                {QLatin1StringView("anyOf"), QJsonArray{chain, parse("{'type': 'string', 'maxLength': 0}")}}};
        }
        const QJsonObject deep = withV(QJsonObject{{QLatin1StringView("items"), chain}});
        QTest::newRow("anyOf 50 deep, 990 items") << deep << items("5", 990) << TooLargeToList;
        QTest::newRow("anyOf 50 deep, 1 item") << deep << items("5", 1)
                                               << QStringLiteral(
                                                      "- arguments/v/0: must be at most 0\n"
                                                      "- arguments/v/0: must be of type string\n"
                                                      "- arguments/v/0: must match at least one schema of anyOf");
        // Valid: the last branch of each matches.
        QTest::newRow("anyOf 50 deep, 100000 right items") << deep << items("0", 100000) << QString();

        // An allOf of 200 anyOfs for each of 990 items, and for 10, with
        // too many errors to list.
        QJsonArray all;
        for (int i = 0; i < 200; ++i) {
            all.append(QJsonObject{{QLatin1StringView("anyOf"),
                                    QJsonArray{QJsonObject{{QLatin1StringView("maximum"), i}},
                                               QJsonObject{{QLatin1StringView("type"), QStringLiteral("string")}}}}});
        }
        const QJsonObject wide =
            withV(QJsonObject{{QLatin1StringView("items"), QJsonObject{{QLatin1StringView("allOf"), all}}}});
        QTest::newRow("allOf of 200 anyOfs, 990 items") << wide << items("1000", 990) << TooLargeToList;
        QTest::newRow("allOf of 200 anyOfs, 10 items") << wide << items("1000", 10) << TooLargeToList;
    }

    void combinatorsAnswerFast()
    {
        QFETCH(QJsonObject, schema);
        QFETCH(QByteArray, arguments);
        QFETCH(QString, expected);
        check(schema, arguments, expected);
    }

    void longStringsAnswerFast_data()
    {
        // Each string of the arguments is read in place, as Qt's parser
        // keeps it, its length counted once for all the subschemas applied
        // to it in place, and a long enum of strings is a table.
        QTest::addColumn<QJsonObject>("schema");
        QTest::addColumn<QJsonObject>("arguments");
        QTest::addColumn<QString>("expected");
        // U+4E2D, three bytes in UTF-8.
        const QChar cjk(0x4E2D);

        QJsonArray values;
        for (int i = 0; i < 3000; ++i) {
            values.append(QStringLiteral("v%1").arg(i));
        }
        QTest::newRow("CJK string against an enum")
            << withV(QJsonObject{{QLatin1StringView("enum"), values}})
            << QJsonObject{{QLatin1StringView("v"), QString(5000000, cjk)}}
            << QStringLiteral("- arguments/v: must be one of the values of enum");

        // 6 MB of UTF-8, which Qt's parser keeps as it is, against 300
        // subschemas that read its length: counted once for all of them.
        QJsonArray counted;
        for (int i = 0; i < 300; ++i) {
            counted.append(QJsonObject{{QLatin1StringView("maxLength"), 3000000 + i}});
        }
        QTest::newRow("CJK string against allOf of 300 maxLength")
            << withV(QJsonObject{{QLatin1StringView("allOf"), counted}})
            << QJsonObject{{QLatin1StringView("v"), QString(2000000, cjk)}} << QString();

        // 7.2 MB of UTF-8, each string counted once for the 35 maxLength of
        // an anyOf.
        QJsonArray lengths;
        for (int i = 1; i <= 35; ++i) {
            lengths.append(QJsonObject{{QLatin1StringView("maxLength"), i}});
        }
        QJsonArray strings;
        for (int i = 0; i < 24; ++i) {
            strings.append(QString(100000, cjk));
        }
        QTest::newRow("CJK strings against maxLength in an anyOf")
            << withV(QJsonObject{{QLatin1StringView("items"), QJsonObject{{QLatin1StringView("anyOf"), lengths}}}})
            << QJsonObject{{QLatin1StringView("v"), strings}}
            << QStringLiteral(
                   "- arguments/v/0: must be at most 1 character long\n"
                   "- arguments/v/0: must be at most 2 characters long\n"
                   "- arguments/v/0: must be at most 3 characters long\n"
                   "- arguments/v/0: must be at most 4 characters long\n"
                   "- arguments/v/0: must be at most 5 characters long\n"
                   "- arguments/v/0: must be at most 6 characters long\n"
                   "- arguments/v/0: must be at most 7 characters long\n"
                   "- arguments/v/0: must be at most 8 characters long\n"
                   "- arguments/v/0: must be at most 9 characters long\n"
                   "- arguments/v/0: must be at most 10 characters long\n"
                   "- arguments: and 854 more errors");

        // Each error that says a property is missing names it in full in its
        // place, which the server keeps to tell it from the others: more than
        // 1 Mi code units of them are one line.
        QJsonArray names;
        for (int i = 0; i < 10; ++i) {
            names.append(QString(QStringLiteral("r%1").arg(i) + QString(65536, QLatin1Char('x'))));
        }
        const QJsonObject requiredItems{
            {QLatin1StringView("items"), QJsonObject{{QLatin1StringView("required"), names}}}};
        QTest::newRow("long required names")
            << withV(requiredItems) << QJsonObject{{QLatin1StringView("v"), repeated(QJsonObject(), 100)}}
            << TooLargeToList;
        QTest::newRow("long required names, 1 item")
            << withV(QJsonObject{
                   {QLatin1StringView("items"), QJsonObject{{QLatin1StringView("required"), QJsonArray{names.at(0)}}}}})
            << QJsonObject{{QLatin1StringView("v"), QJsonArray{QJsonObject()}}}
            << QString(QStringLiteral("- arguments/v/0/r0") + QString(59, QLatin1Char('x'))
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

    void longStringsAnswerFast()
    {
        QFETCH(QJsonObject, schema);
        QFETCH(QJsonObject, arguments);
        QFETCH(QString, expected);
        check(schema, QJsonDocument(arguments).toJson(QJsonDocument::Compact), expected);
    }

    void schemaSizesAreBounded()
    {
        // A schema past the sizes the server checks is refused at addTool,
        // and compiles in time proportional to its size below them.
        McpServer server;
        QString error;
        const auto add = [&](const QJsonObject& schema) {
            QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("McpServer::addTool")));
            const bool added = server.addTool(
                McpTool(QStringLiteral("s%1").arg(server.tools().size())).setInputSchema(schema), noop(), &error);
            return added;
        };
        const auto many = [](int count) {
            QJsonObject properties;
            for (int i = 0; i < count; ++i) {
                properties.insert(QStringLiteral("p%1").arg(i), QJsonObject());
            }
            return QJsonObject{{QLatin1StringView("type"), QStringLiteral("object")},
                               {QLatin1StringView("properties"), properties}};
        };
        // The root and 9999 properties: 10000 subschemas.
        QVERIFY2(server.addTool(McpTool(QStringLiteral("many")).setInputSchema(many(9999)), noop(), &error),
                 qPrintable(error));
        QVERIFY(!add(many(10000)));
        QCOMPARE(error,
                 QStringLiteral("inputSchema: the schema has more than 10000 subschemas, which the server does "
                                "not check"));

        // The root, "a", and items below it: 256 levels below the root.
        const auto deep = [](int levels) {
            QJsonValue value = QJsonObject();
            for (int i = 0; i < levels - 1; ++i) {
                value = QJsonObject{{QLatin1StringView("items"), value}};
            }
            return QJsonObject{{QLatin1StringView("type"), QStringLiteral("object")},
                               {QLatin1StringView("properties"), QJsonObject{{QLatin1StringView("a"), value}}}};
        };
        QVERIFY2(server.addTool(McpTool(QStringLiteral("deep")).setInputSchema(deep(256)), noop(), &error),
                 qPrintable(error));
        QVERIFY(!add(deep(257)));
        QCOMPARE(error,
                 QStringLiteral("inputSchema/properties/a") + QStringLiteral("/items").repeated(256)
                     + QStringLiteral(": is nested more than 256 subschemas deep, which the server does not check"));

        // A megabyte of schema 900 levels deep, which took the jsoncons
        // checker ten seconds and two gigabytes, is refused at once, and one
        // of 9000 subschemas 250 levels deep compiles fast.
        const auto wide = [](int levels, int width) {
            QJsonObject level;
            for (int d = 0; d < levels; ++d) {
                QJsonObject properties;
                for (int i = 0; i < width; ++i) {
                    properties.insert(QStringLiteral("p%1").arg(i), QJsonObject());
                }
                level = QJsonObject{{QLatin1StringView("items"), level}, {QLatin1StringView("properties"), properties}};
            }
            return QJsonObject{{QLatin1StringView("type"), QStringLiteral("object")},
                               {QLatin1StringView("properties"), QJsonObject{{QLatin1StringView("x"), level}}}};
        };
        const double start = threadSeconds();
        QVERIFY(!add(wide(900, 100)));
        QVERIFY(error.endsWith(
            QLatin1StringView("is nested more than 256 subschemas deep, which the server does not check")));
        QVERIFY2(server.addTool(McpTool(QStringLiteral("wide")).setInputSchema(wide(250, 35)), noop(), &error),
                 qPrintable(error));
        QVERIFY2(threadSeconds() - start < BoundSeconds, qPrintable(QString::number(threadSeconds() - start)));
    }
};

QTEST_GUILESS_MAIN(TestPhosphorMcpBounds)
#include "test_phosphor_mcp_bounds.moc"
