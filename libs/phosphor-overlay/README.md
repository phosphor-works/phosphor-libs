<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team -->
<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->

# phosphor-overlay

phosphor-overlay hosts overlays with more than one slot on each screen. It supplies a layer-shell shell host for
each screen, the slot terms, and the connection of animator configurations for overlays with many slots.

## Responsibility

This is a helper layer on [`phosphor-layer`](../phosphor-layer/README.md),
[`phosphor-surfaces`](../phosphor-surfaces/README.md) and [`phosphor-animation`](../phosphor-animation/README.md).
The consumer supplies a surface factory. With it, `ShellHost` owns these parts:

- the lifecycle of the layer-shell shell on each screen (create, destroy, rekey)
- the slot map, keyed by slot names that the consumer chooses
- slot hides that the animator drives
- the registration of animator configurations for each role

The library knows nothing about zones, layouts or specific content. Consumers connect their slot terms through
callbacks.

## Key types

| Type | Purpose |
|------|---------|
| `PhosphorOverlay::ShellHost`             | Owns the `ShellState` map of the screens, the lifecycle methods and the animator wiring |
| `PhosphorOverlay::ShellState`            | Mechanism state for one screen: layer-shell surface, QQuickWindow, physical screen, slot map |
| `PhosphorOverlay::SlotEntry`             | One slot entry: `QPointer<QQuickItem> item` + `PhosphorLayer::Role role` |
| `PhosphorOverlay::makePerInstanceRole()` | Builds a Role for one instance. It appends `-{screenId}-{generation}` to a base scope prefix |

## Use

```cpp
using namespace PhosphorOverlay;

ShellHost host;
host.setSurfaceAnimator(&animator);                          // from phosphor-animation

host.setSurfaceFactory([&](const QString& sid, QScreen* s) {
    const auto role = makePerInstanceRole(MyShellBaseRole, sid, mgr.nextScopeGeneration());
    return mgr.createSurface(/* role, qml url, screen, ... */);
});

host.setPostCreateCallback([&](const QString& sid, ShellState& state) {
    // Look up content slot Items by their QML object names, wire QML
    // signals, prime the RHI pipeline. Insert one SlotEntry per slot:
    auto* item = qvariant_cast<QQuickItem*>(state.shellWindow()->property("osdSlotItem"));
    state.slots.insert(QStringLiteral("osd"), SlotEntry{item, MyOsdRole});
});

host.setPreDestroyCallback([&](const QString& sid) {
    // Drop any parallel consumer-side per-screen state so a stale signal
    // firing during teardown doesn't dereference a dangling pointer.
});

// Lifecycle:
host.registerConfigForRole(MyOsdRole, buildOsdConfig());
host.ensureShell(screenId, physScreen);
host.syncSurfaceState(screenId, {.visible = true, .inputGrabbing = false, .keyboardGrabbing = false});
host.hideSlot(screenId, QStringLiteral("osd"), [&]{ /* on hide-leg settle */ });
host.destroyShell(screenId);
```

## Design notes

- **Raw owning pointers give stable pointers.** Each `ShellState` entry is on the heap, and the host stores
  `QHash<QString, ShellState*>`. Consumers keep the `ShellState*` that `ensureShell` / `stateFor` returns in their
  own state for each screen. They rely on the pointer to stay valid when the hash grows.
  `QHash<QString, std::unique_ptr<ShellState>>` would move the pointee but still rehash the bucket array, so the
  references that consumers keep are stable only through the indirection. `std::shared_ptr` would add a reference
  count that the library does not need: there is one owner, the host, and any number of borrowed observers.
  `ShellState` is not copyable and not movable. This prevents an accidental `auto copy = host.stateFor(id)` (note
  the missing `&`), which would make a shallow copy of the mechanism pointers and the slot map.
- **Callbacks set the content boundary.** Three function-object hooks (`SurfaceFactory`, `PostCreateCallback`,
  `PreDestroyCallback`) carry every decision that is specific to the consumer:
  - the role and qmlSource of the surface
  - the slot QML object names to look up
  - the signal handlers to connect
  - the parallel content state to clear at teardown

  The library owns the surface lifecycle, and the consumer owns the content.
- **The slot role is fixed when the slot is connected.** Each `SlotEntry` holds the `PhosphorLayer::Role` that the
  slot animates as. Thus `hideSlot` can call `SurfaceAnimator::beginHide`, and the caller does not give the role
  again at each dismiss or cancel call site.
- **`syncSurfaceState` is only mechanism.** Consumers calculate `visible` and `inputGrabbing` from the visibility of
  their content slots (for example, a modal or non-modal classification). The library decides the surface mapping
  and the `Qt::WindowTransparentForInput` toggle. A third field, `keyboardGrabbing`, sets the keyboard interactivity
  of the layer surface to Exclusive or None at run time. Ask for it only for content that must read typed
  characters, because while it is on, the focused toplevel gets no keys. It applies to one screen, with no
  arbitration across screens. If a consumer moves such content to another screen, it must sync the screen that it
  left and the screen that it entered.
- **A creation failure on a screen stays.** When the surface factory returns nullptr for a screen, the host marks
  that screen. Later `ensureShell` calls for it return immediately until a call to `clearFailure(screenId)`,
  usually on hot-plug. `failureScreenIds()` gives the current set. Consumers with their own id grammar (for example,
  virtual-screen prefixes) can then clear by prefix, and the library does not need to know the grammar.

## Dependencies

- `QtCore`, `QtQuick`
- [`phosphor-layer`](../phosphor-layer/README.md): `Role`, `Surface` and the wlr-layer-shell wire primitives.
- [`phosphor-shell-patterns`](../phosphor-shell-patterns/README.md): the UI-pattern recipes from which the consumer
  makes base roles.
- [`phosphor-surfaces`](../phosphor-surfaces/README.md): managed surface lifecycle and the scope-generation counter.
- [`phosphor-animation`](../phosphor-animation/README.md): the `SurfaceAnimator` that drives each show and hide leg
  of an overlay.
- [`phosphor-screens`](../phosphor-screens/README.md): screen topology and stable identifiers. This is a dependency
  for the consumer, because the library itself only forward-declares `QScreen`.

## See also

- [`phosphor-layer`](../phosphor-layer/README.md): the lower-level surface and role primitives that this library
  coordinates.
- [`phosphor-shell-patterns`](../phosphor-shell-patterns/README.md): axis-2 UI-pattern recipes that consumers use
  to build their base roles.
