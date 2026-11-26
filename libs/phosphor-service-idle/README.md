<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team -->
<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->

# phosphor-service-idle

phosphor-service-idle is a Wayland idle-management service for Phosphor-based desktop shells. It watches the session
for inactivity through a multi-stage timeout ladder, and it inhibits idle on request.

## Responsibility

The library watches the session for inactivity through a configurable multi-stage timeout policy, and inhibits idle
on request. It is the policy layer over the raw Wayland idle clients in `phosphor-wayland`. It uses those clients and
does not bind the protocols itself. Thus its public API is a clean Qt/QML type, and no Wayland types come out of it.
It reports which idle stage is active. The shell decides what each stage does (dim, lock, display-off). The library
supplies no UI.

- It monitors inactivity as an ordered ladder of stages. Each stage fires after its own timeout
  (`ext-idle-notify-v1`, through `PhosphorWayland::IdleNotifier`).
- It counts references to idle inhibition. Callers can keep the session awake while a video plays or a long task
  runs.
- It is a mechanism, not a policy. The library ships no default stages.

Suspend and power coupling are in the session service (session / logind), not in this library.

## Key types

| Type          | Role                                                                                     |
|---------------|------------------------------------------------------------------------------------------|
| `IdleService` | The session idle host. Configure `stages` (`{ name, timeoutMs }` entries). Read the live `currentStage` / `idle`. Call `inhibit()` / `release(cookie)` to count inhibition references. It is a plain QML type that you can instantiate, not a singleton. |

`IdleService` has these properties and signals:

| Member | Kind | Meaning |
|--------|------|---------|
| `stages` | property | Configures the ladder |
| `currentStage` / `currentStageName` / `idle` | properties | The live position on the ladder |
| `idled(stage)` | signal | The ladder advanced to a stage |
| `resumed()` | signal | Activity reset the ladder to active |

## Use

C++ shell composition root:

```cpp
#include <PhosphorServiceIdle/QmlRegistration.h>

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    PhosphorServiceIdle::registerQmlTypes();
    // ... load shell.qml
}
```

QML idle policy (the shell maps each stage to an action):

```qml
import Phosphor.Service.Idle 1.0

IdleService {
    id: idle
    stages: [
        { "name": "dim",  "timeoutMs":  5 * 60 * 1000 },
        { "name": "lock", "timeoutMs": 10 * 60 * 1000 },
        { "name": "off",  "timeoutMs": 15 * 60 * 1000 }
    ]

    onIdled: (stage) => shellPolicy.enter(idle.currentStageName)
    onResumed: shellPolicy.wake()
}

// While a fullscreen video plays, keep the session awake:
//   const cookie = idle.inhibit()
//   ... later ...
//   idle.release(cookie)
```

## Command-line example

The `phosphor-shell-libs/examples/phosphor-service-idle-cli` demo is the worked example and the acceptance harness.
It registers stages and logs each stage with a timestamp when it fires on a live session:

```sh
# watch a 5s 'dim' and a 10s 'lock' stage; leave input idle to fire them
phosphor-service-idle-cli --stage dim:5 --stage lock:10
# hold an inhibition for the first 8s, then release it
phosphor-service-idle-cli --stage dim:5 --inhibit-for 8
```

## Tests

Five test binaries cover the deterministic behavior, with no compositor:

| Test | What it covers |
|------|----------------|
| Smoke harness | Registration idempotency, inert construction |
| Facade test | Stage round trip and sort, inhibition toggle |
| QML-engine load test | Loading the type in a QML engine |
| Stage state machine | Advance, reset, reconfigure, pause |
| Inhibition | The reference count |

## Design notes

- **It uses the foundation primitives.** The `ext-idle-notify-v1` and `zwp-idle-inhibit-v1` clients are already in
  `phosphor-wayland` (`IdleNotifier` / `IdleInhibitor`), which also vendors the protocol XML. This library links
  `phosphor-wayland` privately and builds the policy on top. It binds no protocols itself.
- **A monotonic stage ladder.** Stages sort by ascending timeout. The source of each stage fires in timeout order
  and advances `currentStage`. The first activity resets the full ladder to active. The shell reads the stage and
  decides the action.
- **Inhibition pauses monitoring.** `inhibit()` / `release(cookie)` count inhibition references. While a cookie is
  held, the ladder is disarmed, so the compositor sends no idle notifications, and the service reports active. This
  is the inhibition without a surface that a shell needs. It is different from the surface-bound
  `PhosphorWayland::IdleInhibitor`, which a QML window uses to keep its own output awake.
- **One host, no model.** The service is one `IdleService` host with the current state and signals, not a
  `QAbstractListModel`. `phosphor-service-polkit` (one active request) uses the same design.
- **Inert without the protocol.** When the compositor does not advertise `ext-idle-notify-v1`, `supported` is false
  and the service does nothing.

## Dependencies

- `phosphor-wayland` (private link). It supplies `IdleNotifier` / `IdleInhibitor` and the generated protocol code.
  There is no separate `wayland-protocols` dependency.
- Qt6 >= 6.10.0 (Core, Qml). The CLI also uses Qt6 Gui for `QGuiApplication`.
- For the live path, a compositor that advertises `ext-idle-notify-v1`. Without it, the library loads inert.
