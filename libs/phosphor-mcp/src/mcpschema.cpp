// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "mcpschema.h"

#include <QHash>
#include <QJsonArray>
#include <QLatin1StringView>
#include <QList>
#include <QLocale>
#include <QSet>

#include <jsoncons/json.hpp>
#include <jsoncons_ext/jsonschema/jsonschema.hpp>

#include <algorithm>
#include <cmath>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace PhosphorMcp {

namespace {

using Json = jsoncons::json;
namespace jss = jsoncons::jsonschema;

// The errors that validate() lists, before a last line that counts the rest.
constexpr qsizetype MaxListedErrors = 10;
// The distinct errors that validate() counts. A value with more gets one
// line that says it is too large to list each error...
constexpr qsizetype MaxCountedErrors = 1000;
// ...and so does a value whose errors' JSON pointers come to more than this
// many UTF-16 code units together, since each is kept to tell it from the
// others.
constexpr qsizetype MaxErrorPointerLength = 1024 * 1024;

// A member name in an error is cut to this many code units: a model's
// arguments may have names of any length, and the error goes back to it.
constexpr qsizetype MaxShownTokenLength = 64;

// Values nested deeper than this are refused before any check. Qt's JSON
// parser takes no more than 1024 levels in a whole message, so arguments
// from the wire never reach it. It bounds the recursion of the walks below
// for a structured content that a handler built.
constexpr int MaxNestingDepth = 1024;

// The bounds of a check, which Estimator works out before it runs. A check
// that could pass one is not run, and the value is refused as too large to
// check. The work is in units of about 10 ns of jsoncons' time in a release
// build, so the budget is about a second. As measured (CPU time at the
// largest value let through, the README has the table): 2.48 million valid
// integers under "items" in 0.58 s, 4 MB of mixed text under 50 maxLength in
// 0.62 s, 2 million members under unevaluatedProperties in 0.70 s, and no
// pattern measured above 0.70 s. jsoncons takes 1.3 s for 8 million
// integers, the most a 16 MiB line holds.
constexpr qint64 CheckBudget = 100'000'000;
// The errors that combinators may collect, about 1.3 KB each, and more for
// a long path: 1.3 GB for an anyOf that fails for each of a million items,
// as measured. The bound lets 53,000 such items through.
constexpr qint64 MaxCollectedBytes = 128 * 1024 * 1024;
// jsoncons copies the path in the schema (its "evaluation path") into each
// keyword it evaluates, and keeps a copy for each level of the walk on the
// stack, so memory grows with the square of that path's length. 1024 tokens
// hold a value 512 levels deep under a schema that recurses through "items"
// and "$ref", in about 70 MB.
constexpr qint64 MaxEvaluationPathTokens = 1024;
// See Estimator.
constexpr qint64 UnitsPerKeyword = 5;
constexpr qint64 UnitsPerPathToken = 3;
constexpr qint64 PointerBytesPerUnit = 16;
constexpr qint64 StringBytesPerUnit = 2;
constexpr qint64 LiteralBytesPerUnit = 16;
constexpr qint64 WrittenBytesPerUnit = 1;
constexpr qint64 DefaultBytesPerUnit = 4;
constexpr qint64 UnitsPerError = 60;
constexpr qint64 BytesPerError = 600;
constexpr qint64 BytesPerPathToken = 32;

// 2^53, from which doubles no longer hold every integer, and 2^63.
constexpr double TwoTo53 = 9007199254740992.0;
constexpr double TwoTo63 = 9223372036854775808.0;

const QString TooLargeToList =
    QStringLiteral(": does not match the schema, and is too large for the server to list each error");
const QString TooLargeToCheck = QStringLiteral(": is too large for the server to check against the schema");

// RFC 6901: "~" and "/" in a member name are escaped. A name without either
// is returned as it is, since QString::replace() costs even when it finds
// nothing.
QString pointerToken(const QString& name)
{
    if (!name.contains(QLatin1Char('~')) && !name.contains(QLatin1Char('/'))) {
        return name;
    }
    QString token = name;
    token.replace(QLatin1Char('~'), QLatin1StringView("~0"));
    token.replace(QLatin1Char('/'), QLatin1StringView("~1"));
    return token;
}

// A name as an error shows it: an unpaired surrogate replaced so that the
// reply stays valid UTF-8, and cut short.
QString shownName(const QString& name)
{
    QString token = name;
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

// A member name or index in a pointer: escaped, then shown.
QString shownToken(const QString& name)
{
    return shownName(pointerToken(name));
}

QString shownPointer(const QStringList& names)
{
    QString pointer;
    for (const QString& name : names) {
        pointer += QLatin1Char('/') + shownToken(name);
    }
    return pointer;
}

// The JSON pointer of `names`, whole.
QString fullPointer(const QStringList& names)
{
    QString pointer;
    for (const QString& name : names) {
        pointer += QLatin1Char('/') + pointerToken(name);
    }
    return pointer;
}

// An integral number that fits qint64, as QJsonValue::toInteger() reads it.
bool isInt64(const QJsonValue& value)
{
    return value.isDouble() && value.toInteger(0) == value.toInteger(1);
}

// A number in a model-facing error, the same in every locale. An integer
// shows all its digits ("3", not "3.000000" or "3e+15").
QString formatNumber(const QJsonValue& value)
{
    if (isInt64(value)) {
        return QString::number(value.toInteger());
    }
    return QString::number(value.toDouble(), 'g', QLocale::FloatingPointShortest);
}

// An integral number that does not fit qint64. The validator would take 2^63
// for an integer while QJsonValue::toInteger() gives a handler 0 for it.
//
// This sees the number as Qt's JSON parser left it, which is not always as
// it was written. The parser reads a number that is not an integer literal
// within qint64 as the nearest double, and keeps a double that is integral
// and fits qint64 as that integer. So -9223372036854775809, down to
// -9223372036854776832, arrives as -9223372036854775808 and passes, the
// value the handler reads too. And 9223372036854775807.0 arrives as 2^63
// and is refused. Neither can be told apart here from what was written.
bool isOutsideInt64(const QJsonValue& value)
{
    const double number = value.toDouble();
    return value.isDouble() && std::trunc(number) == number && !isInt64(value);
}

// 2^63 as a double. The server holds an integral number of a schema that
// fits qint64 as that integer, which jsoncons compares with an argument
// exactly, and any other as a double. An argument is a qint64, and jsoncons
// compares a qint64 with a double as doubles. The largest qint64 values then
// round to 2^63 and equal it, where every other double compares right.
bool isTwoTo63(const QJsonValue& value)
{
    return value.isDouble() && !isInt64(value) && value.toDouble() == TwoTo63;
}

// Lengths and counts: an integer that every JSON reader holds exactly.
bool isCount(const QJsonValue& value)
{
    if (!isInt64(value)) {
        return false;
    }
    const qint64 v = value.toInteger();
    return v >= 0 && double(v) < TwoTo53;
}

// The value as jsoncons holds it for the validator: an integral number that
// fits qint64 as an integer, so that jsoncons compares it exactly with
// another, any other number as a double, and strings and member names in
// UTF-8. Objects keep their members in jsoncons' order, by their UTF-8
// bytes, which is Qt's order (by UTF-16 code units) but for characters from
// U+E000 to U+FFFF beside characters past U+FFFF. They are sorted first,
// since jsoncons inserts each member at its place.
Json toJson(const QJsonValue& value)
{
    switch (value.type()) {
    case QJsonValue::Bool:
        return Json(value.toBool());
    case QJsonValue::Double:
        if (isInt64(value)) {
            return Json(std::int64_t(value.toInteger()));
        }
        return Json(value.toDouble());
    case QJsonValue::String: {
        const QByteArray utf8 = value.toString().toUtf8();
        return Json(utf8.constData(), std::size_t(utf8.size()));
    }
    case QJsonValue::Array: {
        const QJsonArray array = value.toArray();
        Json out(jsoncons::json_array_arg);
        out.reserve(std::size_t(array.size()));
        for (const QJsonValue& item : array) {
            out.push_back(toJson(item));
        }
        return out;
    }
    case QJsonValue::Object: {
        const QJsonObject object = value.toObject();
        std::vector<std::pair<std::string, Json>> members;
        members.reserve(std::size_t(object.size()));
        for (auto it = object.begin(); it != object.end(); ++it) {
            members.emplace_back(it.key().toStdString(), toJson(it.value()));
        }
        const auto byName = [](const std::pair<std::string, Json>& a, const std::pair<std::string, Json>& b) {
            return a.first < b.first;
        };
        if (!std::is_sorted(members.begin(), members.end(), byName)) {
            std::sort(members.begin(), members.end(), byName);
        }
        Json out(jsoncons::json_object_arg);
        out.reserve(members.size());
        for (auto& member : members) {
            out.try_emplace(std::move(member.first), std::move(member.second));
        }
        return out;
    }
    case QJsonValue::Null:
    case QJsonValue::Undefined:
        break;
    }
    return Json::null();
}

// The value at `tokens` below `root`, or null.
const Json* jsonAt(const Json& root, const std::vector<std::string>& tokens, std::size_t count)
{
    const Json* value = &root;
    for (std::size_t i = 0; i < count; ++i) {
        const std::string& token = tokens[i];
        if (value->is_object()) {
            const auto it = value->find(token);
            if (it == value->object_range().end()) {
                return nullptr;
            }
            value = &it->value();
        } else if (value->is_array()) {
            if (token.empty() || token.size() > 18 || !std::all_of(token.begin(), token.end(), [](char c) {
                    return c >= '0' && c <= '9';
                })) {
                return nullptr;
            }
            const std::size_t index = std::stoull(token);
            if (index >= value->size()) {
                return nullptr;
            }
            value = &value->at(index);
        } else {
            return nullptr;
        }
    }
    return value;
}

// One subschema of a compiled schema: what checkSchema accepted, with the
// subschemas it applies resolved to their indices.
struct SchemaNode
{
    bool isBoolean = false;
    bool booleanValue = true;
    // The keywords, for the descriptions of errors.
    QJsonObject object;
    // Its place in the document, member names and indices, unescaped.
    QStringList path;
    // The applicators, -1 when absent.
    std::map<std::string, int> properties;
    std::vector<int> prefixItems;
    std::vector<int> allOf;
    std::vector<int> anyOf;
    std::vector<int> oneOf;
    int items = -1;
    int additionalProperties = -1;
    int contains = -1;
    int propertyNames = -1;
    int notSchema = -1;
    int ifSchema = -1;
    int thenSchema = -1;
    int elseSchema = -1;
    int unevaluatedProperties = -1;
    int unevaluatedItems = -1;
    int ref = -1;
    QString refValue;
    // An $anchor the application gave it.
    QString anchor;
    // What a check of the node costs, for Estimator: the keywords for which
    // jsoncons makes a validator, those that count a string's characters,
    // the UTF-8 bytes of the values that enum and const compare with, the
    // names of required, the triggers of dependentRequired, and the size of
    // a default (-1 without one), which jsoncons copies for each object
    // where the property is missing.
    qint64 keywords = 0;
    qint64 lengthChecks = 0;
    qint64 literalBytes = 0;
    bool hasEnum = false;
    bool hasMultipleOf = false;
    qint64 required = 0;
    qint64 requiredBytes = 0;
    std::vector<std::pair<std::string, qint64>> dependentRequired;
    qint64 defaultBytes = -1;
};

// The UTF-8 bytes of a JSON value as it would be written, about.
qint64 literalBytes(const QJsonValue& value)
{
    switch (value.type()) {
    case QJsonValue::String:
        return 2 + value.toString().size() * 3;
    case QJsonValue::Array: {
        qint64 bytes = 2;
        for (const QJsonValue& item : value.toArray()) {
            bytes += 1 + literalBytes(item);
        }
        return bytes;
    }
    case QJsonValue::Object: {
        qint64 bytes = 2;
        const QJsonObject object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it) {
            bytes += 4 + it.key().size() * 3 + literalBytes(it.value());
        }
        return bytes;
    }
    default:
        return 8;
    }
}

const QSet<QString>& typeNames()
{
    static const QSet<QString> names{
        QStringLiteral("object"),  QStringLiteral("array"),   QStringLiteral("string"), QStringLiteral("number"),
        QStringLiteral("integer"), QStringLiteral("boolean"), QStringLiteral("null"),
    };
    return names;
}

// Checks a value that a schema compares arguments with (enum, const).
QString checkLiteral(const QJsonValue& value, const QString& path)
{
    if (value.isString() && !value.toString().isValidUtf16()) {
        return QStringLiteral("%1: a string with an unpaired UTF-16 surrogate is not supported").arg(path);
    }
    if (isTwoTo63(value)) {
        return QStringLiteral(
                   "%1: the number 2^63 is not supported, because the server would take the largest 64-bit "
                   "integers for it")
            .arg(path);
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

// An array of distinct property names. Both metaschemas have uniqueItems on
// required: a client that checks the schema refuses a name twice.
QString checkNames(const QJsonValue& value, const QString& path)
{
    if (!value.isArray()) {
        return QStringLiteral("%1: must be an array of distinct property names").arg(path);
    }
    QSet<QString> seen;
    const QJsonArray names = value.toArray();
    for (qsizetype i = 0; i < names.size(); ++i) {
        const QJsonValue name = names.at(i);
        if (!name.isString() || seen.contains(name.toString())) {
            return QStringLiteral("%1: must be an array of distinct property names").arg(path);
        }
        if (!name.toString().isValidUtf16()) {
            return QStringLiteral("%1/%2: a property name with an unpaired UTF-16 surrogate is not supported")
                .arg(path, QString::number(i));
        }
        seen.insert(name.toString());
    }
    return QString();
}

// A plain-name fragment, as 2020-12 defines $anchor, but for a first "_",
// which jsoncons refuses.
bool isAnchorName(const QString& name)
{
    if (name.isEmpty()) {
        return false;
    }
    for (qsizetype i = 0; i < name.size(); ++i) {
        const char16_t c = name.at(i).unicode();
        const bool letter = (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z');
        const bool other = (c >= u'0' && c <= u'9') || c == u'-' || c == u'.' || c == u'_';
        if (!letter && (i == 0 || !other)) {
            return false;
        }
    }
    return true;
}

// The characters a URI fragment may hold besides percent-encoded bytes
// (RFC 3986, pchar, "/" and "?").
bool isFragmentCharacter(char16_t c)
{
    if ((c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') || (c >= u'0' && c <= u'9')) {
        return true;
    }
    return QStringView(u"-._~!$&'()*+,;=:@/?").contains(QChar(c));
}

int hexValue(char16_t c)
{
    if (c >= u'0' && c <= u'9') {
        return c - u'0';
    }
    if (c >= u'a' && c <= u'f') {
        return c - u'a' + 10;
    }
    if (c >= u'A' && c <= u'F') {
        return c - u'A' + 10;
    }
    return -1;
}

// The fragment of a URI reference, percent-decoded, or nothing when it is
// not a valid fragment of UTF-8.
std::optional<QString> decodeFragment(const QString& fragment)
{
    QByteArray bytes;
    for (qsizetype i = 0; i < fragment.size(); ++i) {
        const char16_t c = fragment.at(i).unicode();
        if (c == u'%') {
            if (i + 2 >= fragment.size()) {
                return std::nullopt;
            }
            const int high = hexValue(fragment.at(i + 1).unicode());
            const int low = hexValue(fragment.at(i + 2).unicode());
            if (high < 0 || low < 0) {
                return std::nullopt;
            }
            bytes.append(char(high * 16 + low));
            i += 2;
        } else if (isFragmentCharacter(c)) {
            bytes.append(char(c));
        } else {
            return std::nullopt;
        }
    }
    const QString decoded = QString::fromUtf8(bytes);
    if (decoded.toUtf8() != bytes) {
        return std::nullopt;
    }
    return decoded;
}

// Checks a schema against the keyword policy (see the README, "Schemas")
// and builds its nodes.
class SchemaBuilder
{
public:
    explicit SchemaBuilder(std::vector<SchemaNode>& nodes)
        : m_nodes(nodes)
    {
    }

    // The index of the node of `schema` at `path`, or -1 with error() set.
    int build(const QJsonValue& schema, const QStringList& path);
    // After build(): resolves each $ref, and refuses what the nodes may not
    // hold together. Returns the reason or an empty string.
    QString finish();

    [[nodiscard]] const QString& error() const
    {
        return m_error;
    }

private:
    int fail(const QString& error)
    {
        if (m_error.isEmpty()) {
            m_error = error;
        }
        return -1;
    }
    // The child schema `value` at `path`, stored with `store` once built.
    bool child(const QJsonValue& value, const QStringList& path, int& slot);
    bool childArray(const QJsonValue& value, const QStringList& path, std::vector<int>& branches);
    QString keyword(int index, const QString& key, const QJsonValue& value, const QStringList& path);
    QString resolveReference(int index);
    QString findCycle();
    QString checkUnevaluated();

    std::vector<SchemaNode>& m_nodes;
    // The nodes by their JSON pointer, and by their $anchor.
    QHash<QString, int> m_positions;
    QHash<QString, int> m_anchors;
    QString m_error;
};

int SchemaBuilder::build(const QJsonValue& schema, const QStringList& path)
{
    const QString at = fullPointer(path);
    if (!schema.isBool() && !schema.isObject()) {
        return fail(QStringLiteral("%1: a schema must be an object or a boolean").arg(at));
    }
    const int index = int(m_nodes.size());
    m_nodes.emplace_back();
    m_nodes[std::size_t(index)].path = path;
    m_positions.insert(at, index);
    if (schema.isBool()) {
        m_nodes[std::size_t(index)].isBoolean = true;
        m_nodes[std::size_t(index)].booleanValue = schema.toBool();
        return index;
    }
    const QJsonObject object = schema.toObject();
    m_nodes[std::size_t(index)].object = object;
    for (auto it = object.begin(); it != object.end(); ++it) {
        const QString error = keyword(index, it.key(), it.value(), path + QStringList{it.key()});
        if (!error.isEmpty()) {
            return fail(error);
        }
    }
    return index;
}

bool SchemaBuilder::child(const QJsonValue& value, const QStringList& path, int& slot)
{
    const int index = build(value, path);
    if (index < 0) {
        return false;
    }
    slot = index;
    return true;
}

bool SchemaBuilder::childArray(const QJsonValue& value, const QStringList& path, std::vector<int>& branches)
{
    if (!value.isArray() || value.toArray().isEmpty()) {
        fail(QStringLiteral("%1: must be a non-empty array of schemas").arg(fullPointer(path)));
        return false;
    }
    const QJsonArray array = value.toArray();
    std::vector<int> built;
    for (qsizetype i = 0; i < array.size(); ++i) {
        const int index = build(array.at(i), path + QStringList{QString::number(i)});
        if (index < 0) {
            return false;
        }
        built.push_back(index);
    }
    branches = std::move(built);
    return true;
}

// Checks one keyword of the node `index` and builds what it applies. The
// node is looked up again after each child, which grows m_nodes.
QString SchemaBuilder::keyword(int index, const QString& key, const QJsonValue& value, const QStringList& path)
{
    const QString at = fullPointer(path);
    const auto node = [this, index]() -> SchemaNode& {
        return m_nodes[std::size_t(index)];
    };
    const auto built = [this]() {
        return m_error;
    };

    // Refused, with the reason.
    if (key == QLatin1StringView("pattern") || key == QLatin1StringView("patternProperties")) {
        // jsoncons matches them with std::regex, whose libstdc++ executor
        // recurses for each character: a long string overflows the stack
        // (GCC bug 86164), nested quantifiers take forever, and it matches
        // UTF-8 bytes where the client matches UTF-16 code units. Arguments
        // come from the model, so none of that may run.
        return QStringLiteral(
                   "%1: \"%2\" is not supported, because the server cannot match regular expressions safely. "
                   "Use \"enum\", or check the value in the handler")
            .arg(at, key);
    }
    if (key == QLatin1StringView("uniqueItems")) {
        // jsoncons compares every pair of items, even when they all differ.
        return QStringLiteral(
                   "%1: \"uniqueItems\" is not supported, because the server would compare every pair of items. "
                   "Check for repeated items in the handler")
            .arg(at);
    }
    if (key == QLatin1StringView("dependentSchemas")) {
        // jsoncons files every subschema of dependentSchemas under the
        // same place, without its name, and applies it with the
        // property's name added to the place in the value, so its errors
        // name members that do not exist.
        return QStringLiteral(
                   "%1: \"dependentSchemas\" is not supported, because the server's validator reports its errors at "
                   "the wrong place. Use \"if\" with \"required\", and \"then\"")
            .arg(at);
    }
    if (key == QLatin1StringView("$id")) {
        return QStringLiteral(
                   "%1: \"$id\" is not supported, because the server resolves references within the tool's schema "
                   "only")
            .arg(at);
    }
    if (key == QLatin1StringView("$dynamicRef") || key == QLatin1StringView("$dynamicAnchor")) {
        return QStringLiteral(
                   "%1: \"%2\" is not supported, because the server does not resolve dynamic references. Use "
                   "\"$ref\"")
            .arg(at, key);
    }
    if (key == QLatin1StringView("$vocabulary")) {
        return QStringLiteral("%1: \"$vocabulary\" is not supported, because it belongs in a meta-schema").arg(at);
    }
    static const QHash<QString, QString> olderKeywords{
        {QStringLiteral("definitions"), QStringLiteral(". Use \"$defs\"")},
        {QStringLiteral("dependencies"), QStringLiteral(". Use \"dependentRequired\"")},
        {QStringLiteral("additionalItems"), QStringLiteral(". Use \"items\" beside \"prefixItems\"")},
        {QStringLiteral("$recursiveRef"), QStringLiteral(". Use \"$ref\"")},
        {QStringLiteral("$recursiveAnchor"), QString()},
    };
    if (const auto older = olderKeywords.constFind(key); older != olderKeywords.cend()) {
        return QStringLiteral("%1: \"%2\" is not a keyword of JSON Schema 2020-12%3").arg(at, key, *older);
    }

    // Annotations, checked but not applied.
    if (key == QLatin1StringView("title") || key == QLatin1StringView("description")
        || key == QLatin1StringView("$comment") || key == QLatin1StringView("format")
        || key == QLatin1StringView("contentMediaType") || key == QLatin1StringView("contentEncoding")) {
        return value.isString() ? QString() : QStringLiteral("%1: must be a string").arg(at);
    }
    if (key == QLatin1StringView("readOnly") || key == QLatin1StringView("writeOnly")
        || key == QLatin1StringView("deprecated")) {
        return value.isBool() ? QString() : QStringLiteral("%1: must be a boolean").arg(at);
    }
    if (key == QLatin1StringView("examples")) {
        return value.isArray() ? QString() : QStringLiteral("%1: must be an array").arg(at);
    }
    if (key == QLatin1StringView("default")) {
        node().defaultBytes = literalBytes(value);
        return QString();
    }
    if (key == QLatin1StringView("$schema")) {
        if (path.size() != 1) {
            return QStringLiteral("%1: \"$schema\" is supported at the root of the schema only").arg(at);
        }
        if (value != QJsonValue(QStringLiteral("https://json-schema.org/draft/2020-12/schema"))
            && value != QJsonValue(QStringLiteral("https://json-schema.org/draft/2020-12/schema#"))) {
            return QStringLiteral(
                       "%1: the schema must be JSON Schema 2020-12, the dialect that MCP clients read, so "
                       "\"$schema\" may name https://json-schema.org/draft/2020-12/schema only")
                .arg(at);
        }
        return QString();
    }
    if (key == QLatin1StringView("$anchor")) {
        if (!value.isString() || !isAnchorName(value.toString())) {
            return QStringLiteral(
                       "%1: must be a name of letters, digits, \"-\", \"_\" and \".\" that starts with a letter")
                .arg(at);
        }
        if (m_anchors.contains(value.toString())) {
            return QStringLiteral("%1: another schema has this $anchor").arg(at);
        }
        m_anchors.insert(value.toString(), index);
        node().anchor = value.toString();
        return QString();
    }
    if (key == QLatin1StringView("$defs")) {
        if (!value.isObject()) {
            return QStringLiteral("%1: must be an object of schemas").arg(at);
        }
        const QJsonObject definitions = value.toObject();
        for (auto it = definitions.begin(); it != definitions.end(); ++it) {
            if (!it.key().isValidUtf16()) {
                return QStringLiteral("%1/%2: a name with an unpaired UTF-16 surrogate is not supported")
                    .arg(at, pointerToken(it.key()));
            }
            int unused = -1;
            if (!child(it.value(), path + QStringList{it.key()}, unused)) {
                return built();
            }
        }
        return QString();
    }
    if (key == QLatin1StringView("$ref")) {
        if (!value.isString()) {
            return QStringLiteral("%1: must be a string").arg(at);
        }
        node().refValue = value.toString();
        ++node().keywords;
        return QString();
    }

    // Assertions.
    if (key == QLatin1StringView("type")) {
        ++node().keywords;
        return checkType(value, at);
    }
    if (key == QLatin1StringView("enum") || key == QLatin1StringView("const")) {
        if (key == QLatin1StringView("enum") && !value.isArray()) {
            return QStringLiteral("%1: must be an array").arg(at);
        }
        ++node().keywords;
        node().literalBytes += literalBytes(value);
        node().hasEnum = node().hasEnum || key == QLatin1StringView("enum");
        return checkLiteral(value, at);
    }
    if (key == QLatin1StringView("minimum") || key == QLatin1StringView("maximum")
        || key == QLatin1StringView("exclusiveMinimum") || key == QLatin1StringView("exclusiveMaximum")) {
        ++node().keywords;
        // A boolean exclusiveMinimum is Draft 4's form.
        if (!value.isDouble()) {
            return QStringLiteral("%1: must be a number").arg(at);
        }
        return checkLiteral(value, at);
    }
    if (key == QLatin1StringView("multipleOf")) {
        ++node().keywords;
        node().hasMultipleOf = true;
        if (!value.isDouble() || !(value.toDouble() > 0) || !std::isfinite(value.toDouble())) {
            return QStringLiteral("%1: must be a number greater than 0").arg(at);
        }
        return QString();
    }
    if (key == QLatin1StringView("minLength") || key == QLatin1StringView("maxLength")
        || key == QLatin1StringView("minItems") || key == QLatin1StringView("maxItems")
        || key == QLatin1StringView("minProperties") || key == QLatin1StringView("maxProperties")
        || key == QLatin1StringView("minContains") || key == QLatin1StringView("maxContains")) {
        ++node().keywords;
        if (key == QLatin1StringView("minLength") || key == QLatin1StringView("maxLength")) {
            ++node().lengthChecks;
        }
        return isCount(value) ? QString() : QStringLiteral("%1: must be an integer from 0 to 9007199254740991").arg(at);
    }
    if (key == QLatin1StringView("required")) {
        ++node().keywords;
        node().required = value.toArray().size();
        node().requiredBytes = literalBytes(value);
        return checkNames(value, at);
    }
    if (key == QLatin1StringView("dependentRequired")) {
        ++node().keywords;
        if (!value.isObject()) {
            return QStringLiteral("%1: must be an object of arrays of distinct property names").arg(at);
        }
        const QJsonObject dependencies = value.toObject();
        for (auto it = dependencies.begin(); it != dependencies.end(); ++it) {
            const QString inner = at + QLatin1Char('/') + pointerToken(it.key());
            if (!it.key().isValidUtf16()) {
                return QStringLiteral("%1: a property name with an unpaired UTF-16 surrogate is not supported")
                    .arg(inner);
            }
            const QString error = checkNames(it.value(), inner);
            if (!error.isEmpty()) {
                return error;
            }
            node().dependentRequired.emplace_back(it.key().toStdString(), literalBytes(it.value()));
        }
        return QString();
    }

    // Applicators.
    if (key == QLatin1StringView("properties")) {
        ++node().keywords;
        if (!value.isObject()) {
            return QStringLiteral("%1: must be an object of schemas").arg(at);
        }
        const QJsonObject properties = value.toObject();
        for (auto it = properties.begin(); it != properties.end(); ++it) {
            // jsoncons holds the names in UTF-8, which would lose an unpaired
            // surrogate.
            if (!it.key().isValidUtf16()) {
                return QStringLiteral("%1/%2: a property name with an unpaired UTF-16 surrogate is not supported")
                    .arg(at, pointerToken(it.key()));
            }
            int slot = -1;
            if (!child(it.value(), path + QStringList{it.key()}, slot)) {
                return built();
            }
            node().properties.emplace(it.key().toStdString(), slot);
        }
        return QString();
    }
    if (key == QLatin1StringView("items")) {
        ++node().keywords;
        if (value.isArray()) {
            return QStringLiteral(
                       "%1: must be a schema. A list of schemas for the first items is \"prefixItems\" in JSON "
                       "Schema 2020-12")
                .arg(at);
        }
        int slot = -1;
        if (!child(value, path, slot)) {
            return built();
        }
        node().items = slot;
        return QString();
    }
    if (key == QLatin1StringView("prefixItems") || key == QLatin1StringView("allOf")
        || key == QLatin1StringView("anyOf") || key == QLatin1StringView("oneOf")) {
        ++node().keywords;
        std::vector<int> branches;
        if (!childArray(value, path, branches)) {
            return built();
        }
        if (key == QLatin1StringView("prefixItems")) {
            node().prefixItems = std::move(branches);
        } else if (key == QLatin1StringView("allOf")) {
            node().allOf = std::move(branches);
        } else if (key == QLatin1StringView("anyOf")) {
            node().anyOf = std::move(branches);
        } else {
            node().oneOf = std::move(branches);
        }
        return QString();
    }
    if (key == QLatin1StringView("unevaluatedProperties") && !value.isBool()) {
        // jsoncons checks the schema of unevaluatedProperties against each
        // property's value at the place of the object, so its errors name
        // the object, and it counts the members of that value as members of
        // the object.
        return QStringLiteral(
                   "%1: \"unevaluatedProperties\" is supported as true or false only, because the server's "
                   "validator checks its schema at the wrong place")
            .arg(at);
    }
    struct Applicator
    {
        const char* name;
        int SchemaNode::* slot;
    };
    static const Applicator applicators[] = {
        {"additionalProperties", &SchemaNode::additionalProperties},
        {"contains", &SchemaNode::contains},
        {"propertyNames", &SchemaNode::propertyNames},
        {"not", &SchemaNode::notSchema},
        {"if", &SchemaNode::ifSchema},
        {"then", &SchemaNode::thenSchema},
        {"else", &SchemaNode::elseSchema},
        {"unevaluatedProperties", &SchemaNode::unevaluatedProperties},
        {"unevaluatedItems", &SchemaNode::unevaluatedItems},
    };
    for (const Applicator& applicator : applicators) {
        if (key == QLatin1StringView(applicator.name)) {
            // if, then and else make one validator.
            if (key != QLatin1StringView("then") && key != QLatin1StringView("else")) {
                ++node().keywords;
            }
            int slot = -1;
            if (!child(value, path, slot)) {
                return built();
            }
            node().*(applicator.slot) = slot;
            return QString();
        }
    }
    return QStringLiteral("%1: \"%2\" is not supported in a tool schema").arg(at, key);
}

QString SchemaBuilder::finish()
{
    for (std::size_t i = 0; i < m_nodes.size(); ++i) {
        if (!m_nodes[i].refValue.isNull()) {
            const QString error = resolveReference(int(i));
            if (!error.isEmpty()) {
                return error;
            }
        }
    }
    QString error = findCycle();
    if (error.isEmpty()) {
        error = checkUnevaluated();
    }
    return error;
}

// A $ref names a place in the same schema: "#", a JSON pointer after "#",
// or an $anchor. jsoncons resolves no other without fetching it, and the
// server fetches nothing.
QString SchemaBuilder::resolveReference(int index)
{
    SchemaNode& node = m_nodes[std::size_t(index)];
    const QString at = fullPointer(node.path + QStringList{QStringLiteral("$ref")});
    const QString& reference = node.refValue;
    if (!reference.startsWith(QLatin1Char('#'))) {
        return QStringLiteral(
                   "%1: \"$ref\" may refer only to a place in the same schema (\"#...\"), because the server "
                   "fetches no other schema")
            .arg(at);
    }
    const std::optional<QString> fragment = decodeFragment(reference.mid(1));
    if (!fragment) {
        return QStringLiteral("%1: is not a valid URI fragment. Percent-encode the characters that a URI cannot hold")
            .arg(at);
    }
    int target = -1;
    if (fragment->isEmpty() || fragment->startsWith(QLatin1Char('/'))) {
        target = m_positions.value(*fragment, -1);
    } else {
        target = m_anchors.value(*fragment, -1);
    }
    if (target < 0) {
        return QStringLiteral("%1: refers to no schema in the tool's schema").arg(at);
    }
    if (m_nodes[std::size_t(target)].isBoolean) {
        return QStringLiteral(
                   "%1: \"$ref\" to a boolean schema is not supported. Use {} for true, or {\"not\": {}} for false")
            .arg(at);
    }
    // jsoncons files the schema of items beside prefixItems as
    // "additionalItems", where a reference does not find it.
    const QStringList& targetPath = m_nodes[std::size_t(target)].path;
    for (qsizetype i = 0; i < targetPath.size(); ++i) {
        if (targetPath.at(i) != QLatin1StringView("items")) {
            continue;
        }
        const int parent = m_positions.value(fullPointer(targetPath.first(i)), -1);
        if (parent >= 0 && !m_nodes[std::size_t(parent)].prefixItems.empty()
            && m_nodes[std::size_t(parent)].items >= 0) {
            return QStringLiteral(
                       "%1: \"$ref\" into \"items\" beside \"prefixItems\" is not supported, because the server's "
                       "validator files that schema under another name. Move it to \"$defs\"")
                .arg(at);
        }
    }
    m_nodes[std::size_t(index)].ref = target;
    return QString();
}

// The subschemas that apply to the same value as `node`.
std::vector<int> inPlace(const SchemaNode& node)
{
    std::vector<int> next = node.allOf;
    next.insert(next.end(), node.anyOf.begin(), node.anyOf.end());
    next.insert(next.end(), node.oneOf.begin(), node.oneOf.end());
    for (const int single : {node.notSchema, node.ifSchema, node.thenSchema, node.elseSchema, node.ref}) {
        if (single >= 0) {
            next.push_back(single);
        }
    }
    return next;
}

// jsoncons follows each $ref while it checks a value, so a $ref that leads
// back to its own schema through subschemas that apply to the same value
// recurses until the stack overflows. Such a loop holds a $ref, since a
// schema without one is a tree.
QString SchemaBuilder::findCycle()
{
    enum class Mark {
        New,
        Open,
        Done,
    };
    std::vector<Mark> marks(m_nodes.size(), Mark::New);
    std::vector<int> stack;
    std::function<int(int)> visit = [&](int index) -> int {
        marks[std::size_t(index)] = Mark::Open;
        stack.push_back(index);
        for (const int next : inPlace(m_nodes[std::size_t(index)])) {
            if (marks[std::size_t(next)] == Mark::Open) {
                // The loop is the stack from `next` on, and a node on it
                // goes on by its $ref.
                const auto first = std::find(stack.begin(), stack.end(), next);
                for (auto it = first; it != stack.end(); ++it) {
                    const int successor = (it + 1 == stack.end()) ? next : *(it + 1);
                    if (m_nodes[std::size_t(*it)].ref == successor) {
                        return *it;
                    }
                }
                return index;
            }
            if (marks[std::size_t(next)] == Mark::New) {
                const int found = visit(next);
                if (found >= 0) {
                    return found;
                }
            }
        }
        marks[std::size_t(index)] = Mark::Done;
        stack.pop_back();
        return -1;
    };
    for (std::size_t i = 0; i < m_nodes.size(); ++i) {
        if (marks[i] == Mark::New) {
            const int found = visit(int(i));
            if (found >= 0) {
                return QStringLiteral(
                           "%1: \"$ref\" leads back to the same schema without descending into the value, so "
                           "checking it would never end")
                    .arg(fullPointer(m_nodes[std::size_t(found)].path + QStringList{QStringLiteral("$ref")}));
            }
        }
    }
    return QString();
}

// jsoncons keeps the properties and items that a "not" evaluated as
// evaluated, where 2020-12 drops what a failed subschema evaluated, and it
// counts the items of an array inside "contains" as items of the array
// "contains" applies to. So an unevaluatedProperties or unevaluatedItems
// beside them would let through what the client refuses.
QString SchemaBuilder::checkUnevaluated()
{
    for (const SchemaNode& node : m_nodes) {
        if (node.unevaluatedProperties < 0 && node.unevaluatedItems < 0) {
            continue;
        }
        const QString keyword = node.unevaluatedProperties >= 0 ? QStringLiteral("unevaluatedProperties")
                                                                : QStringLiteral("unevaluatedItems");
        const QString at = fullPointer(node.path + QStringList{keyword});
        std::vector<bool> seen(m_nodes.size(), false);
        std::vector<int> pending{int(&node - m_nodes.data())};
        while (!pending.empty()) {
            const int index = pending.back();
            pending.pop_back();
            if (seen[std::size_t(index)]) {
                continue;
            }
            seen[std::size_t(index)] = true;
            const SchemaNode& reached = m_nodes[std::size_t(index)];
            if (reached.notSchema >= 0) {
                return QStringLiteral(
                           "%1: \"%2\" is not supported beside a \"not\" that applies to the same value, because "
                           "the server's validator counts what the subschema of not evaluated")
                    .arg(at, keyword);
            }
            if (node.unevaluatedItems >= 0 && reached.contains >= 0) {
                return QStringLiteral(
                           "%1: \"unevaluatedItems\" is not supported beside a \"contains\" that applies to the "
                           "same value, because the server's validator counts the items of nested arrays that "
                           "contains matched")
                    .arg(fullPointer(node.path + QStringList{QStringLiteral("unevaluatedItems")}));
            }
            const std::vector<int> next = inPlace(reached);
            pending.insert(pending.end(), next.begin(), next.end());
        }
    }
    return QString();
}

// The errors that validate() returns: each once, at most MaxListedErrors of
// them, and a last line that counts the rest.
class Report
{
public:
    // "<pointer>: <description>" for the place at `names`, unless it was
    // added before. The test is on the whole names, not on the line, which
    // cuts them: two long names alike in their first 61 code units are two
    // errors.
    void add(const QStringList& names, const QString& description)
    {
        if (m_overflow) {
            return;
        }
        const QString pointer = fullPointer(names);
        const QString key = pointer + QLatin1StringView(": ") + description;
        if (m_seen.contains(key)) {
            return;
        }
        if (m_seen.size() >= MaxCountedErrors || m_pointerLength + pointer.size() > MaxErrorPointerLength) {
            m_overflow = true;
            return;
        }
        m_seen.insert(key);
        m_pointerLength += pointer.size();
        if (m_lines.size() < MaxListedErrors) {
            m_lines.append(shownPointer(names) + QLatin1StringView(": ") + description);
        } else {
            ++m_more;
        }
    }
    // A line that differs from every other, which the checks before the
    // validator make, and which are counted whatever their number.
    void addDistinct(const QString& line)
    {
        if (m_lines.size() < MaxListedErrors) {
            m_lines.append(line);
        } else {
            ++m_more;
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
    // Whether more errors would change nothing: the value has too many to
    // list.
    [[nodiscard]] bool isDone() const
    {
        return m_overflow;
    }
    [[nodiscard]] bool isEmpty() const
    {
        return m_lines.isEmpty() && !m_overflow;
    }
    QStringList take()
    {
        if (m_overflow) {
            return {TooLargeToList};
        }
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
    qsizetype m_pointerLength = 0;
    qsizetype m_more = 0;
    bool m_overflow = false;
};

// The walk over a value before the validator sees it. It refuses what the
// validator would read differently from a handler, and a value nested too
// deeply for the walks after it.
class Inspector
{
public:
    explicit Inspector(Report& report)
        : m_report(report)
    {
    }

    void walk(const QJsonValue& value)
    {
        if (m_path.size() > MaxNestingDepth) {
            problem(QStringLiteral(": is nested more than %1 levels deep, which the server does not check")
                        .arg(MaxNestingDepth));
            return;
        }
        if (value.isString()) {
            // jsoncons reads strings in UTF-8, which cannot hold an
            // unpaired surrogate. It would check another string than the
            // one the handler gets.
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
                walk(array.at(i));
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
                walk(it.value());
                m_path.removeLast();
            }
        }
    }

private:
    struct Segment
    {
        qsizetype index;
        QString key;
    };

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
};

// The work of a check, worked out before it runs from the schema's nodes and
// the value, by the walk jsoncons makes: each subschema that applies to each
// part of the value, by the same keywords. It counts what makes jsoncons
// slow, in units of about 10 ns of a release build:
//
// - each keyword that a subschema evaluates, for which jsoncons copies its
//   path in the schema and the value's place, which grow with the depth and
//   the names on the way (UnitsPerKeyword, UnitsPerPathToken,
//   PointerBytesPerUnit);
// - each character of a string that a minLength or maxLength counts
//   (StringBytesPerUnit), each value that an enum or const compares
//   (LiteralBytesPerUnit), the value that an enum writes out into its error
//   (WrittenBytesPerUnit), and each default that jsoncons copies for an
//   object without the property (DefaultBytesPerUnit);
// - the errors that a collector could keep: each keyword inside allOf,
//   anyOf, oneOf, not, the condition of if, contains or the schema of
//   additionalProperties may fail, and jsoncons keeps its error until it
//   knows whether the collector passes, with two more copies for each
//   allOf, anyOf and oneOf around it (UnitsPerError, BytesPerError,
//   BytesPerPathToken);
// - the second check of the members of additionalProperties and the names
//   of propertyNames that describe() makes when one fails.
//
// It stops once the work passes CheckBudget, the memory of the errors
// MaxCollectedBytes, or the path in the schema MaxEvaluationPathTokens. It
// also finds the numbers that jsoncons cannot check against multipleOf.
class Estimator
{
public:
    Estimator(const std::vector<SchemaNode>& nodes, Report& report)
        : m_nodes(nodes)
        , m_report(report)
    {
    }

    // Whether the check of `value` against the node `root` fits the bounds.
    bool fits(int root, const Json& value)
    {
        apply(root, value, Context{});
        return !m_exceeded;
    }

private:
    struct Context
    {
        qint64 evaluationTokens = 0;
        qint64 pointerBytes = 0;
        // The collectors around: any, and those of allOf, anyOf and oneOf,
        // whose errors hold the errors they collected as their details.
        qint64 collecting = 0;
        qint64 wrapping = 0;
        qint64 rechecks = 0;
    };
    struct Step
    {
        std::string_view key;
        std::size_t index = 0;
        bool isIndex = false;
    };

    // The memory of a check that describe() makes again is freed before
    // the next, so only the work counts once more for each.
    void add(qint64 units, qint64 bytes, const Context& context)
    {
        m_work += units * (1 + context.rechecks);
        m_memory += bytes;
        if (m_work > CheckBudget || m_memory > MaxCollectedBytes) {
            m_exceeded = true;
        }
    }

    // The tokens of the paths that jsoncons copies for a keyword here.
    [[nodiscard]] qint64 pathTokens(const Context& context) const
    {
        return context.evaluationTokens + qint64(m_path.size());
    }

    // `evaluations` of a subschema or keyword here, `failing` of which may
    // fail. An error that a combinator collects costs no more than its
    // check once the list of errors is full, since the walk then stops.
    void evaluate(qint64 evaluations, qint64 failing, const Context& context)
    {
        const qint64 tokens = pathTokens(context);
        const qint64 each = UnitsPerKeyword + tokens * UnitsPerPathToken + context.pointerBytes / PointerBytesPerUnit;
        add(evaluations * each, 0, context);
        if (context.collecting > 0) {
            const qint64 copies = copiesOfAnError(context) * failing;
            add(copies * (UnitsPerError + each),
                copies * (BytesPerError + tokens * BytesPerPathToken + context.pointerBytes), context);
        }
    }

    // `count` lookups here whose errors would hold `bytes` more text, which
    // jsoncons writes out, or copies, once for each error and again for
    // each copy that a combinator makes.
    void potentialErrors(qint64 count, qint64 bytes, const Context& context)
    {
        const qint64 copies = copiesOfAnError(context);
        add(count * UnitsPerKeyword + copies * bytes / WrittenBytesPerUnit, copies * bytes, context);
    }

    // The copies jsoncons makes of an error here: one in the innermost
    // collector, and two more for each allOf, anyOf and oneOf around, which
    // copy the errors they collected into their own error and that into the
    // collector around them.
    static qint64 copiesOfAnError(const Context& context)
    {
        return context.collecting > 0 ? 1 + 2 * context.wrapping : 0;
    }

    static Context deeper(const Context& context, qint64 tokens)
    {
        Context next = context;
        next.evaluationTokens += tokens;
        return next;
    }

    static Context collected(const Context& context, qint64 tokens, bool wraps = false)
    {
        Context next = deeper(context, tokens);
        ++next.collecting;
        next.wrapping += wraps ? 1 : 0;
        return next;
    }

    // The bytes of `value` written out as JSON, about. Kept for each array
    // and object, since enum writes out the value it refuses.
    qint64 writtenSize(const Json& value)
    {
        if (value.is_string()) {
            return 2 + qint64(value.as_string_view().size());
        }
        if (!value.is_array() && !value.is_object()) {
            return 8;
        }
        const auto known = m_sizes.find(&value);
        if (known != m_sizes.end()) {
            return known->second;
        }
        qint64 size = 2;
        if (value.is_array()) {
            for (const Json& item : value.array_range()) {
                size += 1 + writtenSize(item);
            }
        } else {
            for (const auto& member : value.object_range()) {
                size += 4 + qint64(member.key().size()) + writtenSize(member.value());
            }
        }
        m_sizes.emplace(&value, size);
        return size;
    }

    void problem(const QString& description)
    {
        QStringList names;
        for (const Step& step : m_path) {
            names.append(step.isIndex ? QString::number(step.index)
                                      : QString::fromUtf8(step.key.data(), qsizetype(step.key.size())));
        }
        m_report.add(names, description);
    }

    void enter(const Step& step, Context& context)
    {
        m_path.push_back(step);
        context.pointerBytes += 1 + (step.isIndex ? 8 : qint64(step.key.size()));
    }

    void leave()
    {
        m_path.pop_back();
    }

    void apply(int index, const Json& value, const Context& context);
    void applyObject(const SchemaNode& node, const Json& value, const Context& context);
    void applyArray(const SchemaNode& node, const Json& value, const Context& context);
    void applyItem(int index, const Json& value, std::size_t item, const Context& context);

    const std::vector<SchemaNode>& m_nodes;
    Report& m_report;
    std::vector<Step> m_path;
    std::unordered_map<const Json*, qint64> m_sizes;
    qint64 m_work = 0;
    qint64 m_memory = 0;
    bool m_exceeded = false;
};

void Estimator::apply(int index, const Json& value, const Context& context)
{
    if (m_exceeded) {
        return;
    }
    if (context.evaluationTokens > MaxEvaluationPathTokens) {
        m_exceeded = true;
        return;
    }
    const SchemaNode& node = m_nodes[std::size_t(index)];
    if (node.isBoolean) {
        // A false schema builds its error, and goes on after an error that
        // ends the walk.
        evaluate(1, node.booleanValue ? 0 : 1, context);
        if (!node.booleanValue && context.collecting == 0) {
            add(UnitsPerError + pathTokens(context) * UnitsPerPathToken, 0, context);
        }
        return;
    }
    evaluate(1 + node.keywords, node.keywords, context);

    if (value.is_string()) {
        add(node.lengthChecks * qint64(value.as_string_view().size()) / StringBytesPerUnit, 0, context);
    }
    if (node.literalBytes > 0) {
        add(node.literalBytes / LiteralBytesPerUnit, 0, context);
        // The error of an enum holds the value written out, which jsoncons
        // does for each enum that fails.
        if (node.hasEnum) {
            const qint64 written = writtenSize(value);
            add(written / WrittenBytesPerUnit, 0, context);
            potentialErrors(0, written, context);
        }
    }
    // jsoncons checks multipleOf with doubles, which tell an integer past
    // 2^53 from its neighbours no more: 10^17 passes multipleOf 3.
    if (node.hasMultipleOf && value.is_int64()) {
        const double number = double(value.as<std::int64_t>());
        if (number >= TwoTo53 || number <= -TwoTo53) {
            problem(QStringLiteral(
                "must be less than 2^53 in magnitude, because the server checks multipleOf with doubles"));
        }
    }

    if (value.is_object()) {
        applyObject(node, value, context);
    } else if (value.is_array()) {
        applyArray(node, value, context);
    }

    // The combinators collect the errors of each branch, and so do not and
    // the condition of if, which then make an error of their own without
    // them.
    for (const std::vector<int>* branches : {&node.allOf, &node.anyOf, &node.oneOf}) {
        for (const int branch : *branches) {
            apply(branch, value, collected(context, 2, true));
        }
    }
    if (node.notSchema >= 0) {
        apply(node.notSchema, value, collected(context, 1));
    }
    if (node.ifSchema >= 0) {
        apply(node.ifSchema, value, collected(context, 1));
    }
    // Either then or else runs: both count, the dearer bounding.
    for (const int branch : {node.thenSchema, node.elseSchema}) {
        if (branch >= 0) {
            apply(branch, value, deeper(context, 1));
        }
    }
    if (node.ref >= 0) {
        apply(node.ref, value, deeper(context, 1));
    }
}

void Estimator::applyObject(const SchemaNode& node, const Json& value, const Context& context)
{
    // jsoncons looks each name of required and dependentRequired up, and
    // copies a name that is missing into its error.
    potentialErrors(node.required, node.requiredBytes, context);
    for (const auto& [trigger, bytes] : node.dependentRequired) {
        if (value.find(trigger) != value.object_range().end()) {
            potentialErrors(1, bytes, context);
        }
    }
    if (!node.properties.empty()) {
        // jsoncons looks each schema of properties up in the object, and
        // copies the default of each one that is missing.
        add(qint64(node.properties.size()), 0, context);
        for (const auto& [name, child] : node.properties) {
            const SchemaNode& property = m_nodes[std::size_t(child)];
            if (property.defaultBytes >= 0 && value.find(name) == value.object_range().end()) {
                add(property.defaultBytes / DefaultBytesPerUnit + UnitsPerKeyword, 0, context);
            }
        }
    }
    const bool additionalSchema =
        node.additionalProperties >= 0 && !m_nodes[std::size_t(node.additionalProperties)].isBoolean;
    for (const auto& member : value.object_range()) {
        if (m_exceeded) {
            return;
        }
        add(UnitsPerKeyword + qint64(member.key().size()) / PointerBytesPerUnit, 0, context);
        Context inner = deeper(context, 2);
        enter(Step{member.key(), 0, false}, inner);
        const auto property = node.properties.find(member.key());
        if (property != node.properties.end()) {
            apply(property->second, member.value(), inner);
        } else if (additionalSchema) {
            // The errors of additionalProperties are collected, and
            // describe() checks the member again to name it.
            Context additional = inner;
            ++additional.collecting;
            ++additional.rechecks;
            apply(node.additionalProperties, member.value(), additional);
        } else if (node.additionalProperties >= 0) {
            evaluate(1, 1, inner);
        }
        leave();
    }
    if (node.propertyNames >= 0 && !value.empty()) {
        // jsoncons makes a string value of each name, and checks it at the
        // place of the object. describe() checks the names again to name the
        // one that failed.
        Context names = deeper(context, 1);
        ++names.rechecks;
        for (const auto& member : value.object_range()) {
            if (m_exceeded) {
                return;
            }
            add(UnitsPerKeyword + qint64(member.key().size()) / StringBytesPerUnit, 0, context);
            apply(node.propertyNames, Json(member.key()), names);
        }
    }
}

void Estimator::applyItem(int index, const Json& value, std::size_t item, const Context& context)
{
    Context inner = context;
    enter(Step{{}, item, true}, inner);
    apply(index, value.at(item), inner);
    leave();
}

void Estimator::applyArray(const SchemaNode& node, const Json& value, const Context& context)
{
    const std::size_t size = value.size();
    std::size_t index = 0;
    for (; index < size && index < node.prefixItems.size(); ++index) {
        applyItem(node.prefixItems[index], value, index, deeper(context, 2));
    }
    if (node.items >= 0) {
        for (; index < size && !m_exceeded; ++index) {
            applyItem(node.items, value, index, deeper(context, 1));
        }
    }
    if (node.contains >= 0) {
        for (std::size_t i = 0; i < size && !m_exceeded; ++i) {
            applyItem(node.contains, value, i, collected(context, 1));
        }
    }
    if (node.unevaluatedItems >= 0) {
        // jsoncons looks each index up in the list of the ranges that the
        // other keywords evaluated, a few without contains, which
        // checkUnevaluated() keeps away from it.
        add(4 * UnitsPerKeyword * qint64(size), 0, context);
        if (!m_nodes[std::size_t(node.unevaluatedItems)].isBoolean) {
            for (std::size_t i = 0; i < size && !m_exceeded; ++i) {
                applyItem(node.unevaluatedItems, value, i, deeper(context, 2));
            }
        }
    }
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

// A quoted name in a description, cut short as a pointer's token is.
QString quoted(const QString& name)
{
    return QLatin1Char('"') + shownName(name) + QLatin1Char('"');
}

QStringList tokensOf(const jsoncons::jsonpointer::json_pointer& pointer, std::vector<std::string>& raw)
{
    QStringList names;
    for (const std::string& token : pointer) {
        raw.push_back(token);
        names.append(QString::fromStdString(token));
    }
    return names;
}

// Where an error of jsoncons is in the schema, read from its evaluation
// path: the node whose keyword failed, and how the path ends there.
struct Place
{
    enum class End {
        // The path does not fit the schema.
        Lost,
        // A keyword of the node failed: `keyword`.
        Keyword,
        // The node is a false schema.
        Node,
        // A member or item that additionalProperties, unevaluatedProperties,
        // unevaluatedItems or items does not allow, at the error's place.
        Member,
        // additionalProperties failed for a member of the object at the
        // error's place. jsoncons does not say which, nor why.
        AdditionalSchema,
        // propertyNames failed for a name of the object at the error's
        // place. jsoncons does not say which.
        PropertyNames,
    };
    End end = End::Lost;
    int node = -1;
    std::string keyword;
};

// Builds the lines of jsoncons' errors from the schema and the value, never
// from jsoncons' English text.
class Describer
{
public:
    using Auxiliary = std::function<const jss::json_schema<Json>*(int)>;

    Describer(const std::vector<SchemaNode>& nodes, Auxiliary auxiliary, Report& report)
        : m_nodes(nodes)
        , m_auxiliary(std::move(auxiliary))
        , m_report(report)
    {
    }

    // The value and schema an error's places are relative to: the
    // arguments and the whole schema, or one part of the arguments checked
    // again against the subschema `start` through an auxiliary schema.
    struct Frame
    {
        int start = 0;
        const Json* root = nullptr;
        QStringList prefix;
        // The value is a property name, which the lines say.
        bool propertyName = false;
        // The evaluation path starts with the auxiliary schema's "$ref".
        bool auxiliary = false;
    };

    void describe(const jss::validation_message& message, const Frame& frame);

private:
    Place locate(const std::vector<std::string>& tokens, std::size_t first, int start) const;
    void add(const Frame& frame, const QStringList& names, const QString& description);
    void describeKeyword(const jss::validation_message& message, const Frame& frame, const Place& place,
                         const QStringList& names, const std::vector<std::string>& instance);
    void recheckMembers(int nodeIndex, const QStringList& names, const Json* object);
    void recheckNames(int nodeIndex, const QStringList& names, const Json* object);
    void recheck(int target, const Json& value, const QStringList& names, bool propertyName);
    bool once(const QString& what, int node, const QStringList& names)
    {
        const QString key = what + QString::number(node) + QLatin1Char(':') + fullPointer(names);
        if (m_done.contains(key)) {
            return false;
        }
        m_done.insert(key);
        return true;
    }

    const std::vector<SchemaNode>& m_nodes;
    Auxiliary m_auxiliary;
    Report& m_report;
    QSet<QString> m_done;
};

Place Describer::locate(const std::vector<std::string>& tokens, std::size_t first, int start) const
{
    const auto index = [](const std::string& token, const std::vector<int>& list) -> int {
        if (token.empty() || token.size() > 9 || !std::all_of(token.begin(), token.end(), [](char c) {
                return c >= '0' && c <= '9';
            })) {
            return -1;
        }
        const std::size_t i = std::stoul(token);
        return i < list.size() ? list[i] : -1;
    };
    int node = start;
    std::size_t i = first;
    while (i < tokens.size()) {
        const SchemaNode& n = m_nodes[std::size_t(node)];
        if (n.isBoolean) {
            return {};
        }
        const std::string& token = tokens[i];
        const bool last = i + 1 == tokens.size();
        int next = -1;
        std::size_t used = 1;
        if (token == "properties" && !last) {
            const auto it = n.properties.find(tokens[i + 1]);
            next = it == n.properties.end() ? -1 : it->second;
            used = 2;
        } else if ((token == "prefixItems" || token == "allOf" || token == "anyOf" || token == "oneOf") && !last) {
            const std::vector<int>& list = token == "prefixItems" ? n.prefixItems
                : token == "allOf"                                ? n.allOf
                : token == "anyOf"                                ? n.anyOf
                                                                  : n.oneOf;
            next = index(tokens[i + 1], list);
            used = 2;
        } else if (token == "items" && !last) {
            next = n.items;
        } else if (token == "if") {
            next = n.ifSchema;
        } else if (token == "then") {
            next = n.thenSchema;
        } else if (token == "else") {
            next = n.elseSchema;
        } else if (token == "$ref") {
            next = n.ref;
        } else if (token == "not" && !last) {
            next = n.notSchema;
        } else if (token == "propertyNames") {
            return {Place::End::PropertyNames, node, token};
        } else if (token == "additionalProperties") {
            if (last) {
                return {Place::End::AdditionalSchema, node, token};
            }
            return i + 2 == tokens.size() ? Place{Place::End::Member, node, token} : Place{};
        } else if (token == "unevaluatedProperties" || token == "unevaluatedItems") {
            // jsoncons names unevaluatedItems "unevaluatedProperties" too.
            if (i + 2 == tokens.size()) {
                return {Place::End::Member, node, token};
            }
            next = n.unevaluatedItems;
            used = 2;
        } else if (token == "contains" && !last) {
            // minContains or maxContains, which jsoncons checks for every
            // contains, as 1 and no bound when the schema has neither.
            return i + 2 == tokens.size() ? Place{Place::End::Keyword, node, tokens[i + 1]} : Place{};
        } else if (token == "dependentRequired") {
            return {Place::End::Keyword, node, token};
        } else if (last) {
            return {Place::End::Keyword, node, token};
        }
        if (next < 0) {
            return {};
        }
        node = next;
        i += used;
    }
    return {Place::End::Node, node, std::string()};
}

void Describer::add(const Frame& frame, const QStringList& names, const QString& description)
{
    m_report.add(names, frame.propertyName ? QStringLiteral("the property name ") + description : description);
}

void Describer::describe(const jss::validation_message& message, const Frame& frame)
{
    if (m_report.isDone()) {
        return;
    }
    std::vector<std::string> tokens;
    for (const std::string& token : message.eval_path()) {
        tokens.push_back(token);
    }
    const std::size_t first = frame.auxiliary && !tokens.empty() && tokens.front() == "$ref" ? 1 : 0;
    const Place place = locate(tokens, first, frame.start);

    std::vector<std::string> instance;
    QStringList names = frame.prefix;
    const QStringList relative = tokensOf(message.instance_location(), instance);
    if (!frame.propertyName) {
        names += relative;
    }

    switch (place.end) {
    case Place::End::Lost:
    case Place::End::Node:
        add(frame, names, QStringLiteral("does not match the schema"));
        return;
    case Place::End::Member: {
        const Json* parent = instance.empty() ? nullptr : jsonAt(*frame.root, instance, instance.size() - 1);
        if (parent != nullptr && parent->is_array()) {
            add(frame, names, QStringLiteral("is not allowed, because the schema allows no more items"));
        } else {
            add(frame, names, QStringLiteral("is not a known property"));
        }
        return;
    }
    case Place::End::AdditionalSchema:
        recheckMembers(place.node, names, jsonAt(*frame.root, instance, instance.size()));
        return;
    case Place::End::PropertyNames:
        recheckNames(place.node, names, jsonAt(*frame.root, instance, instance.size()));
        return;
    case Place::End::Keyword:
        describeKeyword(message, frame, place, names, instance);
        return;
    }
}

void Describer::describeKeyword(const jss::validation_message& message, const Frame& frame, const Place& place,
                                const QStringList& names, const std::vector<std::string>& instance)
{
    const SchemaNode& node = m_nodes[std::size_t(place.node)];
    const QString keyword = QString::fromStdString(place.keyword);
    const QJsonValue value = node.object.value(keyword);

    if (keyword == QLatin1StringView("type")) {
        QStringList types;
        if (value.isArray()) {
            for (const QJsonValue& entry : value.toArray()) {
                types.append(entry.toString());
            }
        } else {
            types.append(value.toString());
        }
        add(frame, names, QStringLiteral("must be of type ") + alternatives(types));
        return;
    }
    static const QHash<QString, QString> fixed{
        {QStringLiteral("enum"), QStringLiteral("must be one of the values of enum")},
        {QStringLiteral("const"), QStringLiteral("must be the value of const")},
        {QStringLiteral("not"), QStringLiteral("must not match the schema of not")},
    };
    if (const auto text = fixed.constFind(keyword); text != fixed.cend()) {
        add(frame, names, *text);
        return;
    }
    if (keyword == QLatin1StringView("required")) {
        // jsoncons names the missing property in its English text only, so
        // each one is found from the schema and the value, once.
        const Json* object = jsonAt(*frame.root, instance, instance.size());
        if (object == nullptr || !object->is_object() || !once(QStringLiteral("required"), place.node, names)) {
            return;
        }
        for (const QJsonValue& name : value.toArray()) {
            if (object->find(name.toString().toStdString()) == object->object_range().end()) {
                add(frame, names + QStringList{name.toString()}, QStringLiteral("is required"));
            }
        }
        return;
    }
    if (keyword == QLatin1StringView("dependentRequired")) {
        // jsoncons reports it at the place of the property that requires
        // the others.
        if (instance.empty()) {
            return;
        }
        const Json* object = jsonAt(*frame.root, instance, instance.size() - 1);
        const QStringList objectNames = names.first(names.size() - 1);
        const QString trigger = names.last();
        if (object == nullptr || !object->is_object()
            || !once(QStringLiteral("dependentRequired/") + trigger, place.node, objectNames)) {
            return;
        }
        for (const QJsonValue& name : value.toObject().value(trigger).toArray()) {
            if (object->find(name.toString().toStdString()) == object->object_range().end()) {
                add(frame, objectNames + QStringList{name.toString()},
                    QStringLiteral("is required when %1 is present").arg(quoted(trigger)));
            }
        }
        return;
    }

    struct Counted
    {
        const char* keyword;
        const char* description;
        const char* one;
    };
    static const Counted counted[] = {
        {"maxLength", "must be at most %1 characters long", "must be at most 1 character long"},
        {"minLength", "must be at least %1 characters long", "must be at least 1 character long"},
        {"maxItems", "must have at most %1 items", "must have at most 1 item"},
        {"minItems", "must have at least %1 items", "must have at least 1 item"},
        {"maxProperties", "must have at most %1 properties", "must have at most 1 property"},
        {"minProperties", "must have at least %1 properties", "must have at least 1 property"},
        {"maxContains", "must have at most %1 items that match contains",
         "must have at most 1 item that matches contains"},
        {"minContains", "must have at least %1 items that match contains", "must have an item that matches contains"},
    };
    for (const Counted& entry : counted) {
        if (keyword != QLatin1StringView(entry.keyword)) {
            continue;
        }
        // jsoncons checks minContains as 1 when the schema has none.
        const qint64 count = value.isUndefined() && keyword == QLatin1StringView("minContains") ? 1 : value.toInteger();
        add(frame, names,
            count == 1 ? QString::fromLatin1(entry.one) : QString::fromLatin1(entry.description).arg(count));
        return;
    }
    struct Bound
    {
        const char* keyword;
        const char* description;
    };
    static const Bound bounds[] = {
        {"minimum", "must be at least %1"},
        {"maximum", "must be at most %1"},
        {"exclusiveMinimum", "must be greater than %1"},
        {"exclusiveMaximum", "must be less than %1"},
        {"multipleOf", "must be a multiple of %1"},
    };
    for (const Bound& bound : bounds) {
        if (keyword == QLatin1StringView(bound.keyword)) {
            add(frame, names, QString::fromLatin1(bound.description).arg(formatNumber(value)));
            return;
        }
    }
    if (keyword == QLatin1StringView("items")) {
        // A false items, which jsoncons reports once, at the first item it
        // does not allow.
        const std::size_t allowed = node.prefixItems.size();
        add(frame, names,
            allowed == 0 ? QStringLiteral("is not allowed, because the schema allows no items")
                : allowed == 1
                ? QStringLiteral("is not allowed, because the schema allows at most 1 item")
                : QStringLiteral("is not allowed, because the schema allows at most %1 items").arg(allowed));
        return;
    }
    if (keyword == QLatin1StringView("allOf") || keyword == QLatin1StringView("anyOf")
        || keyword == QLatin1StringView("oneOf")) {
        // The errors of each branch that failed, which jsoncons gives as the
        // details of the combinator's error.
        bool none = keyword != QLatin1StringView("oneOf");
        if (!none) {
            // A oneOf fails when no branch matches, or more than one does.
            // In the first case every branch has errors.
            const std::size_t depth = std::distance(message.eval_path().begin(), message.eval_path().end());
            std::set<std::string> failed;
            for (const jss::validation_message& detail : message.details()) {
                std::size_t i = 0;
                for (const std::string& token : detail.eval_path()) {
                    if (i++ == depth) {
                        failed.insert(token);
                        break;
                    }
                }
            }
            none = failed.size() == node.oneOf.size();
        }
        if (none) {
            for (const jss::validation_message& detail : message.details()) {
                if (m_report.isDone()) {
                    return;
                }
                describe(detail, frame);
            }
        }
        if (keyword == QLatin1StringView("anyOf")) {
            add(frame, names, QStringLiteral("must match at least one schema of anyOf"));
        } else if (keyword == QLatin1StringView("oneOf")) {
            add(frame, names,
                none ? QStringLiteral("must match exactly one schema of oneOf, and matches none")
                     : QStringLiteral("must match exactly one schema of oneOf, and matches more than one"));
        } else if (message.details().empty()) {
            add(frame, names, QStringLiteral("does not match the schema"));
        }
        return;
    }
    add(frame, names, QStringLiteral("does not match the schema"));
}

// additionalProperties with a schema failed for some members of `object`,
// which jsoncons names in its English text only, without the errors. Each
// member that it applies to is checked again on its own, so that its errors
// are listed at its place.
void Describer::recheckMembers(int nodeIndex, const QStringList& names, const Json* object)
{
    if (object == nullptr || !object->is_object() || !once(QStringLiteral("additionalProperties"), nodeIndex, names)) {
        return;
    }
    const SchemaNode& node = m_nodes[std::size_t(nodeIndex)];
    for (const auto& member : object->object_range()) {
        if (m_report.isDone()) {
            return;
        }
        if (node.properties.count(member.key()) == 0) {
            recheck(node.additionalProperties, member.value(),
                    names + QStringList{QString::fromStdString(member.key())}, false);
        }
    }
}

// propertyNames failed for some names of `object`. jsoncons reports the
// error of the name's check at the object's place, so each name is checked
// again on its own.
void Describer::recheckNames(int nodeIndex, const QStringList& names, const Json* object)
{
    if (object == nullptr || !object->is_object() || !once(QStringLiteral("propertyNames"), nodeIndex, names)) {
        return;
    }
    const int target = m_nodes[std::size_t(nodeIndex)].propertyNames;
    const bool none = m_nodes[std::size_t(target)].isBoolean;
    for (const auto& member : object->object_range()) {
        if (m_report.isDone()) {
            return;
        }
        const QStringList memberNames = names + QStringList{QString::fromStdString(member.key())};
        if (none) {
            m_report.add(memberNames, QStringLiteral("is not allowed, because the schema allows no property here"));
        } else {
            recheck(target, Json(member.key()), memberNames, true);
        }
    }
}

void Describer::recheck(int target, const Json& value, const QStringList& names, bool propertyName)
{
    const jss::json_schema<Json>* schema = m_auxiliary(target);
    if (schema == nullptr) {
        m_report.add(names,
                     propertyName ? QStringLiteral("the property name does not match propertyNames")
                                  : QStringLiteral("does not match the schema of additionalProperties"));
        return;
    }
    const Frame frame{target, &value, names, propertyName, true};
    schema->validate(value, [&](const jss::validation_message& message) -> jss::walk_state {
        describe(message, frame);
        return m_report.isDone() ? jss::walk_state::abort : jss::walk_state::advance;
    });
}

} // namespace

struct McpSchema::Private
{
    // Inserts each $anchor that a $ref or an auxiliary schema needs into the
    // copy of the schema for jsoncons, and makes each $ref name its
    // target's anchor. jsoncons then never reads a JSON pointer in a URI,
    // whose encoding it does not normalize.
    void prepare(const QJsonObject& schema);
    const jss::json_schema<Json>* auxiliary(int node) const;

    std::vector<SchemaNode> nodes;
    // The anchor of each node that has or needs one.
    std::vector<std::string> anchors;
    // The schema as jsoncons compiled it.
    Json document;
    std::optional<jss::json_schema<Json>> schema;
    // The schemas that describe() checks a member or a name against again,
    // by their node: the document under $defs, and a $ref to the node.
    mutable std::map<int, std::unique_ptr<jss::json_schema<Json>>> auxiliaries;
    bool compiled = false;
};

void McpSchema::Private::prepare(const QJsonObject& schema)
{
    document = toJson(schema);
    document.erase("$schema");
    anchors.assign(nodes.size(), std::string());
    QSet<QString> taken;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (!nodes[i].anchor.isEmpty()) {
            anchors[i] = nodes[i].anchor.toStdString();
            taken.insert(nodes[i].anchor);
        }
    }
    std::vector<bool> needed(nodes.size(), false);
    for (const SchemaNode& node : nodes) {
        for (const int target : {node.ref, node.additionalProperties, node.propertyNames}) {
            if (target >= 0 && !nodes[std::size_t(target)].isBoolean) {
                needed[std::size_t(target)] = true;
            }
        }
    }
    const auto at = [this](const QStringList& path) -> Json* {
        Json* value = &document;
        for (const QString& name : path) {
            if (value->is_array()) {
                value = &value->at(std::size_t(name.toULongLong()));
            } else {
                value = &value->at(name.toStdString());
            }
        }
        return value;
    };
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (!needed[i] || !anchors[i].empty()) {
            continue;
        }
        QString name = QStringLiteral("phosphorMcp%1").arg(i);
        while (taken.contains(name)) {
            name.append(QLatin1Char('_'));
        }
        taken.insert(name);
        anchors[i] = name.toStdString();
        at(nodes[i].path)->insert_or_assign("$anchor", anchors[i]);
    }
    for (const SchemaNode& node : nodes) {
        if (node.ref >= 0) {
            at(node.path)->insert_or_assign("$ref", "#" + anchors[std::size_t(node.ref)]);
        }
    }
}

const jss::json_schema<Json>* McpSchema::Private::auxiliary(int node) const
{
    auto it = auxiliaries.find(node);
    if (it == auxiliaries.end()) {
        std::unique_ptr<jss::json_schema<Json>> compiled;
        if (!anchors[std::size_t(node)].empty()) {
            Json root(jsoncons::json_object_arg);
            Json definitions(jsoncons::json_object_arg);
            definitions.try_emplace("phosphorMcp", document);
            root.try_emplace("$defs", std::move(definitions));
            root.try_emplace("$ref", "#" + anchors[std::size_t(node)]);
            try {
                compiled = std::make_unique<jss::json_schema<Json>>(jss::make_json_schema(std::move(root)));
            } catch (const std::exception&) {
                compiled.reset();
            }
        }
        it = auxiliaries.emplace(node, std::move(compiled)).first;
    }
    return it->second.get();
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
    std::vector<SchemaNode> nodes;
    SchemaBuilder builder(nodes);
    if (builder.build(schema, QStringList()) < 0) {
        return builder.error();
    }
    const QString error = builder.finish();
    if (!error.isEmpty()) {
        return error;
    }
    try {
        d->nodes = std::move(nodes);
        d->prepare(schema);
        const jss::evaluation_options options = jss::evaluation_options{}
                                                    .default_version(jss::schema_version::draft202012())
                                                    .require_format_validation(false);
        d->schema.emplace(jss::make_json_schema(Json(d->document), options));
        d->compiled = true;
        return QString();
    } catch (const std::exception& e) {
        d->schema.reset();
        d->nodes.clear();
        return QStringLiteral(": the server cannot compile the schema (%1)").arg(QString::fromUtf8(e.what()));
    } catch (...) {
        d->schema.reset();
        d->nodes.clear();
        return QStringLiteral(": the server cannot compile the schema");
    }
}

QStringList McpSchema::validate(const QJsonValue& value) const
{
    if (!d->compiled) {
        return {QStringLiteral(": the schema did not compile")};
    }
    // jsoncons throws on what it cannot check. Every caller of this file is
    // built without exceptions, so nothing may escape.
    try {
        Report report;
        Inspector inspector(report);
        inspector.walk(value);
        if (!report.isEmpty()) {
            return report.take();
        }
        // Each string is converted to UTF-8 here, once for all the checks.
        const Json instance = toJson(value);
        Estimator estimator(d->nodes, report);
        if (!estimator.fits(0, instance)) {
            return {TooLargeToCheck};
        }
        if (!report.isEmpty()) {
            return report.take();
        }
        Describer describer(
            d->nodes,
            [this](int node) {
                return d->auxiliary(node);
            },
            report);
        const Describer::Frame frame{0, &instance, QStringList(), false, false};
        bool failed = false;
        d->schema->validate(instance, [&](const jss::validation_message& message) -> jss::walk_state {
            failed = true;
            describer.describe(message, frame);
            return report.isDone() ? jss::walk_state::abort : jss::walk_state::advance;
        });
        if (!failed) {
            return {};
        }
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
