<!-- SPDX-FileCopyrightText: 2026 fuddlesworth
     SPDX-License-Identifier: LGPL-2.1-or-later -->

# phosphor-mcp

> A Model Context Protocol server over stdio, for Phosphor applications that
> offer tools to an AI agent. The application declares its tools and answers
> their calls. The library speaks the protocol.

## Responsibility

MCP has nothing that belongs to one application, and each application that
offers tools would otherwise write the same JSON-RPC, version checks and
schema checks. `phosphor-mcp` holds them once, so that a new protocol
revision changes one library.

The library owns:

- **The stdio transport.** One JSON-RPC message per line on stdin and stdout,
  read with a `QSocketNotifier`, with a line limit that the application
  gives. Writes never block the event loop: what stdout does not take now
  waits in a queue that drains when the reader takes more. Past a
  high-water mark of that queue the server stops reading requests until the
  client has read its replies, so a client that never reads cannot grow the
  server's memory.
- **JSON-RPC 2.0.** Requests, notifications, ids, and the error codes.
- **The checks of every request.** The `_meta` keys that 2026-07-28 makes
  required, the protocol version, and the client capabilities that a tool
  needs. Nothing is kept from one request for the next.
- **`server/discover`, `tools/list` and `tools/call`.** The results with
  `resultType` and the server's info in `_meta`, a fixed tool list, the
  dispatch of each call to its handler, and the check of the arguments
  against the tool's input schema before the handler runs.
- **Calls that run at the same time.** Each call is a handle that the
  handler answers now or later, with cancellation and progress.
- **The end of the session.** A signal at the end of stdin, and on request at
  SIGTERM and SIGHUP.

The application owns its command line, its `QCoreApplication`, its tools and
what they do, its server name and instructions, and what happens when the
server ends.

Not in the library: an HTTP transport, the `initialize` handshake of older
revisions, resources, prompts, logging, sampling, roots, elicitation,
`subscriptions/listen` and tool lists that change. Add one when an
application needs it.

## Key types

| Type | Purpose |
|------|---------|
| `PhosphorMcp::McpServer` | The server. `setServerInfo`, `setInstructions`, `setCache`, `setMaxLineBytes`, `setMaxPendingWriteBytes`, `addTool(tool, handler)`, `handleTerminationSignals()`, `start()`. Signal `finished(reason)`. Single-threaded. |
| `PhosphorMcp::McpTool` | A tool definition: name, title, description, input and output schemas, the four behavior hints, and the client capabilities that a call needs. |
| `PhosphorMcp::McpCall` | One `tools/call`, handed to the handler. The arguments, the request id and `_meta`, `finish(result)`, `fail(text)`, `progress(value, total, message)`, `onCancelled(callback)`. Move-only. |
| `PhosphorMcp::McpResult` | What a tool answers: text and image content, structured content, and whether the call failed. |
| `PhosphorMcp::McpContent` | One content block: text, or an image as bytes and a MIME type. |
| `PhosphorMcp::McpTransport` | Lines on two file descriptors, stdin and stdout for the server. Tests give it a socket pair. |
| `McpProtocol.h` | The protocol version, the `_meta` keys and the error codes. |

## Wire protocol

MCP revision **2026-07-28** over stdio, and no other revision: one JSON-RPC
2.0 message per line, UTF-8, with no newline inside a message. stdout carries
protocol messages and nothing else. The library's diagnostics go to the
logging category `phosphormcp`, which Qt writes to stderr. The application
must not write to stdout either.

### Requests

Every request carries `params._meta` with
`io.modelcontextprotocol/protocolVersion` (a string) and
`io.modelcontextprotocol/clientCapabilities` (an object, `{}` is valid).
`io.modelcontextprotocol/clientInfo` and `progressToken` (a string or an
integer) are optional.

