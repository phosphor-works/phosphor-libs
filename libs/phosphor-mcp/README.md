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
- arguments/lang: is not a known property
- arguments/text: must be of type string
```

The text lists at most ten errors, and then a line such as
`- arguments: and 3 more errors`. Arguments with more than a thousand
distinct errors, or whose errors' paths come to more than 1 Mi UTF-16 code
units together, get one line instead, which says that they do not match and
are too large for the server to list each error. The check stops there.
The places refused before the check (below) are errors like the others
here, ten listed and a thousand counted. Past a thousand, their one line
says only that the arguments are refused before the check, with too many
errors for the server to list each one, since the check that would say
whether they match never ran.

Each line comes from the keyword that failed, its value in the schema and
the arguments: `must be of type integer`, `is required`, `is not a known
property`, `must be at most 5 characters long`, `must be one of the values
of enum`, `is required when "card" is present`, `must have an item that
matches contains`, `must have unique items, and items 1 and 3 are equal`,
and so on. A number in a line is written the same in every locale: an
integer with all its digits (9223372036854775808 too, below 10^21), any
other number in its shortest form that reads back as the same double
(`0.1`, `1e-07`, `1e+300`).

The errors of a subschema come in the order of its keywords. First those
that apply no subschema (`type`, `const`, `enum`, and the keywords of
numbers and strings), then those of an array (its counts, `uniqueItems`,
`prefixItems`, `items`, `contains`) or of an object. The members of an
object come in the order in which Qt keeps them, that of their names,
each with the errors of its name and of its value. Then come the
properties that `required` and `dependentRequired` miss, and the errors of
`dependentSchemas`. After these come the subschemas applied to the same
value (`$ref`, `allOf`, `anyOf`, `oneOf`, `not`, `if`, `then` and `else`),
and last the members and items that `unevaluatedItems` and
`unevaluatedProperties` refuse. A failed `anyOf`, or a `oneOf` that
matches no branch, lists the errors of each branch and then its own line.
A `oneOf` that matches more than one branch says so alone, and a failed
`allOf` lists the errors of its branches. A `false` subschema has no
reason of its own, so its line names the place and says that it does not
match the schema. What `not`, the condition of an `if` and the items that
`contains` tries find is never listed, since it is no error of the
arguments.

Each member that `additionalProperties: false` or `unevaluatedProperties:
false` refuses has its line, and so has each item that `items: false` or
`unevaluatedItems: false` refuses. The errors that the schema of
`additionalProperties`, `unevaluatedProperties` or `unevaluatedItems`
finds are listed at the member or item, and those of `propertyNames` at
the member, as `the property name must be at most 256 characters long`. A
member or item that a keyword of the schema evaluated is not called
unknown, even when its value failed there, nor is one that a failed
branch of a failed `anyOf` or `oneOf` declares. The arguments fail either
way, and the line that matters is the one about its value.

The check itself has a bound, and it is exact. The checker counts what it
does as it goes, and stops the moment the count passes the bound, with the
line `- arguments: is too large for the server to check against the
schema`. Nothing is estimated beforehand. Each step is charged by what it
costs (a value walked, a subschema applied to a value, a keyword checked,
a member visited, a name looked up, a name or a string read away from the
last one read of its object or array, sixteen code units of a string or a
name read, compared or hashed, a word of an annotation set merged, an
error built), in units of at most about 20 ns of a release build. A sweep
that drives each charge of the checker with the shapes that make it
dominate, in each encoding that Qt keeps text in, with short and long
strings and names, and with names in random order, measured at most 17 ns
a unit for a check of a millisecond or more, and the shapes below measure
as much. A check of less than a millisecond pays fixed costs too, such as
the first use of the stack, and nothing that grows with the arguments. The
bound is 40 million units, under a second whatever the schema and the
arguments.

Strings and names are read where Qt's parser keeps them, and never decoded
to be compared, hashed or counted, not even the names that `propertyNames`
checks as strings. The parser keeps ASCII text in Latin-1, other text in
UTF-8, and text with an escape (`\n`, `\u00e9`) in UTF-16. A code unit of
UTF-8 is a byte, so a string of CJK characters in UTF-8 is charged three
times what the same string in UTF-16 is. Text checked for equality with
text in another encoding, where one of the two is UTF-8, is charged four
times as much, since Qt decodes the UTF-8 as it compares. The length of a
string is counted once for all the subschemas applied to it in place. Qt
keeps the members of an object sorted by name, but their names and strings
in the order that the arguments wrote them, so the names of a large object
written in random order are read from all over its storage, each a miss of
the cache. Each name or string read is charged a unit for each 64 bytes
between it and the last one read of its object or array, up to 5, which is
about a miss. A handler's array whose strings were inserted out of order
is charged the same way. A name is decoded, and its `~` and `/` escaped,
only for the JSON pointer of an error, once for all the errors at its
place and below it while the check stays there, and again when it comes
back. The memory the check holds beyond the arguments and the schema is
counted too, up to 64 MiB. It holds the sets of members and items that
`unevaluatedProperties` and `unevaluatedItems` read, the keys that
`uniqueItems` sorts, and the index of an object's names that `required`,
`dependentRequired` and `dependentSchemas` look names up in. The list of
errors is capped as above.

A check is also stopped 3000 subschemas deep, and when its own frames
take 4 MiB of stack, half of what a Linux thread has by default. Each
subschema applied takes about a kilobyte of stack in a release build, so
there the depth stops a check first. A list nested 1000 levels deep
through `items` and `$ref` is checked in a release build, and one whose
levels each pass through three more `$ref` is refused at 600 levels.

Measured in a release build (CPU time of `validate()`, the walk before
the check included), on arguments parsed from a request line as the
server parses them, for the largest arguments in a line of 16 MiB, or as
many as the bound lets through. `test_phosphor_mcp_checkwork` counts
exactly the units that each step is charged, and
`test_phosphor_mcp_bounds` checks many of them through the server, at
sizes that a debug build with sanitizers checks in seconds.

| Schema and arguments | Arguments | Result | Time |
|---|---|---|---|
| `items: {"type": "integer"}` | 6,600,000 integers | valid | 0.44 s |
| the same | 6,700,000 integers | too large to check | 0.45 s |
| the same | 8,000,000 integers (16 MiB) | too large to check | 0.41 s |
| `items` with `type`, `minimum`, `maximum`, `multipleOf` | 4,000,000 integers | valid | 0.36 s |
| `items: {"type": "string"}` | 8,000,000 integers, every one wrong | too large to list | 0.15 s |
| two `properties` | 1,000,000 objects | valid | 0.36 s |
| two `$ref` to the schema at each level (2^depth subschemas) | 40 levels | too large to check | 0.38 s |
| `anyOf` in `anyOf` 50 deep, the last branch matching | 100,000 integers | valid | 0.30 s |
| `allOf` of 50 `maxLength` | 4,000,000 CJK characters | valid | 0.003 s |
| `allOf` of 300 `maxLength`, below the string's bytes | 5,500,000 CJK characters (16 MiB) | valid | 0.004 s |
| `enum` of 3000 strings | 5,000,000 CJK characters | one line | 0.003 s |
| `enum` of 3000 strings of 100 CJK characters | 50,000 such strings | valid | 0.015 s |
| `const` of an object of 100 members | 20,000 such objects | valid | 0.18 s |
| `required` of 1000 names inside `not` | 200,000 empty objects | valid | 0.02 s |
| `allOf` of 300 `required` | an object of 1,000,000 members | valid | 0.12 s |
| `dependentRequired` of 2000 triggers | 1,000,000 empty objects | valid | 0.08 s |
| `additionalProperties: {"type": "integer"}` | 1,000,000 members | valid | 0.09 s |
| the same | 1,000,000 members written in random order | valid | 0.28 s |
| `allOf` of 50 `additionalProperties` | 300,000 members named with CJK characters | too large to check | 0.36 s |
| the same | 300,000 such members written in random order | too large to check | 0.49 s |
| `propertyNames: {"maxLength": 20}` | 1,000,000 members | valid | 0.08 s |
| the same | 1,000,000 members written in random order | valid | 0.32 s |
| `propertyNames: {"maxLength": 50}` | 134,000 members of 40 CJK characters (16 MiB) | valid | 0.02 s |
| Afterglow's `env` without its `maxProperties` (`additionalProperties` with `anyOf`, `propertyNames`) | 300,000 members | valid | 0.05 s |
| `contains` that no item matches | 4,000,000 integers | one line | 0.24 s |
| `prefixItems`, `items` and `unevaluatedItems` | 4,000,000 integers | valid | 0.27 s |
| `allOf` of 200 `properties` and `unevaluatedProperties` | 35,000 members | valid | 0.18 s |
| `uniqueItems` | 1,000,000 distinct integers (32 MB of keys) | valid | 0.13 s |
| `uniqueItems` | 900,000 equal objects | one line | 0.36 s |
| `uniqueItems` | 300,000 distinct CJK strings | valid | 0.06 s |
| `uniqueItems`, items chosen for one hash under `QT_HASH_SEED=0` | 250,000 pairs | valid | 0.39 s |
| the same | 300,000 pairs | too large to check | 0.45 s |
| `uniqueItems` | 2,100,000 items (past 64 MiB of keys) | too large to check | 0.04 s |
| a list of lists through `items` and `$ref` | 1000 levels | valid | 0.001 s |
| `{"type": "object"}` | 5,500,000 empty arrays (16 MiB) | valid | 0.14 s |

So a valid list of 6.6 million small integers under `items: {"type":
"integer"}` is checked, and one of 8 million, the most a 16 MiB line
holds, is refused after 0.4 s. Each row that ends at the bound took 0.36
to 0.49 s, since the units it spent were cheaper than the dearest. A
million members written in random order take three times as long as the
same members in order, each a miss of the cache, and are charged so. Items
chosen for one hash need the seed, which a model does not know. With the
random seed of a running server their hashes differ, and 550,000 such
pairs are valid in 0.13 s. The walk before the check charges 3 units for
each value, and 5 for an array or an object, so the most values that a 16
MiB line holds are walked in some 28 million units. Qt's parser takes time
and memory for such a line too (some 130 MB for 5.5 million empty arrays),
which the line limit bounds. An application whose tools take no long text
lowers it with `setMaxLineBytes`.

Before the check, the server refuses what a handler would read
differently from the check, or what a reply cannot carry, with a line for
each place: a string or a member name with an unpaired UTF-16 surrogate
(the reply writes it as U+FFFD), a number that is not finite (only a
handler can build one, in structured content), and a value nested more
than 1024 levels deep (only a handler can build one too). A number
without a fraction outside the range of a 64-bit integer (1e19, 2^63,
1e300) is an integer in JSON Schema, but `QJsonValue::toInteger()` gives a
handler 0 for it, so `"type": "integer"` refuses it with `must be from
-9223372036854775808 to 9223372036854775807`. Where the schema allows a
`number`, it passes, and the handler reads it with `toDouble()`. So
`{"not": {"type": "integer"}}` lets 1e19 through, where a client that
reads 2020-12 refuses it. Qt's parser reads a number outside the range of
qint64, and one written with a fraction or an exponent, as the nearest
double, and the check sees what the handler sees. So
-9223372036854775809, down to -9223372036854776832, passes as
-9223372036854775808, 9223372036854775807.0 is refused as 2^63, and
9223372036854775809 is 2^63 against an `exclusiveMinimum` of 2^63.

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
or teardown then has its usual effect, the second one after a signal and
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
content in 2020-12 with a checker of its own (`src/mcpschema*.cpp`), on
Qt's JSON as the handler reads it. Names and strings are compared
character by character, U+0000 included, and numbers by their value. A client
must read 2020-12 and need not read another dialect, so `addTool` refuses
a `$schema` other than `https://json-schema.org/draft/2020-12/schema`
(with or without an empty fragment), and a `$schema` anywhere but at the
root. It refuses as well the keywords that the server does not check,
each with the reason and the JSON pointer of the place (`error`
argument, and a warning).

