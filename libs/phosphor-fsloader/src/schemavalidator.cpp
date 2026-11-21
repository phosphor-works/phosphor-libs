// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorFsLoader/SchemaValidator.h>

#include <QtCore/QCborValue>
#include <QtCore/QFile>
#include <QtCore/QHash>
#include <QtCore/QIODevice>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QLocale>
#include <QtCore/QLoggingCategory>
#include <QtCore/QString>

// jsoncons is pinned (or found on the system) and confined to this
// translation unit, the one built with exceptions: it throws when a schema
// does not compile.
#include <jsoncons/json.hpp>
#include <jsoncons_ext/jsonschema/jsonschema.hpp>

#include <algorithm>
#include <cmath>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace PhosphorFsLoader {

namespace {

// Sorted members, so that an object of many members is built and searched in
// O(n log n) and two objects compare equal whatever order their members were
// written in (const, enum).
using Json = jsoncons::json;
namespace jsonschema = jsoncons::jsonschema;
using CompiledSchema = jsonschema::json_schema<Json>;

/// The most errors validate() lists; the rest are counted in one more entry.
constexpr qsizetype kMaxErrors = 100;
/// How deep rechecks nest. A recheck validates a failed member's value again,
/// and a member that fails inside it is rechecked in turn, which validates a
/// part of that value once more: without a bound, a chain of members nested
/// N deep would be validated about N times. Rechecks stop at this many
/// additionalProperties subschemas, one inside another, and the meta-schema
/// check of a schema at as many maps.
constexpr int kMaxRecheckDepth = 4;
/// How many compiled rechecks a validator keeps for later validate() calls.
/// Each is a copy of the whole schema, compiled, so they take at most this
/// many times the memory of the validator's own compiled schema.
constexpr std::size_t kMaxKeptRechecks = 16;
/// The most UTF-16 code units of a path or a message an error keeps, escapes
/// included, before the ellipsis that says it was cut.
constexpr qsizetype kMaxTextLength = 500;

QString fromUtf8(std::string_view text)
{
    return QString::fromUtf8(text.data(), qsizetype(text.size()));
}

/// Whether an error writes the UTF-16 code unit @p c as \uXXXX: a C0 or C1
/// control or DEL, which could end a log line or start a terminal escape
/// sequence, the line and paragraph separators U+2028 and U+2029, a
/// bidirectional embedding, override or isolate (U+202A to U+202E, U+2066 to
/// U+2069), which could reorder the text around it, and a surrogate, which
/// shown() asks about only when it is unpaired.
bool isEscaped(char16_t c)
{
    return c < 0x20 || (c >= 0x7f && c <= 0x9f) || c == 0x2028 || c == 0x2029 || (c >= 0x202a && c <= 0x202e)
        || (c >= 0x2066 && c <= 0x2069) || QChar::isSurrogate(c);
}

/// @p text as an error shows it, with each code unit isEscaped() names written
/// as \uXXXX (lower-case hex). A backslash is kept as it is. Text longer than
/// @p limit code units is cut before the character or escape that would cross
/// the limit, so never between the two halves of a surrogate pair nor inside
/// an escape, and an ellipsis is appended: @p limit + 1 code units at most. A
/// negative @p limit cuts nothing.
QString shown(QStringView text, qsizetype limit = -1)
{
    QString result;
    result.reserve(limit < 0 ? text.size() : std::min(text.size(), limit + 1));
    for (qsizetype i = 0; i < text.size(); ++i) {
        const char16_t c = text.at(i).unicode();
        const bool pair =
            QChar::isHighSurrogate(c) && i + 1 < text.size() && QChar::isLowSurrogate(text.at(i + 1).unicode());
        const bool escaped = !pair && isEscaped(c);
        const qsizetype width = pair ? 2 : escaped ? 6 : 1;
        if (limit >= 0 && result.size() + width > limit) {
            result.append(QChar(0x2026));
            break;
        }
        if (escaped) {
            result.append(QStringLiteral("\\u%1").arg(uint(c), 4, 16, QChar(u'0')));
        } else {
            result.append(text.at(i));
            if (pair) {
                result.append(text.at(++i));
            }
        }
    }
    return result;
}

/// One JSON Pointer reference token, escaped as jsoncons escapes them ("~" to
/// "~0", "/" to "~1").
QString pointerToken(const QString& name)
{
    QString token;
    token.reserve(name.size());
    for (const QChar c : name) {
        if (c == u'~') {
            token.append(QLatin1StringView("~0"));
        } else if (c == u'/') {
            token.append(QLatin1StringView("~1"));
        } else {
            token.append(c);
        }
    }
    return token;
}

/// The errors of one schema check or one validation: the first kMaxErrors
/// kept, the rest only counted.
class ErrorList
{
public:
    /// False once kMaxErrors are kept: the caller then only calls countOne(),
    /// and does not build the text of an error nobody reads.
    bool wantsMore() const
    {
        return m_errors.size() < kMaxErrors;
    }

    void add(const QString& path, const QString& message)
    {
        ++m_total;
        if (wantsMore()) {
            m_errors.append(SchemaValidator::Error{shown(path, kMaxTextLength), shown(message, kMaxTextLength)});
        }
    }

    void countOne()
    {
        ++m_total;
    }

    bool isEmpty() const
    {
        return m_total == 0;
    }

    /// Every error added or counted so far.
    qsizetype total() const
    {
        return m_total;
    }

