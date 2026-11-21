// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// The check of a value against a compiled schema, in JSON Schema 2020-12:
// the walk before it that refuses what a handler would misread, the walk of
// the schema over the value with its annotations for unevaluatedProperties
// and unevaluatedItems, the lines of its errors, and its bound.

#include "mcpschemacheck.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QLatin1StringView>

#include <algorithm>
#include <bit>
#include <cmath>

namespace PhosphorMcp::Schema {

namespace {

// What each step of a check is charged (see MaxWork), so that a unit is
// about the same time whatever the schema and the value, as measured.
//
// A value walked before the check (its type read, its place kept).
constexpr qint64 UnitsPerValueWalked = 3;
// A subschema applied to a value.
constexpr qint64 UnitsPerApply = 2;
// More for an object or an array, walked or applied to: its members or
// items are read through its own storage, away from the last.
constexpr qint64 UnitsPerContainer = 2;
// A keyword checked, or an item visited.
constexpr qint64 UnitsPerStep = 1;
// A member visited: its name read.
constexpr qint64 UnitsPerMember = 2;
// An error built, beside its pointer's length.
constexpr qint64 UnitsPerError = 16;
// Each this many UTF-16 code units of a string or a name read, compared or
// hashed.
constexpr qint64 CharactersPerUnit = 16;
// Words of an annotation set merged or cleared for each unit.
constexpr qint64 WordsPerUnit = 4;

const QString TooLargeToCheck = QStringLiteral(": is too large for the server to check against the schema");

// How a subschema is applied. With Errors, its failure fails the whole
// value, so each error is listed and every keyword checked. With Verdict,
// only whether it matches counts (a branch of anyOf or oneOf, not, the
// condition of if, contains), and the walk stops at the first failure.
enum class Mode : quint8 {
    Errors,
    Verdict,
};

// "a", "a or b", "a, b or c".
QString alternatives(const QStringList& words)
{
    if (words.size() <= 1) {
        return words.value(0);
    }
    return QStringList(words.first(words.size() - 1)).join(QLatin1StringView(", ")) + QLatin1StringView(" or ")
        + words.last();
}

// The members of an object, or the items of an array, that subschemas
// evaluated: the annotations of 2020-12 that unevaluatedProperties and
// unevaluatedItems read. Every item below `prefix` (prefixItems), all of them
// (items, additionalProperties, a passed unevaluated* inside), or those in
// the set (properties, contains). The set is a bit for each member or item,
// made when the first is set, and its memory is counted.
class Evaluated
{
public:
    Evaluated(Budget& budget, qsizetype size)
        : m_budget(budget)
        , m_size(size)
    {
    }
    ~Evaluated()
    {
        m_budget.release(qint64(m_bits.size() * sizeof(quint64)));
    }
    Evaluated(const Evaluated&) = delete;
    Evaluated& operator=(const Evaluated&) = delete;

    void setAll()
    {
        m_all = true;
    }
    void setPrefix(qsizetype prefix)
    {
        m_prefix = std::max(m_prefix, prefix);
    }
    void set(qsizetype index)
    {
        if (m_all || index < m_prefix || !makeBits()) {
            return;
        }
        m_bits[std::size_t(index / 64)] |= quint64(1) << (index % 64);
    }
    [[nodiscard]] bool has(qsizetype index) const
    {
        return m_all || index < m_prefix || (!m_bits.empty() && (m_bits[std::size_t(index / 64)] >> (index % 64)) & 1);
    }
    [[nodiscard]] bool hasAll() const
    {
        return m_all;
    }
    void merge(const Evaluated& other)
    {
        m_all = m_all || other.m_all;
        m_prefix = std::max(m_prefix, other.m_prefix);
        if (m_all || other.m_bits.empty() || !makeBits()) {
            return;
        }
        m_budget.charge(1 + qint64(m_bits.size()) / WordsPerUnit);
        for (std::size_t i = 0; i < m_bits.size(); ++i) {
            m_bits[i] |= other.m_bits[i];
        }
    }

private:
    bool makeBits()
    {
        if (m_bits.empty()) {
            const std::size_t words = std::size_t((m_size + 63) / 64);
            if (!m_budget.allocate(qint64(words * sizeof(quint64)))) {
                m_budget.release(qint64(words * sizeof(quint64)));
                return false;
            }
            m_budget.charge(1 + qint64(words) / WordsPerUnit);
            m_bits.assign(words, 0);
        }
        return true;
    }

    Budget& m_budget;
    qsizetype m_size;
    qsizetype m_prefix = 0;
    bool m_all = false;
    std::vector<quint64> m_bits;
};

// The walk over a value before the check. It refuses what a handler would
// misread or the reply could not carry (an unpaired surrogate, a number that
// is not finite), and a value nested too deeply for the walks after it.
class Precheck
{
public:
    Precheck(Report& report, Budget& budget)
        : m_report(report)
        , m_budget(budget)
    {
    }