| | Keywords |
|---|---|
| Checked | `type`, `enum`, `const`, `minimum`, `maximum`, `exclusiveMinimum`, `exclusiveMaximum`, `multipleOf`, `minLength`, `maxLength`, `minItems`, `maxItems`, `uniqueItems`, `minProperties`, `maxProperties`, `required`, `dependentRequired`, `properties`, `additionalProperties`, `propertyNames`, `dependentSchemas`, `prefixItems`, `items` (one schema), `contains`, `minContains`, `maxContains`, `allOf`, `anyOf`, `oneOf`, `not`, `if`, `then`, `else`, `unevaluatedProperties`, `unevaluatedItems`, `$ref` (within the schema), `$defs`, `$anchor`, `$schema` (2020-12, at the root), and `patternProperties` when it is empty |
| Annotations, never checked | `title`, `description`, `default`, `examples`, `deprecated`, `readOnly`, `writeOnly`, `$comment`, `contentMediaType`, `contentEncoding`, `contentSchema`, `format`. 2020-12 makes `format` an annotation unless a schema asks for the format vocabulary, and a client reads it so. The server lets `"format": "email"` pass any string. The value of each is checked for its type. `contentSchema` is checked as any subschema is, by the same rules, as a client that reads the meta-schema checks it, but it is never applied, and a `$ref` cannot lead into it. |
| Refused: regular expressions | `pattern`, and `patternProperties` that is not empty. A regular expression engine run on text that the model chose is a risk of its own, which the server does not take. The recursion of some engines grows with the text, backtracking can take forever, and engines differ on UTF-16 code units and code points. Use `enum`, or check the value in the handler. |
| Refused: references | A `$ref` that leaves the schema (another document, the meta-schema), since the server fetches nothing. A `$ref` that leads back to its own schema without descending into the value, through `allOf`, `anyOf`, `oneOf`, `not`, `if`, `dependentSchemas`, another `$ref`, or `then` and `else` beside an `if` (without one they apply to nothing), which would recurse without end, wherever it is, in a subschema that no value reaches too. Without a loop, a chain of more than 3000 subschemas applied to the same value through `$ref`, and more than 20 million subschemas applied to the same value (two `$ref` to the same schema at each of 23 levels), counting each branch of `allOf`, `anyOf`, `oneOf` and `dependentSchemas`, `not` and `if`, and the larger of `then` and `else` beside an `if`, since a value takes one of the two. The check of a value that takes those branches would stop at its bound. `$id`, `$dynamicRef`, `$dynamicAnchor` and `$vocabulary`. |
| Refused: other drafts | `definitions` (use `$defs`), `dependencies` (use `dependentRequired` or `dependentSchemas`), `additionalItems` (use `items` beside `prefixItems`), `$recursiveRef`, `$recursiveAnchor`, and `items` as an array (use `prefixItems`) |
| Refused: other | every other keyword |