    /// The kept errors, then, when some were only counted, one entry with an
    /// empty path that says how many.
    QList<SchemaValidator::Error> take()
    {
        QList<SchemaValidator::Error> errors = std::move(m_errors);
        if (m_total > errors.size()) {
            errors.append(SchemaValidator::Error{
                QString(), QStringLiteral("and %1 more errors not listed").arg(m_total - errors.size())});
        }
        m_errors.clear();
        m_total = 0;
        return errors;
    }

    /// The first error, for the one line a schema that failed reports.
    QString summary() const
    {
        if (m_errors.isEmpty()) {
            return QString();
        }
        const SchemaValidator::Error& first = m_errors.first();
        QString text =
            (first.path.isEmpty() ? QStringLiteral("(root)") : first.path) + QStringLiteral(": ") + first.message;
        if (m_total > 1) {
            text += QStringLiteral(" (and %1 more)").arg(m_total - 1);
        }
        return text;
    }

private:
    QList<SchemaValidator::Error> m_errors;
    qsizetype m_total = 0;
};

/// Where a walk over a QJsonValue is, as a JSON Pointer built only when an
/// error needs it.
class PointerPath
{
public:
    void push(const QString& name)
    {
        m_tokens.push_back(Token{name, -1});
    }
    void push(qsizetype index)
    {
        m_tokens.push_back(Token{QString(), index});
    }
    void pop()
    {
        m_tokens.pop_back();
    }

    /// How many tokens deep the walk is: 1 at a keyword of the root.
    std::size_t depth() const
    {
        return m_tokens.size();
    }

    QString toString() const
    {
        QString path;
        for (const Token& token : m_tokens) {
            path.append(QChar(u'/'));
            path.append(token.index < 0 ? pointerToken(token.name) : QString::number(token.index));
        }
        return path;
    }

private:
    struct Token
    {
        QString name;
        qsizetype index;
    };
    std::vector<Token> m_tokens;
};

// Converts a QJsonValue to the jsoncons value the validator reads, and refuses
// what jsoncons would read differently from the text: a string or member name
// with U+0000 (jsoncons matches `pattern` and looks names up through a C
// string, which ends there), one with an unpaired surrogate (UTF-8 has no
// encoding for it, so names would collapse into one), and a number outside
// int64, below -2^63 or from 2^63 on (jsoncons' integer check converts a
// double to int64, which is undefined behaviour there). Each refusal is an
// error at the node's path, wherever the node is: jsoncons may run its
// integer check at any node a subschema reaches.
//
// A number that Qt holds as an integer stays one. Qt holds every integral
// number within int64 as one, 3.0, 3e0 and -2^63 included, which is what
// 2020-12 means by an integer. Any other number is a double: a fraction, whose
// magnitude is below 2^52 and so converts, or a number outside int64, refused
// here.
class Converter
{
public:
    explicit Converter(ErrorList& errors)
        : m_errors(errors)
    {
    }

    Json convert(const QJsonValue& value)
    {
        switch (value.type()) {
        case QJsonValue::Bool:
            return Json(value.toBool());
        case QJsonValue::Double: {
            const QCborValue number = QCborValue::fromJsonValue(value);
            if (number.isInteger()) {
                return Json(static_cast<int64_t>(number.toInteger()));
            }
            const double real = value.toDouble();
            if (!(std::fabs(real) < 0x1p63)) {
                refuse(QStringLiteral("number %1 is outside the int64 range (-2^63 to 2^63 - 1), which is refused")
                           .arg(real, 0, 'e', QLocale::FloatingPointShortest));
                return Json::null();
            }
            return Json(real);
        }
        case QJsonValue::String:
            return Json(text(value.toString(), false));
        case QJsonValue::Array: {
            const QJsonArray array = value.toArray();
            Json result(jsoncons::json_array_arg);
            result.reserve(std::size_t(array.size()));
            for (qsizetype i = 0; i < array.size(); ++i) {
                m_path.push(i);
                result.push_back(convert(array.at(i)));
                m_path.pop();
            }
            return result;
        }
        case QJsonValue::Object: {
            const QJsonObject object = value.toObject();
            std::vector<std::pair<std::string, Json>> members;
            members.reserve(std::size_t(object.size()));
            for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
                const QString name = it.key();
                m_path.push(name);
                std::string key = text(name, true);
                members.emplace_back(std::move(key), convert(it.value()));
                m_path.pop();
            }
            // Qt orders names by UTF-16 code unit and jsoncons by UTF-8 byte,
            // which differ once a name holds a character past U+FFFF. Sorted
            // the jsoncons way, each insertion is an append.
            std::sort(members.begin(), members.end(), [](const auto& a, const auto& b) {
                return a.first < b.first;
            });
            Json result(jsoncons::json_object_arg);
            result.reserve(members.size());
            for (auto& member : members) {
                result.insert_or_assign(member.first, std::move(member.second));
            }
            return result;
        }
        case QJsonValue::Null:
        case QJsonValue::Undefined:
            break;
        }
        return Json::null();
    }

private:
    std::string text(const QString& text, bool isName)
    {
        if (text.contains(QChar(u'\0'))) {
            refuse(isName ? QStringLiteral("member name contains U+0000, which is refused")
                          : QStringLiteral("string contains U+0000, which is refused"));
        } else if (!text.isValidUtf16()) {
            refuse(isName ? QStringLiteral("member name contains an unpaired UTF-16 surrogate, which is refused")
                          : QStringLiteral("string contains an unpaired UTF-16 surrogate, which is refused"));
        }
        return text.toStdString();
    }

    void refuse(const QString& message)
    {
        if (m_errors.wantsMore()) {
            m_errors.add(m_path.toString(), message);
        } else {
            m_errors.countOne();
        }
    }

