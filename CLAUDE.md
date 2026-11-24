<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->

# phosphor-libs

phosphor-libs is the core layer of the Phosphor libraries: LGPL Qt 6 and Wayland libraries for rendering, layer-shell
surfaces, shader packs, configuration, IPC, scripting and the foundations that window placement builds on.
PlasmaZones, Lambent and Afterglow build it from a pinned commit, and phosphor-engine-libs and phosphor-shell-libs
build on it. The placement engines are not here. They are in phosphor-engine-libs.

The rules every phosphor-works repository follows come from the workspace repository, imported below. This file adds
phosphor-libs' own. The shared Claude Code tooling (the command guard, `phosphor-works:build`, `phosphor-works:debug`,
`phosphor-works:code-audit`, the shared reviewers, the lesson index) comes from the `phosphor-works` plugin, which
`.claude/settings.json` enables and `workspace/tools/setup` installs.

@../workspace/claude/conventions.md

## Layout

- Each library is `libs/phosphor-<name>/` with its own `CMakeLists.txt`, `project()`, `include/`, `src/`, `tests/`
  and find_package config, so it also configures alone. The root `CMakeLists.txt` adds them in dependency order: a
  library that `find_package()`s a sibling comes after it, or the lookup finds a stale installed copy.
- Headers are `<PhosphorX/...>`, the target `PhosphorX::PhosphorX`, the export macro `PHOSPHORX_EXPORT` (from
  `generate_export_header`) and the namespace usually `PhosphorX`. `phosphor-identity` is header-only (INTERFACE) with
  no export macro. `phosphor-screens` has three targets (`PhosphorScreens::Core`, `::Runtime`, `::PhosphorScreens`)
  with their own macros. The namespaces of `phosphor-layout-api`, `phosphor-surface` and `phosphor-pointer` are
  `PhosphorLayout`, `PhosphorSurfaceShaders` and `PhosphorPointerShaders`.
- QML modules: `org.phosphor.animation`, `org.phosphor.surface`, `org.phosphor.control`, `Phosphor.Ipc`.
- `data/schemas/` holds the JSON schemas of the shared pack contracts. PlasmaZones'
  `scripts/validate-json-schemas.py` reads them from the pinned commit.

## Layering

- This is the bottom layer. No library here links a phosphor-engine-libs, phosphor-shell-libs or app target, or
  names an app's data directories. The app passes each one in: the shader-preset subdir, the pointer-pack subdir to
  `PointerShaderRegistry::includePathsFor`, and the surface-pack subdir to
  `PhosphorSurfaceShaders::setSurfacePackDataSubdir` once at startup, before QML loads.
- phosphor-engine-libs fails to configure if an engine reaches `Qt6::Qml` or `Qt6::Quick` anywhere in its link
  closure. A new PUBLIC Qml link on a library the engines reach (`phosphor-registry`, `phosphor-layout-api`) breaks it.
- The suite folder name, `phosphor-works`, is defined once in `cmake/PhosphorSuite.cmake`.
- Everything here is `LGPL-2.1-or-later`, the tests and CMake modules included.

## How apps consume it

- PlasmaZones, Lambent, Afterglow, phosphor-engine-libs and phosphor-shell-libs pin one commit of this repository
  (archive URL and SHA-256) in their `cmake/<Project>Dependencies.cmake` and add it with CPM. When an app adds a
  library tree after phosphor-libs, the app's phosphor-libs pin is the one in use.
- After a change here merges, move each consumer's pin: VERSION, URL, FILE and SHA256 together, and the same archive
  in its nix or packaging recipe. Build a consumer against a local checkout with
  `-DCPM_phosphor-libs_SOURCE=<absolute path>`, or use the `workspace` superbuild to change a library and an app
  together.

## As a subproject

- Only when `PHOSPHOR_LIBS_TOP_LEVEL` is on does the tree set C++20, unity builds, `ENABLE_PCH`, ccache, ECM, the KDE
  settings, `BUILD_TESTING` (default `OFF`), `QT_NO_CAST_FROM_ASCII` and clang-format. As a subproject the app's
  settings are in force. The Qt lookups and `cmake/PhosphorDependencies.cmake` run in both cases.
- `PHOSPHOR_LIBS_CMAKE_DIR` names this tree's `cmake/`, so consumers reach the helpers by name: `PhosphorTestIsolation`
  (private D-Bus and XDG per test), `PhosphorLibTesting` (`phosphor_lib_enable_tests()`) and `PhosphorTestPch` (one
  PCH for all test targets, `phosphor_share_test_pch()`). Tests here include the isolation module by the relative
  path `../../../cmake/`, with a no-op stand-in when it is missing.
