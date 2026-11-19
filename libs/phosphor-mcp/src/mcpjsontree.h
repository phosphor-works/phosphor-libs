// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

// A valijson adapter over a JSON value that is converted once, for
// mcpschema.cpp only (the one translation unit built with exceptions).
//
// valijson's own QtJsonAdapter converts a string to UTF-8 each time valijson
// reads it, which is once for each value of an enum or a const it is
// compared with, and once for each minLength or maxLength. A long string in
// a model's arguments was converted again for every one of them. Here the
// value is converted into a JsonTree first, each string to UTF-8 once, and
// the adapter reads the tree. It is modelled on qtjson_adapter.hpp and
// behaves as QtJsonAdapter does. The types are strict, the members of an
// object come in QJsonObject's order, and a number gives the answers that
// valijson's QtJsonValue gives for the same QJsonValue, asked once when the
// tree is built. So integers and doubles are told apart as the valijson in
// use tells them apart.

#include <QCborValue>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QtGlobal>

#include <valijson/adapters/qtjson_adapter.hpp>
#include <valijson/exceptions.hpp>
#include <valijson/internal/adapter.hpp>
#include <valijson/internal/basic_adapter.hpp>
#include <valijson/internal/frozen_value.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace PhosphorMcp {

// A JSON value with its strings in UTF-8, each converted once.
class JsonTree
{
public:
    enum class Kind : unsigned char {
        Undefined,
        Null,
        Bool,
        // A number that QJsonValue holds as an integer, and one it holds as
        // a double. Both are rebuilt as they were (see number()).
        Integer,
        Double,
        String,
        Array,
        Object,
    };

    // What valijson's QtJsonValue answers for a number, asked once.
    enum QtAnswer : unsigned char {
        QtIsDouble = 1,
        QtIsInteger = 2,
        QtIsNumber = 4,
    };

    // 16 bytes, as small as an element of a QJsonArray.
    struct Node
    {
        union {
            bool boolean;
            qint64 integer;
            double number;
            // String: its first byte in m_text. Array: its first item in
            // m_nodes. Object: its first member in m_members.
            std::size_t begin = 0;
        };
        // String: its bytes. Array: its items. Object: its members.
        quint32 count = 0;
        Kind kind = Kind::Undefined;
        // Integer, Double: the QtAnswer flags.
        unsigned char qt = 0;
    };

    // Indices in m_nodes. The name is a String node, so that it can be
    // checked as a value (propertyNames).
    struct Member
    {
        std::size_t name;
        std::size_t value;
    };

    static constexpr std::size_t npos = std::size_t(-1);

    explicit JsonTree(const QJsonValue& value)
        : m_nodes(1)
    {
        fill(0, value);
    }

    // A copy of the value at `index` in `source`, its root at 0.
    JsonTree(const JsonTree& source, std::size_t index)
        : m_nodes(1)
    {
        copy(source, index, 0);
    }

    // The empty object of a default adapter, as QtJsonAdapter's, and an
    // undefined value.
    static const JsonTree& emptyObject()
    {
        static const JsonTree tree{QJsonValue(QJsonObject())};
        return tree;
    }
    static const JsonTree& undefined()
    {
        static const JsonTree tree{QJsonValue(QJsonValue::Undefined)};
        return tree;
    }

    [[nodiscard]] const Node& node(std::size_t index) const
    {
        return m_nodes[index];
    }

    [[nodiscard]] std::string_view string(std::size_t index) const
    {
        const Node& n = m_nodes[index];
        return std::string_view(m_text.data() + n.begin, n.count);
    }

    // The member at `index` of m_members, as an object's begin counts.
    [[nodiscard]] const Member& member(std::size_t index) const
    {
        return m_members[index];
    }

