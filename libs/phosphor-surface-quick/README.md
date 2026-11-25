<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team -->
<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->

# phosphor-surface-quick

phosphor-surface-quick renders surface-shader packs in a Qt Quick scene. It supplies the `SurfaceShaderItem` type and
the `SurfaceDecoration.qml` chain host in the QML module `org.phosphor.surface`.

## Responsibility

[`phosphor-surface`](../phosphor-surface/README.md) is the pack subsystem. It uses Qt Core and Qt Gui only, so that
the KWin effect can link it without Qt Quick. This library is the on-screen half for a Qt Quick scene. Its consumers
are the PlasmaZones daemon (OSD and popup decorations), the settings app (live decoration preview) and the
phosphor-shell binary (shell chrome).

## Key types

| Type | Purpose |
|------|---------|
| `PhosphorSurfaceQuick::SurfaceShaderItem` | A `PhosphorRendering::ShaderEffect` subclass that renders one surface pack over one live item. It uploads the surface UBO through `PhosphorSurfaceShaders::SurfaceUniformProfile` |
| `SurfaceDecoration.qml` | The chain host. It captures an anchor item, runs one `SurfaceShaderItem` for each resolved stage, and draws the result over the rect of the anchor |

`SurfaceShaderItem` has these surface properties. The host binds them for each stage.

| Property | Type | Meaning |
|----------|------|---------|
| `surfaceOpacity` | `qreal` | Host fade factor for the output, 0 to 1 |
| `surfaceScale` | `qreal` | Logical-to-device pixel scale (`uSurfaceScale`) |
| `surfaceFocused` | `bool` | Whether the window has focus (`uSurfaceFocused`) |
| `surfaceSize`, `surfaceFrameTopLeft`, `surfaceFrameSize` | `QSizeF`, `QPointF`, `QSizeF` | Surface and frame geometry in device px |
| `backdropScreenRect`, `backdropSurfaceRect` | `QRectF` | Wallpaper placement. An empty rect disables placement |

## Use

Before QML loads, set the pack location with `PhosphorSurfaceShaders::setSurfacePackDataSubdir()`.
`SurfaceShaderItem::surfaceIncludePaths()` reads it, and is empty until the app sets it.

```qml
import QtQuick
import org.phosphor.surface

SurfaceDecoration {
    contentItem: card          // the content root that holds the shader anchor
    decorationChain: chain     // stage maps from PhosphorSurfaceShaders::composeStageMap()
    surfaceFocused: true
}
```

An empty `decorationChain` makes the host inert, and the content draws normally. The anchor item must declare
`property bool shaderAnchor: true` as a literal, not as a binding.

## QML module

| CMake target | Kind | Use |
|--------------|------|-----|
| `PhosphorSurfaceQuick::PhosphorSurfaceQuick` | SHARED library | C++ consumers |
| `PhosphorSurfaceQuickQml` | STATIC QML module, `org.phosphor.surface` 1.0 | QML consumers in this tree. It cannot go into an export set |
| `PhosphorSurfaceQuickQmlShared` | SHARED QML plugin | Built and installed only when `PHOSPHOR_SURFACE_QUICK_QML_INSTALL` is on |

## Dependencies

- `Qt6::Core`, `Qt6::Gui`, `Qt6::Qml`, `Qt6::Quick` (PUBLIC), `Qt6::GuiPrivate` (PRIVATE)
- [`phosphor-rendering`](../phosphor-rendering/README.md) and [`phosphor-surface`](../phosphor-surface/README.md)
  (PUBLIC)
- [`phosphor-shaders`](../phosphor-shaders/README.md) (PRIVATE)
- CMake 3.19 or later
