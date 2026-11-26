<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team -->
<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->

# phosphor-ipc

phosphor-ipc is a typed JSON channel over a Unix socket for Phosphor applications such as phosphor-shell. With it,
compositor keybinds, external scripts and other shells start Phosphor actions by verb
(`phosphorctl call launcher.toggle`). The existing D-Bus interface does not change.

## Responsibility

Modern Wayland shells (DMS, Noctalia, Quickshell) ship a typed CLI built on a single socket. Their requests are JSON,
and a schema describes their targets. `phosphor-ipc` is the Phosphor equivalent. It works **alongside** the existing
D-Bus adaptors. Service libraries register an IPC target for each callable, and also a D-Bus method where one is
necessary.

The library owns these parts:

- **`IpcRouter`** is the central dispatcher. A `QLocalServer` listens on `$XDG_RUNTIME_DIR/phosphor.sock`. The router
  registers QObject "targets" by name. It walks their `QMetaObject` to make JSON schemas. It dispatches calls through
  `QMetaMethod::invoke`, and answers when the method returns or later through a deferred reply. It routes
  signal-broadcast events to the subscribers of each target, and it sends events to one client connection.
- **`IpcTarget`** is a QML element. Plugin authors declare one for each verb namespace that they expose, and put
  `Q_INVOKABLE`-style functions directly on it. `IpcTarget.emitEvent("name", [args])` sends a JSON event to every
  subscriber on (target, name).
- **`IpcSchemaGenerator`** converts a `QMetaObject` to a JSON Schema. Every documented QMetaType maps to a JSON Schema
  fragment. An unknown type becomes `{"description": "<QMetaType::name>"}` without a `type` constraint.
