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
  read with a `QSocketNotifier`. A line may be up to 16 MiB long, unless the
  application sets another limit. Writes never block the event loop. What
  stdout does not take now waits in a queue that drains when the reader
  takes more. Past a high-water mark of that queue the server stops reading
  requests until the client has read its replies, so a client that never
  reads cannot grow the server's memory.
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
- **The end of the session.** A signal at the end of stdin and when a read or
  a write fails, and on request at SIGTERM and SIGHUP.

The application owns its command line, its `QCoreApplication`, its tools and
what they do, its server name and instructions, and what happens when the
server ends.

Not in the library: an HTTP transport, the `initialize` handshake of older
revisions, resources, prompts, logging, sampling, roots, elicitation,
`subscriptions/listen` and tool lists that change. Add one when an
application needs it. Content blocks other than text, image and audio have
no typed constructor, and go through `McpContent::fromJson`.

## Key types

| Type | Purpose |
|------|---------|
| `PhosphorMcp::McpServer` | The server. `setServerInfo`, `setInstructions`, `setCache`, `setMaxLineBytes`, `setMaxPendingWriteBytes`, `addTool(tool, handler)`, `tools()`, `handleTerminationSignals()`, `start()`, `isRunning()`, and `transport()` for the transport's read-only state. Signal `finished(reason)`. Single-threaded. |
| `PhosphorMcp::McpTool` | A tool definition. Name, title, description, input and output schemas, the four behavior hints, and the client capabilities that a call needs. |
| `PhosphorMcp::McpCall` | One `tools/call`, handed to the handler. The arguments, the request id and `_meta`, `finish(result)`, `fail(text)`, `progress(value, total, message)`, `onCancelled(callback)`. Move-only. |
| `PhosphorMcp::McpResult` | What a tool answers. Text, image and audio content, any other block through `McpContent::fromJson`, structured content of any JSON type, and whether the call failed. |
| `PhosphorMcp::McpContent` | One content block. Text, an image or audio clip as bytes and a MIME type, or a block given as JSON. |
| `PhosphorMcp::McpTransport` | Lines on two file descriptors, stdin and stdout for the server. Tests give it a socket pair. |
| `McpProtocol.h` | The protocol version, the `_meta` keys and the error codes. |

## Wire protocol

MCP revision **2026-07-28** over stdio, and no other revision. One JSON-RPC
2.0 message goes on each line, in UTF-8, with no newline inside a message.
stdout carries protocol messages and nothing else, so the application must
not write to it either.

The library's diagnostics go to the logging category `phosphormcp`. Qt
writes them to stderr when the process has a controlling terminal, when
stderr itself is a terminal, or when the environment sets
`QT_FORCE_STDERR_LOGGING=1`. Otherwise, with a Qt built with journald
support, they go to the systemd journal. A server that an MCP client starts
often has no terminal (a desktop client or a service does not pass one on),
so a client that keeps the server's stderr as its log gets nothing unless
the variable is set. `examples/wordcount.cpp` sets it before
it creates its `QCoreApplication`, unless the environment already has it.

### Requests

Every request carries `params._meta` with
`io.modelcontextprotocol/protocolVersion` (a string) and
`io.modelcontextprotocol/clientCapabilities` (an object, `{}` is valid).
`io.modelcontextprotocol/clientInfo` and `progressToken` are optional. A
`progressToken` is a string or an integer of at most 64 bits, as an id is.
A string id or token must be valid UTF-16. One with an unpaired surrogate
would come back as U+FFFD and match nothing the client sent, so it is
refused.
A session with the example server, one request on each line:

```json
{"jsonrpc":"2.0","id":1,"method":"server/discover","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{}}}}
{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{}}}}
{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{},"progressToken":"p3"},"name":"count_words","arguments":{"text":"one two"}}}
{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{}},"name":"count_words","arguments":{"text":5,"lang":"en"}}}
```

A client cancels a running call with a notification, which gets no answer:

```json
{"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":3,"reason":"..."}}
```

