<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team -->
<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->

# phosphor-mcp

phosphor-mcp is a Model Context Protocol server over stdio. It is for Phosphor applications that offer tools to an AI
agent. The application declares its tools and answers their calls, and the library speaks the protocol.

## Responsibility

No part of MCP belongs to one application. Without this library, each application that offers tools writes the same
JSON-RPC, version checks and schema checks. `phosphor-mcp` holds them in one place, so a new protocol revision changes
one library.

The library owns these parts:

| Part | What it does |
| --- | --- |
| The stdio transport | One JSON-RPC message on each line of stdin and stdout, read with a `QSocketNotifier`. A line can be up to 16 MiB long, unless the application sets another limit. Writes never block the event loop. Output that stdout does not accept now waits in a queue, which drains when the reader reads more. Past a high-water mark of that queue, the server stops reading requests until the client has read its replies. Thus a client that never reads cannot make the memory of the server grow. |
| JSON-RPC 2.0 | Requests, notifications, ids and the error codes. |
| The checks of each request | The `_meta` keys that 2026-07-28 makes required, the protocol version, and the client capabilities that a tool needs. The server keeps nothing from one request for the next. |
| `server/discover`, `tools/list` and `tools/call` | The results with `resultType` and the server info in `_meta`, a fixed tool list, the dispatch of each call to its handler, and the check of the arguments against the input schema of the tool before the handler runs. |
| Calls that run at the same time | Each call is a handle that the handler answers now or later, with cancellation and progress. |
| The end of the session | A signal at the end of stdin and when a read or a write fails, and, on request, at SIGTERM and SIGHUP. |

The application owns its command line, its `QCoreApplication`, its tools and what they do, its server name and
instructions, and what occurs when the server ends.

These are not in the library: an HTTP transport, the `initialize` handshake of older revisions, resources, prompts,
logging, sampling, roots, elicitation, `subscriptions/listen` and tool lists that change. Add one when an application
needs it. Content blocks other than text, image and audio have no typed constructor. They go through
`McpContent::fromJson`.

## Key types

| Type | Purpose |
|------|---------|
| `PhosphorMcp::McpServer` | The server. `setServerInfo`, `setInstructions`, `setCache`, `setMaxLineBytes`, `setMaxPendingWriteBytes`, `addTool(tool, handler)`, `toolProblem(tool)`, `tools()`, `handleTerminationSignals()`, `start()`, `isRunning()`, and `transport()` for the read-only state of the transport. Signal `finished(reason)`. Single-threaded. |
| `PhosphorMcp::McpTool` | A tool definition. Name, title, description, input and output schemas, the four behavior hints, and the client capabilities that a call needs. `McpTool::isValidName(name)` checks a name against the rule of the server. |
| `PhosphorMcp::McpCall` | One `tools/call`, given to the handler. The arguments, the request id and `_meta`, `finish(result)`, `fail(text)`, `progress(value, total, message)`, `onCancelled(callback)`. Move-only. |
| `PhosphorMcp::McpResult` | The answer of a tool. Text, image and audio content, any other block through `McpContent::fromJson`, structured content of any JSON type, and if the call failed. |
| `PhosphorMcp::McpContent` | One content block. Text, an image or audio clip as bytes and a MIME type, or a block given as JSON. |
| `PhosphorMcp::McpTransport` | Lines on two file descriptors, stdin and stdout for the server. Tests give it a socket pair. |
| `McpProtocol.h` | The protocol version, the `_meta` keys and the error codes. |

## Use

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

`start()` returns false, with a warning, in two cases:

- It runs before the `QCoreApplication` exists.
- stdin or stdout is not a pipe, a socket, a character device or a regular file. One such case is a closed stdin or
  stdout whose number the eventfd of the `QCoreApplication` took.

`examples/phosphor-mcp/wordcount.cpp` is a full server with one tool. The test `test_phosphor_mcp_example` runs it as
a client does.

## Wire protocol

The library uses MCP revision **2026-07-28** over stdio, and no other revision. Each line holds one JSON-RPC 2.0
message, in UTF-8, with no newline inside a message. stdout carries protocol messages and nothing else, so the
application must not write to it either.

The diagnostics of the library go to the logging category `phosphormcp`. Qt writes them to stderr in these cases:

- The process has a controlling terminal.
- stderr is a terminal.
- The environment sets `QT_FORCE_STDERR_LOGGING=1`.

Otherwise, with a Qt built with journald support, they go to the systemd journal. A server that an MCP client starts
often has no terminal, because a desktop client or a service does not pass one on. Thus a client that keeps the
stderr of the server as its log gets nothing, unless the variable is set. `examples/phosphor-mcp/wordcount.cpp` sets
it before it creates its `QCoreApplication`, unless the environment already has it.

### Requests

Each request has `params._meta` with `io.modelcontextprotocol/protocolVersion` (a string) and
`io.modelcontextprotocol/clientCapabilities` (an object, `{}` is valid). `io.modelcontextprotocol/clientInfo` and
`progressToken` are optional. A `progressToken` is a string or an integer of at most 64 bits, the same as an id. A
string id or token must be valid UTF-16. The server refuses one with an unpaired surrogate, because it comes back as
U+FFFD and agrees with nothing that the client sent.

A session with the example server, one request on each line:

```json
{"jsonrpc":"2.0","id":1,"method":"server/discover","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{}}}}
{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{}}}}
{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{},"progressToken":"p3"},"name":"count_words","arguments":{"text":"one two"}}}
{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{}},"name":"count_words","arguments":{"text":5,"lang":"en"}}}
```

A client cancels a running call with a notification. The notification gets no answer:

```json
{"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":3,"reason":"..."}}
```

### Results