- **`IpcClient`** is an asynchronous client of the wire format. A C++ program uses it to drive a running application.
  See [Client](#client).
- **`IpcEngine::install`** is a small bridge. It stores the application's `IpcRouter` on the `QQmlEngine`, so that
  `IpcTarget` instances find it in `componentComplete`.

The **`phosphorctl`** CLI at `phosphor-shell/cli/phosphorctl/` is the client. It has the subcommands `call`, `list`,
`schema` and `subscribe`.

## Key types

| Type | Purpose |
|------|---------|
| `PhosphorIpc::IpcRouter` | The dispatcher of one application. It owns one `QLocalServer`, the registry of named targets, and the subscription set of each socket. It is single-threaded and runs on the thread that owns it, normally the GUI thread. |
| `PhosphorIpc::IpcTarget` | QML element. Each instance binds to one target name. The functions declared on the instance become callable. The explicit `emitEvent(name, args)` sends wire events. |
| `PhosphorIpc::IpcProtocol` | The wire-format constants, the parser and the serializer. The library (server) and the `phosphorctl` binary (client) share it. |
| `PhosphorIpc::IpcEngine` | Bridge between an `IpcRouter` that the application owns and the `IpcTarget` instances in QML. |
| `PhosphorIpc::IpcSchemaGenerator` | Converts a `QMetaObject` to a JSON Schema. The schema response uses it, and the CLI uses it to validate arguments on the client side. |
| `PhosphorIpc::IpcCallContext` | Mixin for a C++ target. `sendErrorReply(message)` fails the running wire call with `CALL_FAILED`, as `QDBusContext` does for D-Bus. `deferReply()` gives the answer to an `IpcPendingReply` for later. `connectionId()` names the client connection that made the call, and `connectionPid()` names the process that opened it. |
| `PhosphorIpc::IpcClient` | Asynchronous client over one `QLocalSocket`: calls, the target list, schemas and subscriptions. It never blocks the event loop. |
| `PhosphorIpc::IpcPendingReply` | Move-only handle to a deferred call. `finish(result)` or `fail(...)` answers it one time. If the handle is dropped without an answer, the call fails with `CALL_ABANDONED`. |

## Use

### C++ shell composition root

```cpp
#include <PhosphorIpc/IpcEngine.h>
#include <PhosphorIpc/IpcRouter.h>

#include <QGuiApplication>
#include <QQmlApplicationEngine>

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    // Declare `router` BEFORE `engine` so reverse-order destruction
    // tears the engine down FIRST; otherwise QML-side IpcTarget
    // instances would unregister against a dead router.
    PhosphorIpc::IpcRouter router;
    if (!router.start()) {  // binds $XDG_RUNTIME_DIR/phosphor.sock
        qWarning("IPC router failed to start "
                 "(XDG_RUNTIME_DIR unset, another live listener on "
                 "the same path, or insufficient permissions)");
        // Application can continue without IPC; failure is non-fatal.
        // The router auto-recovers from STALE socket files (left
        // behind by a crashed previous run) — those don't cause
        // start() failures.
    }

    QQmlApplicationEngine engine;
    PhosphorIpc::IpcEngine::install(&engine, &router);
    engine.loadFromModule(...);  // shell QML
    return app.exec();
}
```

The shell QML can then declare `IpcTarget` instances anywhere in the component tree. Each one registers itself
through the router that is stored on the engine.

### QML side

```qml
import Phosphor.Ipc
import Phosphor.Theme  // for Theme singleton; omit if not needed

IpcTarget {
    target: "theme"

    signal modeChanged(string mode)  // schema-visible

    function setMode(mode: string) {
        Theme.mode = mode           // apply state mutation
        modeChanged(mode)           // notify in-QML observers
        emitEvent("modeChanged", [mode])  // broadcast on the WIRE
    }
}
```

The schema generator shows both `Q_INVOKABLE` methods and declared signals. Thus `phosphorctl schema theme` shows
the signals that a client can subscribe to.

**A call to the QML signal (for example `modeChanged(mode)`) does NOT broadcast to wire subscribers**. It only starts
the connections in QML. Only `emitEvent(name, args)` delivers on the wire. This split is by design. Introspection of
Qt signals through `qt_metacall` collides with the dispatch that moc generates. Also, the explicit `emitEvent` follows
the model that "wire-visible state transitions are an explicit contract". For delivery in QML AND on the wire, plugin
authors must call both.

### Failing a call

If a method cannot do what the client asked, it fails the call. The client then gets an error frame instead of a
reply:

```jsonc
{"type":"error","id":42,"code":"CALL_FAILED","message":"no pane 7","detail":{}}
```

A C++ target inherits `IpcCallContext` next to `QObject`, and calls `sendErrorReply` while its method runs:

```cpp
class Control : public QObject, protected PhosphorIpc::IpcCallContext
{
    Q_OBJECT
public:
    Q_INVOKABLE int focus(int pane)
    {
        if (!exists(pane)) {
            sendErrorReply(QStringLiteral("no pane %1").arg(pane));
            return 0; // dropped
        }
        ...
    }
};
```

A QML function calls `sendErrorReply` on its `IpcTarget`:

```qml
IpcTarget {
    target: "theme"
    function setMode(mode: string) {
        if (mode !== "light" && mode !== "dark") {
            sendErrorReply("mode is light or dark")
            return
        }
        Theme.mode = mode
    }
}
```

The router drops the return value of a failed call. Outside a wire call (a direct C++ call, a QML binding),
`calledFromIpc()` is false and `sendErrorReply` does nothing.

A nested event loop inside the method (`QEventLoop::exec`, `QDialog::exec`) can run other slots of the target. These
slots also see no call. There, `sendErrorReply` and `deferReply` do nothing and write a warning.
`QCoreApplication::processEvents` does not start a loop of its own, and the slots that it runs see the method's call.

In both cases, a method that waits for something defers its reply. It does not wait in a loop. A nested loop also
holds back the later requests of the method's own connection until the method returns. Thus a method must never wait
there for another request from the same client.

`IpcRouter::invoke` reports the failure as `InvokeOutcome::CallFailed` with the target's message.

### Answering later

A call that takes time (it runs a process, or waits for the user) does not have to block the router. The method
defers its reply and returns immediately. The target answers later through the `IpcPendingReply` that it keeps:

```cpp
class Packages : public QObject, protected PhosphorIpc::IpcCallContext
{
    Q_OBJECT
public:
    explicit Packages(PhosphorIpc::IpcRouter& router);

    Q_INVOKABLE void install(const QString& name)
    {
        Job& job = newJob();
        job.reply = deferReply();          // no reply goes out on return
        startJob(job, name);               // may end at once, and answer
    }

Q_SIGNALS:
    void output(const QString& line);      // listed by the schema

private:
    void onOutput(Job& job, const QString& line)
    {
        // To the caller only, ahead of the reply.
        m_router.sendEvent(job.reply.connectionId(), QStringLiteral("pkg"), QStringLiteral("output"), {line});
    }
    void onFinished(Job& job, int exitCode)
    {
        if (exitCode == 0) {
            job.reply.finish(QJsonValue::Null);
        } else {
            job.reply.fail(QStringLiteral("exit code %1").arg(exitCode));
        }
    }
    ...
};
```

Defer the reply before the work starts, so that work that ends immediately can answer. Keep one pending reply for
each call. If every call assigns to a single member, the next call abandons the call that the member held. The router
sends no reply when a deferring method returns, and it drops the return value. Meanwhile, the router continues to
answer other calls on the same connection.

| Rule | Behavior |
| --- | --- |
| One answer | `finish(result)` sends the reply. `fail(message)` sends a `CALL_FAILED` error, and `fail(code, message)` sends an error with the target's code. The first answer stands. Later answers do nothing and return false. If a method calls `sendErrorReply` after `deferReply`, the call gets that error when the method returns, unless the pending reply answered first. |
| Never left waiting | If an `IpcPendingReply` is destroyed (or overwritten by a move) without an answer, the call fails with `CALL_ABANDONED`. |
| Closed connection | If the connection of the call has closed, `finish` and `fail` drop the answer and return false, with no warning. The same occurs for a pending reply that outlives the router. True means that the router queued the answer on an open connection. A client that went away before the router noticed loses it, and `connectionClosed` follows. |
| Move-only, router thread | The handle moves and cannot be copied. Answer it and destroy it on the router's thread. Debug builds assert this. In release builds, `finish` and `fail` write a warning, return false and keep the call. The handle can then still answer it on the router's thread. |
| Handles that hold no call | `deferReply()` returns a handle whose `isValid()` is false in these cases: outside a wire call, a second time in one call (with a warning), on a direct `IpcRouter::invoke`, and when the connection already has `IpcRouter::MaxPendingCallsPerConnection` deferred calls waiting. On `invoke`, which has no client to answer later, `invoke` reports `InvokeOutcome::Deferred`. Past the cap, the call fails with `CALL_FAILED` when the method returns. |
| Call ids | A call sent without an id has id 0. `finish` answers it with `"id":0`. `fail` answers it with an error that has no id, as for all requests without an id. |

QML functions have no `deferReply`. A deferred call needs a C++ target.

### Connections and events to the caller

The router gives an id to each client connection that it accepts. The id is unique in the process and is never used
again, also across routers. While a call runs, `IpcCallContext::connectionId()` returns the id of the connection
that made it. It returns 0 for a call that came from no connection (a direct `IpcRouter::invoke`), and outside a call.

`IpcCallContext::connectionPid()` returns the process ID of the client that made the call. The router reads it from
the kernel (`SO_PEERCRED`) one time, when it accepts the connection. Thus it names the process that connected, and a
client cannot claim a different process. It is 0 where `connectionId()` is 0, on a platform other than Linux, and when
the kernel gives no answer.

`IpcRouter::sendEvent(connectionId, target, signal, args)` writes one event to that connection, whether or not the
connection subscribed to something. Use it to send the output of a call to the client that made the call. The frame
names the event with `target` and `signal`, and has no `subscriptionId`.

Declare the event as a signal on the target, as for broadcast events, so that `phosphorctl schema` lists it. The
schema then also offers the signal for subscription. A subscriber gets only what `broadcastEvent` sends under that
name. Thus a target that wants both kinds of listener calls both.

The id belongs to the router that runs the call. A target registered on more than one router sends through the router
that accepted the connection.

`sendEvent` returns true when it queued the event on an open connection. It returns false, and writes nothing, in
these cases:

- The target name or the signal name is empty.
- The id names no open connection of the router.
- The call is not on the router's thread. Debug builds assert, and release builds write a warning. `broadcastEvent`
  drops its event in the same way.
- The connection is more than 16 MiB behind. The output limit of broadcasts applies: the router closes the connection
  with a warning.

A fifth argument, `qint64* lineBytes`, receives the size of the line that the event used, newline included. A target
that counts the bytes that its client has read uses it. It is 0 when `sendEvent` returns false.

`IpcRouter::connectionClosed(id)` comes one time after each connection closes, for these causes: the client went away,
the router closed it, or `stop()` ran. The router sends it from the event loop, never synchronously from inside a call
into the router such as `sendEvent` or `stop`. Thus a slot can stop the work of that connection and destroy its
pending replies immediately. A target method that runs a nested event loop can get the signal inside that loop, as it
can get any queued signal.

The router's destructor sends `connectionClosed` for the connections that it closes. By then, a receiver that holds
the router as a member is partly destroyed. Thus the receiver declares the router after the state that its slot
uses, or disconnects first.

A client that gets an event that it did not subscribe to can ignore it. `phosphorctl call` prints the events sent to
its call on stderr. `phosphorctl subscribe` prints every event that it receives.

`IpcRouter::connectionOpened(id)` comes when the router accepts a connection, before it reads from it.
`connectionCount()` returns the number of connections that are open now. `setMaxConnections(max)` sets a limit on
them, for a socket that other processes can reach, such as a socket forwarded to another host. If `max` connections
are open, the router closes a new connection immediately and does not read it. That connection gets no id,
`connectionOpened` and `connectionClosed` do not come for it, and `connectionRefused()` comes. A lower limit closes no
open connection. The default, 0, sets no limit.

`IpcRouter::hasSubscribers(target, signal)` tells whether a connection subscribes to the pair now. Thus it tells
whether `broadcastEvent` has a connection to write to. If a target's events cost work to make (it merges them, or runs
a timer for them), the target asks first, and skips the work while there are no subscribers. A subscription counts
from the subscribe that the router accepted. It stops at its unsubscribe, the close of its connection, the end of the
target's subscriptions, or `stop()`. The router keeps a count for each pair, so the call is one hash lookup.

