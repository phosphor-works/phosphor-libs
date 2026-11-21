// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

// The check of a value against a compiled schema, its bound, and the list of
// its errors. See mcpschemanode.h for the compiled schema.

#include "mcpschemanode.h"

#include <QSet>
#include <QString>
#include <QStringList>

#include <vector>

namespace PhosphorMcp::Schema {

// The bound of a check (see Budget).
//
// Work is counted in units, about 10 to 25 ns each of a release build,
// whatever the schema and the value: the checker charges each step it takes
// (a value walked, a subschema applied to a value, a keyword checked, a
// member or an item visited, a name looked up, sixteen characters of a
// string read, compared or hashed, a word of an annotation set merged, an
// error built), weighted by what it costs. The check stops the moment the
// count passes MaxWork, about a second. The README has the measured times.
constexpr qint64 MaxWork = 40'000'000;
// The memory the check holds beyond the value and the schema: the sets of
// the members and items that subschemas evaluated (for unevaluatedProperties
// and unevaluatedItems), the keys that uniqueItems sorts, and the errors
// kept to tell them apart.
constexpr qint64 MaxMemory = 64 * 1024 * 1024;
// Subschemas applied inside each other, each a few frames of the checker's
// recursion on the stack: about 300 bytes each in a release build, as
// measured, and three times that in a debug build with sanitizers...
constexpr int MaxCheckDepth = 3000;
// ...and the stack that the check itself takes, measured from the addresses
// of its frames, whatever the build: half of the 8 MiB that a Linux thread
// has by default.
constexpr qint64 MaxCheckStackBytes = 4 * 1024 * 1024;

// Values nested deeper than this are refused before any check. Qt's JSON
// parser takes no more than 1024 levels in a whole message, so arguments
// from the wire never reach it. It bounds the recursion of the walks for a
// structured content that a handler built.
constexpr int MaxNestingDepth = 1024;

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

// The work and memory of a check, counted as it goes.
class Budget
{
public:
    // Counts `units` of work. False once the work passed MaxWork.
    bool charge(qint64 units)
    {
        m_work += units;
        if (m_work > MaxWork) {
            m_exceeded = true;
        }
        return !m_exceeded;
    }
    // Counts `bytes` more of memory held. False once it passed MaxMemory.
    bool allocate(qint64 bytes)
    {
        m_memory += bytes;
        if (m_memory > m_peak) {
            m_peak = m_memory;
        }
        if (m_memory > MaxMemory) {
            m_exceeded = true;
        }
        return !m_exceeded;
    }
    void release(qint64 bytes)
    {
        m_memory -= bytes;
    }
    void exceed()
    {
        m_exceeded = true;
    }
    [[nodiscard]] bool exceeded() const
    {
        return m_exceeded;
    }
    [[nodiscard]] qint64 work() const
    {
        return m_work;
    }
    [[nodiscard]] qint64 peakMemory() const
    {
        return m_peak;
    }

private:
    qint64 m_work = 0;
    qint64 m_memory = 0;
    qint64 m_peak = 0;
    bool m_exceeded = false;
};

// Comparisons of JSON values as JSON Schema 2020-12 makes them (enum, const,
// uniqueItems): numbers by their value, 1 and 1.0 alike, a boolean never
// equal to a number, strings by their code units, arrays item by item and
// objects member by member, whatever the order of their members. Each
// charges the budget for its work.
bool equalValues(const QJsonValue& a, const QJsonValue& b, Budget& budget);
// The same, with the string of `b` read already (when `b` is a string).
bool equalValues(const QJsonValue& a, const QJsonValue& b, const QString* bText, Budget& budget);
// A total order of JSON values that is 0 exactly when equalValues() is true.
int compareValues(const QJsonValue& a, const QJsonValue& b, Budget& budget);
// A hash that is the same for values that equalValues() finds equal.
quint64 hashValue(const QJsonValue& value, Budget& budget);

// A place in a value: a member name, or an item's index.
struct PathToken
{
    QString name;
    qsizetype index = -1;
};
using Path = std::vector<PathToken>;

// RFC 6901: "~" and "/" in a member name are escaped.
QString pointerToken(const QString& name);
// A name as an error shows it: an unpaired surrogate replaced so that the
// reply stays valid UTF-8, and cut short.
QString shownName(const QString& name);

// The errors that validate() returns: each once, at most MaxListedErrors of
// them, and a last line that counts the rest.
class Report
{
public:
    // "<pointer>: <description>" for the place `path` (and `last` below it,
    // when not null), unless it was added before. The test is on the whole
    // pointer, not on the line, which cuts names: two long names alike in
    // their first 61 code units are two errors. Returns the code units of
    // the pointer, for the budget.
    qsizetype add(const Path& path, const QString* last, const QString& description);
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
    QStringList take();

private:
    QStringList m_lines;
    QSet<QString> m_seen;
    qsizetype m_pointerLength = 0;
    qsizetype m_more = 0;
    bool m_overflow = false;
};

// What a check took, for measurements.
struct CheckStats
{
    qint64 work = 0;
    qint64 peakMemory = 0;
};

// The lines of the errors of `value` against `schema`, empty when it
// matches. See McpSchema::validate().
QStringList checkValue(const CompiledSchema& schema, const QJsonValue& value, CheckStats* stats = nullptr);

} // namespace PhosphorMcp::Schema
