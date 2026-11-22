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

#include <thread>
#include <utility>
#include <vector>

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
        // its range, so a number outside int64 is refused at its path before
        // jsoncons sees it, wherever it is and whatever the schema says there
        // (here nothing). CI's Python accepts it under "number": the one
        // place the load is stricter. -2^63 is within int64 and is an integer.
        const SchemaValidator validator(QByteArrayLiteral(R"({"type": "object"})"));
        QVERIFY(validator.isValid());
        QVERIFY(!validator.validate(parse(R"({"n": 9223372036854775807, "m": -9.2e18})")).has_value());
        const SchemaValidator integers(QByteArrayLiteral(R"({"items": {"type": "integer"}})"));
        QVERIFY(!integers.validate(QJsonDocument::fromJson("[-9223372036854775808]").array()).has_value());
        const struct
        {
            const char* json;
            const char* shown;
        } rows[] = {
            {R"({"n": 9223372036854775808})", "9.223372036854776e+18"},
            {R"({"n": -9223372036854777856})", "-9.223372036854778e+18"},
            {R"({"n": -1e19})", "-1e+19"},
            {R"({"n": 1e300})", "1e+300"},
        };
        for (const auto& row : rows) {
            const auto errors = validator.validate(parse(row.json));
            QVERIFY2(errors.has_value(), row.json);
            QCOMPARE(errors->size(), 1);
            QCOMPARE(errors->first().path, QStringLiteral("/n"));
            QCOMPARE(errors->first().message,
                     QStringLiteral("number %1 is outside the int64 range (-2^63 to 2^63 - 1), which is refused")
                         .arg(QLatin1StringView(row.shown)));
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
            {R"({"definitions": {"a": {}}})", "/definitions",
             "definitions is a Draft 7 keyword, which 2020-12 names $defs"},
            {R"({"properties": {"a": {"dependencies": {"b": ["c"]}}}})", "/properties/a/dependencies",
             "dependencies is a Draft 7 keyword, which 2020-12 splits into dependentRequired and dependentSchemas"},
            {R"({"anyOf": [{}, {"additionalItems": false}]})", "/anyOf/1/additionalItems",
             "additionalItems is a Draft 7 and 2019-09 keyword, which 2020-12 replaces with items beside prefixItems"},
            {R"({"items": [{"type": "string"}]})", "/items",
             "an array under items is the Draft 7 and 2019-09 form, which 2020-12 writes as prefixItems"},
            {"{\"$defs\": {\"x\": {\"$recursiveRef\": \"#\"}}}", "/$defs/x/$recursiveRef",
             "$recursiveRef is a Draft 2019-09 keyword, which 2020-12 replaces with $dynamicRef and $dynamicAnchor "
             "(and this validator supports neither)"},
        };
        for (const auto& row : rows) {
            const SchemaValidator validator{QByteArray(row.schema)};
            QVERIFY2(!validator.isValid(), row.schema);
            const QString message = validator.validate(QJsonValue()).value().first().message;
            QCOMPARE(message,
                     QStringLiteral("schema refused at ") + QLatin1StringView(row.where) + QStringLiteral(": ")
                         + QLatin1StringView(row.instead));
        }
        // A property may still be called "definitions": a name, not a keyword.
        const SchemaValidator named(QByteArrayLiteral(R"({"properties": {"definitions": {"type": "string"}}})"));
        QVERIFY(named.isValid());
        QVERIFY(named.validate(parse(R"({"definitions": 1})")).has_value());
    }

    void refLoopsAreRefused()
    {
        // jsoncons follows a $ref each time it validates through it: one that
        // leads back to its own schema through subschemas that apply to the
        // same value recursed until the stack overflowed, at every validate().
        // Refused at construction, at a $ref on the loop. (Plain strings: a
        // '#' inside a raw string can hide this class from moc.)
        const struct
        {
            const char* schema;
            const char* where;
        } rows[] = {
            {"{\"$ref\": \"#\"}", "/$ref"},
            {"{\"$defs\": {\"a\": {\"$ref\": \"#/$defs/a\"}}}", "/$defs/a/$ref"},
            {"{\"$defs\": {\"a\": {\"$ref\": \"#/$defs/b\"}, \"b\": {\"$ref\": \"#/$defs/a\"}}}", "/$defs/a/$ref"},
            {"{\"anyOf\": [{\"type\": \"string\"}, {\"$ref\": \"#\"}]}", "/anyOf/1/$ref"},
            {"{\"$defs\": {\"a\": {\"$anchor\": \"x\", \"not\": {\"$ref\": \"#x\"}}}}", "/$defs/a/not/$ref"},
            {"{\"$defs\": {\"a\": {\"allOf\": [{\"$ref\": \"#/$defs/b\"}]}, \"b\": {\"oneOf\": [{\"$ref\": "
             "\"#/$defs/a\"}]}}}",
             "/$defs/a/allOf/0/$ref"},
        };
        for (const auto& row : rows) {
            const SchemaValidator validator{QByteArray(row.schema)};
            QVERIFY2(!validator.isValid(), row.schema);
            QCOMPARE(validator.validate(QJsonValue(1)).value().first().message,
                     QStringLiteral("schema refused at ") + QLatin1StringView(row.where)
                         + QStringLiteral(": $ref leads back to the same schema without descending into the "
                                          "document, so validating would never end"));
        }

        // The search keeps a stack of its own: a loop of 20000 $defs does not
        // recurse on the machine's.
        QJsonObject defs;
        for (int i = 0; i < 20000; ++i) {
            defs.insert(QStringLiteral("d%1").arg(i),
                        QJsonObject{{QStringLiteral("$ref"), QStringLiteral("#/$defs/d%1").arg((i + 1) % 20000)}});
        }
        const SchemaValidator chain(QJsonDocument(QJsonObject{{QStringLiteral("$defs"), defs}}).toJson());
        QVERIFY(!chain.isValid());
        QVERIFY(chain.validate(QJsonValue(1)).value().first().message.contains(QStringLiteral("never end")));
    }

    void descendingRefsAreAllowed()
    {
        // A $ref back to its own schema through a member or an item descends
        // into the document, which is finite: allowed, and checked to the
        // document's depth. So is a $ref that nothing applies in place.
        const struct
        {
            const char* schema;
            const char* document;
            const char* firstError; // null: the document passes
        } rows[] = {
            {"{\"type\": \"object\", \"properties\": {\"next\": {\"$ref\": \"#\"}}}",
             R"({"next": {"next": {"next": 5}}})", "/next/next/next"},
            {"{\"type\": \"array\", \"items\": {\"$ref\": \"#\"}}", R"([[[["x"]]]])", "/0/0/0/0"},
            {"{\"type\": \"array\", \"prefixItems\": [{\"$ref\": \"#\"}]}", R"([[[]]])", nullptr},
            {"{\"type\": \"array\", \"prefixItems\": [{\"$ref\": \"#\"}]}", R"([[[1]]])", "/0/0/0"},
            {"{\"type\": \"object\", \"patternProperties\": {\"^a\": {\"$ref\": \"#\"}}}", R"({"a": {"ab": 1}})",
             "/a/ab"},
            {"{\"$defs\": {\"a\": {\"$ref\": \"#\"}}, \"type\": \"integer\"}", R"(1)", nullptr},
        };
        for (const auto& row : rows) {
            const SchemaValidator validator{QByteArray(row.schema)};
            QVERIFY2(validator.isValid(), row.schema);
            const auto errors =
                validator.validate(QJsonDocument::fromJson(QByteArray("[") + row.document + "]").array().at(0));
            if (!row.firstError) {
                QVERIFY2(!errors.has_value(), row.document);
                continue;
            }
            QVERIFY2(errors.has_value(), row.document);
            QCOMPARE(errors->first().path, QLatin1StringView(row.firstError));
        }
    }

    void refsThatCannotBeFollowedAreRefused()
    {
        // Every $ref jsoncons follows is followed here too, to find a loop, so
        // one this validator cannot follow is refused, and so is what would
        // change how a $ref resolves: $dynamicRef, $dynamicAnchor, an $id
        // below the root, an $anchor defined twice.
        const struct
        {
            const char* schema;
            const char* refusal;
        } rows[] = {
            {"{\"$ref\": \"other.json\"}",
             "/$ref: $ref \"other.json\" does not start with \"#\", and this validator resolves only \"#\", "
             "\"#/<JSON pointer>\" and \"#<$anchor>\" within the schema"},
            {"{\"$id\": \"urn:example:s\", \"$defs\": {\"a\": {}}, \"properties\": {\"x\": {\"$ref\": "
             "\"urn:example:s#/$defs/a\"}}}",
             "/properties/x/$ref: $ref \"urn:example:s#/$defs/a\" names this schema through its $id, which this "
             "validator does not resolve (write \"#/$defs/a\")"},
            {"{\"$id\": \"urn:example:s#\", \"properties\": {\"x\": {\"$ref\": \"urn:example:s\"}}}",
             "/properties/x/$ref: $ref \"urn:example:s\" names this schema through its $id, which this validator "
             "does not resolve (write \"#\")"},
            {"{\"$id\": \"urn:example:s\", \"properties\": {\"x\": {\"$ref\": \"urn:example:st#\"}}}",
             "/properties/x/$ref: $ref \"urn:example:st#\" does not start with \"#\", and this validator resolves "
             "only \"#\", \"#/<JSON pointer>\" and \"#<$anchor>\" within the schema"},
            {"{\"$defs\": {\"a b\": {}}, \"$ref\": \"#/$defs/a%20b\"}",
             "/$ref: $ref \"#/$defs/a%20b\" is percent-encoded, which this validator does not resolve"},
            {"{\"properties\": {\"a\": {\"$ref\": \"#/$defs/b\"}}}",
             "/properties/a/$ref: $ref \"#/$defs/b\" names no subschema of this schema"},
            {"{\"$ref\": \"#nowhere\"}", "/$ref: $ref \"#nowhere\" names no subschema of this schema"},
            {"{\"$defs\": {\"a\": {\"$anchor\": \"x\"}, \"b\": {\"$anchor\": \"x\"}}}",
             "/$defs/b/$anchor: $anchor \"x\" is defined at /$defs/a too"},
            {"{\"$dynamicRef\": \"#meta\"}",
             "/$dynamicRef: $dynamicRef is not supported, because this validator resolves only $ref and $anchor"},
            {"{\"$defs\": {\"a\": {\"$dynamicAnchor\": \"meta\"}}}",
             "/$defs/a/$dynamicAnchor: $dynamicAnchor is not supported, because this validator resolves only $ref and "
             "$anchor"},
            {"{\"properties\": {\"a\": {\"$id\": \"urn:other\"}}}",
             "/properties/a/$id: $id below the root is not supported, because this validator resolves $ref within one "
             "resource"},
            // jsoncons does not compile contentSchema (2020-12 makes it an
            // annotation), so nothing below it can be named, by pointer or
            // by anchor.
            {"{\"contentSchema\": {\"type\": \"string\"}, \"properties\": {\"x\": {\"$ref\": \"#/contentSchema\"}}}",
             "/properties/x/$ref: $ref \"#/contentSchema\" names a subschema of contentSchema, which jsoncons does "
             "not compile (2020-12 makes contentSchema an annotation)"},
            {"{\"contentSchema\": {\"$anchor\": \"a\"}, \"properties\": {\"x\": {\"$ref\": \"#a\"}}}",
             "/properties/x/$ref: $ref \"#a\" names a subschema of contentSchema, which jsoncons does not compile "
             "(2020-12 makes contentSchema an annotation)"},
            // jsoncons files items beside prefixItems as additionalItems, and
            // a member of patternProperties or dependentSchemas without the
            // member's name, so nothing below those can be named by its
            // pointer. jsoncons threw on the first four, and resolved the
            // last three to another subschema: a wrong verdict, and for the
            // last two a $ref to the `not` that holds it, which recursed until the
            // stack overflowed at validate().
            {"{\"prefixItems\": [{}], \"items\": {\"properties\": {\"a\": {}}}, \"properties\": {\"x\": "
             "{\"$ref\": \"#/items/properties/a\"}}}",
             "/properties/x/$ref: $ref \"#/items/properties/a\" names a subschema below items beside prefixItems by "
             "its pointer, which jsoncons files under another name and does not resolve (an $anchor there "
             "resolves)"},
            {"{\"$defs\": {\"d\": {\"prefixItems\": [{}], \"items\": {}}}, \"properties\": {\"x\": {\"$ref\": "
             "\"#/$defs/d/items\"}}}",
             "/properties/x/$ref: $ref \"#/$defs/d/items\" names a subschema below items beside prefixItems by its "
             "pointer, which jsoncons files under another name and does not resolve (an $anchor there resolves)"},
            {"{\"patternProperties\": {\"a\": {}}, \"properties\": {\"x\": {\"$ref\": \"#/patternProperties/a\"}}}",
             "/properties/x/$ref: $ref \"#/patternProperties/a\" names a subschema below a patternProperties member "
             "by its pointer, which jsoncons files under another name and does not resolve (an $anchor there "
             "resolves)"},
            {"{\"dependentSchemas\": {\"k\": {}}, \"properties\": {\"x\": {\"$ref\": \"#/dependentSchemas/k\"}}}",
             "/properties/x/$ref: $ref \"#/dependentSchemas/k\" names a subschema below a dependentSchemas member by "
             "its pointer, which jsoncons files under another name and does not resolve (an $anchor there "
             "resolves)"},
            {"{\"patternProperties\": {\"additionalProperties\": {\"type\": \"string\"}, \"^x\": "
             "{\"additionalProperties\": {\"type\": \"integer\"}}}, \"properties\": {\"r\": {\"$ref\": "
             "\"#/patternProperties/additionalProperties\"}}}",
             "/properties/r/$ref: $ref \"#/patternProperties/additionalProperties\" names a subschema below a "
             "patternProperties member by its pointer, which jsoncons files under another name and does not resolve "
             "(an $anchor there resolves)"},
            {"{\"patternProperties\": {\"not\": {}, \"^x\": {\"not\": {\"$ref\": \"#/patternProperties/not\"}}}}",
             "/patternProperties/^x/not/$ref: $ref \"#/patternProperties/not\" names a subschema below a "
             "patternProperties member by its pointer, which jsoncons files under another name and does not resolve "
             "(an $anchor there resolves)"},
            {"{\"dependentSchemas\": {\"not\": {}, \"k\": {\"not\": {\"$ref\": \"#/dependentSchemas/not\"}}}}",
             "/dependentSchemas/k/not/$ref: $ref \"#/dependentSchemas/not\" names a subschema below a "
             "dependentSchemas member by its pointer, which jsoncons files under another name and does not resolve "
             "(an $anchor there resolves)"},
            // jsoncons percent-decodes a location to read it as a JSON
            // pointer. When a "%7E" or "%7e" in it decodes to a "~" followed
            // by neither "0" nor "1", it reads the location as an anchor's
            // name instead. Before this check, it then compiled the member
            // apart from the schema, where a $ref named another subschema
            // than here, and the first row's recursed until the stack
            // overflowed at validate(). Such a name is refused wherever it is.
            {"{\"properties\": {\"a%7E2\": {\"properties\": {\"x\": {\"$ref\": \"#\"}}}}}",
             "/properties/a%7E2: jsoncons percent-encodes the name \"a%7E2\" into a location that decodes to a \"~\" "
             "followed by neither \"0\" nor \"1\", so it reads that location as an anchor's name, not a JSON pointer "
             "(rename the member)"},
            {"{\"$defs\": {\"d%7e\": {}}, \"properties\": {\"y\": {\"type\": \"integer\"}}}",
             "/$defs/d%7e: jsoncons percent-encodes the name \"d%7e\" into a location that decodes to a \"~\" "
             "followed by neither \"0\" nor \"1\", so it reads that location as an anchor's name, not a JSON pointer "
             "(rename the member)"},
            // jsoncons writes a byte below 0x10 among the last two characters
            // with one hex digit, so U+0007 then "E" or "e" becomes "%7E" or
            // "%7e".
            {"{\"properties\": {\"\\u0007E\": {\"properties\": {\"x\": {\"$ref\": \"#\"}}}}}",
             "/properties/\\u0007E: jsoncons percent-encodes the name \"\\u0007E\" into a location that decodes to a "
             "\"~\" followed by neither \"0\" nor \"1\", so it reads that location as an anchor's name, not a JSON "
             "pointer (rename the member)"},
            {"{\"$defs\": {\"x\\u0007e\": {}}}",
             "/$defs/x\\u0007e: jsoncons percent-encodes the name \"x\\u0007e\" into a location that decodes to a "
             "\"~\" followed by neither \"0\" nor \"1\", so it reads that location as an anchor's name, not a JSON "
             "pointer (rename the member)"},
        };
        for (const auto& row : rows) {
            const SchemaValidator validator{QByteArray(row.schema)};
            QVERIFY2(!validator.isValid(), row.schema);
            QCOMPARE(validator.validate(QJsonValue(1)).value().first().message,
                     QStringLiteral("schema refused at ") + QString::fromUtf8(row.refusal));
        }

        // An $anchor below items beside prefixItems resolves, and so does a
        // pointer into items without prefixItems or into prefixItems.
        const SchemaValidator items(QByteArrayLiteral(
            "{\"prefixItems\": [{\"type\": \"integer\"}], \"items\": {\"$anchor\": \"s\", \"type\": "
            "\"string\"}, \"properties\": {\"a\": {\"$ref\": \"#s\"}, \"b\": {\"$ref\": \"#/prefixItems/0\"}, "
            "\"c\": {\"$ref\": \"#/$defs/plain/items\"}}, \"$defs\": {\"plain\": {\"items\": {\"maximum\": "
            "1}}}}"));
        QVERIFY(items.isValid());
        QVERIFY(!items.validate(parse(R"({"a": "x", "b": 1, "c": 1})")).has_value());
        const auto itemErrors = items.validate(parse(R"({"a": 1, "b": "x", "c": 2})"));
        QVERIFY(itemErrors.has_value());
        QCOMPARE(itemErrors->size(), 3);

        // So does an $anchor below a member of patternProperties or of
        // dependentSchemas.
        const SchemaValidator members(
            QByteArrayLiteral("{\"patternProperties\": {\"^p\": {\"$anchor\": \"p\", \"type\": \"string\"}}, "
                              "\"dependentSchemas\": {\"k\": {\"$anchor\": \"k\", \"required\": [\"j\"]}}, "
                              "\"properties\": {\"a\": {\"$ref\": \"#p\"}, \"b\": {\"$ref\": \"#k\"}}}"));
        QVERIFY(members.isValid());
        QVERIFY(!members.validate(parse(R"({"a": "x", "b": {"j": 1}})")).has_value());
        const auto memberErrors = members.validate(parse(R"({"a": 1, "b": {}})"));
        QVERIFY(memberErrors.has_value());
        QCOMPARE(memberErrors->size(), 2);

        // A name whose "%7E" decodes to "~0" or "~1" reads as a pointer, and
        // is kept. So does a literal "~", which jsoncons escapes to "~0"
        // first, before a "%32".
        const SchemaValidator tilde(
            QByteArrayLiteral("{\"properties\": {\"a%7E0\": {\"properties\": {\"x\": {\"type\": \"integer\"}}}}}"));
        QVERIFY(tilde.isValid());
        QVERIFY(tilde.validate(parse(R"({"a%7E0": {"x": "s"}})")).has_value());
        const SchemaValidator literal(
            QByteArrayLiteral("{\"properties\": {\"~%32\": {\"properties\": {\"x\": {\"type\": \"integer\"}}}}}"));
        QVERIFY(literal.isValid());
        QVERIFY(literal.validate(parse(R"({"~%32": {"x": "s"}})")).has_value());

        // An $id at the root is allowed, and a $ref by pointer or by anchor
        // resolves within the schema.
        const SchemaValidator validator(QByteArrayLiteral(
            "{\"$id\": \"urn:example:schema\", \"properties\": {\"a\": {\"$ref\": \"#/$defs/unit\"}, \"b\": "
            "{\"$ref\": \"#small\"}}, \"$defs\": {\"unit\": {\"maximum\": 1}, \"small\": {\"$anchor\": \"small\", "
            "\"maximum\": 2}}}"));
        QVERIFY(validator.isValid());
        QVERIFY(!validator.validate(parse(R"({"a": 1, "b": 2})")).has_value());
        const auto errors = validator.validate(parse(R"({"a": 2, "b": 3})"));
        QVERIFY(errors.has_value());
        QCOMPARE(errors->size(), 2);
        QCOMPARE(errors->at(0).path, QStringLiteral("/a"));
        QCOMPARE(errors->at(1).path, QStringLiteral("/b"));
    }

    void everySubschemaKeywordIsWalked()
    {
        // The walks over the schema reach every place a subschema sits: a
        // Draft 7 keyword is refused below each, and a {"$ref": "#"} there is
        // a loop exactly where the subschema applies to the value itself.
        // then and else apply to it only beside an if, which each schema
        // here has.
        enum class Shape {
            Single,
            Map,
            Array
        };
        const struct
        {
            const char* keyword;
            Shape shape;
            bool inPlace;
        } rows[] = {
            {"additionalProperties", Shape::Single, false},
            {"contains", Shape::Single, false},
            {"contentSchema", Shape::Single, false},
            {"else", Shape::Single, true},
            {"if", Shape::Single, true},
            {"items", Shape::Single, false},
            {"not", Shape::Single, true},
            {"propertyNames", Shape::Single, false},
            {"then", Shape::Single, true},
            {"unevaluatedItems", Shape::Single, false},
            {"unevaluatedProperties", Shape::Single, false},
            {"$defs", Shape::Map, false},
            {"dependentSchemas", Shape::Map, true},
            {"patternProperties", Shape::Map, false},
            {"properties", Shape::Map, false},
            {"allOf", Shape::Array, true},
            {"anyOf", Shape::Array, true},
            {"oneOf", Shape::Array, true},
            {"prefixItems", Shape::Array, false},
        };
        const auto schemaWith = [](const QString& keyword, Shape shape, const QJsonObject& subschema) {
            QJsonValue value = subschema;
            if (shape == Shape::Map) {
                value = QJsonObject{{QStringLiteral("k"), subschema}};
            } else if (shape == Shape::Array) {
                value = QJsonArray{subschema};
            }
            QJsonObject schema{{QStringLiteral("if"), QJsonObject()}};
            schema.insert(keyword, value);
            return QJsonDocument(schema).toJson();
        };
        for (const auto& row : rows) {
            const QString keyword = QString::fromUtf8(row.keyword);
            const QString at = QStringLiteral("/") + keyword
                + (row.shape == Shape::Map         ? QStringLiteral("/k")
                       : row.shape == Shape::Array ? QStringLiteral("/0")
                                                   : QString());

            const SchemaValidator draft7(
                schemaWith(keyword, row.shape, QJsonObject{{QStringLiteral("definitions"), QJsonObject()}}));
            QVERIFY2(!draft7.isValid(), row.keyword);
            QVERIFY2(
                draft7.validate(QJsonValue(1))
                    .value()
                    .first()
                    .message.startsWith(QStringLiteral("schema refused at ") + at + QStringLiteral("/definitions: ")),
                row.keyword);

            const SchemaValidator loop(
                schemaWith(keyword, row.shape, QJsonObject{{QStringLiteral("$ref"), QStringLiteral("#")}}));
            QVERIFY2(loop.isValid() != row.inPlace, row.keyword);
            if (row.inPlace) {
                QCOMPARE(loop.validate(QJsonValue(1)).value().first().message.left(at.size() + 24),
                         QStringLiteral("schema refused at ") + at + QStringLiteral("/$ref:"));
            }
        }

        // Without an if, 2020-12 ignores then and else, and so does jsoncons:
        // a $ref back under them is no loop, and checks nothing.
        const SchemaValidator then(QByteArrayLiteral("{\"then\": {\"$ref\": \"#\"}, \"type\": \"integer\"}"));
        QVERIFY(then.isValid());
        QVERIFY(!then.validate(QJsonValue(1)).has_value());
        const SchemaValidator otherwise(QByteArrayLiteral("{\"else\": {\"$ref\": \"#\"}, \"type\": \"integer\"}"));
        QVERIFY(otherwise.isValid());
        const auto errors = otherwise.validate(QJsonValue(QStringLiteral("x")));
        QVERIFY(errors.has_value());
        QCOMPARE(errors->size(), 1);
        QCOMPARE(errors->first().message, QStringLiteral("Expected integer, found string"));
    }

    void additionalPropertiesSaysWhy()
    {
        // jsoncons reports only that a member failed an additionalProperties
        // subschema. The member is checked again against that subschema, so
        // the reasons follow, at their own nodes and prefixed with the
        // keyword, as the presets maps of the shipped schemas need.
        const SchemaValidator validator(QByteArrayLiteral(R"({
            "properties": {
                "presets": {
                    "type": "object",
                    "additionalProperties": {
                        "type": "object",
                        "maxProperties": 2,
                        "additionalProperties": { "type": ["number", "string"] }
                    }
                }
            }
        })"));
        QVERIFY(validator.isValid());
        const auto errors = validator.validate(parse(R"({"presets": {"a": {"x": true, "y": 1, "z": 2}, "b/c": 5}})"));
        QVERIFY(errors.has_value());
        const QList<SchemaValidator::Error> expected{
            {QStringLiteral("/presets/a"), QStringLiteral("Additional property 'a' found but was invalid.")},
            {QStringLiteral("/presets/a/x"),
             QStringLiteral("additionalProperties: Additional property 'x' found but was invalid.")},
            {QStringLiteral("/presets/a/x"),
             QStringLiteral("additionalProperties: additionalProperties: Expected number, or string, found boolean")},
            {QStringLiteral("/presets/a"),
             QStringLiteral("additionalProperties: Maximum number of properties is 2 but found 3")},
            {QStringLiteral("/presets/b~1c"), QStringLiteral("Additional property 'b/c' found but was invalid.")},
            {QStringLiteral("/presets/b~1c"), QStringLiteral("additionalProperties: Expected object, found integer")},
        };
        QCOMPARE(errors->size(), expected.size());
        for (qsizetype i = 0; i < expected.size(); ++i) {
            QCOMPARE(errors->at(i).path, expected.at(i).path);
            QCOMPARE(errors->at(i).message, expected.at(i).message);
        }

        // Reached through a $ref, with an $id at the root, and with a branch.
        const SchemaValidator referred(QByteArrayLiteral(
            "{\"$id\": \"urn:example:presets\", \"properties\": {\"p\": {\"$ref\": \"#/$defs/map\"}}, \"$defs\": "
            "{\"map\": {\"additionalProperties\": {\"anyOf\": [{\"type\": \"string\"}, {\"minimum\": 3}]}}}}"));
        QVERIFY(referred.isValid());
        const auto branches = referred.validate(parse(R"({"p": {"k": 1}})"));
        QVERIFY(branches.has_value());
        QCOMPARE(branches->size(), 4);
        QCOMPARE(branches->at(3).path, QStringLiteral("/p/k"));
        QCOMPARE(branches->at(3).message,
                 QStringLiteral("additionalProperties: anyOf/1: Minimum value is 3 but found 1"));

        // Still at most 100 errors listed. One error for "_", then 150
        // failing members, each with its reason: the 100th error kept is
        // m049's own line, its reason is not looked for, and the rest of
        // what jsoncons reported is counted, m050 to m149.
        QJsonObject many{{QStringLiteral("_"), 1}};
        for (int i = 0; i < 150; ++i) {
            many.insert(QStringLiteral("m%1").arg(i, 3, 10, QChar(u'0')), i);
        }
        const SchemaValidator strings(QByteArrayLiteral(
            R"({"properties": {"_": {"type": "string"}}, "additionalProperties": {"type": "string"}})"));
        const auto bounded = strings.validate(many);
        QVERIFY(bounded.has_value());
        QCOMPARE(bounded->size(), 101);
        QCOMPARE(bounded->at(0).path, QStringLiteral("/_"));
        QCOMPARE(bounded->at(98).path, QStringLiteral("/m048"));
        QCOMPARE(bounded->at(98).message, QStringLiteral("additionalProperties: Expected string, found integer"));
        QCOMPARE(bounded->at(99).path, QStringLiteral("/m049"));
        QCOMPARE(bounded->at(99).message, QStringLiteral("Additional property 'm049' found but was invalid."));
        QCOMPARE(bounded->last().message, QStringLiteral("and 100 more errors not listed"));

        // Rechecks go four additionalProperties deep, and no deeper: each
        // level rechecks the members below the last, so a document nested
        // without end cannot multiply the work without end.
        const SchemaValidator nested(
            QByteArrayLiteral("{\"type\": \"object\", \"additionalProperties\": {\"$ref\": \"#\"}}"));
        QVERIFY(nested.isValid());
        const auto deep = nested.validate(parse(R"({"a": {"b": {"c": {"d": {"e": {"f": 1}}}}}})"));
        QVERIFY(deep.has_value());
        QCOMPARE(deep->size(), 5);
        QCOMPARE(deep->last().path, QStringLiteral("/a/b/c/d/e"));
        QCOMPARE(deep->last().message,
                 QStringLiteral("additionalProperties: additionalProperties: additionalProperties: "
                                "additionalProperties: Additional property 'e' found but was "
                                "invalid."));

        // Below dependentSchemas, jsoncons appends the name of the member
        // that triggered it to the location of the object it validates, so
        // the location names that member's value. A member that failed there
        // keeps jsoncons' line only, and no reason is read from the wrong
        // value: the member "a" failed, an object, and "/a/a" holds 5, which
        // the recheck reported as "Expected string, found integer".
        const SchemaValidator dependent(
            QByteArrayLiteral(R"({"dependentSchemas": {"a": {"additionalProperties": {"type": "string"}}}})"));
        QVERIFY(dependent.isValid());
        const auto triggered = dependent.validate(parse(R"({"a": {"a": 5}})"));
        QVERIFY(triggered.has_value());
        QCOMPARE(triggered->size(), 1);
        QCOMPARE(triggered->first().path, QStringLiteral("/a/a"));
        QCOMPARE(triggered->first().message, QStringLiteral("Additional property 'a' found but was invalid."));
        // A property named "dependentSchemas" is not the keyword: its member
        // is rechecked.
        const SchemaValidator named(
            QByteArrayLiteral(R"({"properties": {"dependentSchemas": {"additionalProperties": {"type": "string"}}}})"));
        QVERIFY(named.isValid());
        const auto reasons = named.validate(parse(R"({"dependentSchemas": {"k": 1}})"));
        QVERIFY(reasons.has_value());
        QCOMPARE(reasons->size(), 2);
        QCOMPARE(reasons->last().path, QStringLiteral("/dependentSchemas/k"));
        QCOMPARE(reasons->last().message, QStringLiteral("additionalProperties: Expected string, found integer"));

        // jsoncons files some subschemas under one location and keeps the
        // first, so a recheck of any could compile another and give its
        // reason ("Expected integer" here, where the reason is the length).
        // A member that fails one keeps jsoncons' line only. Such are two
        // names it percent-encodes alike ("a{b" and "a%7Bb", and "a" with
        // U+0001 then "0" against "a" with U+0010, since it writes a byte
        // below 0x10 near the end with one hex digit), also below items
        // beside prefixItems, which it files as additionalItems, and two
        // patternProperties members, which it files without their names.
        const auto keepsOnlyTheLine = [](const char* schema, const char* document) {
            const SchemaValidator validator{QByteArray(schema)};
            QVERIFY2(validator.isValid(), schema);
            const QJsonDocument parsed = QJsonDocument::fromJson(document);
            const auto lines =
                validator.validate(parsed.isArray() ? QJsonValue(parsed.array()) : QJsonValue(parsed.object()));
            QVERIFY2(lines.has_value(), document);
            QCOMPARE(lines->size(), 1);
            QCOMPARE(lines->first().message, QStringLiteral("Additional property 'k' found but was invalid."));
        };
        const char* const encodedAlike =
            "{\"properties\": {\"a{b\": {\"additionalProperties\": {\"maxLength\": 1}}, \"a%7Bb\": "
            "{\"additionalProperties\": {\"type\": \"integer\"}}}}";
        keepsOnlyTheLine(encodedAlike, "{\"a{b\": {\"k\": \"abc\"}}");
        keepsOnlyTheLine(encodedAlike, "{\"a%7Bb\": {\"k\": \"abc\"}}");
        keepsOnlyTheLine(
            "{\"properties\": {\"a\\u00010\": {\"additionalProperties\": {\"type\": \"integer\"}}, "
            "\"a\\u0010\": {\"additionalProperties\": {\"maxLength\": 1}}}}",
            "{\"a\\u0010\": {\"k\": \"abc\"}}");
        keepsOnlyTheLine(
            "{\"prefixItems\": [{}], \"items\": {\"properties\": {\"a{b\": {\"additionalProperties\": "
            "{\"maxLength\": 1}}, \"a%7Bb\": {\"additionalProperties\": {\"type\": \"integer\"}}}}}",
            "[0, {\"a{b\": {\"k\": \"abc\"}}]");
        keepsOnlyTheLine(
            "{\"patternProperties\": {\"^a\": {\"additionalProperties\": {\"type\": \"integer\"}}, "
            "\"^b\": {\"additionalProperties\": {\"maxLength\": 1}}}}",
            "{\"bx\": {\"k\": \"abc\"}}");

        // Names that decode alike but that jsoncons keeps apart, since it
        // keeps an existing %XX as written, are each rechecked.
        const SchemaValidator apart(
            QByteArrayLiteral("{\"properties\": {\"aA\": {\"additionalProperties\": {\"maxLength\": 1}}, \"a%41\": "
                              "{\"additionalProperties\": {\"type\": \"integer\"}}}}"));
        QVERIFY(apart.isValid());
        const auto apartReasons = apart.validate(parse("{\"aA\": {\"k\": \"abc\"}}"));
        QVERIFY(apartReasons.has_value());
        QCOMPARE(apartReasons->size(), 2);
        QCOMPARE(apartReasons->last().message,
                 QStringLiteral("additionalProperties: Number of characters must be at most 1"));
    }

    void rechecksAreKeptAcrossCallsAndThreads()
    {
        // The subschemas compiled to recheck failed members are kept for
        // later validate() calls, sixteen at most, and a validator is shared
        // across threads. Twenty failing locations, each its own subschema,
        // so that a validate() uses kept ones and compiles others: four
        // threads validating at once, five times each, on a validator that
        // has kept nothing yet, all report what a validator of their own
        // reports. (Run under TSan for the races.)
        QJsonObject properties;
        QJsonObject document;
        for (int i = 0; i < 20; ++i) {
            const QString name = QStringLiteral("p%1").arg(i, 2, 10, QChar(u'0'));
            properties.insert(name,
                              QJsonObject{{QStringLiteral("additionalProperties"),
                                           QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                                       {QStringLiteral("maxLength"), i}}}});
            document.insert(name, QJsonObject{{QStringLiteral("k"), QStringLiteral("abcdefghijklmnopqrstuvwxyz")}});
        }
        const QByteArray schema = QJsonDocument(QJsonObject{{QStringLiteral("properties"), properties}}).toJson();
        const auto expected = SchemaValidator(schema).validate(document);
        QVERIFY(expected.has_value());
        QCOMPARE(expected->size(), 40);
        QCOMPARE(expected->at(39).path, QStringLiteral("/p19/k"));
        QCOMPARE(expected->at(39).message,
                 QStringLiteral("additionalProperties: Number of characters must be at most 19"));

        const SchemaValidator shared(schema);
        QVERIFY(shared.isValid());
        std::vector<std::optional<QList<SchemaValidator::Error>>> results(20);
        // std::thread, joined: TSan sees the join, and not QThread::wait(),
        // which happens inside Qt, built without it.
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&shared, &document, &results, t] {
                for (int n = 0; n < 5; ++n) {
                    results[std::size_t(t * 5 + n)] = shared.validate(document);
                }
            });
        }
        for (std::thread& thread : threads) {
            thread.join();
        }
        for (const auto& result : results) {
            QVERIFY(result.has_value());
            QCOMPARE(result->size(), expected->size());
            for (qsizetype i = 0; i < expected->size(); ++i) {
                QCOMPARE(result->at(i).path, expected->at(i).path);
                QCOMPARE(result->at(i).message, expected->at(i).message);
            }
        }
        // And on the same thread, once all are kept or compiled.
        const auto again = shared.validate(document);
        QVERIFY(again.has_value());
        QCOMPARE(again->at(39).message, expected->at(39).message);
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
        // The 2020-12 meta-schema now refuses each at construction, with the
        // most specific reason it gives. Below properties and $defs that is
        // the reason the member failed, not only that it was invalid. Below
        // dependentRequired it is only that, and so it is below four maps one
        // inside another, where the rechecks for the reason stop: a schema
        // of maps nested 500 deep took 108 s to refuse in Release, and takes
        // 5 s.
        const struct
        {
            const char* schema;
            const char* reason;
        } rows[] = {
            {R"({"enum": "x"})", "/enum: Expected array, found string"},
            {R"({"maxItems": -1})", "/maxItems: Minimum value is 0 but found -1"},
            {R"({"type": "strin"})", "/type: 'strin' is not a valid enum value. (and 1 more)"},
            {R"({"properties": {"a": {"required": "b"}}})", "/properties/a/required: Expected array, found string"},
            {R"({"$defs": {"a": {"properties": {"b": {"minLength": "1"}}}}})",
             "/$defs/a/properties/b/minLength: Expected integer, found string"},
            {R"({"patternProperties": {"^a": {"type": 1}}})",
             "/patternProperties/^a/type: '1' is not a valid enum value. (and 1 more)"},
            {R"({"dependentSchemas": {"a": {"not": 1}}})",
             "/dependentSchemas/a/not: Expected object, or boolean, found integer (and 7 more)"},
            {R"({"dependentRequired": {"a": "b"}})",
             "/dependentRequired/a: Additional property 'a' found but was invalid."},
            {R"({"properties": {"a": {"properties": {"a": {"properties": {"a": {"properties": {"a": {"type": 1}}}}}}}}})",
             "/properties/a/properties/a/properties/a/properties/a/type: '1' is not a valid enum value. (and 1 more)"},
            {R"({"properties": {"a": {"properties": {"a": {"properties": {"a": {"properties": {"a": {"properties": )"
             R"({"a": {"type": 1}}}}}}}}}}})",
             "/properties/a/properties/a/properties/a/properties/a/properties/a: Additional property 'a' found but "
             "was invalid."},
        };
        for (const auto& row : rows) {
            const SchemaValidator validator{QByteArray(row.schema)};
            QVERIFY2(!validator.isValid(), row.schema);
            const auto errors = validator.validate(QJsonValue(QStringLiteral("x")));
            QVERIFY(errors.has_value());
            QCOMPARE(errors->size(), 1);
            QCOMPARE(errors->first().message,
                     QStringLiteral("schema does not conform to the 2020-12 meta-schema at ")
                         + QString::fromUtf8(row.reason));
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
        // 88000 members, 2.36 MB as compact JSON. jsoncons' insertion-ordered
        // object searched them linearly on every insertion: 18 s in Release.
        // The sorted one takes about 70 ms there, and 5 to 10 s under ASan on
        // a loaded machine, so the bound only catches a return to the
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
        QVERIFY2(timer.elapsed() < 15000, qPrintable(QString::number(timer.elapsed())));
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
        QCOMPARE(errors->size(), 5);
        QCOMPARE(errors->at(0).path, QStringLiteral("/a"));
        QCOMPARE(errors->at(1).path, QStringLiteral("/a"));
        QVERIFY(errors->at(1).message.startsWith(QStringLiteral("anyOf/0: ")));
        QVERIFY(errors->at(2).message.startsWith(QStringLiteral("anyOf/1: ")));
        // jsoncons reports a failed additionalProperties subschema at the
        // object; the error is moved to the member it names, and its reason
        // follows.
        QCOMPARE(errors->at(3).path, QStringLiteral("/b~1c"));
        QCOMPARE(errors->at(4).path, QStringLiteral("/b~1c"));
        QCOMPARE(errors->at(4).message, QStringLiteral("additionalProperties: Expected string, found integer"));
    }

    void errorsAreBounded()
    {
        // 150 failing items: the first 100 errors are listed, and one more
        // entry, with no path, counts the rest. A long message keeps its first
        // 500 UTF-16 code units and gains an ellipsis, 501 in all, so a long
        // string quoted back does not flood the log.
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
        // 101 failing items: the count is of one.
        while (items.size() > 101) {
            items.removeLast();
        }
        errors = validator.validate(items);
        QVERIFY(errors.has_value());
        QCOMPARE(errors->last().message, QStringLiteral("and 1 more error not listed"));

        const SchemaValidator pattern(QByteArrayLiteral(R"({"pattern": "^a+$"})"));
        errors = pattern.validate(QJsonValue(QString(10000, u'b')));
        QVERIFY(errors.has_value());
        QCOMPARE(errors->first().message.size(), 501);
        QVERIFY(errors->first().message.endsWith(QChar(0x2026)));

        // A refused schema's message holds a path and a message of its own,
        // and is cut to the same length.
        const QByteArray name(490, 'n');
        const SchemaValidator refused("{\"properties\": {\"" + name + "\": {\"$ref\": \"#/$defs/" + QByteArray(480, 'r')
                                      + "\"}}}");
        QVERIFY(!refused.isValid());
        errors = refused.validate(QJsonValue(1));
        QVERIFY(errors.has_value());
        QCOMPARE(errors->first().message.size(), 501);
        QVERIFY(errors->first().message.startsWith(QStringLiteral("schema refused at /properties/nnn")));
        QVERIFY(errors->first().message.endsWith(QChar(0x2026)));

        // Nor is an escape in it split: the newline's escape would cross the
        // limit, so the refusal stops before it.
        const SchemaValidator escaped("{\"properties\": {\"" + QByteArray(468, 'n') + "\\n" + QByteArray(20, 'n')
                                      + "\": {\"$ref\": \"#/$defs/x\"}}}");
        QVERIFY(!escaped.isValid());
        errors = escaped.validate(QJsonValue(1));
        QVERIFY(errors.has_value());
        QCOMPARE(errors->first().message,
                 QStringLiteral("schema refused at /properties/") + QString(468, u'n') + QChar(0x2026));
    }

    void errorsEscapeWhatCouldForgeALogLine()
    {
        // A member name or a string quoted from a data file reaches the log
        // unquoted, where a newline would start a line of its own, an ESC a
        // terminal escape sequence, and U+202E would reverse the text after
        // it. Each such character is written as \uXXXX, in the path and in
        // the message, and so logged.
        const SchemaValidator validator(QByteArrayLiteral(
            R"({"properties": {"s": {"pattern": "^a$"}}, "additionalProperties": {"type": "string"}})"));
        QVERIFY(validator.isValid());
        const QString name = QStringLiteral("a\nfsloader: FORGED") + QChar(0x1b) + QStringLiteral("[31m");
        const QString value = QStringLiteral("x\ny") + QChar(0x202e) + QChar(0x85) + QChar(0x7f);
        const auto errors = validator.validate(QJsonObject{{name, 1}, {QStringLiteral("s"), value}});
        QVERIFY(errors.has_value());
        const QString shownName = QStringLiteral("a\\u000afsloader: FORGED\\u001b[31m");
        const QList<SchemaValidator::Error> expected{
            {QStringLiteral("/s"),
             QStringLiteral("String 'x\\u000ay\\u202e\\u0085\\u007f' does not match pattern '^a$'.")},
            {QStringLiteral("/") + shownName,
             QStringLiteral("Additional property '") + shownName + QStringLiteral("' found but was invalid.")},
            {QStringLiteral("/") + shownName, QStringLiteral("additionalProperties: Expected string, found integer")},
        };
        QCOMPARE(errors->size(), expected.size());
        for (qsizetype i = 0; i < expected.size(); ++i) {
            QCOMPARE(errors->at(i).path, expected.at(i).path);
            QCOMPARE(errors->at(i).message, expected.at(i).message);
        }
        const QLoggingCategory category("test.schemavalidator.log");
        for (const SchemaValidator::Error& error : expected) {
            QTest::ignoreMessage(
                QtWarningMsg,
                (QStringLiteral("  ") + error.path + QStringLiteral(": ") + error.message).toUtf8().constData());
        }
        logSchemaErrors(category, *errors);

        // An error built elsewhere is escaped when it is logged.
        QTest::ignoreMessage(QtWarningMsg, "  /a\\u2066: x\\u000d\\u000a\\u009b\\u2028");
        logSchemaErrors(category,
                        {SchemaValidator::Error{QStringLiteral("/a") + QChar(0x2066),
                                                QStringLiteral("x\r\n") + QChar(0x9b) + QChar(0x2028)}});

        // The cut is never made inside an escape: "/", 497 letters and the
        // six units of an escaped newline would cross 500.
        const SchemaValidator closed(QByteArrayLiteral(R"({"additionalProperties": false})"));
        const auto cut = closed.validate(QJsonObject{{QString(497, u'a') + QChar(u'\n'), 1}});
        QVERIFY(cut.has_value());
        QCOMPARE(cut->first().path, QStringLiteral("/") + QString(497, u'a') + QChar(0x2026));
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
        // One line per error, "  <path>: <message>" unquoted, "(root)" for the
        // document itself, and the count validate() appends past its 100th
        // error printed as it is.
        QList<SchemaValidator::Error> errors{SchemaValidator::Error{QString(), QStringLiteral("root")}};
        for (int i = 1; i < 100; ++i) {
            errors.append(SchemaValidator::Error{QStringLiteral("/x"), QStringLiteral("m")});
        }
        errors.append(SchemaValidator::Error{QString(), QStringLiteral("and 2 more errors not listed")});
        QTest::ignoreMessage(QtWarningMsg, "  (root): root");
        for (int i = 1; i < 100; ++i) {
            QTest::ignoreMessage(QtWarningMsg, "  /x: m");
        }
        QTest::ignoreMessage(QtWarningMsg, "  and 2 more errors not listed");
        const QLoggingCategory category("test.schemavalidator.log");
        logSchemaErrors(category, errors);
    }
};

QTEST_MAIN(TestSchemaValidator)
#include "test_schemavalidator.moc"
