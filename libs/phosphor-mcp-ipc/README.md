<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team -->
<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->

# phosphor-mcp-ipc

phosphor-mcp-ipc is an MCP server for an application that has a phosphor-ipc socket. It offers the calls of the socket
to an AI agent as tools. The application lists the calls that an agent can make in a manifest.

## Responsibility

An application that serves its calls on a phosphor-ipc socket (`IpcRouter`) gets an MCP server without new code. It
installs a manifest, and the MCP client of the user starts the program `phosphor-mcp-ipc` with the socket and the
manifest.

The program is an MCP server on stdio ([`phosphor-mcp`](../phosphor-mcp/README.md)) and a client of the socket
([`phosphor-ipc`](../phosphor-ipc/README.md), `IpcClient`). Its code is in a library of its own, so that the MCP
core does not depend on phosphor-ipc.

The library owns these parts:

- **`McpIpcManifest`** reads the manifest and refuses each value that the format does not name.
- **`McpIpcBridge`** asks the socket for its schemas at the start and builds the tools from them. It gives each tool
  a handler that makes its call on the socket.
- **`phosphor-mcp-ipc`** is the program. It owns the command line and the lifetime of the server.

### Why a manifest

A socket serves each public invokable method and public slot of each target to any process of the user. That is
correct for scripts and key bindings. An agent acts on text that it read, and that text can come from anyone. Thus an
agent gets only the calls that the application chose for it.

The server offers the calls that the manifest lists, and no others. It never makes a tool from the schema alone. A
call that the manifest does not list has no tool, so the server refuses a request for it as an unknown tool. The
manifest also gives each tool its description and its behavior hints, which the schema of the socket does not have.

## Key types

| Type | Purpose |
|------|---------|
| `PhosphorMcpIpc::McpIpcManifest` | The list of tools. `parse(json, &error)` reads JSON text, and `load(path, &error, &missing)` reads a file. `problem()` checks a manifest that code built. |
| `PhosphorMcpIpc::McpIpcManifestTool` | One entry of the manifest. The target and the method of the call, the name, title and description of the tool, the optional `params`, and the four optional hints. |
| `PhosphorMcpIpc::McpIpcBridge` | The link between a socket and an `McpServer`. `prepare(socketPath, timeoutMs)`, signals `prepared()` and `failed(error)`, `tools()`, `warnings()`, and `addTools(server, &error)`. |
| `phosphor-mcp-ipc` | The program that an MCP client starts. The CMake package exports it as `PhosphorMcpIpc::phosphor-mcp-ipc`. |

## Use

The examples use one application. Its socket is at `$XDG_RUNTIME_DIR/app.sock`, and it has a target `panes` with the
method `rename(pane, title)`.

### The application

The application starts its router at a path and registers its targets:

```cpp
#include <PhosphorIpc/IpcRouter.h>

#include <QStandardPaths>

PhosphorIpc::IpcRouter router;
Panes panes;  // a QObject with: Q_INVOKABLE QString rename(int pane, const QString& title);
const QString runtimeDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
if (!router.registerTarget(QStringLiteral("panes"), &panes)
    || !router.start(runtimeDir + QStringLiteral("/app.sock"))) {
    qWarning("cannot start the socket");
}
```

It installs a manifest, for example at `/usr/share/app/mcp.json`:

```json
{"tools": [
    {"call": "panes.rename", "name": "rename_pane", "title": "Rename a pane",
     "description": "Renames a pane. pane is the id that list_panes gives.",
     "params": ["pane", "title"],
     "annotations": {"readOnly": false, "destructive": false, "idempotent": true, "openWorld": false}}
]}
```

The user, or the application for the user, adds the server to the configuration of the MCP client:

```json
{"mcpServers": {"app": {"command": "phosphor-mcp-ipc",
                        "args": ["--socket", "/run/user/1000/app.sock",
                                 "--manifest", "/usr/share/app/mcp.json"]}}}
```

