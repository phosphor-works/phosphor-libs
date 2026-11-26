// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <PhosphorMcp/McpTool.h>
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
} // namespace PhosphorMcp

namespace PhosphorMcpIpc {

// Offers the calls of an application's phosphor-ipc socket that a manifest
// allows as the tools of an McpServer. Each tool call becomes a call on
// the socket.
//
//     McpIpcBridge bridge(manifest);
//     PhosphorMcp::McpServer server;
//     QObject::connect(&bridge, &McpIpcBridge::prepared, &server, [&] {
//         if (!bridge.addTools(server) || !server.start()) {
//             QCoreApplication::exit(1);
//         }
//     });
//     QObject::connect(&bridge, &McpIpcBridge::failed, &server, [] {
//         QCoreApplication::exit(1);
//     });
//     bridge.prepare(socketPath);
//
// At the start
//   - prepare() asks the socket one time for the schema of each target
//     that the manifest names. It builds the input schema of each tool
//     from the parameters of its method. The tools stay the same for the
//     life of the bridge, as McpServer wants.
//   - A tool that the application cannot serve is left out, with a line
//     in warnings(). The cases are these:
//       - a target or a method that the socket does not have
//       - a method with more parameters than a call takes
//       - a parameter of a type that this version of the bridge does not
//         know
//       - two parameters with the same name
//       - a tool that McpServer::toolProblem() refuses
//   - A manifest that cannot choose one overload of a method is an error,
//     and failed() comes. So is a chosen method with a parameter that has
//     no name.
//
// A tool call
//   - The named arguments become the positional arguments of the call, in
//     the order of the method's parameters.
//   - The result of the method is the result of the tool. An object is
//     structured content, a string is text, and any other value is its
//     JSON as text.
//   - An error of the socket is a failed tool call, with its code and
//     message, and a line "detail: {...}" when it has a detail.
//   - A closed socket fails the calls that wait, and the next call
//     connects again. The bridge still maps the arguments as the schema of
//     the first connection said. An application that changed a method
//     meanwhile gets them in the old order.
//   - An MCP cancel does not reach the method. The answer is then dropped.
//     Events that a target sends are dropped too.
//
// Lifetime
//   - Single-threaded.
//   - Keep the bridge alive while the server runs: each tool handler calls
//     the bridge.
//   - A slot of prepared() or failed() must not destroy the bridge.
//     deleteLater() is fine there.
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
    // prepared() or failed() follows one time, from the event loop, never
    // from inside this call. Call it one time.
    //
    // A manifest without tools connects to nothing, and prepared() follows
    // with no tools. failed() follows in each of these cases:
    //   - McpIpcManifest::problem() refuses the manifest.
    //   - The path is not absolute. QLocalSocket takes such a name as a
    //     file in the temporary directory.
    //   - `timeoutMs` is under 1.
    //   - The socket cannot be reached, closes, or does not answer within
    //     `timeoutMs` milliseconds.
    //   - The socket answers a schema request with an error other than
    //     NO_SUCH_TARGET, or with a result that is no schema.
    //   - The manifest cannot choose one overload, or a chosen method has a
    //     parameter without a name (see the class comment).
    void prepare(const QString& socketPath, int timeoutMs = DefaultPrepareTimeoutMs);

    // The tools that prepare() built, in the order of the manifest. Empty
    // before prepared() and after failed().
    [[nodiscard]] QList<PhosphorMcp::McpTool> tools() const;
    // What prepare() left out, and why, one line each. The bridge does not
    // log them. The program that owns it shows them where its user reads.
    [[nodiscard]] QStringList warnings() const;

    // Adds each tool to `server`, with a handler that makes its call. Call
    // it after prepared(), and before the server starts. It adds all tools
    // or none. It returns false, with the reason in `error`, in each of
    // these cases, and clears `error` otherwise:
    //   - prepared() has not come, or failed() came.
    //   - The server has a tool with one of the names already.
    //   - The bridge has a tool to add, and the server takes none, because
    //     it runs.
    bool addTools(PhosphorMcp::McpServer& server, QString* error = nullptr);

Q_SIGNALS:
    void prepared();
    void failed(const QString& error);

private:
    struct Private;
    std::unique_ptr<Private> d;
};

} // namespace PhosphorMcpIpc