    // The member of the object at `index` named `name`, as an index in
    // m_members, or npos.
    [[nodiscard]] std::size_t find(std::size_t index, std::string_view name) const
    {
        const Node& object = m_nodes[index];
        const auto first = m_byName.begin() + std::ptrdiff_t(object.begin);
        const auto last = first + std::ptrdiff_t(object.count);
        const auto found = std::lower_bound(first, last, name, [this](std::size_t member, std::string_view wanted) {
            return string(m_members[member].name) < wanted;
        });
        if (found == last || string(m_members[*found].name) != name) {
            return npos;
        }
        return *found;
    }

    // The UTF-8 bytes of all the strings and member names.
    [[nodiscard]] std::size_t textSize() const
    {
        return m_text.size();
    }

    // The number at `index` as Qt held it.
    [[nodiscard]] QJsonValue number(std::size_t index) const
    {
        const Node& n = m_nodes[index];
        return n.kind == Kind::Integer ? QJsonValue(n.integer) : QJsonValue(n.number);
    }

private:
    void fill(std::size_t index, const QJsonValue& value)
    {
        Node node;
        switch (value.type()) {
        case QJsonValue::Null:
            node.kind = Kind::Null;
            break;
        case QJsonValue::Bool:
            node.kind = Kind::Bool;
            node.boolean = value.toBool();
            break;
        case QJsonValue::Double: {
            // QJsonValue::type() says Double for both. The parser keeps an
            // integer literal that fits qint64 as an integer.
            const QCborValue number = QCborValue::fromJsonValue(value);
            if (number.isInteger()) {
                node.kind = Kind::Integer;
                node.integer = number.toInteger();
            } else {
                node.kind = Kind::Double;
                node.number = value.toDouble();
            }
            const valijson::adapters::QtJsonValue qt(value);
            node.qt = (qt.isDouble() ? QtIsDouble : 0) | (qt.isInteger() ? QtIsInteger : 0)
                | (qt.isNumber() ? QtIsNumber : 0);
            break;
        }
        case QJsonValue::String:
            node = stringNode(value.toString());
            break;
        case QJsonValue::Array: {
            const QJsonArray array = value.toArray();
            const std::size_t first = m_nodes.size();
            m_nodes.resize(first + std::size_t(array.size()));
            for (qsizetype i = 0; i < array.size(); ++i) {
                fill(first + std::size_t(i), array.at(i));
            }
            node.kind = Kind::Array;
            node.begin = first;
            node.count = countOf(std::size_t(array.size()));
            break;
        }
        case QJsonValue::Object: {
            const QJsonObject object = value.toObject();
            const std::size_t first = m_members.size();
            const std::size_t names = m_nodes.size();
            const quint32 count = countOf(std::size_t(object.size()));
            m_members.resize(first + count);
            m_byName.resize(first + count);
            m_nodes.resize(names + 2 * std::size_t(count));
            std::size_t i = 0;
            for (auto it = object.constBegin(); it != object.constEnd(); ++it, ++i) {
                const Member member{names + 2 * i, names + 2 * i + 1};
                m_nodes[member.name] = stringNode(it.key());
                fill(member.value, it.value());
                m_members[first + i] = member;
            }
            node.kind = Kind::Object;
            node.begin = first;
            node.count = count;
            indexByName(node);
            break;
        }
        case QJsonValue::Undefined:
            break;
        }
        m_nodes[index] = node;
    }

    // A count that Node holds. A larger value is refused, and validate()
    // says that it could not be checked.
    static quint32 countOf(std::size_t count)
    {
        if (count > std::numeric_limits<quint32>::max()) {
            throw std::length_error("a JSON value with more than 2^32 - 1 items or bytes");
        }
        return quint32(count);
    }

    Node stringNode(const QString& string)
    {
        // What QString::toStdString(), which QtJsonAdapter uses, gives.
        const QByteArray utf8 = string.toUtf8();
        Node node;
        node.kind = Kind::String;
        node.begin = m_text.size();
        node.count = countOf(std::size_t(utf8.size()));
        m_text.append(utf8.constData(), std::size_t(utf8.size()));
        return node;
    }

