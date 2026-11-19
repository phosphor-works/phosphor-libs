// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <PhosphorMcp/McpResult.h>
#include <PhosphorMcp/phosphormcp_export.h>

#include <QJsonObject>
#include <QJsonValue>
#include <QPointer>
#include <QString>

#include <functional>
#include <optional>

namespace PhosphorMcp {

class McpServer;

// One tools/call, handed to the tool's handler after the server checked the
// request and the arguments. The handler answers it then or later, through
// finish() or fail(), and keeps it meanwhile:
//
//     server.addTool(tool, [this](PhosphorMcp::McpCall call) {
//         Job& job = m_jobs.emplace_back(std::move(call));
//         job.call.onCancelled([this, &job] { stop(job); });
//         start(job); // answers through job.call when it ends
//     });
//
// Calls run at the same time: the server reads the next request while this
// one waits, and each reply goes out when its call finishes, in the order
// they finish.
//
// The first answer stands. After it, after a cancellation and after the
// server ended, finish(), fail() and progress() do nothing and return
// false. A call destroyed unanswered is answered with a JSON-RPC internal
// error, so that the client does not wait for an answer that never comes.
//
// Move-only. Use it on the server's thread. It holds the server weakly: a
// call that outlives the server does nothing.
class PHOSPHORMCP_EXPORT McpCall
{
public:
    // Holds no call.
    McpCall();
    ~McpCall();
    McpCall(McpCall&& other) noexcept;
    // Abandons the call this object held, as the destructor does.
    McpCall& operator=(McpCall&& other) noexcept;
    McpCall(const McpCall&) = delete;
    McpCall& operator=(const McpCall&) = delete;

    // True while the call can still be answered.
    [[nodiscard]] bool isValid() const;
    // True once the client cancelled the call, or the server ended while
    // it ran.
    [[nodiscard]] bool isCancelled() const;

    [[nodiscard]] QString toolName() const;
    // The arguments, checked against the tool's input schema. An empty
    // object when the request had none.
    [[nodiscard]] QJsonObject arguments() const;
    // The JSON-RPC id of the request: a string or a number.
    [[nodiscard]] QJsonValue requestId() const;
    // The request's params._meta, for the client's info and capabilities.
    [[nodiscard]] QJsonObject requestMeta() const;
    [[nodiscard]] bool hasProgressToken() const;

    // Sends the result. Returns false when there was nothing to answer.
    bool finish(const McpResult& result);
    // Sends a failed result (isError) with one text block. Returns as
    // finish() does.
    bool fail(const QString& text);

    // Sends notifications/progress, when the request asked for it with a
    // progressToken. `progress` must be larger than the last value sent for
    // this call, and finite, and so must `total` when given. Returns false,
    // and sends nothing, when the request has no token, for a value that
    // does not grow, and after the answer.
    bool progress(double progress, std::optional<double> total = std::nullopt, const QString& message = QString());

    // Runs `callback` once when the client cancels the call
    // (notifications/cancelled), or when the server ends while the call
    // runs. Nothing more goes out for the call after it: a later finish()
    // is dropped. On a call that is already cancelled, `callback` runs now,
    // inside this function. The callback may destroy this object.
    void onCancelled(std::function<void()> callback);

private:
    friend class McpServer;
    McpCall(McpServer* server, quint64 token, const QString& toolName, const QJsonObject& arguments,
            const QJsonValue& requestId, const QJsonObject& requestMeta, bool hasProgressToken);

    void abandon();

    QPointer<McpServer> m_server;
    // The server's key for the call. 0 when this object holds none.
    quint64 m_token = 0;
    QString m_toolName;
    QJsonObject m_arguments;
    QJsonValue m_requestId;
    QJsonObject m_requestMeta;
    bool m_hasProgressToken = false;
};

} // namespace PhosphorMcp
