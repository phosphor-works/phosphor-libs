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
#include <optional>
#include <type_traits>

namespace PhosphorMcp::Schema {

namespace {

const QString TooLargeToCheck = QStringLiteral(": is too large for the server to check against the schema");
// The value that a property name is checked as (see Checker::checkName()).
const QJsonValue NameStandIn(QJsonValue::String);

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

// What the subschemas applied to the same value in place read once and
// share: the type of the value and its number (calls into Qt), the length
// of a string, and an index of an object's member names to look names up
// in. QJsonObject::contains() searches the members in the order that Qt
// keeps them in, and an object that a handler built from a parsed one and
// then added to is in no single order, so it misses names. The index holds
// the names sorted by a hash keyed as uniqueItems' is (a name is found
// among those with its hash, which the compile computed for each name of
// the schema), is built the first time a subschema looks a name up, and
// its memory is counted.
class Shared
{
public:
    Shared(Budget& budget, quint64 key)
        : m_budget(budget)
        , m_key(key)
    {
    }
    ~Shared()
    {
        m_budget.release(m_bytes);
    }
    Shared(const Shared&) = delete;
    Shared& operator=(const Shared&) = delete;

    // The type of `value`, read once.
    QJsonValue::Type type(const QJsonValue& value)
    {
        if (!m_typed) {
            m_type = value.type();
            m_typed = true;
        }
        return m_type;
    }

    // The number of `value`, a number, read once.
    const Number& number(const QJsonValue& value)
    {
        if (!m_numbered) {
            m_number = numberOf(value);
            m_numbered = true;
        }
        return m_number;
    }

    // The length of `text` in code points, counted once.
    qsizetype length(QAnyStringView text)
    {
        if (m_length < 0) {
            m_budget.charge(1 + textUnits(1, text.size()));
            m_length = codePoints(text);
        }
        return m_length;
    }

    // Whether `object` has a member `name`, whose hash is `hash`. False,
    // too, once the budget is spent.
    bool contains(const QJsonObject& object, const QString& name, quint64 hash)
    {
        const qsizetype size = object.size();
        if (size == 0) {
            m_budget.charge(1);
            return false;
        }
        if (!m_indexed && (m_failed || !index(object))) {
            return false;
        }
        // A binary search, and the names of its hash compared with it.
        m_budget.charge(1 + std::bit_width(quint64(size)));
        auto it = std::lower_bound(m_names.begin(), m_names.end(), hash, [](const Name& a, quint64 b) {
            return a.hash < b;
        });
        for (; it != m_names.end() && it->hash == hash; ++it) {
            m_budget.charge(UnitsPerStep + equalUnits(it->name, name));
            if (QAnyStringView::equal(it->name, name)) {
                return true;
            }
        }
        return false;
    }

private:
    struct Name
    {
        quint64 hash;
        QAnyStringView name;
    };

    // Made once, whether it fails or not. The budget is spent when it
    // fails.
    bool index(const QJsonObject& object)
    {
        m_failed = true;
        const qsizetype size = object.size();
        m_bytes = qint64(size) * qint64(sizeof(Name));
        if (!m_budget.allocate(m_bytes)) {
            return false;
        }
        // Each name hashed, and a sort of plain integers.
        qint64 units = qint64(size) * (UnitsPerMember + std::bit_width(quint64(size)) / ComparisonsPerUnit);
        m_names.reserve(std::size_t(size));
        TextReads reads;
        for (auto it = object.begin(); it != object.end(); ++it) {
            const QAnyStringView name = it.keyView();
            units += reads.next(name) + hashUnits(name);
            m_names.push_back(Name{hashText(name, m_key), name});
        }
        if (!m_budget.charge(units)) {
            return false;
        }
        std::sort(m_names.begin(), m_names.end(), [](const Name& a, const Name& b) {
            return a.hash < b.hash;
        });
        m_failed = false;
        m_indexed = true;
        return true;
    }

