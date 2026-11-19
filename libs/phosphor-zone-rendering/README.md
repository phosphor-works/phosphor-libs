# PhosphorZoneRendering

Zone overlays drawn with shader packs: `ZoneShaderItem`, the Qt Quick item that renders a zone-overlay pack over a
layout's zones through PhosphorRendering. The engine runtime's overlays draw with it, and so do a settings app's
previews and the pack tooling (`plasmazones-shader-render`, the pack validator).

## Responsibility

- Render one zone-overlay pack over a set of zones: the zone uniforms, the label texture, the audio spectrum and
  the pack's own parameters, through PhosphorRendering's shader effect.
- Assemble an entry-only pack (one that defines `pZone` or `pImage` instead of `main()`) the same way on the live
  load and on the warm bake, so the bake cache keys agree.
- Resolve the trusted overlay roots, the include paths `#include <common.glsl>` searches and a pack's vertex stage,
  in one place for both paths.

## Key types

| Type | Header | What it is |
| --- | --- | --- |
| `ZoneShaderItem` | `ZoneShaderItem.h` | The Qt Quick item, in the `org.phosphor.zonerendering` QML module |
| `ZoneShaderRenderer` | `qml/ZoneShaderRenderer.qml` | The QML wrapper the overlay and the previews share: one config object in, consistent bindings out |
| `assembleZoneEntrySource` | `ZoneEntryScaffold.h` | The entry scaffold for packs without their own `main()` |
| `expandShaderIncludePaths`, `resolveZoneVertexPath`, `warmShaderBakeCacheForPaths` | `ZoneShaderNodeRhi.h` | Include paths, the vertex stage and the warm bake |
| `ZoneLabelTextureBuilder` | `ZoneLabelTextureBuilder.h` | The zone-number label texture |

## Typical use

Link `PhosphorZoneRendering::PhosphorZoneRenderingQml` and `PhosphorZoneRenderingQmlplugin` (a static module, in
this tree only), import the plugin once, and use the types from QML:

```cpp
Q_IMPORT_QML_PLUGIN(org_phosphor_zonerenderingPlugin)
```

```qml
import org.phosphor.zonerendering

ZoneShaderRenderer {
    config: overlayConfig
}
```

## Design notes

- **One source of truth for include paths and the vertex stage.** `ZoneShaderItem` and the warm bake must compile
  against identical inputs or their bake-cache keys diverge, so neither derives them by hand.
- The overlay roots are the engine runtime's (`ConfigDefaults::overlaysSubdir()` under the host's app directory).
- **Logging** is under `phosphor.zonerendering`.

## Dependencies

Qt Core, Gui, Qml and Quick, PhosphorRendering (the shader effect and RHI node), PhosphorShaders (entry points),
PhosphorEngineRuntime (the overlay data paths) and PhosphorZones (zone JSON keys).