`IpcRouter::subscribersChanged(target, signal)` comes when that answer changes: the first subscriber to the pair
came, or the last one went. The router sends it from the event loop, as it does `connectionClosed`. It sends it only
when the answer is different from the answer of the previous signal for the pair. Thus a subscriber that comes and
goes before the event loop runs causes no signal. The router's destructor sends none.

`stop()` closes each connection immediately and drops the data that it did not write yet. Answers written just before
the call never arrive. `stop(IpcRouter::StopMode::Flush)` delivers them first. The router stops listening and forgets
its connections immediately, as `stop()` does. Each connection closes when the client has read what the router wrote
to it. A client that does not read within the timeout (`DefaultFlushTimeoutMs`, 5 seconds, or the second argument)
loses the rest. The delivery does not need the router, so you can destroy the router immediately after the call.

### CLI side

```
phosphorctl call theme.setMode --arg mode=dark
phosphorctl call pkg.install --arg name=foo --timeout 0   # waits for a deferred answer
phosphorctl list
phosphorctl schema theme | jq
phosphorctl subscribe theme.modeChanged    # stays connected, streams events
```

The CLI finds the socket path in this order: `--socket PATH`, then `$PHOSPHOR_SOCKET`, then
`$XDG_RUNTIME_DIR/phosphor.sock`.

