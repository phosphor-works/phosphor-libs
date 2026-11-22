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
// (a value walked, a subschema applied to a value, and more in place or in
// a large schema, a keyword checked, a member or an item visited, a value
// taken out of its array or object, a name looked up, a name or a string
// read away from the last one read of its object or array, sixteen code
// units of a string or a name, or fewer, read, compared or hashed, a word
// of an annotation set merged, an error built), weighted by what it costs.
// A sweep of each charge, with short and long text in each encoding,
// numbers of each kind, large and deep schemas, and tables of the schema of
// up to a million entries, measured at most about 16 ns a unit while little
// else contended for the caches and for memory, some 1.6 times the 10 ns a
// unit of a list of integers checked for their type by the same build
// beside it. The dearest shapes wait on memory, and slow down when other
// processes contend for it, while the list does not: under such load they
// measured up to about 20 ns a unit, twice the list's. The check stops the
// moment the count passes MaxWork, within the charge of one step (a text
// read, a name or an item hashed or sorted, an error built), in some 0.6 s
// at the most measured without such load and 0.8 s with it. The README has
// the measured times.
constexpr qint64 MaxWork = 40'000'000;
// The memory the check holds beyond the value and the schema, counted as it
// goes: the sets of the members and items that subschemas evaluated (for
// unevaluatedProperties and unevaluatedItems), the keys that uniqueItems
// sorts, the sorted names of the objects whose members are looked up, the
// members of the objects sorted by name to be compared, and the list of the
// triggers of dependentRequired that an object has.
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
// More for a subschema applied in place (allOf, anyOf, oneOf, not, if,
// then, else, $ref, dependentSchemas): the frames of the keyword that
// applies it, and its annotations kept apart.
constexpr qint64 UnitsPerInPlace = 1;
// More for a subschema applied when the schema's nodes and the frames of
// the check (StackBytesPerApply for each subschema it is in) take more
// memory than the caches hold, since each application then misses them: a
// unit from FootprintBytes, and one more at each fourfold, up to
// MaxFootprintUnits (see footprintUnits()).
constexpr qint64 StackBytesPerApply = 1024;
constexpr qint64 FootprintBytes = 256 * 1024;
constexpr qint64 MaxFootprintUnits = 3;
// More for a name or a string looked up in a table of the schema (enum,
// properties, dependentRequired), and for each trigger of dependentRequired
// that an object has, once for its entry and once more for its list of
// names when it has one: a slot and a text read anywhere in the schema's
// tables, misses of the caches and of the TLB once the tables together are
// larger than the caches hold. Four units from FootprintBytes of the
// tables, and twice as many at each fourfold, up to MaxTableUnits (see
// tableUnits()).
constexpr qint64 MaxTableUnits = 16;
// More for an object or an array, walked, applied to, compared or hashed:
// its members or items are read through its own storage, away from the
// last. And for an item that uniqueItems compares, read in the order of
// the hashes, away from the last one read of the array.
constexpr qint64 UnitsPerContainer = 2;
// More for a string applied to, which Qt took out of its array or object:
// the value it makes of the string holds a reference to the container's
// storage, counted up and down again. And for a string, an object or an
// array compared or hashed, taken out of its container, whose value holds a
// reference to that storage or to its own.
constexpr qint64 UnitsPerReference = 2;
// A value taken out of its array or object to be compared or hashed (enum,
// const, uniqueItems), beside UnitsPerReference.
constexpr qint64 UnitsPerTaken = 1;
// A keyword checked, or an item visited.
constexpr qint64 UnitsPerStep = 1;
// A member visited, or a name looked up or sorted.
constexpr qint64 UnitsPerMember = 2;
// A string's text or a member's name read where it is, for each this many
// bytes of it, up to MaxLineUnits. The text of a container is mostly read
// in the order it is kept in (see MaxGapUnits), and a long one puts the
// next that much farther, a miss of the cache when the value is larger than
// the cache. A few lines are all that a read misses, whatever the length.
constexpr qint64 BytesPerLine = 64;
constexpr qint64 MaxLineUnits = 4;
// A name or a string read away from the last one read of the same object or
// array, a unit for each BytesPerLine bytes between the two, up to
// MaxGapUnits (see TextReads). Qt keeps an object's members sorted by name,
// but their text in the order in which it was parsed or inserted, and the
// items of an array that a handler built in the order of their places, but
// their text in the order in which it was inserted. So the next text may be
// anywhere in the container's storage: a miss of the cache, and of the TLB,
// when the container is larger than the cache.
constexpr qint64 MaxGapUnits = 8;
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

