// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// Numbers, and the comparisons of JSON values that enum, const and
// uniqueItems make.

#include "mcpschemacheck.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QLocale>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace PhosphorMcp::Schema {

namespace {

// 2^63, the first double past qint64.
constexpr double TwoTo63 = 9223372036854775808.0;

// Charged for each this many UTF-16 code units of a string read, compared or
// hashed.
constexpr qint64 CharactersPerUnit = 16;

// The order of the JSON types in compareValues().
int typeRank(const QJsonValue& value)
{
    switch (value.type()) {
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
        int e = 0;
        const double fraction = std::frexp(std::fabs(value.real), &e);
        // 53 bits of mantissa, exactly.
        odd = quint64(std::ldexp(fraction, 53));
        exponent = e - 53;
    }
    while ((odd & 1) == 0) {
        odd >>= 1;
        ++exponent;
    }
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

quint64 hashNumber(const Number& number)
{
    if (number.isInteger) {
        return mix(2, quint64(number.integer));
    }
    quint64 bits = 0;
    static_assert(sizeof(bits) == sizeof(number.real));
    std::memcpy(&bits, &number.real, sizeof(bits));
    return mix(3, bits);
}

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

// As Python's jsonschema does it, which JavaScript validators do too: an
// integer divisor divides exactly, and a divisor with a fraction (or past
// qint64) divides as doubles, the quotient a whole number. Only when that
// quotient overflows is the test made exactly on the binary values.
bool isMultipleOf(const Number& value, const Number& divisor)
{
    if (divisor.isInteger) {
        if (value.isInteger) {
            return value.integer % divisor.integer == 0;
        }
        return std::fmod(value.real, double(divisor.integer)) == 0;
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

bool equalValues(const QJsonValue& a, const QJsonValue& b, Budget& budget)
{
    return equalValues(a, b, nullptr, budget);
}

bool equalValues(const QJsonValue& a, const QJsonValue& b, const QString* bText, Budget& budget)
{
    budget.charge(1);
    const int rank = typeRank(a);
    if (rank != typeRank(b)) {
        return false;
    }
    switch (a.type()) {
    case QJsonValue::Null:
    case QJsonValue::Undefined:
        return true;
    case QJsonValue::Bool:
        return a.toBool() == b.toBool();
    case QJsonValue::Double:
        return compareNumbers(numberOf(a), numberOf(b)) == 0;
    case QJsonValue::String: {
        const QString first = a.toString();
        const QString second = bText ? *bText : b.toString();
        // Each string is read whole, and compared when the lengths agree.
        budget.charge((first.size() + (bText ? 0 : second.size())) / CharactersPerUnit
                      + (first.size() == second.size() ? first.size() / CharactersPerUnit : 0));
        return first == second;
    }
    case QJsonValue::Array: {
        const QJsonArray x = a.toArray();
        const QJsonArray y = b.toArray();
        if (x.size() != y.size()) {
            return false;
        }
        for (qsizetype i = 0; i < x.size(); ++i) {
            if (budget.exceeded() || !equalValues(x.at(i), y.at(i), nullptr, budget)) {
                return false;
            }
        }
        return true;
    }
    case QJsonValue::Object: {
        // Qt keeps the members of an object sorted by name, so two objects
        // with the same members list them in the same order.
        const QJsonObject x = a.toObject();
        const QJsonObject y = b.toObject();
        if (x.size() != y.size()) {
            return false;
        }
        for (auto i = x.begin(), j = y.begin(); i != x.end(); ++i, ++j) {
            const QString first = i.key();
            const QString second = j.key();
            budget.charge(2 + (first.size() + second.size()) / CharactersPerUnit);
            if (budget.exceeded() || first != second || !equalValues(i.value(), j.value(), nullptr, budget)) {
                return false;
            }
        }
        return true;
    }
    }
    return false;
}

int compareValues(const QJsonValue& a, const QJsonValue& b, Budget& budget)
{
    budget.charge(1);
    const int rankA = typeRank(a);
    const int rankB = typeRank(b);
    if (rankA != rankB) {
        return rankA < rankB ? -1 : 1;
    }
    switch (a.type()) {
    case QJsonValue::Null:
    case QJsonValue::Undefined:
        return 0;
    case QJsonValue::Bool:
        return int(a.toBool()) - int(b.toBool());
    case QJsonValue::Double:
        return compareNumbers(numberOf(a), numberOf(b));
    case QJsonValue::String: {
        const QString first = a.toString();
        const QString second = b.toString();
        budget.charge((first.size() + second.size()) / CharactersPerUnit * 2);
        const int c = first.compare(second);
        return c < 0 ? -1 : (c > 0 ? 1 : 0);
    }
    case QJsonValue::Array: {
        const QJsonArray x = a.toArray();
        const QJsonArray y = b.toArray();
        const qsizetype n = std::min(x.size(), y.size());
        for (qsizetype i = 0; i < n && !budget.exceeded(); ++i) {
            const int c = compareValues(x.at(i), y.at(i), budget);
            if (c != 0) {
                return c;
            }
        }
        return x.size() < y.size() ? -1 : (x.size() > y.size() ? 1 : 0);
    }
    case QJsonValue::Object: {
        const QJsonObject x = a.toObject();
        const QJsonObject y = b.toObject();
        if (x.size() != y.size()) {
            return x.size() < y.size() ? -1 : 1;
        }
        for (auto i = x.begin(), j = y.begin(); i != x.end() && !budget.exceeded(); ++i, ++j) {
            const QString first = i.key();
            const QString second = j.key();
            budget.charge(2 + (first.size() + second.size()) / CharactersPerUnit * 2);
            const int k = first.compare(second);
            if (k != 0) {
                return k < 0 ? -1 : 1;
            }
            const int c = compareValues(i.value(), j.value(), budget);
            if (c != 0) {
                return c;
            }
        }
        return 0;
    }
    }
    return 0;
}

quint64 hashValue(const QJsonValue& value, Budget& budget)
{
    budget.charge(1);
    switch (value.type()) {
    case QJsonValue::Null:
    case QJsonValue::Undefined:
        return 0x6e756c6cULL;
    case QJsonValue::Bool:
        return value.toBool() ? 0x74727565ULL : 0x66616c73ULL;
    case QJsonValue::Double:
        return hashNumber(numberOf(value));
    case QJsonValue::String: {
        const QString text = value.toString();
        budget.charge(text.size() / CharactersPerUnit * 2);
        return mix(4, qHash(text, 0));
    }
    case QJsonValue::Array: {
        quint64 hash = 5;
        const QJsonArray array = value.toArray();
        for (qsizetype i = 0; i < array.size() && !budget.exceeded(); ++i) {
            hash = mix(hash, hashValue(array.at(i), budget));
        }
        return mix(hash, quint64(array.size()));
    }
    case QJsonValue::Object: {
        quint64 hash = 6;
        const QJsonObject object = value.toObject();
        for (auto it = object.begin(); it != object.end() && !budget.exceeded(); ++it) {
            const QString key = it.key();
            budget.charge(2 + key.size() / CharactersPerUnit * 2);
            hash = mix(hash, qHash(key, 0));
            hash = mix(hash, hashValue(it.value(), budget));
        }
        return mix(hash, quint64(object.size()));
    }
    }
    return 0;
}

} // namespace PhosphorMcp::Schema
