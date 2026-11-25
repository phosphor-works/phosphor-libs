<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team -->
<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->

# phosphor-surfaces

phosphor-surfaces is a layer-shell surface manager with synchronous QML loading and Vulkan wiring. App code uses it
when it needs a zone overlay, a drag ghost or any other layer-shell QML scene.

## Responsibility

This is a higher-level surface manager on top of [`phosphor-layer`](../phosphor-layer/README.md). From a
`SurfaceConfig`, `SurfaceManager::createSurface()` does these steps:

1. It warms up a QML scene.
2. It creates the layer-shell window.
3. It attaches a caller-owned or library-managed `QVulkanInstance`.
4. It returns a `Surface*` that is ready to show.

The manager also keeps a hidden keep-alive surface that keeps the scene graph and QRhi device warm for later
surfaces.

## Key types

| Type | Purpose |
|------|---------|
| `PhosphorSurfaces::SurfaceManager`       | Factory and owner for layer-shell surfaces. |
| `PhosphorSurfaces::SurfaceManagerConfig` | Surface factory, `QQmlEngine` hook, pipeline-cache path, Vulkan instance, Vulkan API version, extra Vulkan device extensions. |

## Use

```cpp
using namespace PhosphorSurfaces;

SurfaceManagerConfig cfg;
cfg.surfaceFactory = factory;                          // from phosphor-layer
cfg.engineConfigurator = [](QQmlEngine& e) { /* imports, context props */ };
cfg.pipelineCachePath = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
    + QStringLiteral("/pipeline.cache");               // a file path

SurfaceManager mgr(std::move(cfg));
PhosphorLayer::SurfaceConfig surfaceCfg;               // screen, role, contentUrl
auto* surface = mgr.createSurface(std::move(surfaceCfg), /*parent*/ this);
```

`createSurface()` returns nullptr on failure and logs the reason. `SurfaceConfig` is move-only. If a surface has a
parent other than the manager, destroy it before the manager, or call `drainDeferredDeletes()` in the destructor of
that parent.

## Keep-alive

`SurfaceManager` creates a 1x1 keep-alive surface on the primary screen and never shows it. If no screen is
available, it tries again when a screen is added. `keepAliveActive()` tells if the surface exists. If the surface is
lost, the manager emits `keepAliveLost()` and creates it again. `nextScopeGeneration()` returns a new, increasing
scope number for each call.

## Design notes

- **Synchronous QML loading only.** Callers pass `qrc:/` or `file:/` URLs that resolve without a network hop.
  `createSurface()` rejects an async QML load path and returns nullptr, so a surface never comes back half-warmed.
- **Caller-owned Vulkan, with a fallback.** If `SurfaceManagerConfig::vulkanInstance` is non-null, the manager calls
  `setVulkanInstance()` with that pointer on every window. If it is null and the active Qt graphics API is Vulkan,
  `SurfaceManager` creates and owns a fallback instance.
- **The pipeline cache is shared.** All surfaces that this manager creates use one on-disk pipeline cache. Thus
  shader-effect warmup on the second surface costs almost nothing.

## Dependencies

- `QtCore`, `QtQuick`
- [`phosphor-layer`](../phosphor-layer/README.md): `Surface` / `SurfaceFactory` / transport primitives
- [`phosphor-shell-patterns`](../phosphor-shell-patterns/README.md): UI-pattern recipes on top of
  `PhosphorLayer::Role`

## See also

- [`phosphor-layer`](../phosphor-layer/README.md): the lower-level primitives that this manager wraps.