Each result has `"resultType": "complete"` and the name and version of the server in `_meta`. `phosphor-mcp-example`
answers the first three requests of the session above with these lines, byte for byte. Qt writes the members of each
object in the order of their names.

```json
{"id":1,"jsonrpc":"2.0","result":{"_meta":{"io.modelcontextprotocol/serverInfo":{"name":"wordcount","version":"0.2.0"}},"cacheScope":"public","capabilities":{"tools":{}},"instructions":"count_words counts the words and characters of a text.","resultType":"complete","supportedVersions":["2026-07-28"],"ttlMs":3600000}}
{"id":2,"jsonrpc":"2.0","result":{"_meta":{"io.modelcontextprotocol/serverInfo":{"name":"wordcount","version":"0.2.0"}},"cacheScope":"public","resultType":"complete","tools":[{"annotations":{"destructiveHint":false,"idempotentHint":true,"openWorldHint":false,"readOnlyHint":true},"description":"Counts the words (runs of characters that are not spaces) and the characters (Unicode code points) of a text.","inputSchema":{"additionalProperties":false,"properties":{"text":{"description":"The text to count","maxLength":1000000,"type":"string"}},"required":["text"],"type":"object"},"name":"count_words","outputSchema":{"properties":{"characters":{"type":"integer"},"words":{"type":"integer"}},"required":["words","characters"],"type":"object"},"title":"Count words"}],"ttlMs":3600000}}
{"id":3,"jsonrpc":"2.0","result":{"_meta":{"io.modelcontextprotocol/serverInfo":{"name":"wordcount","version":"0.2.0"}},"content":[{"text":"{\"characters\":7,\"words\":2}","type":"text"}],"resultType":"complete","structuredContent":{"characters":7,"words":2}}}
```

While a call runs, if its request had a `progressToken`, each `progress()` sends a notification in this form:

```json
{"jsonrpc":"2.0","method":"notifications/progress","params":{"message":"...","progress":1,"progressToken":"p3","total":2}}
```

| Member | When it is there |
| --- | --- |
| `instructions` | Only when the application gives instructions. |
| `ttlMs` and `cacheScope` | Always, because clients need both. |
| `capabilities.tools` | Only when the server has tools. `start()` gives a warning when it has none. |

`tools/list` sends the full list, in the order in which the application added the tools, and has no pages.

Without `setServerInfo`, the server uses the name of the application and its version, or `"0"` when it has none.
`QCoreApplication` sets the name to the name of the executable, unless the application gives another name.

Replies are compact JSON. UTF-8 cannot hold a lone UTF-16 surrogate in a string, so it goes out as U+FFFD.

### Errors

| Code | When |
|------|------|
| -32700 Parse error | A line that is not JSON. The server skips empty and blank lines, with no answer. |
| -32600 Invalid Request | A batch (a JSON array), valid JSON that is not an object, a line over the limit, no `jsonrpc: "2.0"`, no `method` in a message that is not a response, a method that is not a string, an id that is not a valid UTF-16 string or an integer of at most 64 bits, and a `tools/call` whose id another running call has. |
| -32601 Method not found | Each method but the three above, `ping` included. Also `initialize`, with a message that names 2026-07-28 and `data: {"supported": ["2026-07-28"], "requested": ...}`. `requested` is there when `params.protocolVersion` is a string. |
| -32602 Invalid params | `params` that is null or not an object, no `_meta`, a missing or wrong-typed required `_meta` key, a `progressToken` that is not a valid UTF-16 string or an integer of at most 64 bits, a `cursor` on `tools/list`, a `tools/call` with no `name`, an unknown tool, and `arguments` that are not an object. |
| -32603 Internal error | A call dropped without an answer. Also, for a tool with an output schema, a result whose structured content does not match the schema (failed results included), holds a number that is not finite, or is missing from a result that did not fail. `data.errors` names each place. |
| -32021 | The request did not declare a client capability that the tool requires. `data: {"requiredCapabilities": {...}}` holds all capabilities that the tool requires, the declared ones too. The prose of the spec says the missing ones, and its schema, `schema.ts`, says the ones that the server requires. The schema is the source of truth, and the library follows it. |
| -32022 | A protocol version other than 2026-07-28. The server checks it before the other `_meta` keys. `message: "Unsupported protocol version"`, `data: {"supported": ["2026-07-28"], "requested": "<version>"}`. |

An error has the id of the request. When the server cannot read the id, the answer has no id at all. This occurs for a
line that is not JSON, a batch, a value that is not an object, a line over the limit, and an id of the wrong type. The
schema of MCP has `id?: string | number`, so the server leaves out the id, and does not send null.

### Notifications and responses

The server never answers a notification (a message with a method and no id), not even when its params are wrong. It
ignores all notifications but `notifications/cancelled`, and logs the reason of a cancel at debug level.

A message without an id that is not a notification gets an Invalid Request error with no id. This occurs when it has
no `jsonrpc: "2.0"`, no method, or a method that is not a string. The server drops a response (a `result` or an
`error` without a method), because it sends no requests. It never sends `notifications/cancelled`,
`notifications/message`, `ping` or any other request.

## Argument errors

Arguments that do not match the input schema of the tool are not a protocol error. The model made the mistake and can
correct it. Thus the answer is a result with `isError: true` and a text that names the tool and each bad field by its
path. Request 4 of the session above gets this text:

```
Invalid arguments for tool count_words:
- arguments/lang: is not a known property
- arguments/text: must be of type string
```

The text lists at most ten errors, then a line such as `- arguments: and 3 more errors`. Some arguments get one line
in place of the list. The line says that they do not match, and that they are too large for the server to list each
error. This occurs when they have more than a thousand distinct errors, or when the paths of their errors have more
than 1 Mi UTF-16 code units in total. The check stops there.

