// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <PhosphorWorkspaces/IActivityBackend.h>
#include <PhosphorWorkspaces/IVirtualDesktopBackend.h>
#include <PhosphorWorkspaces/Plasma/phosphorworkspacesplasma_export.h>

#include <memory>

/// The KDE Plasma session's backends for VirtualDesktopManager and
/// ActivityManager: KWin's virtual desktops on the session bus, and Plasma's
/// activities. A host running on Plasma hands these to the managers; a
/// compositor that owns its desktops implements the interfaces itself.
namespace PhosphorWorkspaces::Plasma {

/// KWin's virtual desktops (KWinVirtualDesktopBackend).
PHOSPHORWORKSPACESPLASMA_EXPORT std::unique_ptr<IVirtualDesktopBackend> createVirtualDesktopBackend();

/// Plasma's activities, over PlasmaActivities. Null when the library was
/// built without it, so the manager is then unavailable.
PHOSPHORWORKSPACESPLASMA_EXPORT std::unique_ptr<IActivityBackend> createActivityBackend();

} // namespace PhosphorWorkspaces::Plasma