    ErrorList& m_errors;
    PointerPath m_path;
};

// The $schema values read as 2020-12: the meta-schema's URI, with or without
// an empty fragment, as Python's jsonschema reads it too.
bool is202012(const QJsonValue& value)
{
    return value == QLatin1StringView("https://json-schema.org/draft/2020-12/schema")
        || value == QLatin1StringView("https://json-schema.org/draft/2020-12/schema#");
}

// The formats jsoncons checks and that this validator lets a schema use.
// jsoncons ignores every other format, which would leave a schema that names
// one checking nothing, and its "hostname" check is wrong (it refuses the digit
// 9 and reads out of bounds on an empty string), so those are refused.
bool isAssertedFormat(const QString& format)
{
    static const QStringList formats{
        QStringLiteral("date-time"),     QStringLiteral("date"),         QStringLiteral("time"),
        QStringLiteral("email"),         QStringLiteral("ipv4"),         QStringLiteral("ipv6"),
        QStringLiteral("regex"),         QStringLiteral("json-pointer"), QStringLiteral("uri"),
        QStringLiteral("uri-reference"),
    };
    return formats.contains(format);
}

// Keywords of other drafts that 2020-12 does not know, and so would ignore
// without a word: a schema written with them would check less than its
// author meant. Refused, with what 2020-12 says instead.
QString otherDraftKeyword(const QString& keyword)
{
    if (keyword == QLatin1StringView("definitions")) {
        return QStringLiteral("is a Draft 7 keyword, which 2020-12 names $defs");
    }
    if (keyword == QLatin1StringView("dependencies")) {
        return QStringLiteral("is a Draft 7 keyword, which 2020-12 splits into dependentRequired and dependentSchemas");
    }
    if (keyword == QLatin1StringView("additionalItems")) {
        return QStringLiteral("is a Draft 7 and 2019-09 keyword, which 2020-12 replaces with items beside prefixItems");
    }
    if (keyword == QLatin1StringView("$recursiveRef") || keyword == QLatin1StringView("$recursiveAnchor")) {
        return QStringLiteral(
            "is a Draft 2019-09 keyword, which 2020-12 replaces with $dynamicRef and "
            "$dynamicAnchor (and this validator supports neither)");
    }
    return QString();
}

// Keywords of 2020-12 itself that this validator refuses, with why. A
// $dynamicRef resolves by the path that validation took, which the check
// for references that never end (checkReferences) cannot follow, and an $id
// below the root starts another resource, against which that check would
// resolve the references inside it wrongly.
QString unsupportedKeyword(const QString& keyword, const PointerPath& path)
{
    if (keyword == QLatin1StringView("$dynamicRef") || keyword == QLatin1StringView("$dynamicAnchor")) {
        return QStringLiteral("is not supported: this validator resolves only $ref and $anchor");
    }
    if (keyword == QLatin1StringView("$id") && path.depth() > 1) {
        return QStringLiteral("below the root is not supported: this validator resolves $ref within one resource");
    }
    return QString();
}

bool isSubschemaKeyword(const QString& keyword)
{
    static const QStringList keywords{
        QStringLiteral("additionalProperties"),
        QStringLiteral("contains"),
        QStringLiteral("contentSchema"),
        QStringLiteral("else"),
        QStringLiteral("if"),
        QStringLiteral("items"),
        QStringLiteral("not"),
        QStringLiteral("propertyNames"),
        QStringLiteral("then"),
        QStringLiteral("unevaluatedItems"),
        QStringLiteral("unevaluatedProperties"),
    };
    return keywords.contains(keyword);
}

bool isSubschemaMapKeyword(const QString& keyword)
{
    static const QStringList keywords{
        QStringLiteral("$defs"),
        QStringLiteral("dependentSchemas"),
        QStringLiteral("patternProperties"),
        QStringLiteral("properties"),
    };
    return keywords.contains(keyword);
}

bool isSubschemaArrayKeyword(const QString& keyword)
{
    static const QStringList keywords{
        QStringLiteral("allOf"),
        QStringLiteral("anyOf"),
        QStringLiteral("oneOf"),
        QStringLiteral("prefixItems"),
    };
    return keywords.contains(keyword);
}

// The keywords above whose subschemas apply to the same value as the schema
// that holds them, and not to a member or an item of it.
bool isInPlaceKeyword(const QString& keyword)
{
    static const QStringList keywords{
        QStringLiteral("allOf"), QStringLiteral("anyOf"), QStringLiteral("dependentSchemas"),
        QStringLiteral("else"),  QStringLiteral("if"),    QStringLiteral("not"),
        QStringLiteral("oneOf"), QStringLiteral("then"),
    };
    return keywords.contains(keyword);
}