The places that the server refuses before the check (see "Refused before the check") are errors as the others are,
ten listed and a thousand counted. Past a thousand, their one line says only that the server refuses the arguments
before the check, with too many errors to list each one. The check that tells if they match never ran.

### Text of each line

Each line comes from the keyword that failed, its value in the schema and the arguments: `must be of type integer`,
`is required`, `is not a known property`, `must be at most 5 characters long`, `must be one of the values of enum`,
`is required when "card" is present`, `must have an item that matches contains`,
`must have unique items, and items 1 and 3 are equal`, and others. A number in a line is the same in each locale. An
integer has all its digits (10000000000000000000 also for 1e19, below 10^21). Each other number has its shortest form
that reads back as the same double (`0.1`, `1e-07`, `1e+300`).

### Order of the errors

The errors of a subschema come in the order of its keywords:

1. The keywords that apply no subschema (`type`, `const`, `enum`, and the keywords of numbers and strings).
2. The keywords of an array (its counts, `uniqueItems`, `prefixItems`, `items`, `contains`) or of an object. The
   members of an object come in the order in which Qt keeps them, which is the order of their names. Each member
   comes with the errors of its name and of its value. Then come the properties that `required` and
   `dependentRequired` do not find, and the errors of `dependentSchemas`.
3. The subschemas applied to the same value (`$ref`, `allOf`, `anyOf`, `oneOf`, `not`, `if`, `then` and `else`).
4. The members and items that `unevaluatedItems` and `unevaluatedProperties` refuse.

A failed `anyOf`, or a `oneOf` that matches no branch, lists the errors of each branch and then its own line. A
`oneOf` that matches more than one branch says only that. A failed `allOf` lists the errors of its branches. A `false`
subschema has no reason of its own, so its line names the place and says that it does not match the schema. The text
never lists what `not`, the condition of an `if`, and the items that `contains` tries find, because these are not
errors of the arguments.

Each member that `additionalProperties: false` or `unevaluatedProperties: false` refuses has its line. So has each
item that `items: false` or `unevaluatedItems: false` refuses. The errors that the schema of `additionalProperties`,
`unevaluatedProperties` or `unevaluatedItems` finds are at the member or item. The errors of `propertyNames` are at
the member, as `the property name must be at most 256 characters long`. The text does not call a member or item
unknown when a keyword of the schema evaluated it, also when its value failed there. The same is true when a failed
branch of a failed `anyOf` or `oneOf` declares it. The arguments fail in both cases, and the important line is the
one about its value.

## Bound of the check

The check has an exact bound. The checker counts its work as it goes, and stops immediately when the count passes the
bound, with the line `- arguments: is too large for the server to check against the schema`. The checker estimates
nothing before it starts.

Each step costs units for its work. A unit is at most about 20 ns of a release build. These are the steps:

- a value walked
- a subschema applied to a value, and more in place or in a large schema
- a keyword checked
- a member visited
- a value taken out of its array or object
- a name looked up
- a name or a string read away from the last one read of its object or array
- sixteen code units of a string or a name, or fewer, read, compared or hashed
- a word of an annotation set merged
- an error built

A sweep measured each charge of the checker with the shapes that make it dominate. It used these inputs:

- each encoding that Qt keeps text in
- short and long strings and names (near each sixteen code units)
- numbers of each type
- large schemas and checks deep in them
- names in random order
- strings and other values taken out of their arrays and objects, and compared far apart
- schema tables of up to a million entries

For a check of a millisecond or more, while little else contended for the caches and for memory, it measured at most
about 16 ns a unit. That is some 1.6 times what a list of integers checked for their type measures in the same build
(10 ns a unit). The most expensive shapes read an object or an array of 16 MiB again for each of 40 or 50
subschemas, as fast as memory supplies it, or apply thousands of subschemas in random order. These become slower when
other processes contend for the caches and for memory, but the list of integers does not. Under such load the most
expensive shapes measured up to about 20 ns a unit, two times the value of the list.

A check of less than a millisecond also has fixed costs, such as the first use of the stack. These costs do not grow
with the arguments.

| Limit | Value |
| --- | --- |
| Bound of the check | 40 million units, less than a second for all schemas and arguments |
| Time of a check that ends at the bound, without load | up to some 0.6 s |
| Time of a check that ends at the bound, with load | up to some 0.8 s |

The shapes in "Measured times" were measured the same way.

### Strings and names

The checker reads strings and names where the parser of Qt keeps them. It never decodes them to compare, hash or
count them, not even the names that `propertyNames` checks as strings. The parser keeps ASCII text in Latin-1, other
text in UTF-8, and text with an escape (`\n`, `\u00e9`) in UTF-16. A code unit of UTF-8 is a byte. Thus a string of
CJK characters in UTF-8 costs three times the same string in UTF-16. Text compared for equality with text in another
encoding, where one of the two is UTF-8, costs four times as much, because Qt decodes the UTF-8 while it compares. The
checker counts the length of a string one time for all subschemas applied to it in place.

### Memory order

Qt keeps the members of an object sorted by name, but their names and strings stay in the order that the arguments
wrote them. Thus the names of a large object written in random order are read from all over its storage, each a
cache miss. Each name or string read costs a unit for each 64 bytes between it and the last one read of its object
or array, up to 8. That is about a miss of the cache and of the TLB. An array of a handler whose strings were inserted
out of order costs the same.

A string that Qt takes out of its array or object to check it holds a reference to the storage of the container
during the check, and costs units for it. So does each value taken out of one to compare or hash it. `uniqueItems`
compares items in the order of their hashes, far apart in the array. It charges the read of each item away from the
last one read of its run of one hash. It charges the first item of each run as far as possible, because the seed
decides the order of the runs.

### Subschemas and tables