    void copy(const JsonTree& source, std::size_t from, std::size_t to)
    {
        Node node = source.m_nodes[from];
        switch (node.kind) {
        case Kind::String: {
            const std::string_view bytes = source.string(from);
            node.begin = m_text.size();
            m_text.append(bytes);
            break;
        }
        case Kind::Array: {
            const std::size_t first = m_nodes.size();
            m_nodes.resize(first + node.count);
            for (std::size_t i = 0; i < node.count; ++i) {
                copy(source, node.begin + i, first + i);
            }
            node.begin = first;
            break;
        }
        case Kind::Object: {
            const std::size_t first = m_members.size();
            const std::size_t names = m_nodes.size();
            m_members.resize(first + node.count);
            m_byName.resize(first + node.count);
            m_nodes.resize(names + 2 * std::size_t(node.count));
            for (std::size_t i = 0; i < node.count; ++i) {
                const Member member{names + 2 * i, names + 2 * i + 1};
                copy(source, source.m_members[node.begin + i].name, member.name);
                copy(source, source.m_members[node.begin + i].value, member.value);
                m_members[first + i] = member;
            }
            node.begin = first;
            indexByName(node);
            break;
        }
        default:
            break;
        }
        m_nodes[to] = node;
    }

    // Sorts the members of `object` by name for find(). QJsonObject keeps
    // its keys in an order of their own, which is the order of UTF-8 bytes
    // too unless a key has a character above the surrogates, so the sort
    // has nothing to do as a rule.
    void indexByName(const Node& object)
    {
        const auto first = m_byName.begin() + std::ptrdiff_t(object.begin);
        const auto last = first + std::ptrdiff_t(object.count);
        std::iota(first, last, object.begin);
        const auto byName = [this](std::size_t a, std::size_t b) {
            return string(m_members[a].name) < string(m_members[b].name);
        };
        if (!std::is_sorted(first, last, byName)) {
            std::sort(first, last, byName);
        }
    }

    std::vector<Node> m_nodes;
    std::vector<Member> m_members;
    // For each object, its members in m_members sorted by name, at the
    // object's begin. It grows with m_members.
    std::vector<std::size_t> m_byName;
    std::string m_text;
};

class JsonTreeAdapter;
class JsonTreeArray;
class JsonTreeObject;
class JsonTreeArrayIterator;
class JsonTreeObjectIterator;

using JsonTreeObjectMember = std::pair<std::string, JsonTreeAdapter>;

// A node of a JsonTree. It is BasicAdapter's value type, as QtJsonValue is
// QtJsonAdapter's.
class JsonTreeValue
{
public:
    // The empty object, as QtJsonValue's default.
    JsonTreeValue()
        : JsonTreeValue(JsonTree::emptyObject(), 0)
    {
    }

    JsonTreeValue(const JsonTree& tree, std::size_t index)
        : m_tree(&tree)
        , m_index(index)
    {
    }

    [[nodiscard]] const JsonTree& tree() const
    {
        return *m_tree;
    }
    [[nodiscard]] std::size_t index() const
    {
        return m_index;
    }
    [[nodiscard]] const JsonTree::Node& node() const
    {
        return m_tree->node(m_index);
    }
    [[nodiscard]] JsonTree::Kind kind() const
    {
        return node().kind;
    }
    // The bytes of a string.
    [[nodiscard]] std::string_view string() const
    {
        return m_tree->string(m_index);
    }

    // The member of an object named `name`, or nothing.
    [[nodiscard]] std::optional<JsonTreeValue> member(std::string_view name) const
    {
        if (kind() != JsonTree::Kind::Object) {
            return std::nullopt;
        }
        const std::size_t found = m_tree->find(m_index, name);
        if (found == JsonTree::npos) {
            return std::nullopt;
        }
        return JsonTreeValue(*m_tree, m_tree->member(found).value);
    }
    // The item `index` of an array, or nothing.
    [[nodiscard]] std::optional<JsonTreeValue> item(std::size_t index) const
    {
        if (kind() != JsonTree::Kind::Array || index >= node().count) {
            return std::nullopt;
        }
        return JsonTreeValue(*m_tree, node().begin + index);
    }
    // The name and the value of the member `index` of an object, in
    // QJsonObject's order.
    [[nodiscard]] JsonTreeValue memberName(std::size_t index) const
    {
        return JsonTreeValue(*m_tree, m_tree->member(node().begin + index).name);
    }
    [[nodiscard]] JsonTreeValue memberValue(std::size_t index) const
    {
        return JsonTreeValue(*m_tree, m_tree->member(node().begin + index).value);
    }