// What the meta-schema allows but this validator refuses, found by walking
// every subschema: $schema naming another dialect, other drafts' keywords,
// the array form of items, a format jsoncons does not check, $dynamicRef,
// $dynamicAnchor, and $id below the root. Values the meta-schema refuses (a
// subschema that is not one) are left to it.
void checkDialect(const QJsonValue& schema, PointerPath& path, ErrorList& errors)
{
    if (!schema.isObject()) {
        return;
    }
    const QJsonObject object = schema.toObject();
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        const QString keyword = it.key();
        const QJsonValue value = it.value();
        path.push(keyword);
        if (keyword == QLatin1StringView("$schema")) {
            if (!is202012(value)) {
                errors.add(path.toString(),
                           QStringLiteral("names a dialect other than 2020-12 "
                                          "(https://json-schema.org/draft/2020-12/schema), the only one supported"));
            }
        } else if (const QString why = otherDraftKeyword(keyword); !why.isEmpty()) {
            errors.add(path.toString(), keyword + QLatin1Char(' ') + why);
        } else if (const QString unsupported = unsupportedKeyword(keyword, path); !unsupported.isEmpty()) {
            errors.add(path.toString(), keyword + QLatin1Char(' ') + unsupported);
        } else if (keyword == QLatin1StringView("format")) {
            if (value.isString() && !isAssertedFormat(value.toString())) {
                errors.add(path.toString(),
                           QStringLiteral("format \"%1\" is not one this validator checks").arg(value.toString()));
            }
        } else if (keyword == QLatin1StringView("items") && value.isArray()) {
            errors.add(path.toString(),
                       QStringLiteral("an array under items is the Draft 7 and 2019-09 form, which 2020-12 writes as "
                                      "prefixItems"));
        } else if (isSubschemaKeyword(keyword)) {
            checkDialect(value, path, errors);
        } else if (isSubschemaMapKeyword(keyword) && value.isObject()) {
            const QJsonObject members = value.toObject();
            for (auto member = members.constBegin(); member != members.constEnd(); ++member) {
                path.push(member.key());
                checkDialect(member.value(), path, errors);
                path.pop();
            }
        } else if (isSubschemaArrayKeyword(keyword) && value.isArray()) {
            const QJsonArray items = value.toArray();
            for (qsizetype i = 0; i < items.size(); ++i) {
                path.push(i);
                checkDialect(items.at(i), path, errors);
                path.pop();
            }
        }
        path.pop();
    }
}

// jsoncons follows a $ref each time it validates through it, with no guard: a
// $ref that leads back to its own schema through subschemas that apply to the
// same value ({"$ref": "#"}, or two $defs that name each other) recursed
// until the stack overflowed, at every validate(). Such a loop is refused
// when the schema is constructed. A $ref that comes back only through a
// member or an item (properties, items, ...) descends into the document,
// which is finite, and is allowed.
//
// The references followed are those jsoncons resolves within the schema: "#",
// "#/<JSON pointer>" and "#<$anchor>". Anything else is refused, so that no
// reference jsoncons follows is missed here: one to another document (which
// it would not load anyway), one that is percent-encoded (it compares
// fragments as written, which a decoded comparison here could misread), and
// one that names no subschema. $dynamicRef and $id below the root are refused
// before this runs, by checkDialect, and so are U+0000 and unpaired surrogates,
// by the Converter.
class ReferenceCheck
{
public:
    // Adds to `errors` each $ref that cannot be followed, each $anchor
    // defined twice, and else the first $ref found to lead back to its own
    // schema without descending.
    void run(const QJsonObject& root, ErrorList& errors)
    {
        add(root, QString(), errors);
        if (!errors.isEmpty()) {
            return;
        }
        for (Node& node : m_nodes) {
            resolve(node, errors);
        }
        if (errors.isEmpty()) {
            findLoop(errors);
        }
    }

private:
    struct Node
    {
        QJsonObject object; // empty for a boolean schema
        QString pointer; // from the root, as jsoncons writes it
        std::vector<int> inPlace; // the subschemas that apply to the same value
        int reference = -1; // the subschema its $ref names
    };

