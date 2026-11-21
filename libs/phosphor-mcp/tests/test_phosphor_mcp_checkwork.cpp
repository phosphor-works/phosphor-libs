// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// The work and the memory that the checker charges for each shape of schema
// and arguments, counted exactly. The bound of a check (see the README,
// "Errors") holds only while each step is charged what it costs, so each
// charge has a row whose count changes when the charge does, and each shape
// of the README's table of bounds has its own. The checker's sources are
// built into this test, which reads the counts that the library keeps to
// itself.
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

#include <memory>
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
// Sixteen surrogate pairs, written as escapes: 32 code units of UTF-16.
const QByteArray Pairs16 = QByteArray("\\ud83d\\ude00").repeated(16);

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
        // Strings compared in their own encoding when the sizes agree: two
        // units for 32 bytes.
        row("enum of strings", "{'items': {'enum': ['" + B32 + "', '" + A32 + "']}}", repeat("'" + A32 + "'", 256),
            10 + 256 * (3 + 2 + 1 + (1 + 2) + (1 + 2)), 0);
        row("const of a string", "{'items': {'const': '" + A32 + "'}}", repeat("'" + A32 + "'", 256),
            10 + 256 * (3 + 2 + 1 + 2), 0);
        row("const of a string, UTF-8", "{'items': {'const': '" + Cjk11 + "'}}", repeat("'" + Cjk11 + "'", 256),
            10 + 256 * (3 + 2 + 1 + 2), 0);
        // More than eight strings are a table: a hash and a comparison.
        row("enum of nine strings", "{'items': {'enum': ['1', '2', '3', '4', '5', '6', '7', '8', '" + A32 + "']}}",
            repeat("'" + A32 + "'", 256), 10 + 256 * (3 + 2 + 1 + (1 + 2 * 2)), 0);

        // Lengths: counted only when the code units do not decide, once
        // for all the subschemas applied to the string in place. A string
        // in UTF-16 is read through before the check (2 units for 32 code
        // units), and its count is 1 + 2.
        row("maxLength counted", "{'items': {'maxLength': 20}}", repeat("'" + Pairs16 + "'", 256),
            10 + 256 * ((3 + 2) + 2 + 1 + (1 + 2)), 0);
        row("maxLength counted once in place",
            "{'items': {'allOf': [{'maxLength': 20}, {'maxLength': 20}, {'maxLength': 20}]}}",
            repeat("'" + Pairs16 + "'", 256), 10 + 256 * ((3 + 2) + 2 + 3 * (2 + 1) + (1 + 2)), 0);
        // 31 code units of UTF-16 hold at least 16 code points.
        row("minLength not counted", "{'items': {'minLength': 16}}",
            repeat("'" + QByteArray("\\ud83d\\ude00").repeated(15) + "a'", 256), 10 + 256 * ((3 + 1) + 2 + 1), 0);
        // 33 bytes of UTF-8 hold at least 9 code points.
        row("minLength not counted, UTF-8", "{'items': {'minLength': 9}}", repeat("'" + Cjk11 + "'", 256),
            10 + 256 * (3 + 2 + 1), 0);
        row("maxLength counted, UTF-8", "{'items': {'maxLength': 20}}", repeat("'" + Cjk11 + "'", 256),
            10 + 256 * (3 + 2 + 1 + (1 + 2)), 0);

        // Arrays and objects: more for each (2), walked and applied to, and
        // the step of their keywords.
        row("arrays", "{'items': {'type': 'array'}}", repeat("[]", 256), 10 + 256 * ((3 + 2) + (2 + 2) + 1 + 1), 0);
        row("objects", "{'items': {'type': 'object'}}", repeat("{}", 256), 10 + 256 * ((3 + 2) + (2 + 2) + 1 + 1), 0);
        // A member: visited before the check (2) and in it (2), its name
        // hashed and compared (1, and 2 for each 16 bytes).
        row("properties", "{'items': {'properties': {'a': {'type': 'integer'}}}}", repeat("{'a': 1}", 256),
            10 + 256 * ((3 + 2 + 2 + 3) + (2 + 2 + 1) + (2 + 1) + (2 + 1)), 0);
        row("properties of long names", "{'items': {'properties': {'" + A32 + "': {'type': 'integer'}}}}",
            repeat("{'" + A32 + "': 1}", 256), 10 + 256 * ((3 + 2 + 2 + 3) + (2 + 2 + 1) + (2 + 1 + 2 * 2) + (2 + 1)),
            0);
        // A name in UTF-16 is read through before the check, for an unpaired
        // surrogate (1 unit for 19 code units).
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
            (3 + 2) + 256 * (2 + 1 + 3) + (2 + 2 + 1), 0);
        row("additionalProperties", "{'additionalProperties': {'type': 'integer'}}", members("1", 256),
            3 + 2 + 2 + 2 + 1 + 256 * ((2 + 3) + 2 + (2 + 1)), 0);
        // propertyNames: a step, the name as a string (1 unit for each 16
        // code units), and the subschema.
        row("propertyNames", "{'propertyNames': {'maxLength': 20}}", members("1", 256),
            3 + 2 + 2 + 2 + 1 + 256 * ((2 + 3) + 2 + (1 + 2 + 1)), 0);
        row("propertyNames of long names", "{'propertyNames': {'maxLength': 40}}", members("1", 256, 32),
            3 + 2 + 2 + 2 + 1 + 256 * ((2 + 3) + 2 + (1 + 2 + 2 + 1)), 0);

        // Names looked up: the members hashed and sorted once (2 each, and a
        // fourth of the bits of their count), the name hashed (1, the bits
        // of the count, and 2 for each 16 code units) and compared with
        // the members of its hash (1). The index holds 24 bytes a member.
        row("required", "{'items': {'required': ['a']}}", repeat("{'a': 1}", 256),
            10 + 256 * ((3 + 2 + 2 + 3) + (2 + 2 + 1) + 2 + (1 + 1) + 1), 24);
        row("required of a long name", "{'items': {'required': ['" + A32 + "']}}", repeat("{'" + A32 + "': 1}", 256),
            10 + 256 * ((3 + 2 + 2 + 3) + (2 + 2 + 1) + (2 + 2 * 2) + (1 + 1 + 2 * 2) + (1 + 4)), 24);
        // An object of no member: 1.
        row("required in empty objects", "{'items': {'not': {'required': ['a']}}}", repeat("{}", 256),
            10 + 256 * ((3 + 2) + (2 + 2) + 1 + (2 + 2) + 1 + 1), 0);
        // The index once for the subschemas applied in place: 64 members
        // sorted (2 + 1 each), two names looked up (1 + 7 + 1 each).
        row("required in place shares the index", "{'allOf': [{'required': ['k000']}, {'required': ['k001']}]}",
            members("1", 64), (3 + 2) + 64 * (2 + 3) + (2 + 2 + 1) + 2 * (2 + 2 + 1) + 64 * (2 + 1) + 2 * (1 + 7 + 1),
            64 * 24);
        // dependentRequired: a trigger looked up, and its names.
        row("dependentRequired", "{'items': {'dependentRequired': {'a': ['b']}}}", repeat("{'a': 1, 'b': 2}", 256),
            10 + 256 * ((3 + 2 + 2 * (2 + 3)) + (2 + 2 + 1) + 2 * 2 + 2 * (1 + 2 + 1)), 48);
        // More triggers than members: each member hashed (2) in the
        // triggers' table.
        row("dependentRequired of many triggers", "{'items': {'dependentRequired': {'a': ['a'], 'x': [], 'y': []}}}",
            repeat("{'a': 1}", 256), 10 + 256 * ((3 + 2 + 2 + 3) + (2 + 2 + 1) + 2 + 2 + (1 + 1) + 1), 24);
        row("dependentSchemas", "{'items': {'dependentSchemas': {'a': {'required': ['b']}}}}",
            repeat("{'a': 1, 'b': 2}", 256),
            10 + 256 * ((3 + 2 + 2 * (2 + 3)) + (2 + 2 + 1) + 2 * 2 + (1 + 2 + 1) + (2 + 2 + 1) + (1 + 2 + 1)), 48);

        // Errors: 16, and 1 for each token of the place.
        row("errors", "{'items': {'type': 'string'}}", repeat("1", 256), 10 + 256 * (3 + 2 + 1 + 16 + 1), 0,
            QStringLiteral("/0: must be of type string"));
        row("items false", "{'items': false}", repeat("1", 256), 10 + 256 * (3 + 1 + 16 + 1), 0,
            QStringLiteral("/0: is not allowed, because the schema allows no items"));
        // An unpaired surrogate, refused before the check.
        row("unpaired surrogates", "{}", repeat("'\\ud800'", 256), (3 + 2) + 256 * (3 + 16 + 1), 0,
            QStringLiteral("/0: contains an unpaired UTF-16 surrogate"));

        // Annotations: the items that no keyword evaluated, a step each...
        row("unevaluatedItems", "{'prefixItems': [{}], 'unevaluatedItems': {'type': 'integer'}}", repeat("1", 256),
            10 + 2 + 256 * 3 + 255 * (1 + 2 + 1) + 1, 0);
        // ...a set of 1024 bits made (1 + 16 words / 4)...
        row("contains and unevaluatedItems", "{'contains': {'type': 'integer'}, 'unevaluatedItems': false}",
            repeat("1", 1024), 10 + 1024 * 3 + 1024 * (2 + 1) + (1 + 4) + 1024 * 1, 128);
        // ...and merged (1 + 16 / 4) into another, made too.
        row("annotations merged", "{'allOf': [{'contains': {'type': 'integer'}}], 'unevaluatedItems': false}",
            repeat("1", 1024), 10 + 1024 * 3 + (2 + 2 + 1) + 1024 * (2 + 1) + (1 + 4) + (1 + 4) + (1 + 4) + 1024 * 1,
            256);
        row("unevaluatedProperties", "{'properties': {'k000': {}}, 'unevaluatedProperties': {'type': 'integer'}}",
            members("1", 256),
            (3 + 2) + 256 * (2 + 3) + (2 + 2 + 1) + 256 * (2 + 1) + (2 + 2) + 256 * 1 + 255 * (2 + 2 + 1), 32);

        // uniqueItems: 32 bytes for each item, a hash each (1), and a sort
        // of integers (1 + 9 / 4 each).
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
            (3 + 2) + 256 * 3 + (2 + 2 + 1) + 256 * 1 + 256 * (1 + 2), 256 * 32);
        // Strings hashed (2 for 16 code units), objects member by member
        // (2, and the hash of the name), and two equal items compared.
        row("uniqueItems of strings", "{'uniqueItems': true}", repeat("'" + A32 + "'", 256),
            (3 + 2) + 256 * 3 + (2 + 2 + 1) + 256 * (1 + 4) + 256 * (1 + 2) + (1 + 4) + 16, 256 * 32,
            QStringLiteral(": must have unique items, and items 0 and 1 are equal"));
        row("uniqueItems of objects", "{'uniqueItems': true}", repeat("{'a': 1, 'b': [1]}", 256),
            (3 + 2) + 256 * ((3 + 2) + (2 + 3) + (2 + 3 + 2 + 3)) + (2 + 2 + 1) + 256 * (1 + (2 + 1) + (2 + 1 + 1))
                + 256 * (1 + 2) + (1 + (2 + 1) + (2 + 1 + 1)) + 16,
            256 * 32, QStringLiteral(": must have unique items, and items 0 and 1 are equal"));
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
        // by value, each comparison charged 6 for the two items it reads
        // from far apart, and the equal items found.
        QTest::addColumn<QByteArray>("value");
        QTest::addColumn<QByteArray>("twin");
        QTest::addColumn<qint64>("work");
        // The array walked (14) and applied to (5), three hashes (3), the
        // sort of the hashes (3), the first two items compared (1), the
        // run sorted in three comparisons (3 * (6 + 1)), the items compared
        // in their order (2), and the error (16).
        QTest::newRow("numbers") << QByteArray("[0.5, -173880412977153776, 0.5]") << QByteArray("0.5")
                                 << qint64(14 + 5 + 3 + 3 + 1 + 3 * (6 + 1) + 2 + 16);
        // Strings hashed (1 + 4 each), and the two compared in the sort (8
        // more) and after it (4 more).
        QTest::newRow("strings") << "['" + A32 + "', 8151061848735542478, '" + A32 + "']" << "'" + A32 + "'"
                                 << qint64(14 + 5 + (5 + 1 + 5) + 3 + 1 + 3 * (6 + 1) + 8 + 2 + 4 + 16);
        // Objects hashed member by member, and the two compared in the sort
        // with their members sorted by name.
        QTest::newRow("objects") << QByteArray("[{'a': 2, 'b': 1}, 3459555847912036335, {'b': 1, 'a': 2}]")
                                 << QByteArray("{'a': 2, 'b': 1}") << qint64(129);
    }

    void collisionsAreSorted()
    {
        QFETCH(QByteArray, value);
        QFETCH(QByteArray, twin);
        QFETCH(qint64, work);
        const QJsonArray items = parse(value).toArray();
        Budget budget;
        // The integer collides with the value, with the key 0 only.
        QCOMPARE(hashValue(items.at(1), 0, budget), hashValue(parse(twin), 0, budget));
        QVERIFY(hashValue(items.at(1), 1, budget) != hashValue(parse(twin), 1, budget));

        QHashSeed::setDeterministicGlobalSeed();
        const Counts collided = count(parse("{'uniqueItems': true}").toObject(), items);
        QHashSeed::resetRandomGlobalSeed();
        QCOMPARE(collided.lines, QStringList{QStringLiteral(": must have unique items, and items 0 and 2 are equal")});
        QCOMPARE(collided.work, work);

        // With the random seed of the process the hashes differ: the same
        // line, and less work.
        if (QHashSeed::globalSeed() == 0) {
            QSKIP("QT_HASH_SEED=0 keeps the seed 0");
        }
        const Counts apart = count(parse("{'uniqueItems': true}").toObject(), items);
        QCOMPARE(apart.lines, collided.lines);
        QVERIFY2(apart.work < collided.work, qPrintable(QString::number(apart.work)));
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
        // of objects), const (1), the comparison (1), the first names that
        // differ (2), both objects' members sorted by name (8 each), and
        // each member compared (2 + 1).
        const Counts counts = count(QJsonObject{{QLatin1StringView("const"), built}}, parsed);
        QCOMPARE(counts.work, qint64(15 + 4 + 1 + 1 + 1 + 2 + 2 * 8 + 2 * (2 + 1)));
        // Each sorted member is a view of its name and an iterator.
        QCOMPARE(counts.memory, qint64(2 * 2 * (sizeof(QAnyStringView) + sizeof(QJsonObject::const_iterator))));
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
        // object has.
        QJsonObject object = parse("{'\xF0\x9F\x98\x80': 1, '\xEF\xBD\x81': 2, 'a': 3}").toObject();
        object.insert(QStringLiteral("new"), 4);
        object.insert(QString::fromUtf8("\xF0\x9F\x98\x82"), 5);
        object.insert(QString(QChar(0xFF00)), 6);
        const QStringList names{
            QString::fromUtf8("\xF0\x9F\x98\x80"), QString(QChar(0xFF41)), QStringLiteral("a"), QStringLiteral("new"),
            QString::fromUtf8("\xF0\x9F\x98\x82"), QString(QChar(0xFF00))};
        bool missed = false;
        for (const QString& name : names) {
            missed = missed || !object.contains(name);
        }
        if (!missed) {
            qInfo("QJsonObject::contains() finds every name: the checker's own lookup is checked alone");
        }
        QJsonArray required;
        for (const QString& name : names) {
            required.append(name);
        }
        QCOMPARE(count(QJsonObject{{QLatin1StringView("required"), required}}, object).lines, QStringList());
        QJsonObject dependent;
        dependent.insert(names.at(4), QJsonArray{names.at(5), names.at(0)});
        QCOMPARE(count(QJsonObject{{QLatin1StringView("dependentRequired"), dependent}}, object).lines, QStringList());
        QJsonObject schemas;
        schemas.insert(names.at(5), false);
        QCOMPARE(count(QJsonObject{{QLatin1StringView("dependentSchemas"), schemas}}, object).lines,
                 QStringList{QStringLiteral(": does not match the schema")});
        QCOMPARE(count(parse("{'required': ['b']}").toObject(), object).lines,
                 QStringList{QStringLiteral("/b: is required")});
    }
};

QTEST_GUILESS_MAIN(TestPhosphorMcpCheckWork)
#include "test_phosphor_mcp_checkwork.moc"
