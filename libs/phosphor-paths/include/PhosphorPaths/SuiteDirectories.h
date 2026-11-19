// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include "phosphorpaths_export.h"

#include <QString>

namespace PhosphorPaths {

/// Every Phosphor app's files live under one folder, `phosphor-works`, in each
/// XDG root. What the apps share sits in common trees directly beneath it
/// (`phosphor-works/layouts`, `phosphor-works/shaders/<family>`, ...); what
/// belongs to one app sits in that app's own folder (`phosphor-works/<app>`):
/// its config files, state, cache and the data only it reads.

/// The folder every Phosphor app's files live under, in each XDG root.
PHOSPHORPATHS_EXPORT QString suiteDirectoryName();

/// A tree every Phosphor app shares, relative to the XDG data roots:
/// `phosphor-works/<tree>`, the form QStandardPaths::locate takes.
PHOSPHORPATHS_EXPORT QString commonDataSubdir(const QString& tree);

/// The application's own folder name inside `phosphor-works` (PlasmaZones:
/// `plasmazones`). Set once, at the top of main(), before anything reads a
/// path. Nothing here supplies a default: reading it unset is a programming
/// error and aborts, rather than handing out a path every app shares. A name
/// that is not a single directory name aborts too.
PHOSPHORPATHS_EXPORT void setAppDirectoryName(const QString& name);
PHOSPHORPATHS_EXPORT QString appDirectoryName();

/// The application's own folder relative to an XDG root:
/// `phosphor-works/<app>`. Under the data roots, the form
/// QStandardPaths::locate takes for data only this app reads.
PHOSPHORPATHS_EXPORT QString appSubdir();

/// The application's writable config, state and cache directories:
/// `phosphor-works/<app>` under the generic config, state and cache locations.
/// One per app, whichever of its executables asks.
PHOSPHORPATHS_EXPORT QString appConfigDir();
PHOSPHORPATHS_EXPORT QString appStateDir();
PHOSPHORPATHS_EXPORT QString appCacheDir();

} // namespace PhosphorPaths
