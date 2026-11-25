<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team -->
<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->

# phosphor-identity

phosphor-identity defines the stable, cross-process string formats for window IDs, screen IDs and virtual-screen IDs.
All IDs that go through the Phosphor stack use these formats.

## Responsibility

Wayland does not expose a reliable numeric window ID as X11 does. Each compositor identifies "which window" in a
different way. KWin scripts use `internalId`, the compositor-bridge D-Bus interface of the daemon carries string IDs
on the wire, and application QObjects own `QWindow` pointers.

`phosphor-identity` owns the wire formats for those identities. It is an INTERFACE library. Every helper is `inline`
in the public header, so all consumers link the same definitions without an extra `.so`.

It owns three identity formats:

- **`WindowId`**: composite `"appId|instanceId"` strings. Helpers build, parse and pattern-match the canonical form.
- **`ScreenId`**: a stable identifier from the EDID. It falls back to the connector name when no EDID is available.
  It is portable across compositors.
- **`VirtualScreenId`**: `"<physicalId>/vs:<index>"` for the user-defined sub-regions of a physical monitor.

## Key types

| Type | Purpose |
|------|---------|
| `PhosphorIdentity::WindowId`        | Helpers for the canonical `appId\|instanceId` window-id format |
| `PhosphorIdentity::ScreenId`        | EDID parsing and screen-id construction, with a cache shared across TUs |
| `PhosphorIdentity::VirtualScreenId` | `<physicalId>/vs:<index>` make, parse and detect helpers |

Each of these is a namespace of `inline` functions, not a class.

## Use

```cpp
#include <PhosphorIdentity/WindowId.h>
#include <PhosphorIdentity/VirtualScreenId.h>

using namespace PhosphorIdentity;

// Build the wire format from its parts
QString id = WindowId::buildCompositeId(QStringLiteral("firefox"),
                                        QStringLiteral("Navigator"));

// Parse back out
QString app  = WindowId::extractAppId(id);       // "firefox"
QString inst = WindowId::extractInstanceId(id);  // "Navigator"

// Virtual screens
QString vs = VirtualScreenId::make(physicalId, /*index*/ 1);
if (VirtualScreenId::isVirtual(screenId)) {
    QString phys = VirtualScreenId::extractPhysicalId(screenId);
}
```

## Design notes

- **INTERFACE library.** The library ships no `.so`. Every function is `inline` in the public header. By the
  language rule, all cross-process consumers share one definition, and no consumer links a third copy.
- **Header-only by design.** The C++17 rule for static variables in inline functions makes the function-local
  static caches in `ScreenId.h` unique across translation units.
- **The wire format owns the spelling.** Every consumer uses these helpers and does not write the format by hand. To
  change the format, change it here, one time.
- **Main thread only for `ScreenId`.** The sysfs cache has no synchronization, so call every `ScreenId` function
  from the GUI thread.

## Dependencies

- `QtCore` only. The library has no Phosphor dependencies. It is the foundation layer that everything else builds
  on.