The socket path belongs to one user. Many clients pass the arguments as the configuration has them, with no
variables. For such a client, the configuration has the absolute path of that user's socket. The example is for
the user with the ID 1000.

### The program

```
phosphor-mcp-ipc --socket PATH --manifest FILE
```

| Option | Meaning |
| --- | --- |
| `--socket PATH` | The socket of the application, as an absolute path. Required. |
| `--manifest FILE` | The manifest. Required. |
| `--help`, `--help-all`, `--version` | Print the help or the version, and end with exit code 0. |

The program refuses a socket path that is not absolute. `QLocalSocket` takes such a name as a file in the temporary
directory, where another user can listen.

| Exit code | Meaning |
| --- | --- |
| 0 | The client ended the server. It closed stdin or stdout, or it sent a termination signal. |
| 1 | The server cannot run, or its input or output failed. See [At the start](#at-the-start). |
| 2 | The command line is wrong. The program prints the reason and a usage line. |

The program writes each of its reasons and warnings to stderr, with the prefix `phosphor-mcp-ipc:`. The libraries
also log to stderr, without that prefix, so one fact can show two times. stdout holds only protocol messages.

The end of stdin ends the server. The server sends the replies that it has written, and drops each tool call that
still waits. The method of such a call can still run in the application.

### Check a manifest

Start the program by hand and send one `tools/list` request. Keep stdin open until the answer comes:

```sh
(printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{}}}}'; sleep 1) \
    | phosphor-mcp-ipc --socket "$XDG_RUNTIME_DIR/app.sock" --manifest mcp.json
```

stdout shows the tools with their input schemas. stderr shows one line for each tool or target that the program
left out, and the reason when the program cannot start. For a `tools/call` request, keep stdin open until the method
answers. A call that still waits at the end of stdin gets no answer.

### As a library

`McpIpcBridge` is for an application that runs its own MCP server, with its own tools next to the tools of the
socket:

```cpp
#include <PhosphorMcp/McpServer.h>
#include <PhosphorMcpIpc/McpIpcBridge.h>
#include <PhosphorMcpIpc/McpIpcManifest.h>

#include <QCoreApplication>
#include <QDebug>

#include <optional>

QString error;
const std::optional<PhosphorMcpIpc::McpIpcManifest> manifest = PhosphorMcpIpc::McpIpcManifest::load(path, &error);
if (!manifest) {
    qWarning() << error;
    return 1;
}
// The bridge first. Each tool handler of the server calls the bridge.
PhosphorMcpIpc::McpIpcBridge bridge(*manifest);
PhosphorMcp::McpServer server;
QObject::connect(&bridge, &PhosphorMcpIpc::McpIpcBridge::prepared, &server, [&] {
    if (!server.addTool(ownTool, ownHandler) || !bridge.addTools(server, &error) || !server.start()) {
        QCoreApplication::exit(1);
    }
});
QObject::connect(&bridge, &PhosphorMcpIpc::McpIpcBridge::failed, &server, [](const QString& reason) {
    qWarning() << reason;
    QCoreApplication::exit(1);
});
bridge.prepare(socketPath);
```

| Subject | Rule |
| --- | --- |
| `prepare()` | Call it one time. `prepared()` or `failed(error)` follows one time, from the event loop, never inside `prepare()`. |
| `addTools()` | Call it after `prepared()` and before the server starts. It adds all tools or none. It returns `false` when the server already has a tool with one of the names, and when the server runs. |
| `tools()` | The tools that `prepare()` built, in the order of the manifest. |
| `warnings()` | The lines for the tools that `prepare()` left out. The bridge does not log them. The program that owns the bridge shows them to its user. |
| Lifetime | Keep the bridge alive while the server runs. |

## The manifest

The manifest is a JSON object with one member, `tools`, an array. Each entry has these members:

| Member | Required | Meaning |
|--------|----------|---------|
| `call` | yes | `TARGET.METHOD`. The last dot divides the two parts, so a target name can have dots. |
| `name` | yes | The name of the tool. 1 to 128 characters of `A-Z`, `a-z`, `0-9`, `_`, `-` and `.`. No two tools have the same name. |
| `title` | no | A short name for people. Not empty. |
| `description` | yes | What the tool does, for the model. Not empty. |
| `params` | no | The parameter names of one overload of the method, in order. It chooses that overload. `[]` chooses the overload without parameters. |
| `annotations` | no | `readOnly`, `destructive`, `idempotent` and `openWorld`. Each is a boolean, and each is optional. The server does not send a hint that the manifest does not give. The client then assumes the least safe value. |

The format is strict. The program does not start when the manifest has one of these errors:

- a member that the table does not name
- a value of the wrong type
- no `call`, `name` or `description`
- a `call` without a dot, or with nothing before or after the last dot
- a name that the table does not permit, or that two tools have
- an empty `title` or `description`
- a parameter name that `params` has two times, or an empty one

Thus a typing error shows at the start, and does not silently remove a restriction.

| File at the path | Result |
| --- | --- |
| No file | A server without tools. The program says so on stderr, and does not connect to the socket. |
| A manifest with `"tools": []` | The same. |
| A link to a file that does not exist | Error at the start. |
| A directory, a FIFO or a device | Error at the start. |
| A file larger than 4 MiB | Error at the start. |

## At the start

1. The program reads the manifest. An error ends the program with exit code 1.
2. The program connects to the socket and sends one `schema` request for each target that the manifest names. A
   socket that it cannot reach ends the program with exit code 1. So does a socket that does not answer in 10 seconds.
3. The program builds the tools, in the order of the manifest, and starts the MCP server. The list of tools stays the
   same for the life of the process.

A tool that the application cannot serve is left out, with a line on stderr. The other tools stay:

| Case | Result |
|------|--------|
| A target that the socket does not have | No tools for the target, and one line that names them. |
| A method that its target does not have | No tool. |
| A method with more than 10 parameters | No tool. A call takes at most 10 arguments. |
| A parameter of a type that this version does not know | No tool. |
| Two parameters with the same name | No tool. |
| A tool that the MCP server refuses, for example for a parameter name that is not valid UTF-16 | No tool. |

A manifest that cannot choose one overload is an error of the manifest. The program ends with exit code 1:

| Case | Reason |
|------|--------|
| A method with more than one overload, and no `params` | The error names the method and its overloads. |
| `params` that agree with no overload, in names or in order | The error names the overloads. |
| `params` that choose an overload with as many parameters as another overload | The router chooses an overload by the number of arguments only. It cannot tell the two apart. |
| A parameter without a name (a C++ declaration without one) | No argument can name it. |

A C++ method with default arguments has one overload for each number of arguments, so it needs `params` too.

## The input schema

The input schema of each tool is an object with one property for each parameter of the chosen overload. Each
property is required, and `additionalProperties` is `false`. A method without parameters takes no arguments. Each
property comes from the schema of the socket (`IpcSchemaGenerator`):

| Parameter type | Property |
|----------------|----------|
| `bool` | `{"type": "boolean"}` |
| `int`, `qint64` and the other integers | `{"type": "integer"}` |
| `double`, `float` | `{"type": "number"}` |
| `QString` | `{"type": "string"}` |
| `QStringList` | `{"type": "array", "items": {"type": "string"}}` |
| `QVariantList`, `QJsonArray` | `{"type": "array"}` |
| `QVariantMap`, `QJsonObject` | `{"type": "object"}` |
| `QVariant`, `QJsonValue` | `{}`, which accepts each value |
| each other type | `{"description": "custom QMetaType: <name>"}`, which accepts each value |

For a type in the last row, the router converts the argument to the type. The call fails with `INVALID_ARG` when no
conversion exists, as for a `QObject*` parameter. Do not list such a method in a manifest.

An untyped parameter of a QML function (`function run(value)`) is a `QVariant`. Its property is `{}`, so the model
gets no type for it, and the function must check the value. A typed parameter (`function greet(name: string)`) gets
its type.

The MCP server checks the arguments against the schema before the call. An argument of the wrong type, a missing
argument and an unknown argument are tool errors. They never reach the socket.

## A tool call

The named arguments become the positional `args` of the call, in the order of the parameters of the chosen overload.
Then the server calls the method.

| Answer of the socket | Tool result |
|-------|-------------|
| An object | Structured content. The MCP library also adds its JSON as text. |
| A string | The string as text, without quotation marks or escapes. |
| Each other value (a number, a boolean, an array, `null` for a method that returns nothing) | Its compact JSON as text. |
| An error | A failed call (`isError`) with the text `CODE: message`. A line `detail: {...}` follows when the error has a detail. |

Only an object becomes structured content. A model reads text best for the other values.

A call can wait as long as the method takes. A deferred reply answers when its work ends, and the server serves other
calls during that time. An MCP cancel does not reach the method, because the wire format has no cancel. The server
then drops the answer.

The server makes these errors itself. Each is a failed call, and its text starts with the code:

| Code | When | The next call |
| --- | --- | --- |
| `NOT_CONNECTED` | The server cannot reach the socket. | Connects again. |
| `CONNECTION_CLOSED` | The connection ended before the answer came. The method can have run. | Connects again. |
| `WRITE_QUEUE_FULL` | The application does not read its socket, and too many calls wait. | Can succeed later. |
| `REQUEST_TOO_LARGE` | The arguments make a request of more than 1 MiB. | Succeeds with smaller arguments. |
| `MALFORMED_REPLY` | The application sent an error without a code. | Not affected. |

Each call without a connection connects first. Thus the tools work again when the application is back. The tools,
and the order of their arguments, stay as the first connection gave them. Start the server again after a change of
a method in the application.

## Limits

| Limit | Value | Past the limit |
| --- | --- | --- |
| Size of the manifest | 4 MiB | Error at the start. |
| Time for the schemas at the start | 10 s | Error at the start. |
| Time to connect for a call | 5 s | The call fails with `CONNECTION_CLOSED`. |
| Parameters of a method | 10 | The tool is left out. |
| Size of one call on the socket | 1 MiB | The call fails with `REQUEST_TOO_LARGE`. |
| Bytes of calls that the application has not read | 16 MiB | The call fails with `WRITE_QUEUE_FULL`. |
| Size of one answer of the socket | 64 MiB | The connection ends, and the calls that wait fail with `CONNECTION_CLOSED`. |
| Deferred calls that wait in the application | 256 | The router fails the call with `CALL_FAILED`. |
| Time for an answer | No limit | A deferred call can wait as long as its work. |

## Not supported

- **Events.** The server only makes calls. It does not subscribe to signals, and it drops the events that a target
  sends to a call.
- **A list of tools that changes.** The server builds the tools one time, at the start. A target that comes later
  has no tools until the client starts the server again.
- **Cancel.** An MCP cancel does not stop the method.
- **A name or instructions for each application.** Each server has the name `phosphor-mcp-ipc`.

## Dependencies

| Dependency | Why |
| --- | --- |
| `QtCore` | The only Qt module that the library uses directly. |
| `phosphor-mcp` | Public. `McpIpcBridge::addTools` takes an `McpServer`. |
| `phosphor-ipc` | Private, for `IpcClient`. Its shared library loads QtQml and QtNetwork. |
| Linux | As for phosphor-mcp. The manifest reader also uses `open` and `fstat`. |

## See also

- [`phosphor-mcp`](../phosphor-mcp/README.md): the MCP server library, for an application that writes its own tools.
- [`phosphor-ipc`](../phosphor-ipc/README.md): the socket, its wire format and `IpcClient`.
- `tests/test_phosphor_mcp_ipc_program.cpp`: a client that starts the program and calls its tools.