### Results

Every result has `"resultType": "complete"` and the server's name and
version in `_meta`. `phosphor-mcp-example` answers the first three requests
of the session above with these lines, byte for byte. Qt writes the members
of each object in the order of their names.

```json
{"id":1,"jsonrpc":"2.0","result":{"_meta":{"io.modelcontextprotocol/serverInfo":{"name":"wordcount","version":"0.1.0"}},"cacheScope":"public","capabilities":{"tools":{}},"instructions":"count_words counts the words and characters of a text.","resultType":"complete","supportedVersions":["2026-07-28"],"ttlMs":3600000}}
{"id":2,"jsonrpc":"2.0","result":{"_meta":{"io.modelcontextprotocol/serverInfo":{"name":"wordcount","version":"0.1.0"}},"cacheScope":"public","resultType":"complete","tools":[{"annotations":{"destructiveHint":false,"idempotentHint":true,"openWorldHint":false,"readOnlyHint":true},"description":"Counts the words (runs of characters that are not spaces) and the characters (Unicode code points) of a text.","inputSchema":{"additionalProperties":false,"properties":{"text":{"description":"The text to count","maxLength":1000000,"type":"string"}},"required":["text"],"type":"object"},"name":"count_words","outputSchema":{"properties":{"characters":{"type":"integer"},"words":{"type":"integer"}},"required":["words","characters"],"type":"object"},"title":"Count words"}],"ttlMs":3600000}}
{"id":3,"jsonrpc":"2.0","result":{"_meta":{"io.modelcontextprotocol/serverInfo":{"name":"wordcount","version":"0.1.0"}},"content":[{"text":"{\"characters\":7,\"words\":2}","type":"text"}],"resultType":"complete","structuredContent":{"characters":7,"words":2}}}
```

While a call runs, and when its request had a `progressToken`, each
`progress()` sends a notification in this form:

```json
{"jsonrpc":"2.0","method":"notifications/progress","params":{"message":"...","progress":1,"progressToken":"p3","total":2}}
```

`instructions` is left out when the application gives none. `ttlMs` and
`cacheScope` are always there, since clients need both.
`capabilities.tools` is there only when the server has tools, and `start()`
warns when it has none. `tools/list` sends the whole list, in the order the
tools were added, and has no pages.

Without `setServerInfo`, the server takes the application's name, which
`QCoreApplication` sets to the executable's name unless the application
gives another, and the application's version, or `"0"` when it has none.

Replies are compact JSON. A lone UTF-16 surrogate in a string, which UTF-8
cannot hold, goes out as U+FFFD.

### Errors

| Code | When |
|------|------|
| -32700 Parse error | A line that is not JSON. Empty and blank lines are skipped, with no answer. |
| -32600 Invalid Request | A batch (a JSON array), valid JSON that is not an object, a line over the limit, no `jsonrpc: "2.0"`, no `method` in a message that is not a response, a method that is not a string, an id that is not a valid UTF-16 string or an integer of at most 64 bits, and a `tools/call` whose id another running call has. |
| -32601 Method not found | Any method but the three above, `ping` included. `initialize` too, with a message that names 2026-07-28 and `data: {"supported": ["2026-07-28"], "requested": ...}`, where `requested` is there when `params.protocolVersion` is a string. |
| -32602 Invalid params | `params` that is null or not an object, no `_meta`, a missing or wrong-typed required `_meta` key, a `progressToken` that is not a valid UTF-16 string or an integer of at most 64 bits, a `cursor` on `tools/list`, a `tools/call` with no `name`, an unknown tool, and `arguments` that are not an object. |
| -32603 Internal error | A call dropped without an answer. And, for a tool with an output schema, a result whose structured content does not match it (failed results included), holds a number that is not finite, or is missing from a result that did not fail. `data.errors` names each place. |
| -32021 | A tool's required client capability that the request did not declare. `data: {"requiredCapabilities": {...}}` holds every capability the tool requires, the declared ones too. The spec's prose says the missing ones, and its schema, `schema.ts`, says the ones the server requires. The schema is the source of truth, and the library follows it. |
| -32022 | A protocol version other than 2026-07-28, checked before the other `_meta` keys. `message: "Unsupported protocol version"`, `data: {"supported": ["2026-07-28"], "requested": "<version>"}`. |

