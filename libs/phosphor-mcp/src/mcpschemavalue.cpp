// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// Numbers, and the comparisons of JSON values that enum, const and
// uniqueItems make.

#include "mcpschemacheck.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QLocale>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <type_traits>

namespace PhosphorMcp::Schema {

namespace {

// 2^63, the first double past qint64.
constexpr double TwoTo63 = 9223372036854775808.0;

// The order of the JSON types in compareValues().
int typeRank(QJsonValue::Type type)
{
    switch (type) {
    case QJsonValue::Null:
    case QJsonValue::Undefined:
        return 0;
    case QJsonValue::Bool:
        return 1;
    case QJsonValue::Double:
        return 2;
    case QJsonValue::String:
        return 3;
    case QJsonValue::Array:
        return 4;
    case QJsonValue::Object:
        return 5;
    }
    return 0;
}

// |value| as an odd integer times a power of two (value != 0).
void oddAndExponent(const Number& value, quint64& odd, int& exponent)
{
    if (value.isInteger) {
        // The magnitude of qint64's minimum is 2^63, which quint64 holds.
        odd = value.integer < 0 ? quint64(0) - quint64(value.integer) : quint64(value.integer);
        exponent = 0;
    } else {
        // The bits of the double. A subnormal has no hidden bit, and the
        // exponent of the least normal.
        quint64 bits = 0;
        static_assert(sizeof(bits) == sizeof(value.real));
        std::memcpy(&bits, &value.real, sizeof(bits));
        const int biased = int((bits >> 52) & 0x7FF);
        odd = bits & ((quint64(1) << 52) - 1);
        if (biased == 0) {
            exponent = -1074;
        } else {
            odd |= quint64(1) << 52;
            exponent = biased - 1075;
        }
    }
    const int zeros = std::countr_zero(odd);
    odd >>= zeros;
    exponent += zeros;
}

// Whether value / divisor is an integer, exactly, for the binary values as
// they are. value = V * 2^v and divisor = D * 2^d with V and D odd: the
// quotient is (V / D) * 2^(v - d), an integer when D divides V and v >= d.
bool isExactMultiple(const Number& value, const Number& divisor)
{
    if (value.isInteger ? value.integer == 0 : value.real == 0) {
        return true;
    }
    quint64 valueOdd = 0;
    int valueExponent = 0;
    quint64 divisorOdd = 0;
    int divisorExponent = 0;
    oddAndExponent(value, valueOdd, valueExponent);
    oddAndExponent(divisor, divisorOdd, divisorExponent);
    return valueOdd % divisorOdd == 0 && valueExponent >= divisorExponent;
}

quint64 mix(quint64 hash, quint64 value)
{
    // splitmix64's finalizer over the running hash.
    quint64 x = hash ^ (value + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2));
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

// The start of the hash of each kind of value, from the key: values of two
// kinds (an integer and a double, say) collide only by chance, whatever
// their bits, since their starts are unknown without the key.
enum Kind : quint64 {
    KindNull = 1,
    KindBoolean,
    KindInteger,
    KindDouble,
    KindString,
    KindArray,
    KindObject,
    KindName,
};

quint64 start(quint64 key, Kind kind)
{
    return mix(key, kind);
}

quint64 hashNumber(const Number& number, quint64 key)
{
    if (number.isInteger) {
        return mix(start(key, KindInteger), quint64(number.integer));
    }
    quint64 bits = 0;
    static_assert(sizeof(bits) == sizeof(number.real));
    std::memcpy(&bits, &number.real, sizeof(bits));
    return mix(start(key, KindDouble), bits);
}

// Calls `f` with each code point of `text`. Qt's UTF-8 is valid (its parser
// refuses any other), and an unpaired surrogate of UTF-16 is passed as it
// is.
template<typename F>
void forEachCodePoint(QAnyStringView text, F&& f)
{
    text.visit([&f](auto view) {
        using View = decltype(view);
        if constexpr (std::is_same_v<View, QStringView>) {
            const qsizetype size = view.size();
            for (qsizetype i = 0; i < size; ++i) {
                const char16_t c = view.utf16()[i];
                if (QChar::isHighSurrogate(c) && i + 1 < size && QChar::isLowSurrogate(view.utf16()[i + 1])) {
                    f(QChar::surrogateToUcs4(c, view.utf16()[i + 1]));
                    ++i;
                } else {
                    f(char32_t(c));
                }
            }
        } else if constexpr (std::is_same_v<View, QLatin1StringView>) {
            for (const char c : view) {
                f(char32_t(uchar(c)));
            }
        } else {
            const auto* bytes = reinterpret_cast<const uchar*>(view.data());
            const qsizetype size = view.size();
            for (qsizetype i = 0; i < size;) {
                const uchar lead = bytes[i];
                const int length = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
                char32_t c = length == 1 ? lead : lead & (0x7F >> length);
                for (int k = 1; k < length && i + k < size; ++k) {
                    c = (c << 6) | (bytes[i + k] & 0x3F);
                }
                f(c);
                i += length;
            }
        }
    });
}

// The code points of a text one after the other, for a comparison of two
// texts of different encodings.
template<typename View>
class CodePointReader
{
public:
    explicit CodePointReader(View text)
        : m_text(text)
    {
    }
    // The next code point, or -1 at the end.
    qint32 next()
    {
        const qsizetype size = m_text.size();
        if (m_position >= size) {
            return -1;
        }
        if constexpr (std::is_same_v<View, QStringView>) {
            const char16_t c = m_text.utf16()[m_position++];
            if (QChar::isHighSurrogate(c) && m_position < size && QChar::isLowSurrogate(m_text.utf16()[m_position])) {
                return qint32(QChar::surrogateToUcs4(c, m_text.utf16()[m_position++]));
            }
            return c;
        } else if constexpr (std::is_same_v<View, QLatin1StringView>) {
            return uchar(m_text.data()[m_position++]);
        } else {
            const auto* bytes = reinterpret_cast<const uchar*>(m_text.data());
            const uchar lead = bytes[m_position];
            const int length = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
            qint32 c = length == 1 ? lead : lead & (0x7F >> length);
            for (int k = 1; k < length && m_position + k < size; ++k) {
                c = (c << 6) | (bytes[m_position + k] & 0x3F);
            }
            m_position += length;
            return c;
        }
    }

private:
    View m_text;
    qsizetype m_position = 0;
};

// The code point order of two UTF-16 code units that differ: a surrogate
// (U+D800 to U+DFFF, the half of a code point past U+FFFF) comes after
// U+E000 to U+FFFF.
int compareCodeUnits(char16_t a, char16_t b)
{
    const auto fix = [](char16_t c) {
        return c >= 0xE000 ? c - 0x800 : (c >= 0xD800 ? c + 0x2000 : c);
    };
    return fix(a) < fix(b) ? -1 : 1;
}

int compareBytes(const char* a, qsizetype aSize, const char* b, qsizetype bSize)
{
    const int c = std::memcmp(a, b, std::size_t(std::min(aSize, bSize)));
    if (c != 0) {
        return c < 0 ? -1 : 1;
    }
    return aSize < bSize ? -1 : (aSize > bSize ? 1 : 0);
}

// The members of an object in the order of their names' code points, for
// the comparisons that may not take Qt's order. The memory is counted.
class SortedMembers
{
public:
    SortedMembers(const QJsonObject& object, Budget& budget)
        : m_budget(budget)
        , m_bytes(qint64(object.size()) * qint64(sizeof(Member)))
    {
        if (!m_budget.allocate(m_bytes)) {
            return;
        }
        const qint64 comparisons = std::bit_width(quint64(object.size()));
        qint64 units = 0;
        m_members.reserve(std::size_t(object.size()));
        // A name away from the others is read away from them by each
        // comparison of the sort too.
        TextReads reads;
        for (auto it = object.begin(); it != object.end(); ++it) {
            const QAnyStringView name = it.keyView();
            m_members.push_back(Member{name, it});
            const qint64 gap = reads.next(name);
            units += UnitsPerMember + gap + comparisons * (1 + gap + CompareUnits * (name.size() / CharactersPerUnit));
        }
        if (!m_budget.charge(units)) {
            return;
        }
        std::sort(m_members.begin(), m_members.end(), [](const Member& a, const Member& b) {
            return compareText(a.name, b.name) < 0;
        });
        m_ready = true;
    }
    ~SortedMembers()
    {
        m_budget.release(m_bytes);
    }
    SortedMembers(const SortedMembers&) = delete;
    SortedMembers& operator=(const SortedMembers&) = delete;

