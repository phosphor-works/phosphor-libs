// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorScripting/LuauEngine.h>
#include <PhosphorScripting/LuauWatchdog.h>

#include <QTest>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <memory>
#include <thread>

#include <pthread.h>
#include <signal.h>
#include <time.h>

using namespace std::chrono_literals;
using PhosphorScripting::LuauEngine;
using PhosphorScripting::LuauWatchdog;
using PhosphorScripting::WatchdogClock;

namespace {
constexpr int BudgetMs = 50;
constexpr auto Budget = std::chrono::milliseconds(BudgetMs);

std::chrono::nanoseconds cpuTime(clockid_t clock)
{
    timespec ts{};
    if (clock_gettime(clock, &ts) != 0) {
        return std::chrono::nanoseconds::max();
    }
    return std::chrono::seconds(ts.tv_sec) + std::chrono::nanoseconds(ts.tv_nsec);
}

std::chrono::nanoseconds threadCpuTime()
{
    return cpuTime(CLOCK_THREAD_CPUTIME_ID);
}

// spin(ms) runs until os.clock, a monotonic wall clock, has advanced by ms: it
// is CPU work while its thread runs, and costs nothing while the thread does not.
const QByteArray Module = QByteArrayLiteral(
    "return {\n"
    "  spin = function(ms)\n"
    "    local stop = os.clock() + ms / 1000\n"
    "    while os.clock() < stop do end\n"
    "    return true\n"
    "  end,\n"
    "  forever = function()\n"
    "    while true do end\n"
    "  end,\n"
    "}\n");

int loadModule(LuauEngine& engine, QString* error)
{
    if (!engine.init(error)) {
        return -1;
    }
    engine.sandbox();
    return engine.loadModule(QStringLiteral("watchdog_test"), Module, error);
}

// Keeps the thread that takes the signal off the CPU for three budgets, as a
// host function that blocks, or a scheduler that runs other programs, does.
extern "C" void holdThread(int)
{
    const int saved = errno;
    timespec ts{0, 3 * BudgetMs * 1'000'000L};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) { }
    errno = saved;
}

struct HeldCall
{
    LuauEngine::CallOutcome outcome;
    std::chrono::nanoseconds cpuUsed{0};
    QString error;
};

// Calls spin(3 budgets) on a thread of its own. Once that thread has run for a
// few milliseconds inside the call, a signal holds it off the CPU for three
// budgets: the wall clock passes the budget while the call has spent little of
// its CPU time.
HeldCall callHeldOffTheCpu(WatchdogClock clock)
{
    struct sigaction hold{};
    hold.sa_handler = holdThread;
    sigemptyset(&hold.sa_mask);
    hold.sa_flags = SA_RESTART;
    struct sigaction previous{};
    sigaction(SIGUSR1, &hold, &previous);

    auto watchdog = std::make_shared<LuauWatchdog>();
    HeldCall result;
    std::atomic<bool> calling{false};
    std::thread vm([&] {
        LuauEngine engine(watchdog);
        engine.setWatchdogClock(clock);
        const int module = loadModule(engine, &result.error);
        if (module < 0) {
            return;
        }
        calling = true;
        const auto before = threadCpuTime();
        result.outcome = engine.callModule(module, QStringLiteral("spin"), {3 * BudgetMs}, BudgetMs);
        result.cpuUsed = threadCpuTime() - before;
    });

    clockid_t vmClock{};
    if (pthread_getcpuclockid(vm.native_handle(), &vmClock) == 0) {
        const auto giveUp = std::chrono::steady_clock::now() + 10s;
        while (!calling && std::chrono::steady_clock::now() < giveUp) {
            std::this_thread::sleep_for(1ms);
        }
        // A few milliseconds of CPU after the start put the thread inside the
        // call, well short of its budget.
        const auto started = cpuTime(vmClock);
        while (calling && cpuTime(vmClock) - started < 5ms && std::chrono::steady_clock::now() < giveUp) {
            std::this_thread::sleep_for(1ms);
        }
        if (calling) {
            pthread_kill(vm.native_handle(), SIGUSR1);
        }
    } else {
        result.error = QStringLiteral("pthread_getcpuclockid failed");
    }
    vm.join();
    sigaction(SIGUSR1, &previous, nullptr);
    return result;
}
} // namespace

