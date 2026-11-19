// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorMcp/McpCall.h>
#include <PhosphorMcp/McpServer.h>

#include <utility>

namespace PhosphorMcp {

McpCall::McpCall() = default;

McpCall::McpCall(McpServer* server, quint64 token, const QString& toolName, const QJsonObject& arguments,
                 const QJsonValue& requestId, const QJsonObject& requestMeta, bool hasProgressToken)
    : m_server(server)
    , m_token(token)
    , m_toolName(toolName)
    , m_arguments(arguments)
    , m_requestId(requestId)
    , m_requestMeta(requestMeta)
    , m_hasProgressToken(hasProgressToken)
{
}

McpCall::~McpCall()
{
    abandon();
}

McpCall::McpCall(McpCall&& other) noexcept
    : m_server(std::move(other.m_server))
    , m_token(std::exchange(other.m_token, 0))
    , m_toolName(std::move(other.m_toolName))
    , m_arguments(std::move(other.m_arguments))
    , m_requestId(std::move(other.m_requestId))
    , m_requestMeta(std::move(other.m_requestMeta))
    , m_hasProgressToken(std::exchange(other.m_hasProgressToken, false))
{
    other.m_server.clear();
}

McpCall& McpCall::operator=(McpCall&& other) noexcept
{
    if (this != &other) {
        abandon();
        m_server = std::move(other.m_server);
        other.m_server.clear();
        m_token = std::exchange(other.m_token, 0);
        m_toolName = std::move(other.m_toolName);
        m_arguments = std::move(other.m_arguments);
        m_requestId = std::move(other.m_requestId);
        m_requestMeta = std::move(other.m_requestMeta);
        m_hasProgressToken = std::exchange(other.m_hasProgressToken, false);
    }
    return *this;
}

void McpCall::abandon()
{
    if (m_server && m_token != 0) {
        m_server->abandonCall(m_token);
    }
    m_token = 0;
}

bool McpCall::isValid() const
{
    return m_server && m_token != 0 && m_server->callIsValid(m_token);
}

bool McpCall::isCancelled() const
{
    return m_server && m_token != 0 && m_server->callIsCancelled(m_token);
}

QString McpCall::toolName() const
{
    return m_toolName;
}

QJsonObject McpCall::arguments() const
{
    return m_arguments;
}

QJsonValue McpCall::requestId() const
{
    return m_requestId;
}

QJsonObject McpCall::requestMeta() const
{
    return m_requestMeta;
}

bool McpCall::hasProgressToken() const
{
    return m_hasProgressToken;
}

bool McpCall::finish(const McpResult& result)
{
    if (!m_server || m_token == 0) {
        return false;
    }
    return m_server->finishCall(m_token, result);
}

bool McpCall::fail(const QString& text)
{
    return finish(McpResult::error(text));
}

bool McpCall::progress(double progress, std::optional<double> total, const QString& message)
{
    if (!m_server || m_token == 0) {
        return false;
    }
    return m_server->progressCall(m_token, progress, total, message);
}

void McpCall::onCancelled(std::function<void()> callback)
{
    if (!m_server || m_token == 0 || !callback) {
        return;
    }
    m_server->setCancelCallback(m_token, std::move(callback));
}

} // namespace PhosphorMcp