    void walk(const QJsonValue& value)
    {
        if (m_report.isDone() || !m_budget.charge(UnitsPerValueWalked)) {
            return;
        }
        if (qsizetype(m_path.size()) > MaxNestingDepth) {
            problem(QStringLiteral("is nested more than %1 levels deep, which the server does not check")
                        .arg(MaxNestingDepth));
            return;
        }
        switch (value.type()) {
        case QJsonValue::String: {
            const QString text = value.toString();
            m_budget.charge(text.size() / CharactersPerUnit);
            if (!text.isValidUtf16()) {
                problem(QStringLiteral("contains an unpaired UTF-16 surrogate"));
            }
            break;
        }
        case QJsonValue::Double:
            if (!std::isfinite(value.toDouble())) {
                problem(QStringLiteral("is a number that is not finite, which JSON cannot carry"));
            }
            break;
        case QJsonValue::Array: {
            m_budget.charge(UnitsPerContainer);
            const QJsonArray array = value.toArray();
            for (qsizetype i = 0; i < array.size() && !m_report.isDone(); ++i) {
                m_path.push_back(PathToken{QString(), i});
                walk(array.at(i));
                m_path.pop_back();
            }
            break;
        }
        case QJsonValue::Object: {
            m_budget.charge(UnitsPerContainer);
            const QJsonObject object = value.toObject();
            for (auto it = object.begin(); it != object.end() && !m_report.isDone(); ++it) {
                m_path.push_back(PathToken{it.key(), -1});
                m_budget.charge(UnitsPerMember + m_path.back().name.size() / CharactersPerUnit);
                if (!m_path.back().name.isValidUtf16()) {
                    problem(QStringLiteral("the property name contains an unpaired UTF-16 surrogate"));
                }
                walk(it.value());
                m_path.pop_back();
            }
            break;
        }
        case QJsonValue::Null:
        case QJsonValue::Bool:
        case QJsonValue::Undefined:
            break;
        }
    }

private:
    void problem(const QString& description)
    {
        m_budget.charge(UnitsPerError + qint64(m_path.size())
                        + m_report.add(m_path, nullptr, description) / CharactersPerUnit);
    }

    Report& m_report;
    Budget& m_budget;
    Path m_path;
};

// The check of a value against the nodes of a compiled schema.
class Checker
{
public:
    Checker(const std::vector<Node>& nodes, Report& report, Budget& budget)
        : m_nodes(nodes)
        , m_report(report)
        , m_budget(budget)
    {
    }

    bool check(const QJsonValue& value)
    {
        m_stackBase = quintptr(__builtin_frame_address(0));
        return apply(0, value, Mode::Errors, nullptr);
    }

private:
    [[nodiscard]] bool stopped() const
    {
        return m_budget.exceeded() || m_report.isDone();
    }
    bool charge(qint64 units)
    {
        return m_budget.charge(units);
    }
    // Looking `name` up in an object of `size` members: a binary search.
    void chargeLookup(qsizetype size, const QString& name)
    {
        charge(1 + std::bit_width(quint64(size)) + name.size() / CharactersPerUnit);
    }

    // An error at the current place, or at `last` below it. Kept out of
    // the frames of the recursion, as each check below that does not
    // recurse is: their locals would grow every frame of a deep walk.
    Q_NEVER_INLINE void error(const QString& description, const QString* last = nullptr)
    {
        const QString text = m_propertyName ? QStringLiteral("the property name ") + description : description;
        charge(UnitsPerError + qint64(m_path.size()) + m_report.add(m_path, last, text) / CharactersPerUnit);
    }

    bool apply(int index, const QJsonValue& value, Mode mode, Evaluated* out);
    bool applyNode(const Node& node, const QJsonValue& value, Mode mode, Evaluated* out);
    Q_NEVER_INLINE bool checkAssertions(const Node& node, const QJsonValue& value, Mode mode);
    Q_NEVER_INLINE bool checkType(const Node& node, const QJsonValue& value, Mode mode);
    Q_NEVER_INLINE bool checkLiterals(const Node& node, const QJsonValue& value, const QString* text, Mode mode);
    Q_NEVER_INLINE bool checkNumber(const Node& node, const QJsonValue& value, Mode mode);
    Q_NEVER_INLINE bool checkString(const Node& node, const QString& text, Mode mode);
    bool checkArray(const Node& node, const QJsonArray& array, Mode mode, Evaluated* evaluated);
    Q_NEVER_INLINE bool checkUnique(const QJsonArray& array, Mode mode);
    bool checkContains(const Node& node, const QJsonArray& array, Mode mode, Evaluated* evaluated);
    bool checkObject(const Node& node, const QJsonValue& value, const QJsonObject& object, Mode mode,
                     Evaluated* evaluated);
    bool checkName(int target, const QString& name, Mode mode);
    bool checkInPlace(const Node& node, const QJsonValue& value, qsizetype size, Mode mode, Evaluated* evaluated);
    bool checkUnevaluatedItems(const Node& node, const QJsonArray& array, Mode mode, Evaluated& evaluated);
    bool checkUnevaluatedProperties(const Node& node, const QJsonObject& object, Mode mode, Evaluated& evaluated);

