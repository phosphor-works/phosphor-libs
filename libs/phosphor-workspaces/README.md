<!-- SPDX-FileCopyrightText: 2026 fuddlesworth
     SPDX-License-Identifier: LGPL-2.1-or-later -->

# phosphor-workspaces

> Virtual desktop and activity tracking, plus the per-monitor
> dynamic-workspace model. The managers take their desktops and activities
> from backends; the KDE Plasma session's (KWin's virtual desktops over
> D-Bus, Plasma's activities over PlasmaActivities) are a separate library,
> `PhosphorWorkspaces::Plasma`.

## Responsibility

Tracks the active virtual desktop and current Activity, with change
signals on switch. It also owns the per-monitor dynamic-workspace model:
which desktops each monitor owns, in what order, and the state machine that
creates, destroys and moves desktops to keep that model true. It is
independent of the Phosphor daemon, so consumers that need workspace
awareness link this library directly.

The core library reaches no compositor. `VirtualDesktopManager` keeps every
rule about the desktops (validating a list, renumbering, the per-screen
desktops, one read in flight, retries) and asks an `IVirtualDesktopBackend`
only to read, write and report. `ActivityManager` does the same over an
`IActivityBackend`. A compositor that owns its desktops implements the two
interfaces itself.

## Key types

| Type | Purpose |
|------|---------|
| `VirtualDesktopManager` | The desktops: current desktop (global and per screen), count, rows, names, ids, and the renumbering a change causes |
| `IVirtualDesktopBackend` | Where the desktops come from: reads, the switch/create/remove/rename commands, and the compositor's change signals |
| `ActivityManager` | The current activity and the list, kept through an outage of the activity service |
| `IActivityBackend` | Where the activities come from |
| `WorkspaceMap` | Per-monitor workspace ownership: screen id to an ordered slice of desktop ids, with a JSON form the daemon persists as its state file |
| `WorkspaceReconciler` | Dynamic-workspace lifecycle state machine over a `WorkspaceMap`: an echo-safe ledger of the requests it sent the compositor, create-on-occupy, destroy-on-empty, hotplug migration and named workspaces |

`PhosphorWorkspaces::Plasma` (`<PhosphorWorkspaces/Plasma/Backends.h>`):

| Type | Purpose |
|------|---------|
| `KWinVirtualDesktopBackend` | KWin's org.kde.KWin.VirtualDesktopManager on the session bus, every call bounded at a second |
| `Plasma::createVirtualDesktopBackend()` | KWin's backend |
| `Plasma::createActivityBackend()` | Plasma's activities over PlasmaActivities, null when built without it |

## Typical use

```cpp
#include <PhosphorWorkspaces/ActivityManager.h>
#include <PhosphorWorkspaces/Plasma/Backends.h>
#include <PhosphorWorkspaces/VirtualDesktopManager.h>

using namespace PhosphorWorkspaces;

VirtualDesktopManager vdm(Plasma::createVirtualDesktopBackend());
vdm.init();
vdm.start();
connect(&vdm, &VirtualDesktopManager::currentDesktopChanged, [](int desktop) {
    // React to desktop switch
});

ActivityManager activities(Plasma::createActivityBackend());
activities.init();
activities.start();
// activities.currentActivity(): empty without a backend or before the
// service named one
```

## Dependencies

- `QtCore`
- PhosphorEngine (`DesktopRemap`, which the reconciler uses to follow the
  renumbering a desktop removal causes)
- PhosphorIdentity (private, virtual-screen id parsing)

`PhosphorWorkspaces::Plasma` adds `QtDBus` and, optionally, PlasmaActivities
(KF6Activities on older distributions), detected at CMake time. Without it
`Plasma::createActivityBackend()` answers null, so `ActivityManager` is
unavailable and activity queries return empty results, while virtual desktop
tracking works regardless.
