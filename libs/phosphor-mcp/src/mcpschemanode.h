// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

// The compiled form of a tool's schema, shared by the files of the checker:
// mcpschemacompile.cpp builds it, mcpschemacheck.cpp checks values against
// it, mcpschemavalue.cpp compares JSON values, and mcpschemareport.cpp
// writes the lines of the errors. None of it is part of the library's ABI.

#include <QAnyStringView>
#include <QByteArray>
#include <QByteArrayList>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <utility>
#include <vector>

namespace PhosphorMcp::Schema {

// A schema is refused past these sizes (see the README, "Schemas"). A tool's
// schema goes to the model with every tools/list, so a real one is far
// smaller: Afterglow's largest has some sixty subschemas.
constexpr qsizetype MaxNodes = 10000;
// Subschemas inside subschemas, from the root. The compile recurses once for
// each level.
constexpr int MaxNodeDepth = 256;

// A JSON number as the checker compares it: an integral number that fits a
// 64-bit integer as that integer, any other as a double. Qt's parser gives
// an integer literal within the range of qint64, and an integral double
// within it, as an integer, so 3.0 is 3 here as for a handler.
struct Number
{
    bool isInteger = true;
    qint64 integer = 0;
    double real = 0;

    [[nodiscard]] double toDouble() const
    {
        return isInteger ? double(integer) : real;
    }
};

Number numberOf(const QJsonValue& value);
// -1, 0 or 1, exactly: no rounding of an integer to a double or back.
int compareNumbers(const Number& a, const Number& b);
// Whether the number has no fraction, whatever its size (1e300 has none).
bool isIntegral(const Number& number);
// Whether `value` is a multiple of `divisor` (> 0), as the README says.
bool isMultipleOf(const Number& value, const Number& divisor);
// The number as a line of an error shows it, the same in every locale: an
// integer with all its digits (below 10^21), any other in its shortest
// round-trip form.
QString formatNumber(const Number& number);

// The text of a string or of a member name, as Qt keeps it and as the
// checker reads it, in place: Qt's parser keeps ASCII as Latin-1 and other
// text as UTF-8 (it refuses UTF-8 that is not valid), and a string with an
// escape, or one built from a QString that is not ASCII, as UTF-16. Decoding
// it to a QString would cost a pass over it for each subschema that reads
// it. QAnyStringView::size() counts the code units of the text's own
// encoding (bytes, or UTF-16 code units), which is what reading it costs.
//
// Its length in code points, as 2020-12 counts characters, for text without
// an unpaired surrogate, which the walk before the check refuses.
qsizetype codePoints(QAnyStringView text);
// Whether the text has no unpaired UTF-16 surrogate, which only UTF-16 can
// hold.
bool isValidText(QAnyStringView text);
// Whether the text is in UTF-16, or in UTF-8.
bool isUtf16(QAnyStringView text);
bool isUtf8(QAnyStringView text);
// -1, 0 or 1: the order of the texts' code points, whatever their encodings.
// 0 exactly when QAnyStringView::equal() is true (for text without an
// unpaired surrogate).
int compareText(QAnyStringView a, QAnyStringView b);
// A hash of the text's code points, the same in every encoding, keyed by
// `key`.
quint64 hashText(QAnyStringView text, quint64 key);

// Names or strings of the schema, to look a value's text up in whatever its
// encoding: by UTF-16 for text in UTF-16, and by UTF-8 for text in UTF-8 or
// ASCII.
class NameTable
{
public:
    // A name inserted twice keeps the last value.
    void insert(const QString& name, int value)
    {
        insert(name, name.toUtf8(), value);
    }
    // The same, with the name's UTF-8 at hand.
    void insert(const QString& name, const QByteArray& utf8, int value)
    {
        m_utf16.insert(name, value);
        m_utf8.insert(utf8, value);
    }
    void reserve(qsizetype size)
    {
        m_utf16.reserve(size);
        m_utf8.reserve(size);
    }
    // The value of `name`, or -1.
    [[nodiscard]] int value(QAnyStringView name) const;
    [[nodiscard]] bool isEmpty() const
    {
        return m_utf16.isEmpty();
    }

private:
    QHash<QString, int> m_utf16;
    QHash<QByteArray, int> m_utf8;
};

// The JSON types, as bits of Node::types.
enum TypeBit : quint8 {
    TypeNull = 1,
    TypeBoolean = 2,
    TypeObject = 4,
    TypeArray = 8,
    TypeNumber = 16,
    TypeString = 32,
    TypeInteger = 64,
};

// An enum of more strings than this is looked up in a hash.
constexpr qsizetype EnumHashThreshold = 8;

// One subschema. The keywords it checks, and the subschemas it applies by
// their indices in CompiledSchema::nodes (-1 when absent).
struct Node
{
    // What each application of the node reads, first and together: a large
    // schema, whose nodes the cache does not hold, then costs a line or two
    // of it for each subschema applied, not one for each keyword.
    bool isBoolean = false;
    bool booleanValue = true;
    // type: the bits (the names are below).
    quint8 types = 0;
    bool hasConst = false;
    bool hasEnum = false;
    enum NumberKeyword : quint8 {
        Minimum = 1,
        Maximum = 2,
        ExclusiveMinimum = 4,
        ExclusiveMaximum = 8,
        MultipleOf = 16,
    };
    quint8 numberKeywords = 0;
    bool uniqueItems = false;
    // Applicators, by their indices.
    int ref = -1;
    int notSchema = -1;
    int ifSchema = -1;
    int thenSchema = -1;
    int elseSchema = -1;
    int items = -1;
    int contains = -1;
    int additionalProperties = -1;
    int propertyNames = -1;
    int unevaluatedItems = -1;
    int unevaluatedProperties = -1;
    // Lengths and counts, -1 when absent.
    qint64 minLength = -1;
    qint64 maxLength = -1;
    std::vector<int> allOf;
    std::vector<int> anyOf;
    std::vector<int> oneOf;
    std::vector<int> prefixItems;
    qint64 minItems = -1;
    qint64 maxItems = -1;
    qint64 minProperties = -1;
    qint64 maxProperties = -1;
    qint64 minContains = -1;
    qint64 maxContains = -1;

