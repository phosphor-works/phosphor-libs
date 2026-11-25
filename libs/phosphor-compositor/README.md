<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team -->
<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->

# phosphor-compositor

phosphor-compositor is the SDK for compositor plugins of Phosphor **placement**. It supplies the bridge interface,
the typed D-Bus client for the interfaces of the tiling daemon, and the state helpers that a plugin needs. With it,
the users of a plugin get zone-based window management.

## Responsibility

Phosphor has two parts. The **daemon** owns the placement logic, zones, layouts and settings. The **compositor
plugin** observes windows, applies geometry and renders overlays. This library is the plugin side of that split,
for placement only.

The scope is what a plugin needs to place windows. `DaemonClient` speaks the `WindowTracking`, `WindowDrag`,
`Tiling`, `LayoutRegistry`, `Screen`, `Settings` and `CompositorBridge` interfaces. The helpers cover title-bar
ownership, float and zone caches, trigger parsing and rounding that is safe under scaling. The plugin draws all its
visuals itself. In PlasmaZones, the overlay, decoration and animation rendering in `kwin-effect/` uses
`phosphor-rendering` / `phosphor-shaders` / `phosphor-animation`. This library wraps none of it.

A compositor plugin does these steps:

1. It links `PhosphorCompositor`.
2. It implements `ICompositorBridge`, the interface that maps native window handles to the terms of the daemon.
3. It connects handler interfaces for callbacks.
4. It lets `DaemonClient` send the calls that `DaemonClient` wraps.

`DaemonClient` wraps these calls:

- bridge registration
- the notices for a window that opens, closes or activates, and for the focused window that changes screen
- the drag round trips
- the cursor and primary screen notices
- the floating, snapped, pending-restore and virtual-screen queries
- the prune of stale windows
- the geometry, raise, activate, float and snap-assist signals, which it dispatches to the handlers

`DaemonClient` is not a complete client. The plugin's own D-Bus code makes the other calls of the daemon, for
example window metadata, frame geometry, and the `Snap`, `Scrolling` and `Control` interfaces. The PlasmaZones KWin
effect makes all of its D-Bus calls itself and does not use `DaemonClient`. The plugin never touches placement logic
directly. The daemon decides *where*, and the plugin applies *how*.

## Key types

| Type | Purpose |
|------|---------|
| `ICompositorBridge` | Abstract interface that a plugin implements for window lookup, identity, properties, filtering and actions |
| `DaemonClient` | Typed D-Bus client. It handles registration, service watching, reconnection, method calls and signal dispatch |
| `IDragHandler` | Callback interface for drag start, move, end and policy change |
| `IGeometryHandler` | Callback interface for geometry apply, batch operations, raise and activate |
| `ILifecycleHandler` | Callback interface for window open, close, activate and float change |
| `BorderState` + `TilingStateHelpers` | Border-state value type for each screen, and pure helper functions |
| `FloatingCache` | Compositor-side copy of the float state of the daemon |
| `ZoneCache` | Compositor-side copy of the window-to-zone assignments |
| `DecorationManager` | Single owner of the server-side title-bar state (acquire, release, restore of the prior state) |
| `SnapAssistFilter` | Builds snap-assist candidates through the bridge |
| `TriggerParser` | Matches modifier and button activation from the configuration |
| `DebouncedScreenAction` | Debounce utility that merges screen changes |
| `GeometryHelpers` | Rounding that is safe under fractional scaling |

## Use

```cpp
#include <PhosphorCompositor/DaemonClient.h>
#include <PhosphorCompositor/ICompositorBridge.h>
#include <PhosphorCompositor/IGeometryHandler.h>

class MyBridge : public PhosphorCompositor::ICompositorBridge { /* ... */ };

class MyPlugin : public PhosphorCompositor::IGeometryHandler,
                 public PhosphorCompositor::IDragHandler,
                 public PhosphorCompositor::ILifecycleHandler
{
    PhosphorCompositor::DaemonClient m_client;
    MyBridge m_bridge;

    void init() {
        m_client.setGeometryHandler(this);
        m_client.setDragHandler(this);
        m_client.setLifecycleHandler(this);

        connect(&m_client, &PhosphorCompositor::DaemonClient::daemonReady,
                this, [this]() {
            m_client.registerBridge(QStringLiteral("river"), PhosphorProtocol::Service::ApiVersion,
                                    {QStringLiteral("borderless"), QStringLiteral("animation")});
        });
        // Report nothing before this. The daemon admits window, drag and
        // screen reports only from the registered bridge.
        connect(&m_client, &PhosphorCompositor::DaemonClient::bridgeRegistered,
                this, [this]() {
            announceExistingWindows();
        });
    }

    // IGeometryHandler
    void onApplyGeometry(const PhosphorCompositor::GeometryRequest& req) override {
        auto* window = m_bridge.findWindowById(req.windowId);
        if (window) m_bridge.moveResize(window, req.geometry);
    }
    // ... other handlers
};
```

## Existing adapters

| Compositor | Adapter | Location |
|------------|---------|----------|
| KWin 6 | `KWinCompositorBridge` | `plasmazones/kwin-effect/compositor/compositorbridge.{h,cpp}` |

## Architecture

```
Daemon (org.phosphor_works.Placement D-Bus service)
    ↕ D-Bus IPC
DaemonClient (this library)
    ↓ dispatches to
IDragHandler / IGeometryHandler / ILifecycleHandler
    ↓ implemented by
Compositor Plugin
    ↓ calls
ICompositorBridge (plugin implements, wraps native window APIs)
```

The daemon always runs. The plugin keeps no placement state. It applies what the daemon tells it and reports window
events back.

### Registration comes first

The daemon admits the reports of the compositor only from the bus name that registered the bridge. These reports
are window opens, closes and activations, the drag calls, the cursor and primary screen notices, and the prune of
stale windows. The daemon answers every other caller with `AccessDenied`. Thus a plugin must wait for
`bridgeRegistered` before it reports anything. `DaemonClient` refuses a report that comes earlier and logs one
warning. It does this because a fire-and-forget call never sees the refusal of the daemon. The queries are not
gated.

Only the process of the compositor can register. The daemon accepts a registration only from a bus connection that
the process at the far end of its Wayland connection owns. When that process is `kwin_wayland_wrapper`, a direct
child of it is also accepted. The daemon answers any other process with a `REJECTED` session, and `DaemonClient`
reports it as `bridgeRejected`. Thus a bridge must run in the compositor, not in a separate helper process.

Only one bridge is registered at a time. While the registered bridge is on the bus, a registration from another bus
name gets a `REJECTED` session, and `DaemonClient` reports it as `bridgeRejected`. The daemon accepts a new
registration from the same name. It also accepts any name after the old one left the bus.

## Dependencies

- `Qt6::Core`, `Qt6::Gui`, `Qt6::DBus`
- [`phosphor-protocol`](../phosphor-protocol/README.md), [`phosphor-identity`](../phosphor-identity/README.md)