    const std::vector<Node>& m_nodes;
    Report& m_report;
    Budget& m_budget;
    Path m_path;
    // The value is a property name, which propertyNames checks: the lines
    // say so.
    bool m_propertyName = false;
    int m_depth = 0;
    // The frame of check(). The stack grows down on every Linux the library
    // builds for.
    quintptr m_stackBase = 0;
};

// Applies the node `index` to `value`. When `out` is not null, the members
// or items of `value` that the node evaluated are added to it.
bool Checker::apply(int index, const QJsonValue& value, Mode mode, Evaluated* out)
{
    if (stopped()) {
        return false;
    }
    if (m_depth >= MaxCheckDepth || qint64(m_stackBase - quintptr(__builtin_frame_address(0))) > MaxCheckStackBytes) {
        m_budget.exceed();
        return false;
    }
    charge(UnitsPerApply);
    ++m_depth;
    const Node& node = m_nodes[std::size_t(index)];
    bool valid = node.booleanValue;
    if (!node.isBoolean) {
        valid = applyNode(node, value, mode, out);
    } else if (!valid && mode == Mode::Errors) {
        error(QStringLiteral("does not match the schema"));
    }
    --m_depth;
    return valid && !stopped();
}

bool Checker::applyNode(const Node& node, const QJsonValue& value, Mode mode, Evaluated* out)
{
    bool valid = true;
    // A keyword failed: true when the walk of this node ends there.
    const auto fail = [&]() {
        valid = false;
        return mode == Mode::Verdict || stopped();
    };

    const bool isObject = value.isObject();
    const bool isArray = value.isArray();
    if (isObject || isArray) {
        charge(UnitsPerContainer);
    }
    const QJsonObject object = isObject ? value.toObject() : QJsonObject();
    const QJsonArray array = isArray ? value.toArray() : QJsonArray();
    const qsizetype size = isObject ? object.size() : array.size();
    // The annotations of this node: for the caller, or for its own
    // unevaluatedProperties or unevaluatedItems.
    Evaluated local(m_budget, size);
    Evaluated* evaluated = nullptr;
    if (isObject || isArray) {
        if (out) {
            evaluated = out;
        } else if ((isObject && node.unevaluatedProperties >= 0) || (isArray && node.unevaluatedItems >= 0)) {
            evaluated = &local;
        }
    }

    if ((node.types != 0 || node.hasConst || node.hasEnum || node.numberKeywords != 0 || node.minLength >= 0
         || node.maxLength >= 0)
        && !checkAssertions(node, value, mode) && fail()) {
        return false;
    }
    if (isArray && !checkArray(node, array, mode, evaluated) && fail()) {
        return false;
    }
    if (isObject && !checkObject(node, value, object, mode, evaluated) && fail()) {
        return false;
    }
    if (!checkInPlace(node, value, size, mode, evaluated) && fail()) {
        return false;
    }
    if (isArray && node.unevaluatedItems >= 0 && !checkUnevaluatedItems(node, array, mode, *evaluated) && fail()) {
        return false;
    }
    if (isObject && node.unevaluatedProperties >= 0 && !checkUnevaluatedProperties(node, object, mode, *evaluated)
        && fail()) {
        return false;
    }
    return valid;
}

// The keywords that do not apply subschemas: type, const, enum, and those
// of numbers and strings.
bool Checker::checkAssertions(const Node& node, const QJsonValue& value, Mode mode)
{
    bool valid = true;
    const auto fail = [&]() {
        valid = false;
        return mode == Mode::Verdict || stopped();
    };
    if (node.types != 0 && !checkType(node, value, mode) && fail()) {
        return false;
    }
    QString text;
    const bool isString = value.isString();
    if (isString && (node.hasConst || node.hasEnum || node.minLength >= 0 || node.maxLength >= 0)) {
        // Read once, for every keyword that reads it.
        text = value.toString();
        charge(text.size() / CharactersPerUnit);
    }
    if ((node.hasConst || node.hasEnum) && !checkLiterals(node, value, isString ? &text : nullptr, mode) && fail()) {
        return false;
    }
    if (value.isDouble() && node.numberKeywords != 0 && !checkNumber(node, value, mode) && fail()) {
        return false;
    }
    if (isString && (node.minLength >= 0 || node.maxLength >= 0) && !checkString(node, text, mode) && fail()) {
        return false;
    }
    return valid;
}

// A number matches "integer" when it has no fraction and fits a 64-bit
// integer, the range in which a handler reads it with toInteger(). 1e19 is
// an integer in 2020-12, but QJsonValue::toInteger() gives a handler 0 for
// it, so it is refused where the schema asks for an integer, and allowed
// where it asks for a number.
bool Checker::checkType(const Node& node, const QJsonValue& value, Mode mode)
{
    charge(UnitsPerStep);
    quint8 bit = 0;
    bool outsideInt64 = false;
    switch (value.type()) {
    case QJsonValue::Null:
    case QJsonValue::Undefined:
        bit = TypeNull;
        break;
    case QJsonValue::Bool:
        bit = TypeBoolean;
        break;
    case QJsonValue::String:
        bit = TypeString;
        break;
    case QJsonValue::Array:
        bit = TypeArray;
        break;
    case QJsonValue::Object:
        bit = TypeObject;
        break;
    case QJsonValue::Double: {
        const Number number = numberOf(value);
        bit = TypeNumber;
        if (number.isInteger) {
            bit |= TypeInteger;
        } else if (isIntegral(number)) {
            outsideInt64 = true;
        }
        break;
    }
    }
    if ((node.types & bit) != 0) {
        return true;
    }
    if (mode == Mode::Errors) {
        if (outsideInt64 && (node.types & TypeInteger) != 0) {
            error(QStringLiteral("must be from -9223372036854775808 to 9223372036854775807"));
        } else {
            error(QStringLiteral("must be of type ") + alternatives(node.typeNames));
        }
    }
    return false;
}

bool Checker::checkLiterals(const Node& node, const QJsonValue& value, const QString* text, Mode mode)
{
    bool valid = true;
    if (node.hasConst) {
        charge(UnitsPerStep);
        const bool equal = text && node.constValue.isString()
            ? (charge(text->size() == node.constString.size() ? text->size() / CharactersPerUnit : 0),
               *text == node.constString)
            : equalValues(node.constValue, value, text, m_budget);
        if (!equal) {
            valid = false;
            if (mode == Mode::Verdict) {
                return false;
            }
            error(QStringLiteral("must be the value of const"));
        }
    }
    if (node.hasEnum) {
        charge(UnitsPerStep);
        bool found = false;
        if (text) {
            if (!node.enumStringSet.isEmpty()) {
                // A hash of the string, and a comparison with the entry.
                charge(1 + 2 * (text->size() / CharactersPerUnit));
                found = node.enumStringSet.contains(*text);
            } else {
                for (const QString& entry : node.enumStrings) {
                    charge(UnitsPerStep + (entry.size() == text->size() ? text->size() / CharactersPerUnit : 0));
                    if (entry == *text) {
                        found = true;
                        break;
                    }
                }
            }
        } else {
            for (std::size_t i = 0; i < node.enumOthers.size() && !found && !stopped(); ++i) {
                found = equalValues(node.enumOthers[i], value, nullptr, m_budget);
            }
        }
        if (!found) {
            valid = false;
            if (mode == Mode::Errors) {
                error(QStringLiteral("must be one of the values of enum"));
            }
        }
    }
    return valid;
}

bool Checker::checkNumber(const Node& node, const QJsonValue& value, Mode mode)
{
    const Number number = numberOf(value);
    struct Bound
    {
        Node::NumberKeyword bit;
        Number Node::* limit;
        // The comparison with the limit that passes.
        bool (*passes)(int);
        const char* description;
    };
    static const Bound bounds[] = {
        {Node::Minimum, &Node::minimum,
         [](int c) {
             return c >= 0;
         },
         "must be at least %1"},
        {Node::ExclusiveMinimum, &Node::exclusiveMinimum,
         [](int c) {
             return c > 0;
         },
         "must be greater than %1"},
        {Node::Maximum, &Node::maximum,
         [](int c) {
             return c <= 0;
         },
         "must be at most %1"},
        {Node::ExclusiveMaximum, &Node::exclusiveMaximum,
         [](int c) {
             return c < 0;
         },
         "must be less than %1"},
    };
    bool valid = true;
    for (const Bound& bound : bounds) {
        if ((node.numberKeywords & bound.bit) == 0) {
            continue;
        }
        charge(UnitsPerStep);
        if (!bound.passes(compareNumbers(number, node.*(bound.limit)))) {
            valid = false;
            if (mode == Mode::Verdict) {
                return false;
            }
            error(QString::fromLatin1(bound.description).arg(formatNumber(node.*(bound.limit))));
        }
    }
    if ((node.numberKeywords & Node::MultipleOf) != 0) {
        charge(UnitsPerStep);
        if (!isMultipleOf(number, node.multipleOf)) {
            valid = false;
            if (mode == Mode::Errors) {
                error(QStringLiteral("must be a multiple of %1").arg(formatNumber(node.multipleOf)));
            }
        }
    }
    return valid;
}

// Lengths in code points, as 2020-12 counts them: a surrogate pair is one
// (an unpaired surrogate was refused before). Between half the code units
// and all of them, so a string is counted only when that decides.
bool Checker::checkString(const Node& node, const QString& text, Mode mode)
{
    const qsizetype units = text.size();
    qsizetype length = -1;
    const auto count = [&]() {
        if (length < 0) {
            charge(1 + units / CharactersPerUnit);
            length = 0;
            for (const QChar c : text) {
                length += c.isLowSurrogate() ? 0 : 1;
            }
        }
        return length;
    };
    bool valid = true;
    charge(UnitsPerStep);
    if (node.maxLength >= 0 && units > node.maxLength && count() > node.maxLength) {
        valid = false;
        if (mode == Mode::Verdict) {
            return false;
        }
        error(node.maxLength == 1 ? QStringLiteral("must be at most 1 character long")
                                  : QStringLiteral("must be at most %1 characters long").arg(node.maxLength));
    }
    if (node.minLength >= 0 && (units + 1) / 2 < node.minLength && count() < node.minLength) {
        valid = false;
        if (mode == Mode::Errors) {
            error(node.minLength == 1 ? QStringLiteral("must be at least 1 character long")
                                      : QStringLiteral("must be at least %1 characters long").arg(node.minLength));
        }
    }
    return valid;
}

bool Checker::checkArray(const Node& node, const QJsonArray& array, Mode mode, Evaluated* evaluated)
{
    bool valid = true;
    const auto fail = [&]() {
        valid = false;
        return mode == Mode::Verdict || stopped();
    };
    const qsizetype size = array.size();
    charge(UnitsPerStep);
    if (node.minItems >= 0 && size < node.minItems) {
        if (mode == Mode::Errors) {
            error(node.minItems == 1 ? QStringLiteral("must have at least 1 item")
                                     : QStringLiteral("must have at least %1 items").arg(node.minItems));
        }
        if (fail()) {
            return false;
        }
    }
    if (node.maxItems >= 0 && size > node.maxItems) {
        if (mode == Mode::Errors) {
            error(node.maxItems == 1 ? QStringLiteral("must have at most 1 item")
                                     : QStringLiteral("must have at most %1 items").arg(node.maxItems));
        }
        if (fail()) {
            return false;
        }
    }
    if (node.uniqueItems && !checkUnique(array, mode) && fail()) {
        return false;
    }

    const qsizetype prefix = std::min(size, qsizetype(node.prefixItems.size()));
    for (qsizetype i = 0; i < prefix; ++i) {
        m_path.push_back(PathToken{QString(), i});
        const bool ok = apply(node.prefixItems[std::size_t(i)], array.at(i), mode, nullptr);
        m_path.pop_back();
        if (!ok && fail()) {
            return false;
        }
    }
    if (evaluated) {
        evaluated->setPrefix(prefix);
    }
    if (node.items >= 0 && size > prefix) {
        const Node& items = m_nodes[std::size_t(node.items)];
        if (items.isBoolean && !items.booleanValue) {
            // Each item past those of prefixItems.
            const qsizetype allowed = qsizetype(node.prefixItems.size());
            const QString description = allowed == 0
                ? QStringLiteral("is not allowed, because the schema allows no items")
                : allowed == 1
                ? QStringLiteral("is not allowed, because the schema allows at most 1 item")
                : QStringLiteral("is not allowed, because the schema allows at most %1 items").arg(allowed);
            for (qsizetype i = prefix; i < size; ++i) {
                charge(UnitsPerStep);
                if (mode == Mode::Errors) {
                    m_path.push_back(PathToken{QString(), i});
                    error(description);
                    m_path.pop_back();
                }
                if (fail()) {
                    return false;
                }
            }
        } else {
            for (qsizetype i = prefix; i < size; ++i) {
                m_path.push_back(PathToken{QString(), i});
                const bool ok = apply(node.items, array.at(i), mode, nullptr);
                m_path.pop_back();
                if (!ok && fail()) {
                    return false;
                }
            }
        }
        if (evaluated) {
            evaluated->setAll();
        }
    }
    if (node.contains >= 0 && !checkContains(node, array, mode, evaluated) && fail()) {
        return false;
    }
    return valid;
}

bool Checker::checkContains(const Node& node, const QJsonArray& array, Mode mode, Evaluated* evaluated)
{
    const qint64 minimum = node.minContains >= 0 ? node.minContains : 1;
    const qint64 maximum = node.maxContains;
    qint64 matches = 0;
    for (qsizetype i = 0; i < array.size(); ++i) {
        if (stopped()) {
            return false;
        }
        // The errors of an item that does not match are no errors of the
        // value.
        m_path.push_back(PathToken{QString(), i});
        const bool ok = apply(node.contains, array.at(i), Mode::Verdict, nullptr);
        m_path.pop_back();
        if (ok) {
            ++matches;
            if (evaluated) {
                evaluated->set(i);
            } else if ((maximum >= 0 && matches > maximum) || (maximum < 0 && matches >= minimum)) {
                // Nobody needs the other matches, and the verdict is known.
                break;
            }
        }
    }
    bool valid = true;
    if (matches < minimum) {
        valid = false;
        if (mode == Mode::Errors) {
            error(minimum == 1 ? QStringLiteral("must have an item that matches contains")
                               : QStringLiteral("must have at least %1 items that match contains").arg(minimum));
        }
    }
    if (maximum >= 0 && matches > maximum) {
        valid = false;
        if (mode == Mode::Errors) {
            error(maximum == 1 ? QStringLiteral("must have at most 1 item that matches contains")
                               : QStringLiteral("must have at most %1 items that match contains").arg(maximum));
        }
    }
    return valid;
}

// Items compared as 2020-12 compares them (1 and 1.0 alike, members in any
// order), in O(n log n): the items are sorted by a hash of their value, and
// only those with the same hash are compared, sorted by their value. The
// line names the first item that repeats an earlier one.
bool Checker::checkUnique(const QJsonArray& array, Mode mode)
{
    const qsizetype size = array.size();
    if (size < 2) {
        return true;
    }
    struct Key
    {
        quint64 hash;
        qsizetype index;
    };
    const qint64 bytes = qint64(size) * qint64(sizeof(Key)) * 2;
    if (!m_budget.allocate(bytes)) {
        m_budget.release(bytes);
        return false;
    }
    std::vector<Key> keys;
    keys.reserve(std::size_t(size));
    for (qsizetype i = 0; i < size && !stopped(); ++i) {
        keys.push_back(Key{hashValue(array.at(i), m_budget), i});
    }
    // A sort of plain integers, whose comparisons are counted here: about
    // two nanoseconds each.
    charge(qint64(size) * (1 + std::bit_width(quint64(size)) / 4));
    if (stopped()) {
        m_budget.release(bytes);
        return false;
    }
    std::stable_sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) {
        return a.hash < b.hash;
    });