- `PHOSPHOR_ANIMATION_QUICK` is forced `ON`, because the `org.phosphor.animation` plugin needs `QtQuickClock`.
- With tests on, the tree fetches the pinned phosphor-surface-packs unless the app set `PHOSPHOR_SURFACE_PACKS_DIR`.
  `PHOSPHOR_LIBS_BUNDLED_PACKS_DIR` is empty here, so the animation and pointer tests that check an app's bundled packs
  skip. PlasmaZones sets it.

## Build and test

- `cmake --workflow --preset dev` configures into `build/`, builds and tests. The other presets are `release` and
  `relwithdebinfo`. Follow `phosphor-works:build` for the memory cap, jobs, the warning count and the non-unity gate.
- CI is the shared C++ CI of the workspace repository (`cpp-ci.yml`): the `dev` preset on Arch and on Fedora 44 (older
  Qt), with a warning failing the build, clang-format and qmlformat on every tracked file, and `nix flake check`.
  Tests run with `QT_QPA_PLATFORM=offscreen`.
- This repository has no pre-commit hooks. Run `clang-format -i` and `qmlformat -i` on the files you touch.

## Third-party pins and ABI

- `cmake/PhosphorDependencies.cmake` is the only place the pins live: Luau, jsoncons, CPM.cmake and, for the tests,
  phosphor-surface-packs. `nix/package.nix` repeats Luau and jsoncons. Offline builds use
  `-DPHOSPHOR_DEPS_ARCHIVES=<dir>`, filled by `cmake -DPHOSPHOR_DEPS_FETCH=<dir> -P cmake/PhosphorDependencies.cmake`.
- After a bump, run `PHOSPHOR_LIBS_DIR=<this checkout> scripts/check-conventions.py` in PlasmaZones. Its dep-pins
  rule names every recipe there that misses the new pin.
- Each shared library has `SOVERSION 0`, except `phosphor-channel`, whose soname carries `major.minor` below 1.0.
  There is no ABI check yet.

## Where things are

| Library | What it holds |
| --- | --- |
| `phosphor-animation` | Motion runtime, curves, the `org.phosphor.animation` QML module, animation shaders |
| `phosphor-audio` | Audio spectrum provider (CAVA backend) |
| `phosphor-channel` | Ordered byte frames between two processes with backpressure (shared-memory ring, stream), no Qt |
| `phosphor-compositor` | Compositor-plugin SDK (`ICompositorBridge`, shared types) |
| `phosphor-config` | Schema-driven config store, pluggable backends (JSON), migration runner |
| `phosphor-control` | Qt/QML/Kirigami settings-app framework (`org.phosphor.control`) |
| `phosphor-dbus` | Service-agnostic D-Bus client helpers |
| `phosphor-fsloader` | Watched-directory loader scaffolding with live reload |
| `phosphor-geometry` | Pure geometry math, Qt Core only |
| `phosphor-identity` | Composite window id (`appId\|uuid`) and app-id matching, header-only |
| `phosphor-ipc` | JSON-over-Unix-socket `IpcRouter`, `Phosphor.Ipc` QML type |
| `phosphor-layer` | wlr-layer-shell surface lifecycle |
| `phosphor-layout-api` | Shared layout contract (`LayoutPreview`, `ILayoutSource`, layout-file keys) |
| `phosphor-mcp` | Model Context Protocol server over stdio |
| `phosphor-overlay` | Per-screen layer-shell host and slot mechanism for overlays |
| `phosphor-paths` | Where every Phosphor app's files live (the `phosphor-works` folder, app dirs) |
| `phosphor-pointer` | Pointer-shader packs (contract, effect, registry, `PointerHistory`) |
| `phosphor-protocol` | D-Bus wire types and service constants |
| `phosphor-registry` | Generic `Registry<T>`, factory interfaces, plugin and metadata loaders |
| `phosphor-rendering` | Shader rendering through Qt RHI |
| `phosphor-rules` | Window and context rule engine (`MatchExpression`, `RuleEvaluator`) |
| `phosphor-screens` | Screen topology and virtual-screen subdivision |
| `phosphor-scripting` | Embedded Luau host (VM, sandbox, watchdog, marshalling) |
| `phosphor-service-idle` | ext-idle-notify-v1 idle monitoring and inhibition |
| `phosphor-shaders` | Shader-domain primitives (`BaseUniforms`, `IUniformExtension`, `ShaderRegistry`) |
| `phosphor-shell-patterns` | UI-pattern recipes on `PhosphorLayer::Role` |
| `phosphor-shortcuts` | Global shortcut backends (KGlobalAccel, portal, D-Bus trigger) and registry |
| `phosphor-surface` | Surface-shader packs (contract, effect, registry, pack location) |
| `phosphor-surface-quick` | Qt Quick consumer of surface packs (`SurfaceShaderItem`, `org.phosphor.surface`) |
| `phosphor-surfaces` | Managed layer-shell surface lifecycle (keep-alive, scopes) |
| `phosphor-wayland` | Layer-shell QPA plugin and `LayerSurface` |