    // Where the node is: its parent (-1 for the root), and the one or two
    // tokens from the parent's object to it ("items", or "properties" and
    // the name). A JSON pointer is built from them when one is needed, so
    // that a deep schema keeps no path for each node.
    int parent = -1;
    QString keyword;
    QString name;
    bool hasName = false;

    // The names of the types, in the schema's order, for the error.
    QStringList typeNames;

    // const and enum. Their strings are kept here in UTF-16 and in UTF-8,
    // so that a string of the value is compared with them in its own
    // encoding, and a long enum of strings is looked up in a table. Their
    // numbers are read once, here, and a number of the value once for all
    // of them.
    QJsonValue constValue;
    QString constString;
    QByteArray constUtf8;
    Number constNumber;
    QStringList enumStrings;
    QByteArrayList enumUtf8;
    NameTable enumTable;
    std::vector<Number> enumNumbers;
    // The entries that are neither strings nor numbers.
    std::vector<QJsonValue> enumOthers;

    Number minimum;
    Number maximum;
    Number exclusiveMinimum;
    Number exclusiveMaximum;
    Number multipleOf;

    QStringList required;
    std::vector<std::pair<QString, QStringList>> dependentRequired;
    // The triggers of dependentRequired by name, to look the members of an
    // object up in when it has fewer members than there are triggers.
    NameTable dependentRequiredIndex;
    // The hashes of the names above and of the triggers of
    // dependentSchemas, keyed by CompiledSchema::key, in the same order:
    // the check looks them up in an object's names, sorted by the same
    // hash, without hashing them again for each object.
    std::vector<quint64> requiredHashes;
    std::vector<std::pair<quint64, std::vector<quint64>>> dependentRequiredHashes;
    std::vector<quint64> dependentSchemaHashes;

    // The other applicators.
    NameTable properties;
    std::vector<std::pair<QString, int>> dependentSchemas;
    // By name, for a $ref through dependentSchemas.
    QHash<QString, int> dependentSchemaIndex;
    QHash<QString, int> definitions;
    QString refValue;
    bool hasRef = false;
    // contentSchema, checked as any subschema is but never applied: the
    // node, and whether this node is inside one, where no $ref may lead.
    int contentSchema = -1;
    bool inContentSchema = false;
};

struct CompiledSchema
{
    // The root is the first node.
    std::vector<Node> nodes;
    // The key of the hashes of the check (of uniqueItems' items, and of
    // the names it looks up), Qt's global seed when the schema compiled:
    // random in each process, so that a model cannot choose values whose
    // hashes are the same.
    quint64 key = 0;
};

// The JSON pointer of a node, in the schema, built from its parents.
QString nodePointer(const std::vector<Node>& nodes, int index);

// Checks `schema` against the keyword policy (see the README, "Schemas") and
// compiles it into `compiled`. Returns an empty string, or why the schema is
// refused as "<pointer>: <reason>". `objectRoot`: the root must have
// "type": "object".
QString compileSchema(const QJsonObject& schema, bool objectRoot, CompiledSchema& compiled);

} // namespace PhosphorMcp::Schema
