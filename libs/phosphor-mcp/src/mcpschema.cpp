// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "mcpschema.h"

#include <QJsonArray>
#include <QLatin1StringView>
#include <QSet>

#include <valijson/adapters/qtjson_adapter.hpp>
#include <valijson/schema.hpp>
#include <valijson/schema_parser.hpp>
#include <valijson/validation_results.hpp>
#include <valijson/validator.hpp>

#include <cmath>
#include <exception>
#include <string>

namespace PhosphorMcp {

namespace {

// The keywords a tool schema may use. Each means the same under Draft 7,
// which valijson checks, and under 2020-12, which the client reads, so a
// call that the server accepts is one that the client's copy of the schema
// describes. Everything else is refused, the keywords whose meaning differs
// ($ref, $defs, definitions, prefixItems, the array form of items,
// additionalItems, dependencies and its 2020-12 halves, unevaluated*,
// $dynamicRef, $anchor, $id, $schema), format, which valijson asserts and a
// 2020-12 reader takes as an annotation only, multipleOf, which valijson does
// not check against a fractional value when the divisor is an integer (2.5
// passes multipleOf 1), pattern and patternProperties, which valijson
// matches with std::regex (see checkSchema), and the rest (contains,
// if/then/else, ...), which no tool needs yet.
const QSet<QString>& allowedKeywords()
{
    static const QSet<QString> keywords{
        QStringLiteral("type"),
        QStringLiteral("properties"),
        QStringLiteral("required"),
        QStringLiteral("enum"),
        QStringLiteral("const"),
        QStringLiteral("minimum"),
        QStringLiteral("maximum"),
        QStringLiteral("exclusiveMinimum"),
        QStringLiteral("exclusiveMaximum"),
        QStringLiteral("minLength"),
        QStringLiteral("maxLength"),
        QStringLiteral("minItems"),
        QStringLiteral("maxItems"),
        QStringLiteral("uniqueItems"),
        QStringLiteral("items"),
        QStringLiteral("minProperties"),
        QStringLiteral("maxProperties"),
        QStringLiteral("additionalProperties"),
        QStringLiteral("propertyNames"),
        QStringLiteral("oneOf"),
        QStringLiteral("anyOf"),
        QStringLiteral("allOf"),
        QStringLiteral("not"),
        QStringLiteral("description"),
        QStringLiteral("title"),
        QStringLiteral("default"),
        QStringLiteral("examples"),
        QStringLiteral("readOnly"),
        QStringLiteral("writeOnly"),
        QStringLiteral("deprecated"),
        QStringLiteral("$comment"),
    };
    return keywords;
}

// The refused keywords whose meaning differs between the dialects, so the
// reason can say so rather than only "not supported".
const QSet<QString>& dialectKeywords()
{
    static const QSet<QString> keywords{
        QStringLiteral("$schema"),
        QStringLiteral("$ref"),
        QStringLiteral("$defs"),
        QStringLiteral("definitions"),
        QStringLiteral("prefixItems"),
        QStringLiteral("additionalItems"),
        QStringLiteral("dependentRequired"),
        QStringLiteral("dependentSchemas"),
        QStringLiteral("dependencies"),
        QStringLiteral("unevaluatedProperties"),
        QStringLiteral("unevaluatedItems"),
        QStringLiteral("$dynamicRef"),
        QStringLiteral("$dynamicAnchor"),
        QStringLiteral("$anchor"),
        QStringLiteral("$id"),
        QStringLiteral("$recursiveRef"),
        QStringLiteral("$recursiveAnchor"),
        QStringLiteral("$vocabulary"),
    };
    return keywords;
}

const QSet<QString>& typeNames()
{
    static const QSet<QString> names{
        QStringLiteral("object"),  QStringLiteral("array"),   QStringLiteral("string"), QStringLiteral("number"),
        QStringLiteral("integer"), QStringLiteral("boolean"), QStringLiteral("null"),
    };
    return names;
}

// RFC 6901: "~" and "/" in a member name are escaped.
QString pointerToken(const QString& name)
{
    QString token = name;
    token.replace(QLatin1Char('~'), QLatin1StringView("~0"));
    token.replace(QLatin1Char('/'), QLatin1StringView("~1"));
    return token;
}

bool isNonNegativeInteger(const QJsonValue& value)
{
    if (!value.isDouble()) {
        return false;
    }
    const double v = value.toDouble();
    return std::isfinite(v) && v >= 0 && std::trunc(v) == v;
}

QString checkSchema(const QJsonValue& schema, const QString& path);

QString checkSchemaArray(const QJsonValue& value, const QString& path)
{
    if (!value.isArray() || value.toArray().isEmpty()) {
        return QStringLiteral("%1: must be a non-empty array of schemas").arg(path);
    }
    const QJsonArray array = value.toArray();
    for (qsizetype i = 0; i < array.size(); ++i) {
        const QString error = checkSchema(array.at(i), QStringLiteral("%1/%2").arg(path).arg(i));
        if (!error.isEmpty()) {
            return error;
        }
    }
    return QString();
}

QString checkSchemaMap(const QJsonValue& value, const QString& path)
{
    if (!value.isObject()) {
        return QStringLiteral("%1: must be an object of schemas").arg(path);
    }
    const QJsonObject map = value.toObject();
    for (auto it = map.begin(); it != map.end(); ++it) {
        const QString childPath = path + QLatin1Char('/') + pointerToken(it.key());
        const QString error = checkSchema(it.value(), childPath);
        if (!error.isEmpty()) {
            return error;
        }
    }
    return QString();
}

QString checkType(const QJsonValue& value, const QString& path)
{
    if (value.isString()) {
        return typeNames().contains(value.toString())
            ? QString()
            : QStringLiteral("%1: unknown type \"%2\"").arg(path, value.toString());
    }
    if (!value.isArray() || value.toArray().isEmpty()) {
        return QStringLiteral("%1: must be a type name or a non-empty array of them").arg(path);
    }
    QSet<QString> seen;
    for (const QJsonValue& entry : value.toArray()) {
        if (!entry.isString() || !typeNames().contains(entry.toString()) || seen.contains(entry.toString())) {
            return QStringLiteral("%1: must be a type name or an array of distinct type names").arg(path);
        }
        seen.insert(entry.toString());
    }
    return QString();
}

// Walks the schema positions only: the members of `properties` are names,
// and `enum`, `const`, `default` and `examples` hold values, so a "$ref"
// there is not a keyword.
QString checkSchema(const QJsonValue& schema, const QString& path)
{
    if (schema.isBool()) {
        return QString();
    }
    if (!schema.isObject()) {
        return QStringLiteral("%1: a schema must be an object or a boolean").arg(path);
    }
    const QJsonObject object = schema.toObject();
    for (auto it = object.begin(); it != object.end(); ++it) {
        const QString key = it.key();
        const QJsonValue value = it.value();
        const QString at = path + QLatin1Char('/') + pointerToken(key);
        if (dialectKeywords().contains(key)) {
            return QStringLiteral(
                       "%1: \"%2\" is not supported: it means something else, or nothing, in one of JSON Schema "
                       "Draft 7 and 2020-12")
                .arg(at, key);
        }
        if (key == QLatin1StringView("pattern") || key == QLatin1StringView("patternProperties")) {
            // valijson matches them with std::regex, whose libstdc++
            // executor recurses for each character: a long string overflows
            // the stack (GCC bug 86164), nested quantifiers take forever, and
            // it matches UTF-8 bytes where the client matches UTF-16 code
            // units. Arguments come from the model, so none of that may run.
            return QStringLiteral(
                       "%1: \"%2\" is not supported: the server cannot match regular expressions safely. Use "
                       "\"enum\", or check the value in the handler")
                .arg(at, key);
        }
        if (!allowedKeywords().contains(key)) {
            return QStringLiteral("%1: \"%2\" is not supported in a tool schema").arg(at, key);
        }

        QString error;
        if (key == QLatin1StringView("type")) {
            error = checkType(value, at);
        } else if (key == QLatin1StringView("properties")) {
            error = checkSchemaMap(value, at);
        } else if (key == QLatin1StringView("additionalProperties") || key == QLatin1StringView("propertyNames")
                   || key == QLatin1StringView("not")) {
            error = checkSchema(value, at);
        } else if (key == QLatin1StringView("items")) {
            if (value.isArray()) {
                error = QStringLiteral(
                            "%1: \"items\" as an array is not supported: it is a tuple in Draft 7 and an error in "
                            "2020-12 (prefixItems)")
                            .arg(at);
            } else {
                error = checkSchema(value, at);
            }
        } else if (key == QLatin1StringView("oneOf") || key == QLatin1StringView("anyOf")
                   || key == QLatin1StringView("allOf")) {
            error = checkSchemaArray(value, at);
        } else if (key == QLatin1StringView("required")) {
            bool ok = value.isArray();
            for (const QJsonValue& name : value.toArray()) {
                ok = ok && name.isString();
            }
            if (!ok) {
                error = QStringLiteral("%1: must be an array of property names").arg(at);
            }
        } else if (key == QLatin1StringView("enum") || key == QLatin1StringView("examples")) {
            if (!value.isArray()) {
                error = QStringLiteral("%1: must be an array").arg(at);
            }
        } else if (key == QLatin1StringView("minimum") || key == QLatin1StringView("maximum")
                   || key == QLatin1StringView("exclusiveMinimum") || key == QLatin1StringView("exclusiveMaximum")) {
            // A boolean exclusiveMinimum is Draft 4's form.
            if (!value.isDouble()) {
                error = QStringLiteral("%1: must be a number").arg(at);
            }
        } else if (key == QLatin1StringView("minLength") || key == QLatin1StringView("maxLength")
                   || key == QLatin1StringView("minItems") || key == QLatin1StringView("maxItems")
                   || key == QLatin1StringView("minProperties") || key == QLatin1StringView("maxProperties")) {
            if (!isNonNegativeInteger(value)) {
                error = QStringLiteral("%1: must be an integer of 0 or more").arg(at);
            }
        } else if (key == QLatin1StringView("description") || key == QLatin1StringView("title")
                   || key == QLatin1StringView("$comment")) {
            if (!value.isString()) {
                error = QStringLiteral("%1: must be a string").arg(at);
            }
        } else if (key == QLatin1StringView("uniqueItems") || key == QLatin1StringView("readOnly")
                   || key == QLatin1StringView("writeOnly") || key == QLatin1StringView("deprecated")) {
            if (!value.isBool()) {
                error = QStringLiteral("%1: must be a boolean").arg(at);
            }
        }
        if (!error.isEmpty()) {
            return error;
        }
    }
    return QString();
}

// valijson reports a failure at each level it passes through, around the
// one that says what is wrong ("Failed to validate against schema associated
// with property name 'x'."). Those add only noise for the model, which the
// path of the real error already tells.
bool isWrapperError(const std::string& description)
{
    return description.rfind("Failed to validate against schema associated with property name", 0) == 0
        || description.rfind("Failed to validate item #", 0) == 0
        || description.rfind("Failed to validate against child schema", 0) == 0
        || description.rfind("Failed to validate against additional properties schema", 0) == 0;
}

} // namespace

struct McpSchema::Private
{
    valijson::Schema schema;
    mutable valijson::Validator validator;
    bool compiled = false;
};

McpSchema::McpSchema()
    : d(std::make_unique<Private>())
{
}

McpSchema::~McpSchema() = default;

QString McpSchema::compile(const QJsonObject& schema)
{
    if (d->compiled) {
        return QStringLiteral("the schema was compiled before");
    }
    if (schema.value(QLatin1StringView("type")) != QJsonValue(QStringLiteral("object"))) {
        return QStringLiteral("/type: the root of a tool schema must have \"type\": \"object\"");
    }
    const QString error = checkSchema(schema, QString());
    if (!error.isEmpty()) {
        return error;
    }
    try {
        const valijson::adapters::QtJsonAdapter adapter(schema);
        valijson::SchemaParser parser(valijson::SchemaParser::kDraft7);
        parser.populateSchema(adapter, d->schema);
        d->compiled = true;
        return QString();
    } catch (const std::exception& e) {
        return QStringLiteral("the schema does not compile: %1").arg(QString::fromUtf8(e.what()));
    } catch (...) {
        return QStringLiteral("the schema does not compile");
    }
}

QStringList McpSchema::validate(const QJsonValue& value) const
{
    if (!d->compiled) {
        return {QStringLiteral(": the schema did not compile")};
    }
    // valijson throws on what it cannot check. Every caller of this file is
    // built without exceptions, so nothing may escape.
    try {
        valijson::ValidationResults results;
        const valijson::adapters::QtJsonAdapter adapter(value);
        if (d->validator.validate(d->schema, adapter, &results)) {
            return {};
        }
        QStringList all;
        QStringList specific;
        valijson::ValidationResults::Error error;
        while (results.popError(error)) {
            const QString line = QString::fromStdString(error.jsonPointer) + QStringLiteral(": ")
                + QString::fromStdString(error.description);
            if (all.contains(line)) {
                continue;
            }
            all.append(line);
            if (!isWrapperError(error.description)) {
                specific.append(line);
            }
        }
        if (!specific.isEmpty()) {
            return specific;
        }
        if (!all.isEmpty()) {
            return all;
        }
        return {QStringLiteral(": does not match the schema")};
    } catch (const std::exception& e) {
        return {QStringLiteral(": could not be checked: %1").arg(QString::fromUtf8(e.what()))};
    } catch (...) {
        return {QStringLiteral(": could not be checked")};
    }
}

} // namespace PhosphorMcp
