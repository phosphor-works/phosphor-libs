// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Coverage for SchemaValidator: the jsoncons wrapper that backs
// schema-validated JSON loading. Pins the contract (valid passes, each
// structural violation is reported with a JSON-Pointer path, a bad schema
// fails closed, 2020-12 only and checked against its meta-schema, `format`
// asserted, what jsoncons would misread refused first, errors bounded) so a
// jsoncons bump or wrapper refactor surfaces here.

#include <PhosphorFsLoader/SchemaValidator.h>

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLoggingCategory>
#include <QTemporaryFile>
#include <QTest>

#include <utility>

using namespace PhosphorFsLoader;

namespace {

// A small representative schema: required keys, a typed/pattern-constrained
// scalar, an array-of-objects, and a nested object with bounded numbers.
// No "$schema" key: the validator reads it as 2020-12. Besides, a "http://" URL
// inside a multi-line raw string trips moc's line-comment scanner (it treats
// "//" as a comment start), which would hide this file's Q_OBJECT class from
// moc.
QByteArray sampleSchema()
{
    return QByteArrayLiteral(R"({
        "type": "object",
        "required": ["id", "zones"],
        "properties": {
            "id": { "type": "string", "pattern": "^\\{.*\\}$" },
            "zones": {
                "type": "array",
                "minItems": 1,
                "items": {
                    "type": "object",
                    "required": ["geometry"],
                    "properties": {
                        "geometry": {
                            "type": "object",
                            "required": ["x", "width"],
                            "additionalProperties": false,
                            "properties": {
                                "x": { "type": "number", "minimum": 0, "maximum": 1 },
                                "width": { "type": "number", "exclusiveMinimum": 0, "maximum": 1 }
                            }
                        }
                    }
                }
            }
        }
    })");
}

QJsonObject parse(const char* json)
{
    return QJsonDocument::fromJson(json).object();
}

} // namespace