    // What BasicAdapter asks of its value type, answered as QtJsonValue
    // answers it.
    valijson::adapters::FrozenValue* freeze() const;
    std::optional<JsonTreeArray> getArrayOptional() const;
    std::optional<JsonTreeObject> getObjectOptional() const;

    bool getArraySize(std::size_t& result) const
    {
        if (!isArray()) {
            return false;
        }
        result = node().count;
        return true;
    }
    bool getObjectSize(std::size_t& result) const
    {
        if (!isObject()) {
            return false;
        }
        result = node().count;
        return true;
    }
    bool getBool(bool& result) const
    {
        if (!isBool()) {
            return false;
        }
        result = node().boolean;
        return true;
    }
    // QtJsonValue gives QJsonValue::toDouble(), which is the integer
    // converted for a number that QJsonValue holds as an integer.
    bool getDouble(double& result) const
    {
        if (!isDouble()) {
            return false;
        }
        result = kind() == JsonTree::Kind::Integer ? double(node().integer) : node().number;
        return true;
    }
    bool getInteger(int64_t& result) const
    {
        return isQtNumber() && qtValue().getInteger(result);
    }
    bool getString(std::string& result) const
    {
        if (!isString()) {
            return false;
        }
        result = string();
        return true;
    }

    static bool hasStrictTypes()
    {
        return true;
    }

    bool isArray() const
    {
        return kind() == JsonTree::Kind::Array;
    }
    bool isBool() const
    {
        return kind() == JsonTree::Kind::Bool;
    }
    bool isDouble() const
    {
        return (node().qt & JsonTree::QtIsDouble) != 0;
    }
    bool isInteger() const
    {
        return (node().qt & JsonTree::QtIsInteger) != 0;
    }
    bool isNull() const
    {
        return kind() == JsonTree::Kind::Null;
    }
    bool isNumber() const
    {
        return (node().qt & JsonTree::QtIsNumber) != 0;
    }
    bool isObject() const
    {
        return kind() == JsonTree::Kind::Object;
    }
    bool isString() const
    {
        return kind() == JsonTree::Kind::String;
    }

private:
    [[nodiscard]] bool isQtNumber() const
    {
        return kind() == JsonTree::Kind::Integer || kind() == JsonTree::Kind::Double;
    }
    // valijson's reading of the same number, which differs between its
    // releases (1.1.4 tells integers by their value, 1.1.3 by toInt()).
    [[nodiscard]] valijson::adapters::QtJsonValue qtValue() const
    {
        return valijson::adapters::QtJsonValue(m_tree->number(m_index));
    }

    const JsonTree* m_tree;
    std::size_t m_index;
};

// The items of an array node, as QtJsonArray.
class JsonTreeArray
{
public:
    using const_iterator = JsonTreeArrayIterator;
    using iterator = JsonTreeArrayIterator;

    JsonTreeArray()
        : m_tree(&JsonTree::emptyObject())
    {
    }

    explicit JsonTreeArray(const JsonTreeValue& value)
        : m_tree(&value.tree())
        , m_begin(value.node().begin)
        , m_count(value.node().count)
    {
        if (!value.isArray()) {
            valijson::throwRuntimeError("Value is not an array.");
        }
    }

    JsonTreeArrayIterator begin() const;
    JsonTreeArrayIterator end() const;

    std::size_t size() const
    {
        return m_count;
    }

private:
    const JsonTree* m_tree;
    std::size_t m_begin = 0;
    std::size_t m_count = 0;
};

// The members of an object node, as QtJsonObject.
class JsonTreeObject
{
public:
    using const_iterator = JsonTreeObjectIterator;
    using iterator = JsonTreeObjectIterator;

