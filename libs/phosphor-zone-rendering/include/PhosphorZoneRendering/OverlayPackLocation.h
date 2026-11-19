// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include "phosphorzonerendering_export.h"

#include <QString>

namespace PhosphorZoneRendering {

/// Sets where the installed zone-overlay packs live, relative to each
/// QStandardPaths::GenericDataLocation directory (no leading or trailing
/// slash), for the whole process. The library has no default of its own: the
/// app names the directory its overlay packs install to (the engine runtime's
/// is ConfigDefaults::overlaysSubdir(), under the host's app directory).
///
/// Call it once at startup, before QML loads and before a ZoneShaderItem is
/// built or a warm bake runs: trustedShaderRoots() reads it, and with it the
/// include paths and vertex stages both resolve against.
///
/// Safe to call from any thread.
PHOSPHORZONERENDERING_EXPORT void setOverlayPackDataSubdir(const QString& subdir);

/// The subdirectory setOverlayPackDataSubdir() last set. Empty until an app
/// sets it, and while it is empty no installed overlay root is trusted.
PHOSPHORZONERENDERING_EXPORT QString overlayPackDataSubdir();

} // namespace PhosphorZoneRendering
