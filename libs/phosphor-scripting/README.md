<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team -->
<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->

# phosphor-scripting

phosphor-scripting is a generic, sandboxed [Luau](https://luau.org/) host. It supplies the VM lifecycle,
`luaL_sandbox`, a run-time watchdog, a heap cap for each engine, bytecode compile and load, and a
`QVariant`↔Lua marshalling layer. It has **no** knowledge of tiling or of any other domain.

## Responsibility

Phosphor runs scripts that users write. Today these are tiling algorithms. This library owns the **embedding** of
Luau, so that each domain binding can stay small. A binding marshals its parameters into `QVariant`, calls a script
and reads the result back. The library does not know what the scripts compute.

- **`LuauEngine`.** It owns one `lua_State`. Its public API uses only `QVariant`. `lua_State` is forward-declared and
  never crosses the shared-library boundary. The lifecycle has these steps, in this order:

  | Step | What it does |
  |------|--------------|
  | construct | Makes the engine |
  | `init` | Opens the Luau standard library (Luau ships no `io` or `package` library), connects the interrupt callback and installs the capped allocator |
  | `runPrelude` | Zero or more times. Installs host globals, for example a domain standard library |
  | `sandbox` | Freezes the globals and the standard library |
  | `loadModule` | Once for each script |
  | `callModule` / `moduleField` | Calls the script or reads a field of it |

- **`LuauWatchdog`.** A shared supervisor thread that limits the run time of a call. It measures wall time or the
  CPU time of the thread that runs the call (see [Choosing the clock](#choosing-the-clock)). It only sets a co-owned
  atomic flag, which the interrupt callback of the engine reads. It never touches the `lua_State`, so a late fire
  during teardown is safe. Many engines can share one watchdog.
- **Heap cap.** A custom `lua_Alloc` counts the live and peak bytes. It fails any allocation that goes over the cap.
  A runaway script then gets a Luau out-of-memory error that the host can catch, and the host does not crash. The
  default is 64 MiB, and each engine can set its own cap. The cap applies only during the `lua_pcall` of sandboxed
  script execution. Trusted init, preludes and the host-side QVariant marshalling touch the VM outside any
  protected call. Thus a false out-of-memory error never kills them with an abort that the host cannot catch.

## Key types

| Type | Purpose |
|------|---------|
| `PhosphorScripting::LuauEngine`   | The sandboxed VM, with an API that uses only `QVariant` (init / runPrelude / sandbox / loadModule / releaseModule / moduleField / hasFunction / callModule) |
| `PhosphorScripting::LuauWatchdog` | Shared deadline supervisor that stops runaway scripts through the interrupt callback |
| `PhosphorScripting::WatchdogClock` | The clock that a deadline runs on: `Wall` (the default) or `ThreadCpu` |

## Use

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

## Choosing the clock

`LuauEngine::setWatchdogClock` chooses the clock on which the watchdog measures the calls of the engine. It applies
to every call that starts after it, including the prelude and module chunks.

| Clock | The budget counts | Use it when |
|-------|-------------------|-------------|
| `WatchdogClock::Wall` (default) | Wall time from the start of the call, on the steady clock | The call must end by a time, whatever the machine does |
| `WatchdogClock::ThreadCpu` | CPU time of the thread that runs the call | The budget is for the work of the script, and its thread can wait for a CPU |

Luau code advances only while its thread runs. On a loaded machine (during a large compile, for example), a GUI
thread waits for a CPU while the wall clock continues. A call that does little work can then use all of a short wall
budget. With `ThreadCpu`, a call uses no budget while its thread is descheduled, sleeps or blocks in a host
function.

`ThreadCpu` reads the clock of the thread through `pthread_getcpuclockid` and `clock_gettime` (Linux and FreeBSD).
If either call fails, that call falls back to wall time. The watchdog cannot sleep until a CPU time. It sleeps for
the CPU budget that is left, because one thread cannot use CPU time faster than the wall clock runs. Then it reads
the clock again, at least 1 ms later. Thus the interrupt can be up to 1 ms of CPU time late. A direct user of
`LuauWatchdog::arm` with `ThreadCpu` arms on the thread that runs the script, and disarms before that thread exits.

## Bundled Luau

The build compiles Luau from source as part of this library. It uses the archive that
`cmake/PhosphorDependencies.cmake` pins by version, URL and SHA-256. A normal configure downloads the archive one
time with CPM into `.cache/cpm`. An offline build (every distribution package) passes
`-DPHOSPHOR_DEPS_ARCHIVES=<dir>` with the archive in that directory, and downloads nothing. To link a Luau from the
system, pass `-DPHOSPHOR_SYSTEM_LUAU=ON`.

The Luau libraries link **PRIVATE**, and the VM is built PIC. Only on the vendored targets, the build forces
`-fexceptions` on and turns unity builds off. Luau needs C++ unwinding for `lua_pcall`, and its file-local symbol
names collide in unity batches. These settings do not change the global build settings of the project.

## Design notes

- **Two safety nets on two axes.** The watchdog limits run *time*. The capped allocator limits *heap*. Both report a
  violation as an error that the host can catch, and the engine recovers for the next call.
- **`lua_State` is private.** Public headers forward-declare it and expose only `QVariant`. The Luau libraries are a
  PRIVATE link dependency, so no Luau symbols cross the `.so` boundary. Domain bindings marshal their own
  parameters.
- **Not thread-safe.** As with the VM below it, all calls on one engine must occur on the thread that owns it.

## Dependencies

- `QtCore`, `Threads`
- Luau: the archive that `cmake/PhosphorDependencies.cmake` pins, or a system Luau with `-DPHOSPHOR_SYSTEM_LUAU=ON`

## See also

- [`phosphor-tiles`](https://github.com/phosphor-works/phosphor-engine-libs/blob/main/libs/phosphor-tiles/README.md) in phosphor-engine-libs: the first consumer, with `LuauTileAlgorithm` and the `pluau` tiling standard library.
- [PlasmaZones ADR 0001: Luau as the shell scripting language](https://github.com/phosphor-works/PlasmaZones/blob/main/docs/architecture/adr/0001-luau-shell-scripting-language.md): why Phosphor uses Luau.
