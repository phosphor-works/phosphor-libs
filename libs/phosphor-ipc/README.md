<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team
     SPDX-License-Identifier: LGPL-2.1-or-later -->

# phosphor-ipc

> Typed JSON-over-Unix-socket invocation channel for Phosphor
> applications such as phosphor-shell. Lets compositor keybinds, external scripts, and
> other shells drive Phosphor actions by verb (`phosphorctl call
> launcher.toggle`) without rebuilding the existing D-Bus surface.

## Responsibility

Modern Wayland shells (DMS, Noctalia, Quickshell) all ship a typed
CLI built on a single socket, with JSON-shaped requests and
schema-described targets. `phosphor-ipc` is Phosphor's equivalent. It
sits **alongside** the existing D-Bus adaptors. Service libraries register both an IPC
target and (where appropriate) a D-Bus method for each callable.

The library owns:

- **`IpcRouter`**: central dispatcher backed by a `QLocalServer`
  listening on `$XDG_RUNTIME_DIR/phosphor.sock`. Registers QObject
  "targets" by name, walks their `QMetaObject` to generate JSON
  schemas, dispatches calls via `QMetaMethod::invoke` (answered when
  the method returns, or later through a deferred reply), routes
  signal-broadcast events to per-target subscribers, and sends events
  to one client connection.
- **`IpcTarget`**: QML element. Plugin authors declare one per
  exposed verb namespace and put `Q_INVOKABLE`-style functions on it
  directly. `IpcTarget.emitEvent("name", [args])` pushes a JSON
  event to every subscriber on (target, name).
- **`IpcSchemaGenerator`**: `QMetaObject` → JSON Schema. Every
  documented QMetaType maps to a JSON Schema fragment, and unknown types
  degrade to `{"description": "<QMetaType::name>"}` without a `type`
  constraint.
- **`IpcClient`**: the client side of the wire format, for programs
  that drive an application's socket from C++ (see [Client](#client)).
- **`IpcEngine::install`**: small bridge that stashes the
  application's `IpcRouter` on the `QQmlEngine` so `IpcTarget`
  instances find it in `componentComplete`.

Ships alongside the **`phosphorctl`** CLI at `phosphor-shell/cli/phosphorctl/`,
which provides `call`, `list`, `schema`, and `subscribe`
subcommands.

## Key types

| Type | Purpose |
|------|---------|
| `PhosphorIpc::IpcRouter` | Per-application dispatcher. Owns one `QLocalServer`, the registry of named targets, and the per-socket subscription set. Single-threaded: the thread that owns it, normally the GUI thread. |
| `PhosphorIpc::IpcTarget` | QML element. Each instance binds to one target name. Functions declared on the instance become callable, and the explicit `emitEvent(name, args)` pushes wire events. |
| `PhosphorIpc::IpcProtocol` | Wire-format constants + parser/serialiser. Shared between the library (server) and the `phosphorctl` binary (client). |
| `PhosphorIpc::IpcEngine` | Bridge between an application-owned `IpcRouter` and QML-side `IpcTarget` instances. |
| `PhosphorIpc::IpcSchemaGenerator` | `QMetaObject` → JSON Schema. Used by the schema response and by the CLI for client-side argument validation. |
| `PhosphorIpc::IpcCallContext` | Mixin for a C++ target. `sendErrorReply(message)` fails the running wire call with `CALL_FAILED`, like `QDBusContext` does for D-Bus. `deferReply()` defers the answer to an `IpcPendingReply`, `connectionId()` names the client connection that made the call, and `connectionPid()` the process that opened it. |
| `PhosphorIpc::IpcClient` | A client of one router's socket over `QLocalSocket`. `call`, `list`, `schema`, `subscribe` and `unsubscribe` each take a callback that gets the answer as an `IpcReply`, from the event loop. Signals `connected`, `closed(reason)`, `eventReceived` and `subscriptionEnded`. |
| `PhosphorIpc::IpcPendingReply` | Move-only handle to a deferred call. `finish(result)` or `fail(...)` answers it once, and dropping it unanswered fails it with `CALL_ABANDONED`. |

## Wire protocol

One JSON object per line, `\n`-terminated, UTF-8. Same NDJSON shape
as niri-ipc and hyprland-ipc. Stable across protocol version 1.
The version is a build-time invariant only (the
`PHOSPHOR_IPC_PROTOCOL_VERSION` compile definition is cross-checked
by a `static_assert` in `IpcProtocol.h`). It is NOT transmitted on
the wire and there is currently no version-negotiation handshake.
Incompatible bumps surface as malformed-request errors against an
older peer.

### Requests (client → server)

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

### Responses (server → client)