// What reading text costs, in units (see above): `size` code units at
// `weight` units for each CharactersPerUnit, rounded up, so that a short
// text pays its share too.
qint64 textUnits(qint64 weight, qint64 size);
// The units more for a subschema applied, for `bytes` of nodes and frames.
qint64 footprintUnits(qint64 bytes);
// The units more for a lookup in the schema's tables, for `bytes` of them.
qint64 tableUnits(qint64 bytes);
qint64 hashUnits(QAnyStringView text);
qint64 compareUnits(QAnyStringView a, QAnyStringView b);
qint64 equalUnits(QAnyStringView a, QAnyStringView b);
// Decoded to UTF-16, for a JSON pointer. The text of an error's pointer,
// decoded, scanned, written again and hashed, is the one exception to the
// rounding up of textUnits(): it is charged for each whole CharactersPerUnit,
// since the error (UnitsPerError) and each of its tokens (UnitsPerToken, or
// UnitsPerShownToken for a line) are charged units of their own beside it.
qint64 decodeUnits(QAnyStringView text);

// The names and strings of one object or array read one after the other:
// the units of the bytes between each and the last (see MaxGapUnits). The
// first is charged with the container (UnitsPerContainer), or `first`
// units for a first read that may be anywhere in it, and a text read again
// costs nothing more.
class TextReads
{
public:
    TextReads() = default;
    explicit TextReads(qint64 first)
        : m_first(first)
    {
    }

    qint64 next(QAnyStringView text)
    {
        const auto start = quintptr(text.data());
        const quintptr end = start + quintptr(text.size_bytes());
        qint64 units = m_first;
        if (m_end != 0) {
            const quintptr gap = start >= m_end ? start - m_end : (m_start >= end ? m_start - end : 0);
            units = gap >= quintptr(MaxGapUnits * BytesPerLine) ? MaxGapUnits : qint64(gap) / BytesPerLine;
        }
        m_start = start;
        m_end = end;
        return units;
    }

private:
    quintptr m_start = 0;
    quintptr m_end = 0;
    qint64 m_first = 0;
};

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

// Memory counted in a budget while it is held: what allocate() counts,
// whether it passed MaxMemory or not, is released when the holder goes.
class HeldMemory
{
public:
    explicit HeldMemory(Budget& budget)
        : m_budget(budget)
    {
    }
    ~HeldMemory()
    {
        m_budget.release(m_bytes);
    }
    HeldMemory(const HeldMemory&) = delete;
    HeldMemory& operator=(const HeldMemory&) = delete;

    // Counts `bytes` more. False once the memory passed MaxMemory.
    bool allocate(qint64 bytes)
    {
        m_bytes += bytes;
        return m_budget.allocate(bytes);
    }

private:
    Budget& m_budget;
    qint64 m_bytes = 0;
};

// Comparisons of JSON values as JSON Schema 2020-12 makes them (enum, const,
// uniqueItems): numbers by their value, 1 and 1.0 alike, a boolean never
// equal to a number, strings by their characters, arrays item by item and
// objects member by member, whatever the order in which Qt keeps their
// members (an object built in C++ keeps them in the order of their UTF-16
// code units, a parsed one in that of their code points). Each charges the
// budget for its work, and returns early once the budget is spent.
// `readsA` and `readsB`: those of the arrays that the values were taken out
// of, if they were (see UnitsPerTaken).
bool equalValues(const QJsonValue& a, const QJsonValue& b, Budget& budget, TextReads* readsA = nullptr,
                 TextReads* readsB = nullptr);
// A total order of JSON values that is 0 exactly when equalValues() is true.
int compareValues(const QJsonValue& a, const QJsonValue& b, Budget& budget, TextReads* readsA = nullptr,
                  TextReads* readsB = nullptr);
// A hash that is the same for values that equalValues() finds equal, keyed
// by `key` so that a value cannot be chosen to collide with another without
// it. `reads`: those of the array that the value was taken out of, if it
// was.
quint64 hashValue(const QJsonValue& value, quint64 key, Budget& budget, TextReads* reads = nullptr);

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
