// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

// The compiled form of a tool's schema, shared by the files of the checker:
// mcpschemacompile.cpp builds it, mcpschemacheck.cpp checks values against
// it, mcpschemavalue.cpp compares JSON values, and mcpschemareport.cpp
// writes the lines of the errors. None of it is part of the library's ABI.

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QSet>
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
    // Where the node is: its parent (-1 for the root), and the one or two
    // tokens from the parent's object to it ("items", or "properties" and
    // the name). A JSON pointer is built from them when one is needed, so
    // that a deep schema keeps no path for each node.
    int parent = -1;
    QString keyword;
    QString name;
    bool hasName = false;

    bool isBoolean = false;
    bool booleanValue = true;

    // type: the bits, and the names in the schema's order for the error.
    quint8 types = 0;
    QStringList typeNames;

    // const and enum. Their strings are read once here, so that a string of
    // the value is compared with them without reading them again, and a
    // long enum of strings is looked up in a hash.
    bool hasConst = false;
    QJsonValue constValue;
    QString constString;
    bool hasEnum = false;
    QStringList enumStrings;
    QSet<QString> enumStringSet;
    std::vector<QJsonValue> enumOthers;

    enum NumberKeyword : quint8 {
        Minimum = 1,
        Maximum = 2,
        ExclusiveMinimum = 4,
        ExclusiveMaximum = 8,
        MultipleOf = 16,
    };
    quint8 numberKeywords = 0;
    Number minimum;
    Number maximum;
    Number exclusiveMinimum;
    Number exclusiveMaximum;
    Number multipleOf;

    // Lengths and counts, -1 when absent.
    qint64 minLength = -1;
    qint64 maxLength = -1;
    qint64 minItems = -1;
    qint64 maxItems = -1;
    qint64 minProperties = -1;
    qint64 maxProperties = -1;
    qint64 minContains = -1;
    qint64 maxContains = -1;
    bool uniqueItems = false;

    QStringList required;
    std::vector<std::pair<QString, QStringList>> dependentRequired;
    // The triggers of dependentRequired by name, to look the members of an
    // object up in when it has fewer members than there are triggers.
    QHash<QString, int> dependentRequiredIndex;

    // Applicators.
    QHash<QString, int> properties;
    std::vector<std::pair<QString, int>> dependentSchemas;
    QHash<QString, int> definitions;
    std::vector<int> prefixItems;
    std::vector<int> allOf;
    std::vector<int> anyOf;
    std::vector<int> oneOf;
    int items = -1;
    int contains = -1;
    int additionalProperties = -1;
    int propertyNames = -1;
    int notSchema = -1;
    int ifSchema = -1;
    int thenSchema = -1;
    int elseSchema = -1;
    int unevaluatedItems = -1;
    int unevaluatedProperties = -1;
    int ref = -1;
    QString refValue;
    bool hasRef = false;
};

struct CompiledSchema
{
    // The root is the first node.
    std::vector<Node> nodes;
};

// The JSON pointer of a node, in the schema, built from its parents.
QString nodePointer(const std::vector<Node>& nodes, int index);

// Checks `schema` against the keyword policy (see the README, "Schemas") and
// compiles it into `compiled`. Returns an empty string, or why the schema is
// refused as "<pointer>: <reason>". `objectRoot`: the root must have
// "type": "object".
QString compileSchema(const QJsonObject& schema, bool objectRoot, CompiledSchema& compiled);

} // namespace PhosphorMcp::Schema