`unevaluatedProperties` and `unevaluatedItems` read the annotations of
2020-12: the members that `properties`, `additionalProperties`,
`unevaluatedProperties` and `dependentSchemas` evaluated, and the items
that `prefixItems`, `items`, `contains` and `unevaluatedItems` evaluated,
in the subschema and in those it applies to the same value. A subschema
that failed evaluates nothing, nor does `not`, nor the branch of `if`
that was not taken, and the condition of `if` evaluates what it matched
when it holds.

A `$ref` is `#` (the whole schema), `#` and a JSON pointer, or `#` and an
`$anchor`. A pointer is percent-decoded and must then be UTF-8, its
characters percent-encoded where a URI cannot hold them
(`#/$defs/a%20b`), with no `~` but in `~0` and `~1`. It must lead to a
subschema through the keywords that hold subschemas, so `#/required` and
`#/properties/a/enum/0` are refused. An anchor (letters, digits, `-`, `_`
and `.`, starting with a letter or `_`, as 2020-12 has it) is compared as
it is written, so `#%66oo` does not name the anchor `foo`. A `$ref` may
lead to a boolean schema, and into `items` beside `prefixItems`. A `$ref`
may recurse through a keyword that descends into the value (`properties`,
`items`, `additionalProperties`, and so on), and the bound of the check
keeps a schema that doubles its work at each level from going on.

