// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorScripting/LuauWatchdog.h>

#include <algorithm>

#include <pthread.h>

namespace PhosphorScripting {

using Clock = std::chrono::steady_clock;

namespace {
// The shortest wait between two reads of a thread's CPU clock. Without it a
// call a few microseconds short of its budget would wake the watchdog in a
// loop; with it, the interrupt is at most this late in CPU time.
constexpr auto MinCpuRecheck = std::chrono::milliseconds(1);

bool readCpuClock(clockid_t clock, std::chrono::nanoseconds* now)
{
    timespec ts{};
    if (clock_gettime(clock, &ts) != 0) {
        return false;
    }
    *now = std::chrono::seconds(ts.tv_sec) + std::chrono::nanoseconds(ts.tv_nsec);
    return true;
}
} // namespace

LuauWatchdog::LuauWatchdog()
{
    m_thread = std::thread([this] {
        threadMain();
    });
}

LuauWatchdog::~LuauWatchdog()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_shutdown = true;
    }
    m_cv.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void LuauWatchdog::registerFlag(const void* key, std::shared_ptr<std::atomic<bool>> flag)
{
    if (!key || !flag) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        Entry& entry = m_entries[key];
        entry.flag = std::move(flag);
        entry.deadline = Clock::time_point::max();
        entry.onThreadCpu = false;
        ++entry.generation;
    }
    m_cv.notify_all();
}

void LuauWatchdog::unregister(const void* key)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_entries.erase(key);
    }
    m_cv.notify_all();
}

void LuauWatchdog::arm(const void* key, int timeoutMs, WatchdogClock clock)
{
    if (timeoutMs <= 0) {
        return;
    }
    const std::chrono::milliseconds budget(timeoutMs);
    clockid_t cpuClock{};
    const bool wantCpu = clock == WatchdogClock::ThreadCpu && pthread_getcpuclockid(pthread_self(), &cpuClock) == 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_entries.find(key);
        if (it == m_entries.end()) {
            return;
        }
        Entry& entry = it->second;
        std::chrono::nanoseconds cpuNow{0};
        entry.onThreadCpu = wantCpu && readCpuClock(cpuClock, &cpuNow);
        if (entry.onThreadCpu) {
            entry.cpuClock = cpuClock;
            entry.cpuDeadline = cpuNow + budget;
        }
        // Compute the deadline inside the lock so contention can't push it into
        // the past and fire a spurious interrupt. One thread spends CPU time no
        // faster than the wall clock runs, so for ThreadCpu this is the earliest
        // moment the budget can be spent: the first time to read its clock.
        entry.deadline = Clock::now() + budget;
        ++entry.generation;
    }
    m_cv.notify_all();
}

void LuauWatchdog::disarm(const void* key)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_entries.find(key);
        if (it != m_entries.end()) {
            ++it->second.generation;
            it->second.deadline = Clock::time_point::max();
            it->second.onThreadCpu = false;
        }
    }
    m_cv.notify_all();
}

void LuauWatchdog::threadMain()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    while (!m_shutdown) {
        // Earliest armed deadline across all entries.
        const void* nextKey = nullptr;
        auto nextDeadline = Clock::time_point::max();
        uint64_t nextGeneration = 0;
        for (const auto& [key, entry] : m_entries) {
            if (entry.deadline < nextDeadline) {
                nextDeadline = entry.deadline;
                nextKey = key;
                nextGeneration = entry.generation;
            }
        }

        if (!nextKey || nextDeadline == Clock::time_point::max()) {
            m_cv.wait(lock, [this] {
                if (m_shutdown) {
                    return true;
                }
                for (const auto& [key, entry] : m_entries) {
                    if (entry.deadline != Clock::time_point::max()) {
                        return true;
                    }
                }
                return false;
            });
            continue;
        }

        const bool predicateWoke = m_cv.wait_until(lock, nextDeadline, [this, nextKey, nextGeneration] {
            if (m_shutdown) {
                return true;
            }
            auto it = m_entries.find(nextKey);
            if (it == m_entries.end()) {
                return true; // unregistered
            }
            return it->second.generation != nextGeneration; // disarmed / re-armed
        });

        if (m_shutdown) {
            break;
        }
        if (predicateWoke) {
            continue; // state changed; recompute
        }

        // Deadline fired and the entry still holds the generation we captured:
        // raise the interrupt flag. We touch only the shared atomic, never the
        // engine — so a concurrent engine teardown is safe.
        auto it = m_entries.find(nextKey);
        if (it != m_entries.end() && it->second.generation == nextGeneration) {
            Entry& entry = it->second;
            if (entry.onThreadCpu) {
                // The armed thread is alive here: it disarms under m_mutex before
                // it leaves the call, and the generation shows it has not. A
                // thread cannot be waited on until a CPU time, so wait for the
                // CPU budget that is left, a lower bound of the wall time it
                // takes to spend it, and read the clock again. A failed read
                // falls back to the wall deadline that has just passed.
                std::chrono::nanoseconds cpuNow{0};
                if (readCpuClock(entry.cpuClock, &cpuNow) && cpuNow < entry.cpuDeadline) {
                    const std::chrono::nanoseconds left =
                        std::max<std::chrono::nanoseconds>(entry.cpuDeadline - cpuNow, MinCpuRecheck);
                    entry.deadline = Clock::now() + std::chrono::ceil<Clock::duration>(left);
                    continue;
                }
            }
            entry.deadline = Clock::time_point::max();
            entry.onThreadCpu = false;
            if (entry.flag) {
                entry.flag->store(true, std::memory_order_relaxed);
            }
        }
    }
}

} // namespace PhosphorScripting
