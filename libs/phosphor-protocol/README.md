<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team -->
<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->

# phosphor-protocol

phosphor-protocol is the Phosphor D-Bus contract. It holds the service names, the wire types and the daemon client.

## Responsibility

A daemon, a compositor-side plugin (such as a KWin effect) and a settings UI talk to each other over D-Bus. This
library is the one place that owns the shared `org.phosphor_works.Placement` wire surface. Thus consumers do not
each add their own magic strings and ad-hoc marshallers.

The library has layers. The value vocabulary does not bring QtDBus into pure-compute consumers. The generic D-Bus
plumbing stays available for other uses.

```
phosphor-dbus              generic D-Bus client + HasDBusStreaming
   ▲
phosphor-protocol          org.phosphor_works.Placement contract  ── this library
   ▲
PhosphorProtocol::Types    QtCore-only value vocabulary
```

## Key types

The wire surface has one header pair for each interface group. Thus a translation unit compiles again only when
the interface that it uses changes. Each group has a QtCore-only `*Types.h` and a matching QtDBus
`*Marshalling.h`:

| Interface group | Types header | Marshalling header |
|-----------------|--------------|--------------------|
| Drag        | `DragTypes.h`       | `DragMarshalling.h` |
| Window state | `WindowTypes.h`    | `WindowMarshalling.h` |
| Autotile    | `AutotileTypes.h`   | `AutotileMarshalling.h` |
| Snap navigation | `NavigationTypes.h` | `NavigationMarshalling.h` |
| Zone / overlay | `ZoneTypes.h`    | `ZoneMarshalling.h` |
| Compositor bridge | `BridgeTypes.h` | `BridgeMarshalling.h` |

The headers that all groups use:

| Header | Purpose |
|--------|---------|
| `PhosphorProtocol/ServiceConstants.h` | Canonical service name, object path, interface names (`org.phosphor_works.Placement.Tiling`, `.Screen`, `.Overlay`, …), API version, timeouts. **QtCore only**, shipped by `::Types`. |
| `PhosphorProtocol/Registration.h`     | `registerWireTypes()` does one-shot metatype registration. Call it once at startup. |
| `PhosphorProtocol/ClientHelpers.h`    | `daemonClient()` returns a `PhosphorDBus::Client` value bound to the daemon, plus thin `ClientHelpers::` wrappers (`fireAndForget`, `sendOneWay`, `asyncCall`, `syncCall`, `loadSettingAsync`). |
| `PhosphorProtocol/PhosphorProtocol.h` | Umbrella convenience header that pulls in everything. Prefer the per-interface headers. |

A `*Types.h` header is pure value vocabulary (`Q_DECLARE_METATYPE`, no QtDBus). The matching `*Marshalling.h` adds
the `QDBusArgument` operators and the `HasDBusStreaming` `static_assert` guard.

## Use

Register the wire types one time at startup. Then call the daemon through the `ClientHelpers` wrappers:

```cpp
#include <PhosphorProtocol/ClientHelpers.h>
#include <PhosphorProtocol/Registration.h>

#include <QDebug>

using namespace PhosphorProtocol;

registerWireTypes();

// Read one setting from the Settings interface of the daemon.
ClientHelpers::loadSettingAsync(this, Service::SettingProperty::InnerGap, [](const QVariant &value) {
    qDebug() << "innerGap" << value.toInt();
});

// After this process writes config.json, tell the daemon to read it again.
ClientHelpers::reloadDaemonSettings(this);
```

To address a method that has no wrapper, use the client that `daemonClient()` returns, with an interface name from
`Service::Interface`.

## Targets

| CMake target | Links | Linked by |
|--------------|-------|-----------|
| `PhosphorProtocol::Types`            | `Qt6::Core` only | Pure-compute domain libraries (snap engine, placement) that need to *name* protocol results but never touch the bus. |
| `PhosphorProtocol::PhosphorProtocol` | `::Types` + `Qt6::DBus` + `PhosphorDBus` | D-Bus adaptors, the daemon/effect entry points, transport round-trip tests. |

## Design notes

- **Value types do not depend on a transport.** A `*Types.h` header defines its structs with no QtDBus dependency.
  Thus a domain library can name `MoveTargetResult` and not link the bus. The `QDBusArgument` marshallers are free
  functions in the matching `*Marshalling.h`, on top of the types. The struct does not know that it travels over
  D-Bus.
- **One source of truth for interface names.** New compositor integrations use `ServiceConstants`. They do not
  write the string again.
- **Wire compatibility is explicit.** Some enums serialize as their legacy strings (for example the drag policy).
  Thus rolling upgrades do not need an ApiVersion bump. An unknown wire value parses to a safe default. It does not
  cause a failure.
- **The daemon client is a thin binding.** `daemonClient()` returns a `PhosphorDBus::Client` by value. The client is
  already bound to `org.phosphor_works.Placement` on the session bus. The generic call mechanics are in
  `phosphor-dbus`, and this library supplies only the Phosphor endpoint. There is no singleton.
- **Build-time marshaller check.** If you add a struct to a `*Types.h` and do not add its `operator<<` / `>>` in the
  matching `*Marshalling.h`, the `HasDBusStreaming` `static_assert` stops the build. Thus the error does not crash
  the D-Bus dispatch at runtime.

## Dependencies

- `QtCore`, `QtDBus`
- [`phosphor-dbus`](../phosphor-dbus/README.md): the generic D-Bus client and the `HasDBusStreaming` trait.

## See also

- [`phosphor-screens`](../phosphor-screens/README.md): `DBusScreenAdaptor` implements the `.Screen` interface from
  these constants.
- [`phosphor-snap-engine`](https://github.com/phosphor-works/phosphor-engine-libs/blob/main/libs/phosphor-snap-engine/README.md):
  the navigation result types (`MoveTargetResult`, `FocusTargetResult`, etc.) come from `NavigationTypes.h`. The
  snap engine links them without QtDBus through `PhosphorProtocol::Types`.
