<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team -->
<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->

# phosphor-layer

phosphor-layer supplies the primitives for layer-shell surfaces. It has the `Surface` wrapper for each overlay, the
`SurfaceFactory` that builds surfaces with injected transport, engine and screen providers, the registry for each
screen, and the topology coordinator that updates the surfaces when monitors connect and disconnect.

## Responsibility

[`phosphor-wayland`](../phosphor-wayland/README.md) owns the low-level `zwlr_layer_shell_v1` code: the QPA plugin
and the raw `LayerSurface`. `phosphor-layer` is one level higher. It is the **policy layer**. It knows which overlay
surfaces exist now, which screens they belong to, which animator each surface has, and which well-known role each
surface has.

| Part | Type | What it does |
|------|------|--------------|
| Role primitive | `Role` | Value struct with the wlr-layer-shell parameters that do not change after `show()`: layer, anchors, keyboard interactivity, exclusive zone, default margins and scope prefix. It is only protocol terms, with no UI-pattern meaning. The named UI patterns (`Wallpaper()`, `Hud()`, `Modal()`, `Floating()`, `Panel(Edge)`, `Toast(Corner)`) are in the sibling library [`phosphor-shell-patterns`](../phosphor-shell-patterns/README.md), so that this library stays independent of a domain |
| Surface | `Surface` | One layer-shell window. It holds its configuration, knows its current screen, and has show and hide |
| SurfaceFactory | `SurfaceFactory` | Stateless builder that makes a live `Surface` from a `SurfaceConfig` and the injected dependencies. There is one factory for each process, and one `Surface` for each `create()` call |
| Per-screen registry | `ScreenSurfaceRegistry<T>` | Template container that owns one `Surface` (or subclass) for each screen, for surfaces with AllScreens affinity |
| Topology coordinator | `TopologyCoordinator` | Merges bursts of `screensChanged` and tells the registry and the factory to reconcile |
| Surface store | `ISurfaceStore` | Keeps the small part of the surface state that must stay after a restart. `JsonSurfaceStore` is the bundled file-backed implementation |
| Screen provider | `IScreenProvider` | Abstract source of `(screen-id, geometry, scale, refresh-rate)` tuples. Tests use it to inject sets of fake screens. `DefaultScreenProvider` is the bundled default |
| Surface animator | `ISurfaceAnimator` | Lets a consumer run a show or hide animation on the layer surface (opacity fade, slide from an edge and others). The consumer does not need to know the layer-shell internals |
| Layer-shell transport | `ILayerShellTransport` | The boundary between this library and the Wayland binding. The default implementation is `PhosphorWaylandTransport`. Tests use a mock |

## Key types

| Type | Purpose |
|------|---------|
| `PhosphorLayer::Surface`                    | Wrapper for one surface around a layer-shell role |
| `PhosphorLayer::SurfaceFactory`             | Stateless builder: `SurfaceConfig` → `Surface` |
| `PhosphorLayer::SurfaceConfig`              | Descriptor for one surface: role, screen, QML source, properties |
| `PhosphorLayer::Role`                       | Value struct for the wlr-layer-shell configuration (layer, anchors, keyboard, exclusive zone, margins, scope prefix) |
| `PhosphorLayer::ScreenSurfaceRegistry<T>`   | Surface tracking for each screen (template) |
| `PhosphorLayer::TopologyCoordinator`        | Merges bursts of `screensChanged` and reconciles the registry |
| `PhosphorLayer::ISurfaceStore`              | Keeps surface state across restarts |
| `PhosphorLayer::IScreenProvider`            | Lists screens with geometry and scale |
| `PhosphorLayer::ISurfaceAnimator`           | Show and hide animator for layer surfaces |
| `PhosphorLayer::ILayerShellTransport`       | Adapter to the Wayland binding |
| `PhosphorLayer::IQmlEngineProvider`         | Shared `QQmlEngine` that makes the QML of the surfaces |
| `PhosphorLayer::JsonSurfaceStore`           | Bundled file-backed `ISurfaceStore` (atomic through `QSaveFile`) |
| `PhosphorLayer::DefaultScreenProvider`      | Bundled `IScreenProvider` over `QGuiApplication::screens()` |
| `PhosphorLayer::NoOpSurfaceAnimator`        | Animator that shows and hides immediately (the default) |
| `PhosphorLayer::PhosphorWaylandTransport`   | Default `ILayerShellTransport` over the `LayerSurface` of `phosphor-wayland` (in `PhosphorLayer::Wayland`) |
| `PhosphorLayer::XdgToplevelTransport`       | Fallback transport that uses `xdg_toplevel` (no layer-shell) |