```jsonc
{"jsonrpc":"2.0","id":1,"method":"server/discover","params":{"_meta":{
  "io.modelcontextprotocol/protocolVersion":"2026-07-28",
  "io.modelcontextprotocol/clientCapabilities":{}}}}
{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{"_meta":{...}}}
{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"_meta":{...,"progressToken":"p3"},
  "name":"count_words","arguments":{"text":"one two"}}}
// No answer: a notification.
{"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":3,"reason":"..."}}
```

### Results

Every result has `"resultType": "complete"` and the server's name and
version in `_meta`:

```jsonc
{"jsonrpc":"2.0","id":1,"result":{"resultType":"complete",
  "supportedVersions":["2026-07-28"],"capabilities":{"tools":{}},
  "instructions":"...","ttlMs":60000,"cacheScope":"public",
  "_meta":{"io.modelcontextprotocol/serverInfo":{"name":"example","version":"1.0"}}}}
{"jsonrpc":"2.0","id":2,"result":{"resultType":"complete","tools":[...],
  "ttlMs":60000,"cacheScope":"public","_meta":{...}}}
{"jsonrpc":"2.0","id":3,"result":{"resultType":"complete",
  "content":[{"type":"text","text":"{\"words\":2,\"characters\":7}"}],
  "structuredContent":{"words":2,"characters":7},"_meta":{...}}}
// While call 3 runs, when its request had a progressToken:
{"jsonrpc":"2.0","method":"notifications/progress","params":{"progressToken":"p3","progress":1,"total":2,"message":"..."}}
```

`instructions` is left out when the application gives none. `ttlMs` and
`cacheScope` are always there: clients need both. `tools/list` sends the
whole list, in the order the tools were added, and has no pages.

### Errors

| Code | When |
|------|------|
| -32700 Parse error | A line that is not JSON. The id is `null`. |
| -32600 Invalid Request | A batch (a JSON array), a line over the limit (both with id `null`), no `jsonrpc: "2.0"`, a method that is not a string, an id that is not a string or an integer (id `null`), and a `tools/call` whose id another running call has. |
| -32601 Method not found | Any method but the three above, `ping` included. `initialize` too, with a message that names 2026-07-28 and `data: {"supported": ["2026-07-28"], "requested": ...}`. |
| -32602 Invalid params | No `params`, no `_meta`, a missing or wrong-typed required `_meta` key, a `progressToken` that is not a string or an integer, a `cursor` on `tools/list`, a `tools/call` with no `name`, an unknown tool, and `arguments` that are not an object. |
| -32603 Internal error | A call dropped without an answer, and a result whose structured content does not match the tool's output schema. |
| -32021 | A tool's required client capability that the request did not declare. `data: {"requiredCapabilities": {...}}`. |
| -32022 | A protocol version other than 2026-07-28. `message: "Unsupported protocol version"`, `data: {"supported": ["2026-07-28"], "requested": "<version>"}`. |

Arguments that do not match the tool's input schema are not a protocol error.
The model made the mistake and can mend it, so the answer is a result with
`isError: true` and a text that names the tool and each bad field by its path:

```
Invalid arguments for tool pane_tool:
- arguments/options/depth: Expected number less than or equal to 3.000000
- arguments: Missing required property 'pane'.
```

Notifications are never answered. The server ignores every notification but
`notifications/cancelled`, and ignores responses, since it sends no requests.
It never sends `notifications/cancelled`, `notifications/message`, `ping` or
any other request.

## Typical use

```cpp
#include <PhosphorMcp/McpServer.h>

#include <QCoreApplication>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    PhosphorMcp::McpServer server;
    server.setServerInfo(QStringLiteral("example"), QStringLiteral("1.0"));
    server.setInstructions(QStringLiteral("What the tools are for."));
    // The list never changes and is the same for every client.
    server.setCache(60 * 60 * 1000, PhosphorMcp::McpServer::CacheScope::Public);
    // The largest request the tools take, with its JSON-RPC envelope.
    server.setMaxLineBytes(4 * 1024 * 1024);

    PhosphorMcp::McpTool tool(QStringLiteral("read_pane"));
    tool.setDescription(QStringLiteral("Reads the text of a pane."))
        .setInputSchema(schema) // {"type": "object", "properties": {...}, ...}
        .setHints({.readOnly = true, .destructive = false, .idempotent = true, .openWorld = false});
    server.addTool(tool, [&](PhosphorMcp::McpCall call) {
        // The arguments match the schema here.
        const int pane = call.arguments().value(QLatin1StringView("pane")).toInt();
        if (!exists(pane)) {
            call.fail(QStringLiteral("No pane %1. list_panes gives the panes.").arg(pane));
            return;
        }
        call.finish(PhosphorMcp::McpResult::text(textOf(pane)));
    });

    server.handleTerminationSignals();
    QObject::connect(&server, &PhosphorMcp::McpServer::finished, &app, &QCoreApplication::quit);
    if (!server.start()) {
        return 1;
    }
    return app.exec();
}
```