    Budget& m_budget;
    quint64 m_key;
    std::vector<Name> m_names;
    qint64 m_bytes = 0;
    qsizetype m_length = -1;
    Number m_number;
    QJsonValue::Type m_type = QJsonValue::Null;
    bool m_typed = false;
    bool m_numbered = false;
    bool m_indexed = false;
    bool m_failed = false;
};

// A text read where it is (see BytesPerLine).
qint64 lineUnits(QAnyStringView text)
{
    return std::min(MaxLineUnits, qint64(text.size_bytes()) / BytesPerLine);
}

// A member visited, its name read where it is.
qint64 memberUnits(QAnyStringView name)
{
    return UnitsPerMember + lineUnits(name);
}

// A text looked up in a NameTable: a hash of it, and a comparison with the
// entry, each a unit for each 16 code units.
qint64 lookupUnits(QAnyStringView text)
{
    return UnitsPerMember + textUnits(2, text.size());
}

// Whether a string of a value is `text`, which the schema keeps in UTF-16
// and in UTF-8: compared in the string's own encoding, never decoded, and
// only when the sizes agree.
bool sameText(QAnyStringView value, const QString& text, const QByteArray& utf8, Budget& budget)
{
    return value.visit([&](auto view) {
        using View = decltype(view);
        constexpr bool isUtf8 = !std::is_same_v<View, QStringView> && !std::is_same_v<View, QLatin1StringView>;
        const qsizetype size = isUtf8 ? utf8.size() : text.size();
        if (view.size() != size) {
            return false;
        }
        budget.charge(textUnits(1, size));
        if constexpr (std::is_same_v<View, QStringView>) {
            return view == QStringView(text);
        } else if constexpr (std::is_same_v<View, QLatin1StringView>) {
            return view == text;
        } else {
            return QByteArrayView(reinterpret_cast<const char*>(view.data()), view.size()) == utf8;
        }
    });
}

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

