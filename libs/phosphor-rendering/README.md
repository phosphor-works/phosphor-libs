<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team -->
<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->

# phosphor-rendering

phosphor-rendering runs per-frame shader passes inside a Qt Quick scene graph. It supplies the `ShaderEffect` /
`ShaderNodeRhi` / `ShaderCompiler` infrastructure, plus the zone-aware UBO extension that the overlay uses.

## Responsibility

The built-in `ShaderEffect` of Qt Quick does not support multipass, render targets for each pass, custom UBO
layouts or shader-file `#include`. This library replaces it with three parts that work together:

- **`ShaderEffect`** is a `QQuickItem` subclass that owns one render node. It exposes the shader source and the
  parameters as `Q_PROPERTY`s. It gives uniform packing to a pluggable `IUniformExtension` (declared in
  [`phosphor-shaders`](../phosphor-shaders/README.md)).
- **`ShaderNodeRhi`** is the scene-graph render node. It owns the QRhi pipeline, the vertex and index buffers, the
  uniform buffer object (UBO), the texture bindings and the targets of each pass. It supports multipass with
  ping-pong buffers. It supports input-channel textures (`iChannel0..7`, as in Shadertoy, with
  `iChannelResolution` declared for the first four) and writeable depth attachments.
- **`ShaderCompiler`** compiles GLSL to SPIR-V at runtime with `Qt6::ShaderToolsPrivate`. Its output goes into the
  shader pipeline of Qt. It caches compiled modules by source hash in memory, and in a content-addressed cache on
  disk. Thus the editor does not compile an unchanged shader again.

A second pair of types specializes the base node for the **zone-overlay** case:

- `ZoneShaderNodeRhi` adds a labels texture binding and the zone counts in the base UBO.
- `ZoneUniformExtension` writes into the UBO tail the zone rectangles, the fill and border colors, the parameters
  of each zone, and the logical-to-device scale for the lengths among them (corner radius and border width).

The zone types are optional. Shader effects without zones use `ShaderEffect` + `ShaderNodeRhi` directly.

## Key types

| Type | Purpose |
|------|---------|
| `PhosphorRendering::ShaderEffect`         | The QQuickItem you instantiate in QML |
| `PhosphorRendering::ShaderNodeRhi`        | The QRhi-backed scene-graph node it owns |
| `PhosphorRendering::ShaderCompiler`       | GLSL to SPIR-V pipeline with on-disk cache |
| `PhosphorRendering::ZoneShaderNodeRhi`    | Zone-aware subclass: labels texture + zone counts in `BaseUniforms::appField0/1` |
| `PhosphorRendering::ZoneShaderUniforms`   | The GLSL-matching UBO struct, with `MaxZones` and the offset asserts, in `ZoneShaderCommon.h` |
| `PhosphorRendering::ZoneUniformExtension` | `IUniformExtension` writing zone rects / colors / params / scale |

## Use

This library does not ship a QML module. A consumer registers `ShaderEffect` (and any subclass) with
`qmlRegisterType()` under its own module URI. Then it instantiates the type in QML:

```cpp
qmlRegisterType<PhosphorRendering::ShaderEffect>("MyApp.Shaders", 1, 0, "ShaderEffect");
```

```qml
import MyApp.Shaders 1.0

ShaderEffect {
    anchors.fill: parent
    shaderSource: "qrc:/shaders/neon-city/effect.frag"
    bufferShaderPaths: [
        "qrc:/shaders/neon-city/buffer.frag"
    ]

    // Numbered vec4 / color slots packed into the UBO (customParams1..8,
    // customColor1..16):
    customParams1: Qt.vector4d(0.7, 1.0, 0.35, 0.0)
    customColor1: "#3B82F6"
    customColor2: "#A855F7"
}
```

On the C++ side, the `setUniformExtension()` method attaches an `IUniformExtension` such as
`ZoneUniformExtension`. The extension writes the data of each zone into the UBO tail.

## Design notes

- **The item owns no GPU texture.** `ShaderNodeRhi` owns every `QRhiTexture` and does all uploads on the render
  thread. This prevents the GPU-resource lifetime bugs that are frequent in simple `ShaderEffect`
  reimplementations. The item does own decoded CPU-side images. `setShaderParams` calls `loadUserTextureFile`
  synchronously on the CALLING thread and keeps the results. The wallpaper `QImage` is on the item, behind its own
  mutex.
- **Multipass is opt-in.** Single-pass shaders do not pay for the additional framebuffers. A non-empty
  `bufferShaderPaths` list enables the multipass path.
- **No direct GL calls.** All calls go through QRhi, so the same code runs on the OpenGL, Vulkan and Metal
  backends. Shaders use Vulkan-flavor GLSL 450.
- **The UBO is `BaseUniforms` + extension.** The base layout from
  [`phosphor-shaders`](../phosphor-shaders/README.md) is compatible with Shadertoy. Consumers attach an
  `IUniformExtension` to append application-specific data. Zone rendering uses `ZoneUniformExtension`. Animations
  use a different extension, declared in [`phosphor-animation`](../phosphor-animation/README.md).

## Dependencies

- `QtCore`, `QtGui`, `QtQuick`, `QtSvg`
- [`phosphor-shaders`](../phosphor-shaders/README.md): `BaseUniforms`, `IUniformExtension`
- `Qt6::ShaderToolsPrivate` for runtime compilation

## See also

- [`phosphor-shaders`](../phosphor-shaders/README.md): `ShaderRegistry`, parameter metadata, wallpaper provider,
  include resolver.
- [`phosphor-animation`](../phosphor-animation/README.md): the animation-shader registry on top of the render node
  of this library.
