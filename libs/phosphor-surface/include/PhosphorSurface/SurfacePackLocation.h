// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <PhosphorSurface/phosphorsurface_export.h>

#include <QString>

namespace PhosphorSurfaceShaders {

/// Sets where the installed surface packs live, relative to each
/// QStandardPaths::GenericDataLocation directory (no leading or trailing
/// slash), for the whole process. The library has no default of its own: the
/// app names the directory its packs install to, normally the
/// PHOSPHOR_SURFACE_PACKS_DATA_SUBDIR that phosphor-surface-packs'
/// PhosphorSurfacePacks.cmake sets, passed in as a compile definition.
///
/// Call it once at startup, before QML loads and before any surface pack is
/// resolved. Two lookups read it: SurfaceShaderItem::surfaceIncludePaths(),
/// which a SurfaceShaderItem built from QML reaches with no C++ caller to hand
/// it a path, and the installed-copy fallback of
/// SurfaceShaderRegistry::resolveBuiltinBufferShader(). Packs reached through
/// SurfaceShaderRegistry::addSearchPaths() do not depend on it: their shared
/// helpers are found beside them.
///
/// Safe to call from any thread.
PHOSPHORSURFACE_EXPORT void setSurfacePackDataSubdir(const QString& subdir);

/// The subdirectory setSurfacePackDataSubdir() last set. Empty until an app
/// sets it, and while it is empty neither lookup above searches the XDG data
/// directories at all.
PHOSPHORSURFACE_EXPORT QString surfacePackDataSubdir();

} // namespace PhosphorSurfaceShaders
