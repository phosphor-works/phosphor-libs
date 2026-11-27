<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team -->
<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->

# phosphor-wayland

phosphor-wayland connects Qt windows to the Wayland layer-shell. It has a custom QPA plugin that shows `QWindow`s
as `zwlr_layer_shell_v1` surfaces. It also has the `LayerSurface` Qt wrapper, which gives anchors, exclusive zone,
margins and keyboard interactivity as `Q_PROPERTY`s.

## Responsibility

The standard QPA plugins of Qt (`wayland`, `xcb`) make desktop-level windows. Overlays such as zone outlines,
snap-assist previews and shader wallpapers must be on a **layer-shell surface**. Such a surface needs anchor,
margin, exclusive-zone and keyboard-interactivity settings for each surface, and the usual Qt surface API does not
give them.

`phosphor-wayland` is the lowest level of the stack:

- **A custom QPA plugin** (`phosphorwayland`). To load it, set `QT_WAYLAND_SHELL_INTEGRATION=phosphorwayland`
  before `QGuiApplication`. It hosts a `QQuickWindow` on a `wlr-layer-shell-v1` surface.
- **`LayerSurface`**, the Qt wrapper around a layer-shell role. The API is only Qt. Callers get `QObject` and
  `Q_PROPERTY`s and never see Wayland types. The wrapper talks to the QPA plugin through a small set of dynamic
  property keys (`LayerSurfaceProps::*`). Thus a build that does not link this library can still construct a
  `QWindow` with the same properties, and the plugin reads them.
- **`registerLayerShellPlugin()`**, a header-only helper. It sets the QPA environment variable when a Wayland
  compositor runs. It keeps an existing `QT_WAYLAND_SHELL_INTEGRATION`, so that a developer can override it for
  debugging. For compositors such as COSMIC that do not set `WAYLAND_DISPLAY`, it falls back to a check of
  `XDG_RUNTIME_DIR/wayland-{0,1}`.

[`phosphor-layer`](../phosphor-layer/README.md) is one level higher. It supplies the policy layer (roles, the
surface registry for each screen, the topology coordinator). Application code usually uses `phosphor-layer` or
[`phosphor-surfaces`](../phosphor-surfaces/README.md). It touches this library only through
`registerLayerShellPlugin()` at startup.

## Key types

| Type | Purpose |
|------|---------|
| `PhosphorWayland::LayerSurface`            | `QObject` wrapper around a `zwlr_layer_shell_v1` surface, with a `Q_PROPERTY` for layer, anchors, exclusive zone, keyboard, scope, screen and margins |
| `PhosphorWayland::LayerSurface::Layer`     | Background / Bottom / Top / Overlay |
| `PhosphorWayland::LayerSurface::Anchors`   | `Q_FLAGS` of `AnchorTop / Bottom / Left / Right`, and the `AnchorAll` constant for full-screen anchors |
| `PhosphorWayland::LayerSurface::KeyboardInteractivity` | None / Exclusive / OnDemand |
| `PhosphorWayland::LayerSurfaceProps`       | Property-key constants for `LayerSurface` ↔ QPA plugin |
| `PhosphorWayland::registerLayerShellPlugin`| Header-only setup of the environment variable before `QGuiApplication` |
| `PhosphorWayland::addCompositorLostCallback` / `removeCompositorLostCallback` | Process-wide subscription to the global removal of `zwlr_layer_shell_v1` (one-shot, thread-safe) |
| `PhosphorWayland::BackgroundEffect`        | Compositor blur behind a region of a window (`ext-background-effect-v1`). `blurSupported` says if the compositor applies it. The region has no effect until the compositor applies it |

## Use

Register the plugin before `QGuiApplication`. Then build a layer surface in the usual Qt style:

```cpp
#include <PhosphorWayland/LayerShellPluginLoader.h>
#include <PhosphorWayland/LayerSurface.h>

int main(int argc, char** argv) {
    PhosphorWayland::registerLayerShellPlugin();   // safe no-op on non-Wayland sessions
    QGuiApplication app(argc, argv);

    using namespace PhosphorWayland;
    // Attach a LayerSurface to a QWindow BEFORE the first show(). The
    // surface drives the QPA plugin via dynamic properties on the window.
    QWindow window;
    auto* surface = LayerSurface::get(&window);   // factory; never `new LayerSurface`
    surface->setLayer(LayerSurface::LayerOverlay);
    surface->setAnchors(LayerSurface::AnchorAll);
    surface->setKeyboardInteractivity(LayerSurface::KeyboardInteractivityOnDemand);
    surface->setScope(QStringLiteral("zone-overlay"));
    surface->setScreen(QGuiApplication::primaryScreen());
    surface->setMargins(QMargins(8, 8, 8, 8));
    window.show();
    return app.exec();
}
```

## Design notes

- **The API is only Qt.** No Wayland types come through `LayerSurface`. Callers see `QMargins`, `QScreen*` and Qt
  enums. The QPA plugin reads the dynamic properties on the `QWindow` below and drives `wl_layer_shell` itself.
- **Header-only setup of the environment variable.** `registerLayerShellPlugin()` is inline. Thus callers do not
  link the library of the QPA plugin only to set one environment variable. When the variable is set, the plugin
  loads through the standard QPA discovery of Qt.
- **The check for a running compositor is conservative.** `WAYLAND_DISPLAY` proves that a compositor runs. The
  library does not use `XDG_SESSION_TYPE`, because it can be `wayland` in SSH sessions or before the compositor
  starts. The fallback checks `$XDG_RUNTIME_DIR/wayland-{0,1}` for COSMIC and for socket-activation setups that do
  not set `WAYLAND_DISPLAY`.
- **Compositor loss is public.** The QPA plugin watches `wl_registry::global_remove` for `zwlr_layer_shell_v1`. It
  sends the event through `PhosphorWayland::addCompositorLostCallback` (see `<PhosphorWayland/CompositorLost.h>`).
  The broadcaster fires one time for each process, and it accepts a registration before `QGuiApplication`. The
  default `PhosphorWaylandTransport` in [`phosphor-layer`](../phosphor-layer/README.md) subscribes through this
  interface. Thus a compositor crash during a session fires the transport callbacks, in addition to the existing
  clean-exit `aboutToQuit` path.

## Dependencies

- `QtCore`, `QtGui`
- `wayland-client`, `Qt6::WaylandClientPrivate`, and the protocols vendored in `protocols/` (wlr-layer-shell,
  ext-session-lock, ext-background-effect and the others)

The generated protocol code is compiled into the library and the QPA plugin, so each protocol's licence notice
(MIT or HPND-sell-variant) ships with them. The install puts the notices in
`share/licenses/phosphor-libs/LICENSE.wayland-protocols`. A project that builds this library in-tree and installs it
on its own terms finds that file in `PHOSPHORWAYLAND_LICENSE_FILES`.

## See also

- [`phosphor-layer`](../phosphor-layer/README.md): the policy layer (roles, registry for each screen, topology coordinator) on `LayerSurface`.
- [`phosphor-surfaces`](../phosphor-surfaces/README.md): a higher-level surface manager with QML and Vulkan wiring.
- [`phosphor-shaders`](../phosphor-shaders/README.md): shader-domain headers (`BaseUniforms`, `IUniformExtension`, `ShaderRegistry`).