`examples/wordcount.cpp` is a whole server with one tool, which the test
`test_phosphor_mcp_example` runs as a client would.

### Answering later

A handler need not answer before it returns. It keeps the `McpCall` and
answers when the work ends, and the server reads and runs other requests
meanwhile. Replies go out in the order the calls finish.

```cpp
server.addTool(waitTool, [this](PhosphorMcp::McpCall call) {
    Wait& wait = m_waits.emplace_back(std::move(call)); // std::list: stays put
    wait.call.onCancelled([this, &wait] {
        stop(wait);
        m_waits.remove_if(...); // the callback may destroy the call
    });
    start(wait); // later: wait.call.progress(n, {}, line); wait.call.finish(result);
});
```

The rules:

- **One answer.** The first `finish` or `fail` stands. After it, after a
  cancellation and after the server ended, `finish`, `fail` and `progress`
  send nothing and return false.
- **Never left waiting.** A call destroyed unanswered (or overwritten by a
  move) answers with a -32603 error and a warning.
- **Cancellation.** `notifications/cancelled` for a running call runs its
  `onCancelled` callback once, and nothing more goes out for the request. A
  callback set on a call that is already cancelled runs at once. A cancel
  for an id that is not running is ignored.
- **Progress.** `progress(value, total, message)` sends
  `notifications/progress` only when the request had a `progressToken`. Each
  value must be larger than the one before, and finite. Nothing goes out
  after the answer.
- **Failed calls.** `fail(text)`, or a result with `setError(true)`, is a
  result the model reads: say what went wrong and what to do.
- **Structured content.** A tool with an output schema answers each call
  that did not fail with structured content that matches it. The library
  adds the compact JSON of the value as the last text block, as the spec
  asks, so do not add it yourself. A value that does not match is a bug of
  the tool: the client gets -32603, and `finish` returns false.
- **Move-only, one thread.** Use calls on the server's thread. A call that
  outlives the server does nothing. Destroy the calls before the server, or
  with it, not after it from an object that the server owns.

### The end

`finished(reason)` comes once, from the event loop:

| Reason | When |
|--------|------|
| `EndOfInput` | stdin ended: the client is done. A last line without a newline is still read. The replies already queued go out first, for up to `DrainTimeoutMs` (2 s). |
| `InputFailed` | A read of stdin failed. |
| `OutputClosed` | stdout went away (EPIPE) or a write failed. |
| `Terminated`, `HangUp` | SIGTERM or SIGHUP, after `handleTerminationSignals()`. |

Before it, every call still running is cancelled: its callback runs, so the
application stops the work. The server then reads and writes nothing more.

`handleTerminationSignals()` installs handlers for SIGTERM and SIGHUP that
write to a self-pipe. Signal dispositions are global to the process, so the
application asks for it, one server at a time holds them, and the server's
destruction puts back what it found.

The library never changes SIGPIPE's disposition. An ignored SIGPIPE survives
`exec`, so every program that the application starts would inherit it, and
a shell pipeline like `yes | head` there would no longer end as it should.
A closed stdout still never kills the process: the transport blocks SIGPIPE
in its own thread around each write, and takes back the one that the write
raised. The write fails with EPIPE, and the server ends with `OutputClosed`.

### Flow control

