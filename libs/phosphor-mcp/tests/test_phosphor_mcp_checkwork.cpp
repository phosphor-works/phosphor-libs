// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// The work and the memory that the checker charges for each step, counted
// exactly. The bound of a check (see the README, "Errors") holds only while
// each step is charged what it costs, so each charge, and each term of it
// that grows with a length, has a row whose count changes when it does. The
// checker's objects are linked into this test, which reads the counts that
// the library keeps to itself.
//
// The arguments are parsed as the server parses a request line, so that
// their strings are kept as Qt's parser keeps them: ASCII as Latin-1, other
// text as UTF-8, and a string with an escape as UTF-16.

#include "mcpschemacheck.h"
#include "mcpschemanode.h"

#include <QHashSeed>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QTest>
#include <QThread>

#include <algorithm>
#include <memory>
#include <random>
#include <type_traits>

using namespace PhosphorMcp::Schema;

namespace {

// JSON with single quotes for double ones.
QJsonValue parse(QByteArray text)
{
    text.replace('\'', '"');
    QJsonParseError error;
    const QJsonValue value = QJsonValue::fromJson(text, &error);
    return error.error == QJsonParseError::NoError ? value : QJsonValue(QJsonValue::Undefined);
}

QByteArray repeat(const QByteArray& item, int count)
{
    QByteArray list = "[";
    for (int i = 0; i < count; ++i) {
        list += (i ? "," : "") + item;
    }
    return list + "]";
}

// An object of `count` members k000, k001... with the value `item`.
QByteArray members(const QByteArray& item, int count, int width = 3)
{
    QByteArray object = "{";
    for (int i = 0; i < count; ++i) {
        object += (i ? ",'k" : "'k") + QByteArray::number(i).rightJustified(width, '0') + "':" + item;
    }
    return object + "}";
}

// What the encoding of a string is, as the checker reads it.
const char* encodingOf(const QJsonValue& value)
{
    return value.toStringView().visit([](auto view) {
        using View = decltype(view);
        if constexpr (std::is_same_v<View, QStringView>) {
            return "UTF-16";
        } else if constexpr (std::is_same_v<View, QLatin1StringView>) {
            return "Latin-1";
        } else {
            return "UTF-8";
        }
    });
}

struct Counts
{
    qint64 work = 0;
    qint64 memory = 0;
    QStringList lines;
};

Counts count(const QJsonObject& schema, const QJsonValue& value)
{
    CompiledSchema compiled;
    const QString error = compileSchema(schema, false, compiled);
    if (!error.isEmpty()) {
        return Counts{-1, -1, {error}};
    }
    CheckStats stats;
    const QStringList lines = checkValue(compiled, value, &stats);
    return Counts{stats.work, stats.peakMemory, lines};
}

const QByteArray A32(32, 'a');
const QByteArray B32(32, 'b');
// U+4E2D eleven times, 33 bytes of UTF-8.
const QByteArray Cjk11 = QByteArray("\xE4\xB8\xAD").repeated(11);
// Sixteen surrogate pairs, written as escapes: 32 code units of UTF-16, 64
// bytes.
const QByteArray Pairs16 = QByteArray("\\ud83d\\ude00").repeated(16);
// Text long enough that a name after it is far from the name before it.
const QByteArray X140(140, 'x');
const QByteArray X600(600, 'x');
// Two members whose names Qt keeps in the other order from their text: "a"
// is read first, and "b", some 620 bytes before it, after. Each value
// walked is 4 lines.
const QByteArray Apart = "{'b': '" + X600 + "', 'a': '" + X600 + "'}";

// An array that a handler built: three strings of 600 bytes, the third
// inserted between the other two, so that it is read away from the first
// (8), and the second beside it.
QJsonArray itemsApart()
{
    QJsonArray array;
    array.append(QString::fromLatin1(X600));
    array.append(QString(600, QLatin1Char('y')));
    array.insert(1, QString(600, QLatin1Char('z')));
    return array;
}

// An object that a handler built: "a" and "b", whose values of 600 bytes
// were set later, "b" first, so that each value lies after both names, the
// value of "a" away from "a" (8), and "b" away from it (8).
QJsonObject membersApart()
{
    QJsonObject object;
    object.insert(QLatin1StringView("a"), 1);
    object.insert(QLatin1StringView("b"), 1);
    object.insert(QLatin1StringView("b"), QString(600, QLatin1Char('y')));
    object.insert(QLatin1StringView("a"), QString::fromLatin1(X600));
    return object;
}

// The memory that the checker counts, from the layouts of what it holds.
// A name of the index of an object's names: its hash and a view of it.
constexpr qint64 IndexBytes = sizeof(quint64) + sizeof(QAnyStringView);
// An item of uniqueItems: its hash and its index, twice (the keys, and the
// room that the sort merges into).
constexpr qint64 UniqueBytes = 2 * (sizeof(quint64) + sizeof(qsizetype));
// A word of a set of annotations, a bit for each member or item.
constexpr qint64 WordBytes = sizeof(quint64);
// A member sorted by name: a view of the name and an iterator.
constexpr qint64 SortedBytes = sizeof(QAnyStringView) + sizeof(QJsonObject::const_iterator);

} // namespace