Numbers compare exactly. An integral number of the schema or the
arguments that fits a 64-bit integer is held as one, any other as a
double, and an integer and a double compare exactly too. So
`"maximum": 9007199254740993` refuses 9007199254740994, and
9223372036854775807 is below an `exclusiveMaximum` of 2^63. `const`,
`enum` and `uniqueItems` take 1 and 1.0 for the same value, never `true`
and 1, and objects with the same members in any order, whatever the order
in which Qt keeps them. An object built in C++ keeps its members in the
order of their UTF-16 code units, and a parsed one in that of their code
points, which differ for U+E000 to U+FFFF and the characters past U+FFFF.
`required`, `dependentRequired` and `dependentSchemas` look names up in an
index of their own as well, since `QJsonObject::contains()` misses names
in an object that a handler built from a parsed one and then added to.
`uniqueItems` sorts the items by a hash and then by value, so it takes
O(n log n) whatever their number, and its line names the first item that
repeats an earlier one. The hash is keyed by Qt's global seed, random in
each process unless `QT_HASH_SEED` is 0, so a model cannot choose items
whose hashes are the same. `multipleOf` divides exactly when it is an
integer, whatever the value, so 10^17 is not a multiple of 3, and
9223372036854775807 is one of 7. A value written with a fraction or an
exponent, or past the range of qint64, is the double that Qt's parser
reads, and is divided exactly too. Python's `jsonschema` rounds an integer
divisor past 2^53 to the nearest double against a value written with a
fraction or an exponent. So for it 18014398509481986.0 (the double
18014398509481984) is a multiple of 9007199254740993, and for the server
it is not. A `multipleOf` with a fraction divides as doubles, as Python's
`jsonschema` and JavaScript validators do, and the value passes when the
quotient is a whole number. So 0.5 is a multiple of 0.1 (0.5 / 0.1 is 5),
and 0.3 is not (0.3 / 0.1 is 2.9999999999999996), though both are in
decimal. Only when the quotient overflows (1e308 against 0.5) is the test
made exactly on the binary values, as Python does too.