    // Adds the schema at `pointer` and every subschema below it, as
    // checkDialect walks them. Returns its index, or -1 for a value that is
    // not a schema (which the meta-schema has refused already).
    int add(const QJsonValue& schema, const QString& pointer, ErrorList& errors)
    {
        if (!schema.isObject() && !schema.isBool()) {
            return -1;
        }
        const int index = int(m_nodes.size());
        m_nodes.push_back(Node{schema.toObject(), pointer, {}, -1});
        m_byPointer.insert(pointer, index);
        const QJsonObject object = schema.toObject();
        if (const QJsonValue anchor = object.value(QLatin1StringView("$anchor")); anchor.isString()) {
            if (const auto first = m_anchors.constFind(anchor.toString()); first != m_anchors.constEnd()) {
                const QString& other = m_nodes[std::size_t(*first)].pointer;
                errors.add(pointer + QStringLiteral("/$anchor"),
                           QStringLiteral("$anchor \"%1\" is defined at %2 too")
                               .arg(anchor.toString(), other.isEmpty() ? QStringLiteral("the root") : other));
            } else {
                m_anchors.insert(anchor.toString(), index);
            }
        }
        for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
            const QString keyword = it.key();
            const QString at = pointer + QChar(u'/') + pointerToken(keyword);
            const bool inPlace = isInPlaceKeyword(keyword);
            const auto child = [&](const QJsonValue& value, const QString& childPointer) {
                const int added = add(value, childPointer, errors);
                if (inPlace && added >= 0) {
                    m_nodes[std::size_t(index)].inPlace.push_back(added);
                }
            };
            if (isSubschemaKeyword(keyword)) {
                child(it.value(), at);
            } else if (isSubschemaMapKeyword(keyword) && it.value().isObject()) {
                const QJsonObject members = it.value().toObject();
                for (auto member = members.constBegin(); member != members.constEnd(); ++member) {
                    child(member.value(), at + QChar(u'/') + pointerToken(member.key()));
                }
            } else if (isSubschemaArrayKeyword(keyword) && it.value().isArray()) {
                const QJsonArray items = it.value().toArray();
                for (qsizetype i = 0; i < items.size(); ++i) {
                    child(items.at(i), at + QChar(u'/') + QString::number(i));
                }
            }
        }
        return index;
    }

    void resolve(Node& node, ErrorList& errors) const
    {
        const QJsonValue value = node.object.value(QLatin1StringView("$ref"));
        if (!value.isString()) {
            return;
        }
        const QString reference = value.toString();
        const QString at = node.pointer + QStringLiteral("/$ref");
        if (!reference.startsWith(QChar(u'#'))) {
            errors.add(at,
                       QStringLiteral("$ref \"%1\" names another document, and this validator resolves only "
                                      "\"#\", \"#/<JSON pointer>\" and \"#<$anchor>\" within the schema")
                           .arg(reference));
            return;
        }
        if (reference.contains(QChar(u'%'))) {
            errors.add(
                at,
                QStringLiteral("$ref \"%1\" is percent-encoded, which this validator does not resolve").arg(reference));
            return;
        }
        const QString fragment = reference.mid(1);
        const bool isPointer = fragment.isEmpty() || fragment.startsWith(QChar(u'/'));
        const QHash<QString, int>& names = isPointer ? m_byPointer : m_anchors;
        const auto found = names.constFind(fragment);
        if (found == names.constEnd()) {
            errors.add(at, QStringLiteral("$ref \"%1\" names no subschema of this schema").arg(reference));
            return;
        }
        node.reference = *found;
        node.inPlace.push_back(*found);
    }

    // A depth-first search over the in-place edges, with a stack of its own:
    // a chain of $defs that name each other must not recurse on the
    // machine's. A loop holds a $ref, since without one the edges form a
    // tree, and the $ref reported is the first on the loop.
    void findLoop(ErrorList& errors) const
    {
        enum class Mark : quint8 {
            New,
            Open,
            Done,
        };
        struct Frame
        {
            int node;
            std::size_t next;
        };
        std::vector<Mark> marks(m_nodes.size(), Mark::New);
        std::vector<Frame> stack;
        for (std::size_t start = 0; start < m_nodes.size(); ++start) {
            if (marks[start] != Mark::New) {
                continue;
            }
            marks[start] = Mark::Open;
            stack.push_back(Frame{int(start), 0});
            while (!stack.empty()) {
                Frame& top = stack.back();
                const Node& node = m_nodes[std::size_t(top.node)];
                if (top.next == node.inPlace.size()) {
                    marks[std::size_t(top.node)] = Mark::Done;
                    stack.pop_back();
                    continue;
                }
                const int next = node.inPlace[top.next++];
                if (marks[std::size_t(next)] == Mark::Open) {
                    errors.add(loopReference(stack, next) + QStringLiteral("/$ref"),
                               QStringLiteral("$ref leads back to the same schema without descending into the "
                                              "document, so validating would never end"));
                    return;
                }
                if (marks[std::size_t(next)] == Mark::New) {
                    marks[std::size_t(next)] = Mark::Open;
                    stack.push_back(Frame{next, 0});
                }
            }
        }
    }

    // The pointer of the schema on the loop that closes at `to` whose $ref
    // is one of its steps.
    template<typename Stack>
    QString loopReference(const Stack& stack, int to) const
    {
        std::size_t first = 0;
        while (stack[first].node != to) {
            ++first;
        }
        for (std::size_t k = first; k < stack.size(); ++k) {
            const int successor = k + 1 < stack.size() ? stack[k + 1].node : to;
            const Node& node = m_nodes[std::size_t(stack[k].node)];
            if (node.reference == successor) {
                return node.pointer;
            }
        }
        return m_nodes[std::size_t(stack.back().node)].pointer;
    }

    std::vector<Node> m_nodes;
    QHash<QString, int> m_byPointer;
    QHash<QString, int> m_anchors;
};

jsonschema::evaluation_options evaluationOptions()
{
    // 2020-12 unless the schema names another draft in $schema (which
    // checkDialect refuses). `format` is asserted, not only annotated: a date
    // that is not a date fails.
    return jsonschema::evaluation_options{}
        .default_version(jsonschema::schema_version::draft202012())
        .require_format_validation(true);
}

// The 2020-12 meta-schema jsoncons carries, compiled once per process, the
// first time a schema is checked against it.
const jsonschema::json_schema<Json>& metaSchema()
{
    static const jsonschema::json_schema<Json> schema = jsonschema::make_json_schema(
        jsonschema::draft202012::schema_draft202012<Json>::get_schema(), evaluationOptions());
    return schema;
}

// Which jsoncons messages addMessage() adds: every one, or only those that
// carry no reasons of their own (for the one line a refused schema reports,
// which should be the specific reason and not "found unmatched schemas").
enum class Messages {
    All,
    Leaves
};

// Validates `member` again, the value of a member that failed an
// additionalProperties subschema, against that subschema, and adds why it
// failed at `memberPath`, each prefixed with `prefix`. jsoncons keeps those
// reasons to itself and reports only that the member was invalid. `depth` is
// 1 for a member jsoncons reported, and one more for each recheck it is in.
using Recheck = std::function<void(const Json& member, const QString& memberPath,
                                   const jsonschema::validation_message& message, const QString& prefix, int depth)>;

// Where addMessage() puts what it reads, and what it needs to find a member.
struct Report
{
    ErrorList& errors;
    Messages which;
    // The value validated, and its path in the document ("" at its root).
    const Json& instance;
    QString instancePath;
    // Null: a failed additionalProperties member keeps only jsoncons' line.
    const Recheck* recheck;
    // How many rechecks this report is in.
    int depth;
};