By default, `phosphorctl call` waits two seconds for the answer. `--timeout SECONDS` changes that time.
`--timeout 0` waits without a limit, for a deferred call that answers when its work ends. Events that the target sends
to the call go to stderr when they arrive, one compact JSON line each. Thus stdout has only the result.

### Acceptance harness

`phosphor-shell-libs/examples/phosphor-ipc-demo/` is the canonical end-to-end test. The demo window has a live event
log panel that shows the broadcast stream. Run `phosphorctl call count.increment` from one other terminal, and the
events show in the demo window in real time. You do not need a separate `phosphorctl subscribe` terminal:

```
# terminal 1
./bin/phosphor-ipc-demo --socket /tmp/p.sock
# terminal 2
export PHOSPHOR_SOCKET=/tmp/p.sock
phosphorctl call count.increment        # demo's event log gains an entry
phosphorctl subscribe count.countChanged   # optional: same events on stdout
```

## Wire protocol

Each message is one JSON object on one line, UTF-8, terminated by `\n`. This is the same NDJSON shape as niri-ipc and
hyprland-ipc. The protocol is stable across protocol version 1.

The version is a build-time invariant only. A `static_assert` in `IpcProtocol.h` checks the
`PHOSPHOR_IPC_PROTOCOL_VERSION` compile definition. The version is NOT sent on the wire, and there is no handshake to
agree on a version. An incompatible change shows as malformed-request errors with an older peer.

### Requests from the client to the server