    // Within each run of one hash, a merge sort by value (stable, so equal
    // items stay in the order of their indices), which stops when the work
    // is spent, whatever the comparisons said.
    qsizetype firstRepeat = -1;
    qsizetype repeated = -1;
    std::vector<Key> scratch;
    std::size_t start = 0;
    while (start < keys.size() && !stopped()) {
        std::size_t end = start + 1;
        while (end < keys.size() && keys[end].hash == keys[start].hash) {
            ++end;
        }
        if (end - start > 1 && equalValues(array.at(keys[start].index), array.at(keys[start + 1].index), m_budget)) {
            // The run is in the order of the indices: its first two items
            // are equal, and no repeat in it comes earlier.
            if (firstRepeat < 0 || keys[start + 1].index < firstRepeat) {
                firstRepeat = keys[start + 1].index;
                repeated = keys[start].index;
            }
        } else if (end - start > 2) {
            const auto less = [&](const Key& a, const Key& b) {
                return compareValues(array.at(a.index), array.at(b.index), m_budget) < 0;
            };
            scratch.resize(end - start);
            for (std::size_t width = 1; width < end - start && !stopped(); width *= 2) {
                for (std::size_t low = start; low < end && !stopped(); low += 2 * width) {
                    const std::size_t middle = std::min(low + width, end);
                    const std::size_t high = std::min(low + 2 * width, end);
                    std::merge(keys.begin() + std::ptrdiff_t(low), keys.begin() + std::ptrdiff_t(middle),
                               keys.begin() + std::ptrdiff_t(middle), keys.begin() + std::ptrdiff_t(high),
                               scratch.begin(), less);
                    std::copy(scratch.begin(), scratch.begin() + std::ptrdiff_t(high - low),
                              keys.begin() + std::ptrdiff_t(low));
                }
            }
            for (std::size_t i = start + 1; i < end && !stopped(); ++i) {
                if (equalValues(array.at(keys[i - 1].index), array.at(keys[i].index), m_budget)) {
                    // The first two of a group of equal items are its first
                    // item and its first repeat.
                    if (firstRepeat < 0 || keys[i].index < firstRepeat) {
                        firstRepeat = keys[i].index;
                        repeated = keys[i - 1].index;
                    }
                    while (i + 1 < end && equalValues(array.at(keys[i].index), array.at(keys[i + 1].index), m_budget)) {
                        ++i;
                    }
                }
            }
        }
        if (firstRepeat >= 0 && mode == Mode::Verdict) {
            break;
        }
        start = end;
    }
    m_budget.release(bytes);
    if (stopped() || firstRepeat < 0) {
        return firstRepeat < 0 && !stopped();
    }
    if (mode == Mode::Errors) {
        error(QStringLiteral("must have unique items, and items %1 and %2 are equal").arg(repeated).arg(firstRepeat));
    }
    return false;
}

