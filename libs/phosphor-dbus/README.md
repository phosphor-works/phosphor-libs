<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team -->
<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->

# phosphor-dbus

phosphor-dbus gives generic D-Bus client utilities for Qt applications. It does not depend on a particular service.

## Responsibility

Each consumer that talks to a D-Bus service needs the same boilerplate. It makes a `QDBusMessage`, sends an async
call, allocates a `QDBusPendingCallWatcher`, routes errors and cancels on destruction. Also, each adaptor that
marshals a custom struct can crash at runtime with "demarshalling function failed" if a streaming operator is
missing.

`phosphor-dbus` owns that generic plumbing. It knows nothing about Phosphor or about any particular service.
Project-specific service names and wire types belong one layer up (see
[`phosphor-protocol`](../phosphor-protocol/README.md)). Any Qt app, shell plugin or tool can use this library
directly with its own endpoints.

## Key types

| Type | Purpose |
|------|---------|
| `PhosphorDBus::Client`           | Value-type method-call client bound to a `(connection, service, objectPath)` triple. Provides `fireAndForget`, `sendOneWay`, `asyncCall`, `syncCall`, `createCall`. |
| `PhosphorDBus::ObjectManager`    | Service-agnostic observer for `org.freedesktop.DBus.ObjectManager`. Issues `GetManagedObjects`, tracks `InterfacesAdded` / `InterfacesRemoved`, and emits raw `(path, interfaces)` payloads for consumers to materialize their own typed objects. |
| `PhosphorDBus::HasDBusStreaming` | Compile-time check that a type has `QDBusArgument` `operator<<` / `operator>>`. Use in `static_assert` to catch a missing marshaller at build time. |
| `PhosphorDBus::lcPhosphorDBus`   | Default logging category for call-failure warnings. |

## Use

```cpp
#include <PhosphorDBus/Client.h>

using PhosphorDBus::Client;

// Bind once to a destination — Client is a cheap, copyable value.
Client client(QDBusConnection::sessionBus(),
              QStringLiteral("org.example.Service"),
              QStringLiteral("/example/Object"));

// Fire-and-forget; the watcher is parented so it cancels with `this`.
client.fireAndForget(this, QStringLiteral("org.example.Iface"),
                     QStringLiteral("doThing"), {42});

// Async call the caller consumes itself.
QDBusPendingCall pending =
    client.asyncCall(QStringLiteral("org.example.Iface"), QStringLiteral("query"));
```

```cpp
#include <PhosphorDBus/Streaming.h>

static_assert(PhosphorDBus::HasDBusStreaming<MyEntry>::value,
              "MyEntry needs QDBusArgument operator<< and operator>>");
```

## Design notes

- **`Client` is a value object, not a singleton.** It holds a connection handle and two strings. Thus it is cheap
  to copy and to construct at each call site. A project that always targets one daemon puts the construction in a
  small free-function factory that returns a `Client` by value. There is no shared global state and no
  `Q_GLOBAL_STATIC`.
- **No synchronous introspection.** Calls use `QDBusMessage::createMethodCall`, never `QDBusInterface`. Thus a call
  never blocks the calling thread on wire introspection. This is important for compositor plugins.
- **Errors go to a category that the caller supplies.** `Client` takes an optional `QLoggingCategory*`. Thus
  consumers see failures under their own category, not under a category of this library.
- **`ObjectManager` has no types.** It demarshals the `a{oa{sa{sv}}}` / `a{sa{sv}}` payloads by hand (no
  nested-container metatype to register). It emits the raw `(path, interfaces)` maps, and each service builds its
  own domain objects.
- **`ObjectManager` signals the first snapshot.** Its `ready()` signal fires one time, after the initial
  `GetManagedObjects` round-trip completes (success or error). Consumers then know that the initial snapshot
  arrived before they rely on incremental signals.

## Dependencies

- `QtCore`, `QtDBus`

## See also

- [`phosphor-protocol`](../phosphor-protocol/README.md): the Phosphor contract layer. It uses `Client` and
  `HasDBusStreaming` with the `org.phosphor_works.Placement` service constants and wire types.
