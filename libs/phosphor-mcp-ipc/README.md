<!-- SPDX-FileCopyrightText: 2026 fuddlesworth
     SPDX-License-Identifier: LGPL-2.1-or-later -->

# phosphor-mcp-ipc

> An MCP server that offers the calls of an application's phosphor-ipc
> socket to an AI agent as tools, as far as the application's manifest
> allows them.

## Responsibility

An application that already serves its calls on a phosphor-ipc socket
(`IpcRouter`) gets an MCP server without code of its own. It installs a
manifest that lists the calls an agent may make, and the user's MCP
client starts `phosphor-mcp-ipc` with the socket and the manifest:

```json
{"mcpServers": {"app": {"command": "phosphor-mcp-ipc",
                        "args": ["--socket", "/run/user/1000/app.sock",
                                 "--manifest", "/usr/share/app/mcp.json"]}}}
```

The server is an MCP server on stdio ([`phosphor-mcp`](../phosphor-mcp/README.md))
and a client of the socket ([`phosphor-ipc`](../phosphor-ipc/README.md),
`IpcClient`). It is a library of its own so that the MCP core does not
depend on phosphor-ipc.

The library owns:

- **`McpIpcManifest`**: the manifest's shape, read strictly.
- **`McpIpcBridge`**: the schema requests at start, the tools built from
  them, and a handler for each tool that makes its call on the socket.

The program `phosphor-mcp-ipc` owns the command line and the server's
lifetime.

## Why a manifest

A socket serves every public method of every target to any process of
the user, which is right for scripts and key bindings. An agent is
different: it acts on text that it read, which may come from anyone, so
it gets only the calls that the application chose for it. The server
offers what the manifest lists and nothing else. It never derives a tool
from the schema alone, and a call that the manifest does not list has no
tool, so the client's request for it is refused as an unknown tool. The
manifest also gives each tool the description and behavior hints that the
model and the client read, which the schema does not have.

## The manifest

A JSON file that the application installs, in exactly this shape:

```json
{"tools": [
    {"call": "panes.rename", "name": "rename_pane", "title": "Rename a pane",
     "description": "Renames a pane. pane is the id that list_panes gives.",
     "params": ["pane", "title"],
     "annotations": {"readOnly": false, "destructive": false, "idempotent": true, "openWorld": false}}
]}
```

| Member | Required | Meaning |
|--------|----------|---------|
| `call` | yes | `TARGET.METHOD`, split at the last dot, since a method name has none. |
| `name` | yes | The tool's name, 1 to 128 of `[A-Za-z0-9_.-]`, unique in the manifest. |
| `title` | no | A short name for people. |
| `description` | yes | What the tool does, for the model. |
| `params` | no | The parameter names of one overload of the method, in order. It chooses that overload. `[]` chooses the overload without parameters. |
| `annotations` | no | `readOnly`, `destructive`, `idempotent` and `openWorld`, each a boolean, each optional. A hint that is not given is not sent, and the client then assumes the least safe value. |

The shape is strict. A member that the table does not name, a value of
the wrong type, a `call` without a dot, a name that two tools use and
`params` that names a parameter twice are errors, and the program does
not start. A typo then shows at once, and does not silently drop a
restriction.

No manifest file at the path, or `"tools": []`, gives a server without
tools. The program says so on stderr and runs: the application has not
offered anything yet. It does not connect to the socket then.

## At start

1. The program reads the manifest. An error ends it with exit code 1.
2. It connects to the socket and sends one `schema` request for each
   target that the manifest names. A socket that cannot be reached, or
   that does not answer within 10 seconds, ends it with exit code 1.
3. It builds the tools, in the order of the manifest, and starts the MCP
   server. The tool list stays the same for the life of the process.

What the socket lacks:

| Case | Result |
|------|--------|
| A target the socket does not have | No tools for it, and a warning on stderr that names them. |
| A method its target does not have | No tool, and a warning. |
| A method with more than one overload, without `params` | Error at start, which names the method and its overloads. |
| `params` that match no overload (names or order) | Error at start. |
| `params` that choose an overload with as many parameters as another | Error at start. The router chooses an overload by the number of arguments alone, so it could never call that one. |
| A parameter without a name (a C++ declaration without one) | Error at start, since no argument could name it. |