bool Checker::checkObject(const Node& node, const QJsonValue& value, const QJsonObject& object, Mode mode,
                          Evaluated* evaluated)
{
    bool valid = true;
    const auto fail = [&]() {
        valid = false;
        return mode == Mode::Verdict || stopped();
    };
    const qsizetype size = object.size();
    charge(UnitsPerStep);
    if (node.minProperties >= 0 && size < node.minProperties) {
        if (mode == Mode::Errors) {
            error(node.minProperties == 1 ? QStringLiteral("must have at least 1 property")
                                          : QStringLiteral("must have at least %1 properties").arg(node.minProperties));
        }
        if (fail()) {
            return false;
        }
    }
    if (node.maxProperties >= 0 && size > node.maxProperties) {
        if (mode == Mode::Errors) {
            error(node.maxProperties == 1 ? QStringLiteral("must have at most 1 property")
                                          : QStringLiteral("must have at most %1 properties").arg(node.maxProperties));
        }
        if (fail()) {
            return false;
        }
    }

    // Each member once: its name against propertyNames, then its value
    // against its schema in properties, or additionalProperties.
    if (!node.properties.isEmpty() || node.additionalProperties >= 0 || node.propertyNames >= 0) {
        const bool closed = node.additionalProperties >= 0 && m_nodes[std::size_t(node.additionalProperties)].isBoolean
            && !m_nodes[std::size_t(node.additionalProperties)].booleanValue;
        qsizetype i = 0;
        for (auto it = object.begin(); it != object.end(); ++it, ++i) {
            if (stopped()) {
                return false;
            }
            m_path.push_back(PathToken{it.key(), -1});
            const QString& name = m_path.back().name;
            charge(UnitsPerMember + name.size() / CharactersPerUnit);
            bool ok = node.propertyNames < 0 || checkName(node.propertyNames, name, mode);
            if (ok || mode == Mode::Errors) {
                int child = -1;
                if (!node.properties.isEmpty()) {
                    // A hash of the name.
                    charge(1 + name.size() / CharactersPerUnit);
                    child = node.properties.value(name, -1);
                }
                if (child >= 0) {
                    if (evaluated) {
                        evaluated->set(i);
                    }
                    ok = apply(child, it.value(), mode, nullptr) && ok;
                } else if (closed) {
                    if (mode == Mode::Errors) {
                        error(QStringLiteral("is not a known property"));
                    }
                    ok = false;
                } else if (node.additionalProperties >= 0) {
                    ok = apply(node.additionalProperties, it.value(), mode, nullptr) && ok;
                }
            }
            m_path.pop_back();
            if (!ok && fail()) {
                return false;
            }
        }
        if (evaluated && node.additionalProperties >= 0) {
            evaluated->setAll();
        }
    }

    for (const QString& name : node.required) {
        chargeLookup(size, name);
        if (!object.contains(name)) {
            if (mode == Mode::Errors) {
                error(QStringLiteral("is required"), &name);
            }
            if (fail()) {
                return false;
            }
        }
    }
    // The triggers that the object has: each trigger looked up in the
    // object, or each member in the triggers, whichever are fewer.
    std::vector<int> triggered;
    if (!node.dependentRequired.empty()) {
        if (qsizetype(node.dependentRequired.size()) <= size) {
            for (std::size_t i = 0; i < node.dependentRequired.size(); ++i) {
                chargeLookup(size, node.dependentRequired[i].first);
                if (object.contains(node.dependentRequired[i].first)) {
                    triggered.push_back(int(i));
                }
            }
        } else {
            for (auto it = object.begin(); it != object.end(); ++it) {
                const QString name = it.key();
                charge(UnitsPerMember + 2 * (name.size() / CharactersPerUnit));
                const int i = node.dependentRequiredIndex.value(name, -1);
                if (i >= 0) {
                    triggered.push_back(i);
                }
            }
            // In the schema's order, as the other way finds them.
            std::sort(triggered.begin(), triggered.end());
        }
    }
    for (const int i : triggered) {
        const auto& [trigger, names] = node.dependentRequired[std::size_t(i)];
        for (const QString& name : names) {
            chargeLookup(size, name);
            if (!object.contains(name)) {
                if (mode == Mode::Errors) {
                    error(QStringLiteral("is required when \"%1\" is present").arg(shownName(trigger)), &name);
                }
                if (fail()) {
                    return false;
                }
            }
        }
    }
    for (const auto& [trigger, schema] : node.dependentSchemas) {
        chargeLookup(size, trigger);
        if (!object.contains(trigger)) {
            continue;
        }
        Evaluated sub(m_budget, size);
        const bool ok = apply(schema, value, mode, evaluated ? &sub : nullptr);
        // The annotations of a subschema that failed are dropped. With
        // Errors the value fails anyway, and they keep its members from
        // being named unknown too.
        if (evaluated && (ok || mode == Mode::Errors)) {
            evaluated->merge(sub);
        }
        if (!ok && fail()) {
            return false;
        }
    }
    return valid;
}

