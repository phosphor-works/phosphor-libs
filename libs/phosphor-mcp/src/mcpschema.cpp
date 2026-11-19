// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "mcpschema.h"

#include <QJsonArray>
#include <QLatin1StringView>
#include <QList>
#include <QLocale>
#include <QSet>

#include <valijson/adapters/qtjson_adapter.hpp>
#include <valijson/schema.hpp>
#include <valijson/schema_parser.hpp>
#include <valijson/validation_results.hpp>
#include <valijson/validator.hpp>

#include <algorithm>
#include <cmath>
#include <exception>
#include <optional>
#include <string>
#include <vector>

namespace PhosphorMcp {

namespace {

// The errors that validate() lists, before a last line that counts the rest.
constexpr qsizetype MaxListedErrors = 10;

// valijson keeps every error it finds in a queue that has no bound, and a
// value can hold millions of mistakes. So the errors are collected only for
// a value this small, which bounds their number and the length of their
// paths: at most this many JSON values in it...
constexpr qsizetype MaxCollectedNodes = 1000;
// ...and at most this many UTF-16 code units in the JSON pointers of all of
// them together.
constexpr qsizetype MaxCollectedPointerLength = 64 * 1024;

// A member name in an error is cut to this many code units: a model's
// arguments may have names of any length, and the error goes back to it.
constexpr qsizetype MaxShownTokenLength = 64;

// 2^53, from which doubles no longer hold every integer, and 2^63.
constexpr double TwoTo53 = 9007199254740992.0;
constexpr double TwoTo63 = 9223372036854775808.0;

// The keywords a tool schema may use. Each means the same under Draft 7,
// which valijson checks, and under 2020-12, which the client reads, so a
// call that the server accepts is one that the client's copy of the schema
// describes. Everything else is refused: the keywords whose meaning differs
// between the dialects (dialectKeywords() below), format, which valijson
// asserts and a 2020-12 reader takes as an annotation only, multipleOf,
// which valijson does not check against a fractional value when the divisor
// is an integer (2.5 passes multipleOf 1), pattern and patternProperties,
// which valijson matches with std::regex (see checkSchema), uniqueItems,
// which valijson checks by comparing every pair of items, and the rest
// (contains, if/then/else, ...), which no tool needs yet. propertyNames is
// allowed where the server checks it itself (see checkSchema).
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

QString unescapePointerToken(const QString& token)
{
    QString name = token;
    name.replace(QLatin1StringView("~1"), QLatin1StringView("/"));
    name.replace(QLatin1StringView("~0"), QLatin1StringView("~"));
    return name;
}

// The member names of a JSON pointer, unescaped.
QStringList pointerTokens(const QString& pointer)
{
    QStringList tokens = pointer.split(QLatin1Char('/'));
    tokens.removeFirst();
    for (QString& token : tokens) {
        token = unescapePointerToken(token);
    }
    return tokens;
}

// A member name or index as an error shows it: escaped, an unpaired
// surrogate replaced so that the reply stays valid UTF-8, and cut short.
QString shownToken(const QString& name)
{
    QString token = pointerToken(name);
    for (qsizetype i = 0; i < token.size(); ++i) {
        const QChar c = token.at(i);
        if (c.isHighSurrogate() && i + 1 < token.size() && token.at(i + 1).isLowSurrogate()) {
            ++i;
        } else if (c.isSurrogate()) {
            token[i] = QChar(QChar::ReplacementCharacter);
        }
    }
    if (token.size() > MaxShownTokenLength) {
        token.truncate(MaxShownTokenLength - 3);
        if (token.back().isHighSurrogate() || token.back() == QLatin1Char('~')) {
            token.chop(1);
        }
        token += QLatin1StringView("...");
    }
    return token;
}

QString shownPointer(const QStringList& names)
{
    QString pointer;
    for (const QString& name : names) {
        pointer += QLatin1Char('/') + shownToken(name);
    }
    return pointer;
}

// The value at `names` below `value`, or Undefined.
QJsonValue valueAt(QJsonValue value, const QStringList& names)
{
    for (const QString& name : names) {
        if (value.isObject()) {
            value = value.toObject().value(name);
        } else if (value.isArray()) {
            bool ok = false;
            const qsizetype index = name.toLongLong(&ok);
            const QJsonArray array = value.toArray();
            if (!ok || index < 0 || index >= array.size()) {
                return QJsonValue(QJsonValue::Undefined);
            }
            value = array.at(index);
        } else {
            return QJsonValue(QJsonValue::Undefined);
        }
    }
    return value;
}

// A number in a model-facing error, the same in every locale. An integer
// shows all its digits ("3", not "3.000000").
QString formatNumber(double value)
{
    if (std::trunc(value) == value && std::fabs(value) < TwoTo53) {
        return QString::number(qint64(value));
    }
    return QString::number(value, 'g', QLocale::FloatingPointShortest);
}

// A number that valijson, which compares numbers as doubles, would compare
// wrongly with an integer argument: from 2^53, where doubles skip integers,
// up to 2^63, the end of the integers that an argument may hold.
bool isInexactNumber(const QJsonValue& value)
{
    const double magnitude = std::fabs(value.toDouble());
    return value.isDouble() && magnitude >= TwoTo53 && magnitude <= TwoTo63;
}

// An integral number that does not fit qint64. valijson takes 2^63 for an
// integer while QJsonValue::toInteger() gives a handler 0 for it.
bool isOutsideInt64(const QJsonValue& value)
{
    const double number = value.toDouble();
    return value.isDouble() && std::trunc(number) == number && value.toInteger(0) != value.toInteger(1);
}

// Lengths and counts: an integer that every JSON reader holds exactly. A
// larger one is undefined behaviour in valijson's parser from 2^63.
bool isCount(const QJsonValue& value)
{
    if (!value.isDouble()) {
        return false;
    }
    const double v = value.toDouble();
    return std::isfinite(v) && v >= 0 && v < TwoTo53 && std::trunc(v) == v;
}

QString inexactNumberError(const QString& path)
{
    return QStringLiteral(
               "%1: a number from 2^53 to 2^63 in magnitude is not supported here, because the server compares "
               "numbers as doubles, which round the integers there")
        .arg(path);
}

// Checks a value that a schema compares arguments with (enum, const).
QString checkLiteral(const QJsonValue& value, const QString& path)
{
    if (value.isString() && !value.toString().isValidUtf16()) {
        return QStringLiteral("%1: a string with an unpaired UTF-16 surrogate is not supported").arg(path);
    }
    if (isInexactNumber(value)) {
        return inexactNumberError(path);
    }
    if (value.isArray()) {
        const QJsonArray array = value.toArray();
        for (qsizetype i = 0; i < array.size(); ++i) {
            const QString error = checkLiteral(array.at(i), path + QLatin1Char('/') + QString::number(i));
            if (!error.isEmpty()) {
                return error;
            }
        }
    } else if (value.isObject()) {
        const QJsonObject object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it) {
            const QString at = path + QLatin1Char('/') + pointerToken(it.key());
            if (!it.key().isValidUtf16()) {
                return QStringLiteral("%1: a member name with an unpaired UTF-16 surrogate is not supported").arg(at);
            }
            const QString error = checkLiteral(it.value(), at);
            if (!error.isEmpty()) {
                return error;
            }
        }
    }
    return QString();
}

// A property name that valijson can hold: it keys each subschema by its
// path in the schema without escaping the names in it, so a name with "/"
// would share the key of another subschema and take its schema. A name that
// is not valid UTF-16 would lose its unpaired surrogates on the way in.
QString checkPropertyName(const QString& name, const QString& path)
{
    if (name.contains(QLatin1Char('/'))) {
        return QStringLiteral(
                   "%1: a property name with \"/\" is not supported, because the validator would confuse it with "
                   "another place in the schema")
            .arg(path);
    }
    if (!name.isValidUtf16()) {
        return QStringLiteral("%1: a property name with an unpaired UTF-16 surrogate is not supported").arg(path);
    }
    return QString();
}

// `fixedLocation`: the schema applies to one place in the instance, known
// from the schema alone (the root, or a chain of "properties" from it).
QString checkSchema(const QJsonValue& schema, const QString& path, bool fixedLocation);

QString checkSchemaArray(const QJsonValue& value, const QString& path)
{
    if (!value.isArray() || value.toArray().isEmpty()) {
        return QStringLiteral("%1: must be a non-empty array of schemas").arg(path);
    }
    const QJsonArray array = value.toArray();
    for (qsizetype i = 0; i < array.size(); ++i) {
        const QString error = checkSchema(array.at(i), path + QLatin1Char('/') + QString::number(i), false);
        if (!error.isEmpty()) {
            return error;
        }
    }
    return QString();
}

QString checkProperties(const QJsonValue& value, const QString& path, bool fixedLocation)
{
    if (!value.isObject()) {
        return QStringLiteral("%1: must be an object of schemas").arg(path);
    }
    const QJsonObject map = value.toObject();
    for (auto it = map.begin(); it != map.end(); ++it) {
        const QString childPath = path + QLatin1Char('/') + pointerToken(it.key());
        QString error = checkPropertyName(it.key(), childPath);
        if (error.isEmpty()) {
            error = checkSchema(it.value(), childPath, fixedLocation);
        }
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

QString checkRequired(const QJsonValue& value, const QString& path)
{
    if (!value.isArray()) {
        return QStringLiteral("%1: must be an array of distinct property names").arg(path);
    }
    // Both metaschemas have uniqueItems on it: a client that checks the
    // schema refuses a name twice.
    QSet<QString> seen;
    const QJsonArray names = value.toArray();
    for (qsizetype i = 0; i < names.size(); ++i) {
        const QJsonValue name = names.at(i);
        if (!name.isString() || seen.contains(name.toString())) {
            return QStringLiteral("%1: must be an array of distinct property names").arg(path);
        }
        // A "/" is harmless here: valijson does not key anything by these.
        if (!name.toString().isValidUtf16()) {
            return QStringLiteral("%1/%2: a property name with an unpaired UTF-16 surrogate is not supported")
                .arg(path, QString::number(i));
        }
        seen.insert(name.toString());
    }
    return QString();
}

// Walks the schema positions only: the members of `properties` are names,
// and `enum`, `const`, `default` and `examples` hold values, so a "$ref"
// there is not a keyword.
QString checkSchema(const QJsonValue& schema, const QString& path, bool fixedLocation)
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
                       "%1: \"%2\" is not supported, because it means something else, or nothing, in one of JSON "
                       "Schema Draft 7 and 2020-12")
                .arg(at, key);
        }
        if (key == QLatin1StringView("pattern") || key == QLatin1StringView("patternProperties")) {
            // valijson matches them with std::regex, whose libstdc++
            // executor recurses for each character: a long string overflows
            // the stack (GCC bug 86164), nested quantifiers take forever, and
            // it matches UTF-8 bytes where the client matches UTF-16 code
            // units. Arguments come from the model, so none of that may run.
            return QStringLiteral(
                       "%1: \"%2\" is not supported, because the server cannot match regular expressions safely. "
                       "Use \"enum\", or check the value in the handler")
                .arg(at, key);
        }
        if (key == QLatin1StringView("uniqueItems")) {
            // valijson compares every pair of items, even when they all
            // differ: 20000 integers take seconds.
            return QStringLiteral(
                       "%1: \"uniqueItems\" is not supported, because the server would compare every pair of "
                       "items. Check for repeated items in the handler")
                .arg(at);
        }
        if (!allowedKeywords().contains(key)) {
            return QStringLiteral("%1: \"%2\" is not supported in a tool schema").arg(at, key);
        }

