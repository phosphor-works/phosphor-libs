<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team -->
<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->

# phosphor-pointer

phosphor-pointer is the pointer-shader pack subsystem: packs that decorate the mouse pointer with trails, halos, click
ripples and sparks. It holds the contract, pack metadata, registry and the `PointerHistory` sampler that both hosts
use.

## Responsibility

Pointer shaders are the fourth shader-pack family, beside overlay (phosphor-shaders), animation
(phosphor-animation) and surface ([`phosphor-surface`](../phosphor-surface/README.md)). The library has the same
shape as phosphor-surface. Rendering is in the consumers: the compositor, and the Qt RHI preview.

The namespace is `PhosphorPointerShaders`.

## Key types

| Type | Purpose |
|------|---------|
| `PointerShaderContract` | Named-uniform contract shared by both runtimes, and its limits |
| `PointerShaderUniforms` (`PointerUniformsTail`) | std140 mirror of the UBO tail, with the layout pinned |
| `PointerShaderEffect` | Parsed `metadata.json` of one pack, including its `Layer` (`Below` or `Above` the cursor) and `trailSeconds` |
| `PointerShaderRegistry` | `Registry<PointerPack>` and `MetadataPackLoader`: search paths, live reload, parameter translation, the `p_<id>` preamble |
| `PointerFrameState` | Plain-data snapshot of the tail for one frame, shared by both hosts |
| `PointerUniformExtension` | `IUniformExtension` that writes the tail after `BaseUniforms` on the Qt RHI preview runtime |
| `PointerHistory` | Ring-buffer pointer sampler. It answers `frameState`, `isLive` and `damageRect` |

## Use

The host feeds pointer and button events into `PointerHistory`, then reads one `PointerFrameState` for each frame.
Positions are in device px. Timestamps are milliseconds on one monotonic clock.

```cpp
#include <PhosphorPointer/PointerHistory.h>

using namespace PhosphorPointerShaders;

PointerHistory history;
history.setTrailSeconds(effect.trailSeconds);  // from the longest pack in the chain
history.notePointer(devicePos, nowMs);
history.noteButtons(buttonsNow, buttonsBefore, devicePos, nowMs);

if (history.isLive(nowMs, effect.trailSeconds)) {
    const PointerFrameState state = history.frameState(nowMs, scale);
    const QRectF damage = history.damageRect(reachPx, nowMs, effect.trailSeconds);
}
```

The app names its pack location. `PointerShaderRegistry::includePathsFor(packDir, packDataSubdir)` takes the subdir
relative to a `GenericDataLocation` directory. PlasmaZones passes `phosphor-works/shaders/pointer`.

## Limits

| Limit | Value | Constant |
|-------|-------|----------|
| Trail samples in the ring | 32 | `PointerShaderContract::kMaxTrailPoints` |
| Buffer passes for each pack (other families have 8) | 2 | `PointerShaderContract::kMaxBufferPasses` |
| User texture slots | 3 | `PointerShaderContract::kMaxUserTextureSlots` |
| Declared parameters | 48 | `PointerShaderContract::kMaxDeclaredParameters` |
| Minimum gap between ring samples | 8 ms | `PointerHistory::kMinSampleGapMs` |

## Tests

Some cases of `test_pointershaderregistry` and `test_pointercurve` check against the shared shader helper. That helper
ships with the packs of the app, not with this library. The cases run in the build of the app, which sets
`PHOSPHOR_LIBS_BUNDLED_PACKS_DIR`. In a build of this tree alone, they skip.

## Dependencies

- `Qt6::Core`, `Qt6::Gui`
- [`phosphor-registry`](../phosphor-registry/README.md), [`phosphor-fsloader`](../phosphor-fsloader/README.md) and
  [`phosphor-shaders`](../phosphor-shaders/README.md), all PUBLIC
