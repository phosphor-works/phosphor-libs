// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorMcp/McpTool.h>

#include <QLatin1StringView>

namespace PhosphorMcp {

McpTool::McpTool(const QString& name)
    : m_name(name)
{
}

QString McpTool::name() const
{
    return m_name;
}

McpTool& McpTool::setName(const QString& name)
{
    m_name = name;
    return *this;
}

QString McpTool::title() const
{
    return m_title;
}

McpTool& McpTool::setTitle(const QString& title)
{
    m_title = title;
    return *this;
}

QString McpTool::description() const
{
    return m_description;
}

McpTool& McpTool::setDescription(const QString& description)
{
    m_description = description;
    return *this;
}

QJsonObject McpTool::inputSchema() const
{
    if (m_inputSchema.isEmpty()) {
        // The spec's form for a tool without arguments.
        return QJsonObject{
            {QLatin1StringView("type"), QStringLiteral("object")},
            {QLatin1StringView("additionalProperties"), false},
        };
    }
    return m_inputSchema;
}

McpTool& McpTool::setInputSchema(const QJsonObject& schema)
{
    m_inputSchema = schema;
    return *this;
}

QJsonObject McpTool::outputSchema() const
{
    return m_outputSchema;
}

McpTool& McpTool::setOutputSchema(const QJsonObject& schema)
{
    m_outputSchema = schema;
    return *this;
}

std::optional<bool> McpTool::readOnlyHint() const
{
    return m_readOnly;
}

McpTool& McpTool::setReadOnlyHint(bool value)
{
    m_readOnly = value;
    return *this;
}

std::optional<bool> McpTool::destructiveHint() const
{
    return m_destructive;
}

McpTool& McpTool::setDestructiveHint(bool value)
{
    m_destructive = value;
    return *this;
}

std::optional<bool> McpTool::idempotentHint() const
{
    return m_idempotent;
}

McpTool& McpTool::setIdempotentHint(bool value)
{
    m_idempotent = value;
    return *this;
}

std::optional<bool> McpTool::openWorldHint() const
{
    return m_openWorld;
}

McpTool& McpTool::setOpenWorldHint(bool value)
{
    m_openWorld = value;
    return *this;
}

McpTool& McpTool::setHints(const Hints& hints)
{
    m_readOnly = hints.readOnly;
    m_destructive = hints.destructive;
    m_idempotent = hints.idempotent;
    m_openWorld = hints.openWorld;
    return *this;
}

QJsonObject McpTool::requiredClientCapabilities() const
{
    return m_requiredCapabilities;
}

McpTool& McpTool::setRequiredClientCapabilities(const QJsonObject& capabilities)
{
    m_requiredCapabilities = capabilities;
    return *this;
}

QJsonObject McpTool::toJson() const
{
    QJsonObject tool{
        {QLatin1StringView("name"), m_name},
        {QLatin1StringView("inputSchema"), inputSchema()},
    };
    if (!m_title.isEmpty()) {
        tool.insert(QLatin1StringView("title"), m_title);
    }
    if (!m_description.isEmpty()) {
        tool.insert(QLatin1StringView("description"), m_description);
    }
    if (!m_outputSchema.isEmpty()) {
        tool.insert(QLatin1StringView("outputSchema"), m_outputSchema);
    }
    QJsonObject annotations;
    if (m_readOnly) {
        annotations.insert(QLatin1StringView("readOnlyHint"), *m_readOnly);
    }
    if (m_destructive) {
        annotations.insert(QLatin1StringView("destructiveHint"), *m_destructive);
    }
    if (m_idempotent) {
        annotations.insert(QLatin1StringView("idempotentHint"), *m_idempotent);
    }
    if (m_openWorld) {
        annotations.insert(QLatin1StringView("openWorldHint"), *m_openWorld);
    }
    if (!annotations.isEmpty()) {
        tool.insert(QLatin1StringView("annotations"), annotations);
    }
    return tool;
}

} // namespace PhosphorMcp