    JsonTreeObject()
        : m_tree(&JsonTree::emptyObject())
    {
    }

    explicit JsonTreeObject(const JsonTreeValue& value)
        : m_tree(&value.tree())
        , m_index(value.index())
        , m_begin(value.node().begin)
        , m_count(value.node().count)
    {
        if (!value.isObject()) {
            valijson::throwRuntimeError("Value is not an object.");
        }
    }

    JsonTreeObjectIterator begin() const;
    JsonTreeObjectIterator end() const;
    JsonTreeObjectIterator find(const std::string& propertyName) const;

    std::size_t size() const
    {
        return m_count;
    }

private:
    const JsonTree* m_tree;
    std::size_t m_index = 0;
    std::size_t m_begin = 0;
    std::size_t m_count = 0;
};

// A copy of a value of a schema (enum, const), kept by the compiled schema.
class JsonTreeFrozenValue : public valijson::adapters::FrozenValue
{
public:
    explicit JsonTreeFrozenValue(JsonTree tree)
        : m_tree(std::move(tree))
    {
    }

    FrozenValue* clone() const override
    {
        return new JsonTreeFrozenValue(m_tree);
    }

    bool equalTo(const valijson::adapters::Adapter& other, bool strict) const override;

private:
    JsonTree m_tree;
};

class JsonTreeAdapter : public valijson::adapters::BasicAdapter<JsonTreeAdapter, JsonTreeArray, JsonTreeObjectMember,
                                                                JsonTreeObject, JsonTreeValue>
{
public:
    // The empty object.
    JsonTreeAdapter() = default;

    JsonTreeAdapter(const JsonTreeValue& value)
        : BasicAdapter(value)
        , m_node(value)
    {
    }

    // The root of `tree`. valijson's schema parser builds an adapter from a
    // document this way.
    explicit JsonTreeAdapter(const JsonTree& tree)
        : JsonTreeAdapter(JsonTreeValue(tree, 0))
    {
    }

    [[nodiscard]] const JsonTreeValue& value() const
    {
        return m_node;
    }

    // BasicAdapter compares two strings by copying each out of its adapter
    // (asString()), so an enum would copy a long argument once for each of
    // its values. Two strings of this adapter compare in place. Everything
    // else is BasicAdapter's comparison, which copies no other string out
    // under strict types, and which brings the items of arrays and objects
    // back here.
    bool equalTo(const valijson::adapters::Adapter& other, bool strict) const override
    {
        if (strict && m_node.isString()) {
            if (const auto* tree = dynamic_cast<const JsonTreeAdapter*>(&other)) {
                return tree->m_node.isString() && tree->m_node.string() == m_node.string();
            }
        }
        return BasicAdapter::equalTo(other, strict);
    }

private:
    // BasicAdapter keeps its own copy, which it does not share.
    JsonTreeValue m_node;
};

class JsonTreeArrayIterator
{
public:
    using iterator_category = std::bidirectional_iterator_tag;
    using value_type = JsonTreeAdapter;
    using difference_type = JsonTreeAdapter;
    using pointer = JsonTreeAdapter*;
    using reference = JsonTreeAdapter&;

    JsonTreeArrayIterator(const JsonTree* tree, std::size_t index)
        : m_tree(tree)
        , m_index(index)
    {
    }

    JsonTreeAdapter operator*() const
    {
        return JsonTreeAdapter(JsonTreeValue(*m_tree, m_index));
    }

    valijson::adapters::DerefProxy<JsonTreeAdapter> operator->() const
    {
        return valijson::adapters::DerefProxy<JsonTreeAdapter>(**this);
    }

    bool operator==(const JsonTreeArrayIterator& other) const
    {
        return m_tree == other.m_tree && m_index == other.m_index;
    }
    bool operator!=(const JsonTreeArrayIterator& other) const
    {
        return !(*this == other);
    }

