<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team -->
<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->

# Changelog

This file records each release of phosphor-libs, the shared Qt and C++ libraries of the suite.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). The versions follow
[Semantic Versioning](https://semver.org/spec/v2.0.0.html), as the phosphor-works conventions say.

## [Unreleased]

### Added

- A `PhosphorLibs` CMake package. `find_package(PhosphorLibs 0.1 COMPONENTS PhosphorGeometry ...)` finds the
  installed library packages as components, and sets `PHOSPHOR_LIBS_CMAKE_DIR` to the installed helper modules
  (`PhosphorSuite`, `PhosphorTestIsolation`, `PhosphorLibTesting`, `PhosphorTestPch` and the test session bus
  configuration), as an in-tree phosphor-libs does.
- `PhosphorLayer::Testing`: the header-only test doubles of phosphor-layer (`mocks/mocktransport.h`,
  `mocks/mockscreenprovider.h`, `mocks/testroles.h`), in the tree and in the installed package, for the tests of
  its consumers.

## [0.1.0] - 2026-11-26

The first release. It has 32 libraries: `phosphor-animation`, `phosphor-audio`, `phosphor-channel`, `phosphor-compositor`, `phosphor-config`, `phosphor-control`, `phosphor-dbus`, `phosphor-fsloader`, `phosphor-geometry`, `phosphor-identity`, `phosphor-ipc`, `phosphor-layer`, `phosphor-layout-api`, `phosphor-mcp-ipc`, `phosphor-mcp`, `phosphor-overlay`, `phosphor-paths`, `phosphor-pointer`, `phosphor-protocol`, `phosphor-registry`, `phosphor-rendering`, `phosphor-rules`, `phosphor-screens`, `phosphor-scripting`, `phosphor-service-idle`, `phosphor-shaders`, `phosphor-shell-patterns`, `phosphor-shortcuts`, `phosphor-surface-quick`, `phosphor-surface`, `phosphor-surfaces`, `phosphor-wayland`. The tag `v0.1.0` is commit `5f7ce79f`, which PlasmaZones, Lambent and Afterglow pin.

[Unreleased]: https://github.com/phosphor-works/phosphor-libs/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/phosphor-works/phosphor-libs/releases/tag/v0.1.0
