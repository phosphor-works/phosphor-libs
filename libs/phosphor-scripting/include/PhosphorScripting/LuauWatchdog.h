// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include "phosphorscripting_export.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <time.h>

namespace PhosphorScripting {

/**
 * @brief The clock that a @ref LuauWatchdog deadline runs on.
 *
 * - `Wall` (the default): the steady clock. The deadline passes @p timeoutMs
 *   after @ref LuauWatchdog::arm, whatever the armed thread did in that time.
 * - `ThreadCpu`: the CPU time of the thread that called @ref LuauWatchdog::arm.
 *   Luau code advances only while its thread runs, so this counts the work of
 *   the call and not the time the scheduler gave to other programs. A call whose
 *   thread is descheduled, sleeps or blocks in a host function spends no budget
 *   while it waits. If `pthread_getcpuclockid` or `clock_gettime` fails for the
 *   thread, that call falls back to wall time.
 *
 * A host whose scripts run on a busy GUI thread with a short budget wants
 * `ThreadCpu`: under load, the wall clock runs on while the thread waits for a
 * CPU, and a call that does little work times out.
 */
enum class WatchdogClock : std::uint8_t {
    Wall,
    ThreadCpu,
};

/**
 * @brief One supervisor thread that interrupts runaway scripts across many engines.
 *
 * Each @ref LuauEngine registers a shared interrupt flag keyed by its own
 * address. On @ref arm the watchdog records a deadline on the chosen
 * @ref WatchdogClock; if the deadline passes before @ref disarm, the watchdog
 * sets `*flag = true`. The engine's interrupt callback (running on the VM
 * thread at the next safepoint) reads the flag and unwinds via `luaL_error`.
 *
 * Crucially the watchdog only ever writes a `std::shared_ptr<std::atomic<bool>>`
 * it co-owns — it never touches the engine or its `lua_State`. A late write
 * racing engine teardown is therefore safe (the atomic outlives the engine via
 * the shared_ptr), which removes the use-after-free lock-discipline a
 * `setInterrupted`-style watchdog needs.
 *
 * One watchdog may be shared (via `std::shared_ptr`) by every engine in a
 * composition root, so N engines cost one supervisor thread, not N.
 */
class PHOSPHORSCRIPTING_EXPORT LuauWatchdog
{
public:
    LuauWatchdog();
    ~LuauWatchdog();

    LuauWatchdog(const LuauWatchdog&) = delete;
    LuauWatchdog& operator=(const LuauWatchdog&) = delete;

    /// Register @p key's interrupt flag. Idempotent; replaces an existing flag.
    void registerFlag(const void* key, std::shared_ptr<std::atomic<bool>> flag);

    /// Remove @p key entirely. Safe to call from the engine destructor.
    void unregister(const void* key);

    /// Start a @p timeoutMs deadline for @p key on @p clock. No-op if @p key is
    /// unregistered or @p timeoutMs <= 0. With WatchdogClock::ThreadCpu the
    /// budget is the CPU time of the calling thread, so call this on the thread
    /// that runs the script, and @ref disarm (or @ref unregister) before that
    /// thread exits: the watchdog reads the thread's CPU clock while the key is
    /// armed.
    void arm(const void* key, int timeoutMs, WatchdogClock clock = WatchdogClock::Wall);

    /// Cancel @p key's pending deadline.
    void disarm(const void* key);

private:
    struct Entry
    {
        std::shared_ptr<std::atomic<bool>> flag;
        // Wall: when the interrupt fires. ThreadCpu: when to read the thread's
        // CPU clock next, which is never later than the CPU budget can run out.
        std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();
        uint64_t generation = 0;
        // ThreadCpu only: the armed thread's CPU clock, and the reading of it at
        // which the call has spent its budget.
        bool onThreadCpu = false;
        clockid_t cpuClock{};
        std::chrono::nanoseconds cpuDeadline{0};
    };

    void threadMain();

    std::unordered_map<const void*, Entry> m_entries;
    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_shutdown = false;
};

} // namespace PhosphorScripting
