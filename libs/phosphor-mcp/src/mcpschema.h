// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>

#include <memory>

namespace PhosphorMcp {

// A tool's input or output schema, in JSON Schema 2020-12, the dialect that
// MCP clients read. Only the keywords that the server checks the way the
// dialect says compile: see compile() and the README, "Schemas".
//
// jsoncons, the validator, is confined to mcpschema.cpp, the one translation
// unit built with exceptions, because it throws.
class McpSchema
{
public:
    McpSchema();
    ~McpSchema();
    McpSchema(const McpSchema&) = delete;
    McpSchema& operator=(const McpSchema&) = delete;

    // What the root of a schema must be. A tool's input schema describes the
    // arguments object, so its root has "type": "object". An output schema
    // describes structuredContent, which may be any JSON value.
    enum class Root {
        Object,
        Any,
    };

    // Checks `schema` (a root as `root` asks, and only the keywords the
    // server supports) and compiles it. Returns an empty string on success,
    // else why the schema is refused as "<pointer>: <reason>", the pointer
    // being "" for the root itself.
    QString compile(const QJsonObject& schema, Root root = Root::Object);

    // The errors of `value` against the compiled schema, one
    // "<pointer>: <description>" for each, the pointer relative to the
    // value's root ("" for the root itself). Empty when `value` matches.
    //
    // A value that the validator would read differently from a handler is
    // refused before the validator sees it. That is a string or member name
    // with an unpaired UTF-16 surrogate, or an integral number outside the
    // range of qint64 as Qt's JSON parser left it. The parser rounds a number
    // outside qint64, and one written with a fraction or an exponent, to the
    // nearest double. So -9223372036854775809, down to -9223372036854776832,
    // passes as -9223372036854775808, which the handler reads too, and
    // 9223372036854775807.0 is refused as 2^63.
    //
    // The work is bounded. Before the check the server works out how much
    // the check of this value against this schema can cost, and refuses a
    // value whose check would take more than about a second with one line
    // that says so. The list of errors is short whatever the size of
    // `value`, with at most ten errors and a last line that counts the rest.
    // A value with more than a thousand errors gets one line that says so.
    //
    // Not thread-safe: the compiled schema keeps the helper validators it
    // builds for the errors of propertyNames and additionalProperties.
    [[nodiscard]] QStringList validate(const QJsonValue& value) const;

private:
    struct Private;
    std::unique_ptr<Private> d;
};

} // namespace PhosphorMcp