A subschema applied through `allOf`, `anyOf`, `oneOf`, `not`, `if`, `then`, `else`, `$ref` or `dependentSchemas`
costs a unit more than one applied to a member or an item. Each subschema applied costs one more unit when the
subschemas of the schema and the frames of the check (a kilobyte for each subschema it is in) come to 256 KiB. It costs
one more at each fourfold, up to 3, because the caches no longer hold them.

A string looked up in an `enum` of more than eight strings costs 4 units more when the tables of the full schema come
to 256 KiB, and two times as many at each fourfold, up to 16, because each lookup then misses the caches. The same is
true for a name looked up in `properties` or in the triggers of `dependentRequired`. It is also true for each trigger
of `dependentRequired` that an object has, one time for its entry and one more time for its list of names when it has
one. The schema sets the size of its tables, and the server does not limit it.

### Pointers and memory

The checker decodes a name, and escapes its `~` and `/`, only for the JSON pointer of an error. It does this one time
for all errors at its place and below it while the check stays there, and again when the check comes back. The text
of a pointer costs units for each full sixteen code units. It is not rounded up as other text is, because the error
and each of its tokens have their own units.

The checker also counts the memory that the check holds beyond the arguments and the schema, up to 64 MiB. That memory
holds these items:

- the sets of members and items that `unevaluatedProperties` and `unevaluatedItems` read
- the keys that `uniqueItems` sorts
- the index of the names of an object, in which `required`, `dependentRequired` and `dependentSchemas` look up names
- the members of an object sorted by name for a comparison
- the list of the triggers of `dependentRequired` that an object has

The list of errors has the limits in "Argument errors".

### Depth

| Limit | Value |
| --- | --- |
| Subschemas deep | 3000 |
| Stack of the frames of the check | 4 MiB, half of the default stack of a Linux thread |

Each subschema applied uses about a kilobyte of stack in a release build, so there the depth stops a check first. A
release build checks a list nested 1000 levels deep through `items` and `$ref`. It refuses a list whose levels each go
through three more `$ref` at 600 levels.

### Measured times

The table gives the CPU time of `validate()` in a release build, with the walk before the check included. The
arguments were parsed from a request line as the server parses them. They were the largest arguments in a line of
16 MiB, or as many as the bound accepts. `test_phosphor_mcp_checkwork` counts exactly the units of each step.
`test_phosphor_mcp_bounds` checks many of them through the server, at sizes that a debug build with sanitizers checks
in seconds.

| Schema and arguments | Arguments | Result | Time |
|---|---|---|---|
| `items: {"type": "integer"}` | 6,600,000 integers | valid | 0.39 s |
| the same | 6,700,000 integers | too large to check | 0.40 s |
| the same | 8,000,000 integers (16 MiB) | too large to check | 0.36 s |
| `items` with `type`, `minimum`, `maximum`, `multipleOf` | 4,000,000 integers | valid | 0.32 s |
| `items: {"type": "string"}` | 8,000,000 integers, every one wrong | too large to list | 0.14 s |
| two `properties` | 1,000,000 objects | valid | 0.37 s |
| two `$ref` to the schema at each level (2^depth subschemas) | 40 levels | too large to check | 0.32 s |
| `anyOf` in `anyOf` 50 deep, the last branch matching | 100,000 integers | valid | 0.23 s |
| `allOf` of 50 `maxLength` | 4,000,000 CJK characters | valid | 0.003 s |
| `allOf` of 300 `maxLength`, below the string's bytes | 5,500,000 CJK characters (16 MiB) | valid | 0.004 s |
| `enum` of 3000 strings | 5,000,000 CJK characters | one line | 0.002 s |
| `enum` of 3000 strings of 100 CJK characters | 50,000 such strings | valid | 0.01 s |
| `const` of an object of 100 members named with one or two letters | 20,000 such objects | valid | 0.19 s |
| `required` of 1000 names inside `not` | 200,000 empty objects | valid | 0.02 s |
| `allOf` of 300 `required` | an object of 1,000,000 members | valid | 0.12 s |
| `dependentRequired` of 2000 triggers | 1,000,000 empty objects | valid | 0.08 s |
| `enum` of 100,000 strings | 1,000,000 of its strings | valid | 0.21 s |
| `allOf` of 8 `enum`s of 100,000 strings | 600,000 strings that each holds | too large to check | 0.36 s |
| `dependentRequired` of 100,000 triggers, each requiring itself | 900,000 objects of one trigger | too large to check | 0.32 s |
| `additionalProperties: {"type": "integer"}` | 1,000,000 members | valid | 0.08 s |
| the same | 1,000,000 members written in random order | valid | 0.25 s |
| `allOf` of 50 `additionalProperties` | 300,000 members named with CJK characters | too large to check | 0.34 s |
| the same | 300,000 such members written in random order | too large to check | 0.44 s |
| `propertyNames: {"maxLength": 20}` | 1,000,000 members | valid | 0.07 s |
| the same | 1,000,000 members written in random order | valid | 0.25 s |
| `propertyNames: {"maxLength": 50}` | 134,000 members of 40 CJK characters (16 MiB) | valid | 0.01 s |
| Afterglow's `env` without its `maxProperties` (`additionalProperties` with `anyOf`, `propertyNames`) | 300,000 members | valid | 0.05 s |
| `contains` that no item matches | 4,000,000 integers | one line | 0.23 s |
| `prefixItems`, `items` and `unevaluatedItems` | 4,000,000 integers | valid | 0.24 s |
| `allOf` of 200 `properties` and `unevaluatedProperties` | 30,000 members | valid | 0.14 s |
| `uniqueItems` | 1,000,000 distinct integers (32 MB of keys) | valid | 0.13 s |
| `uniqueItems` | 750,000 equal objects | one line | 0.27 s |
| `uniqueItems` | 300,000 distinct CJK strings | valid | 0.06 s |
| `uniqueItems`, items chosen for one hash under `QT_HASH_SEED=0` | 175,000 pairs | valid | 0.26 s |
| the same | 200,000 pairs | too large to check | 0.29 s |
| `uniqueItems` | 2,100,000 items (past 64 MiB of keys) | too large to check | 0.04 s |
| a list of lists through `items` and `$ref` | 1000 levels | valid | 0.001 s |
| `{"type": "object"}` | 5,500,000 empty arrays (16 MiB) | valid | 0.13 s |