## Use

```cpp
#include <PhosphorLayer/SurfaceFactory.h>
#include <PhosphorLayer/SurfaceConfig.h>
#include <PhosphorLayer/Role.h>
#include <PhosphorLayer/defaults/PhosphorWaylandTransport.h>
#include <PhosphorLayer/defaults/DefaultScreenProvider.h>

using namespace PhosphorLayer;

auto transport = std::make_unique<PhosphorWaylandTransport>();
auto screens   = std::make_unique<DefaultScreenProvider>();
SurfaceFactory factory(SurfaceFactory::Deps{.transport = transport.get(),
                                            .screens = screens.get(),
                                            .engineProvider = engineProvider,
                                            .animator = animator});

SurfaceConfig cfg;
// Role is a plain value type. Construct one directly or use a recipe from
// the sibling phosphor-shell-patterns library if you prefer named UI
// patterns (Wallpaper, Hud, Modal, Floating, Panel, Toast).
cfg.role       = Role{Layer::Overlay,
                      AnchorAll,
                      -1,
                      KeyboardInteractivity::None,
                      QMargins(),
                      QStringLiteral("zone-outline")};
cfg.screen     = targetScreen;  // QScreen*; nullptr resolves to the provider's primary()
cfg.contentUrl = QUrl(QStringLiteral("qrc:/overlays/ZoneOutline.qml"));
cfg.contextProperties.insert(QStringLiteral("zones"), QVariant::fromValue(zoneList));

Surface *s = factory.create(std::move(cfg), /*parent*/ this);
s->show();
// Later:
s->hide();
```

## Targets

| Target | Holds | Links |
|--------|-------|-------|
| `PhosphorLayer::PhosphorLayer` | Everything below except `PhosphorWaylandTransport` | Qt Core/Gui/Qml/Quick. No Wayland client code |
| `PhosphorLayer::Wayland` | `PhosphorWaylandTransport` | `PhosphorLayer::PhosphorLayer`, and `phosphor-wayland` privately |

Link `PhosphorLayer::Wayland` only where you construct a `PhosphorWaylandTransport`. Some hosts show surfaces
without a Wayland client connection, for example a compositor that draws them as scene nodes through its own
`ILayerShellTransport`. Such a host links only the core target. The build makes the Wayland target when
`PHOSPHORLAYER_WITH_PHOSPHORWAYLAND` is ON (the default).

## Design notes

- **The scope prefix is the identity of a role.** Two `Role` values with the same scope prefix refer to the same
  logical kind of surface. The registry for each screen uses this to merge duplicates. Consumers make their roles
  from a pattern of `phosphor-shell-patterns` and put their own scope prefix on it.
- **The transport is injectable.** Every interaction with the Wayland layer-shell goes through
  `ILayerShellTransport`. Tests verify that the factory told the transport to change the anchor to Top+Right, with
  no live Wayland compositor.
- **The animator is optional.** A surface without an animator shows and hides with no transition. Attach an
  `ISurfaceAnimator` to get a fade, a slide or a scale.
- **`defaults/` is next to the interfaces.** Each `I*` interface has a bundled implementation in
  `PhosphorLayer/defaults/`. A composition root can connect the common case in three lines, and replace any one part
  for tests or for other compositors.

## Dependencies

- `QtCore`, `QtGui`, `QtQml`, `QtQuick`
- [`phosphor-wayland`](../phosphor-wayland/README.md). The default transport binds to its `LayerSurface`.

## See also

- [`phosphor-shell-patterns`](../phosphor-shell-patterns/README.md). UI-pattern recipes (Wallpaper, Hud, Modal, Floating, Panel, Toast) on the `Role` primitive of this library.
- [`phosphor-surfaces`](../phosphor-surfaces/README.md). Higher-level surface manager on these primitives. It adds Vulkan and pipeline-cache wiring.
- [`phosphor-wayland`](../phosphor-wayland/README.md). The QPA plugin and raw layer-shell binding that the default transport talks to.
