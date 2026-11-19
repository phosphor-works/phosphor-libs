// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <PhosphorMcp/phosphormcp_export.h>

#include <QJsonObject>
#include <QString>

#include <optional>

namespace PhosphorMcp {

// The definition of one tool. tools/list shows all of it but the client
// capabilities that a call needs, which only the server reads.
// McpServer::addTool checks the name, and that both schemas are ones the
// server can check arguments against (see the README, "Schemas").
//
// The behavior hints are the four of the spec's ToolAnnotations. A client
// assumes the least safe value for a hint the tool does not give (not
// read-only, destructive, not idempotent, open world), so a tool sets all
// four, most simply with setHints.
class PHOSPHORMCP_EXPORT McpTool
{
public:
    struct Hints
    {
        bool readOnly = false;
        bool destructive = true;
        bool idempotent = false;
        bool openWorld = true;
    };

    McpTool() = default;
    explicit McpTool(const QString& name);

    [[nodiscard]] QString name() const;
    McpTool& setName(const QString& name);

    // A short name for people. Omitted from the list when empty.
    [[nodiscard]] QString title() const;
    McpTool& setTitle(const QString& title);

    // What the tool does, for the model. Omitted from the list when empty.
    [[nodiscard]] QString description() const;
    McpTool& setDescription(const QString& description);

    // A JSON Schema whose root has "type": "object". Empty means a tool
    // without arguments: {"type":"object","additionalProperties":false}.
    [[nodiscard]] QJsonObject inputSchema() const;
    McpTool& setInputSchema(const QJsonObject& schema);

    // The schema of the result's structured content, which may be any JSON
    // value. Empty means none. A tool with one must answer every call that
    // is not an error with structured content that matches it.
    [[nodiscard]] QJsonObject outputSchema() const;
    McpTool& setOutputSchema(const QJsonObject& schema);

    // Each hint is sent only when set.
    [[nodiscard]] std::optional<bool> readOnlyHint() const;
    McpTool& setReadOnlyHint(bool value);
    [[nodiscard]] std::optional<bool> destructiveHint() const;
    McpTool& setDestructiveHint(bool value);
    [[nodiscard]] std::optional<bool> idempotentHint() const;
    McpTool& setIdempotentHint(bool value);
    [[nodiscard]] std::optional<bool> openWorldHint() const;
    McpTool& setOpenWorldHint(bool value);
    // Sets all four.
    McpTool& setHints(const Hints& hints);

    // The client capabilities a call needs, in the shape of the request's
    // io.modelcontextprotocol/clientCapabilities. Each member names a
    // capability, and its value is an object of the options the call needs
    // in it, {} for none. A call whose request does not declare each of them
    // as an object (and, for a non-empty value, each option inside it, the
    // same way) is refused with ErrorCode::MissingRequiredClientCapability.
    // Empty means none. The server only checks the declaration, and sends
    // no request to the client itself (see the README, "Not in the
    // library"). The Tool object of tools/list leaves this out.
    [[nodiscard]] QJsonObject requiredClientCapabilities() const;
    McpTool& setRequiredClientCapabilities(const QJsonObject& capabilities);

    // The Tool object of tools/list.
    [[nodiscard]] QJsonObject toJson() const;

private:
    QString m_name;
    QString m_title;
    QString m_description;
    QJsonObject m_inputSchema;
    QJsonObject m_outputSchema;
    std::optional<bool> m_readOnly;
    std::optional<bool> m_destructive;
    std::optional<bool> m_idempotent;
    std::optional<bool> m_openWorld;
    QJsonObject m_requiredCapabilities;
};

} // namespace PhosphorMcp