// The member a failed additionalProperties subschema names: jsoncons
// reports it at the object, with the name in its message.
std::optional<std::string> invalidAdditionalProperty(const jsonschema::validation_message& message)
{
    constexpr std::string_view head = "Additional property '";
    constexpr std::string_view tail = "' found but was invalid.";
    const std::string& text = message.message();
    if (message.keyword() != "additionalProperties" || text.size() < head.size() + tail.size()
        || text.compare(0, head.size(), head) != 0 || text.compare(text.size() - tail.size(), tail.size(), tail) != 0) {
        return std::nullopt;
    }
    return text.substr(head.size(), text.size() - head.size() - tail.size());
}

// Whether jsoncons reached a message through a dependentSchemas keyword. Its
// evaluation path is a list of keywords, where properties, patternProperties,
// allOf, anyOf, oneOf and prefixItems are each followed by a member name or
// an index (and dependentSchemas by nothing): a property of that name is not
// the keyword.
bool isBelowDependentSchemas(const jsoncons::jsonpointer::json_pointer& evalPath)
{
    for (auto token = evalPath.begin(); token != evalPath.end(); ++token) {
        if (*token == "dependentSchemas") {
            return true;
        }
        if (*token == "properties" || *token == "patternProperties" || *token == "allOf" || *token == "anyOf"
            || *token == "oneOf" || *token == "prefixItems") {
            if (++token == evalPath.end()) {
                break;
            }
        }
    }
    return false;
}

// Finds the member `name` of the object at `message`'s instance location and
// rechecks it. Adds nothing when there is no such member.
//
// Nor below dependentSchemas. jsoncons validates a dependentSchemas subschema
// against the same object but appends the name of the member that triggered
// it to the instance location, so the location names that member's value,
// and a member found there is not the one that failed. Such a member keeps
// jsoncons' line only.
void recheckMember(const Report& report, const jsonschema::validation_message& message, const std::string& name,
                   const QString& memberPath, const QString& prefix)
{
    if (isBelowDependentSchemas(message.eval_path())) {
        return;
    }
    std::error_code ec;
    const Json& object = jsoncons::jsonpointer::get(report.instance, message.instance_location(), ec);
    if (ec || !object.is_object()) {
        return;
    }
    const auto member = object.find(name);
    if (member == object.object_range().end()) {
        return;
    }
    (*report.recheck)((*member).value(), memberPath, message, prefix, report.depth + 1);
}

// Adds one jsoncons message, then the reasons it carries: why each branch of
// an anyOf, oneOf or allOf failed, each at its own node and prefixed with the
// branch, and why the member a failed additionalProperties subschema names
// failed it, prefixed "additionalProperties: ".
void addMessage(const Report& report, const jsonschema::validation_message& message, const QString& prefix)
{
    ErrorList& errors = report.errors;
    if (report.which == Messages::Leaves && !message.details().empty()) {
        // Only its reasons, below.
    } else if (!errors.wantsMore()) {
        errors.countOne();
    } else {
        QString path = report.instancePath + fromUtf8(message.instance_location().to_string());
        const QString text = prefix + fromUtf8(message.message());
        // The error belongs at the member, not at the object jsoncons names.
        const std::optional<std::string> member = invalidAdditionalProperty(message);
        if (member) {
            path += QChar(u'/') + pointerToken(fromUtf8(*member));
        }
        if (!member || !report.recheck) {
            errors.add(path, text);
        } else if (report.which == Messages::All) {
            errors.add(path, text);
            // Past the last error kept, nothing is rechecked: the count then
            // holds only what jsoncons reported.
            if (errors.wantsMore()) {
                recheckMember(report, message, *member, path, prefix + QStringLiteral("additionalProperties: "));
            }
        } else {
            // A leaf only when nothing more specific is found.
            const qsizetype before = errors.total();
            recheckMember(report, message, *member, path, prefix);
            if (errors.total() == before) {
                errors.add(path, text);
            }
        }
    }
    if (message.details().empty()) {
        return;
    }
    const std::string parent = message.eval_path().to_string();
    for (const jsonschema::validation_message& detail : message.details()) {
        QString branch = fromUtf8(message.keyword());
        const std::string eval = detail.eval_path().to_string();
        if (eval.size() > parent.size() + 1 && eval.compare(0, parent.size(), parent) == 0
            && eval[parent.size()] == '/') {
            const std::size_t end = eval.find('/', parent.size() + 1);
            const std::string token = eval.substr(parent.size() + 1, end - parent.size() - 1);
            if (!token.empty() && std::all_of(token.begin(), token.end(), [](char c) {
                    return c >= '0' && c <= '9';
                })) {
                branch += QChar(u'/') + fromUtf8(token);
            }
        }
        // The branches of the meta-schema mean nothing to a schema's author.
        addMessage(report, detail, report.which == Messages::All ? prefix + branch + QStringLiteral(": ") : prefix);
    }
}

// The $id the copy of the schema that rechecks additionalProperties members
// takes, which names it for the $ref to one of its subschemas. Every $ref
// inside it is a fragment (ReferenceCheck), which resolves against it.
constexpr std::string_view kRecheckId = "urn:phosphor-fsloader:schema";

} // namespace

struct SchemaValidator::Private
{
    // Compiled once at construction and reused by every validate(). Empty when
    // the schema was refused or did not compile, and then every validate()
    // fails closed.
    std::optional<CompiledSchema> schema;

    // Why the schema was refused, reported by every validate().
    QString compileError;

