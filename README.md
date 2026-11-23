# phosphor-libs

LGPL Qt 6 and Wayland libraries for rendering, layer-shell surfaces, desktop shells and the foundations window
placement builds on. They are the shared code of the [phosphor-works](https://github.com/phosphor-works) apps:
PlasmaZones, Afterglow and the Phosphor compositor and shell. The placement engines themselves (zones, tiles,
workspaces, the snap, tile and scroll engines and the engine runtime) are in
[phosphor-engine-libs](https://github.com/phosphor-works/phosphor-engine-libs), which builds on this repository.

| Area | Libraries |
| --- | --- |
| Placement foundations | `phosphor-layout-api`, `phosphor-geometry`, `phosphor-screens`, `phosphor-rules`, `phosphor-identity` |
| Rendering and surfaces | `phosphor-rendering`, `phosphor-shaders`, `phosphor-animation`, `phosphor-surface`, `phosphor-surface-quick`, `phosphor-pointer`, `phosphor-layer`, `phosphor-overlay`, `phosphor-surfaces`, `phosphor-shell-patterns`, `phosphor-wayland` |
| Infrastructure | `phosphor-paths`, `phosphor-config`, `phosphor-control`, `phosphor-registry`, `phosphor-fsloader`, `phosphor-ipc`, `phosphor-mcp`, `phosphor-channel`, `phosphor-dbus`, `phosphor-protocol`, `phosphor-scripting`, `phosphor-shortcuts`, `phosphor-service-idle`, `phosphor-audio`, `phosphor-compositor` |

Each library lives in `libs/<name>/` with its own `CMakeLists.txt`, headers, sources and tests.

## Build

Needs CMake 3.23 (3.25 for the presets below, and for Qt 6.12, whose CMake packages ask for it), Qt 6.10 with its
private headers (Gui, Quick, ShaderTools, WaylandClient), extra-cmake-modules and Wayland, plus the Vulkan headers
when Qt was built with Vulkan support, as most distributions build it. The first configure downloads the pinned Luau
and jsoncons archives (`cmake/PhosphorDependencies.cmake`) into `.cache/cpm`, and with the tests on, the pinned
[phosphor-surface-packs](https://github.com/phosphor-works/phosphor-surface-packs) commit the surface tests load.

```sh
cmake --workflow --preset dev
```

That configures into `build/`, builds and runs the tests.

## Use from an app

An app adds this repository as a CMake subproject with [CPM](https://github.com/cpm-cmake/CPM.cmake), pinned to one
commit by its archive URL and SHA-256:

```cmake
CPMAddPackage(NAME phosphor-libs
              URL "https://github.com/phosphor-works/phosphor-libs/archive/<commit>.tar.gz"
              URL_HASH SHA256=<sha256>)
```

Its `find_package` and `target_link_libraries` calls then resolve to the targets here. To build the app against a
local checkout instead, configure it with `-DCPM_phosphor-libs_SOURCE=/path/to/phosphor-libs`.

The libraries name no app's data directories. The app passes each one in: the shader-preset subdirectory to the
classes and functions that read it, the pointer-pack subdirectory to
`PointerShaderRegistry::includePathsFor`, and, once at startup before any QML loads, the surface-pack subdirectory to
`PhosphorSurfaceShaders::setSurfacePackDataSubdir` (`PhosphorSurface/SurfacePackLocation.h`).

## License

LGPL-2.1-or-later. See `COPYING.LESSER`.