An error carries the request's id. When the id cannot be read (a line that
is not JSON, a batch, a value that is not an object, a line over the limit,
an id of the wrong type), the answer has no id at all. MCP's schema has
`id?: string | number`, so the id is left out rather than null.

Arguments that do not match the tool's input schema are not a protocol error.
The model made the mistake and can mend it, so the answer is a result with
`isError: true` and a text that names the tool and each bad field by its
path. Request 4 of the session above gets this text:

```
Invalid arguments for tool count_words:
- arguments/text: must be of type string
- arguments/lang: is not a known property
```

The text lists at most ten errors, and then a line such as
`- arguments: and 3 more errors`. Arguments with more than a thousand
errors, or whose errors' paths come to more than 1 Mi UTF-16 code units
together, get one line instead, which says that they do not match and are
too large for the server to list each error. The check stops there.

Each line comes from the keyword that failed, its value in the schema and
the arguments, never from jsoncons' English: `must be of type integer`,
`is required`, `is not a known property`, `must be at most 5 characters
long`, `must be one of the values of enum`, `is required when "card" is
present`, `must have an item that matches contains`, and so on. A number in
a line is written as the schema has it, the same in every locale. A failed
`anyOf`, or a `oneOf` that matches no branch, lists the errors of each
branch and then its own line. A `oneOf` that matches more than one branch
says so alone, and a failed `allOf` lists the errors of its branches. A
`false` subschema has no reason of its own, so its line names the place and
says that it does not match the schema. jsoncons names neither the member
that the schema of `additionalProperties` refuses nor the name that
`propertyNames` refuses, so the server checks each one again on its own,
and lists its errors at its place (`the property name must be at most 256
characters long`). jsoncons reports only the first item that a `false`
`items` or `unevaluatedItems` refuses, and the first property that
`unevaluatedProperties: false` refuses. It counts an item or a property that
fails another keyword as unevaluated, so `unevaluatedItems` and
`unevaluatedProperties` add a line for it, where Python's jsonschema, for
one, does not. The arguments fail either way.

The check itself has a bound. Before it the server works out, from the
schema and the arguments, how much jsoncons would do, and refuses arguments
whose check could take more than about a second in a release build, or keep
more than 128 MB of errors, with the line
`- arguments: is too large for the server to check against the schema`.
jsoncons does more than the size of the arguments suggests. It copies its
path in the schema and the place in the value into each keyword it checks,
so the work grows with the depth, and it keeps a copy of that path for each
level of the walk, so the memory grows with the square of the depth. It
keeps every error inside an `allOf`, `anyOf`, `oneOf`, `not`, `contains`,
the condition of an `if` and the schema of `additionalProperties` until it
knows whether they pass, and copies those errors twice more for each
`allOf`, `anyOf` and `oneOf` around them. A `$ref` to a schema that holds it
is followed anew at each level of the value, so two of them double the work
at each level. The server reckons each of these, and refuses a schema
path longer than 1024 tokens (a value 512 levels deep under a schema that
recurses through `items` and `$ref`). Measured in a release build, as the
CPU time of the check of the largest arguments that the bound lets through:

