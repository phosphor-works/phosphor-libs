// SPDX-FileCopyrightText: 2026 Phosphor Works Team
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
// dialect says compile: see compile() and the README, "Schemas". The checker
// is the library's own (mcpschemacompile.cpp, mcpschemacheck.cpp), on Qt's
// JSON as the handler reads it.
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
    // server supports, within the sizes it states) and compiles it. Returns
    // an empty string on success, else why the schema is refused as
    // "<pointer>: <reason>", the pointer being "" for the root itself.
    QString compile(const QJsonObject& schema, Root root = Root::Object);

    // The errors of `value` against the compiled schema, one
    // "<pointer>: <description>" for each, the pointer relative to the
    // value's root ("" for the root itself). Empty when `value` matches.
    //
    // Before the check, a value is refused that a handler would misread or
    // a reply could not carry, with a line for each place: a string or a
    // member name with an unpaired UTF-16 surrogate, a number that is not
    // finite, and a value nested more than 1024 levels deep. Where the
    // schema asks for an integer, a number without a fraction outside the
    // range of qint64 (1e19) is refused, since QJsonValue::toInteger() reads
    // 0 for it.
    //
    // The work is bounded. The check counts what it does and the memory it
    // holds as it goes, and stops with one line that says the value is too
    // large to check the moment either passes its bound (about a second of
    // a release build). The list of errors is short whatever the size of
    // `value`: at most ten errors and a last line that counts the rest, and
    // one line that says so for a value with more than a thousand.
    //
    // Thread-safe: a check keeps nothing in the compiled schema.
    [[nodiscard]] QStringList validate(const QJsonValue& value) const;

private:
    struct Private;
    std::unique_ptr<Private> d;
};

} // namespace PhosphorMcp