Thus the server checks a valid list of 6.6 million small integers under `items: {"type": "integer"}`. It refuses a list
of 8 million, the most that a 16 MiB line holds, after 0.4 s. Each row that ends at the bound took 0.29 to 0.44 s,
because its units cost less than the most expensive ones.

A million members written in random order take three times as long as the same members in order, each a cache miss.
They cost about two times as many units. Items chosen for one hash need the seed, which a model does not know. With the
random seed of a running server their hashes are different, and 550,000 such pairs are valid in 0.14 s.

The walk before the check costs 3 units for each value, and 5 for an array or an object. Thus the walk of the most
values that a 16 MiB line holds costs some 28 million units. The parser of Qt also uses time and memory for such a
line (some 130 MB for 5.5 million empty arrays), and the line limit sets a bound on it. An application whose tools take
no long text decreases the limit with `setMaxLineBytes`.

## Refused before the check

Before the check, the server refuses values that a handler reads differently from the check, or that a reply cannot
carry. It gives a line for each place. These values are refused:

- a string or a member name with an unpaired UTF-16 surrogate (the reply writes it as U+FFFD)
- a number that is not finite (only a handler can build one, in structured content)
- a value nested more than 1024 levels deep (only a handler can build one too)

A number without a fraction outside the range of a 64-bit integer (1e19, 2^63, 1e300) is an integer in JSON Schema.
But `QJsonValue::toInteger()` gives a handler 0 for it. Thus `"type": "integer"` refuses it with
`must be from -9223372036854775808 to 9223372036854775807`. Where the schema allows a `number`, it passes, and the
handler reads it with `toDouble()`. Thus `{"not": {"type": "integer"}}` accepts 1e19, but a client that reads
2020-12 refuses it.

The parser of Qt reads a number outside the range of qint64, and a number with a fraction or an exponent, as the
nearest double. The check sees what the handler sees. Thus:

- -9223372036854775809, down to -9223372036854776832, passes as -9223372036854775808.
- 9223372036854775807.0 is refused as 2^63.
- 9223372036854775809 is 2^63 against a `maximum` of 9223372036854775807.

## Calls answered later

A handler does not have to answer before it returns. It can keep the `McpCall` and answer when the work ends. The
server reads and runs other requests in the meantime. Replies go out in the order in which the calls finish.

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

| Rule | Detail |
| --- | --- |
| One answer | The first `finish` or `fail` stays. After it, after a cancellation and after the server ended, `finish`, `fail` and `progress` send nothing and return false. `finish` returns true when the reply is in the queue for the client. This does not tell that the client read it. |
| Never left waiting | A call destroyed without an answer (or overwritten by a move) answers with a -32603 error and a warning. |
| Cancellation | `notifications/cancelled` for a running call runs its `onCancelled` callback one time, and nothing more goes out for the request. A callback set on a call that is already cancelled runs immediately. A second `onCancelled` replaces the first, which never runs. An empty one changes nothing. The server ignores a cancel for an id that is not running. |
| Progress | `progress(value, total, message)` sends `notifications/progress` only when the request had a `progressToken`. Each value must be larger than the value before it, and finite. Nothing goes out after the answer. Nothing goes out while the client has more than the high-water mark of replies not read (see "Flow control"). The server does not send a dropped notification later. |
| Failed calls | `fail(text)`, or a result with `setError(true)`, is a result that the model reads. Say what went wrong and what to do. |
| Structured content | Any JSON value. A tool with an output schema answers each call that did not fail with structured content that matches the schema. A failed result that has structured content must also match it. The library adds the compact JSON of the value as the last text block, as the spec asks, so do not add it yourself. A value that does not match is a bug of the tool: the client gets -32603, and `finish` returns false. A number that is not finite has no JSON form. With an output schema the server refuses it the same way. Without one it goes out as null, with a warning. |
| Large results | An image or audio clip goes out as base64, 4/3 of its bytes. While the server builds and writes the reply, the JSON objects and the serialized line each hold that text again. Thus the peak memory grows for a short time by about eight times the size of the clip (133 MiB for 16 MiB, measured). The reply in the queue then holds 4/3 of it until the client reads it. |
| Move-only, one thread | A moved-from call holds nothing. Use calls on the thread of the server. On another thread, a call does nothing, with a warning. A call can outlive the server, or be destroyed with it inside a handler or a cancel callback that owns it. When the server is destroyed, it sends nothing and runs no callback, and an unanswered call then reads `isCancelled()` true. Never destroy the server from inside a handler or a cancel callback. Call `deleteLater()` there. |
| No exceptions | Handlers and cancel callbacks must not throw. The library is built without exceptions, and an exception that leaves a callback is undefined behavior. |

## End of the session

`finished(reason)` comes one time, from the event loop.

