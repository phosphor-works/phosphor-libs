<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team -->
<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->

# Changelog

This file records each release of phosphor-libs, the shared Qt and C++ libraries of the suite.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). The versions follow
[Semantic Versioning](https://semver.org/spec/v2.0.0.html), as the phosphor-works conventions say.

## [Unreleased]

### Added

- phosphor-ipc: `IpcRouter::defaultSocketPath(appName)`, `socketEnvironmentVariable(appName)` and
  `socketPathForApp(appName)` give each app its own socket, `$XDG_RUNTIME_DIR/<app>.sock`, which the variable
  `<APP>_SOCKET` overrides. (phosphor-works/workspace#76)
- phosphor-ipc: the router closes a connection from a process of another user at accept, with a warning, and emits
  `connectionRefused()`. (phosphor-works/workspace#36)
- An app can embed the libraries: with `PHOSPHOR_EMBED_APP` set to its name before it adds the tree, and its programs
  and plugins named with `phosphor_install_embedded(FOR ...)`, the install holds only the shared libraries that those
  link, with the Qt plugins, licence notices and user data of their directories, in `<libdir>/phosphor-works/<app>`,
  `share/licenses/<app>` and `share/phosphor-works/<app>`, with `$ORIGIN` run paths. `phosphor_embedded_dirs()` gives the app both directories, and
  `PhosphorWayland::registerLayerShellPlugin()` finds the plugin beside the libraries. Without it the install is the
  developer install, as before. Every library installs through `phosphor_install_targets`, `phosphor_install_devel`,
  `phosphor_install_qt_plugin` and `phosphor_install_notice` in `cmake/PhosphorSuite.cmake`, which PhosphorPaths'
  CMake package now installs.
- `phosphor-scripting` installs Lua's MIT notice (`LICENSE.Lua`) beside Luau's: Luau derives from Lua and carries it.
- `PHOSPHOR_LIBS_QT=OFF` configures the tree without Qt and builds only `phosphor-channel`, for a program with no Qt.
- `phosphor-wayland` installs the licence notices of the Wayland protocols it compiles in, as
  `share/licenses/phosphor-libs/LICENSE.wayland-protocols`: each protocol's notice under its name and SPDX identifier
  (MIT or HPND-sell-variant), read from the vendored XML at configure time. A protocol with no notice, or with another
  licence, stops the configure. (phosphor-works/PlasmaZones#1193)

### Removed

- `cmake/PhosphorLibTesting.cmake` (`phosphor_lib_enable_tests()`). Each library gates its tests on
  `PHOSPHOR_LIBS_TESTS`.
- `PHOSPHOR_LIBS_BUNDLED_PACKS_DIR` and the animation and pointer test cases that read an app's bundled packs.
  PlasmaZones checks its packs against the library constants in its own tests.
- `PhosphorProtocol::Service::Apps`, the single-instance identities of PlasmaZones' settings app (Facet) and layout
  editor. They belong to those programs: PlasmaZones keeps them as `PlasmaZones::AppIdentity`.

### Changed

- **Breaking:** phosphor-ipc has no default socket. `IpcRouter::start()` needs a path and fails with an empty one,
  and `IpcRouter::defaultSocketPath()` takes the app name. The name `phosphor.sock` belongs to the Phosphor compositor.
  (phosphor-works/workspace#76)
- The libraries' tests build only in this repository's own build, or in a library configured on its own. An app or
  the workspace superbuild that adds the tree with `BUILD_TESTING` on no longer builds them.
- `PhosphorWayland::ClipboardDevice` uses `ext-data-control-v1` when the compositor has it, and the older
  `wlr-data-control-unstable-v1` only when it does not. KWin advertises only the ext protocol, so clipboard history
  now works there.

### Fixed

- The phosphor-screens libraries use CMake's own run path in the build tree, not their installed `$ORIGIN` one, so
  their tests and the tests that load them also run in an app whose libraries build in separate directories.

## [0.1.0] - 2026-11-26

The first release. It has 32 libraries: `phosphor-animation`, `phosphor-audio`, `phosphor-channel`, `phosphor-compositor`, `phosphor-config`, `phosphor-control`, `phosphor-dbus`, `phosphor-fsloader`, `phosphor-geometry`, `phosphor-identity`, `phosphor-ipc`, `phosphor-layer`, `phosphor-layout-api`, `phosphor-mcp-ipc`, `phosphor-mcp`, `phosphor-overlay`, `phosphor-paths`, `phosphor-pointer`, `phosphor-protocol`, `phosphor-registry`, `phosphor-rendering`, `phosphor-rules`, `phosphor-screens`, `phosphor-scripting`, `phosphor-service-idle`, `phosphor-shaders`, `phosphor-shell-patterns`, `phosphor-shortcuts`, `phosphor-surface-quick`, `phosphor-surface`, `phosphor-surfaces`, `phosphor-wayland`. The tag `v0.1.0` is commit `5f7ce79f`, which PlasmaZones, Lambent and Afterglow pin.

[Unreleased]: https://github.com/phosphor-works/phosphor-libs/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/phosphor-works/phosphor-libs/releases/tag/v0.1.0
