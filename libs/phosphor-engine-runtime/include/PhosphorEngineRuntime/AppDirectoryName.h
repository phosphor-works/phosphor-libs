// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include "phosphorengineruntime_export.h"

#include <QString>

namespace PhosphorEngineRuntime {

/// Every Phosphor app's files live under one folder, `phosphor-works`, in each
/// XDG root. What the apps share sits in common trees directly beneath it
/// (`phosphor-works/layouts`, `phosphor-works/animations`, ...); what belongs
/// to one app sits in that app's own folder (`phosphor-works/<app>`): its
/// config files, state and cache.

/// The folder every Phosphor app's files live under, in each XDG root.
PHOSPHORENGINERUNTIME_EXPORT QString suiteDirectoryName();

/// A tree every Phosphor app shares, relative to the XDG data roots:
/// `phosphor-works/<tree>`, the form QStandardPaths::locate takes.
PHOSPHORENGINERUNTIME_EXPORT QString commonDataSubdir(const QString& tree);

/// The application's own folder name inside `phosphor-works` (PlasmaZones:
/// `plasmazones`). Set once, at the top of main(), before anything reads a
/// path; each host names its own (PlasmaZones' through initPlasmaZonesHost()).
/// Nothing here supplies a default. Reading it unset is a programming error
/// and aborts, rather than handing out a path that every app shares.
PHOSPHORENGINERUNTIME_EXPORT void setAppDirectoryName(const QString& name);
PHOSPHORENGINERUNTIME_EXPORT QString appDirectoryName();

/// The application's own folder relative to an XDG root: `phosphor-works/<app>`.
PHOSPHORENGINERUNTIME_EXPORT QString appSubdir();

/// The application's writable config, state and cache directories:
/// `phosphor-works/<app>` under the generic config, state and cache locations.
/// One per app, whichever of its executables asks.
PHOSPHORENGINERUNTIME_EXPORT QString appConfigDir();
PHOSPHORENGINERUNTIME_EXPORT QString appStateDir();
PHOSPHORENGINERUNTIME_EXPORT QString appCacheDir();

} // namespace PhosphorEngineRuntime
