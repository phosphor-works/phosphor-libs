// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// The compile of a tool's schema: the keyword policy (see the README,
// "Schemas"), the nodes, the resolution of each $ref, and the refusal of a
// $ref that loops on the same value.

#include "mcpschemacheck.h"

#include <QJsonObject>
#include <QLatin1StringView>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <optional>

namespace PhosphorMcp::Schema {

namespace {

// 2^53, from which doubles no longer hold every integer.
constexpr double TwoTo53 = 9007199254740992.0;

QString fullPointer(const QStringList& tokens)
{
    QString pointer;
    for (const QString& token : tokens) {
        pointer += QLatin1Char('/') + pointerToken(token);
    }
    return pointer;
}

const QHash<QString, quint8>& typeBits()
{
    static const QHash<QString, quint8> bits{
        {QStringLiteral("null"), TypeNull},       {QStringLiteral("boolean"), TypeBoolean},
        {QStringLiteral("object"), TypeObject},   {QStringLiteral("array"), TypeArray},
        {QStringLiteral("number"), TypeNumber},   {QStringLiteral("string"), TypeString},
        {QStringLiteral("integer"), TypeInteger},
    };
    return bits;
}

// Lengths and counts: an integer that every JSON reader holds exactly.
bool isCount(const QJsonValue& value)
{
    if (!value.isDouble()) {
        return false;
    }
    const Number number = numberOf(value);
    return number.isInteger && number.integer >= 0 && double(number.integer) < TwoTo53;
}

// A plain-name fragment, as 2020-12 defines $anchor:
// ^[A-Za-z_][-A-Za-z0-9._]*$.
bool isAnchorName(const QString& name)
{
    if (name.isEmpty()) {
        return false;
    }
    for (qsizetype i = 0; i < name.size(); ++i) {
        const char16_t c = name.at(i).unicode();
        const bool first = (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') || c == u'_';
        const bool other = (c >= u'0' && c <= u'9') || c == u'-' || c == u'.';
        if (!first && (i == 0 || !other)) {
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

enum class FragmentError {
    None,
    // A character that a URI cannot hold, or a "%" without two hex digits.
    Syntax,
    // Percent-encoded bytes that are not UTF-8.
    NotUtf8,
};

// The fragment of a URI reference, percent-decoded.
std::optional<QString> decodeFragment(const QString& fragment, FragmentError& error)
{
    QByteArray bytes;
    for (qsizetype i = 0; i < fragment.size(); ++i) {
        const char16_t c = fragment.at(i).unicode();
        if (c == u'%') {
            const int high = i + 2 < fragment.size() ? hexValue(fragment.at(i + 1).unicode()) : -1;
            const int low = i + 2 < fragment.size() ? hexValue(fragment.at(i + 2).unicode()) : -1;
            if (high < 0 || low < 0) {
                error = FragmentError::Syntax;
                return std::nullopt;
            }
            bytes.append(char(high * 16 + low));
            i += 2;
        } else if (isFragmentCharacter(c)) {
            bytes.append(char(c));
        } else {
            error = FragmentError::Syntax;
            return std::nullopt;
        }
    }
    const QString decoded = QString::fromUtf8(bytes);
    if (decoded.toUtf8() != bytes) {
        error = FragmentError::NotUtf8;
        return std::nullopt;
    }
    return decoded;
}

// The tokens of a JSON pointer (RFC 6901), unescaped, or nothing when it is
// not one.
std::optional<QStringList> pointerTokens(const QString& pointer)
{
    QStringList tokens;
    if (pointer.isEmpty()) {
        return tokens;
    }
    if (!pointer.startsWith(QLatin1Char('/'))) {
        return std::nullopt;
    }
    for (const QString& raw : pointer.mid(1).split(QLatin1Char('/'))) {
        QString token;
        for (qsizetype i = 0; i < raw.size(); ++i) {
            if (raw.at(i) != QLatin1Char('~')) {
                token += raw.at(i);
            } else if (i + 1 < raw.size() && (raw.at(i + 1) == QLatin1Char('0') || raw.at(i + 1) == QLatin1Char('1'))) {
                token += raw.at(i + 1) == QLatin1Char('0') ? QLatin1Char('~') : QLatin1Char('/');
                ++i;
            } else {
                return std::nullopt;
            }
        }
        tokens.append(token);
    }
    return tokens;
}

// An array index in a JSON pointer: "0", or digits without a leading zero.
int arrayIndex(const QString& token, std::size_t size)
{
    if (token.isEmpty() || token.size() > 9 || (token.size() > 1 && token.front() == QLatin1Char('0'))) {
        return -1;
    }
    int index = 0;
    for (const QChar c : token) {
        if (c < QLatin1Char('0') || c > QLatin1Char('9')) {
            return -1;
        }
        index = index * 10 + (c.unicode() - u'0');
    }
    return std::size_t(index) < size ? index : -1;
}

// The subschemas that apply to the same value as `node`.
std::vector<int> inPlace(const Node& node)
{
    std::vector<int> next = node.allOf;
    next.insert(next.end(), node.anyOf.begin(), node.anyOf.end());
    next.insert(next.end(), node.oneOf.begin(), node.oneOf.end());
    for (const int single : {node.notSchema, node.ifSchema, node.thenSchema, node.elseSchema}) {
        if (single >= 0) {
            next.push_back(single);
        }
    }
    for (const auto& dependent : node.dependentSchemas) {
        next.push_back(dependent.second);
    }
    if (node.ref >= 0) {
        next.push_back(node.ref);
    }
    return next;
}

// Checks a schema against the keyword policy and builds its nodes.
class SchemaBuilder
{
public:
    explicit SchemaBuilder(std::vector<Node>& nodes)
        : m_nodes(nodes)
    {
    }

    // The index of the node of `schema`, the child of `parent` by `keyword`
    // (and `name`), or -1 with error() set. The caller pushed the tokens to
    // it on m_path.
    int build(const QJsonValue& schema, int parent, const QString& keyword, const QString* name, int depth);
    // After build(): resolves each $ref, and refuses a loop.
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
    [[nodiscard]] QString here() const
    {
        return fullPointer(m_path);
    }
    // The child schema `value` at the keyword on m_path (and `name`).
    int child(int parent, const QString& keyword, const QJsonValue& value, const QString* name, int depth);
    bool childArray(int parent, const QString& keyword, const QJsonValue& value, std::vector<int>& branches, int depth);
    bool childObject(int parent, const QString& keyword, const QJsonValue& value, const QString& what,
                     std::vector<std::pair<QString, int>>& children, int depth);
    QString keyword(int index, const QString& key, const QJsonValue& value, int depth);
    QString checkLiteral(const QJsonValue& value, int depth);
    QString checkType(int index, const QJsonValue& value);
    QString checkNames(const QJsonValue& value, QStringList& names);
    QString resolveReference(int index);
    int resolvePointer(const QStringList& tokens) const;
    QString findCycle();

    std::vector<Node>& m_nodes;
    // The tokens from the root to the place being built, for an error.
    QStringList m_path;
    QHash<QString, int> m_anchors;
    QString m_error;
    // The contentSchema keywords that the place being built is inside.
    int m_contentSchemas = 0;
};

int SchemaBuilder::build(const QJsonValue& schema, int parent, const QString& keyword, const QString* name, int depth)
{
    if (!schema.isBool() && !schema.isObject()) {
        return fail(QStringLiteral("%1: a schema must be an object or a boolean").arg(here()));
    }
    if (qsizetype(m_nodes.size()) >= MaxNodes) {
        return fail(
            QStringLiteral(": the schema has more than %1 subschemas, which the server does not check").arg(MaxNodes));
    }
    if (depth > MaxNodeDepth) {
        return fail(QStringLiteral("%1: is nested more than %2 subschemas deep, which the server does not check")
                        .arg(here(), QString::number(MaxNodeDepth)));
    }
    const int index = int(m_nodes.size());
    m_nodes.emplace_back();
    Node& node = m_nodes.back();
    node.parent = parent;
    node.keyword = keyword;
    node.inContentSchema = m_contentSchemas > 0;
    if (name) {
        node.name = *name;
        node.hasName = true;
    }
    if (schema.isBool()) {
        node.isBoolean = true;
        node.booleanValue = schema.toBool();
        return index;
    }
    const QJsonObject object = schema.toObject();
    for (auto it = object.begin(); it != object.end(); ++it) {
        const QString key = it.key();
        m_path.append(key);
        const QString error = this->keyword(index, key, it.value(), depth);
        m_path.removeLast();
        if (!error.isEmpty()) {
            return fail(error);
        }
    }
    return index;
}

int SchemaBuilder::child(int parent, const QString& keyword, const QJsonValue& value, const QString* name, int depth)
{
    if (name) {
        m_path.append(*name);
    }
    const int index = build(value, parent, keyword, name, depth + 1);
    if (name) {
        m_path.removeLast();
    }
    return index;
}

bool SchemaBuilder::childArray(int parent, const QString& keyword, const QJsonValue& value, std::vector<int>& branches,
                               int depth)
{
    if (!value.isArray() || value.toArray().isEmpty()) {
        fail(QStringLiteral("%1: must be a non-empty array of schemas").arg(here()));
        return false;
    }
    const QJsonArray array = value.toArray();
    for (qsizetype i = 0; i < array.size(); ++i) {
        const QString position = QString::number(i);
        const int index = child(parent, keyword, array.at(i), &position, depth);
        if (index < 0) {
            return false;
        }
        branches.push_back(index);
    }
    return true;
}

// An object of schemas by property name (properties, dependentSchemas,
// $defs). A name with an unpaired surrogate is refused: no argument has
// one, and no URI reaches it.
bool SchemaBuilder::childObject(int parent, const QString& keyword, const QJsonValue& value, const QString& what,
                                std::vector<std::pair<QString, int>>& children, int depth)
{
    if (!value.isObject()) {
        fail(QStringLiteral("%1: must be an object of schemas").arg(here()));
        return false;
    }
    const QJsonObject object = value.toObject();
    for (auto it = object.begin(); it != object.end(); ++it) {
        const QString name = it.key();
        if (!name.isValidUtf16()) {
            fail(QStringLiteral("%1/%2: %3 with an unpaired UTF-16 surrogate is not supported")
                     .arg(here(), pointerToken(name), what));
            return false;
        }
        const int index = child(parent, keyword, it.value(), &name, depth);
        if (index < 0) {
            return false;
        }
        children.emplace_back(name, index);
    }
    return true;
}

// A value that a schema compares arguments with (enum, const): no string or
// member name with an unpaired surrogate, which no argument has, and no
// number that is not finite, which JSON has no form for.
QString SchemaBuilder::checkLiteral(const QJsonValue& value, int depth)
{
    if (depth > MaxNestingDepth) {
        return QStringLiteral("%1: is nested more than %2 levels deep, which the server does not check")
            .arg(here(), QString::number(MaxNestingDepth));
    }
    if (value.isString() && !value.toString().isValidUtf16()) {
        return QStringLiteral("%1: a string with an unpaired UTF-16 surrogate is not supported").arg(here());
    }
    if (value.isDouble() && !std::isfinite(value.toDouble())) {
        return QStringLiteral("%1: a number that is not finite is not supported").arg(here());
    }
    if (value.isArray()) {
        const QJsonArray array = value.toArray();
        for (qsizetype i = 0; i < array.size(); ++i) {
            m_path.append(QString::number(i));
            const QString error = checkLiteral(array.at(i), depth + 1);
            m_path.removeLast();
            if (!error.isEmpty()) {
                return error;
            }
        }
    } else if (value.isObject()) {
        const QJsonObject object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it) {
            m_path.append(it.key());
            QString error;
            if (!it.key().isValidUtf16()) {
                error =
                    QStringLiteral("%1: a member name with an unpaired UTF-16 surrogate is not supported").arg(here());
            } else {
                error = checkLiteral(it.value(), depth + 1);
            }
            m_path.removeLast();
            if (!error.isEmpty()) {
                return error;
            }
        }
    }
    return QString();
}

QString SchemaBuilder::checkType(int index, const QJsonValue& value)
{
    Node& node = m_nodes[std::size_t(index)];
    if (value.isString()) {
        const auto bit = typeBits().constFind(value.toString());
        if (bit == typeBits().cend()) {
            return QStringLiteral("%1: unknown type \"%2\"").arg(here(), value.toString());
        }
        node.types = *bit;
        node.typeNames = {value.toString()};
        return QString();
    }
    if (!value.isArray() || value.toArray().isEmpty()) {
        return QStringLiteral("%1: must be a type name or a non-empty array of them").arg(here());
    }
    for (const QJsonValue& entry : value.toArray()) {
        const auto bit = entry.isString() ? typeBits().constFind(entry.toString()) : typeBits().cend();
        if (bit == typeBits().cend() || (node.types & *bit) != 0) {
            return QStringLiteral("%1: must be a type name or an array of distinct type names").arg(here());
        }
        node.types |= *bit;
        node.typeNames.append(entry.toString());
    }
    return QString();
}

// An array of distinct property names. The meta-schema has uniqueItems on
// required: a client that checks the schema refuses a name twice.
QString SchemaBuilder::checkNames(const QJsonValue& value, QStringList& names)
{
    if (!value.isArray()) {
        return QStringLiteral("%1: must be an array of distinct property names").arg(here());
    }
    QSet<QString> seen;
    const QJsonArray array = value.toArray();
    for (qsizetype i = 0; i < array.size(); ++i) {
        const QJsonValue name = array.at(i);
        if (!name.isString() || seen.contains(name.toString())) {
            return QStringLiteral("%1: must be an array of distinct property names").arg(here());
        }
        if (!name.toString().isValidUtf16()) {
            return QStringLiteral("%1/%2: a property name with an unpaired UTF-16 surrogate is not supported")
                .arg(here(), QString::number(i));
        }
        seen.insert(name.toString());
        names.append(name.toString());
    }
    return QString();
}

// Checks one keyword of the node `index` (on m_path) and builds what it
// applies. The node is looked up again after each child, which grows
// m_nodes.
QString SchemaBuilder::keyword(int index, const QString& key, const QJsonValue& value, int depth)
{
    const auto node = [this, index]() -> Node& {
        return m_nodes[std::size_t(index)];
    };
    const auto is = [&key](const char* name) {
        return key == QLatin1StringView(name);
    };

    // Refused, with the reason.
    if (is("patternProperties") && !value.isObject()) {
        return QStringLiteral("%1: must be an object of schemas").arg(here());
    }
    if (is("pattern") || (is("patternProperties") && !value.toObject().isEmpty())) {
        // A regular expression engine on text that the model chose is its
        // own risk (stack depth, backtracking time, UTF-16 or code points),
        // which the server does not take.
        return QStringLiteral(
                   "%1: \"%2\" is not supported, because the server cannot match regular expressions safely. "
                   "Use \"enum\", or check the value in the handler")
            .arg(here(), key);
    }
    if (is("$id")) {
        return QStringLiteral(
                   "%1: \"$id\" is not supported, because the server resolves references within the tool's schema "
                   "only")
            .arg(here());
    }
    if (is("$dynamicRef") || is("$dynamicAnchor")) {
        return QStringLiteral(
                   "%1: \"%2\" is not supported, because the server does not resolve dynamic references. Use "
                   "\"$ref\"")
            .arg(here(), key);
    }
    if (is("$vocabulary")) {
        return QStringLiteral("%1: \"$vocabulary\" is not supported, because it belongs in a meta-schema").arg(here());
    }
    static const QHash<QString, QString> olderKeywords{
        {QStringLiteral("definitions"), QStringLiteral(". Use \"$defs\"")},
        {QStringLiteral("dependencies"), QStringLiteral(". Use \"dependentRequired\" or \"dependentSchemas\"")},
        {QStringLiteral("additionalItems"), QStringLiteral(". Use \"items\" beside \"prefixItems\"")},
        {QStringLiteral("$recursiveRef"), QStringLiteral(". Use \"$ref\"")},
        {QStringLiteral("$recursiveAnchor"), QString()},
    };
    if (const auto older = olderKeywords.constFind(key); older != olderKeywords.cend()) {
        return QStringLiteral("%1: \"%2\" is not a keyword of JSON Schema 2020-12%3").arg(here(), key, *older);
    }

    // Annotations, checked but not applied.
    if (is("title") || is("description") || is("$comment") || is("format") || is("contentMediaType")
        || is("contentEncoding")) {
        return value.isString() ? QString() : QStringLiteral("%1: must be a string").arg(here());
    }
    if (is("readOnly") || is("writeOnly") || is("deprecated")) {
        return value.isBool() ? QString() : QStringLiteral("%1: must be a boolean").arg(here());
    }
    if (is("examples")) {
        return value.isArray() ? QString() : QStringLiteral("%1: must be an array").arg(here());
    }
    if (is("default") || is("patternProperties")) {
        // patternProperties here is {}, which applies nothing.
        return QString();
    }
    if (is("contentSchema")) {
        // An annotation about the decoded content, never applied. It is
        // checked as any subschema is, as a client that reads the
        // meta-schema checks it, and no $ref may lead into it.
        ++m_contentSchemas;
        const int built = child(index, key, value, nullptr, depth);
        --m_contentSchemas;
        if (built < 0) {
            return m_error;
        }
        node().contentSchema = built;
        return QString();
    }
    if (is("$schema")) {
        if (m_path.size() != 1) {
            return QStringLiteral("%1: \"$schema\" is supported at the root of the schema only").arg(here());
        }
        if (value != QJsonValue(QStringLiteral("https://json-schema.org/draft/2020-12/schema"))
            && value != QJsonValue(QStringLiteral("https://json-schema.org/draft/2020-12/schema#"))) {
            return QStringLiteral(
                       "%1: the schema must be JSON Schema 2020-12, the dialect that MCP clients read, so "
                       "\"$schema\" may name https://json-schema.org/draft/2020-12/schema only")
                .arg(here());
        }
        return QString();
    }
    if (is("$anchor")) {
        if (!value.isString() || !isAnchorName(value.toString())) {
            return QStringLiteral(
                       "%1: must be a name of letters, digits, \"-\", \"_\" and \".\" that starts with a letter or "
                       "\"_\"")
                .arg(here());
        }
        if (m_anchors.contains(value.toString())) {
            return QStringLiteral("%1: another schema has this $anchor").arg(here());
        }
        m_anchors.insert(value.toString(), index);
        return QString();
    }
    if (is("$defs")) {
        std::vector<std::pair<QString, int>> children;
        if (!childObject(index, key, value, QStringLiteral("a name"), children, depth)) {
            return m_error;
        }
        for (const auto& [name, child] : children) {
            node().definitions.insert(name, child);
        }
        return QString();
    }
    if (is("$ref")) {
        if (!value.isString()) {
            return QStringLiteral("%1: must be a string").arg(here());
        }
        node().refValue = value.toString();
        node().hasRef = true;
        return QString();
    }

    // Assertions.
    if (is("type")) {
        return checkType(index, value);
    }
    if (is("enum")) {
        if (!value.isArray()) {
            return QStringLiteral("%1: must be an array").arg(here());
        }
        const QString error = checkLiteral(value, 0);
        if (!error.isEmpty()) {
            return error;
        }
        Node& target = node();
        target.hasEnum = true;
        for (const QJsonValue& entry : value.toArray()) {
            if (entry.isString()) {
                const QString text = entry.toString();
                target.enumStrings.append(text);
                target.enumUtf8.append(text.toUtf8());
            } else {
                target.enumOthers.push_back(entry);
            }
        }
        if (target.enumStrings.size() > EnumHashThreshold) {
            target.enumTable.reserve(target.enumStrings.size());
            for (qsizetype i = 0; i < target.enumStrings.size(); ++i) {
                target.enumTable.insert(target.enumStrings.at(i), target.enumUtf8.at(i), 0);
            }
        }
        return QString();
    }
    if (is("const")) {
        const QString error = checkLiteral(value, 0);
        if (!error.isEmpty()) {
            return error;
        }
        node().hasConst = true;
        node().constValue = value;
        node().constString = value.toString();
        node().constUtf8 = node().constString.toUtf8();
        return QString();
    }
    struct Bound
    {
        const char* name;
        Node::NumberKeyword bit;
        Number Node::* slot;
    };
    static const Bound bounds[] = {
        {"minimum", Node::Minimum, &Node::minimum},
        {"maximum", Node::Maximum, &Node::maximum},
        {"exclusiveMinimum", Node::ExclusiveMinimum, &Node::exclusiveMinimum},
        {"exclusiveMaximum", Node::ExclusiveMaximum, &Node::exclusiveMaximum},
        {"multipleOf", Node::MultipleOf, &Node::multipleOf},
    };
    for (const Bound& bound : bounds) {
        if (!is(bound.name)) {
            continue;
        }
        // A boolean exclusiveMinimum is Draft 4's form.
        if (!value.isDouble() || !std::isfinite(value.toDouble())) {
            return bound.bit == Node::MultipleOf ? QStringLiteral("%1: must be a number greater than 0").arg(here())
                                                 : QStringLiteral("%1: must be a number").arg(here());
        }
        const Number number = numberOf(value);
        if (bound.bit == Node::MultipleOf && !(value.toDouble() > 0)) {
            return QStringLiteral("%1: must be a number greater than 0").arg(here());
        }
        node().numberKeywords |= bound.bit;
        node().*(bound.slot) = number;
        return QString();
    }
    struct Count
    {
        const char* name;
        qint64 Node::* slot;
    };
    static const Count counts[] = {
        {"minLength", &Node::minLength},         {"maxLength", &Node::maxLength},
        {"minItems", &Node::minItems},           {"maxItems", &Node::maxItems},
        {"minProperties", &Node::minProperties}, {"maxProperties", &Node::maxProperties},
        {"minContains", &Node::minContains},     {"maxContains", &Node::maxContains},
    };
    for (const Count& count : counts) {
        if (!is(count.name)) {
            continue;
        }
        if (!isCount(value)) {
            return QStringLiteral("%1: must be an integer from 0 to 9007199254740991").arg(here());
        }
        node().*(count.slot) = numberOf(value).integer;
        return QString();
    }
    if (is("uniqueItems")) {
        if (!value.isBool()) {
            return QStringLiteral("%1: must be a boolean").arg(here());
        }
        node().uniqueItems = value.toBool();
        return QString();
    }
    if (is("required")) {
        QStringList names;
        const QString error = checkNames(value, names);
        node().required = names;
        return error;
    }
    if (is("dependentRequired")) {
        if (!value.isObject()) {
            return QStringLiteral("%1: must be an object of arrays of distinct property names").arg(here());
        }
        const QJsonObject dependencies = value.toObject();
        for (auto it = dependencies.begin(); it != dependencies.end(); ++it) {
            m_path.append(it.key());
            QString error;
            QStringList names;
            if (!it.key().isValidUtf16()) {
                error = QStringLiteral("%1: a property name with an unpaired UTF-16 surrogate is not supported")
                            .arg(here());
            } else {
                error = checkNames(it.value(), names);
            }
            m_path.removeLast();
            if (!error.isEmpty()) {
                return error;
            }
            node().dependentRequiredIndex.insert(it.key(), int(node().dependentRequired.size()));
            node().dependentRequired.emplace_back(it.key(), names);
        }
        return QString();
    }

    // Applicators.
    if (is("properties")) {
        std::vector<std::pair<QString, int>> children;
        if (!childObject(index, key, value, QStringLiteral("a property name"), children, depth)) {
            return m_error;
        }
        for (const auto& [name, child] : children) {
            node().properties.insert(name, child);
        }
        return QString();
    }
    if (is("dependentSchemas")) {
        std::vector<std::pair<QString, int>> children;
        if (!childObject(index, key, value, QStringLiteral("a property name"), children, depth)) {
            return m_error;
        }
        for (const auto& [name, child] : children) {
            node().dependentSchemaIndex.insert(name, child);
        }
        node().dependentSchemas = std::move(children);
        return QString();
    }
    if (is("items") && value.isArray()) {
        return QStringLiteral(
                   "%1: must be a schema. A list of schemas for the first items is \"prefixItems\" in JSON "
                   "Schema 2020-12")
            .arg(here());
    }
    struct Lists
    {
        const char* name;
        std::vector<int> Node::* slot;
    };
    static const Lists lists[] = {
        {"prefixItems", &Node::prefixItems},
        {"allOf", &Node::allOf},
        {"anyOf", &Node::anyOf},
        {"oneOf", &Node::oneOf},
    };
    for (const Lists& list : lists) {
        if (!is(list.name)) {
            continue;
        }
        std::vector<int> branches;
        if (!childArray(index, key, value, branches, depth)) {
            return m_error;
        }
        node().*(list.slot) = std::move(branches);
        return QString();
    }
    struct Single
    {
        const char* name;
        int Node::* slot;
    };
    static const Single singles[] = {
        {"items", &Node::items},
        {"contains", &Node::contains},
        {"additionalProperties", &Node::additionalProperties},
        {"propertyNames", &Node::propertyNames},
        {"not", &Node::notSchema},
        {"if", &Node::ifSchema},
        {"then", &Node::thenSchema},
        {"else", &Node::elseSchema},
        {"unevaluatedItems", &Node::unevaluatedItems},
        {"unevaluatedProperties", &Node::unevaluatedProperties},
    };
    for (const Single& single : singles) {
        if (!is(single.name)) {
            continue;
        }
        const int built = child(index, key, value, nullptr, depth);
        if (built < 0) {
            return m_error;
        }
        node().*(single.slot) = built;
        return QString();
    }
    return QStringLiteral("%1: \"%2\" is not supported in a tool schema").arg(here(), key);
}

QString SchemaBuilder::finish()
{
    for (std::size_t i = 0; i < m_nodes.size(); ++i) {
        if (m_nodes[i].hasRef) {
            const QString error = resolveReference(int(i));
            if (!error.isEmpty()) {
                return error;
            }
        }
    }
    return findCycle();
}

// The node at a JSON pointer, read through the keywords that hold schemas,
// or -1.
int SchemaBuilder::resolvePointer(const QStringList& tokens) const
{
    int index = 0;
    qsizetype i = 0;
    while (i < tokens.size()) {
        const Node& node = m_nodes[std::size_t(index)];
        if (node.isBoolean) {
            return -1;
        }
        const QString& token = tokens.at(i);
        const bool last = i + 1 == tokens.size();
        int next = -1;
        if (token == QLatin1StringView("properties") && !last) {
            next = node.properties.value(tokens.at(i + 1));
            i += 2;
        } else if ((token == QLatin1StringView("$defs") || token == QLatin1StringView("dependentSchemas")) && !last) {
            const QHash<QString, int>& children =
                token == QLatin1StringView("$defs") ? node.definitions : node.dependentSchemaIndex;
            next = children.value(tokens.at(i + 1), -1);
            i += 2;
        } else if ((token == QLatin1StringView("prefixItems") || token == QLatin1StringView("allOf")
                    || token == QLatin1StringView("anyOf") || token == QLatin1StringView("oneOf"))
                   && !last) {
            const std::vector<int>& list = token == QLatin1StringView("prefixItems") ? node.prefixItems
                : token == QLatin1StringView("allOf")                                ? node.allOf
                : token == QLatin1StringView("anyOf")                                ? node.anyOf
                                                                                     : node.oneOf;
            const int position = arrayIndex(tokens.at(i + 1), list.size());
            next = position < 0 ? -1 : list[std::size_t(position)];
            i += 2;
        } else {
            static const QHash<QString, int Node::*> singles{
                {QStringLiteral("items"), &Node::items},
                {QStringLiteral("contains"), &Node::contains},
                {QStringLiteral("additionalProperties"), &Node::additionalProperties},
                {QStringLiteral("propertyNames"), &Node::propertyNames},
                {QStringLiteral("not"), &Node::notSchema},
                {QStringLiteral("if"), &Node::ifSchema},
                {QStringLiteral("then"), &Node::thenSchema},
                {QStringLiteral("else"), &Node::elseSchema},
                {QStringLiteral("unevaluatedItems"), &Node::unevaluatedItems},
                {QStringLiteral("unevaluatedProperties"), &Node::unevaluatedProperties},
                {QStringLiteral("contentSchema"), &Node::contentSchema},
            };
            const auto slot = singles.constFind(token);
            next = slot == singles.cend() ? -1 : node.*(*slot);
            i += 1;
        }
        if (next < 0) {
            return -1;
        }
        index = next;
    }
    return index;
}

// A $ref names a place in the same schema: "#", a JSON pointer after "#",
// or an $anchor. The server fetches no other schema.
QString SchemaBuilder::resolveReference(int index)
{
    const QString at = nodePointer(m_nodes, index) + QStringLiteral("/$ref");
    const QString reference = m_nodes[std::size_t(index)].refValue;
    if (!reference.startsWith(QLatin1Char('#'))) {
        return QStringLiteral(
                   "%1: \"$ref\" may refer only to a place in the same schema (\"#...\"), because the server "
                   "fetches no other schema")
            .arg(at);
    }
    const QString fragment = reference.mid(1);
    FragmentError fragmentError = FragmentError::None;
    const std::optional<QString> decoded = decodeFragment(fragment, fragmentError);
    if (!decoded) {
        if (fragmentError == FragmentError::NotUtf8) {
            return QStringLiteral("%1: the percent-encoded bytes of the fragment are not UTF-8").arg(at);
        }
        return QStringLiteral("%1: is not a valid URI fragment. Percent-encode the characters that a URI cannot hold")
            .arg(at);
    }
    int target = -1;
    if (fragment.isEmpty() || fragment.startsWith(QLatin1Char('/'))) {
        // A JSON pointer, percent-decoded (RFC 6901, section 6).
        const std::optional<QStringList> tokens = pointerTokens(*decoded);
        if (!tokens) {
            return QStringLiteral("%1: is not a valid JSON pointer, where \"~\" must be followed by \"0\" or \"1\"")
                .arg(at);
        }
        target = resolvePointer(*tokens);
    } else {
        // A plain name, compared as it is written: RFC 3986 does not decode
        // a fragment, and "%66oo" names no anchor.
        target = m_anchors.value(fragment, -1);
    }
    if (target < 0) {
        return QStringLiteral("%1: refers to no schema in the tool's schema").arg(at);
    }
    if (m_nodes[std::size_t(target)].inContentSchema) {
        return QStringLiteral(
                   "%1: refers to a schema inside \"contentSchema\", which is an annotation that the server never "
                   "applies")
            .arg(at);
    }
    m_nodes[std::size_t(index)].ref = target;
    return QString();
}

// The checker follows each $ref while it checks a value, so a $ref that
// leads back to its own schema through subschemas that apply to the same
// value would recurse without end. Such a loop holds a $ref, since a schema
// without one is a tree. A loop is refused wherever it is, in a subschema
// that no value reaches too. The walk keeps its own stack: a long chain of
// subschemas must not recurse on the machine's.
//
// Without a loop, $ref still lets a schema apply more subschemas to one
// value than its nesting shows: a chain of $ref longer than the check goes
// deep, or two $ref to the same schema at each of forty levels, 2^40 of
// them. Such a schema would stop the check of a value that takes its
// branches, so it is refused too: a chain of more than MaxCheckDepth
// subschemas applied to the same value, or more of them than the bound of
// the work lets the check apply. The count takes every branch of allOf,
// anyOf, oneOf and dependentSchemas, not and if, and the larger of then and
// else, since a value takes one of the two.
QString SchemaBuilder::findCycle()
{
    enum class Mark : quint8 {
        New,
        Open,
        Done,
    };
    struct Frame
    {
        int node;
        std::vector<int> next;
        std::size_t position = 0;
    };
    std::vector<Mark> marks(m_nodes.size(), Mark::New);
    std::vector<Frame> stack;
    // For each node done: the longest chain of subschemas that it applies
    // to its value, itself included, and how many it applies at the most,
    // up to the most that the work allows.
    constexpr qint64 MostApplied = MaxWork / UnitsPerApply;
    std::vector<int> chain(m_nodes.size(), 0);
    std::vector<qint64> applied(m_nodes.size(), 0);
    const auto loopAt = [&](int to) -> int {
        // The loop is the stack from `to` on, and a node on it goes on by
        // its $ref.
        std::size_t first = 0;
        while (stack[first].node != to) {
            ++first;
        }
        for (std::size_t k = first; k < stack.size(); ++k) {
            const int successor = k + 1 < stack.size() ? stack[k + 1].node : to;
            if (m_nodes[std::size_t(stack[k].node)].ref == successor) {
                return stack[k].node;
            }
        }
        return stack.back().node;
    };
    for (std::size_t root = 0; root < m_nodes.size(); ++root) {
        if (marks[root] != Mark::New) {
            continue;
        }
        marks[root] = Mark::Open;
        stack.push_back(Frame{int(root), inPlace(m_nodes[root])});
        while (!stack.empty()) {
            Frame& top = stack.back();
            if (top.position == top.next.size()) {
                const std::size_t done = std::size_t(top.node);
                const Node& node = m_nodes[done];
                int longest = 0;
                for (const int next : top.next) {
                    longest = std::max(longest, chain[std::size_t(next)]);
                }
                qint64 total = 1;
                const auto add = [&](qint64 count) {
                    total = std::min(MostApplied + 1, total + count);
                };
                const auto count = [&](int child) -> qint64 {
                    return child >= 0 ? applied[std::size_t(child)] : 0;
                };
                for (const auto* branches : {&node.allOf, &node.anyOf, &node.oneOf}) {
                    for (const int branch : *branches) {
                        add(count(branch));
                    }
                }
                for (const auto& dependent : node.dependentSchemas) {
                    add(count(dependent.second));
                }
                add(count(node.notSchema));
                add(count(node.ifSchema));
                add(std::max(count(node.thenSchema), count(node.elseSchema)));
                add(count(node.ref));
                // The subschemas of inPlace(), but for then and else.
                chain[done] = longest + 1;
                applied[done] = total;
                if (chain[done] > MaxCheckDepth) {
                    return QStringLiteral(
                               "%1: applies a chain of more than %2 subschemas to the same value, which the server "
                               "does not check")
                        .arg(nodePointer(m_nodes, top.node), QString::number(MaxCheckDepth));
                }
                if (total > MostApplied) {
                    return QStringLiteral(
                               "%1: applies more than %2 subschemas to the same value, counting each branch that a "
                               "value can take, which the server does not check")
                        .arg(nodePointer(m_nodes, top.node), QString::number(MostApplied));
                }
                marks[done] = Mark::Done;
                stack.pop_back();
                continue;
            }
            const int next = top.next[top.position++];
            if (marks[std::size_t(next)] == Mark::Open) {
                const QString pointer = nodePointer(m_nodes, loopAt(next));
                return QStringLiteral(
                           "%1/$ref: \"$ref\" leads back to the same schema without descending into the value, so "
                           "checking it would never end")
                    .arg(pointer);
            }
            if (marks[std::size_t(next)] == Mark::New) {
                marks[std::size_t(next)] = Mark::Open;
                stack.push_back(Frame{next, inPlace(m_nodes[std::size_t(next)])});
            }
        }
    }
    return QString();
}

} // namespace

QString compileSchema(const QJsonObject& schema, bool objectRoot, CompiledSchema& compiled)
{
    if (objectRoot) {
        // The members read one by one: QJsonObject::value() misses a name in
        // an object that the application built from a parsed one and then
        // added to (see the Members of the check).
        bool isObject = false;
        for (auto it = schema.begin(); it != schema.end(); ++it) {
            if (QAnyStringView::equal(it.keyView(), QLatin1StringView("type"))) {
                isObject = it.value() == QJsonValue(QStringLiteral("object"));
            }
        }
        if (!isObject) {
            return QStringLiteral("/type: the root of a tool schema must have \"type\": \"object\"");
        }
    }
    std::vector<Node> nodes;
    SchemaBuilder builder(nodes);
    if (builder.build(schema, -1, QString(), nullptr, 0) < 0) {
        return builder.error();
    }
    const QString error = builder.finish();
    if (!error.isEmpty()) {
        return error;
    }
    compiled.nodes = std::move(nodes);
    return QString();
}

} // namespace PhosphorMcp::Schema