| Reason | When |
|--------|------|
| `EndOfInput` | stdin ended, so the client is done. A client on a socket that exits with replies it did not read resets the connection, and that is also an end. The server still reads a last line without a newline. The replies already in the queue go out first, until the client reads none of them for `DrainTimeoutMs` (2 s). Each read starts that wait again. |
| `InputFailed` | A read of stdin failed, with a warning. The replies already in the queue still go out, as after `EndOfInput`. |
| `OutputClosed` | stdout went away (EPIPE). |
| `OutputFailed` | A write to stdout failed for another reason (ENOSPC, EIO), with a warning. |
| `Terminated`, `HangUp` | SIGTERM or SIGHUP, after `handleTerminationSignals()`. The server ends immediately, and drops the replies still in the queue. |

A signal, or an output that closes or fails, during that drain ends it immediately, with its own reason.

Before `finished`, the server cancels each call that still runs. Its callback runs, so the application stops the work.
The server does not send an answer that comes later (`finish` returns false). This is also true for the end of stdin.
A client that closes stdin immediately after a request that the handler answers later gets no answer to it. The
server reads nothing more, and when `finished` comes, it writes nothing more.

### Termination signals

`handleTerminationSignals()` installs handlers for SIGTERM and SIGHUP that write to a self-pipe. Signal dispositions
are global to the process. Thus the application asks for it, and one server at a time holds them.

When the server ends without a wait for its replies (a signal, a closed or failed output), it puts back the handlers
that it found. It does this before the cancel callbacks run, unless another handler was installed after it. A signal
during a slow callback or teardown then has its usual effect. That is the second signal after a signal, and the first
after a closed or failed output.

After the end of stdin or a failed read, the server keeps the signals while its replies drain, so that a signal can
stop the wait. It puts them back when it finishes (at the latest when it is destroyed). But the server reads a signal
from its event loop. Thus a signal that comes while a cancel callback blocks without an event loop is read only after
the callbacks returned. If the server finished before that, the signal is not read.

A signal that the process ignores (SIGHUP under `nohup`) stays ignored. `handleTerminationSignals()` returns false,
with a warning, before the `QCoreApplication` exists and after the server ended.

A child forked before `exec` gets the disposition that it had without the server, not the handler of the server. Thus
a process that forks to keep the server running in the child, as a daemon does, calls `handleTerminationSignals()` in
the child, after the fork. It does not call it in the parent before the fork. After a call in the parent, a call in
the child returns false, with a warning, while the child holds its copy of that server, because the handler stays the
handler of the parent. When the child has destroyed its copy, a server in the child gets the signals.

### SIGPIPE

The library never changes the disposition of SIGPIPE. An ignored SIGPIPE stays through `exec`. Thus each program that
the application starts gets it, and a shell pipeline like `yes | head` in such a program no longer ends correctly. A
closed stdout still never stops the process:

- The transport writes a socket with `MSG_NOSIGNAL`.
- Around a write to any other output, the transport blocks SIGPIPE in its own thread and takes back the SIGPIPE that
  the write caused. It does this also when the write copied part of its data before the reader went away.

The write fails with EPIPE, and the server ends with `OutputClosed`.

## Flow control

Replies wait in a queue when stdout does not accept them immediately. The queue has a high-water mark,
`setMaxPendingWriteBytes` (64 MiB by default). Past it, these occur:

- The server reads no more requests.
- The lines that it has read also wait.
- The server drops progress notifications.

The server reads again when the queue is down to half the mark. It never drops replies, and it still writes a single
reply that is larger than the mark in full. A client that sends requests and never reads the replies then fills its
own pipe, not the memory of the server.

| Setting | Default |
| --- | --- |
| `setMaxLineBytes` | 16 MiB |
| `setMaxPendingWriteBytes` | 64 MiB |

Both settings have an effect immediately, also on a running server. A value less than 1 counts as 1.

A handler that runs a nested event loop (`QEventLoop::exec`) sees no new request inside it. The server reads nothing
while it gives out the requests that it has read. Thus a cancel of that call and the end of stdin wait until the
handler returns, as all other requests do. Answer later through the `McpCall`, and do not wait in a nested loop.

## stdout and terminals

Writes to stdout never block the event loop. How the server does this depends on what stdout is.

- A socket: the server writes it with `send(MSG_DONTWAIT | MSG_NOSIGNAL)`, and its flags never change.
- A terminal: the server opens it again through `/proc/self/fd/1`, for a non-blocking file description of its own.
  The terminal that a shell shares with the server keeps its flags, also when the server is killed. In some cases the
  server gives a warning and treats the terminal as a pipe, as below. These cases are: the terminal cannot be opened
  again (a pty that another user owns, after `su`, or no `/proc`), it opens as another device or as one that the
  server cannot identify, or it is a pty master. Its shared description is then non-blocking until the server ends.
- Any other output, such as the pipe that an MCP client gives: the server sets `O_NONBLOCK`. At the end, it clears
  only that bit, and only when it set it.

## Schemas

MCP schemas are JSON Schema 2020-12 unless they name another dialect, and clients read them that way. The library
checks arguments and structured content in 2020-12 with its own checker (`src/mcpschema*.cpp`), on the JSON of Qt as
the handler reads it. It compares names and strings character by character, U+0000 included, and numbers by their
value.

A client must read 2020-12 and does not have to read another dialect. Thus `addTool` refuses a `$schema` other than
`https://json-schema.org/draft/2020-12/schema` (with or without an empty fragment), and a `$schema` that is not at
the root. It also refuses the keywords that the server does not check. Each refusal gives the reason and the JSON
pointer of the place (`error` argument, and a warning).

