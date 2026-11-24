// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <PhosphorMcpIpc/McpIpcManifest.h>
#include <PhosphorMcpIpc/phosphormcpipc_export.h>

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QtCore/qtclasshelpermacros.h>

#include <memory>

namespace PhosphorMcp {
class McpServer;
class McpTool;
} // namespace PhosphorMcp

namespace PhosphorMcpIpc {

// Offers the calls of an application's phosphor-ipc socket that a manifest
// allows as the tools of an McpServer, and makes each tool call a call on
// the socket.
//
//     McpIpcBridge bridge(manifest);
//     QObject::connect(&bridge, &McpIpcBridge::prepared, [&] {
//         bridge.addTools(server);
//         server.start();
//     });
//     bridge.prepare(socketPath);
//
// prepare() asks the socket for the schema of each target that the
// manifest names, once, and builds each tool's input schema from the
// parameters of its function. The tools stay the same for the life of the
// bridge, as McpServer wants. A target that the socket does not have, and a
// function that its target does not have, give no tool and a warning. A
// manifest that cannot choose one overload of a function is an error.
//
// A tool call maps its named arguments to the positional arguments of the
// call, in the order of the function's parameters, and answers with the
// result. An error of the socket is a failed tool call, with its code and
// message. A closed socket fails the calls that wait, and the next call
// connects again.
//
// Single-threaded. The bridge must outlive the server whose tools it added.
class PHOSPHORMCPIPC_EXPORT McpIpcBridge : public QObject
{
    Q_OBJECT
public:
    // How long prepare() waits for the schemas by default.
    static constexpr int DefaultPrepareTimeoutMs = 10000;

    explicit McpIpcBridge(McpIpcManifest manifest, QObject* parent = nullptr);
    ~McpIpcBridge() override;
    Q_DISABLE_COPY_MOVE(McpIpcBridge)

    // Connects to the socket at `socketPath` and builds the tools.
    // prepared() or failed() follows once, from the event loop. A manifest
    // without tools connects to nothing, and prepared() follows with no
    // tools. A socket that cannot be reached, and one that does not answer
    // within `timeoutMs`, fail. Call it once.
    void prepare(const QString& socketPath, int timeoutMs = DefaultPrepareTimeoutMs);

    // The tools that prepare() built, in the order of the manifest.
    [[nodiscard]] QList<PhosphorMcp::McpTool> tools() const;
    // What prepare() left out, and why, one line each. Each is also logged
    // as a warning of the category "phosphormcpipc".
    [[nodiscard]] QStringList warnings() const;

    // Adds each tool to `server`, with a handler that makes its call. After
    // prepared(), and before the server starts. False, with the reason in
    // `error`, when the server refuses a tool.
    bool addTools(PhosphorMcp::McpServer& server, QString* error = nullptr);

Q_SIGNALS:
    void prepared();
    void failed(const QString& error);

private:
    struct Private;
    std::unique_ptr<Private> d;
};

} // namespace PhosphorMcpIpc
