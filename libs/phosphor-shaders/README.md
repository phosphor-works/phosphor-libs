<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team -->
<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->

# phosphor-shaders

phosphor-shaders holds the shared shader-domain parts for Phosphor shader effects. Every consumer of
[`phosphor-rendering`](../phosphor-rendering/README.md) builds on them. These parts are the shader-effect registry,
the base UBO layout, the `IUniformExtension` contract, GLSL `#include` resolution and the abstract wallpaper
provider.

## Responsibility

The render node in `phosphor-rendering` is generic. It owns the QRhi pipeline and a UBO with no fixed shape.
`phosphor-shaders` supplies the *shape*:

- the std140 base UBO that every shader effect inherits
- the extension contract, which lets a consumer append application-specific data after the base region
- the metadata that makes a directory of shader files into a discoverable effect with parameters

These consumers use the library:

| Consumer | How |
|----------|-----|
| [`phosphor-rendering`](../phosphor-rendering/README.md) | Writes the base UBO and calls `IUniformExtension::write()` for the rest |
| [`phosphor-animation`](../phosphor-animation/README.md) | Its `AnimationShaderRegistry` uses the same strategy |
| The Phosphor overlay | Hosts `ShaderEffect` items in QML |

## Key types

| Type | Purpose |
|------|---------|
| `PhosphorShaders::BaseUniforms`           | std140 base UBO layout. A Shadertoy-compatible block and two `appField` ints for cheap state that the consumer defines |
| `PhosphorShaders::IUniformExtension`      | Contract to append custom uniform data after `BaseUniforms`. `extensionSize()` does not change during the life of the instance |
| `PhosphorShaders::CustomParams` / `CustomColors` | Canonical key formats (`customParams<N>_<x|y|z|w>`, `customColor<N>`) for the parameter sub-slots of each effect in `BaseUniforms` (namespaces in `CustomParamsKey.h`) |
| `PhosphorShaders::ShaderRegistry`         | Finds shader effects on search paths through metadata-pack scans. One instance for each process, no singleton |
| `PhosphorShaders::ShaderRegistry::ParameterInfo` | Parameter declaration: name, type, default, range, UBO uniform name |
| `PhosphorShaders::ShaderIncludeResolver`  | Expands `#include "path"` / `#include <path>`, with a depth limit |
| `PhosphorShaders::IWallpaperProvider`     | Abstract source for the image path of the active desktop wallpaper |
| `PhosphorShaders::createWallpaperProvider`| Factory that detects KDE / Hyprland / Sway / GNOME |
| `PhosphorShaders::buildParamPreamble`     | Makes the `#define` preamble of each effect, which maps the declared parameter ids to UBO lanes |
| `PhosphorShaders::spliceAfterVersion`     | Inserts generated GLSL directly after the `#version` line of a source |
| `PhosphorShaders::versionDirectiveEnd`    | Finds the `#version` directive of a source. It knows comments and the BOM, and every splice uses it |
| `PhosphorShaders::lineDirective`          | A `#line` re-anchor that numbers correctly under GLSL before 330 and from 330 |
| `PhosphorShaders::kwinCompositorSource`   | A pack stage as the KWin effect compiles it: with the compositor define block spliced in and the varying locations removed |
| `PhosphorShaders::kwinDefineBlock`        | The `PHOSPHOR_KWIN` define block, with its guard for desktop-only extensions |
| `PhosphorShaders::kwinTopDownTexcoordsDefine` | The define that the per-window path of KWin 6.8 adds for its top-down offscreen texcoords |
| `PhosphorShaders::ShaderStage` / `stripVaryingLocations` | Removes `layout(location)` from the varyings of a stage, because the GLSL ES 300 compile of KWin rejects it on Mesa |
| `PhosphorShaders::kwinGlesDialect` / `kwinDesktopDialect` | The top-level rewrite of KWin itself on a GLES and on a desktop context, for offline gates |
| `PhosphorShaders::BaseUniformProfile`     | Says which base-UBO regions a consumer uploads |
| `PhosphorShaders::IUboProfile`            | Contract for a bounded set of UBO upload regions (`kCapacity` of them) |
| `PhosphorShaders::ShaderEntryPoint`       | The entry point that the stage of a pack declares, so that the loader can wrap it correctly |

## Use

Set up a shader effect in QML with a registry for the process. Effects that sample the desktop background also need
a wallpaper provider:

```cpp
#include <PhosphorShaders/ShaderRegistry.h>
#include <PhosphorShaders/IWallpaperProvider.h>

using namespace PhosphorShaders;

ShaderRegistry registry;
registry.addSearchPath(QStringLiteral("/usr/share/myapp/shaders"));
registry.setUserPath(QStandardPaths::writableLocation(
    QStandardPaths::AppDataLocation) + QStringLiteral("/shaders"));
registry.refresh();

auto wallpaper = createWallpaperProvider();
QString path = wallpaper->wallpaperPath();   // "" if unsupported DE
```

Implement an `IUniformExtension` to append data for each zone to the UBO:

```cpp
class MyExtension : public PhosphorShaders::IUniformExtension {
public:
    int  extensionSize() const override { return sizeof(MyTail); }
    bool isDirty() const override       { return m_dirty.exchange(false); }
    void write(void* dst) override      { std::memcpy(dst, &m_tail, sizeof(MyTail)); }
private:
    MyTail m_tail;
    mutable std::atomic<bool> m_dirty{true};
};
```

## Design notes

- **No singleton in the library.** Each composition root owns a `ShaderRegistry` instance for its process and
  registers search paths explicitly. Tests construct a registry for each fixture, and downstream consumers do the
  same.
- **`extensionSize()` does not change during the life of the instance.** The render node sets the size of the UBO
  and the staging buffer one time, when `setUniformExtension()` installs an extension. It uses both again on each
  frame. To change the size, install a new extension instance. This makes the render node make the UBO again.
- **`appField0 / appField1` are always there.** They fill the std140 alignment slot between `iResolution` (vec2) and
  `iMouse` (vec4). Without them, the layout breaks. Use them for small state (≤2 ints) that changes frequently and
  must be in `BaseUniforms`, not in the extension region.
- **Reads and changes only on the GUI thread.** The shader map is a `Registry<ShaderPack>` in `ShaderRegistry`. The
  rescan builds it again on the GUI thread. The public lookup methods (`availableShaders`, `shader`, `shaderInfo`,
  `shaderUrl`) read it without synchronization. `searchPaths()` returns a snapshot by value, which is safe to give to
  worker threads (the shader-warming path).
- **It uses a `PhosphorRegistry::MetadataPackLoader`.** `phosphor-fsloader` manages the search paths.
  `ShaderRegistry` adds only the lookup API that is specific to shaders.

## Dependencies

- `QtCore`, `QtGui`
- [`phosphor-fsloader`](../phosphor-fsloader/README.md): schema validation and path guards, reached through the `MetadataPackLoader` of `phosphor-registry`
- [`phosphor-registry`](../phosphor-registry/README.md): `Registry<T>` and `MetadataPackLoader<T>`. The public headers of `ShaderRegistry` reach both, so this is a PUBLIC dependency

## See also

- [`phosphor-rendering`](../phosphor-rendering/README.md): `ShaderEffect` and `ShaderNodeRhi` use `BaseUniforms` and `IUniformExtension`.
- [`phosphor-animation`](../phosphor-animation/README.md): `AnimationShaderRegistry` is a parallel registry for transition effects.