A C++ method with default arguments has an overload for each number of
arguments that moc generates, so it needs `params` too.

## The input schema

Each tool's input schema is an object with one property for each
parameter of the chosen overload, all of them required, and
`additionalProperties: false`. A method without parameters takes no
arguments. Each property is the parameter's fragment of the phosphor-ipc
schema (`IpcSchemaGenerator`) without its `name`:

| Parameter type | Property |
|----------------|----------|
| `bool` | `{"type": "boolean"}` |
| `int`, `qint64` and the other integers | `{"type": "integer"}` |
| `double`, `float` | `{"type": "number"}` |
| `QString` | `{"type": "string"}` |
| `QStringList` | `{"type": "array", "items": {"type": "string"}}` |
| `QVariantList`, `QJsonArray` | `{"type": "array"}` |
| `QVariantMap`, `QJsonObject` | `{"type": "object"}` |
| `QVariant`, `QJsonValue` | `{}`: any value |
| any other type | `{"description": "custom QMetaType: <name>"}`: any value |

An untyped parameter of a QML function (`function run(value)`) is a
`QVariant`, so its property is the open schema `{}`, which takes any JSON
value: the model gets no type for it, and the function must check what it
gets. A typed one (`function greet(name: string)`) gets its type. The
MCP server checks the arguments against the schema before the call, so an
argument of the wrong type, a missing one and an unknown one are tool
errors that never reach the socket.

## A tool call

The named arguments become the positional `args` of the call, in the
order of the chosen overload's parameters, and the server calls the
method. The answer:

| Reply | Tool result |
|-------|-------------|
| An object | Structured content. The library adds its JSON as text too. |
| A string | The string as text, without quotes or escapes. |
| Any other value (a number, a boolean, an array, null for a method that returns nothing) | Its compact JSON as text. |
| An error | A failed call (`isError`) with the text `CODE: message`, and a line `detail: {...}` when the error has a detail. |

Only an object becomes structured content, since a model reads text best
for the rest, and a tool without an output schema gains nothing from
structured content that is not an object.

A call may wait as long as the method takes: a deferred reply answers
when its work ends, and the server serves other calls meanwhile. An MCP
cancel does not reach the method, since the wire format has no cancel.
The answer is then dropped.

When the socket goes away, the calls that wait fail with
`CONNECTION_CLOSED: ...`. Each call made without a connection first
connects again, so the tools work again once the application is back.
While it is not, such a call fails with `NOT_CONNECTED: ...`, or with
`CONNECTION_CLOSED: ...` when the connection fails after it started.
The tools are still the ones built at start.

## Limits

- **No events.** The server makes calls only. It does not subscribe to
  signals, and it drops the events that a target sends to a call.
- **No tool list that changes.** The tools are built once at start. A
  target that comes later has no tools until the client starts the server
  again.
- **No cancel.** See above.

## Use as a library

`McpIpcBridge` serves an application that runs its own MCP server, with
tools of its own beside the socket's:

```cpp
PhosphorMcp::McpServer server;
PhosphorMcpIpc::McpIpcBridge bridge(*PhosphorMcpIpc::McpIpcManifest::load(path, &error));
QObject::connect(&bridge, &PhosphorMcpIpc::McpIpcBridge::prepared, [&] {
    server.addTool(ownTool, ownHandler);
    bridge.addTools(server);
    server.start();
});
QObject::connect(&bridge, &PhosphorMcpIpc::McpIpcBridge::failed, [](const QString& error) { ... });
bridge.prepare(socketPath);
```

`warnings()` lists what `prepare()` left out. The bridge must outlive the
server whose tools it added. Its diagnostics go to the logging category
`phosphormcpipc`.

## Dependencies

- `QtCore`
- `phosphor-mcp` (public: `addTools` takes an `McpServer`)
- `phosphor-ipc` (private)