Only the root of an input schema must have `"type": "object"`. The root of
an output schema may be any type, since structured content may be any JSON
value. Keyword values are checked too, everywhere below the root. Lengths
and counts are integers from 0 to 9007199254740991, and `multipleOf` is
greater than 0. `required` and each list of `dependentRequired` may not
name a property twice. Names in `properties`, `required`,
`dependentRequired`, `dependentSchemas` and `$defs`, and the strings and
member names in `enum` and `const`, may not hold an unpaired UTF-16
surrogate. No argument that holds one gets to the check, and no URI
reaches such a name. A number
in `enum`, `const` or a bound must be finite. A Draft 4 boolean
`exclusiveMinimum` and an unknown type name are refused as well. Names
inside `properties`, `$defs`, `dependentRequired` and `dependentSchemas`,
and values inside `enum`, `const`, `default` and `examples`, are data, not
keywords. A tool without an input schema takes no arguments, as with
`{"type": "object", "additionalProperties": false}`.

A schema of more than 10000 subschemas, or with subschemas nested more
than 256 deep, is refused. A tool's schema goes to the model with every
`tools/list`, and Afterglow's largest has some sixty. The compile keeps
for each subschema its parent and one token, not its path, and takes time
in proportion to the schema. In a release build, 9999 empty `properties`
compile in 6 ms, 9037 subschemas 251 levels deep in 6.5 ms, and an `enum`
of 100,000 strings (1.2 MB) in 35 ms. A megabyte of schema 900 levels
deep is refused at once.

## Dependencies

- `QtCore`. No QtGui, QtQml or QtNetwork, so a server inside a command-line
  program pulls in nothing more. The JSON Schema checker is the library's
  own, so there is nothing else to build, install or license. A build
  directory from when the library used valijson or jsoncons keeps their
  entries in its cache, and a configure removes those that the library
  set for itself. `PHOSPHORMCP_USE_SYSTEM_VALIJSON`,
  `PHOSPHORMCP_VALIJSON_INCLUDE_DIR`, `VALIJSON_INCLUDE_DIR` and
  `PHOSPHORMCP_USE_SYSTEM_JSONCONS` go with a warning when they chose
  something (a switch on, a directory set), and the internal entries that
  kept the checks of their headers, and the flags that a development build
  of phosphor-fsloader set to warn once about the first two, go silently.
  The rest stay, since other libraries of the tree set them too.
  `JSONCONS_INCLUDE_DIR` names phosphor-fsloader's headers. The entries of
  `cmake/PhosphorDependencies.cmake`, which fetched valijson
  (`PHOSPHOR_DEPS_ARCHIVES`, and CPM's `CPM_PACKAGES` and
  `CPM_PACKAGE_valijson_SOURCE_DIR`, `_BINARY_DIR` and `_VERSION`), are
  the tree's, and stay in a build directory of the library alone too,
  where nothing reads them.
- Linux, for `pipe2`, `sigtimedwait`, `MSG_NOSIGNAL` and `/proc/self/fd`.

## See also

- [`phosphor-fsloader`](../phosphor-fsloader/README.md). It checks JSON files
  against schemas too, with jsoncons, for data files that the application
  ships or a user writes. This library checks arguments that a model
  chose, so it keeps a checker of its own, with a check before it that
  refuses what a handler would misread, an exact bound on the work, and a
  short list of errors in plain words.
- [`phosphor-ipc`](../phosphor-ipc/README.md). JSON over a Unix socket, for
  scripts and shells that drive a running Phosphor application. This library
  serves an agent that starts the application as a child instead.
- [`phosphor-channel`](../phosphor-channel/README.md). Its `SigpipeBlock` is
  the original of the transport's copy, and the two are kept in step.
- `examples/wordcount.cpp` and `tests/test_phosphor_mcp_example.cpp`. A
  whole server, and a client that runs it.