class TestSchemaValidator : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void compilesValidSchema()
    {
        const SchemaValidator validator(sampleSchema());
        QVERIFY(validator.isValid());
    }

    void acceptsConformingDocument()
    {
        const SchemaValidator validator(sampleSchema());
        const auto doc = parse(R"({"id":"{abc}","zones":[{"geometry":{"x":0,"width":0.5}}]})");
        QVERIFY(!validator.validate(doc).has_value());
    }

    void rejectsMissingRequiredKey()
    {
        const SchemaValidator validator(sampleSchema());
        const auto doc = parse(R"({"zones":[{"geometry":{"x":0,"width":0.5}}]})");
        QVERIFY(validator.validate(doc).has_value());
    }

    void rejectsEmptyArray()
    {
        const SchemaValidator validator(sampleSchema());
        const auto doc = parse(R"({"id":"{abc}","zones":[]})");
        QVERIFY(validator.validate(doc).has_value());
    }

    void rejectsOutOfRangeNumberWithPath()
    {
        const SchemaValidator validator(sampleSchema());
        const auto doc = parse(R"({"id":"{abc}","zones":[{"geometry":{"x":0,"width":1.5}}]})");
        const auto errors = validator.validate(doc);
        QVERIFY(errors.has_value());
        QVERIFY(!errors->isEmpty());
        // The JSON Pointer should point at the offending node.
        QCOMPARE(errors->first().path, QStringLiteral("/zones/0/geometry/width"));
        QVERIFY(!errors->first().message.isEmpty());
    }

    void rejectsZeroWidthExclusiveMinimum()
    {
        const SchemaValidator validator(sampleSchema());
        const auto doc = parse(R"({"id":"{abc}","zones":[{"geometry":{"x":0,"width":0}}]})");
        QVERIFY(validator.validate(doc).has_value());
    }

    void rejectsPatternViolation()
    {
        const SchemaValidator validator(sampleSchema());
        const auto doc = parse(R"({"id":"no-braces","zones":[{"geometry":{"x":0,"width":0.5}}]})");
        QVERIFY(validator.validate(doc).has_value());
    }

    void rejectsUnknownGeometryKey()
    {
        const SchemaValidator validator(sampleSchema());
        const auto doc = parse(R"({"id":"{abc}","zones":[{"geometry":{"x":0,"width":0.5,"wdith":0.5}}]})");
        QVERIFY(validator.validate(doc).has_value());
    }

    void malformedSchemaIsInvalidAndFailsClosed()
    {
        const SchemaValidator validator(QByteArrayLiteral("{ this is not json"));
        QVERIFY(!validator.isValid());
        // A validator whose schema failed to compile rejects every document
        // rather than passing it through unvalidated.
        const auto doc = parse(R"({"id":"{abc}","zones":[{"geometry":{"x":0,"width":0.5}}]})");
        const auto errors = validator.validate(doc);
        QVERIFY(errors.has_value());
        QCOMPARE(errors->size(), 1);
    }

    void nonObjectSchemaIsInvalid()
    {
        // Valid JSON, so this is the root check refusing it, not the parser.
        const SchemaValidator validator(QByteArrayLiteral("[1, 2, 3]"));
        QVERIFY(!validator.isValid());
        QCOMPARE(validator.validate(QJsonValue()).value().first().message,
                 QStringLiteral("schema root is not a JSON object"));
    }

    void movePreservesCompiledSchema()
    {
        SchemaValidator source(sampleSchema());
        QVERIFY(source.isValid());
        const SchemaValidator moved(std::move(source));
        QVERIFY(moved.isValid());
        const auto doc = parse(R"({"id":"{abc}","zones":[{"geometry":{"x":0,"width":0.5}}]})");
        QVERIFY(!moved.validate(doc).has_value());
    }

    void fromResourceMissingFailsClosed()
    {
        // A missing resource (e.g. a build regression dropping the embedded
        // schema) must fail closed: invalid validator, every document rejected.
        const QLoggingCategory cat("test.schemavalidator.fromresource");
        const auto validator = SchemaValidator::fromResource(QStringLiteral("/no/such/schema.json"), cat);
        QVERIFY(!validator.isValid());
        const auto errors = validator.validate(parse(R"({"id":"{abc}","zones":[{"geometry":{"x":0,"width":0.5}}]})"));
        QVERIFY(errors.has_value());
        QCOMPARE(errors->size(), 1);
    }

    void fromResourceValidFileCompiles()
    {
        QTemporaryFile file;
        QVERIFY(file.open());
        file.write(sampleSchema());
        QVERIFY(file.flush());
        const QLoggingCategory cat("test.schemavalidator.fromresource");
        const auto validator = SchemaValidator::fromResource(file.fileName(), cat);
        QVERIFY(validator.isValid());
        const auto doc = parse(R"({"id":"{abc}","zones":[{"geometry":{"x":0,"width":0.5}}]})");
        QVERIFY(!validator.validate(doc).has_value());
    }

    void moveAssignmentPreservesCompiledSchema()
    {
        SchemaValidator source(sampleSchema());
        SchemaValidator target(QByteArrayLiteral("{}")); // empty schema, matches anything
        target = std::move(source);
        QVERIFY(target.isValid());
        const auto doc = parse(R"({"id":"{abc}","zones":[{"geometry":{"x":0,"width":0.5}}]})");
        QVERIFY(!target.validate(doc).has_value());
        const auto bad = parse(R"({"zones":[]})");
        QVERIFY(target.validate(bad).has_value());
    }

    void invalidPatternFailsClosedWithoutThrowing()
    {
        // A `pattern` that is not an ECMAScript regex leaves the validator
        // invalid (the meta-schema's "regex" format refuses it, and jsoncons
        // would not compile it), and every document is refused. Nothing
        // throws out of the library: callers are built under -fno-exceptions,
        // where an escaping throw would terminate.
        // NOTE: a plain (non-raw) string literal. An unbalanced '[' inside a raw
        // string mis-tracks moc's bracket counting in this multi-raw-string TU
        // and hides the Q_OBJECT class (empty .moc -> undefined vtable). Verified
        // by reverting: the raw form fails to link.
        const SchemaValidator validator(QByteArrayLiteral("{\"type\":\"string\",\"pattern\":\"[\"}"));
        QVERIFY(!validator.isValid());
        const auto errors = validator.validate(QJsonValue(QStringLiteral("abc")));
        QVERIFY(errors.has_value());
        QCOMPARE(errors->size(), 1);
    }

    void scalarDocuments()
    {
        // Any JSON value validates, not only objects.
        const SchemaValidator validator(QByteArrayLiteral(R"({"type": "integer", "minimum": 2})"));
        QVERIFY(validator.isValid());
        QVERIFY(!validator.validate(QJsonValue(3)).has_value());
        QVERIFY(validator.validate(QJsonValue(3.5)).has_value());
        QVERIFY(validator.validate(QJsonValue(1)).has_value());
        QVERIFY(validator.validate(QJsonValue(QStringLiteral("3"))).has_value());
    }

    void integersStayExact()
    {
        // An integer reaches jsoncons as an int64, in the schema and in the
        // document, so 2^53 + 1 is not rounded to 2^53, which a double would
        // do, and the bound holds. 3.0 is an integer, as 2020-12 says.
        const SchemaValidator validator(QByteArrayLiteral(R"({"type": "integer", "maximum": 9007199254740992.0})"));
        QVERIFY(validator.isValid());
        QVERIFY(!validator.validate(QJsonValue(qint64(9007199254740992))).has_value());
        const auto errors = validator.validate(QJsonValue(qint64(9007199254740993)));
        QVERIFY(errors.has_value());
        QVERIFY(errors->first().message.contains(QStringLiteral("9007199254740993")));
        QVERIFY(!validator.validate(QJsonDocument::fromJson("[3.0]").array().at(0)).has_value());
    }

    void numbersPastInt64AreRefused()
    {
        // jsoncons' integer check converts a double to int64, undefined past
        // its range, so a number of magnitude 2^63 or more is refused at its
        // path before jsoncons sees it, whatever the schema says. CI's Python
        // accepts it under "number": the one place the load is stricter.
        const SchemaValidator validator(QByteArrayLiteral(R"({"type": "object"})"));
        QVERIFY(validator.isValid());
        QVERIFY(!validator.validate(parse(R"({"n": 9223372036854775807, "m": -9.2e18})")).has_value());
        for (const char* json : {R"({"n": 9223372036854775808})", R"({"n": -1e19})", R"({"n": 1e300})"}) {
            const auto errors = validator.validate(parse(json));
            QVERIFY2(errors.has_value(), json);
            QCOMPARE(errors->size(), 1);
            QCOMPARE(errors->first().path, QStringLiteral("/n"));
            QVERIFY(errors->first().message.contains(QStringLiteral("out of range")));
        }
        // In the schema too.
        QVERIFY(!SchemaValidator(QByteArrayLiteral(R"({"maximum": 1e19})")).isValid());
    }

    void refReachesIntoDefs()
    {
        // $ref reaches into $defs, 2020-12's place for subschemas. (Plain
        // strings: a '#' inside a raw string can hide this class from moc.)
        const SchemaValidator validator(
            QByteArrayLiteral("{\"type\": \"object\", \"properties\": {\"size\": {\"$ref\": \"#/$defs/unit\"}}, "
                              "\"$defs\": {\"unit\": {\"type\": \"number\", \"minimum\": 0, \"maximum\": 1}}}"));
        QVERIFY(validator.isValid());
        QVERIFY(!validator.validate(parse(R"({"size": 0.5})")).has_value());
        const auto errors = validator.validate(parse(R"({"size": 2})"));
        QVERIFY(errors.has_value());
        QCOMPARE(errors->first().path, QStringLiteral("/size"));
    }

    void prefixItemsAre2020()
    {
        // prefixItems exists only in 2020-12. Draft 7 would ignore it and read
        // items: false as refusing every item, so ["a", 1] would fail there.
        const SchemaValidator validator(QByteArrayLiteral(R"({
            "type": "array",
            "prefixItems": [ { "type": "string" }, { "type": "number" } ],
            "items": false
        })"));
        QVERIFY(validator.isValid());
        QVERIFY(!validator.validate(QJsonDocument::fromJson(R"(["a", 1])").array()).has_value());
        QVERIFY(validator.validate(QJsonDocument::fromJson(R"([1, "a"])").array()).has_value());
        QVERIFY(validator.validate(QJsonDocument::fromJson(R"(["a", 1, 2])").array()).has_value());
    }

    void formatIsAsserted()
    {
        // 2020-12 makes `format` an annotation by default. Here it is checked,
        // so a release date that is not a date is refused.
        const SchemaValidator validator(QByteArrayLiteral(R"({"type": "string", "format": "date"})"));
        QVERIFY(validator.isValid());
        QVERIFY(!validator.validate(QJsonValue(QStringLiteral("2026-11-19"))).has_value());
        QVERIFY(validator.validate(QJsonValue(QStringLiteral("2026-13-45"))).has_value());
        QVERIFY(validator.validate(QJsonValue(QStringLiteral("soon"))).has_value());
    }

    void otherDialectsAreRefused()
    {
        // Only 2020-12 is read: $schema may name it, with or without an empty
        // fragment, and nothing else. jsoncons would read Draft 7 by its own
        // rules, which the meta-schema check and the rest of this contract do
        // not cover. (The URLs are split: "//" inside a string literal trips
        // moc's comment scanner.)
        const QByteArray draft202012 =
            QByteArrayLiteral("https:/") + QByteArrayLiteral("/json-schema.org/draft/2020-12/schema");
        QVERIFY(SchemaValidator("{\"$schema\": \"" + draft202012 + "\"}").isValid());
        QVERIFY(SchemaValidator("{\"$schema\": \"" + draft202012 + "#\"}").isValid());
        const QByteArray refused[] = {
            QByteArrayLiteral("http:/") + QByteArrayLiteral("/json-schema.org/draft/2020-12/schema"),
            QByteArrayLiteral("http:/") + QByteArrayLiteral("/json-schema.org/draft-07/schema#"),
            QByteArrayLiteral("https:/") + QByteArrayLiteral("/json-schema.org/draft/2019-09/schema"),
        };
        for (const QByteArray& uri : refused) {
            const SchemaValidator validator("{\"$schema\": \"" + uri + "\", \"type\": \"string\"}");
            QVERIFY2(!validator.isValid(), uri.constData());
            const auto errors = validator.validate(QJsonValue(QStringLiteral("a")));
            QVERIFY(errors.has_value());
            QVERIFY(errors->first().message.contains(QStringLiteral("/$schema")));
        }
    }

    void otherDraftKeywordsAreRefused()
    {
        // 2020-12 ignores these, so a schema written with them would check
        // less than its author meant. Refused anywhere a subschema sits, each
        // with what 2020-12 has instead.
        const struct
        {
            const char* schema;
            const char* where;
            const char* instead;
        } rows[] = {
            {R"({"definitions": {"a": {}}})", "/definitions", "names it $defs"},
            {R"({"properties": {"a": {"dependencies": {"b": ["c"]}}}})", "/properties/a/dependencies",
             "dependentRequired"},
            {R"({"anyOf": [{}, {"additionalItems": false}]})", "/anyOf/1/additionalItems", "items beside prefixItems"},
            {R"({"items": [{"type": "string"}]})", "/items", "uses prefixItems"},
            {"{\"$defs\": {\"x\": {\"$recursiveRef\": \"#\"}}}", "/$defs/x/$recursiveRef", "$dynamicRef"},
        };
        for (const auto& row : rows) {
            const SchemaValidator validator{QByteArray(row.schema)};
            QVERIFY2(!validator.isValid(), row.schema);
            const QString message = validator.validate(QJsonValue()).value().first().message;
            QVERIFY2(message.startsWith(QStringLiteral("schema refused at ") + QLatin1StringView(row.where)
                                        + QStringLiteral(": ")),
                     qPrintable(message));
            QVERIFY2(message.contains(QLatin1StringView(row.instead)), qPrintable(message));
        }
        // A property may still be called "definitions": a name, not a keyword.
        const SchemaValidator named(QByteArrayLiteral(R"({"properties": {"definitions": {"type": "string"}}})"));
        QVERIFY(named.isValid());
        QVERIFY(named.validate(parse(R"({"definitions": 1})")).has_value());
    }

    void unknownKeywordsAreIgnored()
    {
        // 2020-12 ignores a keyword it does not know, and so does this
        // validator: a misspelt "minimun" checks nothing. CI reads the schemas
        // the same way, so a typo shows there as a check that never fails.
        const SchemaValidator validator(QByteArrayLiteral(R"({"type": "integer", "minimun": 3})"));
        QVERIFY(validator.isValid());
        QVERIFY(!validator.validate(QJsonValue(1)).has_value());
    }

    void schemaIsCheckedAgainstTheMetaSchema()
    {
        // jsoncons compiles a keyword with a value of the wrong kind: {"enum":
        // "x"} compiled, and then every validate() went down its catch path.
        // The 2020-12 meta-schema now refuses each at construction.
        const char* rows[] = {
            R"({"enum": "x"})",
            R"({"maxItems": -1})",
            R"({"type": "strin"})",
            R"({"properties": {"a": {"required": "b"}}})",
        };
        for (const char* schema : rows) {
            const SchemaValidator validator{QByteArray(schema)};
            QVERIFY2(!validator.isValid(), schema);
            const auto errors = validator.validate(QJsonValue(QStringLiteral("x")));
            QVERIFY(errors.has_value());
            QCOMPARE(errors->size(), 1);
            QVERIFY2(errors->first().message.contains(QStringLiteral("meta-schema")),
                     qPrintable(errors->first().message));
        }
    }

    void onlyCheckedFormatsAreAllowed()
    {
        // jsoncons ignores a format it does not know, which would leave the
        // schema checking nothing, and its hostname check is wrong, so a
        // schema naming either is refused rather than half-checked.
        for (const char* format : {"uuid", "hostname", "duration", "idn-email"}) {
            const SchemaValidator validator(QByteArrayLiteral("{\"format\": \"") + format + "\"}");
            QVERIFY2(!validator.isValid(), format);
        }
        for (const char* format :
             {"date-time", "date", "time", "email", "ipv4", "ipv6", "regex", "json-pointer", "uri", "uri-reference"}) {
            const SchemaValidator validator(QByteArrayLiteral("{\"format\": \"") + format + "\"}");
            QVERIFY2(validator.isValid(), format);
        }
    }

    void schemaTextIsReadAsQtReadsIt()
    {
        // Comments are refused, and of two members with one name the last is
        // kept, as Qt keeps it for the documents and Python's json for CI.
        QVERIFY(!SchemaValidator(QByteArrayLiteral("{/* note */ \"type\": \"string\"}")).isValid());
        QVERIFY(!SchemaValidator(QByteArrayLiteral("{\"type\": \"string\"} // note")).isValid());
        const SchemaValidator validator(QByteArrayLiteral(R"({"type": "string", "type": "integer"})"));
        QVERIFY(validator.isValid());
        QVERIFY(!validator.validate(QJsonValue(3)).has_value());
        QVERIFY(validator.validate(QJsonValue(QStringLiteral("x"))).has_value());
    }

    void booleanSchemas()
    {
        // A subschema may be a boolean. The root may not: it must be an object.
        const SchemaValidator validator(QByteArrayLiteral(R"({"properties": {"yes": true, "no": false}})"));
        QVERIFY(validator.isValid());
        QVERIFY(!validator.validate(parse(R"({"yes": 1})")).has_value());
        const auto errors = validator.validate(parse(R"({"no": 1})"));
        QVERIFY(errors.has_value());
        QCOMPARE(errors->first().path, QStringLiteral("/no"));
        QVERIFY(!SchemaValidator(QByteArrayLiteral("true")).isValid());
    }

    void objectsCompareWhateverTheirOrder()
    {
        // Members are compared by name, not by the order they were written in.
        const SchemaValidator validator(QByteArrayLiteral(R"({"const": {"b": 1, "a": {"d": 2, "c": 3}}})"));
        QVERIFY(validator.isValid());
        QVERIFY(!validator.validate(parse(R"({"a": {"c": 3, "d": 2}, "b": 1})")).has_value());
        QVERIFY(validator.validate(parse(R"({"a": {"c": 3, "d": 2}, "b": 2})")).has_value());
    }

    void manyMembersAreLinear()
    {
        // 88000 members, about 1.3 MB. jsoncons' insertion-ordered object
        // searched them linearly on every insertion: 18 s. The sorted one
        // takes tens of milliseconds; the bound only catches a return to the
        // quadratic build.
        QJsonObject presets;
        for (int i = 0; i < 88000; ++i) {
            presets.insert(QStringLiteral("preset%1").arg(i, 6, 10, QChar(u'0')),
                           QJsonObject{{QStringLiteral("a"), i}});
        }
        const SchemaValidator validator(QByteArrayLiteral(R"({"additionalProperties": {"required": ["a"]}})"));
        QVERIFY(validator.isValid());
        QElapsedTimer timer;
        timer.start();
        QVERIFY(!validator.validate(presets).has_value());
        QVERIFY2(timer.elapsed() < 5000, qPrintable(QString::number(timer.elapsed())));
    }

    void nulIsRefused()
    {
        // jsoncons matches `pattern` against a C string and looks member names
        // up through one, both ending at U+0000: "abc\u0000../x" matched
        // ^[a-z]+$, and a name with U+0000 slipped past additionalProperties.
        // Either is refused at its path before jsoncons sees it.
        const SchemaValidator validator(QByteArrayLiteral(
            R"({"properties": {"a": {"type": "string", "pattern": "^[a-z]+$"}}, "additionalProperties": false})"));
        QVERIFY(validator.isValid());
        auto errors = validator.validate(QJsonDocument::fromJson(R"({"a": "abc\u0000../x"})").object());
        QVERIFY(errors.has_value());
        QCOMPARE(errors->size(), 1);
        QCOMPARE(errors->first().path, QStringLiteral("/a"));
        QVERIFY(errors->first().message.contains(QStringLiteral("U+0000")));
        errors = validator.validate(QJsonDocument::fromJson(R"({"a\u0000b": "abc"})").object());
        QVERIFY(errors.has_value());
        QCOMPARE(errors->first().path, QStringLiteral("/a\\u0000b"));
        QVERIFY(errors->first().message.contains(QStringLiteral("member name")));
        // In the schema too.
        QVERIFY(!SchemaValidator(QByteArrayLiteral(R"({"const": "a\u0000"})")).isValid());
    }

    void unpairedSurrogatesAreRefused()
    {
        // UTF-8 has no encoding for an unpaired surrogate: converted, two such
        // names became one, and only the last value was checked.
        const SchemaValidator validator(
            QByteArrayLiteral(R"({"maxProperties": 1, "additionalProperties": {"type": "integer"}})"));
        QVERIFY(validator.isValid());
        auto errors = validator.validate(QJsonDocument::fromJson(R"({"\ud800": "x", "\udc00": 1})").object());
        QVERIFY(errors.has_value());
        QCOMPARE(errors->size(), 2);
        QCOMPARE(errors->at(0).path, QStringLiteral("/\\ud800"));
        QVERIFY(errors->at(0).message.contains(QStringLiteral("surrogate")));
        errors = validator.validate(QJsonDocument::fromJson(R"({"a": ["\udfff"]})").object());
        QVERIFY(errors.has_value());
        QCOMPARE(errors->first().path, QStringLiteral("/a/0"));
        // A pair is a character, and passes.
        const SchemaValidator strings(QByteArrayLiteral(R"({"type": "string"})"));
        QVERIFY(!strings.validate(QJsonDocument::fromJson(R"(["😀"])").array().at(0)).has_value());
    }

    void branchReasonsAreReported()
    {
        // anyOf and oneOf say why each branch failed, at the branch's node.
        const SchemaValidator validator(QByteArrayLiteral(
            R"({"properties": {"a": {"anyOf": [{"type": "string"}, {"minimum": 3}]}}, "additionalProperties": {"type": "string"}})"));
        QVERIFY(validator.isValid());
        const auto errors = validator.validate(parse(R"({"a": 1, "b/c": 2})"));
        QVERIFY(errors.has_value());
        QCOMPARE(errors->size(), 4);
        QCOMPARE(errors->at(0).path, QStringLiteral("/a"));
        QCOMPARE(errors->at(1).path, QStringLiteral("/a"));
        QVERIFY(errors->at(1).message.startsWith(QStringLiteral("anyOf/0: ")));
        QVERIFY(errors->at(2).message.startsWith(QStringLiteral("anyOf/1: ")));
        // jsoncons reports a failed additionalProperties subschema at the
        // object; the error is moved to the member it names.
        QCOMPARE(errors->at(3).path, QStringLiteral("/b~1c"));
    }

    void errorsAreBounded()
    {
        // 150 failing items: the first 100 errors are listed, and one more
        // entry, with no path, counts the rest. Each message is cut to 500
        // characters, so a long string quoted back does not flood the log.
        const SchemaValidator validator(QByteArrayLiteral(R"({"items": {"type": "string", "maxLength": 1}})"));
        QVERIFY(validator.isValid());
        QJsonArray items;
        for (int i = 0; i < 150; ++i) {
            items.append(i);
        }
        auto errors = validator.validate(items);
        QVERIFY(errors.has_value());
        QCOMPARE(errors->size(), 101);
        QCOMPARE(errors->at(99).path, QStringLiteral("/99"));
        QVERIFY(errors->last().path.isEmpty());
        QCOMPARE(errors->last().message, QStringLiteral("and 50 more errors not listed"));

        const SchemaValidator pattern(QByteArrayLiteral(R"({"pattern": "^a+$"})"));
        errors = pattern.validate(QJsonValue(QString(10000, u'b')));
        QVERIFY(errors.has_value());
        QCOMPARE(errors->first().message.size(), 501);
        QVERIFY(errors->first().message.endsWith(QChar(0x2026)));
    }

    void movedFromFailsClosed()
    {
        SchemaValidator source(sampleSchema());
        const SchemaValidator moved(std::move(source));
        // The moved-from validator holds nothing: invalid, and every document
        // refused rather than a null dereference.
        QVERIFY(!source.isValid()); // NOLINT(bugprone-use-after-move)
        const auto errors = source.validate(QJsonValue(1)); // NOLINT(bugprone-use-after-move)
        QVERIFY(errors.has_value());
        QCOMPARE(errors->size(), 1);
        QVERIFY(moved.isValid());
    }

    void logSchemaErrorsPrintsTheCountWithoutAPath()
    {
        // One line per error, "(root)" for the document itself, and the count
        // validate() appends past its 100th error printed as it is.
        QList<SchemaValidator::Error> errors{SchemaValidator::Error{QString(), QStringLiteral("root")}};
        for (int i = 1; i < 100; ++i) {
            errors.append(SchemaValidator::Error{QStringLiteral("/x"), QStringLiteral("m")});
        }
        errors.append(SchemaValidator::Error{QString(), QStringLiteral("and 2 more errors not listed")});
        QTest::ignoreMessage(QtWarningMsg, "  \"(root)\": \"root\"");
        for (int i = 1; i < 100; ++i) {
            QTest::ignoreMessage(QtWarningMsg, "  \"/x\": \"m\"");
        }
        QTest::ignoreMessage(QtWarningMsg, "  \"and 2 more errors not listed\"");
        const QLoggingCategory category("test.schemavalidator.log");
        logSchemaErrors(category, errors);
    }
};

QTEST_MAIN(TestSchemaValidator)
#include "test_schemavalidator.moc"