class TestPhosphorMcpCheckWork : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        // The rows count the code units of strings in the encoding that Qt's
        // parser keeps them in, which this checks first.
        QCOMPARE(encodingOf(parse("'" + A32 + "'")), "Latin-1");
        QCOMPARE(encodingOf(parse("'" + Cjk11 + "'")), "UTF-8");
        QCOMPARE(encodingOf(parse("'" + Pairs16 + "'")), "UTF-16");
    }

    void work_data()
    {
        QTest::addColumn<QByteArray>("schema");
        QTest::addColumn<QByteArray>("value");
        QTest::addColumn<qint64>("work");
        QTest::addColumn<qint64>("memory");
        // The first line of the errors, or empty when the value matches.
        QTest::addColumn<QString>("first");
        const auto row = [](const char* name, const QByteArray& schema, const QByteArray& value, qint64 work,
                            qint64 memory, const QString& first = QString()) {
            QTest::newRow(name) << schema << value << work << memory << first;
        };
        // The root, an array of no item against {}: walked before the check
        // (3, and 2 more for an array), applied to (2, and 2), with the
        // step of the keywords of arrays (1). Each row of an array below
        // costs this and its items.
        row("an empty array", "{}", "[]", (3 + 2) + (2 + 2) + 1, 0);

        // Values walked and subschemas applied: 256 items, each walked (3)
        // and applied to (2), with one keyword (1).
        row("type", "{'items': {'type': 'integer'}}", repeat("1", 256), 10 + 256 * (3 + 2 + 1), 0);
        // Each bound of a number, and multipleOf.
        row("bounds of numbers",
            "{'items': {'minimum': 0, 'maximum': 9, 'exclusiveMinimum': -1, 'exclusiveMaximum': 10, "
            "'multipleOf': 1}}",
            repeat("1", 256), 10 + 256 * (3 + 2 + 5), 0);
        // const and enum: a step each, and a comparison of values for each
        // entry tried.
        row("const of a number", "{'items': {'const': 1}}", repeat("1", 256), 10 + 256 * (3 + 2 + 1 + 1), 0);
        row("enum of numbers", "{'items': {'enum': [5, 1]}}", repeat("1", 256), 10 + 256 * (3 + 2 + 1 + 2), 0);
        // A number tries the numbers of enum alone, and a value of another
        // type the entries of other types alone.
        row("enum of mixed values, numbers", "{'items': {'enum': [true, null, 'x', 1.5]}}", repeat("1.5", 256),
            10 + 256 * (3 + 2 + 1 + 1), 0);
        row("enum of mixed values, null", "{'items': {'enum': [true, 2, 'x', null]}}", repeat("null", 256),
            10 + 256 * (3 + 2 + 1 + 2), 0);
        // A string applied to as an item, taken out of its array (2), its
        // text read for the keywords of strings (1, and 1 for each 64 bytes,
        // at most 4), and compared in its own encoding when the sizes agree:
        // a unit for each 16 code units, rounded up, two for 32 bytes and
        // three for 33. type alone reads no text.
        row("type of strings", "{'items': {'type': 'string'}}", repeat("'" + A32 + "'", 256),
            10 + 256 * (3 + 2 + 2 + 1), 0);
        row("enum of strings", "{'items': {'enum': ['" + B32 + "', '" + A32 + "']}}", repeat("'" + A32 + "'", 256),
            10 + 256 * (3 + 2 + 2 + 1 + 1 + (1 + 2) + (1 + 2)), 0);
        row("const of a string", "{'items': {'const': '" + A32 + "'}}", repeat("'" + A32 + "'", 256),
            10 + 256 * (3 + 2 + 2 + 1 + 1 + 2), 0);
        row("const of a string, UTF-8", "{'items': {'const': '" + Cjk11 + "'}}", repeat("'" + Cjk11 + "'", 256),
            10 + 256 * (3 + 2 + 2 + 1 + 1 + 3), 0);
        // A string of 4096 bytes, whose reads stop at 4 lines, before the
        // check and in it, and 256 units to compare.
        row("const of a long string", "{'items': {'const': '" + QByteArray(4096, 'a') + "'}}",
            repeat("'" + QByteArray(4096, 'a') + "'", 4), 10 + 4 * ((3 + 4) + 2 + 2 + (1 + 4) + 1 + 256), 0);
        // More than eight strings are a table: a name looked up, and a hash
        // and a comparison for each 16 code units, rounded up.
        row("enum of nine strings", "{'items': {'enum': ['1', '2', '3', '4', '5', '6', '7', '8', '" + A32 + "']}}",
            repeat("'" + A32 + "'", 256), 10 + 256 * (3 + 2 + 2 + 1 + 1 + (2 + 2 * 2)), 0);

        // Lengths: counted only when the code units do not decide, once
        // for all the subschemas applied to the string in place. A string
        // in UTF-16 is read through before the check (2 units for 32 code
        // units, rounded up), its 64 bytes are a line each time it is read,
        // and its count is 1 + 2 (1, and 1 for each 16 code units, rounded
        // up). Each string is taken out of the array once (2),
        // whatever the subschemas applied to it in place.
        row("maxLength counted", "{'items': {'maxLength': 20}}", repeat("'" + Pairs16 + "'", 256),
            10 + 256 * ((3 + 1 + 2) + 2 + 2 + (1 + 1) + 1 + (1 + 2)), 0);
        row("maxLength counted once in place",
            "{'items': {'allOf': [{'maxLength': 20}, {'maxLength': 20}, {'maxLength': 20}]}}",
            repeat("'" + Pairs16 + "'", 256), 10 + 256 * ((3 + 1 + 2) + 2 + 2 + 3 * (2 + (1 + 1) + 1) + (1 + 2)), 0);
        // 31 code units of UTF-16 hold at least 16 code points, and are
        // read through in 2.
        row("minLength not counted", "{'items': {'minLength': 16}}",
            repeat("'" + QByteArray("\\ud83d\\ude00").repeated(15) + "a'", 256), 10 + 256 * ((3 + 2) + 2 + 2 + 1 + 1),
            0);
        // 33 bytes of UTF-8 hold at least 9 code points.
        row("minLength not counted, UTF-8", "{'items': {'minLength': 9}}", repeat("'" + Cjk11 + "'", 256),
            10 + 256 * (3 + 2 + 2 + 1 + 1), 0);
        row("maxLength counted, UTF-8", "{'items': {'maxLength': 20}}", repeat("'" + Cjk11 + "'", 256),
            10 + 256 * (3 + 2 + 2 + 1 + 1 + (1 + 3)), 0);
        // Two strings of 64 code units of UTF-16, 128 bytes each (2 lines,
        // and 4 units read through before the check), the second 8 bytes
        // after the end of the first: no line between them.
        row("strings of UTF-16 one after the other", "{'items': {'maxLength': 1000}}",
            repeat("'" + QByteArray("\\u00e9").repeated(64) + "'", 2),
            (3 + 2) + 2 * (3 + 2 + 4) + (2 + 2 + 1) + 2 * (2 + 2 + (1 + 2) + 1), 0);

        // Arrays and objects: more for each (2), walked and applied to, and
        // the step of their keywords.
        row("arrays", "{'items': {'type': 'array'}}", repeat("[]", 256), 10 + 256 * ((3 + 2) + (2 + 2) + 1 + 1), 0);
        row("objects", "{'items': {'type': 'object'}}", repeat("{}", 256), 10 + 256 * ((3 + 2) + (2 + 2) + 1 + 1), 0);
        // A member: visited before the check (2) and in it (2), its name
        // looked up (2, and 2 for each 16 bytes, rounded up).
        row("properties", "{'items': {'properties': {'a': {'type': 'integer'}}}}", repeat("{'a': 1}", 256),
            10 + 256 * ((3 + 2 + 2 + 3) + (2 + 2 + 1) + (2 + 2 + 1) + (2 + 1)), 0);
        row("properties of long names", "{'items': {'properties': {'" + A32 + "': {'type': 'integer'}}}}",
            repeat("{'" + A32 + "': 1}", 256), 10 + 256 * ((3 + 2 + 2 + 3) + (2 + 2 + 1) + (2 + 2 + 2 * 2) + (2 + 1)),
            0);
        // A name in UTF-16 is read through before the check, for an unpaired
        // surrogate (2 units for 19 code units, rounded up).
        row(
            "names in UTF-16", "{}",
            []() -> QByteArray {
                QByteArray object = "{";
                for (int i = 0; i < 256; ++i) {
                    object += (i ? ",'" : "'") + QByteArray("\\u00e9").repeated(16)
                        + QByteArray::number(i).rightJustified(3, '0') + "':1";
                }
                return object + "}";
            }(),
            (3 + 2) + 256 * (2 + 2 + 3) + (2 + 2 + 1), 0);
        row("additionalProperties", "{'additionalProperties': {'type': 'integer'}}", members("1", 256),
            3 + 2 + 2 + 2 + 1 + 256 * ((2 + 3) + 2 + (2 + 1)), 0);
        // A name read away from the last one read of its object: a unit for
        // each 64 bytes between the two, up to 8. Here "b" lies some 620
        // bytes before "a", which is read first (8), before the check and
        // in it. Each value is a string, taken out of the object (2).
        row("names apart", "{'additionalProperties': {'type': 'string'}}", Apart,
            (3 + 2) + 2 * (2 + (3 + 4)) + 8 + (2 + 2 + 1) + 2 * (2 + (2 + 2 + 1)) + 8, 0);
        // A value of 140 bytes between two names, and the header of each
        // text: 2 lines, in the check, which does not read the value. The
        // walk before it reads it, which leaves the next name beside the
        // last text read.
        row("names a value apart", "{'additionalProperties': {'type': 'string'}}",
            "{'a': '" + X140 + "', 'b': '" + X140 + "'}",
            (3 + 2) + 2 * (2 + (3 + 2)) + (2 + 2 + 1) + 2 * (2 + (2 + 2 + 1)) + 2, 0);
        // "b" and a value of 300 or 488 bytes before "a", which is read
        // first: 327 bytes back to "b" (5), or 511 (7), short of the cap of
        // 512, before the check and in it. The value of "a" is a number,
        // and that of "b" a string of 4 lines, taken out of the object (2).
        row("names 327 bytes apart", "{'additionalProperties': {}}", "{'b': '" + QByteArray(300, 'x') + "', 'a': 1}",
            (3 + 2) + 2 * 2 + 5 + 3 + (3 + 4) + (2 + 2 + 1) + 2 * 2 + 5 + 2 + (2 + 2), 0);
        row("names 511 bytes apart", "{'additionalProperties': {}}", "{'b': '" + QByteArray(488, 'x') + "', 'a': 1}",
            (3 + 2) + 2 * 2 + 7 + 3 + (3 + 4) + (2 + 2 + 1) + 2 * 2 + 7 + 2 + (2 + 2), 0);
        // propertyNames: a step, and the subschema applied to the name in
        // place, which reads its text.
        row("propertyNames", "{'propertyNames': {'maxLength': 20}}", members("1", 256),
            3 + 2 + 2 + 2 + 1 + 256 * ((2 + 3) + 2 + (1 + 2 + 1 + 1)), 0);
        // Names of 71 bytes: a line more each time one is visited or read.
        row("propertyNames of long names", "{'propertyNames': {'maxLength': 100}}", members("1", 256, 70),
            3 + 2 + 2 + 2 + 1 + 256 * ((2 + 1 + 3) + (2 + 1) + (1 + 2 + (1 + 1) + 1)), 0);
        // The name is read where its member was visited (8 for "b", which
        // lies before "a"), and not again in the reads of the array that
        // the object is an item of. The array has read nothing yet, and
        // there "b" would be 8 more again, away from "a" in the same
        // storage.
        row("propertyNames of an item, names apart", "{'items': {'propertyNames': {'maxLength': 100}}}",
            "[" + Apart + "]",
            (3 + 2) + (3 + 2) + 2 * (2 + (3 + 4)) + 8 + (2 + 2 + 1) + (2 + 2 + 1) + 2 * (2 + (1 + 2 + 1 + 1)) + 8, 0);

        // Names looked up: the members hashed and sorted once (2 each, half
        // the bits of their count, and 3 for each 16 code units, rounded
        // up), the name found by its hash, which the compile computed (1,
        // and the bits of the count), and compared with the members of its
        // hash (1, and 1 for each 16 code units of the two, or 4 when one of
        // the two is UTF-8, rounded up). The schema's names are UTF-16.
        row("required", "{'items': {'required': ['a']}}", repeat("{'a': 1}", 256),
            10 + 256 * ((3 + 2 + 2 + 3) + (2 + 2 + 1) + (2 + 1) + (1 + 1) + (1 + 1)), IndexBytes);
        row("required of a long name", "{'items': {'required': ['" + A32 + "']}}", repeat("{'" + A32 + "': 1}", 256),
            10 + 256 * ((3 + 2 + 2 + 3) + (2 + 2 + 1) + (2 + 3 * 2) + (1 + 1) + (1 + 4)), IndexBytes);
        // 34 bytes of UTF-8 hashed (7) and compared with 12 code units of
        // UTF-16 (4 * 46 / 16, 12).
        row("required of a long name, UTF-8", "{'items': {'required': ['" + Cjk11 + "x']}}",
            repeat("{'" + Cjk11 + "x': 1}", 256),
            10 + 256 * ((3 + 2 + 2 + 3) + (2 + 2 + 1) + (2 + 7) + (1 + 1) + (1 + 12)), IndexBytes);
        // The index of names apart: 8 more for the second.
        row("required of names apart", "{'required': ['a']}", Apart,
            (3 + 2) + 2 * (2 + (3 + 4)) + 8 + (2 + 2 + 1) + 2 * (2 + 1 + 1) + 8 + (1 + 2) + (1 + 1), 2 * IndexBytes);
        // An object of no member: 1.
        row("required in empty objects", "{'items': {'not': {'required': ['a']}}}", repeat("{}", 256),
            10 + 256 * ((3 + 2) + (2 + 2) + 1 + (2 + 2) + 1 + 1), 0);
        // The index once for the subschemas applied in place: 64 members
        // hashed and sorted (2 + 3 + 1 each), two names looked up (1 + 7,
        // and 1 + 1 to compare).
        row("required in place shares the index", "{'allOf': [{'required': ['k000']}, {'required': ['k001']}]}",
            members("1", 64),
            (3 + 2) + 64 * (2 + 3) + (2 + 2 + 1) + 2 * (2 + 2 + 1) + 64 * (2 + 3 + 1) + 2 * (1 + 7 + (1 + 1)),
            64 * IndexBytes);
        // dependentRequired: a trigger looked up, and its names.
        row("dependentRequired", "{'items': {'dependentRequired': {'a': ['b']}}}", repeat("{'a': 1, 'b': 2}", 256),
            10 + 256 * ((3 + 2 + 2 * (2 + 3)) + (2 + 2 + 1) + 2 * (2 + 1 + 1) + 2 * (1 + 2 + (1 + 1))), 2 * IndexBytes);
        // More triggers than members: each member looked up in the
        // triggers' table (2, and 2 for each 16 code units, rounded up).
        row("dependentRequired of many triggers", "{'items': {'dependentRequired': {'a': ['a'], 'x': [], 'y': []}}}",
            repeat("{'a': 1}", 256), 10 + 256 * ((3 + 2 + 2 + 3) + (2 + 2 + 1) + (2 + 1) + (2 + 1) + (1 + 1) + (1 + 1)),
            IndexBytes);
        row("dependentRequired of many triggers, names apart", "{'dependentRequired': {'a': [], 'x': [], 'y': []}}",
            Apart, (3 + 2) + 2 * (2 + (3 + 4)) + 8 + (2 + 2 + 1) + 2 * (2 + 1) + 8, 0);
        row("dependentRequired of many triggers, long names",
            "{'items': {'dependentRequired': {'" + A32 + "': [], 'x': [], 'y': []}}}",
            repeat("{'" + A32 + "': 1}", 256), 10 + 256 * ((3 + 2 + 2 + 3) + (2 + 2 + 1) + (2 + 2 * 2)), 0);
        row("dependentSchemas", "{'items': {'dependentSchemas': {'a': {'required': ['b']}}}}",
            repeat("{'a': 1, 'b': 2}", 256),
            10
                + 256
                    * ((3 + 2 + 2 * (2 + 3)) + (2 + 2 + 1) + 2 * (2 + 1 + 1) + (1 + 2 + (1 + 1)) + (2 + 2 + 1)
                       + (1 + 2 + (1 + 1))),
            2 * IndexBytes);

        // Errors: 24, the tokens of the pointer that the last error did not
        // build (2 each, and the name decoded), the pointer hashed (1 for
        // each 16 code units), and the tokens of the first ten lines (16
        // each). Here each item's error builds its index.
        row("errors", "{'items': {'type': 'string'}}", repeat("1", 256), 10 + 256 * (3 + 2 + 1 + 24 + 2) + 10 * 16, 0,
            QStringLiteral("/0: must be of type string"));
        row("items false", "{'items': false}", repeat("1", 256), 10 + 256 * (3 + 1 + 24 + 2) + 10 * 16, 0,
            QStringLiteral("/0: is not allowed, because the schema allows no items"));
        // A repeated error builds nothing more. Its pointer is hashed and
        // found.
        row("errors repeated", "{'items': {'allOf': [{'type': 'string'}, {'type': 'string'}]}}", repeat("1", 256),
            10 + 256 * (3 + 2 + 2 * (2 + 1 + 24) + 2) + 10 * 16, 0, QStringLiteral("/0: must be of type string"));
        // Names of 33 bytes of Latin-1, decoded for a pointer (2 + 2) and
        // scanned for "~" and "/" (2), which is 34 code units long (2). The
        // start of each name in a line is decoded and scanned too (2).
        row("errors under long names", "{'additionalProperties': {'type': 'string'}}", members("1", 256, 32),
            (3 + 2) + 256 * (2 + 3) + (2 + 2 + 1) + 256 * (2 + 2 + 1 + 24 + (2 + 2 + 2) + 2) + 10 * (16 + 2), 0,
            QStringLiteral("/k00000000000000000000000000000000: must be of type string"));
        // Names of 35 characters, 32 of them "~" and "/": decoded (2 + 2),
        // and scanned and written again with their escapes (67 / 16), for
        // a pointer 68 code units long (4). The line shows the start of the
        // escaped name.
        row(
            "errors under names to escape", "{'additionalProperties': {'type': 'string'}}",
            []() -> QByteArray {
                QByteArray object = "{";
                for (int i = 0; i < 256; ++i) {
                    object += (i ? ",'" : "'") + QByteArray("~/").repeated(16)
                        + QByteArray::number(i).rightJustified(3, '0') + "':1";
                }
                return object + "}";
            }(),
            (3 + 2) + 256 * (2 + 3) + (2 + 2 + 1) + 256 * (2 + 2 + 1 + 24 + (2 + 2 + 4) + 4) + 10 * (16 + 4), 0,
            QStringLiteral("/") + QStringLiteral("~0~1").repeated(15) + QStringLiteral("...: must be of type string"));
        // Names of 37 bytes of UTF-8, decoded at 4 units for each 16 bytes
        // (2 + 8), for a pointer 16 code units long (1).
        row(
            "errors under long names, UTF-8", "{'additionalProperties': {'type': 'string'}}",
            []() -> QByteArray {
                QByteArray object = "{";
                for (int i = 0; i < 256; ++i) {
                    object += (i ? ",'" : "'") + Cjk11 + QByteArray::number(i).rightJustified(4, '0') + "':1";
                }
                return object + "}";
            }(),
            (3 + 2) + 256 * (2 + 3) + (2 + 2 + 1) + 256 * (2 + 2 + 1 + 24 + (2 + 8) + 1) + 10 * 16, 0,
            QString::fromUtf8("/" + Cjk11 + "0000: must be of type string"));
        // An unpaired surrogate, refused before the check, its UTF-16 read
        // through (1).
        row("unpaired surrogates", "{}", repeat("'\\ud800'", 256), (3 + 2) + 256 * (3 + 1 + 24 + 2) + 10 * 16, 0,
            QStringLiteral("/0: contains an unpaired UTF-16 surrogate"));
        // A name with one, shown with U+FFFD in its stead: its member (2,
        // and 1 to read its UTF-16 through), the error (24), its token (2)
        // and that of the line (16).
        row("unpaired surrogate in a name", "{}", "{'x\\udc00': 1}", (3 + 2) + (2 + 1) + 24 + 2 + 16 + 3, 0,
            QStringLiteral("/x") + QChar(QChar::ReplacementCharacter)
                + QStringLiteral(": the property name contains an unpaired UTF-16 surrogate"));

        // Annotations: the items that no keyword evaluated, a step each...
        row("unevaluatedItems", "{'prefixItems': [{}], 'unevaluatedItems': {'type': 'integer'}}", repeat("1", 256),
            10 + 2 + 256 * 3 + 256 * 1 + 255 * (2 + 1), 0);
        // ...a set of 1024 bits made (1 + 16 words / 4)...
        row("contains and unevaluatedItems", "{'contains': {'type': 'integer'}, 'unevaluatedItems': false}",
            repeat("1", 1024), 10 + 1024 * 3 + 1024 * (2 + 1) + (1 + 4) + 1024 * 1, 16 * WordBytes);
        // ...and merged (1 + 16 / 4) into another, made too.
        row("annotations merged", "{'allOf': [{'contains': {'type': 'integer'}}], 'unevaluatedItems': false}",
            repeat("1", 1024), 10 + 1024 * 3 + (2 + 2 + 1) + 1024 * (2 + 1) + (1 + 4) + (1 + 4) + (1 + 4) + 1024 * 1,
            2 * 16 * WordBytes);
        row("unevaluatedProperties", "{'properties': {'k000': {}}, 'unevaluatedProperties': {'type': 'integer'}}",
            members("1", 256),
            (3 + 2) + 256 * (2 + 3) + (2 + 2 + 1) + 256 * (2 + 2 + 1) + (2 + 2) + 256 * 1 + 255 * (2 + 2 + 1),
            4 * WordBytes);
        // Names of 71 bytes, a line more each time a member is visited.
        row("unevaluatedProperties of long names", "{'unevaluatedProperties': {'type': 'integer'}}",
            members("1", 256, 70), (3 + 2) + 256 * ((2 + 1) + 3) + (2 + 2 + 1) + 256 * (1 + (2 + 1) + 2 + 1), 0);
        row("unevaluatedProperties of names apart", "{'unevaluatedProperties': {'type': 'string'}}", Apart,
            (3 + 2) + 2 * (2 + (3 + 4)) + 8 + (2 + 2 + 1) + 2 * (1 + 2 + (2 + 2 + 1)) + 8, 0);

        // uniqueItems: a hash of each item (1), taken out of the array (1),
        // and a sort of integers (1, and half the bits of the count, for
        // each).
        row(
            "uniqueItems", "{'uniqueItems': true}",
            "[" +
                [] {
                    QByteArray list;
                    for (int i = 0; i < 256; ++i) {
                        list += (i ? "," : "") + QByteArray::number(i);
                    }
                    return list;
                }()
                + "]",
            (3 + 2) + 256 * 3 + (2 + 2 + 1) + 256 * (1 + 1) + 256 * (1 + 4), 256 * UniqueBytes);
        // The keys of one array released before those of the next.
        const QByteArray distinct = "[" + [] {
            QByteArray list;
            for (int i = 0; i < 256; ++i) {
                list += (i ? "," : "") + QByteArray::number(i);
            }
            return list;
        }() + "]";
        row("uniqueItems twice", "{'items': {'uniqueItems': true}}", "[" + distinct + "," + distinct + "]",
            (3 + 2) + 2 * ((3 + 2) + 256 * 3) + (2 + 2 + 1) + 2 * ((2 + 2 + 1) + 256 * (1 + 1) + 256 * (1 + 4)),
            256 * UniqueBytes);
        // Strings hashed (3 for 16 code units), each taken out of its array
        // with a reference to its storage (1 + 2). Two equal items compared:
        // each read away from the last one in the array (2) and taken out
        // of it (1 + 2), the first text that their run of one hash reads
        // anywhere in the array (8), and 1 for 16 code units.
        row("uniqueItems of strings", "{'uniqueItems': true}", repeat("'" + A32 + "'", 256),
            (3 + 2) + 256 * 3 + (2 + 2 + 1) + 256 * (1 + 3 + 3 * 2) + 256 * (1 + 4) + (2 * 2 + 1 + 2 * 3 + 8 + 4) + 24,
            256 * UniqueBytes, QStringLiteral(": must have unique items, and items 0 and 1 are equal"));
        // Two equal strings of 600 bytes with another between them, each
        // hashed in 113 (3 * 600 / 16 rounded up): the second read some 600
        // bytes after the first (8) when the two are compared.
        row("uniqueItems of strings apart", "{'uniqueItems': true}",
            "['" + X600 + "', '" + QByteArray(600, 'y') + "', '" + X600 + "']",
            (3 + 2) + 3 * (3 + 4) + (2 + 2 + 1) + 3 * (1 + 3 + 113) + 3 * (1 + 1) + (2 * 2 + 1 + 2 * 3 + 8 + 8 + 75)
                + 24,
            3 * UniqueBytes, QStringLiteral(": must have unique items, and items 0 and 2 are equal"));
        // Arrays and objects hashed and compared through their own storage
        // (2, and 4 for two), and taken out of their container (1 + 2, or
        // 1 for a number), objects member by member (2, and the hash or the
        // comparison of the name, 1 for a letter).
        row("uniqueItems of objects", "{'uniqueItems': true}", repeat("{'a': 1, 'b': [1]}", 256),
            (3 + 2) + 256 * ((3 + 2) + (2 + 3) + (2 + 3 + 2 + 3)) + (2 + 2 + 1)
                + 256 * ((1 + 3) + 2 + (2 + 1 + (1 + 1)) + (2 + 1 + (1 + 3) + 2 + (1 + 1))) + 256 * (1 + 4)
                + (2 * 2 + (1 + 2 * 3) + 4 + (2 + 1 + (1 + 2 * 1)) + (2 + 1 + (1 + 2 * 3) + 4 + (1 + 2 * 1))) + 24,
            256 * UniqueBytes, QStringLiteral(": must have unique items, and items 0 and 1 are equal"));
        // Names of 32 code units hashed (2 + 6) and compared (2 + 4).
        row("uniqueItems of objects of long names", "{'uniqueItems': true}", repeat("{'" + A32 + "': 1}", 256),
            (3 + 2) + 256 * ((3 + 2) + (2 + 3)) + (2 + 2 + 1) + 256 * ((1 + 3) + 2 + (2 + 3 * 2) + (1 + 1))
                + 256 * (1 + 4) + (2 * 2 + (1 + 2 * 3) + 4 + (2 + 4) + (1 + 2 * 1)) + 24,
            256 * UniqueBytes, QStringLiteral(": must have unique items, and items 0 and 1 are equal"));
        // Strings of three letters hashed (1, 3 * 3 / 16 rounded up) and
        // compared (1, 6 / 16 rounded up).
        row("uniqueItems of short strings", "{'uniqueItems': true}", repeat("'abc'", 256),
            (3 + 2) + 256 * 3 + (2 + 2 + 1) + 256 * (1 + 3 + 1) + 256 * (1 + 4) + (2 * 2 + 1 + 2 * 3 + 8 + 1) + 24,
            256 * UniqueBytes, QStringLiteral(": must have unique items, and items 0 and 1 are equal"));
        // Booleans and null are taken out of the array without a reference
        // to storage (1): hashed (1 + 1), and the two runs of equal items
        // compared (2 * 2, and 1 + 1 + 1), each run anew.
        row("uniqueItems of booleans and null", "{'uniqueItems': true}", "[true, null, false, true, null]",
            (3 + 2) + 5 * 3 + (2 + 2 + 1) + 5 * (1 + 1) + 5 * (1 + 1) + 2 * (2 * 2 + (1 + 1 + 1)) + 24, 5 * UniqueBytes,
            QStringLiteral(": must have unique items, and items 0 and 3 are equal"));
        // Two runs of two strings of 600 bytes, whichever comes first in
        // the order of the hashes: the first text that each run reads is
        // anywhere in the array (8), and the second 1200 bytes after it (8).
        // Hashed in 113 each, compared in 75.
        const QByteArray y600(600, 'y');
        row("uniqueItems of two runs apart", "{'uniqueItems': true}",
            "['" + X600 + "', '" + y600 + "', '" + X600 + "', '" + y600 + "']",
            (3 + 2) + 4 * (3 + 4) + (2 + 2 + 1) + 4 * (1 + 3 + 113) + 4 * (1 + 1) + 2 * (2 * 2 + 1 + 2 * 3 + 8 + 8 + 75)
                + 24,
            4 * UniqueBytes, QStringLiteral(": must have unique items, and items 0 and 2 are equal"));
        // Inside not (applied to, 2 + 2 + 1), uniqueItems compares each run
        // too, though the first repeat decides: the same work whatever the
        // run that comes first.
        row("uniqueItems of two runs apart, verdict", "{'not': {'uniqueItems': true}}",
            "['" + X600 + "', '" + y600 + "', '" + X600 + "', '" + y600 + "']",
            (3 + 2) + 4 * (3 + 4) + 2 * (2 + 2 + 1) + 4 * (1 + 3 + 113) + 4 * (1 + 1)
                + 2 * (2 * 2 + 1 + 2 * 3 + 8 + 8 + 75),
            4 * UniqueBytes);
    }

    void work()
    {
        QFETCH(QByteArray, schema);
        QFETCH(QByteArray, value);
        QFETCH(qint64, work);
        QFETCH(qint64, memory);
        QFETCH(QString, first);
        const QJsonValue parsed = parse(value);
        QVERIFY(!parsed.isUndefined());
        const Counts counts = count(parse(schema).toObject(), parsed);
        QCOMPARE(counts.lines.value(0), first);
        QCOMPARE(counts.work, work);
        QCOMPARE(counts.memory, memory);
    }

    void collisionsAreSorted_data()
    {
        // Items whose hashes are the same with the key 0 (QT_HASH_SEED=0):
        // an integer chosen for each value. The run of one hash is sorted
        // by value, and the equal items found. Each comparison reads its two
        // items away from the last one in the array (2 each), and takes them
        // out of it (1 each, and 2 more for a string or an object).
        QTest::addColumn<QByteArray>("value");
        QTest::addColumn<QByteArray>("twin");
        // The item that repeats the first.
        QTest::addColumn<int>("repeat");
        QTest::addColumn<qint64>("work");
        // The array walked (14) and applied to (5), three hashes of items
        // taken out of it (3 * 2), the sort of the hashes (3 * (1 + 1)), the
        // first two items compared (4 + 3), the run sorted in three
        // comparisons (3 * (4 + 3)), the items compared in their order
        // (2 * (4 + 3)), and the error (24).
        QTest::newRow("numbers") << QByteArray("[0.5, -173880412977153776, 0.5]") << QByteArray("0.5") << 2
                                 << qint64(14 + 5 + 3 * 2 + 3 * (1 + 1) + (4 + 3) + 3 * (4 + 3) + 2 * (4 + 3) + 24);
        // Strings hashed (1 + 3 + 6 each). A comparison of a string and the
        // integer is 4 + 5, and one of the two strings 4 + 7, with 8 more
        // in the sort (by code point), or 4 more after it. The first text
        // that the run reads is anywhere in the array (8).
        QTest::newRow("strings") << "['" + A32 + "', 8151061848735542478, '" + A32 + "']" << "'" + A32 + "'" << 2
                                 << qint64(14 + 5 + (10 + 2 + 10) + 3 * (1 + 1) + (4 + 5 + 8)
                                           + (2 * (4 + 5) + (4 + 7 + 8)) + (4 + 5) + (4 + 7 + 4) + 24);
        // Objects walked (15 each) and hashed (1 + 3 + 2, and 2 + 3 for each
        // member, its name 1), and the two compared in the sort (4 + 7)
        // through their storage (4), their members sorted by name (2, and 2
        // comparisons of 1 + 1, each, twice) and compared (2 + 1 + 3 each),
        // and after it in the same order (4 + 7 + 4, and 2 + 1 + 3 each).
        // The integer and an object compared are 4 + 5.
        QTest::newRow("objects") << QByteArray("[{'a': 2, 'b': 1}, 3459555847912036335, {'b': 1, 'a': 2}]")
                                 << QByteArray("{'a': 2, 'b': 1}") << 2
                                 << qint64((5 + 15 + 3 + 15) + 5 + (2 * (1 + 3 + 2 + 2 * (2 + 3)) + 2) + 3 * (1 + 1)
                                           + (4 + 5)
                                           + (2 * (4 + 5) + (4 + 7 + 4 + 2 * 2 * (2 + 2 * (1 + 1)) + 2 * (2 + 1 + 3)))
                                           + (4 + 5) + (4 + 7 + 4 + 2 * (2 + 1 + 3)) + 24);
        // Four strings of 600 bytes one after the other, X0, X2, X3 and X4
        // by their indices, and the integer: a run of five. Each comparison of two strings is 4 + 7, and 150
        // by code point in the sort or 75 after it, and reads each string
        // away from the last one that the run read (0 or 8 here, and 8 for
        // the first), whether the sort or the comparisons after it read it.
        // The sort reads X3 and X2 (8 + 0), X2 and X0 (0 + 0), and X4 with
        // each of the three others (8 + 8, 8 + 8 and 8 + 0). After it, X0
        // is read away from X3 (8), and X2, X3 and X4 in their order.
        QTest::newRow("strings apart") << "['" + X600 + "', -6792571494992432110, '" + X600 + "', '" + X600 + "', '"
                + X600 + "']" << "'" + X600 + "'"
                                       << 2
                                       << qint64((5 + 4 * 7 + 3) + 5 + (4 * (1 + 3 + 113) + (1 + 1)) + 5 * (1 + 1)
                                                 + (4 + 5) + ((4 + 5) + (4 + 7 + 8 + 150)) + ((4 + 5) + (4 + 7 + 150))
                                                 + ((4 + 5) + 2 * (4 + 7 + 8 + 8 + 150) + (4 + 7 + 8 + 150))
                                                 + ((4 + 5) + (4 + 7 + 8 + 75) + 2 * (4 + 7 + 75)) + 24);
    }

    void collisionsAreSorted()
    {
        QFETCH(QByteArray, value);
        QFETCH(QByteArray, twin);
        QFETCH(int, repeat);
        QFETCH(qint64, work);
        const QJsonArray items = parse(value).toArray();
        Budget budget;
        // The integer collides with the value, with the key 0 only.
        QCOMPARE(hashValue(items.at(1), 0, budget), hashValue(parse(twin), 0, budget));
        QVERIFY(hashValue(items.at(1), 1, budget) != hashValue(parse(twin), 1, budget));

        QHashSeed::setDeterministicGlobalSeed();
        const Counts collided = count(parse("{'uniqueItems': true}").toObject(), items);
        // A random seed again, also under QT_HASH_SEED=0.
        QHashSeed::resetRandomGlobalSeed();
        QCOMPARE(collided.lines,
                 QStringList{QStringLiteral(": must have unique items, and items 0 and %1 are equal").arg(repeat)});
        QCOMPARE(collided.work, work);

        // With a random seed the hashes differ: the same line, and less
        // work.
        QVERIFY(QHashSeed::globalSeed() != 0);
        const Counts apart = count(parse("{'uniqueItems': true}").toObject(), items);
        QCOMPARE(apart.lines, collided.lines);
        QVERIFY2(apart.work < collided.work, qPrintable(QString::number(apart.work)));
    }

    void uniqueItemsWorkWhateverTheSeed()
    {
        // Pairs of equal strings in a random order, short ones and strings
        // of 600 bytes: the runs of one hash come in an order that the key
        // decides, and each charges the same. So does a check of
        // uniqueItems inside not, which a repeat decides.
        QByteArray value = "[";
        std::vector<int> order;
        for (int i = 0; i < 400; ++i) {
            order.push_back(i / 2);
        }
        std::mt19937 random(7);
        std::shuffle(order.begin(), order.end(), random);
        for (std::size_t i = 0; i < order.size(); ++i) {
            const QByteArray text = QByteArray::number(order[i]).rightJustified(order[i] % 2 ? 8 : 600, 'x');
            value += (i ? ",'" : "'") + text + "'";
        }
        const QJsonValue parsed = parse(value + "]");
        QVERIFY(parsed.isArray());
        for (const QByteArray& schema :
             {QByteArray("{'uniqueItems': true}"), QByteArray("{'not': {'uniqueItems': true}}")}) {
            QHashSeed::setDeterministicGlobalSeed();
            const Counts first = count(parse(schema).toObject(), parsed);
            QHashSeed::resetRandomGlobalSeed();
            QVERIFY(first.work > 0);
            for (int i = 0; i < 4; ++i) {
                QHashSeed::resetRandomGlobalSeed();
                const Counts other = count(parse(schema).toObject(), parsed);
                QCOMPARE(other.lines, first.lines);
                QCOMPARE(other.work, first.work);
            }
        }
    }

    void objectsCompareWhateverTheirOrder()
    {
        // An object built in C++ keeps its members in the order of their
        // UTF-16 code units, and a parsed one in that of their code points:
        // U+1F600 before U+FF41 in the first, after it in the second.
        const QString emoji = QString::fromUtf8("\xF0\x9F\x98\x80");
        const QString letter(QChar(0xFF41));
        QJsonObject built;
        built.insert(emoji, 1);
        built.insert(letter, 2);
        const QJsonObject parsed = parse("{'\xEF\xBD\x81': 2, '\xF0\x9F\x98\x80': 1}").toObject();
        QCOMPARE(built.begin().key(), emoji);
        QCOMPARE(parsed.begin().key(), letter);
        Budget budget;
        QVERIFY(equalValues(built, parsed, budget));
        QCOMPARE(compareValues(built, parsed, budget), 0);
        QCOMPARE(hashValue(built, 7, budget), hashValue(parsed, 7, budget));

        QCOMPARE(count(QJsonObject{{QLatin1StringView("const"), built}}, parsed).lines, QStringList());
        QCOMPARE(count(QJsonObject{{QLatin1StringView("enum"), QJsonArray{built}}}, parsed).lines, QStringList());
        QCOMPARE(count(parse("{'uniqueItems': true}").toObject(), QJsonArray{built, parsed}).lines,
                 QStringList{QStringLiteral(": must have unique items, and items 0 and 1 are equal")});
        QCOMPARE(count(parse("{'uniqueItems': true}").toObject(), QJsonArray{built, 5, parsed}).lines,
                 QStringList{QStringLiteral(": must have unique items, and items 0 and 2 are equal")});
        // The object walked (15) and applied to (4, and 1 for the keywords
        // of objects), const (1), the comparison through the storage of the
        // two (1 + 4), the first names that differ (2, and 2 to compare 2
        // code units of UTF-16 with 3 bytes of UTF-8), both objects' members
        // sorted by name (2, and 2 comparisons of 1 + 1, each), and each
        // member compared (2 + 1, and 1 or 2 for its names), its two values
        // taken out of their objects (1 each).
        const Counts counts = count(QJsonObject{{QLatin1StringView("const"), built}}, parsed);
        QCOMPARE(
            counts.work,
            qint64(15 + 4 + 1 + 1 + (1 + 4) + (2 + 2) + 2 * 2 * (2 + 2 * (1 + 1)) + (2 + 1 + 1 + 2) + (2 + 1 + 2 + 2)));
        QCOMPARE(counts.memory, qint64(2 * 2 * SortedBytes));
    }

    void valuesCompared_data()
    {
        // The comparisons of values, by themselves: the units that each
        // charges, and that each term of a length counts. The values are
        // parsed in a list, as the strings and names of arguments are.
        QTest::addColumn<QJsonValue>("first");
        QTest::addColumn<QJsonValue>("second");
        QTest::addColumn<qint64>("equal");
        QTest::addColumn<qint64>("compare");
        QTest::addColumn<qint64>("hash");
        const auto row = [](const char* name, const QJsonValue& first, const QJsonValue& second, qint64 equal,
                            qint64 compare, qint64 hash) {
            QTest::newRow(name) << first << second << equal << compare << hash;
        };
        const auto item = [](const QByteArray& text) {
            return parse("[" + text + "]").toArray().at(0);
        };
        // Strings: equal 1 for each 16 code units of the two, by code point
        // 2, hashed 3 for each 16 of the first, each rounded up.
        row("strings", item("'" + A32 + "'"), item("'" + A32 + "'"), 1 + 4, 1 + 2 * 4, 1 + 3 * 2);
        // Both in UTF-8: 66 bytes, compared as they are (66 / 16, 5), by
        // code point (2 * 66 / 16, 9), and hashed (3 * 33 / 16, 7).
        row("strings, UTF-8", item("'" + Cjk11 + "'"), item("'" + Cjk11 + "'"), 1 + 5, 1 + 9, 1 + 7);
        // Short ones: a unit each, rounded up from 6 / 16, 12 / 16 and
        // 9 / 16, or from 4 * 3 / 16, 2 * 3 / 16 and 3 * 2 / 16 for "é" in
        // UTF-8 and UTF-16.
        row("short strings", item("'abc'"), item("'abc'"), 1 + 1, 1 + 1, 1 + 1);
        row("short strings, UTF-8 and UTF-16", item("'\xC3\xA9'"), item("'\\u00e9'"), 1 + 1, 1 + 1, 1 + 1);
        // Where one is UTF-8 and the other not, Qt decodes as it compares:
        // 4 for each 16 code units of the two (33 bytes and 11 code units,
        // 4 * 44 / 16), and 2 by code point (2 * 44 / 16, 6).
        row("strings, UTF-8 and UTF-16", item("'" + Cjk11 + "'"), item("'" + QByteArray("\\u4e2d").repeated(11) + "'"),
            1 + 11, 1 + 6, 1 + 7);
        // Objects through their storage (1 + 4, or 1 + 2 for the hash), and
        // their members in the same order: 2 each and its names, or both
        // objects sorted by name first for compareValues() (2, and 2
        // comparisons of 1 + 2 * 2, for each member). Each value is taken
        // out of its object: 1, and 2 more for a string, an array or an
        // object.
        const QByteArray object = "{'" + A32 + "': 1, '" + B32 + "': 2}";
        row("objects of long names", item(object), item(object), (1 + 4) + 2 * (2 + 4 + (1 + 2)),
            (1 + 4) + 2 * 2 * (2 + 2 * (1 + 2 * 2)) + 2 * (2 + 2 * 4 + (1 + 2)), (1 + 2) + 2 * (2 + 3 * 2 + (1 + 1)));
        // An object built in C++ keeps its names in UTF-16, in the order of
        // their code units, and a parsed one in UTF-8, in that of their code
        // points. The first names differ (2, and 4 * 69 / 16 rounded up, 18),
        // and equalValues() sorts both too. Each name is 33 or 34 code units,
        // or 35 or 36 bytes, 5 to compare by code point in each comparison of
        // the sort (2 * 33 / 16 to 2 * 36 / 16, rounded up), and 17 or 18 to
        // compare with its twin (9 by code point), and hashed in 7.
        const QString first = QString::fromLatin1(A32) + QChar(0xFF41);
        const QString second = QString::fromLatin1(A32) + QString::fromUtf8("\xF0\x9F\x98\x80");
        QJsonObject built;
        built.insert(first, 1);
        built.insert(second, 2);
        row("objects of long names in two orders", built,
            item("{'" + first.toUtf8() + "': 1, '" + second.toUtf8() + "': 2}"),
            (1 + 4) + (2 + 18) + 2 * 2 * (2 + 2 * (1 + 5)) + (2 + 17 + (1 + 2)) + (2 + 18 + (1 + 2)),
            (1 + 4) + 2 * 2 * (2 + 2 * (1 + 5)) + 2 * (2 + 9 + (1 + 2)), (1 + 2) + 2 * (2 + 7 + (1 + 1)));
        row("arrays", item("[1, [2]]"), item("[1, [2]]"), (1 + 4) + (1 + 2) + (1 + 2 * 3 + 4) + (1 + 2),
            (1 + 4) + (1 + 2) + (1 + 2 * 3 + 4) + (1 + 2), (1 + 2) + (1 + 1) + (1 + 3 + 2) + (1 + 1));
        // Strings of 600 bytes, the third read away from the first in each
        // array (8).
        row("arrays of strings apart", itemsApart(), itemsApart(), (1 + 4) + 3 * (1 + 2 * 3 + 75) + 2 * 8,
            (1 + 4) + 3 * (1 + 2 * 3 + 2 * 75) + 2 * 8, (1 + 2) + 3 * (1 + 3 + 113) + 8);
        // Names apart (8 for the second name of each object), with values
        // of 600 bytes: compared (1 + 75 each, or 1 + 2 * 75 by code point)
        // and hashed (1 + 113, 3 * 600 / 16 rounded up). A name of one
        // letter is 1 to compare or hash. The sort reads the second name
        // away from the first in each of its comparisons too (2 * 8). Each
        // value is read beside its name.
        row("objects of names apart", item(Apart), item(Apart), (1 + 4) + 2 * (2 + 1 + 2 * 3 + 76) + 2 * 8,
            (1 + 4) + 2 * (2 * 2 + 2 * 2 * (1 + 1) + (8 + 2 * 8)) + 2 * (2 + 1 + 2 * 3 + 151) + 2 * 8,
            (1 + 2) + 2 * (2 + 1 + 3 + 1 + 113) + 8);
        // Values set after both names, "b" first: the value of "a" is read
        // away from its name, and the name "b" away from that value (8
        // each, in each object). The value of "b" lies beside its name.
        row("objects of values apart", membersApart(), membersApart(), (1 + 4) + 2 * (2 + 1 + 2 * 3 + 76) + 2 * 2 * 8,
            (1 + 4) + 2 * (2 * 2 + 2 * 2 * (1 + 1)) + 2 * (2 + 1 + 2 * 3 + 151) + 2 * 2 * 8,
            (1 + 2) + 2 * (2 + 1 + 3 + 1 + 113) + 2 * 8);
        // The same names in two orders, their text in the order of the
        // second name first in both: the first names compared (2 + 18),
        // each object sorted, the name read after the other apart (8, and 8
        // in each comparison), and the members compared in the order of
        // their code points, the second name apart in both. The built object
        // keeps its members in the order of its text, which its hash reads.
        QJsonObject builtApart;
        builtApart.insert(second, QString::fromLatin1(X600));
        builtApart.insert(first, QString::fromLatin1(X600));
        row("objects of names apart in two orders", builtApart,
            item("{'" + second.toUtf8() + "': '" + X600 + "', '" + first.toUtf8() + "': '" + X600 + "'}"),
            (1 + 4) + (2 + 18) + 2 * (2 * 2 + 2 * 2 * (1 + 5) + (8 + 2 * 8)) + (2 + 17 + 2 * 3 + 76)
                + (2 + 18 + 2 * 3 + 76) + 2 * 8,
            (1 + 4) + 2 * (2 * 2 + 2 * 2 * (1 + 5) + (8 + 2 * 8)) + 2 * (2 + 9 + 2 * 3 + 151) + 2 * 8,
            (1 + 2) + 2 * (2 + 7 + 1 + 3 + 113));
        // The same, with the values of the built object set after its names,
        // the second first. In the order of code points, its first value is
        // read after the second (8), its second name before the first value
        // (8), and the second value after the first name (1). The parsed
        // object's text is in that order, so only its sort, which reads the
        // names alone, finds the second name away from the first (8, and 8
        // in each comparison).
        QJsonObject builtLater;
        builtLater.insert(second, 1);
        builtLater.insert(first, 1);
        builtLater.insert(second, QString::fromLatin1(X600));
        builtLater.insert(first, QString::fromLatin1(X600));
        row("objects of values apart in two orders", builtLater,
            item("{'" + first.toUtf8() + "': '" + X600 + "', '" + second.toUtf8() + "': '" + X600 + "'}"),
            (1 + 4) + (2 + 18) + (2 * 2 + 2 * 2 * (1 + 5)) + (2 * 2 + 2 * 2 * (1 + 5) + 8 + 2 * 8) + (2 + 17) + (2 + 18)
                + 2 * (1 + 2 * 3 + 75) + (8 + 8 + 1),
            (1 + 4) + (2 * 2 + 2 * 2 * (1 + 5)) + (2 * 2 + 2 * 2 * (1 + 5) + 8 + 2 * 8) + 2 * (2 + 9)
                + 2 * (1 + 2 * 3 + 2 * 75) + (8 + 8 + 1),
            (1 + 2) + 2 * (2 + 7) + 2 * (1 + 3 + 113) + (1 + 8));
    }

    void stringsApart_data()
    {
        QTest::addColumn<QByteArray>("schema");
        QTest::addColumn<qint64>("work");
        // Walked: the array (3 + 2) and each string (3 + 4), the third away
        // from the first (8). Applied to: the array (2 + 2), with the step
        // of the keywords of arrays (1).
        constexpr qint64 Walked = (3 + 2) + 3 * (3 + 4) + 8 + (2 + 2 + 1);
        QTest::newRow("walked") << QByteArray("{}") << Walked;
        // Each string taken out of the array (2) and applied to (2), its
        // text read (1 + 4) and not counted (1), the third away from the
        // first.
        QTest::newRow("items") << QByteArray("{'items': {'maxLength': 1000}}")
                               << Walked + 3 * (2 + 2 + (1 + 4) + 1) + 8;
        QTest::newRow("prefixItems") << QByteArray(
            "{'prefixItems': [{'maxLength': 1000}, {'maxLength': 1000}, {'maxLength': 1000}]}")
                                     << Walked + 3 * (2 + 2 + (1 + 4) + 1) + 8;
        QTest::newRow("contains") << QByteArray("{'contains': {'maxLength': 1000}, 'minContains': 3}")
                                  << Walked + 3 * (2 + 2 + (1 + 4) + 1) + 8;
        // A step more for each item.
        QTest::newRow("unevaluatedItems")
            << QByteArray("{'unevaluatedItems': {'maxLength': 1000}}") << Walked + 3 * (1 + 2 + 2 + (1 + 4) + 1) + 8;
        // Taken out of the array (1 + 2) and hashed (1 + 113, 3 * 600 / 16
        // rounded up), and the hashes sorted (1 + 1 each).
        QTest::newRow("uniqueItems") << QByteArray("{'uniqueItems': true}")
                                     << Walked + 3 * (1 + 3 + 113) + 8 + 3 * (1 + 1);
        // Compared (1 + 4, and 1 + 75 for each string, each of the two
        // taken out of its array, 1 + 2) with the schema's, parsed in the
        // order of its items.
        QTest::newRow("const") << QByteArray("{'const': ['" + X600 + "', '" + QByteArray(600, 'z') + "', '"
                                             + QByteArray(600, 'y') + "']}")
                               << Walked + 1 + (1 + 4) + 3 * (1 + 2 * 3 + 75) + 8;
    }

    void valuesApart_data()
    {
        QTest::addColumn<QByteArray>("schema");
        QTest::addColumn<qint64>("work");
        // Walked: the object (3 + 2), each member (2) and its value (3 +
        // 4), the value of "a" and the name "b" each away from the last
        // text read (8 + 8). Applied to: the object (2 + 2), with the step
        // of the keywords of objects (1).
        constexpr qint64 Walked = (3 + 2) + 2 * (2 + 3 + 4) + 8 + 8 + (2 + 2 + 1);
        // Each member visited (2), its value taken out of the object (2),
        // applied to (2), read (1 + 4) and not counted (1), and the same two
        // reads away (8 + 8).
        QTest::newRow("additionalProperties") << QByteArray("{'additionalProperties': {'maxLength': 1000}}")
                                              << Walked + 2 * (2 + 2 + 2 + (1 + 4) + 1) + 8 + 8;
        // Each name looked up too (2 + 1).
        QTest::newRow("properties") << QByteArray(
            "{'properties': {'a': {'maxLength': 1000}, 'b': {'maxLength': 1000}}}")
                                    << Walked + 2 * (2 + (2 + 1) + 2 + 2 + (1 + 4) + 1) + 8 + 8;
        // A step more for each member.
        QTest::newRow("unevaluatedProperties") << QByteArray("{'unevaluatedProperties': {'maxLength': 1000}}")
                                               << Walked + 2 * (1 + 2 + 2 + 2 + (1 + 4) + 1) + 8 + 8;
    }

    void valuesApart()
    {
        QFETCH(QByteArray, schema);
        QFETCH(qint64, work);
        const Counts counts = count(parse(schema).toObject(), membersApart());
        QCOMPARE(counts.lines, QStringList());
        QCOMPARE(counts.work, work);
    }

    void stringsApart()
    {
        QFETCH(QByteArray, schema);
        QFETCH(qint64, work);
        const Counts counts = count(parse(schema).toObject(), itemsApart());
        QCOMPARE(counts.lines, QStringList());
        QCOMPARE(counts.work, work);
    }

    void valuesCompared()
    {
        QFETCH(QJsonValue, first);
        QFETCH(QJsonValue, second);
        QFETCH(qint64, equal);
        QFETCH(qint64, compare);
        QFETCH(qint64, hash);
        QVERIFY(!first.isUndefined() && !second.isUndefined());
        Budget equalBudget;
        QVERIFY(equalValues(first, second, equalBudget));
        QCOMPARE(equalBudget.work(), equal);
        Budget compareBudget;
        QCOMPARE(compareValues(first, second, compareBudget), 0);
        QCOMPARE(compareBudget.work(), compare);
        Budget hashBudget;
        hashValue(first, 7, hashBudget);
        QCOMPARE(hashBudget.work(), hash);
    }

    void valuesOfTwoTypesCompared()
    {
        // A string and an integer, each taken out of its array: 1, and 1 for
        // each taken out, 2 more for the string, which holds a reference to
        // its storage. Each is charged by its own type, in either order.
        const QJsonValue text = parse("['" + A32 + "']").toArray().at(0);
        const QJsonValue number = parse("[1]").toArray().at(0);
        for (const auto& [first, second] : {std::pair(text, number), std::pair(number, text)}) {
            TextReads readsA;
            TextReads readsB;
            Budget equalBudget;
            QVERIFY(!equalValues(first, second, equalBudget, &readsA, &readsB));
            QCOMPARE(equalBudget.work(), qint64(1 + (1 + 2) + 1));
            Budget compareBudget;
            QVERIFY(compareValues(first, second, compareBudget, &readsA, &readsB) != 0);
            QCOMPARE(compareBudget.work(), qint64(1 + (1 + 2) + 1));
        }
    }

    void textIsTheSameInEveryEncoding()
    {
        // The same text in Latin-1, UTF-8 and UTF-16 (Qt keeps a string of a
        // handler's in Latin-1 only when it is ASCII, but the checker does
        // not count on it): one name, one order, one hash, one length.
        const QString text = QString::fromUtf8("caf\xC3\xA9");
        const QByteArray utf8 = text.toUtf8();
        const QList<QAnyStringView> views{QLatin1StringView("caf\xe9"), QUtf8StringView(utf8), QStringView(text)};
        NameTable table;
        table.insert(QStringLiteral("x"), 1);
        table.insert(text, 7);
        for (const QAnyStringView& view : views) {
            QCOMPARE(table.value(view), 7);
            QCOMPARE(codePoints(view), qsizetype(4));
            QCOMPARE(hashText(view, 3), hashText(text, 3));
            for (const QAnyStringView& other : views) {
                QCOMPARE(compareText(view, other), 0);
            }
        }
        QCOMPARE(table.value(QLatin1StringView("x")), 1);
        QCOMPARE(table.value(QLatin1StringView("caf")), -1);
        // Code points in order, whatever the encoding: U+FF41 before
        // U+1F600, which UTF-16 has the other way.
        const QString letter(QChar(0xFF41));
        const QString emoji = QString::fromUtf8("\xF0\x9F\x98\x80");
        QCOMPARE(compareText(letter, emoji), -1);
        QCOMPARE(compareText(QUtf8StringView("\xEF\xBD\x81"), emoji), -1);
        QCOMPARE(compareText(emoji, QUtf8StringView("\xEF\xBD\x81")), 1);
    }

    void deepChecksStayOnTheStack()
    {
        // A list 1000 levels deep through four subschemas a level: the check
        // stops at 3000 subschemas, or at 4 MiB of its own frames when a
        // build's frames are larger (with sanitizers), before a thread of 5
        // MiB runs out.
        const QJsonObject schema =
            parse(
                "{'$ref': '#/$defs/n', '$defs': {'n': {'type': ['array', 'integer'], 'items': "
                "{'$ref': '#/$defs/b'}}, 'b': {'$ref': '#/$defs/c'}, 'c': {'$ref': '#/$defs/n'}}}")
                .toObject();
        const QJsonValue value = parse(QByteArray("[").repeated(1000) + "1" + QByteArray("]").repeated(1000));
        QVERIFY(value.isArray());
        Counts counts;
        std::unique_ptr<QThread> thread(QThread::create([&] {
            counts = count(schema, value);
        }));
        thread->setStackSize(5 * 1024 * 1024);
        thread->start();
        QVERIFY(thread->wait(QDeadlineTimer(600000)));
        QCOMPARE(counts.lines,
                 QStringList{QStringLiteral(": is too large for the server to check against the schema")});
    }

    void namesAreFoundInAnObjectAddedTo()
    {
        // A parsed object that a handler added to is in no order that
        // QJsonObject::contains() can search: it misses names that the
        // object has (four of these, with Qt 6.12).
        QStringList names;
        QByteArray text = "{";
        for (const char32_t c : {U'\U0001F600', U'\uFF41', U'a', U'\uE000', U'\U00010000', U'\uFFFD', U'b',
                                 U'\U0001F602', U'\uFF00', U'z'}) {
            names.append(QString::fromUcs4(&c, 1));
            text += (names.size() > 1 ? ",'" : "'") + names.last().toUtf8() + "':" + QByteArray::number(names.size());
        }
        QJsonObject object = parse(text + "}").toObject();
        for (const char32_t c : {U'n', U'\U0001F603', U'\uFF02', U'\uE001'}) {
            names.append(QString::fromUcs4(&c, 1));
            object.insert(names.last(), 0);
        }
        QCOMPARE(object.size(), names.size());
        bool missed = false;
        for (const QString& name : std::as_const(names)) {
            missed = missed || !object.contains(name);
        }
        QVERIFY(missed);

        QJsonArray required;
        for (const QString& name : std::as_const(names)) {
            required.append(name);
        }
        QCOMPARE(count(QJsonObject{{QLatin1StringView("required"), required}}, object).lines, QStringList());
        QJsonObject dependent;
        dependent.insert(names.at(11), QJsonArray{names.at(4), names.at(0)});
        QCOMPARE(count(QJsonObject{{QLatin1StringView("dependentRequired"), dependent}}, object).lines, QStringList());
        QJsonObject schemas;
        schemas.insert(names.at(13), false);
        QCOMPARE(count(QJsonObject{{QLatin1StringView("dependentSchemas"), schemas}}, object).lines,
                 QStringList{QStringLiteral(": does not match the schema")});
        QCOMPARE(count(parse("{'required': ['c']}").toObject(), object).lines,
                 QStringList{QStringLiteral("/c: is required")});
    }

    void anIndexThatFailsIsNotMadeAgain()
    {
        // An object whose index of names would hold more than MaxMemory.
        // The first trigger of dependentRequired makes it and fails, and
        // the second does not make it again.
        const qint64 size = MaxMemory / IndexBytes + 1;
        QByteArray text = "{";
        for (qint64 i = 0; i < size; ++i) {
            text += (i ? ",\"" : "\"") + QByteArray::number(i) + "\":0";
        }
        text += '}';
        QJsonParseError error;
        const QJsonValue object = QJsonValue::fromJson(text, &error);
        QCOMPARE(error.error, QJsonParseError::NoError);
        const Counts counts = count(parse("{'dependentRequired': {'0': [], '1': []}}").toObject(), object);
        QCOMPARE(counts.lines,
                 QStringList{QStringLiteral(": is too large for the server to check against the schema")});
        QCOMPARE(counts.memory, size * IndexBytes);
    }

    void errorsBuildTheirPointersOnce()
    {
        // "~" and "/" escaped in the pointer that tells errors apart, so
        // that "/a~1b" and "/a/b" are two places.
        const QJsonObject strings =
            parse("{'additionalProperties': {'type': 'string', 'additionalProperties': {'type': 'string'}}}")
                .toObject();
        QCOMPARE(
            count(strings, parse("{'a/b': 1, 'a': {'b': 1}}")).lines,
            (QStringList{QStringLiteral("/a: must be of type string"), QStringLiteral("/a/b: must be of type string"),
                         QStringLiteral("/a~1b: must be of type string")}));
        // A long name of UTF-8 shown cut, as its start decoded shows it.
        const QByteArray cjk = QByteArray("\xE4\xB8\xAD").repeated(70);
        QCOMPARE(count(strings, parse("{'" + cjk + "': 1}")).lines,
                 QStringList{QString::fromUtf8("/" + cjk.left(3 * 61) + "...: must be of type string")});

        // A pointer longer than any that the report keeps is not built: one
        // line for the value, at once. Names of 400,000 code units, three
        // deep.
        QByteArray value = "1";
        for (int i = 0; i < 3; ++i) {
            value = "{'" + QByteArray(400000, char('a' + i)) + "': " + value + "}";
        }
        const QJsonObject deep = parse("{'additionalProperties': {'$ref': '#'}, 'type': 'object'}").toObject();
        const Counts counts = count(deep, parse(value));
        QCOMPARE(counts.lines,
                 QStringList{QStringLiteral(": does not match the schema, and is too large for the server to list "
                                            "each error")});
        // Walked (3 for each value, 2 more for each object, 2 for each
        // member and 4 for the lines of its name). The root applied to each
        // object (4, type 1, 1 for the keywords of objects, and the member
        // 2 + 4), and the $ref to the two inner ones (4 + 1), and both to
        // the number (2, and 2 + 1). The error: two names decoded and
        // scanned for "~" and "/" (2 + 25000 + 25000 each), and a third
        // token begun (2) that would not fit.
        QCOMPARE(counts.work,
                 qint64(3 * (3 + 2 + 2 + 4) + 3 + 3 * (4 + 1 + 1 + 6) + 2 * (4 + 1) + 2 + (2 + 1) + 24
                        + 2 * (2 + 400000 / 16 + 400000 / 16) + 2));

        // The same with names of UTF-8, which holds a code unit of UTF-16 in
        // three bytes at the least: seven names of 140,000 CJK characters
        // decoded (2, 4 for each 16 bytes, and 1 for each 16 code units
        // scanned), and an eighth of 80,000 that would not fit, begun (2).
        // Counted as bytes, or as a code unit for each four, it would be
        // decoded first.
        QByteArray cjkValue = "1";
        for (int i = 0; i < 8; ++i) {
            const QByteArray name = QByteArray("\xE4\xB8\xAD").repeated(i == 0 ? 80000 : 140000);
            cjkValue = "{'" + name + QByteArray::number(i) + "': " + cjkValue + "}";
        }
        const Counts cjkCounts = count(deep, parse(cjkValue));
        QCOMPARE(cjkCounts.lines, counts.lines);
        QCOMPARE(cjkCounts.work,
                 qint64(8 * (3 + 2 + 2 + 4) + 3 + 8 * (4 + 1 + 1 + 6) + 7 * (4 + 1) + 2 + (2 + 1) + 24
                        + 7 * (2 + 4 * (420001 / 16) + 140001 / 16) + 2));
    }
};

QTEST_GUILESS_MAIN(TestPhosphorMcpCheckWork)
#include "test_phosphor_mcp_checkwork.moc"