```jsonc
{"type":"reply","id":42,"result":"Hello, nate"}
// Event to a subscription
{"type":"event","subscriptionId":45,"args":[7]}
// Event to one connection (IpcRouter::sendEvent), with no subscription
{"type":"event","target":"pkg","signal":"output","args":["fetching"]}
{"type":"error","id":42,"code":"NO_SUCH_TARGET","message":"...","detail":{}}
```

Error codes: `NO_SUCH_TARGET`, `NO_SUCH_FN`, `NO_SUCH_SIGNAL`,
`NO_SUCH_SUBSCRIPTION`, `INVALID_ARG`, `INVOCATION_FAILED`,
`MALFORMED_REQUEST`, `CALL_FAILED`, `CALL_ABANDONED`. A deferred reply
may also fail with a code of the target's own.

`INVOCATION_FAILED` means that the router could not run the method.
`CALL_FAILED` means that the method ran and failed the call itself (see
[Failing a call](#failing-a-call)). `CALL_ABANDONED` means that the
target deferred the reply and then dropped the call without answering
it (see [Answering later](#answering-later)).

Every reply and request-correlated error carries the request's `id`, and
the client matches them by it. Most calls are answered in request order,
but a deferred call is answered when the target finishes it, which may be
after the replies to calls made later on the same connection. A client
that keeps more than one request in flight gives each its own id, one that
no live subscription on the connection uses either: the error that ends a
subscription carries the subscription id. Everything the router writes to
one connection arrives in the order it was written.

## Typical use

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

The shell QML can now declare `IpcTarget` instances anywhere in the
component tree, and each one auto-registers via the router stashed on
the engine.

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

The schema generator surfaces both `Q_INVOKABLE` methods and
declared signals, so `phosphorctl schema theme` shows the
subscribable surface.

**Calling the QML signal (e.g. `modeChanged(mode)`) does NOT
broadcast to wire subscribers**. It only fires in-QML connections.
Wire delivery happens through `emitEvent(name, args)` exclusively.
The split is by design. Introspecting Qt signals via `qt_metacall`
collides with moc-generated dispatch, and explicit `emitEvent`
matches the "wire-visible state transitions are an explicit
contract" model. Plugin authors must call both for in-QML AND wire
delivery.

### Failing a call

A method that cannot do what the client asked fails the call, so that the
client gets an error frame instead of a reply:

```jsonc
{"type":"error","id":42,"code":"CALL_FAILED","message":"no pane 7","detail":{}}
```

A C++ target inherits `IpcCallContext` next to `QObject` and calls
`sendErrorReply` while its method runs:

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

The return value of a failed call is dropped. Outside a wire call (a
direct C++ call, a QML binding) `calledFromIpc()` is false and
`sendErrorReply` does nothing. The same goes for the target's other slots
that a nested event loop inside the method runs (`QEventLoop::exec`,
`QDialog::exec`), where `sendErrorReply` and `deferReply` also warn that
they did nothing. `QCoreApplication::processEvents` does not start a loop
of its own, and the slots it runs see the method's call. Either way a
method that waits for something defers its reply instead of waiting in a
loop. A nested loop also holds back the later requests of the method's own
connection until the method returns, so a method must never wait there for
another request from the same client. `IpcRouter::invoke` reports the
failure as `InvokeOutcome::CallFailed` with the target's message.

### Answering later

A call that takes a while (it runs a process, waits for the user) need not
block the router. The method defers its reply and returns at once, and
the target answers through the `IpcPendingReply` it keeps:

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

Defer before the work starts, so that work that ends at once can answer,
and keep one pending reply per call: a single member that every call
assigns to abandons the call it held when the next one comes. The router
sends no reply when a deferring method returns, and drops its return
value. Calls on the same connection go on being answered meanwhile. The
rules:

- **One answer.** `finish(result)` sends the reply, `fail(message)` a
  `CALL_FAILED` error and `fail(code, message)` an error with the
  target's code. The first answer stands, and later ones do nothing and
  return false. A method that calls `sendErrorReply` after `deferReply`
  is answered with that error when it returns, unless the pending reply
  answered first.
- **Never left waiting.** An `IpcPendingReply` destroyed (or overwritten
  by a move) unanswered fails the call with `CALL_ABANDONED`.
- **Closed connection.** `finish` and `fail` on a call whose connection
  has closed drop the answer and return false, with no warning. So does
  a pending reply that outlives the router. True means the answer was
  queued on an open connection: a client that went away before the
  router noticed loses it, and `connectionClosed` follows.
- **Move-only, router thread.** The handle moves and cannot be copied.
  Answer it, and destroy it, on the router's thread. Debug builds
  assert this. Release builds warn, and `finish` and `fail` return false
  and keep the call, which the handle can still answer on the router's
  thread.
- **Handles that hold no call.** `deferReply()` returns a handle whose
  `isValid()` is false outside a wire call, a second time in one call
  (with a warning), on a direct `IpcRouter::invoke`, and when the
  connection already has `IpcRouter::MaxPendingCallsPerConnection`
  deferred calls waiting. On `invoke`, which
  has no client to answer later, `invoke` reports
  `InvokeOutcome::Deferred`. Past the cap, the call fails with
  `CALL_FAILED` when the method returns.
- **Call ids.** A call sent without an id has id 0. `finish` answers it
  with `"id":0`, and `fail` with an error that carries no id, as for any
  request without one.

QML functions have no `deferReply`. A deferred call needs a C++ target.

### Connections and events to the caller

The router gives every client connection it accepts an id, unique in the
process and never reused, also across routers. While a call runs,
`IpcCallContext::connectionId()` returns the id of the connection that
made it, and 0 for a call that came from no connection (a direct
`IpcRouter::invoke`) or outside a call.

`IpcCallContext::connectionPid()` returns the process ID of the client that
made the call. The router reads it from the kernel (`SO_PEERCRED`) once,
when it accepts the connection, so it names the process that connected and
a client cannot claim another. It is 0 where `connectionId()` is 0, and on
a platform other than Linux or when the kernel gives no answer.

`IpcRouter::sendEvent(connectionId, target, signal, args)` writes one
event to that connection, whether it subscribed to anything or not. It is
meant for a call's own output, sent to the client that made the call. The
frame names the event with `target` and `signal` and carries no
`subscriptionId`. Declare the event as a signal on the target, as for
broadcast events, so `phosphorctl schema` lists it. The schema then also
offers the signal for subscription, and a subscriber gets only what
`broadcastEvent` sends under that name: a target that wants both kinds of
listener calls both. The id belongs to the router that runs the call, so a
target registered on several routers sends through the one that accepted
the connection. `sendEvent` returns true when the event was queued on an
open connection. It returns false, and writes nothing, for an empty target
or signal name, for an id that names no open connection of the router, and
off the router's thread (an assert in debug builds, a warning in release
builds; `broadcastEvent` drops its event the same way). The output limit of broadcasts
applies: a connection more than 16 MiB behind is closed with a warning,
and `sendEvent` returns false. A fifth argument, `qint64* lineBytes`,
receives the size of the line that the event took, its newline included,
for a target that counts the bytes its client has read. It is 0 when
`sendEvent` returns false.

`IpcRouter::connectionClosed(id)` follows the close of every connection,
once: the client went away, the router closed it, or `stop()` ran. It is
sent from the event loop, never synchronously from inside a call into the
router such as `sendEvent` or `stop`, so a slot can stop that
connection's work and destroy its pending replies on the spot. A target
method that spins a nested event loop can get it inside that loop, as it
can any queued signal. The router's destructor sends it for the
connections it closes. A receiver that holds the router as a member is
partly destroyed by then, so it declares the router after the state its
slot uses, or disconnects first. A client that gets an event it did not
subscribe to is free to skip it. `phosphorctl call` prints the events sent
to its call on stderr, and `phosphorctl subscribe` prints every event it
receives.

`IpcRouter::connectionOpened(id)` comes when the router accepts a
connection, before it reads anything from it, and `connectionCount()`
returns the connections open now. `setMaxConnections(max)` caps them, for
a socket that other processes can reach, such as one forwarded to another
host. A connection accepted while `max` are open is closed at once and
unread. It gets no id, `connectionOpened` and `connectionClosed` do not
come for it, and `connectionRefused()` does. Lowering the cap closes no
open connection. The default, 0, sets no limit.

`IpcRouter::hasSubscribers(target, signal)` tells whether a connection
subscribes to the pair now, so whether `broadcastEvent` has anyone to
write to. A target whose events cost work to build (it merges them, or
runs a timer for them) asks first and skips the work while nobody
listens. A subscription counts from the subscribe that the router
accepted until its unsubscribe, the close of its connection, the end of
the target's subscriptions or `stop()`. The router keeps a count for
each pair, so the call is one hash lookup.
`IpcRouter::subscribersChanged(target, signal)` comes when that answer
changes: the first subscriber to the pair came, or the last one went. It
is sent from the event loop, as `connectionClosed` is, and only when the
answer differs from the one that the previous signal for the pair told,
so a subscriber that comes and goes before the event loop runs sends
none. The router's destructor sends none.

`stop()` closes each connection at once and drops what is still
unwritten to it, so answers written just before it never arrive.
`stop(IpcRouter::StopMode::Flush)` delivers them first: the router stops
listening and forgets its connections at once, as `stop()` does, and each
connection closes when the client has taken what was written to it. A
client that does not read within the timeout (`DefaultFlushTimeoutMs`,
5 seconds, or the second argument) loses the rest. The delivery does not
need the router, which may be destroyed right after the call.

### CLI side

```
phosphorctl call theme.setMode --arg mode=dark
phosphorctl call pkg.install --arg name=foo --timeout 0   # waits for a deferred answer
phosphorctl list
phosphorctl schema theme | jq
phosphorctl subscribe theme.modeChanged    # stays connected, streams events
```

Socket-path priority: `--socket PATH` > `$PHOSPHOR_SOCKET` >
`$XDG_RUNTIME_DIR/phosphor.sock`.

`phosphorctl call` waits two seconds for the answer by default.
`--timeout SECONDS` changes that, and `--timeout 0` waits without a limit,
for a deferred call that answers when its work ends. Events the target
sends to the call go to stderr as they arrive, one compact JSON line each,
so stdout carries only the result.

### Acceptance harness

`phosphor-shell-libs/examples/phosphor-ipc-demo/` is the canonical end-to-end exercise.
The demo window includes a live event-log panel that mirrors the
broadcast stream. Running `phosphorctl call count.increment` from a
single sidecar terminal makes events appear in the demo window in
real time, with no separate `phosphorctl subscribe` terminal needed:

```
# terminal 1
./bin/phosphor-ipc-demo --socket /tmp/p.sock
# terminal 2
export PHOSPHOR_SOCKET=/tmp/p.sock
phosphorctl call count.increment        # demo's event log gains an entry
phosphorctl subscribe count.countChanged   # optional: same events on stdout
```

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

## Arbitration / lifecycle

- **Duplicate target id arbitrated.** Re-registering the SAME
  `QObject` under the SAME name is a silent idempotent success (no
  warning, no signal re-emit). Registering a DIFFERENT `QObject`
  under an existing name is rejected with a `qWarning`, and the
  incumbent registration is preserved. `registerTarget` returns
  `bool` so wrappers like `IpcTarget` can detect rejection and
  skip the paired unregister on teardown.
- **Signal name validated at subscribe.** Subscribe rejects with
  `NO_SUCH_SIGNAL` if the signal isn't declared on the target's
  metaobject. Typos surface immediately instead of silently
  ignoring later `emitEvent` calls.
- **Subscription scope is per-socket.** A subscription id is unique
  per connection, not globally. The wire identity is `(socket,
  subscriptionId)`, and a subscribe under an id that a live subscription
  on the connection uses is refused with `MALFORMED_REQUEST`.
- **A target's subscriptions end with it.** `unregisterTarget` ends
  them, and so does the destruction of a target object that was never
  unregistered. Each subscriber gets `NO_SUCH_TARGET` under its
  subscription id, and a later target under the name does not stream to
  it.
- **Not destroyed from its own stack.** The router is never destroyed
  from inside a call it runs or a signal it emits (a target method, a
  `connectionClosed` slot). `stop()` is fine there. So is
  `deleteLater()`, except inside a nested event loop that a target
  method spins, which runs the deletion while the method's call is still
  on the stack.
- **Disconnect prunes subscriptions.** When a subscriber socket
  disconnects, all its subscriptions are dropped automatically, and so
  are the connection's deferred calls. `connectionClosed(id)` follows.
- **Stale socket file recovery.** On `start`, if the socket path is
  occupied, the router probes with a quick connect. If the connect is
  refused, the stale path is unlinked and taken. A live listener, also a
  busy one, and a file that is not a socket are left alone (start fails
  cleanly rather than clobbering).
- **Socket mode without the umask.** The socket file has mode 0600. The
  router binds it in a new 0700 directory beside the path, sets the mode
  there and links it into place, so it never changes the umask, which
  every thread of the process shares. `stop()` removes the socket file,
  unless another file has taken the path since.
- **No subscribe auto-broadcast from Qt signals.** Plugin authors
  call `IpcTarget.emitEvent("name", args)` explicitly. Firing the
  QML signal (e.g. `modeChanged(...)`) only notifies in-QML
  subscribers, NOT wire subscribers. This split is by design.
  Introspecting arbitrary Qt signals via `qt_metacall` clashes with
  Q_OBJECT moc-generated code, and explicit `emitEvent` matches the
  "wire-visible state transitions are an explicit contract" model.

## Dependencies

- `QtCore` (always)
- `QtGui` (transitive, only via the static QML plugin `PhosphorIpcQml`. The runtime library has no QtGui usage.)
- `QtQml` (for `IpcTarget` QML registration)
- `QtNetwork` (for `QLocalServer` / `QLocalSocket`)

## See also

- `phosphor-shell/cli/phosphorctl/`: typed CLI shipped alongside the library.
- `phosphor-shell-libs/examples/phosphor-ipc-demo/`: acceptance harness. Three
  `IpcTarget` instances (greet / count / set-value) exercise the
  full call / list / schema / subscribe wire surface.
