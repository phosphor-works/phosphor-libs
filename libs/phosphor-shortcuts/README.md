<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team -->
<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->

# phosphor-shortcuts

phosphor-shortcuts registers global shortcuts through one API over pluggable backends. The backends are
KGlobalAccel (Plasma-native), the XDG Portal (`xdg-desktop-portal.GlobalShortcuts`) and an ad-hoc D-Bus fallback.

## Responsibility

A shortcut such as "Super+1 snaps to zone 1" must register a global shortcut from a userspace process. On Plasma,
this goes through KGlobalAccel. On generic Wayland compositors that support the portal, it goes through
`org.freedesktop.portal.GlobalShortcuts`. Compositors with neither have no standard. For them, the ad-hoc D-Bus
fallback lets a compositor script or the compositor dispatch the shortcut.

The consumer does not need to know which backend is active. `phosphor-shortcuts` supplies these parts:

- **A single registration interface** (`Registry::bind`). It takes a shortcut id, a default sequence, a description
  and a callback.
- **A pluggable backend** (`IBackend`). The application chooses it at startup, or the library selects it
  automatically from the services that are available.
- **A factory helper** (`createBackend(BackendHint)`). It builds the correct backend from a compositor or desktop
  hint.
- **Ad-hoc registrations** (`IAdhocRegistrar`), kept apart from "system shortcuts". System shortcuts are persistent
  and each user can configure them. Ad-hoc registrations are for transient UI, for example modal-capture dialogs.

## Key types

| Type | Purpose |
|------|---------|
| `PhosphorShortcuts::Registry`        | Front end: `bind()` a shortcut id and callback |
| `PhosphorShortcuts::IBackend`        | Abstract backend. Shipped implementations: KGlobalAccel, XDG-Portal, D-Bus |
| `PhosphorShortcutsIntegration::IAdhocRegistrar` | Short-lived registrations that skip persistent storage. The consumer implements it |
| `PhosphorShortcuts::createBackend`   | Selects the correct backend for the running environment |

## Use

`bind()` queues the shortcut. It takes effect after `flush()`.

```cpp
#include <PhosphorShortcuts/Registry.h>
#include <PhosphorShortcuts/Factory.h>

using namespace PhosphorShortcuts;

auto backend = createBackend();  // picks KGlobalAccel on Plasma
Registry registry(backend.get());

registry.bind(
    /*id*/          QStringLiteral("snap-to-quick-1"),
    /*defaultSeq*/  QKeySequence(QStringLiteral("Meta+1")),
    /*description*/ tr("Snap active window to Quick Layout 1"),
    /*callback*/    [] { engine.snapTo(1); },
    /*persistent*/  true
);
registry.flush();
```

To use a different backend, pass a different `IBackend`. The `Registry` API stays the same.

```cpp
auto portalBackend = createBackend(BackendHint::Portal);
Registry adhoc(portalBackend.get());
adhoc.bind(id, seq, desc, cb, /*persistent*/ false);
```

## Design notes

- **Callers do not include KGlobalAccel directly.** Callers never see the Plasma-specific API. Thus the whole suite
  compiles without KF6 when the library is built with the portal and D-Bus backends only.
- **`IAdhocRegistrar` is a separate interface.** Binds that must never appear in the user's persistent shortcut
  editor go through a different API. Thus the backend persists only the binds that ask for it.
- **`createBackend(BackendHint::Auto)`** probes at runtime. The first preference is KGlobalAccel (fastest, native
  Plasma integration), then the portal, and last D-Bus.

## Dependencies

- `QtCore`, `QtGui` (for `QKeySequence`), `QtDBus`
- **Optional**: `KF6::GlobalAccel` (Plasma backend), compiled in
  when `-DPHOSPHORSHORTCUTS_USE_KGLOBALACCEL=ON` (the default) and CMake finds KF6GlobalAccel. Without it, the
  library builds the Portal and DBusTrigger backends only.