bool Checker::checkName(int target, const QString& name, Mode mode)
{
    const Node& node = m_nodes[std::size_t(target)];
    if (node.isBoolean) {
        if (!node.booleanValue && mode == Mode::Errors) {
            error(QStringLiteral("is not allowed, because the schema allows no property here"));
        }
        return node.booleanValue;
    }
    charge(UnitsPerStep);
    const bool saved = m_propertyName;
    m_propertyName = true;
    const bool ok = apply(target, QJsonValue(name), mode, nullptr);
    m_propertyName = saved;
    return ok;
}

// The subschemas that apply to the value itself. Those that must pass
// ($ref, allOf, then or else) add their annotations when they pass, and
// with Errors also when they fail, since the value fails then anyway. A
// branch of anyOf or oneOf adds them only when it passes, and not and the
// condition of if that failed never do.
bool Checker::checkInPlace(const Node& node, const QJsonValue& value, qsizetype size, Mode mode, Evaluated* evaluated)
{
    bool valid = true;
    const auto fail = [&]() {
        valid = false;
        return mode == Mode::Verdict || stopped();
    };
    const auto required = [&](int child) {
        if (!evaluated) {
            return apply(child, value, mode, nullptr);
        }
        Evaluated sub(m_budget, size);
        const bool ok = apply(child, value, mode, &sub);
        if (ok || mode == Mode::Errors) {
            evaluated->merge(sub);
        }
        return ok;
    };

    if (node.ref >= 0 && !required(node.ref) && fail()) {
        return false;
    }
    for (const int branch : node.allOf) {
        if (!required(branch) && fail()) {
            return false;
        }
    }
    if (!node.anyOf.empty()) {
        // Whether a branch passes, first. Only when none does are the
        // branches' errors the value's, and they are listed then.
        int passed = 0;
        for (const int branch : node.anyOf) {
            Evaluated sub(m_budget, size);
            if (apply(branch, value, Mode::Verdict, evaluated ? &sub : nullptr)) {
                ++passed;
                if (!evaluated) {
                    break;
                }
                evaluated->merge(sub);
            }
            if (stopped()) {
                return false;
            }
        }
        if (passed == 0) {
            if (mode == Mode::Errors) {
                for (const int branch : node.anyOf) {
                    // Every branch failed: their annotations keep the members
                    // that a branch declares from being named unknown.
                    required(branch);
                }
                error(QStringLiteral("must match at least one schema of anyOf"));
            }
            if (fail()) {
                return false;
            }
        }
    }
    if (!node.oneOf.empty()) {
        int passed = 0;
        for (const int branch : node.oneOf) {
            Evaluated sub(m_budget, size);
            if (apply(branch, value, Mode::Verdict, evaluated ? &sub : nullptr)) {
                ++passed;
                if (evaluated) {
                    evaluated->merge(sub);
                }
                if (passed == 2) {
                    break;
                }
            }
            if (stopped()) {
                return false;
            }
        }
        if (passed != 1) {
            if (mode == Mode::Errors) {
                if (passed == 0) {
                    for (const int branch : node.oneOf) {
                        required(branch);
                    }
                    error(QStringLiteral("must match exactly one schema of oneOf, and matches none"));
                } else {
                    error(QStringLiteral("must match exactly one schema of oneOf, and matches more than one"));
                }
            }
            if (fail()) {
                return false;
            }
        }
    }
    if (node.notSchema >= 0) {
        // Whatever not's subschema evaluated, it failed, or not fails.
        if (apply(node.notSchema, value, Mode::Verdict, nullptr)) {
            if (mode == Mode::Errors) {
                error(QStringLiteral("must not match the schema of not"));
            }
            if (fail()) {
                return false;
            }
        }
        if (stopped()) {
            return false;
        }
    }
    // An if without then or else decides nothing, and only its
    // annotations count.
    if (node.ifSchema >= 0 && (evaluated || node.thenSchema >= 0 || node.elseSchema >= 0)) {
        Evaluated condition(m_budget, size);
        const bool holds = apply(node.ifSchema, value, Mode::Verdict, evaluated ? &condition : nullptr);
        if (stopped()) {
            return false;
        }
        if (holds) {
            if (evaluated) {
                evaluated->merge(condition);
            }
            if (node.thenSchema >= 0 && !required(node.thenSchema) && fail()) {
                return false;
            }
        } else if (node.elseSchema >= 0 && !required(node.elseSchema) && fail()) {
            return false;
        }
    }
    return valid;
}

