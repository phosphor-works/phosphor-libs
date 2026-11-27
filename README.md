<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team -->
<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->

# phosphor-libs

phosphor-libs is the shared LGPL Qt 6 and Wayland code of the [phosphor-works](https://github.com/phosphor-works)
apps: PlasmaZones, Afterglow and the Phosphor compositor and shell. Its libraries do rendering, layer-shell surfaces,
desktop shells and the foundations that window placement uses. The placement engines are in
[phosphor-engine-libs](https://github.com/phosphor-works/phosphor-engine-libs), which builds on this repository.
These engines are zones, tiles, workspaces, the snap, tile and scroll engines, and the engine runtime.

## Libraries

Each library is in `libs/<name>/`, with its own `CMakeLists.txt`, headers, sources and tests.

| Library | Area | Description |
| --- | --- | --- |
| [`phosphor-layout-api`](libs/phosphor-layout-api/README.md) | Placement foundations | Defines the interfaces and value types that describe a layout across library boundaries. |
| [`phosphor-geometry`](libs/phosphor-geometry/README.md) | Placement foundations | Holds pure geometry functions for zones and windows. |
| [`phosphor-screens`](libs/phosphor-screens/README.md) | Placement foundations | Tracks physical and virtual screen topology and panel reservations. |
| [`phosphor-rules`](libs/phosphor-rules/README.md) | Placement foundations | Is the window and context rule engine for Phosphor. |
| [`phosphor-identity`](libs/phosphor-identity/README.md) | Placement foundations | Defines the stable, cross-process string formats for window IDs, screen IDs and virtual-screen IDs. |
| [`phosphor-rendering`](libs/phosphor-rendering/README.md) | Rendering and surfaces | Runs per-frame shader passes inside a Qt Quick scene graph. |
| [`phosphor-shaders`](libs/phosphor-shaders/README.md) | Rendering and surfaces | Holds the shared shader-domain parts for Phosphor shader effects. |
| [`phosphor-animation`](libs/phosphor-animation/README.md) | Rendering and surfaces | Runs motion and shader transitions for Phosphor surfaces. |
| [`phosphor-surface`](libs/phosphor-surface/README.md) | Rendering and surfaces | Holds the surface-shader pack contract, metadata, registry and pack location for per-window decorations. |
| [`phosphor-surface-quick`](libs/phosphor-surface-quick/README.md) | Rendering and surfaces | Renders surface-shader packs in a Qt Quick scene. |
| [`phosphor-pointer`](libs/phosphor-pointer/README.md) | Rendering and surfaces | Holds the pointer-shader packs that decorate the mouse pointer with trails, halos, click ripples and sparks. |
| [`phosphor-layer`](libs/phosphor-layer/README.md) | Rendering and surfaces | Supplies the primitives for layer-shell surfaces. |
| [`phosphor-overlay`](libs/phosphor-overlay/README.md) | Rendering and surfaces | Hosts overlays with more than one slot on each screen. |
| [`phosphor-surfaces`](libs/phosphor-surfaces/README.md) | Rendering and surfaces | Manages layer-shell surfaces with synchronous QML loading and Vulkan wiring. |
| [`phosphor-shell-patterns`](libs/phosphor-shell-patterns/README.md) | Rendering and surfaces | Gives named UI-pattern recipes on top of `PhosphorLayer::Role`. |
| [`phosphor-wayland`](libs/phosphor-wayland/README.md) | Rendering and surfaces | Connects Qt windows to the Wayland layer-shell. |
| [`phosphor-paths`](libs/phosphor-paths/README.md) | Infrastructure | Names the folders where every Phosphor app keeps its files. |
| [`phosphor-config`](libs/phosphor-config/README.md) | Infrastructure | Stores app settings with schema defaults, validation and versioned migrations. |
| [`phosphor-control`](libs/phosphor-control/README.md) | Infrastructure | Is a Qt 6, QML and Kirigami framework for settings apps. |
| [`phosphor-registry`](libs/phosphor-registry/README.md) | Infrastructure | Is a generic registry of factories by name. |
| [`phosphor-fsloader`](libs/phosphor-fsloader/README.md) | Infrastructure | Is the generic base for registries that load their entries from directories of files. |
| [`phosphor-ipc`](libs/phosphor-ipc/README.md) | Infrastructure | Is a typed JSON channel over a Unix socket for Phosphor applications. |
| [`phosphor-mcp`](libs/phosphor-mcp/README.md) | Infrastructure | Is a Model Context Protocol server over stdio. |
| [`phosphor-mcp-ipc`](libs/phosphor-mcp-ipc/README.md) | Infrastructure | Offers the calls of an application's phosphor-ipc socket that a manifest lists as MCP tools. |
| [`phosphor-channel`](libs/phosphor-channel/README.md) | Infrastructure | Sends ordered frames of opaque bytes from one process to another, with backpressure. |
| [`phosphor-dbus`](libs/phosphor-dbus/README.md) | Infrastructure | Gives generic D-Bus client utilities for Qt applications. |
| [`phosphor-protocol`](libs/phosphor-protocol/README.md) | Infrastructure | Is the Phosphor D-Bus contract. |
| [`phosphor-scripting`](libs/phosphor-scripting/README.md) | Infrastructure | Is a generic, sandboxed Luau host. |
| [`phosphor-shortcuts`](libs/phosphor-shortcuts/README.md) | Infrastructure | Registers global shortcuts through one API over pluggable backends. |
| [`phosphor-service-idle`](libs/phosphor-service-idle/README.md) | Infrastructure | Is a Wayland idle-management service for Phosphor-based desktop shells. |
| [`phosphor-audio`](libs/phosphor-audio/README.md) | Infrastructure | Supplies an audio spectrum to audio-reactive shaders and overlays. |
| [`phosphor-compositor`](libs/phosphor-compositor/README.md) | Infrastructure | Is the SDK for compositor plugins of Phosphor placement. |

## Build

The build needs these tools and libraries:

| Item | Requirement |
| --- | --- |
| CMake | 3.23. The presets below and Qt 6.12 (its CMake packages ask for it) need 3.25. |
| Qt | 6.10, with its private headers (Gui, Quick, ShaderTools, WaylandClient) |
| extra-cmake-modules | 6.26.0 |
| Wayland | Required |
| Vulkan headers | Required only when Qt was built with Vulkan support. Most distributions build Qt that way. |

The first configure downloads the pinned Luau and jsoncons archives (`cmake/PhosphorDependencies.cmake`) into
`.cache/cpm`. If the tests are on, it also downloads the pinned
[phosphor-surface-packs](https://github.com/phosphor-works/phosphor-surface-packs) commit that the surface tests load.

To configure into `build/`, build and run the tests, enter:

```sh
cmake --workflow --preset dev
```

## Use from an app

An app adds this repository as a CMake subproject with [CPM](https://github.com/cpm-cmake/CPM.cmake). The app pins
one commit by its archive URL and SHA-256:

```cmake
CPMAddPackage(NAME phosphor-libs
              URL "https://github.com/phosphor-works/phosphor-libs/archive/<commit>.tar.gz"
              URL_HASH SHA256=<sha256>)
```

The app's `find_package` and `target_link_libraries` calls then resolve to the targets here. To build the app against
a local checkout, configure the app with `-DCPM_phosphor-libs_SOURCE=/path/to/phosphor-libs`.

An app ships its own private copy of the libraries. Before it adds the tree, it sets its own name:

```cmake
set(PHOSPHOR_EMBED_APP "<app>")
```

Where it installs each program or plugin, the app names it:

```cmake
phosphor_install_embedded(FOR <target>...)
```

The install then holds only the libraries that these targets link, directly or through other libraries. With each
library come the plugins, notices and data of its directory. All of it goes in the app's own directories:

| What | Where |
| --- | --- |
| The shared libraries, with an `$ORIGIN` run path to each other | `<libdir>/phosphor-works/<app>` |
| The Qt plugins, such as the Wayland shell integration | `<libdir>/phosphor-works/<app>/plugins/<type>` |
| The licence notices of the code compiled in (Luau, Lua, jsoncons, the Wayland protocols) | `share/licenses/<app>` |
| Data for the app's users, such as the type definitions for tiling scripts | `share/phosphor-works/<app>` |

The install rules are made at the end of the app's top-level directory, when every target exists. An app that sets
`PHOSPHOR_EMBED_APP` and names no target fails to configure.

Headers, CMake packages, the development symlinks, the shared QML modules and the developer programs are not
installed. Two apps therefore never install the same file, and each runs the commit it pins. After it adds the tree,
the app reads the two directories with `phosphor_embedded_dirs(LIBDIR <var> LICENSEDIR <var>)` and gives its own
executables and plugins a run path to the library directory. `PhosphorWayland::registerLayerShellPlugin()` finds the
plugin directory beside the libraries by itself. Without `PHOSPHOR_EMBED_APP`, the install is the developer install
of a library repository: the libdir, the headers, the CMake packages and Qt's plugin and QML directories. The functions
in `cmake/PhosphorSuite.cmake` (`phosphor_install_targets`, `_devel`, `_qt_plugin`, `_notice`, `_app_data`) decide this
for every library, and a new install rule goes through them.

A program built with no Qt sets `PHOSPHOR_LIBS_QT` to `OFF`: the tree then builds only the libraries that do not use
Qt (`phosphor-channel`).

## Contributing

Commit messages use [Conventional Commits](https://www.conventionalcommits.org/). The Git hooks in `lefthook.yml`
come from the [workspace](https://github.com/phosphor-works/workspace) repository. They format the staged files and
check the commit message. To set up the hooks, run `lefthook install`. The suite conventions are in
[`claude/conventions.md`](https://github.com/phosphor-works/workspace/blob/main/claude/conventions.md) of the
workspace repository.

## License

The libraries are licensed under LGPL-2.1-or-later. The license text is in `COPYING.LESSER`.