    // {"$defs": {"schema": <the schema, its $id kRecheckId>}}, from which
    // validate() compiles a schema that is one failed additionalProperties
    // subschema, to say why a member failed it.
    Json recheckBase;

    // The rechecks compiled so far, by the fragment of the subschema's
    // location, kept for later validate() calls: kMaxKeptRechecks at most,
    // never removed. Null for one that did not compile (it should). A
    // validator is shared across threads and validate() is const, so the map
    // is behind a mutex, held only to look an entry up or add one, never
    // while compiling or validating.
    std::mutex recheckMutex;
    std::map<std::string, std::shared_ptr<const CompiledSchema>> keptRechecks;

    // The subschema at `fragment` as a schema of its own: a copy of the whole
    // schema whose root is a $ref to it. Null when it does not compile.
    std::shared_ptr<const CompiledSchema> compileRecheck(const std::string& fragment) const
    {
        try {
            Json wrapper = recheckBase;
            wrapper.insert_or_assign("$ref", std::string(kRecheckId) + '#' + fragment);
            return std::make_shared<const CompiledSchema>(
                jsonschema::make_json_schema(std::move(wrapper), evaluationOptions()));
        } catch (...) {
            // The member keeps jsoncons' line only.
            return nullptr;
        }
    }

    // The compiled recheck for `fragment`: a kept one, else one this
    // validate() compiled past the kept ones (in `local`), else compiled now
    // and kept, or put in `local` once kMaxKeptRechecks are kept.
    std::shared_ptr<const CompiledSchema>
    recheckFor(const std::string& fragment, std::map<std::string, std::shared_ptr<const CompiledSchema>>& local)
    {
        {
            const std::lock_guard lock(recheckMutex);
            if (const auto kept = keptRechecks.find(fragment); kept != keptRechecks.end()) {
                return kept->second;
            }
        }
        if (const auto found = local.find(fragment); found != local.end()) {
            return found->second;
        }
        std::shared_ptr<const CompiledSchema> compiled = compileRecheck(fragment);
        {
            const std::lock_guard lock(recheckMutex);
            if (keptRechecks.size() < kMaxKeptRechecks) {
                // Another thread may have kept one meanwhile, the same.
                return keptRechecks.try_emplace(fragment, std::move(compiled)).first->second;
            }
        }
        local.emplace(fragment, compiled);
        return compiled;
    }
};

SchemaValidator::SchemaValidator(const QByteArray& schemaJson)
    : d(std::make_unique<Private>())
{
    // Qt reads the text, as it reads the documents: comments are refused, and
    // of two members with one name the last is kept, as Python's json module
    // keeps it in CI.
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(schemaJson, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        d->compileError = QStringLiteral("schema is not valid JSON: %1 at offset %2")
                              .arg(parseError.errorString())
                              .arg(parseError.offset);
        return;
    }
    if (!document.isObject()) {
        d->compileError = QStringLiteral("schema root is not a JSON object");
        return;
    }
    const QJsonObject root = document.object();

    // jsoncons throws on a schema it cannot compile, which includes a
    // `pattern` that is not a valid ECMAScript regex. The schemas compiled
    // here are our own embedded resources, checked in CI, so this is a
    // build-time guarantee in practice. A bad one still degrades to
    // fail-closed validation rather than ending the process.
    try {
        ErrorList problems;
        PointerPath path;
        checkDialect(root, path, problems);
        if (!problems.isEmpty()) {
            d->compileError = QStringLiteral("schema refused at %1").arg(problems.summary());
            return;
        }
        Json schema = Converter(problems).convert(root);
        if (!problems.isEmpty()) {
            d->compileError = QStringLiteral("schema refused at %1").arg(problems.summary());
            return;
        }
        // jsoncons compiles what it understands and passes over the rest, so
        // a keyword with a value of the wrong kind ({"enum": "x"}, a negative
        // maxItems) would compile and then fail, or check nothing, at every
        // validate(). The meta-schema refuses it here.
        //
        // A member of $defs, properties, patternProperties or dependentSchemas
        // is checked against the whole meta-schema ({"$dynamicRef": "#meta"}),
        // through additionalProperties, which says only that the member was
        // invalid: the member is checked again, for the reason. The other
        // maps the meta-schema closes that way ($vocabulary,
        // dependentRequired) keep jsoncons' line.
        Recheck metaRecheck;
        metaRecheck = [&problems, &metaRecheck](const Json& member, const QString& memberPath,
                                                const jsonschema::validation_message& message, const QString& prefix,
                                                int depth) {
            const jsoncons::jsonpointer::json_pointer& map = message.instance_location();
            if (map.empty() || !isSubschemaMapKeyword(fromUtf8(*map.rbegin()))) {
                return;
            }
            // A recheck validates the member's subschemas again, so without a
            // bound a schema of maps nested N deep and refused at the bottom
            // was validated about N times: 108 s to refuse one 500 deep in a
            // Release build. A member below the bound keeps jsoncons' line.
            if (depth > kMaxRecheckDepth) {
                return;
            }
            const Report inner{problems, Messages::Leaves, member, memberPath, &metaRecheck, depth};
            metaSchema().validate(member, [&inner, &prefix](const jsonschema::validation_message& reason) {
                addMessage(inner, reason, prefix);
                return jsonschema::walk_state::advance;
            });
        };
        const Report report{problems, Messages::Leaves, schema, QString(), &metaRecheck, 0};
        metaSchema().validate(schema, [&report](const jsonschema::validation_message& message) {
            addMessage(report, message, QString());
            return jsonschema::walk_state::advance;
        });
        if (!problems.isEmpty()) {
            d->compileError =
                QStringLiteral("schema does not conform to the 2020-12 meta-schema at %1").arg(problems.summary());
            return;
        }
        ReferenceCheck().run(root, problems);
        if (!problems.isEmpty()) {
            d->compileError = QStringLiteral("schema refused at %1").arg(problems.summary());
            return;
        }
        Json recheck = schema;
        recheck.erase("$schema");
        recheck.insert_or_assign("$id", std::string(kRecheckId));
        Json defs(jsoncons::json_object_arg);
        defs.insert_or_assign("schema", std::move(recheck));
        Json recheckBase(jsoncons::json_object_arg);
        recheckBase.insert_or_assign("$defs", std::move(defs));
        d->schema.emplace(jsonschema::make_json_schema(std::move(schema), evaluationOptions()));
        d->recheckBase = std::move(recheckBase);
    } catch (const std::exception& e) {
        d->compileError =
            QStringLiteral("schema failed to compile: %1").arg(shown(QString::fromUtf8(e.what()), kMaxTextLength));
    } catch (...) {
        d->compileError = QStringLiteral("schema failed to compile: unknown error");
    }
}