| | Keywords |
|---|---|
| Checked | `type`, `enum`, `const`, `minimum`, `maximum`, `exclusiveMinimum`, `exclusiveMaximum`, `multipleOf`, `minLength`, `maxLength`, `minItems`, `maxItems`, `uniqueItems`, `minProperties`, `maxProperties`, `required`, `dependentRequired`, `properties`, `additionalProperties`, `propertyNames`, `dependentSchemas`, `prefixItems`, `items` (one schema), `contains`, `minContains`, `maxContains`, `allOf`, `anyOf`, `oneOf`, `not`, `if`, `then`, `else`, `unevaluatedProperties`, `unevaluatedItems`, `$ref` (within the schema), `$defs`, `$anchor`, `$schema` (2020-12, at the root), and `patternProperties` when it is empty |
| Annotations, never checked | `title`, `description`, `default`, `examples`, `deprecated`, `readOnly`, `writeOnly`, `$comment`, `contentMediaType`, `contentEncoding`, `contentSchema`, `format`. 2020-12 makes `format` an annotation unless a schema asks for the format vocabulary, and a client reads it that way. The server lets `"format": "email"` pass any string. The server checks the type of the value of each annotation. It checks `contentSchema` as it checks any subschema, by the same rules, as a client that reads the meta-schema does. But it never applies `contentSchema`, and a `$ref` cannot lead into it. |
| Refused: regular expressions | `pattern`, and `patternProperties` that is not empty. A regular expression engine that runs on text that the model chose is a risk of its own, and the server does not take it. The recursion of some engines grows with the text, backtracking can take forever, and engines are different on UTF-16 code units and code points. Use `enum`, or check the value in the handler. |
| Refused: references | A `$ref` that leaves the schema (another document, the meta-schema), because the server fetches nothing. A `$ref` that leads back to its own schema without descending into the value, through `allOf`, `anyOf`, `oneOf`, `not`, `if`, `dependentSchemas`, another `$ref`, or `then` and `else` next to an `if` (without one they apply to nothing). It would recurse without end, wherever it is, also in a subschema that no value gets to. Without a loop, a chain of more than 3000 subschemas applied to the same value through `$ref`, and more than 20 million subschemas applied to the same value (two `$ref` to the same schema at each of 23 levels). This count includes each branch of `allOf`, `anyOf`, `oneOf` and `dependentSchemas`, `not` and `if`, and the larger of `then` and `else` next to an `if`, because a value takes one of the two. The check of a value that takes those branches would stop at its bound. `$id`, `$dynamicRef`, `$dynamicAnchor` and `$vocabulary`. |
| Refused: other drafts | `definitions` (use `$defs`), `dependencies` (use `dependentRequired` or `dependentSchemas`), `additionalItems` (use `items` beside `prefixItems`), `$recursiveRef`, `$recursiveAnchor`, and `items` as an array (use `prefixItems`) |
| Refused: other | every other keyword |

### Annotations of evaluation

`unevaluatedProperties` and `unevaluatedItems` read the annotations of 2020-12. These are the members that
`properties`, `additionalProperties`, `unevaluatedProperties` and `dependentSchemas` evaluated, and the items that
`prefixItems`, `items`, `contains` and `unevaluatedItems` evaluated. They come from the subschema and from the
subschemas that it applies to the same value. A subschema that failed evaluates nothing. Also, `not` and the branch of
`if` that was not taken evaluate nothing. The condition of `if` evaluates what it matched when it holds.

### References

A `$ref` is `#` (the full schema), `#` and a JSON pointer, or `#` and an `$anchor`.

- The checker percent-decodes a pointer, which must then be UTF-8. A character that a URI cannot hold must be
  percent-encoded (`#/$defs/a%20b`). A `~` is allowed only in `~0` and `~1`.
- A pointer must lead to a subschema through the keywords that hold subschemas. Thus the checker refuses
  `#/required` and `#/properties/a/enum/0`.
- An anchor has letters, digits, `-`, `_` and `.`, and starts with a letter or `_`, as in 2020-12. The checker
  compares it as it is written, so `#%66oo` does not name the anchor `foo`.
- A `$ref` can lead to a boolean schema, and into `items` next to `prefixItems`.
- A `$ref` can recurse through a keyword that descends into the value (`properties`, `items`,
  `additionalProperties`, and others). The bound of the check stops a schema that doubles its work at each level.

### Numbers

Numbers compare exactly. An integral number of the schema or the arguments that fits a 64-bit integer is held as an
integer, and any other number as a double. An integer and a double also compare exactly. Thus
`"maximum": 9007199254740993` refuses 9007199254740994, and 9223372036854775807 is less than an `exclusiveMaximum` of
1e19.

`const`, `enum` and `uniqueItems` take 1 and 1.0 as the same value, but never `true` and 1. They take objects with the
same members in any order as the same, whatever the order in which Qt keeps them. An object built in C++ keeps its
members in the order of their UTF-16 code units. A parsed object keeps them in the order of their code points. The two
orders are different for U+E000 to U+FFFF and for the characters past U+FFFF. `required`, `dependentRequired` and
`dependentSchemas` also look up names in their own index. The reason is that `QJsonObject::contains()` does not find
names in an object that a handler built from a parsed one and then added to.

`uniqueItems` sorts the items by a hash and then by value. Thus it takes O(n log n) for any number of items, and its
line names the first item that repeats an earlier one. The global seed of Qt at the time the schema compiled is the
key of the hash. The seed is random in each process unless `QT_HASH_SEED` is 0, so a model cannot select items whose
hashes are the same. The work of the check is the same for all seeds. The only exception is distinct items or names
whose hashes are the same by chance, about one pair in 2^64.

`multipleOf` divides exactly when it is a whole number, for all values. Thus 10^17 is not a multiple of 3,
9223372036854775807 is a multiple of 7, and 5e-324 is not a multiple of 1e19. A value with a fraction or an exponent,
or past the range of qint64, is the double that the parser of Qt reads, and is also divided exactly.

Python's `jsonschema` is different in three ways. For the server, none of the three is a multiple.

