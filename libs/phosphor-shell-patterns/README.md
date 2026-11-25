<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team -->
<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->

# phosphor-shell-patterns

phosphor-shell-patterns gives named UI-pattern recipes on top of `PhosphorLayer::Role`. It is the axis-2 vocabulary
between the wlr-layer-shell wire primitives (axis 1, in phosphor-layer) and the application roles of a consumer
(axis 3, for example `PhosphorRoles` of Phosphor).

## Responsibility

A `Role` in [`phosphor-layer`](../phosphor-layer/README.md) is a bundle of wlr-layer-shell parameters: `Layer`,
`Anchors`, exclusive zone, keyboard interactivity and scope prefix. It does not know about any domain.

`phosphor-shell-patterns` gives a name to each *UI pattern* that a shell wants, as a `Role` value that is ready to
use. For example, a wallpaper covers the background, a panel is attached to an edge, a modal grabs the keyboard and
a toast shows in a corner. Consumers make their public roles from these named recipes. They do not derive the
layer, anchor and keyboard combination again each time.

## Key types

| Pattern | Signature | Purpose |
|---------|-----------|---------|
| `Wallpaper()`           | `const Role&` | Background layer, all anchors, exclusive zone 0, kbd None |
| `Hud()`                 | `const Role&` | Overlay layer, all anchors, click-through, no exclusive zone |
| `Modal()`               | `const Role&` | Top layer, no anchors (the compositor centers it), exclusive kbd grab |
| `Floating()`            | `const Role&` | Overlay layer, no anchors, no kbd. The consumer sets the position with margins. |
| `Panel(Edge)`           | `Role`        | Top layer, anchored to the edge and its two neighbor edges, exclusive zone 0, kbd OnDemand |
| `Toast(Corner)`         | `Role`        | Top layer, corner-anchored, click-through, no exclusive zone |

`Edge` is `Top | Bottom | Left | Right`. `Corner` is `TopLeft |
TopRight | BottomLeft | BottomRight`.

A panel that must reserve space sets the size of its exclusive zone with `withExclusiveZone()` of
`PhosphorLayer::Role`.

The four fixed presets are accessor functions. Each returns a reference to a function-local static (Meyers
singleton). The first call constructs the Role lazily, and each subsequent call returns the same object. Thus a
consumer can use the presets in its static initializer, whatever the dynamic-init order across translation units or
shared libraries.

The parameterized `Panel` and `Toast` functions return a new `Role` for each call. The scope prefix of that role
encodes the edge or corner variant.

## Use

```cpp
#include <PhosphorShellPatterns/Patterns.h>
#include <PhosphorLayer/Role.h>

namespace PSP = PhosphorShellPatterns;

// Compose an app-specific role from a pattern plus a scope prefix
// that identifies the consumer:
inline const PhosphorLayer::Role MyAppOverlay =
    PSP::Hud().withScopePrefix(QStringLiteral("myapp-overlay"));

// Panels and toasts take a variation parameter:
inline const PhosphorLayer::Role MyAppLeftRail =
    PSP::Panel(PSP::Edge::Left).withScopePrefix(QStringLiteral("myapp-left-rail"));

inline const PhosphorLayer::Role MyAppToast =
    PSP::Toast(PSP::Corner::BottomRight).withScopePrefix(QStringLiteral("myapp-toast"));
```

## Design notes

- **Three separate axes.** The protocol is axis 1 (on `Role` in `phosphor-layer`). The UI pattern is axis 2 (this
  library). The app role is axis 3 (in the consumer). A consumer that overrides one axis changes only that axis.
  For example, it can use `Wallpaper` instead of `Hud` for one consumer role, and keep every other field.
- **The vocabulary is open.** A new pattern (for example `Card` for in-place transient surfaces) costs one entry
  here and no change to `phosphor-layer`. The library holds the recipes that consumers use again, not a complete
  set.
- **No shell-specific words.** "Panel" / "Toast" / "Modal" / "Hud" tell what a surface *is*, not which shell
  rendered it. The library does not use words such as "popup" or "notification", because they come from the
  vocabulary of specific shells.
- **Each pattern variation has its own scope prefix.** Each preset and each factory output carries a different scope
  prefix. Thus compositors can put surfaces in separate namespaces.

## Dependencies

- `QtCore`, `QtGui` (for `QMargins`)
- [`phosphor-layer`](../phosphor-layer/README.md): the `Role` struct that the patterns produce.

## See also

- [`phosphor-layer`](../phosphor-layer/README.md): the wlr-layer-shell primitives (axis 1) that this library
  composes.