| Schema and arguments | Largest checked | Time |
|---|---|---|
| `items: {"type": "integer"}`, valid integers | 2,480,000 integers | 0.58 s |
| `items` with four keywords, valid integers | 914,000 integers | 0.42 s |
| `items: {"type": "string"}`, every item wrong | 2,480,000 integers | 0.22 s |
| `anyOf` of an array of strings and null, every item wrong | 53,000 integers (the 128 MB of errors) | 0.05 s |
| the same in three `anyOf` | 19,900 integers | 0.05 s |
| `oneOf` of 20 arrays, every item wrong | 2,700 integers | 0.06 s |
| `allOf` of 50 `maxLength`, text that mixes ASCII and CJK | 4 MB | 0.62 s |
| `minLength` and `maxLength` | 16 MB | 0.12 s |
| `enum` of 3000 strings, a CJK string | 15 MB | 0.03 s |
| `anyOf` of four `enum`, an array of strings (each `enum` writes it out) | 566,000 strings | 0.20 s |
| `required` of 1000 names, objects without them | 19,800 objects | 0.01 s |
| `additionalProperties: {"type": "integer"}` | 167,000 members | 0.09 s |
| Afterglow's `env` (`additionalProperties` with `anyOf`, `propertyNames`), every member wrong | 22,300 members | 0.01 s |
| `propertyNames` with an `anyOf`, every name wrong | 28,000 names | 0.01 s |
| `contains` that no item matches | 87,000 integers | 0.06 s |
| `prefixItems`, `items` and `unevaluatedItems` | 1,660,000 integers | 0.40 s |
| `allOf`, `anyOf` and `unevaluatedProperties: false` | 2,000,000 members | 0.70 s |
| a `default` of 2000 members, objects without the property | 5,800 objects | 0.21 s |
| `items` through `$ref` to `{"not": {}}` | 890,000 integers | 0.10 s |
| a list of lists through `items` and `$ref` | 510 levels | 0.03 s |
| `allOf`, `$ref`, `anyOf` and `items` at each level | 40 levels | 0.01 s |
| two `$ref` to the schema at each level | 9 levels | 0.01 s |

So a valid list of 2.5 million small integers is checked, and one of three
million, about 1.3 s of jsoncons' work, is refused. The bound is reckoned
for the worst case, so arguments whose `anyOf` matches at its first branch
are refused as soon as those that fail every branch. Before any check the
server converts the arguments once into jsoncons' JSON, whatever the
schema, which takes 0.14 to 0.65 s of CPU time (the most for many small objects) for a 16 MiB line, about what the earlier
valijson tree took. That work grows with the longest line the server reads,
so an application whose tools take no long text lowers it with
`setMaxLineBytes`.

A string or a member name with an unpaired UTF-16 surrogate, and an
integral number outside the range of a 64-bit integer (1e19, 2^63, 1e300),
are refused whatever the schema says, since jsoncons, which reads UTF-8 and
64-bit integers, would read them differently from the handler. So is a value
nested more than 1024 levels deep, which only a handler can build. The range
is checked on the number as Qt's JSON parser reads it, and the parser rounds
a number outside it, and one written with a fraction or an exponent, to the
nearest double. So -9223372036854775809, down to -9223372036854776832,
passes as -9223372036854775808, the value that the handler reads, and
9223372036854775807.0 is refused as 2^63.