    // `reads`: those of the container that `value` is an item or a member
    // of, if any.
    void walk(const QJsonValue& value, TextReads* reads = nullptr)
    {
        if (m_report.isDone() || !m_budget.charge(UnitsPerValueWalked)) {
            return;
        }
        if (qsizetype(m_place.tokens().size()) > MaxNestingDepth) {
            problem(QStringLiteral("is nested more than %1 levels deep, which the server does not check")
                        .arg(MaxNestingDepth));
            return;
        }
        switch (value.type()) {
        case QJsonValue::String: {
            // Read in place: only UTF-16 can hold an unpaired surrogate,
            // and only it is read through.
            const QAnyStringView text = value.toStringView();
            m_budget.charge(lineUnits(text) + (reads ? reads->next(text) : 0)
                            + (isUtf16(text) ? textUnits(1, text.size()) : 0));
            if (!isValidText(text)) {
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
            TextReads items;
            for (qsizetype i = 0; i < array.size() && !m_report.isDone(); ++i) {
                m_place.pushIndex(i);
                walk(array.at(i), &items);
                m_place.pop();
            }
            break;
        }
        case QJsonValue::Object: {
            m_budget.charge(UnitsPerContainer);
            const QJsonObject object = value.toObject();
            TextReads members;
            for (auto it = object.begin(); it != object.end() && !m_report.isDone(); ++it) {
                const QAnyStringView name = it.keyView();
                m_place.pushName(name);
                m_budget.charge(memberUnits(name) + members.next(name)
                                + (isUtf16(name) ? textUnits(1, name.size()) : 0));
                if (!isValidText(name)) {
                    problem(QStringLiteral("the property name contains an unpaired UTF-16 surrogate"));
                }
                walk(it.value(), &members);
                m_place.pop();
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
        m_budget.charge(UnitsPerError + m_report.add(m_place, description));
    }

    Report& m_report;
    Budget& m_budget;
    Place m_place;
};

// The check of a value against the nodes of a compiled schema.
class Checker
{
public:
    Checker(const CompiledSchema& schema, Report& report, Budget& budget)
        : m_nodes(schema.nodes)
        , m_report(report)
        , m_budget(budget)
        , m_key(schema.key)
        , m_nodeBytes(qint64(schema.nodes.size() * sizeof(Node)))
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
    // An error at the current place. Kept out of the frames of the
    // recursion, as each check below that does not recurse is, since their
    // locals would grow every frame of a deep walk.
    Q_NEVER_INLINE void error(const QString& description)
    {
        const QString text = m_propertyName ? QStringLiteral("the property name ") + description : description;
        charge(UnitsPerError + m_report.add(m_place, text));
    }

    bool apply(int index, const QJsonValue& value, Mode mode, Evaluated* out, Shared* shared = nullptr);
    bool applyTo(TextReads& reads, int index, const QJsonValue& value, Mode mode);
    bool applyNode(const Node& node, const QJsonValue& value, Mode mode, Evaluated* out, Shared* shared);
    Q_NEVER_INLINE bool checkAssertions(const Node& node, const QJsonValue& value, Mode mode, Shared& shared);
    Q_NEVER_INLINE bool checkType(const Node& node, const QJsonValue& value, Mode mode, Shared& shared);
    Q_NEVER_INLINE void typeError(const Node& node, const QJsonValue& value, Shared& shared);
    Q_NEVER_INLINE bool checkLiterals(const Node& node, const QJsonValue& value, const QAnyStringView* text, Mode mode,
                                      Shared& shared);
    Q_NEVER_INLINE bool checkNumber(const Node& node, const Number& number, Mode mode);
    Q_NEVER_INLINE bool checkString(const Node& node, QAnyStringView text, Mode mode, Shared& shared);
    bool checkArray(const Node& node, const QJsonArray& array, Mode mode, Evaluated* evaluated);
    Q_NEVER_INLINE bool checkUnique(const QJsonArray& array, Mode mode);
    bool checkContains(const Node& node, const QJsonArray& array, Mode mode, Evaluated* evaluated);
    // Out of applyNode(), whose frame each subschema applied in place takes.
    Q_NEVER_INLINE bool checkObject(const Node& node, const QJsonValue& value, const QJsonObject& object, Mode mode,
                                    Evaluated* evaluated, Shared& shared);
    Q_NEVER_INLINE bool checkRequired(const Node& node, const QJsonObject& object, Mode mode, Shared& shared);
    bool checkName(int target, QAnyStringView name, Mode mode);
    bool checkInPlace(const Node& node, const QJsonValue& value, qsizetype size, Mode mode, Evaluated* evaluated,
                      Shared& shared);
    bool checkUnevaluatedItems(const Node& node, const QJsonArray& array, Mode mode, Evaluated& evaluated);
    bool checkUnevaluatedProperties(const Node& node, const QJsonObject& object, Mode mode, Evaluated& evaluated);

    const std::vector<Node>& m_nodes;
    Report& m_report;
    Budget& m_budget;
    Place m_place;
    // The value is a property name, which propertyNames checks: the lines
    // say so, and its text is m_name (see checkName()).
    bool m_propertyName = false;
    QAnyStringView m_name;
    // The reads of the container that the value applied to is an item or a
    // member of, if any (see applyTo()).
    TextReads* m_texts = nullptr;
    int m_depth = 0;
    // The frame of check(). The stack grows down on every Linux the library
    // builds for.
    quintptr m_stackBase = 0;
    // The key of the hashes (see checkUnique() and Shared).
    quint64 m_key;
    // The memory of the schema's nodes (see footprintUnits()).
    qint64 m_nodeBytes;
};

// Applies the node `index` to `value`. When `out` is not null, the members
// or items of `value` that the node evaluated are added to it. `shared`:
// what the subschemas applied to `value` in place read once, when this one
// is applied in place too.
bool Checker::apply(int index, const QJsonValue& value, Mode mode, Evaluated* out, Shared* shared)
{
    if (stopped()) {
        return false;
    }
    if (m_depth >= MaxCheckDepth || qint64(m_stackBase - quintptr(__builtin_frame_address(0))) > MaxCheckStackBytes) {
        m_budget.exceed();
        return false;
    }
    charge(UnitsPerApply + (shared ? UnitsPerInPlace : 0) + footprintUnits(m_nodeBytes + m_depth * StackBytesPerApply));
    ++m_depth;
    const Node& node = m_nodes[std::size_t(index)];
    bool valid = node.booleanValue;
    if (!node.isBoolean) {
        valid = applyNode(node, value, mode, out, shared);
    } else if (!valid && mode == Mode::Errors) {
        error(QStringLiteral("does not match the schema"));
    }
    --m_depth;
    return valid && !stopped();
}

// Applies the node `index` to an item or a member of a container, taken
// out of it: its text, when a keyword reads it, is read after the last one
// that `reads` saw.
bool Checker::applyTo(TextReads& reads, int index, const QJsonValue& value, Mode mode)
{
    if (value.isString()) {
        charge(UnitsPerReference);
    }
    TextReads* const outer = m_texts;
    m_texts = &reads;
    const bool valid = apply(index, value, mode, nullptr);
    m_texts = outer;
    return valid;
}

bool Checker::applyNode(const Node& node, const QJsonValue& value, Mode mode, Evaluated* out, Shared* shared)
{
    bool valid = true;
    // A keyword failed: true when the walk of this node ends there.
    const auto fail = [&]() {
        valid = false;
        return mode == Mode::Verdict || stopped();
    };

    // What the subschemas applied in place share: this node's own when it
    // is not applied in place itself.
    std::optional<Shared> own;
    if (!shared) {
        own.emplace(m_budget, m_key);
    }
    Shared& reads = shared ? *shared : *own;
    const QJsonValue::Type type = reads.type(value);
    const bool isObject = type == QJsonValue::Object;
    const bool isArray = type == QJsonValue::Array;
    // A value with no members or items takes nothing of a container's
    // (a QJsonObject or a QJsonArray made and destroyed is a call into Qt
    // each). One path for both, so that the subschemas applied in place
    // are applied from one place in this frame, which the recursion takes.
    std::optional<QJsonObject> object;
    std::optional<QJsonArray> array;
    qsizetype size = 0;
    if (isObject) {
        charge(UnitsPerContainer);
        object.emplace(value.toObject());
        size = object->size();
    } else if (isArray) {
        charge(UnitsPerContainer);
        array.emplace(value.toArray());
        size = array->size();
    }
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
        && !checkAssertions(node, value, mode, reads) && fail()) {
        return false;
    }
    if (isArray && !checkArray(node, *array, mode, evaluated) && fail()) {
        return false;
    }
    if (isObject && !checkObject(node, value, *object, mode, evaluated, reads) && fail()) {
        return false;
    }
    if (!checkInPlace(node, value, size, mode, evaluated, reads) && fail()) {
        return false;
    }
    if (isArray && node.unevaluatedItems >= 0 && !checkUnevaluatedItems(node, *array, mode, *evaluated) && fail()) {
        return false;
    }
    if (isObject && node.unevaluatedProperties >= 0 && !checkUnevaluatedProperties(node, *object, mode, *evaluated)
        && fail()) {
        return false;
    }
    return valid;
}

// The keywords that do not apply subschemas: type, const, enum, and those
// of numbers and strings.
bool Checker::checkAssertions(const Node& node, const QJsonValue& value, Mode mode, Shared& shared)
{
    bool valid = true;
    const auto fail = [&]() {
        valid = false;
        return mode == Mode::Verdict || stopped();
    };
    if (node.types != 0 && !checkType(node, value, mode, shared) && fail()) {
        return false;
    }
    // A string's text, read in place when a keyword reads it (a step), and
    // never decoded. Each keyword charges what it reads of it.
    const QJsonValue::Type type = shared.type(value);
    const bool isString = type == QJsonValue::String;
    QAnyStringView text;
    if (isString && (node.hasConst || node.hasEnum || node.minLength >= 0 || node.maxLength >= 0)) {
        // A name was read where its member was visited.
        text = m_propertyName ? m_name : value.toStringView();
        charge(UnitsPerStep + lineUnits(text) + (!m_propertyName && m_texts ? m_texts->next(text) : 0));
    }
    if ((node.hasConst || node.hasEnum) && !checkLiterals(node, value, isString ? &text : nullptr, mode, shared)
        && fail()) {
        return false;
    }
    if (type == QJsonValue::Double && node.numberKeywords != 0 && !checkNumber(node, shared.number(value), mode)
        && fail()) {
        return false;
    }
    if (isString && (node.minLength >= 0 || node.maxLength >= 0) && !checkString(node, text, mode, shared) && fail()) {
        return false;
    }
    return valid;
}

// A number matches "integer" when it has no fraction and fits a 64-bit
// integer, the range in which a handler reads it with toInteger(). 1e19 is
// an integer in 2020-12, but QJsonValue::toInteger() gives a handler 0 for
// it, so it is refused where the schema asks for an integer, and allowed
// where it asks for a number.
bool Checker::checkType(const Node& node, const QJsonValue& value, Mode mode, Shared& shared)
{
    charge(UnitsPerStep);
    quint8 bit = 0;
    switch (shared.type(value)) {
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
    case QJsonValue::Double:
        bit = shared.number(value).isInteger ? TypeNumber | TypeInteger : TypeNumber;
        break;
    }
    if ((node.types & bit) != 0) {
        return true;
    }
    if (mode == Mode::Errors) {
        typeError(node, value, shared);
    }
    return false;
}

// The line of a type that failed, kept out of checkType(), whose frame stays
// small.
void Checker::typeError(const Node& node, const QJsonValue& value, Shared& shared)
{
    const bool outsideInt64 =
        shared.type(value) == QJsonValue::Double && !shared.number(value).isInteger && isIntegral(shared.number(value));
    if (outsideInt64 && (node.types & TypeInteger) != 0) {
        error(QStringLiteral("must be from -9223372036854775808 to 9223372036854775807"));
    } else {
        error(QStringLiteral("must be of type ") + alternatives(node.typeNames));
    }
}

bool Checker::checkLiterals(const Node& node, const QJsonValue& value, const QAnyStringView* text, Mode mode,
                            Shared& shared)
{
    bool valid = true;
    // A number of the value read once, and compared with the numbers that
    // the compile read: a step for each.
    const bool isNumber = shared.type(value) == QJsonValue::Double;
    const Number number = isNumber ? shared.number(value) : Number();
    if (node.hasConst) {
        charge(UnitsPerStep);
        bool equal = false;
        if (text && node.constValue.isString()) {
            equal = sameText(*text, node.constString, node.constUtf8, m_budget);
        } else if (isNumber && node.constValue.isDouble()) {
            charge(UnitsPerStep);
            equal = compareNumbers(number, node.constNumber) == 0;
        } else {
            equal = equalValues(node.constValue, value, m_budget);
        }
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
            if (!node.enumTable.isEmpty()) {
                // A hash of the string, and a comparison with the entry.
                charge(lookupUnits(*text));
                found = node.enumTable.value(*text) >= 0;
            } else {
                for (qsizetype i = 0; i < node.enumStrings.size() && !found; ++i) {
                    charge(UnitsPerStep);
                    found = sameText(*text, node.enumStrings.at(i), node.enumUtf8.at(i), m_budget);
                }
            }
        } else if (isNumber) {
            for (std::size_t i = 0; i < node.enumNumbers.size() && !found && !stopped(); ++i) {
                charge(UnitsPerStep);
                found = compareNumbers(number, node.enumNumbers[i]) == 0;
            }
        } else {
            for (std::size_t i = 0; i < node.enumOthers.size() && !found && !stopped(); ++i) {
                found = equalValues(node.enumOthers[i], value, m_budget);
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

bool Checker::checkNumber(const Node& node, const Number& number, Mode mode)
{
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
// (an unpaired surrogate was refused before). At most one for each code
// unit, and at least one for every two of UTF-16 and for every four bytes
// of UTF-8 (each for one of Latin-1), so a string is counted only when that
// decides, once for all the subschemas applied to it in place.
bool Checker::checkString(const Node& node, QAnyStringView text, Mode mode, Shared& shared)
{
    const qsizetype units = text.size();
    const qsizetype least = text.visit([](auto view) -> qsizetype {
        using View = decltype(view);
        if constexpr (std::is_same_v<View, QStringView>) {
            return (view.size() + 1) / 2;
        } else if constexpr (std::is_same_v<View, QLatin1StringView>) {
            return view.size();
        } else {
            return (view.size() + 3) / 4;
        }
    });
    const auto count = [&]() {
        return shared.length(text);
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
    if (node.minLength >= 0 && least < node.minLength && count() < node.minLength) {
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

    TextReads texts;
    const qsizetype prefix = std::min(size, qsizetype(node.prefixItems.size()));
    for (qsizetype i = 0; i < prefix; ++i) {
        m_place.pushIndex(i);
        const bool ok = applyTo(texts, node.prefixItems[std::size_t(i)], array.at(i), mode);
        m_place.pop();
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
                    m_place.pushIndex(i);
                    error(description);
                    m_place.pop();
                }
                if (fail()) {
                    return false;
                }
            }
        } else {
            for (qsizetype i = prefix; i < size; ++i) {
                m_place.pushIndex(i);
                const bool ok = applyTo(texts, node.items, array.at(i), mode);
                m_place.pop();
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
    TextReads items;
    for (qsizetype i = 0; i < array.size(); ++i) {
        if (stopped()) {
            return false;
        }
        // The errors of an item that does not match are no errors of the
        // value.
        m_place.pushIndex(i);
        const bool ok = applyTo(items, node.contains, array.at(i), Mode::Verdict);
        m_place.pop();
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
// hash is keyed by the schema's key (Qt's global seed when it compiled),
// random in each process, so that a model cannot choose items whose hashes
// collide. The line names the first item that repeats an earlier one,
// whatever the hashes.
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
    TextReads items;
    for (qsizetype i = 0; i < size && !stopped(); ++i) {
        keys.push_back(Key{hashValue(array.at(i), m_key, m_budget, &items), i});
    }
    // A sort of plain integers, whose comparisons are counted here: some
    // five nanoseconds each, since the hashes are in no order and the
    // branches of the sort are taken at random.
    charge(qint64(size) * (1 + std::bit_width(quint64(size)) / ComparisonsPerUnit));
    if (stopped()) {
        m_budget.release(bytes);
        return false;
    }
    std::stable_sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) {
        return a.hash < b.hash;
    });

    // Within each run of one hash, a merge sort by value (stable, so equal
    // items stay in the order of their indices), which stops when the work
    // is spent, whatever the comparisons said. Each comparison reads two
    // items far apart in the array, in the order of their hashes: the place
    // of each in the array's storage, away from the last one read
    // (UnitsPerContainer), and its text away from the last text read of
    // the run, or its own storage. The runs come in the order of their
    // hashes, which the key decides, so the first text that a run reads is
    // charged as if it were anywhere in the array (MaxGapUnits), and each
    // run is compared to its end. So the work is the same whatever the key,
    // but for distinct items whose hashes are the same by chance.
    qsizetype firstRepeat = -1;
    qsizetype repeated = -1;
    std::vector<Key> scratch;
    std::size_t start = 0;
    while (start < keys.size() && !stopped()) {
        std::size_t end = start + 1;
        while (end < keys.size() && keys[end].hash == keys[start].hash) {
            ++end;
        }
        TextReads apart(MaxGapUnits);
        const auto equal = [&](qsizetype a, qsizetype b) {
            charge(2 * UnitsPerContainer);
            return equalValues(array.at(a), array.at(b), m_budget, &apart, &apart);
        };
        if (end - start > 1 && equal(keys[start].index, keys[start + 1].index)) {
            // The run is in the order of the indices: its first two items
            // are equal, and no repeat in it comes earlier.
            if (firstRepeat < 0 || keys[start + 1].index < firstRepeat) {
                firstRepeat = keys[start + 1].index;
                repeated = keys[start].index;
            }
        } else if (end - start > 2) {
            const auto less = [&](const Key& a, const Key& b) {
                charge(2 * UnitsPerContainer);
                return compareValues(array.at(a.index), array.at(b.index), m_budget, &apart, &apart) < 0;
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
                if (equal(keys[i - 1].index, keys[i].index)) {
                    // The first two of a group of equal items are its first
                    // item and its first repeat.
                    if (firstRepeat < 0 || keys[i].index < firstRepeat) {
                        firstRepeat = keys[i].index;
                        repeated = keys[i - 1].index;
                    }
                    while (i + 1 < end && equal(keys[i].index, keys[i + 1].index)) {
                        ++i;
                    }
                }
            }
        }
        start = end;
    }
    m_budget.release(bytes);
    if (stopped() || firstRepeat < 0) {
        return firstRepeat < 0 && !stopped();
    }
    if (mode == Mode::Errors) {
        error(QStringLiteral("must have unique items, and items %1 and %2 are equal")
                  .arg(QString::number(repeated), QString::number(firstRepeat)));
    }
    return false;
}

bool Checker::checkObject(const Node& node, const QJsonValue& value, const QJsonObject& object, Mode mode,
                          Evaluated* evaluated, Shared& shared)
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
    // against its schema in properties, or additionalProperties. The names
    // are read in place.
    if (!node.properties.isEmpty() || node.additionalProperties >= 0 || node.propertyNames >= 0) {
        const bool closed = node.additionalProperties >= 0 && m_nodes[std::size_t(node.additionalProperties)].isBoolean
            && !m_nodes[std::size_t(node.additionalProperties)].booleanValue;
        TextReads reads;
        qsizetype i = 0;
        for (auto it = object.begin(); it != object.end(); ++it, ++i) {
            if (stopped()) {
                return false;
            }
            const QAnyStringView name = it.keyView();
            m_place.pushName(name);
            charge(memberUnits(name) + reads.next(name));
            bool ok = node.propertyNames < 0 || checkName(node.propertyNames, name, mode);
            if (ok || mode == Mode::Errors) {
                int child = -1;
                if (!node.properties.isEmpty()) {
                    // A hash of the name, and a comparison with the entry.
                    charge(lookupUnits(name));
                    child = node.properties.value(name);
                }
                if (child >= 0) {
                    if (evaluated) {
                        evaluated->set(i);
                    }
                    ok = applyTo(reads, child, it.value(), mode) && ok;
                } else if (closed) {
                    if (mode == Mode::Errors) {
                        error(QStringLiteral("is not a known property"));
                    }
                    ok = false;
                } else if (node.additionalProperties >= 0) {
                    ok = applyTo(reads, node.additionalProperties, it.value(), mode) && ok;
                }
            }
            m_place.pop();
            if (!ok && fail()) {
                return false;
            }
        }
        if (evaluated && node.additionalProperties >= 0) {
            evaluated->setAll();
        }
    }

    if ((!node.required.isEmpty() || !node.dependentRequired.empty()) && !checkRequired(node, object, mode, shared)
        && fail()) {
        return false;
    }
    for (std::size_t k = 0; k < node.dependentSchemas.size(); ++k) {
        if (stopped()) {
            return false;
        }
        const auto& [trigger, schema] = node.dependentSchemas[k];
        if (!shared.contains(object, trigger, node.dependentSchemaHashes[k])) {
            continue;
        }
        Evaluated sub(m_budget, size);
        const bool ok = apply(schema, value, mode, evaluated ? &sub : nullptr, &shared);
        // The annotations of a subschema that failed are kept as well. With
        // Errors the value fails anyway, and they keep its members from
        // being named unknown too. With Verdict this node fails with it, and
        // whoever applied the node drops what it evaluated.
        if (evaluated) {
            evaluated->merge(sub);
        }
        if (!ok && fail()) {
            return false;
        }
    }
    return valid;
}

// required and dependentRequired: names looked up in the object's own
// sorted names (see Shared).
bool Checker::checkRequired(const Node& node, const QJsonObject& object, Mode mode, Shared& shared)
{
    bool valid = true;
    const auto fail = [&]() {
        valid = false;
        return mode == Mode::Verdict || stopped();
    };
    const qsizetype size = object.size();
    for (qsizetype k = 0; k < node.required.size(); ++k) {
        const QString& name = node.required.at(k);
        if (!shared.contains(object, name, node.requiredHashes[std::size_t(k)])) {
            if (stopped()) {
                return false;
            }
            if (mode == Mode::Errors) {
                m_place.pushName(name);
                error(QStringLiteral("is required"));
                m_place.pop();
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
                if (shared.contains(object, node.dependentRequired[i].first, node.dependentRequiredHashes[i].first)) {
                    triggered.push_back(int(i));
                }
            }
        } else {
            TextReads reads;
            for (auto it = object.begin(); it != object.end(); ++it) {
                const QAnyStringView name = it.keyView();
                // A hash of the name, and a comparison with the entry.
                charge(reads.next(name) + lookupUnits(name));
                const int i = node.dependentRequiredIndex.value(name);
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
        const std::vector<quint64>& hashes = node.dependentRequiredHashes[std::size_t(i)].second;
        for (qsizetype k = 0; k < names.size(); ++k) {
            const QString& name = names.at(k);
            if (!shared.contains(object, name, hashes[std::size_t(k)])) {
                if (stopped()) {
                    return false;
                }
                if (mode == Mode::Errors) {
                    m_place.pushName(name);
                    error(QStringLiteral("is required when \"%1\" is present").arg(shownName(trigger)));
                    m_place.pop();
                }
                if (fail()) {
                    return false;
                }
            }
        }
    }
    return valid;
}

bool Checker::checkName(int target, QAnyStringView name, Mode mode)
{
    const Node& node = m_nodes[std::size_t(target)];
    if (node.isBoolean) {
        if (!node.booleanValue && mode == Mode::Errors) {
            error(QStringLiteral("is not allowed, because the schema allows no property here"));
        }
        return node.booleanValue;
    }
    // The name is checked in place, never decoded. The subschema gets a
    // string that stands for it, and reads the name's text where it reads
    // a string's (see checkAssertions()). A string has no members or items,
    // so every subschema below sees the name.
    charge(UnitsPerStep);
    m_propertyName = true;
    m_name = name;
    const bool ok = apply(target, NameStandIn, mode, nullptr);
    m_propertyName = false;
    m_name = QAnyStringView();
    return ok;
}

// The subschemas that apply to the value itself. Those that must pass
// ($ref, allOf, then or else) add their annotations when they pass, and
// with Errors also when they fail, since the value fails then anyway. A
// branch of anyOf or oneOf adds them only when it passes, and not and the
// condition of if that failed never do.
bool Checker::checkInPlace(const Node& node, const QJsonValue& value, qsizetype size, Mode mode, Evaluated* evaluated,
                           Shared& shared)
{
    bool valid = true;
    const auto fail = [&]() {
        valid = false;
        return mode == Mode::Verdict || stopped();
    };
    const auto required = [&](int child) {
        if (!evaluated) {
            return apply(child, value, mode, nullptr, &shared);
        }
        Evaluated sub(m_budget, size);
        const bool ok = apply(child, value, mode, &sub, &shared);
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
            if (apply(branch, value, Mode::Verdict, evaluated ? &sub : nullptr, &shared)) {
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
            if (apply(branch, value, Mode::Verdict, evaluated ? &sub : nullptr, &shared)) {
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
        if (apply(node.notSchema, value, Mode::Verdict, nullptr, &shared)) {
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
        const bool holds = apply(node.ifSchema, value, Mode::Verdict, evaluated ? &condition : nullptr, &shared);
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
        TextReads items;
        for (qsizetype i = 0; i < array.size(); ++i) {
            if (stopped()) {
                return false;
            }
            charge(UnitsPerStep);
            if (evaluated.has(i)) {
                continue;
            }
            m_place.pushIndex(i);
            bool ok = true;
            if (closed) {
                if (mode == Mode::Errors) {
                    error(QStringLiteral("is not allowed, because the schema allows no more items"));
                }
                ok = false;
            } else {
                ok = applyTo(items, node.unevaluatedItems, array.at(i), mode);
            }
            m_place.pop();
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
        TextReads reads;
        qsizetype i = 0;
        for (auto it = object.begin(); it != object.end(); ++it, ++i) {
            if (stopped()) {
                return false;
            }
            charge(UnitsPerStep);
            if (evaluated.has(i)) {
                continue;
            }
            const QAnyStringView name = it.keyView();
            m_place.pushName(name);
            charge(memberUnits(name) + reads.next(name));
            bool ok = true;
            if (closed) {
                if (mode == Mode::Errors) {
                    error(QStringLiteral("is not a known property"));
                }
                ok = false;
            } else {
                ok = applyTo(reads, node.unevaluatedProperties, it.value(), mode);
            }
            m_place.pop();
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

qint64 footprintUnits(qint64 bytes)
{
    const qint64 steps = bytes / FootprintBytes;
    if (steps == 0) {
        return 0;
    }
    return std::min(MaxFootprintUnits, qint64(1 + (std::bit_width(quint64(steps)) - 1) / 2));
}

QStringList checkValue(const CompiledSchema& schema, const QJsonValue& value, CheckStats* stats)
{
    Budget budget;
    Report refused(Report::Kind::Precheck);
    Report report(Report::Kind::Check);
    QStringList lines;
    Precheck(refused, budget).walk(value);
    if (refused.isDone()) {
        lines = refused.take();
    } else if (budget.exceeded()) {
        lines = {TooLargeToCheck};
    } else if (!refused.isEmpty()) {
        lines = refused.take();
    } else {
        Checker checker(schema, report, budget);
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