```jsonc
// Call
{"type":"call","id":42,"target":"greet","fn":"sayHello","args":["nate"]}
// Enumerate targets
{"type":"list","id":43}
// Schema for one target
{"type":"schema","id":44,"target":"greet"}
// Subscribe to a signal. Server streams events tagged with this id
// until unsubscribe / disconnect
{"type":"subscribe","id":45,"target":"count","signal":"countChanged"}
// Cancel a subscription
{"type":"unsubscribe","id":46,"subscriptionId":45}
```

### Responses from the server to the client

```jsonc
{"type":"reply","id":42,"result":"Hello, nate"}
// Event to a subscription
{"type":"event","subscriptionId":45,"args":[7]}
// Event to one connection (IpcRouter::sendEvent), with no subscription
{"type":"event","target":"pkg","signal":"output","args":["fetching"]}
{"type":"error","id":42,"code":"NO_SUCH_TARGET","message":"...","detail":{}}
```

### Error codes

The router sends these codes: `NO_SUCH_TARGET`, `NO_SUCH_FN`, `NO_SUCH_SIGNAL`, `NO_SUCH_SUBSCRIPTION`,
`INVALID_ARG`, `INVOCATION_FAILED`, `MALFORMED_REQUEST`, `CALL_FAILED`, `CALL_ABANDONED`. A deferred reply can also
fail with a code that the target defines.

