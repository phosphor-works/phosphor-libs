// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Coverage for SchemaValidator: the thin jsoncons wrapper that backs
// schema-validated JSON loading. Pins the contract (valid passes, each
// structural violation is reported with a JSON-Pointer path, a bad schema
// fails closed, 2020-12 by default and `format` asserted) so a jsoncons bump
// or wrapper refactor surfaces here.

#include <PhosphorFsLoader/SchemaValidator.h>

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
        const SchemaValidator validator(QByteArrayLiteral("[1, 2, 3]"));
        QVERIFY(!validator.isValid());
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
        // A `pattern` compiles with the schema, so an invalid ECMAScript regex
        // leaves the validator invalid, and every document is refused. Nothing
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
        // Any JSON value validates, not only objects, and an integer stays one.
        const SchemaValidator validator(QByteArrayLiteral(R"({"type": "integer", "minimum": 2})"));
        QVERIFY(validator.isValid());
        QVERIFY(!validator.validate(QJsonValue(3)).has_value());
        QVERIFY(validator.validate(QJsonValue(3.5)).has_value());
        QVERIFY(validator.validate(QJsonValue(1)).has_value());
        QVERIFY(validator.validate(QJsonValue(QStringLiteral("3"))).has_value());
    }

    void defsAndRefsAre2020()
    {
        // $defs is 2020-12's place for subschemas, and $ref reaches into it.
        const SchemaValidator validator(QByteArrayLiteral(R"({
            "type": "object",
            "properties": { "size": { "$ref": "#/$defs/unit" } },
            "$defs": { "unit": { "type": "number", "minimum": 0, "maximum": 1 } }
        })"));
        QVERIFY(validator.isValid());
        QVERIFY(!validator.validate(parse(R"({"size": 0.5})")).has_value());
        const auto errors = validator.validate(parse(R"({"size": 2})"));
        QVERIFY(errors.has_value());
        QCOMPARE(errors->first().path, QStringLiteral("/size"));
    }

    void prefixItemsAre2020()
    {
        // prefixItems exists only in 2020-12. Under Draft 7 it would be an
        // unknown keyword, and every array would pass.
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

    void declaredDraftIsHonored()
    {
        // A schema that names Draft 7 in $schema is read as Draft 7: there
        // `items` as an array checks each position.
        const SchemaValidator validator(
            QByteArrayLiteral("{\"$schema\": \"http:/"
                              "/json-schema.org/draft-07/schema#\", "
                              "\"type\": \"array\", \"items\": [{\"type\": \"string\"}]}"));
        QVERIFY(validator.isValid());
        QVERIFY(!validator.validate(QJsonDocument::fromJson(R"(["a", 1])").array()).has_value());
        QVERIFY(validator.validate(QJsonDocument::fromJson(R"([1])").array()).has_value());
    }
};

QTEST_MAIN(TestSchemaValidator)
#include "test_schemavalidator.moc"
