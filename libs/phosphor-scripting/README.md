<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team
     SPDX-License-Identifier: LGPL-2.1-or-later -->

# phosphor-scripting

> A generic, sandboxed [Luau](https://luau.org/) host: VM lifecycle,
> `luaL_sandbox`, a run-time watchdog, a per-engine heap cap, bytecode
> compile/load, and a `QVariant`↔Lua marshalling layer, with **no** knowledge
> of tiling or any other domain.

## Responsibility

Phosphor runs user-authored scripts (tiling algorithms today, other
shell surfaces later). This library owns the **embedding** of Luau so each domain
binding can stay small. A binding marshals its params into `QVariant`, calls a
script, and reads the result back. It deliberately knows nothing about what the
scripts compute.

- **`LuauEngine`.** Owns one `lua_State`. Lifecycle: construct → `init`
  (open Luau's stdlib, which ships no `io` or `package` library, wire the interrupt callback, install the capped
  allocator) → `runPrelude` (zero or more, to install host globals such as a
  domain standard library) → `sandbox` (freeze globals + stdlib) → `loadModule`
  (per script) → `callModule` / `moduleField`. The public surface is entirely
  `QVariant`-based, and `lua_State` is forward-declared and never leaks across the
  shared-library boundary.
- **`LuauWatchdog`.** A shared supervisor thread that bounds the run time of a
  call, on wall time or on the CPU time of the thread that runs it (see
  [Choosing the clock](#choosing-the-clock)): it only
  ever flips a co-owned atomic flag the engine's interrupt callback reads, and
  never touches the `lua_State`, so a late fire during teardown is safe. One
  watchdog can be shared across many engines.
- **Heap cap.** A custom `lua_Alloc` tracks live/peak bytes and fails any
  allocation that would cross the cap, turning a runaway script into a catchable
  Luau out-of-memory error rather than a host crash. Default 64 MiB, configurable
  per engine. Enforcement is armed only around the `lua_pcall` of sandboxed
  script execution, so trusted init/preludes and host-side QVariant marshalling
  (which touch the VM outside any protected call) can never be spuriously
  OOM-killed into an uncatchable abort.

## Key types

| Type | Purpose |
|------|---------|
| `PhosphorScripting::LuauEngine`   | The sandboxed VM, with a `QVariant`-only API (init / runPrelude / sandbox / loadModule / releaseModule / moduleField / hasFunction / callModule) |
| `PhosphorScripting::LuauWatchdog` | Shared deadline supervisor that aborts runaway scripts via the interrupt callback |
| `PhosphorScripting::WatchdogClock` | The clock a deadline runs on: `Wall` (the default) or `ThreadCpu` |

## Typical use

```cpp
#include <PhosphorScripting/LuauEngine.h>
#include <PhosphorScripting/LuauWatchdog.h>

using namespace PhosphorScripting;

auto watchdog = std::make_shared<LuauWatchdog>();
LuauEngine engine(watchdog);          // default 64 MiB heap cap
engine.setWatchdogClock(WatchdogClock::ThreadCpu);  // optional, Wall by default
engine.init();
engine.runPrelude(QStringLiteral("pluau"), preludeSource);  // install host globals
engine.sandbox();                     // freeze globals + stdlib

QString err;
const int mod = engine.loadModule(QStringLiteral("script"), source, &err);
const auto out = engine.callModule(mod, QStringLiteral("tile"), {ctx}, 100 /*ms*/);
if (out.status == LuauEngine::CallStatus::Ok) {
    // out.result is a QVariant marshalled from the script's return value.
}
```

## Bundled Luau

Luau is built from source as part of this library, from the archive
`cmake/PhosphorDependencies.cmake` pins by version, URL and SHA-256. A normal
configure downloads it once with CPM into `.cache/cpm`. An offline build (every
distro package) passes `-DPHOSPHOR_DEPS_ARCHIVES=<dir>` with the archive in it,
and nothing is downloaded. Pass `-DPHOSPHOR_SYSTEM_LUAU=ON` to link a
system-provided Luau instead. The Luau libraries are linked **PRIVATE** and the
VM is built PIC, with `-fexceptions` force-enabled and unity builds disabled on
the vendored targets only (Luau needs C++ unwinding for `lua_pcall`, and its
file-local symbol names collide under unity batching). This is independent of the
project's global build settings.

## Design notes

- **Two safety nets, two axes.** The watchdog bounds run *time*. The capped
  allocator bounds *heap*. Both surface a violation as a catchable error and let
  the engine recover for the next call.
- **`lua_State` is private.** Public headers forward-declare it and expose only
  `QVariant`. The Luau libraries are a PRIVATE link dependency, so no Luau
  symbols cross the `.so` boundary. Domain bindings marshal their own params.
- **Not thread-safe.** Like the underlying VM, all calls on one engine must
  occur on its owning thread.

## Choosing the clock

`LuauEngine::setWatchdogClock` chooses the clock that the watchdog measures the
engine's calls on. It applies to every call that starts after it, the prelude and
module chunks included.

| Clock | The budget counts | Use it when |
|-------|-------------------|-------------|
| `WatchdogClock::Wall` (default) | Wall time from the start of the call, on the steady clock | The call must end by a time, whatever the machine does |
| `WatchdogClock::ThreadCpu` | CPU time of the thread that runs the call | The budget is for the script's work, and its thread can wait for a CPU |

Luau code advances only while its thread runs. On a loaded machine (a large
compile, for example) a GUI thread waits for a CPU while the wall clock runs on,
and a call that does little work spends a short wall budget. With `ThreadCpu`, a
call spends no budget while its thread is descheduled, sleeps or blocks in a host
function.

`ThreadCpu` reads the thread's clock through `pthread_getcpuclockid` and
`clock_gettime` (Linux and FreeBSD). If either fails, that call falls back to
wall time. The watchdog cannot sleep until a CPU time, so it sleeps for the CPU
budget that is left (one thread spends CPU time no faster than the wall clock
runs) and reads the clock again, at least 1 ms later. The interrupt is therefore
up to 1 ms of CPU time late. A direct user of `LuauWatchdog::arm` with
`ThreadCpu` arms on the thread that runs the script, and disarms before that
thread exits.

## Dependencies

- `QtCore`, `Threads`
- Luau, the archive `cmake/PhosphorDependencies.cmake` pins, or a system Luau via `-DPHOSPHOR_SYSTEM_LUAU=ON`

## See also

- [`phosphor-tiles`](../phosphor-tiles/README.md) — the first consumer: `LuauTileAlgorithm` + the `pluau` tiling standard library.
- [PlasmaZones ADR 0001: Luau as the shell scripting language](https://github.com/phosphor-works/PlasmaZones/blob/main/docs/architecture/adr/0001-luau-shell-scripting-language.md) — why Luau.
