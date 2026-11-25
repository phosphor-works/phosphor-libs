<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team -->
<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->

# phosphor-screens

phosphor-screens tracks physical and virtual screen topology and panel reservations. It also supplies the canonical
`org.phosphor_works.Placement.Screen` D-Bus surface.

## Responsibility

The library maps a cursor position to the screen ID that the next event goes to. It tracks these items:

- Physical screens, as `QScreen` reports them.
- User-defined virtual sub-regions in each physical screen.
- Panel reservations from taskbars, docks and status bars, which take part of the usable geometry.

It exposes all of them on the canonical `org.phosphor_works.Placement.Screen` D-Bus interface, so downstream
consumers stay compositor-agnostic.

## Key types

| Type | Purpose |
|------|---------|
| `PhosphorScreens::ScreenManager`      | Physical and virtual topology state, with change signals when screens come and go. |
| `PhosphorScreens::ScreenResolver`     | Point-to-screen lookup that accepts an optional D-Bus endpoint override. |
| `PhosphorScreens::IPanelSource`       | Pluggable panel-reservation source for each desktop (Plasma, GNOME, wlr). |
| `PhosphorScreens::PlasmaPanelSource`  | Bundled `IPanelSource` for `org.kde.plasmashell` reservations. |
| `PhosphorScreens::VirtualScreenDef`   | One rectangular sub-region of a physical screen with its own screen ID. |
| `PhosphorScreens::VirtualScreenSwapper` | Directional virtual-screen swaps (`left`, `right`, `up`, `down`) and rotation, written through an `IConfigStore`. `DBusScreenAdaptor` exposes them on D-Bus. |
| `PhosphorScreens::DBusScreenAdaptor`  | Canonical `org.phosphor_works.Placement.Screen` D-Bus surface. |
| `PhosphorScreens::IConfigStore`       | Persisted virtual-screen configuration. `InMemoryConfigStore` is the default for tests. |

## Use

Link the smallest target that has the types you need (see "Targets"). The consumer owns the panel source and the
config store, and both must outlive the manager. Call every `ScreenManager` member on the GUI thread.

```cpp
#include <PhosphorScreens/InMemoryConfigStore.h>
#include <PhosphorScreens/Manager.h>
#include <PhosphorScreens/PlasmaPanelSource.h>

using namespace PhosphorScreens;

PlasmaPanelSource panels;
InMemoryConfigStore store;
ScreenManager manager(ScreenManagerConfig{.panelSource = &panels, .configStore = &store});
manager.start();

const QString screenId = manager.effectiveScreenAt(QCursor::pos());
const QRect usable = manager.screenAvailableGeometry(screenId);
```

A process that does not own a `ScreenManager` asks the daemon over D-Bus with
`ScreenResolver::effectiveScreenAt(pos)`. If the daemon is not on the bus, it falls back to Qt.

## Targets

The library has three targets, so that the screen-topology core does not bring QtDBus into pure-compute consumers.

| CMake target | Links | Linked by |
|--------------|-------|-----------|
| `PhosphorScreens::Core`            | `Qt6::Core` + `Qt6::Gui` + `PhosphorIdentity::PhosphorIdentity` (+ `PhosphorGeometry` privately) | POD-only consumers (for example phosphor-overlay) that need the topology types and identity, but never touch the bus or the live manager. |
| `PhosphorScreens::Runtime`         | `::Core` + `Qt6::Core` + `Qt6::Gui` (+ `PhosphorWayland` privately) | Consumers that need the live `ScreenManager` (physical-screen tracking through Wayland): the daemon, the snap and tile engines, placement and zones. |
| `PhosphorScreens::PhosphorScreens` | `::Runtime` + `Qt6::DBus` + `PhosphorProtocol::Types` | The daemon and anything that needs the D-Bus surface (`DBusScreenAdaptor`, `PlasmaPanelSource`, `ScreenResolver`). |

## Design notes

- **Virtual screens are first-class.** Each virtual screen gets its own screen ID, layout assignments, autotile
  state and overlay windows. Everything downstream treats them exactly like physical screens.
- **The panel source is pluggable.** Plasma exposes reservations through `org.kde.plasmashell`, GNOME through
  `org.gnome.Mutter`, and sway and Hyprland through wlr-foreign-toplevel. The manager core stays compositor-agnostic,
  because it delegates to an `IPanelSource` that the consumer owns.
- **Direction tokens match the wire format.** `Direction::Left` / `Right` / `Up` / `Down` are lower-case ASCII
  strings. The D-Bus `swapVirtualScreenInDirection` method accepts the same strings. Thus adaptors can pass user
  strings through without change.
- **The D-Bus surface is a separate target.** `DBusScreenAdaptor`, the Plasma panel source and the resolver are in
  `PhosphorScreens`. The live `ScreenManager` is in `PhosphorScreens::Runtime`, and the POD topology types are in
  `PhosphorScreens::Core`. A snap or tiling engine links `::Runtime` and does not pull in QtDBus to reason about
  screens.

## Dependencies

| Target | Dependencies |
|--------|--------------|
| `PhosphorScreens::Core` | `QtCore`, `QtGui`, [`phosphor-identity`](../phosphor-identity/README.md) (`VirtualScreenId` format helpers), `phosphor-geometry` (private) |
| `PhosphorScreens::Runtime` | Also `phosphor-wayland` (private) for the live `ScreenManager` |
| `PhosphorScreens` | Also `QtDBus` and `PhosphorProtocol::Types` (for the `org.phosphor_works.Placement.Screen` service constants) |

## See also

- [`phosphor-identity`](../phosphor-identity/README.md): where the screen-id wire format is defined.
- [`phosphor-protocol`](../phosphor-protocol/README.md): the `org.phosphor_works.Placement.Screen` interface name
  comes from `ServiceConstants`.
