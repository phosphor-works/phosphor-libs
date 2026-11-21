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
// Work is counted in units, at most about 20 ns each of a release build,
// whatever the schema and the value. The checker charges each step it takes
// (a value walked, a subschema applied to a value, a keyword checked, a
// member or an item visited, a name looked up, sixteen code units of a
// string or a name read, compared or hashed, a word of an annotation set
// merged, an error built), weighted by what it costs as a sweep of each
// charge measured it. The check stops the moment the count passes MaxWork,
// under a second. The README has the measured times.
constexpr qint64 MaxWork = 40'000'000;
// The memory the check holds beyond the value and the schema, counted as it
// goes: the sets of the members and items that subschemas evaluated (for
// unevaluatedProperties and unevaluatedItems), the keys that uniqueItems
// sorts, and the sorted names of the objects whose members are looked up.
// The errors are not counted here, since MaxCountedErrors and
// MaxErrorPointerLength bound them.
constexpr qint64 MaxMemory = 64 * 1024 * 1024;
// Subschemas applied inside each other, each a few frames of the checker's
// recursion on the stack: about a kilobyte each in a release build, as
// measured, and more in a debug build with sanitizers...
constexpr int MaxCheckDepth = 3000;
// ...and the stack that the check itself takes, measured from the addresses
// of its frames, whatever the build: half of the 8 MiB that a Linux thread
// has by default.
constexpr qint64 MaxCheckStackBytes = 4 * 1024 * 1024;

// What each step of a check is charged, so that a unit is about the same
// time whatever the schema and the value, as measured.
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
// A member visited, or a name looked up or sorted.
constexpr qint64 UnitsPerMember = 2;
// A string's text or a member's name read where it is, for each this many
// bytes of it, up to MaxLineUnits. The members of an object and the strings
// of a list are read one after the other, and a long one puts the next
// that much farther, a miss of the cache when the value is larger than the
// cache. A few lines are all that a read misses, whatever the length.
constexpr qint64 BytesPerLine = 64;
constexpr qint64 MaxLineUnits = 4;
// Comparisons of a sort of plain integers (hashes) for each unit.
constexpr qint64 ComparisonsPerUnit = 2;
// An error built, beside its pointer.
constexpr qint64 UnitsPerError = 24;
// A token of an error's JSON pointer built, beside its name decoded...
constexpr qint64 UnitsPerToken = 2;
// ...and one of the line of a listed error.
constexpr qint64 UnitsPerShownToken = 16;
// Each this many code units of a string or a name read in its own encoding
// (bytes of UTF-8 or Latin-1, or UTF-16 code units): counted, scanned, or
// compared with text in the same encoding...
constexpr qint64 CharactersPerUnit = 16;
// ...and the units for as many hashed by code point (hashText()), or
// compared by code point (compareText())...
constexpr qint64 HashUnits = 3;
constexpr qint64 CompareUnits = 2;
// ...or compared with text in another encoding where one is UTF-8, which Qt
// decodes as it compares, or decoded from UTF-8 to UTF-16.
constexpr qint64 DecodeUnits = 4;
// Words of an annotation set merged or cleared for each unit.
constexpr qint64 WordsPerUnit = 4;

// What reading text costs, in units (see above).
qint64 hashUnits(QAnyStringView text);
qint64 compareUnits(QAnyStringView a, QAnyStringView b);
qint64 equalUnits(QAnyStringView a, QAnyStringView b);
// Decoded to UTF-16, for a JSON pointer.
qint64 decodeUnits(QAnyStringView text);

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
// equal to a number, strings by their characters, arrays item by item and
// objects member by member, whatever the order in which Qt keeps their
// members (an object built in C++ keeps them in the order of their UTF-16
// code units, a parsed one in that of their code points). Each charges the
// budget for its work, and returns early once the budget is spent.
bool equalValues(const QJsonValue& a, const QJsonValue& b, Budget& budget);
// A total order of JSON values that is 0 exactly when equalValues() is true.
int compareValues(const QJsonValue& a, const QJsonValue& b, Budget& budget);
// A hash that is the same for values that equalValues() finds equal, keyed
// by `key` so that a value cannot be chosen to collide with another without
// it.
quint64 hashValue(const QJsonValue& value, quint64 key, Budget& budget);

// A token of a place in a value: a member name, read in place in the value
// (which outlives the place), or an item's index.
struct PathToken
{
    QAnyStringView name;
    qsizetype index = -1;
};
using Path = std::vector<PathToken>;

// RFC 6901: "~" and "/" in a member name are escaped.
QString pointerToken(const QString& name);
// The same in place, for the text of `text` from `start`, in one pass and
// a second from the back when there is an escape. Returns the escapes.
qsizetype escapeToken(QString& text, qsizetype start);
// A name as an error shows it: an unpaired surrogate replaced so that the
// reply stays valid UTF-8, and cut short.
QString shownName(const QString& name);

// The place in a value that a walk is at: its tokens from the root, and its
// JSON pointer. The pointer is built when an error needs it, from the tokens
// that the last error left unbuilt, and cut back as the walk leaves them, so
// errors at the same place, or below one, build nothing twice.
class Place
{
public:
    void pushName(QAnyStringView name)
    {
        m_tokens.push_back(PathToken{name, -1});
    }
    void pushIndex(qsizetype index)
    {
        m_tokens.push_back(PathToken{QAnyStringView(), index});
    }
    void pop();
    [[nodiscard]] const Path& tokens() const
    {
        return m_tokens;
    }
    // The JSON pointer of the place, or null when it would be longer than
    // MaxErrorPointerLength, which no error that the report kept is. Adds
    // the work of the tokens built to `units`.
    const QString* pointer(qint64& units);

private:
    Path m_tokens;
    QString m_pointer;
    // The length of m_pointer after each token built: one more entry than
    // the tokens built.
    std::vector<qsizetype> m_ends{0};
};

// The errors that validate() returns: each once, at most MaxListedErrors of
// them, and a last line that counts the rest.
class Report
{
public:
    // What the line says when there are too many errors to list: that the
    // value does not match the schema, or, for the walk before the check,
    // only that it is refused.
    enum class Kind : quint8 {
        Check,
        Precheck,
    };
    explicit Report(Kind kind)
        : m_kind(kind)
    {
    }

    // "<pointer>: <description>" for the place, unless it was added before.
    // The test is on the whole pointer, not on the line, which cuts names:
    // two long names alike in their first 61 code units are two errors.
    // Returns the work it took beside the description, in units.
    qint64 add(Place& place, const QString& description);
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
    // An error as it is told from the others.
    struct Seen
    {
        QString pointer;
        QString description;
        friend bool operator==(const Seen& a, const Seen& b) noexcept
        {
            return a.pointer == b.pointer && a.description == b.description;
        }
        friend size_t qHash(const Seen& seen, size_t seed) noexcept
        {
            return qHashMulti(seed, seen.pointer, seen.description);
        }
    };

    QStringList m_lines;
    QSet<Seen> m_seen;
    qsizetype m_pointerLength = 0;
    qsizetype m_more = 0;
    bool m_overflow = false;
    Kind m_kind;
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
