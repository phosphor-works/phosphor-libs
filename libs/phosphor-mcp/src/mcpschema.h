// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>

#include <memory>

namespace PhosphorMcp {

// A tool's input or output schema, compiled for valijson's Draft 7
// validator. Only schemas that mean the same under Draft 7 and under JSON
// Schema 2020-12, the dialect that MCP clients read, compile: see
// compile() and the README, "Schemas".
//
// valijson is confined to mcpschema.cpp, the one translation unit built with
// exceptions, because its parser throws.
class McpSchema
{
public:
    McpSchema();
    ~McpSchema();
    McpSchema(const McpSchema&) = delete;
    McpSchema& operator=(const McpSchema&) = delete;

    // Checks `schema` (a root with "type": "object", and only the keywords
    // that both dialects share) and compiles it. Returns an empty string on
    // success, else why the schema is refused, with the JSON pointer of the
    // place.
    QString compile(const QJsonObject& schema);

    // The errors of `value` against the compiled schema, one
    // "<pointer>: <description>" for each, the pointer relative to the
    // value's root ("" for the root itself). Empty when `value` matches.
    [[nodiscard]] QStringList validate(const QJsonValue& value) const;

private:
    struct Private;
    std::unique_ptr<Private> d;
};

} // namespace PhosphorMcp