SchemaValidator SchemaValidator::fromResource(const QString& resourcePath, const QLoggingCategory& category)
{
    QFile file(resourcePath);
    if (!file.open(QIODevice::ReadOnly)) {
        // A missing embedded resource is a build error, not user input. Fail
        // closed: an empty-bytes validator reports invalid and rejects every
        // document, rather than silently passing them through unvalidated.
        qCWarning(category) << "JSON schema resource missing:" << resourcePath
                            << "- all documents will be rejected (fail-closed)";
        return SchemaValidator(QByteArray());
    }
    return SchemaValidator(file.readAll());
}

SchemaValidator::~SchemaValidator() = default;
SchemaValidator::SchemaValidator(SchemaValidator&&) noexcept = default;
SchemaValidator& SchemaValidator::operator=(SchemaValidator&&) noexcept = default;

bool SchemaValidator::isValid() const
{
    return d && d->schema.has_value();
}

std::optional<QList<SchemaValidator::Error>> SchemaValidator::validate(const QJsonValue& document) const
{
    if (!d) {
        return QList<Error>{Error{QString(), QStringLiteral("schema validator was moved from")}};
    }
    if (!d->schema) {
        return QList<Error>{Error{QString(), d->compileError}};
    }

    // A guard, which no input known reaches: validation with a reporter does
    // not throw by design, and the std::regex matches it runs are not known to
    // throw either (libstdc++ recurses instead). But this translation unit is
    // built with exceptions and every caller is not, so nothing may leave it,
    // whatever jsoncons or the standard library do: fail closed instead, as
    // the constructor does.
    try {
        ErrorList errors;
        const Json instance = Converter(errors).convert(document);
        // Each failed additionalProperties subschema is compiled the first
        // time a member fails it (Private::recheckFor), at most kMaxErrors per
        // validate() since a recheck follows an error kept. Those compiled
        // when kMaxKeptRechecks are kept already live in `local`, for this
        // validate() only.
        std::map<std::string, std::shared_ptr<const CompiledSchema>> local;
        Recheck recheck;
        recheck = [this, &errors, &local, &recheck](const Json& member, const QString& memberPath,
                                                    const jsonschema::validation_message& message,
                                                    const QString& prefix, int depth) {
            if (depth > kMaxRecheckDepth) {
                return;
            }
            const jsoncons::string_view fragment = message.schema_location().encoded_fragment();
            const std::shared_ptr<const CompiledSchema> subschema =
                d->recheckFor(std::string(fragment.data(), fragment.size()), local);
            if (!subschema) {
                return;
            }
            const Report inner{errors, Messages::All, member, memberPath, &recheck, depth};
            subschema->validate(member, [&inner, &prefix](const jsonschema::validation_message& reason) {
                addMessage(inner, reason, prefix);
                return jsonschema::walk_state::advance;
            });
        };
        if (errors.isEmpty()) {
            const Report report{errors, Messages::All, instance, QString(), &recheck, 0};
            d->schema->validate(instance, [&report](const jsonschema::validation_message& message) {
                addMessage(report, message, QString());
                return jsonschema::walk_state::advance;
            });
        }
        if (errors.isEmpty()) {
            return std::nullopt;
        }
        return errors.take();
    } catch (const std::exception& e) {
        return QList<Error>{Error{
            QString(),
            QStringLiteral("schema validation error: %1").arg(shown(QString::fromUtf8(e.what()), kMaxTextLength))}};
    } catch (...) {
        return QList<Error>{Error{QString(), QStringLiteral("schema validation error: unknown")}};
    }
}

void logSchemaErrors(const QLoggingCategory& category, const QList<SchemaValidator::Error>& errors)
{
    for (qsizetype i = 0; i < errors.size(); ++i) {
        const SchemaValidator::Error& error = errors.at(i);
        // validate() has escaped both already. An error built elsewhere is
        // escaped here, so that no line it logs holds a control character.
        const QString message = shown(error.message);
        // The entry past the last kept error, which validate() adds when it
        // only counted the rest, is a count and has no node.
        if (i == kMaxErrors && error.path.isEmpty()) {
            qCWarning(category).noquote().nospace() << "  " << message;
            continue;
        }
        qCWarning(category).noquote().nospace()
            << "  " << (error.path.isEmpty() ? QStringLiteral("(root)") : shown(error.path)) << ": " << message;
    }
}

} // namespace PhosphorFsLoader
