<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team -->
<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->

# phosphor-surface

phosphor-surface is the surface-shader pack subsystem: the contract, pack metadata, registry and pack location for
persistent per-window decorations. Surface packs draw window chrome such as a border, rounded corners, a focus tint
or a glow. The same `effect.frag` runs on the compositor (`kwin-effect`) path and on the daemon surface-layer path.

## Responsibility

Surface shaders are the third shader-pack family, beside the animation shaders of phosphor-animation and the overlay
shaders of phosphor-shaders. The library parses and registers the packs, translates their parameters into uniform
slots, and resolves a decoration chain the same way for every host. It has no QML, no rendering and no surface
lifecycle. Rendering is in the consumers, for example [`phosphor-surface-quick`](../phosphor-surface-quick/README.md).

The namespace is `PhosphorSurfaceShaders`.

## Key types

| Type | Purpose |
|------|---------|
| `SurfaceShaderContract` | Named-uniform contract shared by both runtimes (`uTexture0`, `uSurfaceSize`, `uSurfaceFrameTopLeft`, `uSurfaceFrameSize`, `uSurfaceScale`, `uSurfaceFocused`) |
| `SurfaceShaderEffect` | Parsed `metadata.json` of one pack: id, shader paths, parameters, texture slots |
| `SurfaceShaderRegistry` | `Registry<SurfacePack>` and `MetadataPackLoader<SurfacePack>`: search paths, live reload, parameter translation, the generated `p_<id>` preamble |
| `SurfaceUniforms`, `SurfaceUniformProfile` | C++ mirror of the 672-byte std140 surface UBO, and the profile that fills it |
| `DecorationProfile`, `DecorationProfileTree` | The pack chain and parameters of one surface, and a tree over dot paths (for example `window.tiled`) with walk-up inheritance |
| `setSurfacePackDataSubdir()` | Sets where the installed surface packs are, relative to each `GenericDataLocation` directory |
| `resolveThemeParamColors()`, `resolveWindowStateParams()`, `composeStageMap()` | Helpers that every decoration host calls, so all hosts resolve a chain the same way |

## Use

Call `setSurfacePackDataSubdir()` one time at startup, before QML loads. The library has no default. The app normally
passes the `PHOSPHOR_SURFACE_PACKS_DATA_SUBDIR` that phosphor-surface-packs sets. If the subdir is empty, the lookups
that use it do not search the XDG data directories.

```cpp
#include <PhosphorSurface/SurfacePackLocation.h>
#include <PhosphorSurface/SurfaceShaderRegistry.h>

using namespace PhosphorSurfaceShaders;

setSurfacePackDataSubdir(QStringLiteral("phosphor-works/shaders/surface"));

SurfaceShaderRegistry registry;
registry.addSearchPaths(packDirs);  // LiveReload::On is the default
if (registry.hasEffect(QStringLiteral("border"))) {
    const SurfaceShaderEffect border = registry.effect(QStringLiteral("border"));
    const QVariantMap slots = SurfaceShaderRegistry::translateSurfaceParams(
        border, {{QStringLiteral("borderWidth"), 2}});
}
```

Call the registry on the GUI thread only. `effectsChanged()` fires after each committed rescan.

## Parameter slots

`translateSurfaceParams()` maps a friendly parameter map to slot keys, in metadata declaration order. Color
parameters and other parameters use separate allocators.

| Parameter type | Slot keys | Limit |
|----------------|-----------|-------|
| float, int, bool | `customParams1_x` to `customParams8_w` | 32 slots (`SurfaceShaderContract::kMaxParameterSlots`) |
| color | `customColor1` to `customColor16` | 16 slots (`SurfaceShaderContract::kMaxCustomColors`) |

If a parameter does not fit, the registry logs a warning and drops it. Color strings use the Qt form `#aarrggbb`. The
CSS form `#rrggbbaa` is not accepted.

## Dependencies

- `Qt6::Core`, `Qt6::Gui`
- [`phosphor-registry`](../phosphor-registry/README.md), [`phosphor-fsloader`](../phosphor-fsloader/README.md) and
  [`phosphor-shaders`](../phosphor-shaders/README.md), all PUBLIC

## See also

- [`phosphor-surface-quick`](../phosphor-surface-quick/README.md): the Qt Quick consumer of these packs.
- [`phosphor-pointer`](../phosphor-pointer/README.md): the pointer-shader family, which has the same shape.