| Code | Meaning |
| --- | --- |
| `INVOCATION_FAILED` | The router could not run the method. |
| `CALL_FAILED` | The method ran and failed the call itself (see [Failing a call](#failing-a-call)). |
| `CALL_ABANDONED` | The target deferred the reply and then dropped the call without an answer (see [Answering later](#answering-later)). |

### Request ids and order

Each reply, and each error that belongs to a request, has the request's `id`. The client matches them by this id.
The router answers most calls in request order. But it answers a deferred call when the target finishes it, and that
can be after the replies to later calls on the same connection.

A client that keeps more than one request in flight gives each request its own id. That id must also be different from
the id of each live subscription on the connection, because the error that ends a subscription has the subscription
id. All data that the router writes to one connection arrives in the order that the router wrote it.

## Client

`IpcClient` speaks the wire format from the other end, for a C++
program that drives a running application, such as an MCP server that
offers the application's calls to an agent. It never blocks the event
loop.

```cpp
#include <PhosphorIpc/IpcClient.h>

PhosphorIpc::IpcClient client;
QObject::connect(&client, &PhosphorIpc::IpcClient::closed, [](const QString& reason) {
    qWarning() << "IPC connection ended:" << reason;
});
client.connectToServer(path);
client.call(QStringLiteral("theme"), QStringLiteral("setMode"), {QStringLiteral("dark")},
            [](const PhosphorIpc::IpcReply& reply) {
                if (reply.isError()) {
                    qWarning() << reply.errorCode << reply.errorMessage;
                    return;
                }
                qDebug() << reply.result;
            });
```

The rules:

- **Ids.** Each request gets an id of its own, and the client matches the
  answer to it. Any number of requests may be in flight, and they may be
  answered in any order (a deferred call answers after later ones). Ids
  only grow, so a request never takes the id of a live subscription.
- **Callbacks.** Each callback runs once, from the event loop, never
  from inside the call that made the request. `IpcReply` holds the
  result, or an error with its code, message and detail. The code is
  the server's (`ErrorCode`, or a target's own code for a deferred call)
  or one of the client's own (`ClientErrorCode`), which never travel on
  the wire:

  | Code | When |
  |------|------|
  | `NOT_CONNECTED` | The request was made without a connection. |
  | `CONNECTION_CLOSED` | The connection ended before the answer came. |
  | `WRITE_QUEUE_FULL` | The request would pass the write cap (below). |
  | `MALFORMED_REPLY` | The server sent an error without a code. |

- **Connecting.** Requests made while the client connects wait, and go
  out once it is connected. A connection that fails at once (no socket
  at the path) leaves the client unconnected when `connectToServer`
  returns. `connected()` and `closed(reason)` come from the event loop.
- **The end.** When the connection ends (the server went away, a read
  failed, `close()` ran), every request still waiting fails with
  `CONNECTION_CLOSED`, and then `closed(reason)` comes, once. The live
  subscriptions end with it. `connectToServer` may then start another
  connection.
- **Events.** A subscription's events come as
  `eventReceived(subscriptionId, target, signal, args)` from the answer of
  `subscribe` until `unsubscribe`, after which none comes, not even one
  already on its way. An error that the server sends under a
  subscription's id (`NO_SUCH_TARGET` when the target went away) ends it
  with `subscriptionEnded`. Events that a target sends to the connection
  (`IpcRouter::sendEvent`) come with subscription id 0.
- **Bounded memory.** The bytes that the server has not read yet,
  requests that wait for the connection included, are capped at
  `MaxPendingWriteBytes` (16 MiB, the router's own cap on a connection
  that is behind). A request past it fails with `WRITE_QUEUE_FULL`, so a
  server that stops reading cannot grow the client without bound. A line
  from the server longer than `MaxLineBytes` (64 MiB) ends the
  connection.
- **Lifetime.** Destroying the client drops the callbacks that wait,
  unrun, since they may hold objects that are gone by then. Never destroy
  it from inside one of its callbacks or signals. `deleteLater()` is fine
  there. Single-threaded, like the router.

The client has no timeout. A deferred call may take as long as its work,
so the caller decides how long to wait, and `close()` ends the wait.

## Arbitration and lifecycle

- **Duplicate target ids.** If the SAME `QObject` registers again under the SAME name, the call succeeds silently and
  changes nothing (no warning, no signal emitted again). If a DIFFERENT `QObject` registers under an existing name,
  the router rejects it with a `qWarning` and keeps the first registration. `registerTarget` returns `bool`. Thus
  wrappers such as `IpcTarget` can detect the rejection and skip the paired unregister at teardown.
- **Signal names validated at subscribe.** If the target's metaobject does not declare the signal, the subscribe
  fails with `NO_SUCH_SIGNAL`. A typo shows immediately, and later `emitEvent` calls are not silently ignored.
- **Subscriptions belong to one socket.** A subscription id is unique in its connection, not globally. The wire
  identity is (socket, `subscriptionId`). If a live subscription on the connection uses an id, a subscribe with that
  id fails with `MALFORMED_REQUEST`.
- **A target's subscriptions end with it.** `unregisterTarget` ends them. The destruction of a target object that was
  never unregistered also ends them. Each subscriber gets `NO_SUCH_TARGET` under its subscription id. A later target
  with the same name does not send events to it.
- **Not destroyed from its own stack.** Never destroy the router from inside a call that it runs or a signal that it
  emits (a target method, a `connectionClosed` slot). `stop()` is safe there. `deleteLater()` is also safe, except
  inside a nested event loop that a target method runs. That loop runs the deletion while the method's call is still
  on the stack.
- **Disconnect removes subscriptions.** When a subscriber socket disconnects, the router drops all its subscriptions
  and the deferred calls of the connection. `connectionClosed(id)` follows.
- **Stale socket files.** On `start`, if a file is at the socket path, the router tries a quick connect. If the
  connect is refused, the router unlinks the stale path and takes it. The router does not change a live listener
  (busy or not) or a file that is not a socket. In that case, `start` fails cleanly.
- **Socket mode without the umask.** The socket file has mode 0600. The router binds it in a new 0700 directory next
  to the path, sets the mode there and links it into place. Thus it never changes the umask, which all threads of the
  process share. `stop()` removes the socket file, unless a different file has taken the path since.
- **No automatic broadcast from Qt signals.** Plugin authors call `IpcTarget.emitEvent("name", args)` explicitly. A
  QML signal (for example `modeChanged(...)`) only tells subscribers in QML, NOT wire subscribers. This split is by
  design. Introspection of all Qt signals through `qt_metacall` collides with the code that moc generates for
  Q_OBJECT. Also, the explicit `emitEvent` follows the model that "wire-visible state transitions are an explicit
  contract".

## Dependencies

- `QtCore` (always)
- `QtGui` (transitive, only through the static QML plugin `PhosphorIpcQml`. The runtime library does not use QtGui.)
- `QtQml` (for the QML registration of `IpcTarget`)
- `QtNetwork` (for `QLocalServer` / `QLocalSocket`)

## See also

- `phosphor-shell/cli/phosphorctl/`: the typed CLI that is the client of the library.
- `phosphor-shell-libs/examples/phosphor-ipc-demo/`: the acceptance harness. Three `IpcTarget` instances
  (greet / count / set-value) use all of the wire interface: call, list, schema and subscribe.