        QString error;
        if (key == QLatin1StringView("type")) {
            error = checkType(value, at);
        } else if (key == QLatin1StringView("properties")) {
            error = checkProperties(value, at, fixedLocation);
        } else if (key == QLatin1StringView("propertyNames")) {
            // valijson builds the propertyNames subschema at the path of the
            // schema that holds it, so the two share keywords in its registry
            // and each checks the other's (tristanpenman/valijson#254, not
            // merged in 1.1.4). The server checks propertyNames itself, which
            // it can do where the object it applies to is known from the
            // schema alone.
            if (!fixedLocation) {
                return QStringLiteral(
                           "%1: \"propertyNames\" is supported only at the root and under a chain of "
                           "\"properties\" from it")
                    .arg(at);
            }
            error = checkSchema(value, at, false);
        } else if (key == QLatin1StringView("additionalProperties") || key == QLatin1StringView("not")) {
            error = checkSchema(value, at, false);
        } else if (key == QLatin1StringView("items")) {
            if (value.isArray()) {
                error = QStringLiteral(
                            "%1: \"items\" as an array is not supported, because it is a tuple in Draft 7 and an "
                            "error in 2020-12 (prefixItems)")
                            .arg(at);
            } else {
                error = checkSchema(value, at, false);
            }
        } else if (key == QLatin1StringView("oneOf") || key == QLatin1StringView("anyOf")
                   || key == QLatin1StringView("allOf")) {
            error = checkSchemaArray(value, at);
        } else if (key == QLatin1StringView("required")) {
            error = checkRequired(value, at);
        } else if (key == QLatin1StringView("enum")) {
            error = value.isArray() ? checkLiteral(value, at) : QStringLiteral("%1: must be an array").arg(at);
        } else if (key == QLatin1StringView("const")) {
            error = checkLiteral(value, at);
        } else if (key == QLatin1StringView("examples")) {
            if (!value.isArray()) {
                error = QStringLiteral("%1: must be an array").arg(at);
            }
        } else if (key == QLatin1StringView("minimum") || key == QLatin1StringView("maximum")
                   || key == QLatin1StringView("exclusiveMinimum") || key == QLatin1StringView("exclusiveMaximum")) {
            // A boolean exclusiveMinimum is Draft 4's form.
            if (!value.isDouble()) {
                error = QStringLiteral("%1: must be a number").arg(at);
            } else if (isInexactNumber(value)) {
                error = inexactNumberError(at);
            }
        } else if (key == QLatin1StringView("minLength") || key == QLatin1StringView("maxLength")
                   || key == QLatin1StringView("minItems") || key == QLatin1StringView("maxItems")
                   || key == QLatin1StringView("minProperties") || key == QLatin1StringView("maxProperties")) {
            if (!isCount(value)) {
                error = QStringLiteral("%1: must be an integer from 0 to 9007199254740991").arg(at);
            }
        } else if (key == QLatin1StringView("description") || key == QLatin1StringView("title")
                   || key == QLatin1StringView("$comment")) {
            if (!value.isString()) {
                error = QStringLiteral("%1: must be a string").arg(at);
            }
        } else if (key == QLatin1StringView("readOnly") || key == QLatin1StringView("writeOnly")
                   || key == QLatin1StringView("deprecated")) {
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

// The errors that validate() returns: each once, at most MaxListedErrors of
// them, and a last line that counts the rest.
class Report
{
public:
    // A line that may repeat one before it.
    void add(const QString& line)
    {
        if (m_seen.contains(line)) {
            return;
        }
        m_seen.insert(line);
        addDistinct(line);
    }
    // A line that differs from every other.
    void addDistinct(const QString& line)
    {
        if (isFull()) {
            ++m_more;
        } else {
            m_lines.append(line);
        }
    }
    // One more distinct error, not listed: for a caller that need not build
    // its line once the list is full.
    void countDistinct()
    {
        ++m_more;
    }
    [[nodiscard]] bool isFull() const
    {
        return m_lines.size() >= MaxListedErrors;
    }
    [[nodiscard]] bool isEmpty() const
    {
        return m_lines.isEmpty();
    }
    // The listed lines, and the number of the others.
    [[nodiscard]] const QStringList& lines() const
    {
        return m_lines;
    }
    [[nodiscard]] qsizetype unlisted() const
    {
        return m_more;
    }
    QStringList take()
    {
        QStringList lines = std::move(m_lines);
        if (m_more == 1) {
            lines.append(QStringLiteral(": and 1 more error"));
        } else if (m_more > 1) {
            lines.append(QStringLiteral(": and %1 more errors").arg(m_more));
        }
        return lines;
    }

private:
    QStringList m_lines;
    QSet<QString> m_seen;
    qsizetype m_more = 0;
};

// The walk over a value before valijson sees it. It refuses what valijson
// would read differently from a handler, and measures the value, to know
// whether its errors can be collected.
class Inspector
{
public:
    explicit Inspector(Report& report)
        : m_report(report)
    {
    }

    void walk(const QJsonValue& value, qsizetype pointerLength = 0)
    {
        ++m_nodes;
        m_pointerLength += pointerLength;
        if (value.isString()) {
            // valijson reads strings through QString::toStdString(), which
            // drops an unpaired surrogate: it would check another string
            // than the one the handler gets.
            if (!value.toString().isValidUtf16()) {
                problem(QStringLiteral(": contains an unpaired UTF-16 surrogate"));
            }
        } else if (value.isDouble()) {
            if (isOutsideInt64(value)) {
                problem(QStringLiteral(": must be from -9223372036854775808 to 9223372036854775807"));
            }
        } else if (value.isArray()) {
            const QJsonArray array = value.toArray();
            for (qsizetype i = 0; i < array.size(); ++i) {
                m_path.append(Segment{i, QString()});
                walk(array.at(i), pointerLength + 1 + digits(i));
                m_path.removeLast();
            }
        } else if (value.isObject()) {
            const QJsonObject object = value.toObject();
            for (auto it = object.begin(); it != object.end(); ++it) {
                const QString key = it.key();
                m_path.append(Segment{-1, key});
                if (!key.isValidUtf16()) {
                    problem(QStringLiteral(": the property name contains an unpaired UTF-16 surrogate"));
                }
                walk(it.value(), pointerLength + 1 + key.size());
                m_path.removeLast();
            }
        }
    }

    // Whether the errors of the value are few and short enough to collect.
    [[nodiscard]] bool isSmall() const
    {
        return m_nodes <= MaxCollectedNodes && m_pointerLength <= MaxCollectedPointerLength;
    }

private:
    struct Segment
    {
        qsizetype index;
        QString key;
    };

    static qsizetype digits(qsizetype n)
    {
        qsizetype count = 1;
        for (; n >= 10; n /= 10) {
            ++count;
        }
        return count;
    }

    void problem(const QString& description)
    {
        if (m_report.isFull()) {
            m_report.countDistinct();
            return;
        }
        QStringList names;
        for (const Segment& segment : std::as_const(m_path)) {
            names.append(segment.index >= 0 ? QString::number(segment.index) : segment.key);
        }
        m_report.addDistinct(shownPointer(names) + description);
    }

    Report& m_report;
    QList<Segment> m_path;
    qsizetype m_nodes = 0;
    qsizetype m_pointerLength = 0;
};

// The schemas below `schema` that apply to the instance at `names`, as
// valijson walks them: through properties, additionalProperties and items,
// and into every branch of allOf, anyOf and oneOf. Not into not, whose
// failures valijson reports as one.
void applicableSchemas(const QJsonValue& schema, const QJsonValue& instance, const QStringList& names, qsizetype index,
                       QList<QJsonObject>& out)
{
    if (!schema.isObject()) {
        return;
    }
    const QJsonObject node = schema.toObject();
    for (const QLatin1StringView combinator :
         {QLatin1StringView("allOf"), QLatin1StringView("anyOf"), QLatin1StringView("oneOf")}) {
        for (const QJsonValue& branch : node.value(combinator).toArray()) {
            applicableSchemas(branch, instance, names, index, out);
        }
    }
    if (index == names.size()) {
        out.append(node);
        return;
    }
    const QString& name = names.at(index);
    if (instance.isObject()) {
        const QJsonValue child = instance.toObject().value(name);
        const QJsonObject properties = node.value(QLatin1StringView("properties")).toObject();
        if (properties.contains(name)) {
            applicableSchemas(properties.value(name), child, names, index + 1, out);
        } else {
            applicableSchemas(node.value(QLatin1StringView("additionalProperties")), child, names, index + 1, out);
        }
    } else if (instance.isArray()) {
        applicableSchemas(node.value(QLatin1StringView("items")), valueAt(instance, {name}), names, index + 1, out);
    }
}

bool typeAccepts(const QString& type, const QJsonValue& value)
{
    if (type == QLatin1StringView("integer")) {
        return value.isDouble() && std::trunc(value.toDouble()) == value.toDouble();
    }
    return (type == QLatin1StringView("object") && value.isObject())
        || (type == QLatin1StringView("array") && value.isArray())
        || (type == QLatin1StringView("string") && value.isString())
        || (type == QLatin1StringView("number") && value.isDouble())
        || (type == QLatin1StringView("boolean") && value.isBool())
        || (type == QLatin1StringView("null") && value.isNull());
}

// "a", "a or b", "a, b or c".
QString alternatives(const QStringList& words)
{
    if (words.size() <= 1) {
        return words.value(0);
    }
    return QStringList(words.first(words.size() - 1)).join(QLatin1StringView(", ")) + QLatin1StringView(" or ")
        + words.last();
}

// valijson's errors that only say that a level below failed, and the
// pointer of the place they are about.
std::optional<QString> wrapperSubject(const std::string& description, const QString& pointer)
{
    static const std::string property = "Failed to validate against schema associated with property name '";
    static const std::string item = "Failed to validate item #";
    if (description.rfind(property, 0) == 0 && description.size() >= property.size() + 2) {
        const std::string name = description.substr(property.size(), description.size() - property.size() - 2);
        return pointer + QLatin1Char('/') + pointerToken(QString::fromStdString(name));
    }
    if (description.rfind(item, 0) == 0) {
        const std::string rest = description.substr(item.size());
        return pointer + QLatin1Char('/') + QString::fromStdString(rest.substr(0, rest.find(' ')));
    }
    if (description.rfind("Failed to validate against child schema #", 0) == 0
        || description == "Failed to validate against additional properties schema.") {
        return pointer;
    }
    return std::nullopt;
}

// The text between `prefix` and `suffix` when `description` is
// prefix + text + suffix.
std::optional<QString> between(const std::string& description, const std::string& prefix, const std::string& suffix)
{
    if (description.size() < prefix.size() + suffix.size() || description.rfind(prefix, 0) != 0
        || description.compare(description.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return std::nullopt;
    }
    return QString::fromStdString(
        description.substr(prefix.size(), description.size() - prefix.size() - suffix.size()));
}

} // namespace

struct McpSchema::Private
{
    // A propertyNames that the server checks: the object at `location`
    // (member names from the root) has only names that `names` accepts.
    struct NameRule
    {
        QStringList location;
        // Null for propertyNames false, which accepts no name.
        std::unique_ptr<McpSchema> names;
    };

    QString takePropertyNames(QJsonObject& node, const QStringList& location);
    bool matches(const QJsonValue& value) const;
    void collect(const QJsonValue& value, Report& report) const;
    void describe(const valijson::ValidationResults::Error& error, const QJsonValue& value,
                  const QSet<QString>& covered, Report& report) const;

    // The schema that valijson compiled, without propertyNames.
    QJsonObject source;
    valijson::Schema schema;
    mutable valijson::Validator validator;
    std::vector<NameRule> nameRules;
    bool compiled = false;
};

QString McpSchema::Private::takePropertyNames(QJsonObject& node, const QStringList& location)
{
    const QJsonValue names = node.take(QLatin1StringView("propertyNames"));
    if (names.isBool() && !names.toBool()) {
        nameRules.push_back(NameRule{location, nullptr});
    } else if (names.isObject()) {
        auto compiled = std::make_unique<McpSchema>();
        const QString error = compiled->compileChecked(names.toObject());
        if (!error.isEmpty()) {
            return error;
        }
        nameRules.push_back(NameRule{location, std::move(compiled)});
    }
    if (!node.contains(QLatin1StringView("properties"))) {
        return QString();
    }
    QJsonObject properties = node.value(QLatin1StringView("properties")).toObject();
    for (auto it = properties.begin(); it != properties.end(); ++it) {
        if (!it.value().isObject()) {
            continue;
        }
        QJsonObject child = it.value().toObject();
        const QString error = takePropertyNames(child, location + QStringList{it.key()});
        if (!error.isEmpty()) {
            return error;
        }
        *it = child;
    }
    node.insert(QLatin1StringView("properties"), properties);
    return QString();
}

bool McpSchema::Private::matches(const QJsonValue& value) const
{
    // No results: valijson stops at the first failure and keeps nothing.
    const valijson::adapters::QtJsonAdapter adapter(value);
    if (!validator.validate(schema, adapter, nullptr)) {
        return false;
    }
    for (const NameRule& rule : nameRules) {
        const QJsonValue object = valueAt(value, rule.location);
        if (!object.isObject()) {
            continue;
        }
        const QJsonObject members = object.toObject();
        for (auto it = members.begin(); it != members.end(); ++it) {
            if (!rule.names || !rule.names->d->matches(QJsonValue(it.key()))) {
                return false;
            }
        }
    }
    return true;
}

void McpSchema::Private::collect(const QJsonValue& value, Report& report) const
{
    valijson::ValidationResults results;
    const valijson::adapters::QtJsonAdapter adapter(value);
    validator.validate(schema, adapter, &results);

    // The places that an error other than a wrapper is at or below: a
    // wrapper about one of them adds nothing.
    QSet<QString> covered;
    for (const valijson::ValidationResults::Error& error : results) {
        QString pointer = QString::fromStdString(error.jsonPointer);
        if (wrapperSubject(error.description, pointer)) {
            continue;
        }
        while (!covered.contains(pointer)) {
            covered.insert(pointer);
            if (pointer.isEmpty()) {
                break;
            }
            pointer.truncate(pointer.lastIndexOf(QLatin1Char('/')));
        }
    }
    for (const valijson::ValidationResults::Error& error : results) {
        describe(error, value, covered, report);
    }

    for (const NameRule& rule : nameRules) {
        const QJsonValue object = valueAt(value, rule.location);
        if (!object.isObject()) {
            continue;
        }
        const QJsonObject members = object.toObject();
        for (auto it = members.begin(); it != members.end(); ++it) {
            const QString at = shownPointer(rule.location + QStringList{it.key()});
            if (!rule.names) {
                report.add(at + QStringLiteral(": is not allowed, because the schema allows no property here"));
                continue;
            }
            const QJsonValue name(it.key());
            if (rule.names->d->matches(name)) {
                continue;
            }
            Report nameReport;
            rule.names->d->collect(name, nameReport);
            if (nameReport.isEmpty()) {
                report.add(at + QStringLiteral(": the property name does not match propertyNames"));
            }
            for (const QString& line : nameReport.lines()) {
                // The name is the root of its own check: ": <description>".
                report.add(at + QStringLiteral(": the property name ") + line.mid(2));
            }
            // Each differs from every line before: it has this name in it.
            for (qsizetype i = 0; i < nameReport.unlisted(); ++i) {
                report.countDistinct();
            }
        }
    }
}

void McpSchema::Private::describe(const valijson::ValidationResults::Error& error, const QJsonValue& value,
                                  const QSet<QString>& covered, Report& report) const
{
    const std::string& text = error.description;
    const QString pointer = QString::fromStdString(error.jsonPointer);
    const QStringList names = pointerTokens(pointer);
    const QString at = shownPointer(names);
    const auto add = [&](const QString& description) {
        report.add(at + QStringLiteral(": ") + description);
    };

    if (const std::optional<QString> subject = wrapperSubject(text, pointer)) {
        // A failure with nothing more specific at or below its place, as of
        // a false subschema.
        if (!covered.contains(*subject)) {
            report.add(shownPointer(pointerTokens(*subject)) + QStringLiteral(": does not match the schema"));
        }
        return;
    }
    if (const std::optional<QString> name = between(text, "Missing required property '", "'.")) {
        report.add(shownPointer(names + QStringList{*name}) + QStringLiteral(": is required"));
        return;
    }

    // valijson writes these counts with std::to_string of an integer, the
    // same in every locale.
    struct Counted
    {
        const char* prefix;
        const char* suffix;
        const char* description;
        const char* one;
    };
    static const Counted counted[] = {
        {"String should be no more than ", " characters in length.", "must be at most %1 characters long",
         "must be at most 1 character long"},
        {"String should be no fewer than ", " characters in length.", "must be at least %1 characters long",
         "must be at least 1 character long"},
        {"Array should contain no more than ", " elements.", "must have at most %1 items", "must have at most 1 item"},
        {"Array should contain no fewer than ", " elements.", "must have at least %1 items",
         "must have at least 1 item"},
        {"Object should have no more than ", " properties.", "must have at most %1 properties",
         "must have at most 1 property"},
        {"Object should have no fewer than ", " properties.", "must have at least %1 properties",
         "must have at least 1 property"},
    };
    for (const Counted& entry : counted) {
        if (const std::optional<QString> count = between(text, entry.prefix, entry.suffix)) {
            add(*count == QLatin1StringView("1") ? QString::fromLatin1(entry.one)
                                                 : QString::fromLatin1(entry.description).arg(*count));
            return;
        }
    }
    struct Fixed
    {
        const char* valijson;
        const char* description;
    };
    static const Fixed fixed[] = {
        {"Failed to match against any enum values.", "must be one of the values of enum"},
        {"Failed to match expected value set by 'const' constraint.", "must be the value of const"},
        {"Target should not validate against schema specified in 'not' constraint.",
         "must not match the schema of not"},
        {"Failed to validate against any schemas allowed by anyOf constraint.",
         "must match at least one schema of anyOf"},
        {"Failed to validate against any child schemas allowed by oneOf constraint.",
         "must match exactly one schema of oneOf, and matches none"},
        {"Failed to validate against exactly one child schema.",
         "must match exactly one schema of oneOf, and matches more than one"},
    };
    for (const Fixed& entry : fixed) {
        if (text == entry.valijson) {
            add(QString::fromLatin1(entry.description));
            return;
        }
    }

    // The rest say what is wrong from the schema, which valijson's text
    // does not tell exactly.
    const QJsonValue instance = valueAt(value, names);
    QList<QJsonObject> schemas;
    applicableSchemas(source, value, names, 0, schemas);
    bool described = false;
    const auto addFound = [&](const QString& description) {
        add(description);
        described = true;
    };

    if (text.rfind("Object contains a property that could not be validated", 0) == 0) {
        // valijson names only the first property it does not know. Each one
        // is named, so that the model mends them all at once.
        const QJsonObject members = instance.toObject();
        for (const QJsonObject& schema : std::as_const(schemas)) {
            if (schema.value(QLatin1StringView("additionalProperties")) != QJsonValue(false)) {
                continue;
            }
            const QJsonObject properties = schema.value(QLatin1StringView("properties")).toObject();
            for (auto it = members.begin(); it != members.end(); ++it) {
                if (!properties.contains(it.key())) {
                    report.add(shownPointer(names + QStringList{it.key()})
                               + QStringLiteral(": is not a known property"));
                    described = true;
                }
            }
        }
        if (!described) {
            add(QStringLiteral("has a property that the schema does not allow"));
        }
        return;
    }
    if (text == "Value type not permitted by 'type' constraint.") {
        for (const QJsonObject& schema : std::as_const(schemas)) {
            const QJsonValue type = schema.value(QLatin1StringView("type"));
            if (type.isUndefined()) {
                continue;
            }
            QStringList types;
            if (type.isArray()) {
                for (const QJsonValue& entry : type.toArray()) {
                    types.append(entry.toString());
                }
            } else {
                types.append(type.toString());
            }
            if (std::none_of(types.begin(), types.end(), [&](const QString& t) {
                    return typeAccepts(t, instance);
                })) {
                addFound(QStringLiteral("must be of type ") + alternatives(types));
            }
        }
        if (!described) {
            add(QStringLiteral("has a type that the schema does not allow"));
        }
        return;
    }
    const bool belowMinimum = text.rfind("Expected number greater than", 0) == 0;
    const bool aboveMaximum = text.rfind("Expected number less than", 0) == 0;
    if (belowMinimum || aboveMaximum) {
        // valijson formats the bound with std::to_string, six decimals in
        // the locale of the process ("3,000000"). The bound comes from the
        // schema instead.
        const double number = instance.toDouble();
        for (const QJsonObject& schema : std::as_const(schemas)) {
            const QJsonValue minimum = schema.value(QLatin1StringView("minimum"));
            const QJsonValue exclusiveMinimum = schema.value(QLatin1StringView("exclusiveMinimum"));
            const QJsonValue maximum = schema.value(QLatin1StringView("maximum"));
            const QJsonValue exclusiveMaximum = schema.value(QLatin1StringView("exclusiveMaximum"));
            if (belowMinimum && minimum.isDouble() && number < minimum.toDouble()) {
                addFound(QStringLiteral("must be at least ") + formatNumber(minimum.toDouble()));
            }
            if (belowMinimum && exclusiveMinimum.isDouble() && number <= exclusiveMinimum.toDouble()) {
                addFound(QStringLiteral("must be greater than ") + formatNumber(exclusiveMinimum.toDouble()));
            }
            if (aboveMaximum && maximum.isDouble() && number > maximum.toDouble()) {
                addFound(QStringLiteral("must be at most ") + formatNumber(maximum.toDouble()));
            }
            if (aboveMaximum && exclusiveMaximum.isDouble() && number >= exclusiveMaximum.toDouble()) {
                addFound(QStringLiteral("must be less than ") + formatNumber(exclusiveMaximum.toDouble()));
            }
        }
        if (!described) {
            add(QStringLiteral("is out of the range that the schema allows"));
        }
        return;
    }
    add(QString::fromStdString(text));
}

McpSchema::McpSchema()
    : d(std::make_unique<Private>())
{
}

McpSchema::~McpSchema() = default;

QString McpSchema::compile(const QJsonObject& schema, Root root)
{
    if (d->compiled) {
        return QStringLiteral(": the schema was compiled before");
    }
    if (root == Root::Object && schema.value(QLatin1StringView("type")) != QJsonValue(QStringLiteral("object"))) {
        return QStringLiteral("/type: the root of a tool schema must have \"type\": \"object\"");
    }
    const QString error = checkSchema(schema, QString(), true);
    if (!error.isEmpty()) {
        return error;
    }
    return compileChecked(schema);
}

QString McpSchema::compileChecked(const QJsonObject& schema)
{
    QJsonObject stripped = schema;
    const QString error = d->takePropertyNames(stripped, QStringList());
    if (!error.isEmpty()) {
        return error;
    }
    try {
        const valijson::adapters::QtJsonAdapter adapter(stripped);
        valijson::SchemaParser parser(valijson::SchemaParser::kDraft7);
        parser.populateSchema(adapter, d->schema);
        d->source = stripped;
        d->compiled = true;
        return QString();
    } catch (const std::exception& e) {
        return QStringLiteral(": valijson cannot compile the schema (%1)").arg(QString::fromUtf8(e.what()));
    } catch (...) {
        return QStringLiteral(": valijson cannot compile the schema");
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
        Report report;
        Inspector inspector(report);
        inspector.walk(value);
        if (!report.isEmpty()) {
            return report.take();
        }
        if (d->matches(value)) {
            return {};
        }
        if (!inspector.isSmall()) {
            return {QStringLiteral(": does not match the schema, and is too large for the server to list each error")};
        }
        d->collect(value, report);
        if (report.isEmpty()) {
            return {QStringLiteral(": does not match the schema")};
        }
        return report.take();
    } catch (const std::exception& e) {
        return {QStringLiteral(": could not be checked (%1)").arg(QString::fromUtf8(e.what()))};
    } catch (...) {
        return {QStringLiteral(": could not be checked")};
    }
}

} // namespace PhosphorMcp