A notification (a message with a method and no id) is never answered, not
even when its params are wrong. The server ignores every notification but
`notifications/cancelled`, and logs the reason of a cancel at debug level. A
message without an id that is no notification, because it lacks
`jsonrpc: "2.0"` or a method, or its method is not a string, gets an Invalid
Request error with no id. A response (a `result` or an `error` without a
method) is dropped, since the server sends no requests. It never sends
`notifications/cancelled`, `notifications/message`, `ping` or any other
request.

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
    const bool added = server.addTool(tool, [&](PhosphorMcp::McpCall call) {
        // The arguments match the schema here.
        const int pane = call.arguments().value(QLatin1StringView("pane")).toInt();
        if (!exists(pane)) {
            call.fail(QStringLiteral("No pane %1. list_panes gives the panes.").arg(pane));
            return;
        }
        call.finish(PhosphorMcp::McpResult::text(textOf(pane)));
    });
    if (!added) {
        return 1; // addTool() gave the reason in a warning
    }

    server.handleTerminationSignals();
    using Reason = PhosphorMcp::McpServer::FinishReason;
    QObject::connect(&server, &PhosphorMcp::McpServer::finished, &app, [&app](Reason reason) {
        // A failed read or write is an error. The other reasons are ends.
        app.exit(reason == Reason::InputFailed || reason == Reason::OutputFailed ? 1 : 0);
    });
    // A server that does not start never finishes, so do not wait for it.
    if (!server.start()) {
        return 1;
    }
    return app.exec();
}
```

`start()` returns false, with a warning, when it runs before the
`QCoreApplication` exists, and when stdin or stdout is not a pipe, a socket,
a character device or a regular file. A closed stdin or stdout whose number
the `QCoreApplication`'s own eventfd took is one such case.

`examples/wordcount.cpp` is a whole server with one tool, which the test
`test_phosphor_mcp_example` runs as a client would.

### Answering later

A handler need not answer before it returns. It keeps the `McpCall` and
answers when the work ends, and the server reads and runs other requests
meanwhile. Replies go out in the order the calls finish.

```cpp
server.addTool(waitTool, [this](PhosphorMcp::McpCall call) {
    Wait& wait = m_waits.emplace_back(std::move(call)); // a std::list, whose elements stay put
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
  send nothing and return false. `finish` returns true when the reply was
  queued for the client, which does not say that the client read it.
- **Never left waiting.** A call destroyed unanswered (or overwritten by a
  move) answers with a -32603 error and a warning.
- **Cancellation.** `notifications/cancelled` for a running call runs its
  `onCancelled` callback once, and nothing more goes out for the request. A
  callback set on a call that is already cancelled runs at once. A second
  `onCancelled` replaces the first, which never runs, and an empty one
  changes nothing. A cancel for an id that is not running is ignored.
- **Progress.** `progress(value, total, message)` sends
  `notifications/progress` only when the request had a `progressToken`. Each
  value must be larger than the one before, and finite. Nothing goes out
  after the answer, and nothing while the client leaves more than the
  high-water mark of replies unread (see "Flow control"). A dropped
  notification is not sent later.
- **Failed calls.** `fail(text)`, or a result with `setError(true)`, is a
  result the model reads. Say what went wrong and what to do.
- **Structured content.** Any JSON value. A tool with an output schema
  answers each call that did not fail with structured content that matches
  it, and a failed result that has structured content must match it too.
  The library adds the compact JSON of the value as the last text block, as
  the spec asks, so do not add it yourself. A value that does not match is a
  bug of the tool. The client gets -32603, and `finish` returns false. A
  number that is not finite has no JSON form. With an output schema it is
  refused the same way, and without one it goes out as null, with a warning.
- **Large results.** An image or audio clip goes out as base64, 4/3 of its
  bytes. While the reply is built and written, the JSON objects and the
  serialized line each hold that text again, so the peak memory grows by
  about eight times the clip's size for a moment (133 MiB for 16 MiB,
  measured). The queued reply then holds 4/3 of it until the client reads
  it.
- **Move-only, one thread.** A moved-from call holds nothing. Use calls on
  the server's thread. Off it, a call does nothing, with a warning. A call
  may outlive the server, or go with it inside a handler or a cancel
  callback that owns it. Destroying the server sends nothing and runs no
  callback, and an unanswered call then reads `isCancelled()` true. Never
  destroy the server from inside a handler or a cancel callback. Call
  `deleteLater()` there instead.
- **No exceptions.** Handlers and cancel callbacks must not throw. The
  library is built without exceptions, and an exception that leaves a
  callback is undefined behavior.

### The end

`finished(reason)` comes once, from the event loop.

| Reason | When |
|--------|------|
| `EndOfInput` | stdin ended, so the client is done. A client on a socket that exits with replies it did not read resets the connection, and that counts as the end too. A last line without a newline is still read. The replies already queued go out first, until the client reads none of them for `DrainTimeoutMs` (2 s). Each read starts that wait again. |
| `InputFailed` | A read of stdin failed, with a warning. The replies already queued still go out, as after `EndOfInput`. |
| `OutputClosed` | stdout went away (EPIPE). |
| `OutputFailed` | A write to stdout failed for another reason (ENOSPC, EIO), with a warning. |
| `Terminated`, `HangUp` | SIGTERM or SIGHUP, after `handleTerminationSignals()`. The server ends at once, and the replies still queued are dropped. |

A signal, or an output that closes or fails, during that drain ends it at
once, with its own reason.

Before `finished`, every call still running is cancelled. Its callback runs,
so the application stops the work, and an answer that comes later is not
sent (`finish` returns false). That holds for the end of stdin too. A client
that closes stdin right after a request that the handler answers later gets
no answer to it. The server reads nothing more, and once `finished` comes it
writes nothing more either.

`handleTerminationSignals()` installs handlers for SIGTERM and SIGHUP that
write to a self-pipe. Signal dispositions are global to the process, so the
application asks for it, and one server at a time holds them. The server
puts back what it found as soon as it ends without waiting for its replies
(a signal, a closed or failed output), before the cancel callbacks run,
unless another handler was installed since. A signal during a slow callback
or teardown then has its usual effect: the second one after a signal, and
the first one after a closed or failed output. After the end of stdin or a
failed read the server keeps the signals while its replies drain, so that a
signal cuts the wait short, and puts them back at its finish (at the latest
its destruction). The server reads a signal from its event loop, though, so
one that comes while a cancel callback blocks without an event loop is read
only after the callbacks returned, and not at all when the server finished
by then. A signal that the process ignores (SIGHUP under `nohup`) stays
ignored. `handleTerminationSignals()` returns false, with a warning, before
the `QCoreApplication` exists and after the server ended. A child forked
before `exec` gets the disposition it would have had without the server, not
the server's handler. So a process that forks to keep the server running in
the child, as a daemon does, calls `handleTerminationSignals()` in the
child, after the fork, and not in the parent before it. After a call in the
parent, a call in the child returns false, with a warning, while the child
holds its copy of that server, since the handler stays the parent's. Once
the child destroyed its copy, a server in the child takes the signals.

The library never changes SIGPIPE's disposition. An ignored SIGPIPE survives
`exec`, so every program that the application starts would inherit it, and
a shell pipeline like `yes | head` there would no longer end as it should.
A closed stdout still never kills the process. A socket is written with
`MSG_NOSIGNAL`. Around a write to anything else, the transport blocks
SIGPIPE in its own thread and takes back the SIGPIPE that the write raised,
also when the write copied part of its data before the reader went away.
The write fails with EPIPE, and the server ends with `OutputClosed`.

### Flow control

Replies wait in a queue when stdout does not take them at once. The queue
has a high-water mark, `setMaxPendingWriteBytes` (64 MiB by default). Past
it, the server reads no more requests, the lines it has read wait too, and
progress notifications are dropped. It reads again when the queue is down
to half the mark. Replies are never dropped, and a single reply larger than
the mark is still written whole. A client that sends requests and never
reads the replies then fills its own pipe, not the server's memory.

`setMaxLineBytes` (16 MiB by default) and `setMaxPendingWriteBytes` take
effect at once, on a running server too. A value under 1 counts as 1.

A handler that runs a nested event loop (`QEventLoop::exec`) sees no new
request inside it. The server reads nothing while it hands out the requests
it has read, so a cancel of that very call and the end of stdin wait until
the handler returns, like every other request. Answer later through the
`McpCall` instead of waiting in a nested loop.

### stdout and terminals

Writes to stdout never block the event loop, and how the server does that
depends on what stdout is.

- A socket is written with `send(MSG_DONTWAIT | MSG_NOSIGNAL)`, and its
  flags never change.
- A terminal is opened again through `/proc/self/fd/1`, for a non-blocking
  file description of the server's own. The terminal that a shell shares
  with the server keeps its flags, even when the server is killed. When the
  terminal cannot be opened again (a pty that another user owns, after
  `su`, or no `/proc`), opens as another device or as one the server cannot
  identify, or is a pty master, the server warns and treats it as
  a pipe, as below. Its shared description is then non-blocking until the
  server ends.
- Anything else, such as the pipe an MCP client gives, gets `O_NONBLOCK`. At
  the end only that bit is cleared, and only when the server set it.

## Schemas

MCP schemas are JSON Schema 2020-12 unless they name another dialect, and
clients read them that way. The library checks arguments and structured
content with jsoncons, in 2020-12. A client must read 2020-12 and need not
read another dialect, so `addTool` refuses a `$schema` other than
`https://json-schema.org/draft/2020-12/schema` (with or without an empty
fragment). It refuses as well the keywords that the server cannot check as
2020-12 says, each with the reason and the JSON pointer of the place
(`error` argument, and a warning).

| | Keywords |
|---|---|
| Checked | `type`, `enum`, `const`, `minimum`, `maximum`, `exclusiveMinimum`, `exclusiveMaximum`, `multipleOf`, `minLength`, `maxLength`, `minItems`, `maxItems`, `minProperties`, `maxProperties`, `required`, `dependentRequired`, `properties`, `additionalProperties`, `propertyNames`, `prefixItems`, `items` (one schema), `contains`, `minContains`, `maxContains`, `allOf`, `anyOf`, `oneOf`, `not`, `if`, `then`, `else`, `unevaluatedProperties` (`true` or `false`), `unevaluatedItems`, `$ref` (within the schema), `$defs`, `$anchor`, `$schema` (2020-12, at the root) |
| Annotations, never checked | `title`, `description`, `default`, `examples`, `deprecated`, `readOnly`, `writeOnly`, `$comment`, `contentMediaType`, `contentEncoding`, `format`. 2020-12 makes `format` an annotation unless a schema asks for the format vocabulary, and a client reads it so: the server lets `"format": "email"` pass any string. |
| Refused: regular expressions | `pattern`, `patternProperties`. jsoncons matches them with `std::regex`, whose libstdc++ executor recurses once for each character. A string of a few hundred thousand characters overflows the stack and kills the process (GCC bug 86164), and nested quantifiers take forever. It also matches UTF-8 bytes, where a client matches UTF-16 code units (`^.{1,5}$` and "naïve" differ). Use `enum`, or check the value in the handler. |
| Refused: cost | `uniqueItems`. jsoncons compares every pair of items, so 20000 items take seconds. Check for repeated items in the handler. |
| Refused: jsoncons gets them wrong | `dependentSchemas`, whose subschemas jsoncons files under one name and whose errors it reports at a member that does not exist (use `if`, `required` and `then`). `unevaluatedProperties` with a schema, which jsoncons checks at the place of the object instead of each property. `unevaluatedProperties` and `unevaluatedItems` beside a `not` that applies to the same value, since jsoncons counts what the subschema of `not` evaluated, where 2020-12 drops it. `unevaluatedItems` beside a `contains`, since jsoncons counts the items of a nested array that `contains` matched as items of the outer one. |
| Refused: references | A `$ref` that leaves the schema (another document, the meta-schema): the server fetches nothing. A `$ref` that leads back to its own schema without descending into the value, through `allOf`, `anyOf`, `oneOf`, `not`, `if`, `then`, `else` or another `$ref`, which jsoncons would follow until the stack overflows. A `$ref` to a boolean schema, or into an `items` beside `prefixItems`, which jsoncons files under another name. `$id`, `$dynamicRef`, `$dynamicAnchor` and `$vocabulary`. |
| Refused: other drafts | `definitions` (use `$defs`), `dependencies` (use `dependentRequired`), `additionalItems` (use `items` beside `prefixItems`), `$recursiveRef`, `$recursiveAnchor`, and `items` as an array (use `prefixItems`) |
| Refused: other | every other keyword |

A `$ref` is `#` (the whole schema), `#` and a JSON pointer, its characters
percent-encoded where a URI cannot hold them (`#/$defs/a%20b`), or `#` and
an `$anchor`. jsoncons takes an `$anchor` that starts with a letter only,
where 2020-12 also allows "_". A `$ref` may recurse through a keyword that
descends into the value (`properties`, `items`, `additionalProperties`, and
so on), and the work bound above keeps such a schema from doubling its work
without end.

Integers compare exactly, with 64 bits: the server holds every integral
number of the schema and the arguments that fits a 64-bit integer as one. So
`"maximum": 9007199254740993` refuses 9007199254740994. jsoncons compares an
integer with another number as doubles, which round the largest 64-bit
integers to 2^63, so 2^63 itself is refused in a bound, `enum` or `const`.
jsoncons checks `multipleOf` with doubles and lets a value pass within one
unit in its last place of a multiple. So 2.5 fails `multipleOf: 1`, 0.3
passes `multipleOf: 0.1` (which a client that divides doubles may refuse),
and an integer of 2^53 or more in magnitude under `multipleOf` is refused,
since doubles do not tell 10^17 from a multiple of 3.

Only the root of an input schema must have `"type": "object"`. The root of
an output schema may be any type, since structured content may be any JSON
value. Keyword values are checked too, everywhere below the root. Lengths
and counts are integers from 0 to 9007199254740991, and `multipleOf` is
greater than 0. `required` and each list of `dependentRequired` may not name
a property twice. Names, and the strings in `enum` and `const`, may not
hold an unpaired UTF-16 surrogate, which UTF-8 cannot hold. A Draft 4
boolean `exclusiveMinimum` and an unknown type name are refused as well.
Names inside `properties`, `$defs` and `dependentRequired` and values inside
`enum`, `const`, `default` and `examples` are data, not keywords. A tool
without an input schema takes no arguments, as with
`{"type": "object", "additionalProperties": false}`.

## Dependencies

- `QtCore`. No QtGui, QtQml or QtNetwork, so a server inside a command-line
  program pulls in nothing more.
- jsoncons 1.7.0 or later (header-only, BSL-1.0). By default it comes from
  the archive that `cmake/PhosphorDependencies.cmake` pins (1.10.0), and its
  notice then installs as `licenses/phosphor-libs/LICENSE.jsoncons`. With
  `-DPHOSPHORMCP_USE_SYSTEM_JSONCONS=ON` the system's headers are used, found
  through `JSONCONS_INCLUDE_DIR`, the cache variable that phosphor-fsloader
  uses too, and the configure checks the version floor by compiling against
  them (1.7.0 is the first with `jsonschema::walk_state`). The option's
  default is `AUTO`, which follows `PHOSPHORFSLOADER_USE_SYSTEM_JSONCONS` at
  every configure, so phosphor-fsloader's switch moves both libraries to the
  system's copy. `ON` and `OFF` choose for this library alone. The keyword
  policy and the lines of the errors were checked against 1.10.0, and an
  older release may report its errors otherwise, which then read as "does
  not match the schema". jsoncons is private to the library, and a consumer
  needs nothing of it.
- Linux, for `pipe2`, `sigtimedwait`, `MSG_NOSIGNAL` and `/proc/self/fd`.

## See also

- [`phosphor-fsloader`](../phosphor-fsloader/README.md). It checks JSON files
  against schemas with the same pinned jsoncons, and this library follows
  its system switch. The two keep separate validators on purpose. They share
  only the few calls that drive jsoncons, while this one adds what arguments
  from a model need: the keywords that jsoncons checks as 2020-12 says, a
  check of the value before jsoncons sees it, a bound on the work of the
  check, and a short list of errors in plain words.
- [`phosphor-ipc`](../phosphor-ipc/README.md). JSON over a Unix socket, for
  scripts and shells that drive a running Phosphor application. This library
  serves an agent that starts the application as a child instead.
- [`phosphor-channel`](../phosphor-channel/README.md). Its `SigpipeBlock` is
  the original of the transport's copy, and the two are kept in step.
- `examples/wordcount.cpp` and `tests/test_phosphor_mcp_example.cpp`. A
  whole server, and a client that runs it.