    const JsonTreeArrayIterator& operator++()
    {
        ++m_index;
        return *this;
    }
    JsonTreeArrayIterator operator++(int)
    {
        JsonTreeArrayIterator before(*this);
        ++m_index;
        return before;
    }
    const JsonTreeArrayIterator& operator--()
    {
        --m_index;
        return *this;
    }
    void advance(std::ptrdiff_t n)
    {
        m_index = std::size_t(std::ptrdiff_t(m_index) + n);
    }

private:
    const JsonTree* m_tree;
    std::size_t m_index;
};

class JsonTreeObjectIterator
{
public:
    using iterator_category = std::bidirectional_iterator_tag;
    using value_type = JsonTreeObjectMember;
    using difference_type = JsonTreeObjectMember;
    using pointer = JsonTreeObjectMember*;
    using reference = JsonTreeObjectMember&;

    // `member`: an index in the tree's members.
    JsonTreeObjectIterator(const JsonTree* tree, std::size_t member)
        : m_tree(tree)
        , m_member(member)
    {
    }

    JsonTreeObjectMember operator*() const
    {
        const JsonTree::Member& member = m_tree->member(m_member);
        return JsonTreeObjectMember(std::string(m_tree->string(member.name)),
                                    JsonTreeAdapter(JsonTreeValue(*m_tree, member.value)));
    }

    valijson::adapters::DerefProxy<JsonTreeObjectMember> operator->() const
    {
        return valijson::adapters::DerefProxy<JsonTreeObjectMember>(**this);
    }

    bool operator==(const JsonTreeObjectIterator& other) const
    {
        return m_tree == other.m_tree && m_member == other.m_member;
    }
    bool operator!=(const JsonTreeObjectIterator& other) const
    {
        return !(*this == other);
    }

    const JsonTreeObjectIterator& operator++()
    {
        ++m_member;
        return *this;
    }
    JsonTreeObjectIterator operator++(int)
    {
        JsonTreeObjectIterator before(*this);
        ++m_member;
        return before;
    }
    const JsonTreeObjectIterator& operator--()
    {
        --m_member;
        return *this;
    }

private:
    const JsonTree* m_tree;
    std::size_t m_member;
};

inline valijson::adapters::FrozenValue* JsonTreeValue::freeze() const
{
    return new JsonTreeFrozenValue(JsonTree(*m_tree, m_index));
}

inline std::optional<JsonTreeArray> JsonTreeValue::getArrayOptional() const
{
    if (!isArray()) {
        return std::nullopt;
    }
    return JsonTreeArray(*this);
}

inline std::optional<JsonTreeObject> JsonTreeValue::getObjectOptional() const
{
    if (!isObject()) {
        return std::nullopt;
    }
    return JsonTreeObject(*this);
}

inline bool JsonTreeFrozenValue::equalTo(const valijson::adapters::Adapter& other, bool strict) const
{
    return JsonTreeAdapter(m_tree).equalTo(other, strict);
}

inline JsonTreeArrayIterator JsonTreeArray::begin() const
{
    return JsonTreeArrayIterator(m_tree, m_begin);
}

inline JsonTreeArrayIterator JsonTreeArray::end() const
{
    return JsonTreeArrayIterator(m_tree, m_begin + m_count);
}

inline JsonTreeObjectIterator JsonTreeObject::begin() const
{
    return JsonTreeObjectIterator(m_tree, m_begin);
}

inline JsonTreeObjectIterator JsonTreeObject::end() const
{
    return JsonTreeObjectIterator(m_tree, m_begin + m_count);
}

inline JsonTreeObjectIterator JsonTreeObject::find(const std::string& propertyName) const
{
    const std::size_t found = m_tree->find(m_index, propertyName);
    return found == JsonTree::npos ? end() : JsonTreeObjectIterator(m_tree, found);
}

} // namespace PhosphorMcp

namespace valijson::adapters {

template<>
struct AdapterTraits<PhosphorMcp::JsonTreeAdapter>
{
    using DocumentType = PhosphorMcp::JsonTree;

    static std::string adapterName()
    {
        return "PhosphorMcp::JsonTreeAdapter";
    }
};

} // namespace valijson::adapters
