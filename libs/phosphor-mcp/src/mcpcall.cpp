// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorMcp/McpCall.h>
#include <PhosphorMcp/McpServer.h>

#include "mcplogging.h"

#include <QThread>

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

// Qt's move assignments swap, so each member is exchanged with an empty
// value: the moved-from object holds nothing of the call.
McpCall::McpCall(McpCall&& other) noexcept
    : m_server(std::exchange(other.m_server, nullptr))
    , m_token(std::exchange(other.m_token, 0))
    , m_toolName(std::exchange(other.m_toolName, QString()))
    , m_arguments(std::exchange(other.m_arguments, QJsonObject()))
    , m_requestId(std::exchange(other.m_requestId, QJsonValue()))
    , m_requestMeta(std::exchange(other.m_requestMeta, QJsonObject()))
    , m_hasProgressToken(std::exchange(other.m_hasProgressToken, false))
{
}

McpCall& McpCall::operator=(McpCall&& other) noexcept
{
    if (this != &other) {
        // `other` is read whole before the old call is abandoned. Abandoning
        // destroys the old call's cancel callback, which may own `other` or
        // this object.
        McpCall incoming(std::move(other));
        swapWith(incoming);
        // `incoming` now holds the old call, and its destruction at the end
        // of this block abandons it. Nothing touches this object or `other`
        // after that.
    }
    return *this;
}

void McpCall::swapWith(McpCall& other) noexcept
{
    m_server.swap(other.m_server);
    std::swap(m_token, other.m_token);
    m_toolName.swap(other.m_toolName);
    m_arguments.swap(other.m_arguments);
    std::swap(m_requestId, other.m_requestId);
    m_requestMeta.swap(other.m_requestMeta);
    std::swap(m_hasProgressToken, other.m_hasProgressToken);
}

McpServer* McpCall::serverOnThisThread() const
{
    McpServer* server = m_server.data();
    if (!server || m_token == 0) {
        return nullptr;
    }
    // The server's books are not thread-safe. Off its thread the call does
    // nothing, and waits for its answer on the server's thread.
    const bool onServerThread = server->thread() == QThread::currentThread();
    Q_ASSERT(onServerThread);
    if (!onServerThread) {
        qCWarning(lcMcp).noquote() << "McpCall: used off the server's thread, where it does nothing";
        return nullptr;
    }
    return server;
}

void McpCall::abandon()
{
    McpServer* server = serverOnThisThread();
    // The token goes first: abandonCall() destroys the call's cancel
    // callback, which may own this object.
    const quint64 token = std::exchange(m_token, 0);
    if (server) {
        server->abandonCall(token);
    }
}

bool McpCall::isValid() const
{
    McpServer* server = serverOnThisThread();
    return server && server->callIsValid(m_token);
}

bool McpCall::isCancelled() const
{
    if (m_token == 0) {
        return false;
    }
    // The server went while the call ran unanswered: an answered call holds
    // no token.
    if (!m_server) {
        return true;
    }
    McpServer* server = serverOnThisThread();
    return server && server->callIsCancelled(m_token);
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
    McpServer* server = serverOnThisThread();
    if (!server || !server->callIsValid(m_token)) {
        return false;
    }
    // The answer takes the call, so this object holds none from here on.
    // The token goes before finishCall(), which destroys the call's cancel
    // callback, and that may own this object.
    const quint64 token = std::exchange(m_token, 0);
    return server->finishCall(token, result);
}

bool McpCall::fail(const QString& text)
{
    return finish(McpResult::error(text));
}

bool McpCall::progress(double progress, std::optional<double> total, const QString& message)
{
    McpServer* server = serverOnThisThread();
    return server && server->progressCall(m_token, progress, total, message);
}

void McpCall::onCancelled(std::function<void()> callback)
{
    if (!callback) {
        return;
    }
    if (McpServer* server = serverOnThisThread()) {
        // May run the callback, or destroy the one it replaces, and either
        // may destroy this object: nothing touches it after the call.
        server->setCancelCallback(m_token, std::move(callback));
    }
}

} // namespace PhosphorMcp