    // False when the budget was spent before the members were sorted.
    [[nodiscard]] bool isReady() const
    {
        return m_ready;
    }
    [[nodiscard]] QAnyStringView name(std::size_t i) const
    {
        return m_members[i].name;
    }
    [[nodiscard]] QJsonValue value(std::size_t i) const
    {
        return m_members[i].at.value();
    }

private:
    struct Member
    {
        QAnyStringView name;
        QJsonObject::const_iterator at;
    };
    Budget& m_budget;
    qint64 m_bytes;
    std::vector<Member> m_members;
    bool m_ready = false;
};

} // namespace

Number numberOf(const QJsonValue& value)
{
    Number number;
    // toInteger() gives the default unless the value is a whole number that
    // qint64 holds.
    const qint64 a = value.toInteger(0);
    if (a == value.toInteger(1)) {
        number.integer = a;
        return number;
    }
    number.isInteger = false;
    number.real = value.toDouble();
    return number;
}

int compareNumbers(const Number& a, const Number& b)
{
    if (a.isInteger && b.isInteger) {
        return a.integer < b.integer ? -1 : (a.integer > b.integer ? 1 : 0);
    }
    if (!a.isInteger && !b.isInteger) {
        return a.real < b.real ? -1 : (a.real > b.real ? 1 : 0);
    }
    if (!a.isInteger) {
        return -compareNumbers(b, a);
    }
    // An integer against a double, exactly. A double from -2^63 up to (not
    // including) 2^63 has an integral part that qint64 holds.
    const double d = b.real;
    if (d >= TwoTo63) {
        return -1;
    }
    if (d < -TwoTo63) {
        return 1;
    }
    const double whole = std::trunc(d);
    const qint64 t = qint64(whole);
    if (a.integer != t) {
        return a.integer < t ? -1 : 1;
    }
    const double fraction = d - whole;
    return fraction > 0 ? -1 : (fraction < 0 ? 1 : 0);
}

bool isIntegral(const Number& number)
{
    return number.isInteger || number.real == std::trunc(number.real);
}

// An integral divisor divides exactly, whatever the value. A divisor with a
// fraction divides as doubles, as Python's jsonschema and JavaScript
// validators do, the quotient a whole number. Only when that quotient
// overflows is the test made exactly on the binary values.
bool isMultipleOf(const Number& value, const Number& divisor)
{
    if (isIntegral(divisor)) {
        if (value.isInteger && divisor.isInteger) {
            return value.integer % divisor.integer == 0;
        }
        // A double, or a divisor past qint64, tested on the binary values
        // rather than by std::fmod(), which takes a step for each bit
        // between the exponents (some 90 ns for 1e308 by 3).
        return isExactMultiple(value, divisor);
    }
    const double quotient = value.toDouble() / divisor.real;
    if (std::isfinite(quotient)) {
        return quotient == std::trunc(quotient);
    }
    return isExactMultiple(value, divisor);
}

QString formatNumber(const Number& number)
{
    if (number.isInteger) {
        return QString::number(number.integer);
    }
    // An integral double past qint64 with all its digits (2^63 is
    // 9223372036854775808, not 9223372036854776000), up to where the
    // shortest form uses an exponent anyway.
    if (number.real == std::trunc(number.real) && std::fabs(number.real) < 1e21) {
        return QString::number(number.real, 'f', 0);
    }
    return QString::number(number.real, 'g', QLocale::FloatingPointShortest);
}

qsizetype codePoints(QAnyStringView text)
{
    return text.visit([](auto view) -> qsizetype {
        using View = decltype(view);
        qsizetype length = 0;
        if constexpr (std::is_same_v<View, QStringView>) {
            const char16_t* units = view.utf16();
            for (qsizetype i = 0; i < view.size(); ++i) {
                length += QChar::isLowSurrogate(units[i]) ? 0 : 1;
            }
        } else if constexpr (std::is_same_v<View, QLatin1StringView>) {
            length = view.size();
        } else {
            // Each code point has one byte that is not a continuation.
            for (const auto c : view) {
                length += (uchar(c) & 0xC0) == 0x80 ? 0 : 1;
            }
        }
        return length;
    });
}

bool isValidText(QAnyStringView text)
{
    return text.visit([](auto view) {
        if constexpr (std::is_same_v<decltype(view), QStringView>) {
            return view.isValidUtf16();
        } else {
            return true;
        }
    });
}

bool isUtf16(QAnyStringView text)
{
    return text.visit([](auto view) {
        return std::is_same_v<decltype(view), QStringView>;
    });
}

int compareText(QAnyStringView a, QAnyStringView b)
{
    return a.visit([&](auto x) {
        return b.visit([&](auto y) {
            using X = decltype(x);
            using Y = decltype(y);
            // x.utf16() sits under a test of X alone, which a compiler
            // settles for each X before it knows Y (clang checks the
            // branches that wait on Y with X known).
            if constexpr (std::is_same_v<X, Y>) {
                if constexpr (std::is_same_v<X, QStringView>) {
                    // UTF-16 in order is code points in order, but for the
                    // surrogates.
                    const qsizetype n = std::min(x.size(), y.size());
                    for (qsizetype i = 0; i < n; ++i) {
                        if (x.utf16()[i] != y.utf16()[i]) {
                            return compareCodeUnits(x.utf16()[i], y.utf16()[i]);
                        }
                    }
                    return x.size() < y.size() ? -1 : (x.size() > y.size() ? 1 : 0);
                } else {
                    // Latin-1 or UTF-8: bytes in order are code points in
                    // order.
                    return compareBytes(reinterpret_cast<const char*>(x.data()), x.size(),
                                        reinterpret_cast<const char*>(y.data()), y.size());
                }
            } else {
                CodePointReader<X> first(x);
                CodePointReader<Y> second(y);
                for (;;) {
                    const qint32 c = first.next();
                    const qint32 d = second.next();
                    if (c != d) {
                        return c < d ? -1 : 1;
                    }
                    if (c < 0) {
                        return 0;
                    }
                }
            }
        });
    });
}

quint64 hashText(QAnyStringView text, quint64 key)
{
    // Three code points of 21 bits to each word that is mixed in.
    quint64 hash = key;
    quint64 word = 0;
    int count = 0;
    qint64 total = 0;
    forEachCodePoint(text, [&](char32_t c) {
        word = (word << 21) | quint64(c);
        if (++count == 3) {
            hash = mix(hash, word);
            word = 0;
            count = 0;
        }
        ++total;
    });
    return mix(mix(hash, word), quint64(total));
}

bool isUtf8(QAnyStringView text)
{
    return text.visit([](auto view) {
        return !std::is_same_v<decltype(view), QStringView> && !std::is_same_v<decltype(view), QLatin1StringView>;
    });
}

qint64 hashUnits(QAnyStringView text)
{
    return HashUnits * (text.size() / CharactersPerUnit);
}

qint64 compareUnits(QAnyStringView a, QAnyStringView b)
{
    return CompareUnits * ((a.size() + b.size()) / CharactersPerUnit);
}

qint64 equalUnits(QAnyStringView a, QAnyStringView b)
{
    return (isUtf8(a) == isUtf8(b) ? 1 : DecodeUnits) * ((a.size() + b.size()) / CharactersPerUnit);
}

qint64 decodeUnits(QAnyStringView text)
{
    return (isUtf8(text) ? DecodeUnits : 1) * (text.size() / CharactersPerUnit);
}

int NameTable::value(QAnyStringView name) const
{
    return name.visit([this](auto view) -> int {
        using View = decltype(view);
        if constexpr (std::is_same_v<View, QStringView>) {
            return m_utf16.value(QString::fromRawData(view.data(), view.size()), -1);
        } else if constexpr (std::is_same_v<View, QLatin1StringView>) {
            // Qt keeps text in Latin-1 only when it is ASCII, which is UTF-8
            // too. Other Latin-1, which no QJsonValue holds, is looked up by
            // UTF-16, so that the table is right for any view.
            uchar high = 0;
            for (const char c : view) {
                high |= uchar(c);
            }
            if (high < 0x80) {
                return m_utf8.value(QByteArray::fromRawData(view.data(), view.size()), -1);
            }
            return m_utf16.value(QString(view), -1);
        } else {
            return m_utf8.value(QByteArray::fromRawData(reinterpret_cast<const char*>(view.data()), view.size()), -1);
        }
    });
}

namespace {

// A text read in its container, if it is in one.
qint64 readUnits(TextReads* reads, QAnyStringView text)
{
    return reads ? reads->next(text) : 0;
}

// A value of the type `type` taken out of its container, if it was: a
// QJsonValue made of it, which holds a reference to storage when it is a
// string, an object or an array.
qint64 takenUnits(TextReads* reads, QJsonValue::Type type)
{
    if (!reads) {
        return 0;
    }
    const bool referenced = type == QJsonValue::String || type == QJsonValue::Array || type == QJsonValue::Object;
    return UnitsPerTaken + (referenced ? UnitsPerReference : 0);
}

// The comparisons and the hash below, with the reads of the containers that
// the values are items or members of, when they are (see TextReads).
bool equalIn(const QJsonValue& a, const QJsonValue& b, Budget& budget, TextReads* readsA, TextReads* readsB)
{
    // Each type read once: QJsonValue::type() is a call into Qt.
    const QJsonValue::Type type = a.type();
    const QJsonValue::Type other = b.type();
    budget.charge(1 + takenUnits(readsA, type) + takenUnits(readsB, other));
    if (typeRank(type) != typeRank(other)) {
        return false;
    }
    switch (type) {
    case QJsonValue::Null:
    case QJsonValue::Undefined:
        return true;
    case QJsonValue::Bool:
        return a.toBool() == b.toBool();
    case QJsonValue::Double:
        return compareNumbers(numberOf(a), numberOf(b)) == 0;
    case QJsonValue::String: {
        const QAnyStringView first = a.toStringView();
        const QAnyStringView second = b.toStringView();
        budget.charge(readUnits(readsA, first) + readUnits(readsB, second) + equalUnits(first, second));
        return QAnyStringView::equal(first, second);
    }
    case QJsonValue::Array: {
        // Read through their own storage, as an array applied to is.
        budget.charge(2 * UnitsPerContainer);
        const QJsonArray x = a.toArray();
        const QJsonArray y = b.toArray();
        if (x.size() != y.size()) {
            return false;
        }
        TextReads itemsX;
        TextReads itemsY;
        for (qsizetype i = 0; i < x.size(); ++i) {
            if (budget.exceeded() || !equalIn(x.at(i), y.at(i), budget, &itemsX, &itemsY)) {
                return false;
            }
        }
        return true;
    }
    case QJsonValue::Object: {
        budget.charge(2 * UnitsPerContainer);
        const QJsonObject x = a.toObject();
        const QJsonObject y = b.toObject();
        if (x.size() != y.size()) {
            return false;
        }
        // Most often both keep their members in the same order. When a
        // name differs there, the members are sorted first.
        bool sameOrder = true;
        TextReads readsX;
        TextReads readsY;
        for (auto i = x.begin(), j = y.begin(); i != x.end(); ++i, ++j) {
            const QAnyStringView first = i.keyView();
            const QAnyStringView second = j.keyView();
            budget.charge(UnitsPerMember + readsX.next(first) + readsY.next(second) + equalUnits(first, second));
            if (budget.exceeded()) {
                return false;
            }
            if (!QAnyStringView::equal(first, second)) {
                sameOrder = false;
                break;
            }
            if (!equalIn(i.value(), j.value(), budget, &readsX, &readsY)) {
                return false;
            }
        }
        if (sameOrder) {
            return true;
        }
        const SortedMembers first(x, budget);
        const SortedMembers second(y, budget);
        if (!first.isReady() || !second.isReady()) {
            return false;
        }
        TextReads sortedX;
        TextReads sortedY;
        for (std::size_t i = 0; i < std::size_t(x.size()); ++i) {
            budget.charge(UnitsPerMember + sortedX.next(first.name(i)) + sortedY.next(second.name(i))
                          + equalUnits(first.name(i), second.name(i)));
            if (budget.exceeded() || !QAnyStringView::equal(first.name(i), second.name(i))
                || !equalIn(first.value(i), second.value(i), budget, &sortedX, &sortedY)) {
                return false;
            }
        }
        return true;
    }
    }
    return false;
}

int compareIn(const QJsonValue& a, const QJsonValue& b, Budget& budget, TextReads* readsA, TextReads* readsB)
{
    const QJsonValue::Type type = a.type();
    const QJsonValue::Type other = b.type();
    budget.charge(1 + takenUnits(readsA, type) + takenUnits(readsB, other));
    const int rankA = typeRank(type);
    const int rankB = typeRank(other);
    if (rankA != rankB) {
        return rankA < rankB ? -1 : 1;
    }
    switch (type) {
    case QJsonValue::Null:
    case QJsonValue::Undefined:
        return 0;
    case QJsonValue::Bool:
        return int(a.toBool()) - int(b.toBool());
    case QJsonValue::Double:
        return compareNumbers(numberOf(a), numberOf(b));
    case QJsonValue::String: {
        const QAnyStringView first = a.toStringView();
        const QAnyStringView second = b.toStringView();
        budget.charge(readUnits(readsA, first) + readUnits(readsB, second) + compareUnits(first, second));
        return compareText(first, second);
    }
    case QJsonValue::Array: {
        budget.charge(2 * UnitsPerContainer);
        const QJsonArray x = a.toArray();
        const QJsonArray y = b.toArray();
        const qsizetype n = std::min(x.size(), y.size());
        TextReads itemsX;
        TextReads itemsY;
        for (qsizetype i = 0; i < n && !budget.exceeded(); ++i) {
            const int c = compareIn(x.at(i), y.at(i), budget, &itemsX, &itemsY);
            if (c != 0) {
                return c;
            }
        }
        return x.size() < y.size() ? -1 : (x.size() > y.size() ? 1 : 0);
    }
    case QJsonValue::Object: {
        budget.charge(2 * UnitsPerContainer);
        const QJsonObject x = a.toObject();
        const QJsonObject y = b.toObject();
        if (x.size() != y.size()) {
            return x.size() < y.size() ? -1 : 1;
        }
        // Member by member in the order of their names, whatever order Qt
        // keeps them in.
        const SortedMembers first(x, budget);
        const SortedMembers second(y, budget);
        if (!first.isReady() || !second.isReady()) {
            return 0;
        }
        TextReads readsX;
        TextReads readsY;
        for (std::size_t i = 0; i < std::size_t(x.size()) && !budget.exceeded(); ++i) {
            budget.charge(UnitsPerMember + readsX.next(first.name(i)) + readsY.next(second.name(i))
                          + compareUnits(first.name(i), second.name(i)));
            const int k = compareText(first.name(i), second.name(i));
            if (k != 0) {
                return k;
            }
            const int c = compareIn(first.value(i), second.value(i), budget, &readsX, &readsY);
            if (c != 0) {
                return c;
            }
        }
        return 0;
    }
    }
    return 0;
}

quint64 hashIn(const QJsonValue& value, quint64 key, Budget& budget, TextReads* reads)
{
    const QJsonValue::Type type = value.type();
    budget.charge(1 + takenUnits(reads, type));
    switch (type) {
    case QJsonValue::Null:
    case QJsonValue::Undefined:
        return start(key, KindNull);
    case QJsonValue::Bool:
        return mix(start(key, KindBoolean), value.toBool() ? 1 : 0);
    case QJsonValue::Double:
        return hashNumber(numberOf(value), key);
    case QJsonValue::String: {
        const QAnyStringView text = value.toStringView();
        budget.charge(readUnits(reads, text) + hashUnits(text));
        return hashText(text, start(key, KindString));
    }
    case QJsonValue::Array: {
        budget.charge(UnitsPerContainer);
        quint64 hash = start(key, KindArray);
        const QJsonArray array = value.toArray();
        TextReads items;
        for (qsizetype i = 0; i < array.size() && !budget.exceeded(); ++i) {
            hash = mix(hash, hashIn(array.at(i), key, budget, &items));
        }
        return mix(hash, quint64(array.size()));
    }
    case QJsonValue::Object: {
        // A sum of the hashes of the members, which does not depend on
        // their order.
        budget.charge(UnitsPerContainer);
        quint64 sum = 0;
        const QJsonObject object = value.toObject();
        const quint64 names = start(key, KindName);
        TextReads members;
        for (auto it = object.begin(); it != object.end() && !budget.exceeded(); ++it) {
            const QAnyStringView name = it.keyView();
            budget.charge(UnitsPerMember + members.next(name) + hashUnits(name));
            sum += mix(hashText(name, names), hashIn(it.value(), key, budget, &members));
        }
        return mix(mix(start(key, KindObject), sum), quint64(object.size()));
    }
    }
    return 0;
}

} // namespace

bool equalValues(const QJsonValue& a, const QJsonValue& b, Budget& budget, TextReads* readsA, TextReads* readsB)
{
    return equalIn(a, b, budget, readsA, readsB);
}

int compareValues(const QJsonValue& a, const QJsonValue& b, Budget& budget, TextReads* readsA, TextReads* readsB)
{
    return compareIn(a, b, budget, readsA, readsB);
}

quint64 hashValue(const QJsonValue& value, quint64 key, Budget& budget, TextReads* reads)
{
    return hashIn(value, key, budget, reads);
}

} // namespace PhosphorMcp::Schema