Replies wait in a queue when stdout does not take them at once. The queue
has a high-water mark, `setMaxPendingWriteBytes` (64 MiB by default). Past
it, the server reads no more requests, and the lines it has read wait too.
It reads again when the queue is down to half the mark. Replies are never
dropped, and a single reply larger than the mark is still written whole. A
client that sends requests and never reads the replies then fills its own
pipe, not the server's memory.

A handler that runs a nested event loop (`QEventLoop::exec`) sees no new
request inside it: the server reads nothing while it hands out the requests
it has read. Answer later through the `McpCall` instead of waiting in a
nested loop.

### stdout and terminals

`start()` sets `O_NONBLOCK` on stdout, so that a write never blocks the
event loop, and puts the old flags back when the server ends. The flag
belongs to the open file description, not to the descriptor: when stdin,
stdout and stderr are one terminal, as in a shell, the terminal's other
users see it too until then. An MCP client gives the server pipes, so this
matters only when you run a server by hand in a terminal to try it.

## Schemas

MCP schemas are JSON Schema 2020-12 unless they name another dialect, and
clients read them that way. The library checks arguments with valijson,
which implements Draft 7. So `addTool` takes only schemas that mean the same
in both dialects, without a `$schema`, and refuses the rest with the reason
and the JSON pointer of the place (`error` argument, and a warning):

| | Keywords |
|---|---|
| Allowed | `type`, `properties`, `required`, `enum`, `const`, `minimum`, `maximum`, `exclusiveMinimum`, `exclusiveMaximum` (numbers), `minLength`, `maxLength`, `minItems`, `maxItems`, `uniqueItems`, `items` (one schema), `minProperties`, `maxProperties`, `additionalProperties` (a boolean or a schema), `propertyNames`, `oneOf`, `anyOf`, `allOf`, `not`, and the annotations `description`, `title`, `default`, `examples`, `readOnly`, `writeOnly`, `deprecated`, `$comment` |
| Refused: the drafts differ | `$schema`, `$ref`, `$defs`, `definitions`, `prefixItems`, `items` as an array, `additionalItems`, `dependentRequired`, `dependentSchemas`, `dependencies`, `unevaluatedProperties`, `unevaluatedItems`, `$dynamicRef`, `$dynamicAnchor`, `$anchor`, `$id`, `$recursiveRef`, `$recursiveAnchor`, `$vocabulary` |
| Refused: regular expressions | `pattern`, `patternProperties`. valijson matches them with `std::regex`, whose libstdc++ executor recurses once for each character: a string of a few hundred thousand characters overflows the stack and kills the process (GCC bug 86164), and nested quantifiers take forever. It also matches UTF-8 bytes, where a client matches UTF-16 code units (`^.{1,5}$` and "naïve" differ). Use `enum`, or check the value in the handler. |
| Refused: other | `format` (valijson asserts it, a 2020-12 reader need not), `multipleOf` (valijson lets 2.5 pass `multipleOf: 1`), and every keyword not in the first row (`contains`, `if`/`then`/`else`, ...) |

The root of an input or output schema has `"type": "object"`. Keyword values
are checked too (a Draft 4 boolean `exclusiveMinimum`, an unknown type
name, a negative `maxLength`), and so is everything below the
root. Names inside `properties` and values inside `enum`, `const`, `default`
and `examples` are data, not keywords. A tool without an input schema takes
no arguments: `{"type": "object", "additionalProperties": false}`.

## Dependencies

- `QtCore`. No QtGui, QtQml or QtNetwork: a server inside a command-line
  program pulls in nothing more.
- valijson (header-only, BSD-2-Clause), from the archive that
  `cmake/PhosphorDependencies.cmake` pins, or the system's headers with
  `-DPHOSPHORMCP_USE_SYSTEM_VALIJSON=ON`. It is private to the library: a
  consumer needs nothing of it. Its notice installs as
  `licenses/phosphor-libs/LICENSE.valijson`.
- POSIX: `pipe2`, `sigaction`, `pthread_sigmask`, `sigtimedwait`.