| Python's `jsonschema` | Example |
| --- | --- |
| It rounds an integer divisor past 2^53 to the nearest double against a value with a fraction or an exponent. | For it, 18014398509481986.0 (the double 18014398509481984) is a multiple of 9007199254740993. |
| It keeps an integer of the arguments past the range of qint64 exact. The server divides the double 2^63. | For it, 9223372036854775809 is a multiple of 3. |
| It reads a divisor with an exponent as a float and divides as floats. | For it, 5e-324 is a multiple of 1e19, because the quotient underflows to 0. |

A `multipleOf` with a fraction divides as doubles, as Python's `jsonschema` and JavaScript validators do. The value
passes when the quotient is a whole number. Thus 0.5 is a multiple of 0.1 (0.5 / 0.1 is 5), and 0.3 is not (0.3 / 0.1
is 2.9999999999999996), but both are multiples in decimal. Only when the quotient overflows (1e308 against 0.5) does
the test use the exact binary values, as Python also does.

### Keyword values

Only the root of an input schema must have `"type": "object"`. The root of an output schema can be any type, because
structured content can be any JSON value. The checker also checks keyword values, everywhere below the root:

- Lengths and counts are integers from 0 to 9007199254740991, and `multipleOf` is greater than 0.
- `required` and each list of `dependentRequired` cannot name a property two times.
- Names in `properties`, `required`, `dependentRequired`, `dependentSchemas` and `$defs`, and the strings and member
  names in `enum` and `const`, cannot hold an unpaired UTF-16 surrogate. No argument that holds one gets to the
  check, and no URI gets to such a name.
- A number in `enum`, `const`, a bound or `multipleOf` must be finite.
- A Draft 4 boolean `exclusiveMinimum` and an unknown type name are refused.

The checker refuses a whole number in these places when `tools/list` would send it rounded to another integer, as Qt
writes it. Thus a client compares with the same number as the server. Qt writes a double in its shortest form, so
2^63 goes out as 9223372036854776000. A client that reads integers exactly, as Python does, reads that integer. A whole
number in the range of a 64-bit integer that an application built in C++ as a double goes out the same way. Thus
`QJsonValue(4611686018427387904.0)`, which is 2^62, goes out as 4611686018427388000. When it is parsed from JSON, or
built as an integer, it goes out with all its digits. 1e19 and 1e300 go out with an exponent, and are read as the same
double.

Names inside `properties`, `$defs`, `dependentRequired` and `dependentSchemas`, and values inside `enum`, `const`,
`default` and `examples`, are data, not keywords. A tool without an input schema takes no arguments, the same as with
`{"type": "object", "additionalProperties": false}`.

### Schema size

| Limit | Value |
| --- | --- |
| Subschemas in a schema | 10000 |
| Depth of nested subschemas | 256 |

The server refuses a schema past these limits. The schema of a tool goes to the model with each `tools/list`, and the
largest schema of Afterglow has some sixty subschemas. For each subschema, the compile keeps its parent and one token,
not its path. Its time is in proportion to the schema. These times were measured in a release build, on a machine
loaded with other work:

| Schema | Compile time |
| --- | --- |
| 9999 empty `properties` | 8 to 15 ms |
| some 9000 subschemas 251 levels deep | 8 to 13 ms |
| an `enum` of 100,000 strings (1.1 MB) | 36 to 43 ms |

The server refuses a megabyte of schema 900 levels deep immediately.

## Dependencies

| Dependency | Why |
| --- | --- |
| `QtCore` | The only Qt module. There is no QtGui, QtQml or QtNetwork, so a server in a command-line program adds nothing more. The JSON Schema checker is part of the library, so there is nothing else to build, install or license. |
| Linux | For `pipe2`, `sigtimedwait`, `MSG_NOSIGNAL` and `/proc/self/fd`. |

### Old build directories

A build directory from when the library used valijson or jsoncons keeps their entries in its cache. A configure
removes `PHOSPHORMCP_USE_SYSTEM_VALIJSON`, `PHOSPHORMCP_VALIJSON_INCLUDE_DIR`, `VALIJSON_INCLUDE_DIR` and
`PHOSPHORMCP_USE_SYSTEM_JSONCONS`. It gives a warning when they selected something (a switch on, a directory set). It
removes these entries without a warning:

- the internal entries that kept the checks of their headers
- the flags that a development build of phosphor-fsloader set to warn one time about the first two

These entries stay:

- `JSONCONS_INCLUDE_DIR`, because it names the headers of phosphor-fsloader.
- The entries that `cmake/PhosphorDependencies.cmake` set when it fetched valijson: `PHOSPHOR_DEPS_ARCHIVES`, and the
  entries of CPM and FetchContent (`CPM_PACKAGE_valijson_*` among them, which nothing reads now). The other
  libraries of the tree fetch through that file.

In a build directory of the library alone, nothing reads them.

## See also

- [`phosphor-fsloader`](../phosphor-fsloader/README.md): it also checks JSON files against schemas, with jsoncons,
  for data files that the application ships or that a user writes. This library checks arguments that a model chose.
  Thus it keeps its own checker, with a check before it that refuses what a handler would read incorrectly, an exact
  bound on the work, and a short list of errors in plain words.
- [`phosphor-ipc`](../phosphor-ipc/README.md): JSON over a Unix socket, for scripts and shells that control a running
  Phosphor application. This library serves an agent that starts its server as a child.
- [`phosphor-mcp-ipc`](../phosphor-mcp-ipc/README.md): a server made with this library. It offers the calls of a
  running application's phosphor-ipc socket to an agent.
- [`phosphor-channel`](../phosphor-channel/README.md): its `SigpipeBlock` is the original of the copy in the
  transport, and the two are kept the same.
- `examples/phosphor-mcp/wordcount.cpp` and `tests/test_phosphor_mcp_example.cpp`: a full server, and a client that
  runs it.
