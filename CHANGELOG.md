<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team -->
<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->

# Changelog

This file records each release of phosphor-libs, the shared Qt and C++ libraries of the suite.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). The versions follow
[Semantic Versioning](https://semver.org/spec/v2.0.0.html), as the phosphor-works conventions say.

## [Unreleased]

### Added

- `phosphor-wayland` installs the licence notices of the Wayland protocols it compiles in, as
  `share/licenses/phosphor-libs/LICENSE.wayland-protocols`: each protocol's notice under its name and SPDX identifier
  (MIT or HPND-sell-variant), read from the vendored XML at configure time. A protocol with no notice, or with another
  licence, stops the configure. `PHOSPHORWAYLAND_LICENSE_FILES` names the file for a project that builds the library
  in-tree and installs it on its own terms. (phosphor-works/PlasmaZones#1193)

### Removed

- `PhosphorProtocol::Service::Apps`, the single-instance identities of PlasmaZones' settings app (Facet) and layout
  editor. They belong to those programs: PlasmaZones keeps them as `PlasmaZones::AppIdentity`.

### Changed

- `PhosphorWayland::ClipboardDevice` uses `ext-data-control-v1` when the compositor has it, and the older
  `wlr-data-control-unstable-v1` only when it does not. KWin advertises only the ext protocol, so clipboard history
  now works there.

## [0.1.0] - 2026-11-26

The first release. It has 32 libraries: `phosphor-animation`, `phosphor-audio`, `phosphor-channel`, `phosphor-compositor`, `phosphor-config`, `phosphor-control`, `phosphor-dbus`, `phosphor-fsloader`, `phosphor-geometry`, `phosphor-identity`, `phosphor-ipc`, `phosphor-layer`, `phosphor-layout-api`, `phosphor-mcp-ipc`, `phosphor-mcp`, `phosphor-overlay`, `phosphor-paths`, `phosphor-pointer`, `phosphor-protocol`, `phosphor-registry`, `phosphor-rendering`, `phosphor-rules`, `phosphor-screens`, `phosphor-scripting`, `phosphor-service-idle`, `phosphor-shaders`, `phosphor-shell-patterns`, `phosphor-shortcuts`, `phosphor-surface-quick`, `phosphor-surface`, `phosphor-surfaces`, `phosphor-wayland`. The tag `v0.1.0` is commit `5f7ce79f`, which PlasmaZones, Lambent and Afterglow pin.

[Unreleased]: https://github.com/phosphor-works/phosphor-libs/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/phosphor-works/phosphor-libs/releases/tag/v0.1.0
