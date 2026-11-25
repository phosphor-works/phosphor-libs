// SPDX-FileCopyrightText: 2026 Phosphor Works Team
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
//     // m_jobs is a std::list, whose elements stay put, so `job` stays valid
//     // until the job is erased.
//     server.addTool(tool, [this](PhosphorMcp::McpCall call) {
//         Job& job = m_jobs.emplace_back(std::move(call));
//         job.call.onCancelled([this, &job] { stop(job); });
//         start(job); // answers through job.call when it ends
//     });
//
// Calls run at the same time. The server reads the next request while this
// one waits, and each reply goes out when its call finishes, in the order
// they finish.
//
// The first answer stands. After it, after a cancellation and after the
// server ended, finish(), fail() and progress() do nothing and return
// false. A call destroyed unanswered is answered with a JSON-RPC internal
// error, so that the client does not wait for an answer that never comes.
//
// Move-only. A moved-from call holds no call, as a default-constructed one
// does. It is not valid, not cancelled, its accessors return empty values,
// and its destruction sends nothing.
//
// Use it on the server's thread only. Off that thread every function does
// nothing, with a warning (and an assertion in a debug build), and the call
// stays unanswered. It holds the server weakly: a call that outlives the
// server does nothing.
class PHOSPHORMCP_EXPORT McpCall
{
public:
    // Holds no call.
    McpCall();
    ~McpCall();
    // `other` holds no call afterwards.
    McpCall(McpCall&& other) noexcept;
    // Takes the call `other` holds, then abandons the call this object held,
    // as the destructor does. `other` holds no call afterwards.
    McpCall& operator=(McpCall&& other) noexcept;
    McpCall(const McpCall&) = delete;
    McpCall& operator=(const McpCall&) = delete;

    // True while the call can still be answered.
    [[nodiscard]] bool isValid() const;
    // True once the client cancelled the call, or the server ended or was
    // destroyed while the call ran unanswered. It stays true.
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

    // Sends the result. True when the reply was queued for the client, which
    // is not a promise that the client reads it. False when there was
    // nothing to answer (see above), and when the output is already gone.
    // Also false when the tool has an output schema and the result does not
    // match it. The call is answered then all the same, with a JSON-RPC
    // internal error, and holds nothing more.
    bool finish(const McpResult& result);
    // Sends a failed result (isError) with one text block. Returns as
    // finish() does.
    bool fail(const QString& text);

    // Sends notifications/progress, when the request asked for it with a
    // progressToken. `progress` must be finite and larger than the last
    // value sent for this call. `total`, when given, must be finite, and may
    // stay the same or change from one notification to the next. `message`
    // is left out when empty. Returns false, and sends nothing, when the
    // request has no token, for a value that is not finite or does not grow
    // (both with a warning), after the answer, after a cancellation, and
    // while the client leaves more than the server's high-water mark of
    // replies unread (McpServer::setMaxPendingWriteBytes). A dropped
    // notification is not sent later.
    bool progress(double progress, std::optional<double> total = std::nullopt, const QString& message = QString());

    // Runs `callback` once when the client cancels the call
    // (notifications/cancelled), or when the server ends while the call
    // runs. Nothing more goes out for the call after it, and a later
    // finish() is dropped. On a call that is already cancelled, `callback`
    // runs now, inside this function. The callback may destroy this object.
    //
    // One callback per call. A second one replaces the first, which is
    // destroyed without running. An empty `callback` changes nothing. On a
    // call that holds nothing (answered, moved from, or whose server is
    // gone) it does nothing, and the callback is dropped. Destroying the
    // server runs no callback.
    void onCancelled(std::function<void()> callback);

private:
    friend class McpServer;
    McpCall(McpServer* server, quint64 token, const QString& toolName, const QJsonObject& arguments,
            const QJsonValue& requestId, const QJsonObject& requestMeta, bool hasProgressToken);

    // The server, when this object holds a call and runs on its thread.
    McpServer* serverOnThisThread() const;
    void abandon();
    void swapWith(McpCall& other) noexcept;

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