bool Checker::checkUnevaluatedItems(const Node& node, const QJsonArray& array, Mode mode, Evaluated& evaluated)
{
    bool valid = true;
    if (!evaluated.hasAll()) {
        const Node& rest = m_nodes[std::size_t(node.unevaluatedItems)];
        const bool closed = rest.isBoolean && !rest.booleanValue;
        for (qsizetype i = 0; i < array.size(); ++i) {
            if (stopped()) {
                return false;
            }
            charge(UnitsPerStep);
            if (evaluated.has(i)) {
                continue;
            }
            m_path.push_back(PathToken{QString(), i});
            bool ok = true;
            if (closed) {
                if (mode == Mode::Errors) {
                    error(QStringLiteral("is not allowed, because the schema allows no more items"));
                }
                ok = false;
            } else {
                ok = apply(node.unevaluatedItems, array.at(i), mode, nullptr);
            }
            m_path.pop_back();
            if (!ok) {
                valid = false;
                if (mode == Mode::Verdict) {
                    return false;
                }
            }
        }
    }
    evaluated.setAll();
    return valid;
}

bool Checker::checkUnevaluatedProperties(const Node& node, const QJsonObject& object, Mode mode, Evaluated& evaluated)
{
    bool valid = true;
    if (!evaluated.hasAll()) {
        const Node& rest = m_nodes[std::size_t(node.unevaluatedProperties)];
        const bool closed = rest.isBoolean && !rest.booleanValue;
        qsizetype i = 0;
        for (auto it = object.begin(); it != object.end(); ++it, ++i) {
            if (stopped()) {
                return false;
            }
            charge(UnitsPerStep);
            if (evaluated.has(i)) {
                continue;
            }
            m_path.push_back(PathToken{it.key(), -1});
            charge(UnitsPerMember + m_path.back().name.size() / CharactersPerUnit);
            bool ok = true;
            if (closed) {
                if (mode == Mode::Errors) {
                    error(QStringLiteral("is not a known property"));
                }
                ok = false;
            } else {
                ok = apply(node.unevaluatedProperties, it.value(), mode, nullptr);
            }
            m_path.pop_back();
            if (!ok) {
                valid = false;
                if (mode == Mode::Verdict) {
                    return false;
                }
            }
        }
    }
    evaluated.setAll();
    return valid;
}

} // namespace

QStringList checkValue(const CompiledSchema& schema, const QJsonValue& value, CheckStats* stats)
{
    Budget budget;
    Report report;
    QStringList lines;
    Precheck(report, budget).walk(value);
    if (report.isDone()) {
        lines = report.take();
    } else if (budget.exceeded()) {
        lines = {TooLargeToCheck};
    } else if (!report.isEmpty()) {
        lines = report.take();
    } else {
        Checker checker(schema.nodes, report, budget);
        const bool valid = checker.check(value);
        // Too many errors to list is a verdict, and stops the check.
        if (report.isDone()) {
            lines = report.take();
        } else if (budget.exceeded()) {
            lines = {TooLargeToCheck};
        } else if (!valid) {
            lines = report.isEmpty() ? QStringList{QStringLiteral(": does not match the schema")} : report.take();
        }
    }
    if (stats) {
        stats->work = budget.work();
        stats->peakMemory = budget.peakMemory();
    }
    return lines;
}

} // namespace PhosphorMcp::Schema
