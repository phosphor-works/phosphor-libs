// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorWorkspaces/Plasma/Backends.h>

#include <PhosphorWorkspaces/Plasma/KWinVirtualDesktopBackend.h>

#ifdef HAVE_KACTIVITIES
#include "PlasmaActivityBackend.h"
#endif

namespace PhosphorWorkspaces::Plasma {

std::unique_ptr<IVirtualDesktopBackend> createVirtualDesktopBackend()
{
    return std::make_unique<KWinVirtualDesktopBackend>();
}

std::unique_ptr<IActivityBackend> createActivityBackend()
{
#ifdef HAVE_KACTIVITIES
    return std::make_unique<PlasmaActivityBackend>();
#else
    return nullptr;
#endif
}

} // namespace PhosphorWorkspaces::Plasma