class TestWatchdog : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void wallIsTheDefault()
    {
        auto watchdog = std::make_shared<LuauWatchdog>();
        LuauEngine engine(watchdog);
        QVERIFY(engine.watchdogClock() == WatchdogClock::Wall);

        // A deadline armed without a clock passes while the thread sleeps.
        int key = 0;
        auto flag = std::make_shared<std::atomic<bool>>(false);
        watchdog->registerFlag(&key, flag);
        watchdog->arm(&key, BudgetMs);
        std::this_thread::sleep_for(3 * Budget);
        QTRY_VERIFY_WITH_TIMEOUT(flag->load(), 5000);
        watchdog->unregister(&key);
    }

    void threadCpuSpendsNoBudgetWhileTheThreadSleeps()
    {
        LuauWatchdog watchdog;
        int key = 0;
        auto flag = std::make_shared<std::atomic<bool>>(false);
        watchdog.registerFlag(&key, flag);
        watchdog.arm(&key, BudgetMs, WatchdogClock::ThreadCpu);
        std::this_thread::sleep_for(3 * Budget);
        QVERIFY(!flag->load());
        watchdog.disarm(&key);
        watchdog.unregister(&key);
    }

    void threadCpuFiresOnceTheThreadSpentItsBudget()
    {
        LuauWatchdog watchdog;
        int key = 0;
        auto flag = std::make_shared<std::atomic<bool>>(false);
        watchdog.registerFlag(&key, flag);
        const auto before = threadCpuTime();
        watchdog.arm(&key, BudgetMs, WatchdogClock::ThreadCpu);
        while (!flag->load() && threadCpuTime() - before < 10s) { }
        const auto used = threadCpuTime() - before;
        QVERIFY(flag->load());
        QVERIFY2(used >= Budget, qPrintable(QStringLiteral("fired after %1 us of CPU time").arg(used.count() / 1000)));
        watchdog.unregister(&key);
    }

    void wallStopsABusyLoop()
    {
        LuauEngine engine(std::make_shared<LuauWatchdog>());
        QString error;
        const int module = loadModule(engine, &error);
        QVERIFY2(module >= 0, qPrintable(error));
        const auto outcome = engine.callModule(module, QStringLiteral("forever"), {}, BudgetMs);
        QVERIFY(outcome.status == LuauEngine::CallStatus::TimedOut);
        QVERIFY2(!outcome.message.contains(QLatin1String("CPU")), qPrintable(outcome.message));
    }

    void threadCpuStopsABusyLoopAfterItsBudget()
    {
        LuauEngine engine(std::make_shared<LuauWatchdog>());
        engine.setWatchdogClock(WatchdogClock::ThreadCpu);
        QString error;
        const int module = loadModule(engine, &error);
        QVERIFY2(module >= 0, qPrintable(error));
        const auto before = threadCpuTime();
        const auto outcome = engine.callModule(module, QStringLiteral("forever"), {}, BudgetMs);
        const auto used = threadCpuTime() - before;
        QVERIFY(outcome.status == LuauEngine::CallStatus::TimedOut);
        QVERIFY2(outcome.message.contains(QLatin1String("of CPU time")), qPrintable(outcome.message));
        QVERIFY2(used >= Budget,
                 qPrintable(QStringLiteral("stopped after %1 us of CPU time").arg(used.count() / 1000)));
    }

    void threadCpuLetsACallHeldOffTheCpuFinish()
    {
        const HeldCall call = callHeldOffTheCpu(WatchdogClock::ThreadCpu);
        QVERIFY2(call.error.isEmpty(), qPrintable(call.error));
        QVERIFY2(call.outcome.status == LuauEngine::CallStatus::Ok, qPrintable(call.outcome.message));
        QCOMPARE(call.outcome.result.toBool(), true);
        QVERIFY2(call.cpuUsed < Budget,
                 qPrintable(QStringLiteral("the call used %1 us of CPU time").arg(call.cpuUsed.count() / 1000)));
    }

    void wallStopsACallHeldOffTheCpu()
    {
        const HeldCall call = callHeldOffTheCpu(WatchdogClock::Wall);
        QVERIFY2(call.error.isEmpty(), qPrintable(call.error));
        QVERIFY2(call.outcome.status == LuauEngine::CallStatus::TimedOut, qPrintable(call.outcome.message));
    }
};

QTEST_GUILESS_MAIN(TestWatchdog)
#include "test_watchdog.moc"
