// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// handleTerminationSignals(): SIGTERM and SIGHUP end the server, also while
// the replies drain, and a signal the process ignores stays ignored.
// SIGPIPE's disposition is never touched (an ignored SIGPIPE would survive
// exec into the application's children). One server at a time holds the
// signals, and the old dispositions come back when it finishes, whole, unless
// someone installed another handler since. A child forked before exec keeps
// its own signals, the signals follow the server to another thread, and a
// thread without an event dispatcher is refused. And a closed output never
// ends the process, with or without it, whether the application blocks
// SIGPIPE or has one pending.
//
// The signals go to this process, with the server's handler installed: the
// test checks the handler is there before it sends one, so a failure is a
// failed check, not a test binary killed by its own signal.

#include "mcptesthelpers.h"

#include <PhosphorMcp/McpServer.h>
#include <PhosphorMcp/McpTransport.h>

#include <QElapsedTimer>
#include <QEventLoop>
#include <QPointer>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTest>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <ctime>
#include <optional>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace PhosphorMcp;
using namespace PhosphorMcpTests;

namespace {

struct sigaction actionOf(int signal)
{
    struct sigaction current;
    sigaction(signal, nullptr, &current);
    return current;
}

// sa_handler and sa_sigaction share their storage, so this compares either.
void (*handlerOf(int signal))(int)
{
    return actionOf(signal).sa_handler;
}

bool handlerIs(int signal, void (*handler)(int))
{
    return handlerOf(signal) == handler;
}

// The whole disposition: the handler, the flags (but SA_RESTORER, which the C
// library adds on its own) and the mask. A restore that put back only the
// handler would break QtTest's SA_SIGINFO handler.
bool sameAction(const struct sigaction& a, const struct sigaction& b)
{
#ifdef SA_RESTORER
    constexpr int Ignored = SA_RESTORER;
#else
    constexpr int Ignored = 0;
#endif
    if (a.sa_handler != b.sa_handler || (a.sa_flags & ~Ignored) != (b.sa_flags & ~Ignored)) {
        return false;
    }
    for (int signal = 1; signal < NSIG; ++signal) {
        if (sigismember(&a.sa_mask, signal) != sigismember(&b.sa_mask, signal)) {
            return false;
        }
    }
    return true;
}

// What the test found: QtTest installs its own handlers for SIGTERM and
// SIGHUP, among others, to report a crash.
struct sigaction s_termAtStart;
struct sigaction s_hupAtStart;
struct sigaction s_pipeAtStart;

bool hasServerHandler(int signal)
{
    const auto handler = handlerOf(signal);
    const auto atStart = (signal == SIGTERM ? s_termAtStart : s_hupAtStart).sa_handler;
    return handler != SIG_DFL && handler != SIG_IGN && handler != atStart;
}

// One byte from a forked child, within a bound, so that a child stuck after
// the fork fails the test instead of hanging it.
bool readFromChild(int fd, char& byte)
{
    // A signal to this process interrupts poll(), which SA_RESTART does not
    // restart.
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 10000) {
        pollfd ready = {fd, POLLIN, 0};
        const int got = ::poll(&ready, 1, int(10000 - timer.elapsed()));
        if (got == 1) {
            return ::read(fd, &byte, 1) == 1;
        }
        if (got == 0 || errno != EINTR) {
            return false;
        }
    }
    return false;
}

bool restored()
{
    return sameAction(actionOf(SIGTERM), s_termAtStart) && sameAction(actionOf(SIGHUP), s_hupAtStart)
        && sameAction(actionOf(SIGPIPE), s_pipeAtStart);
}

void laterHandler(int)
{
}

// Takes a pending SIGPIPE off this thread, if there is one. SIGPIPE must be
// blocked.
bool takePendingSigpipe()
{
    sigset_t pipeSet;
    sigemptyset(&pipeSet);
    sigaddset(&pipeSet, SIGPIPE);
    const timespec noWait{0, 0};
    return sigtimedwait(&pipeSet, nullptr, &noWait) == SIGPIPE;
}

} // namespace

class TestPhosphorMcpSignals : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        s_termAtStart = actionOf(SIGTERM);
        s_hupAtStart = actionOf(SIGHUP);
        s_pipeAtStart = actionOf(SIGPIPE);
        QVERIFY(s_pipeAtStart.sa_handler != SIG_IGN);
        QVERIFY(s_termAtStart.sa_handler != SIG_IGN);
        QVERIFY(s_hupAtStart.sa_handler != SIG_IGN);
    }

    void terminateEndsTheServer_data()
    {
        QTest::addColumn<int>("signal");
        QTest::addColumn<McpServer::FinishReason>("reason");
        QTest::newRow("SIGTERM") << SIGTERM << McpServer::FinishReason::Terminated;
        QTest::newRow("SIGHUP") << SIGHUP << McpServer::FinishReason::HangUp;
    }

    void terminateEndsTheServer()
    {
        QFETCH(int, signal);
        QFETCH(McpServer::FinishReason, reason);
        Peer peer;
        {
            McpServer server;
            QSignalSpy finished(&server, &McpServer::finished);
            QVERIFY(server.handleTerminationSignals());
            QVERIFY(server.handleTerminationSignals());
            QVERIFY(hasServerHandler(SIGTERM));
            QVERIFY(hasServerHandler(SIGHUP));
            QVERIFY(sameAction(actionOf(SIGPIPE), s_pipeAtStart));
            QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("has no tools")));
            QVERIFY(server.start(peer.serverFd(), peer.serverFd()));

            QVERIFY(hasServerHandler(signal));
            QCOMPARE(::kill(::getpid(), signal), 0);
            QVERIFY(spinUntil([&] {
                return finished.count() == 1;
            }));
            QCOMPARE(finished.first().first().value<McpServer::FinishReason>(), reason);
            QVERIFY(!server.isRunning());
            // Finished, the server gave the signals back already: a second
            // one, during a slow teardown of the application, has its usual
            // effect. Here that is QtTest's handler, which ends the test
            // binary, so none is sent.
            QVERIFY(restored());
            // Another server may take them now.
            McpServer next;
            QVERIFY(next.handleTerminationSignals());
            QVERIFY(hasServerHandler(SIGTERM));
        }
        QVERIFY(restored());
    }

    void signalReleasesBeforeItsCallbacks()
    {
        // SIGTERM itself starts the end. The dispositions are back before the
        // cancel callbacks run, so that a second SIGTERM, during a callback
        // that hangs, would end the process. (It is not sent: here that
        // would be QtTest's fatal handler.)
        Peer peer;
        McpServer server;
        QSignalSpy finished(&server, &McpServer::finished);
        std::vector<McpCall> calls;
        QVERIFY(server.addTool(McpTool(QStringLiteral("hold")), [&calls](McpCall call) {
            calls.push_back(std::move(call));
        }));
        QVERIFY(server.handleTerminationSignals());
        QVERIFY(server.start(peer.serverFd(), peer.serverFd()));
        QVERIFY(peer.send(callRequest(1, QStringLiteral("hold"))));
        QVERIFY(spinUntil([&] {
            return calls.size() == 1;
        }));
        bool ran = false;
        bool restoredInTheCallback = false;
        calls.front().onCancelled([&] {
            ran = true;
            restoredInTheCallback = restored();
        });
        QVERIFY(hasServerHandler(SIGTERM));
        QCOMPARE(::kill(::getpid(), SIGTERM), 0);
        QVERIFY(spinUntil([&] {
            return finished.count() == 1;
        }));
        QVERIFY(ran);
        QVERIFY(restoredInTheCallback);
        QCOMPARE(finished.first().first().value<McpServer::FinishReason>(), McpServer::FinishReason::Terminated);
    }

    void signalDuringTheDrainEndsAtOnce()
    {
        // The end of the input started a drain to a client that reads
        // nothing. SIGTERM does not wait DrainTimeoutMs for it.
        Peer peer;
        McpServer server;
        QSignalSpy finished(&server, &McpServer::finished);
        QVERIFY(server.addTool(McpTool(QStringLiteral("big")), [](McpCall call) {
            call.finish(McpResult::text(QString(4 * 1024 * 1024, QLatin1Char('b'))));
        }));
        QVERIFY(server.handleTerminationSignals());
        QVERIFY(server.start(peer.serverFd(), peer.serverFd()));
        QVERIFY(peer.send(callRequest(1, QStringLiteral("big"))));
        QVERIFY(spinUntil([&] {
            return server.transport() && server.transport()->pendingWriteBytes() > 0;
        }));
        peer.closeWrite();
        QVERIFY(spinUntil([&] {
            return !server.isRunning();
        }));
        // The reply still drains: nobody reads it.
        QCOMPARE(finished.count(), 0);

        QVERIFY(hasServerHandler(SIGTERM));
        QElapsedTimer elapsed;
        elapsed.start();
        QCOMPARE(::kill(::getpid(), SIGTERM), 0);
        QVERIFY(spinUntil([&] {
            return finished.count() == 1;
        }));
        QVERIFY(elapsed.elapsed() < McpServer::DrainTimeoutMs);
        QCOMPARE(finished.first().first().value<McpServer::FinishReason>(), McpServer::FinishReason::Terminated);
        QVERIFY(restored());
        // An ended server does not take the signals again.
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("the server has ended")));
        QVERIFY(!server.handleTerminationSignals());
        QVERIFY(restored());
    }

    void signalInsideACancelCallbackWaitsForTheOtherCallbacks()
    {
        // At the end of the input the server runs the cancel callbacks. Each
        // here runs a nested event loop, and SIGTERM comes inside the first.
        // The signal decides the reason, but finished() still comes after
        // every callback, and its slot may delete the server. The signals
        // come back at once, while the callbacks still run, so that a second
        // one would have its usual effect. (It is not sent: here that would
        // be QtTest's fatal handler.)
        Peer peer;
        auto* server = new McpServer;
        const QPointer<McpServer> alive(server);
        std::vector<McpCall> calls;
        QVERIFY(server->addTool(McpTool(QStringLiteral("hold")), [&calls](McpCall call) {
            calls.push_back(std::move(call));
        }));
        QVERIFY(server->handleTerminationSignals());
        QVERIFY(server->start(peer.serverFd(), peer.serverFd()));
        QVERIFY(peer.send(callRequest(1, QStringLiteral("hold"))));
        QVERIFY(peer.send(callRequest(2, QStringLiteral("hold"))));
        QVERIFY(spinUntil([&] {
            return calls.size() == 2;
        }));
        QVERIFY(hasServerHandler(SIGTERM));
        int callbacksRun = 0;
        int callbacksBeforeFinished = -1;
        bool restoredDuringTheCallbacks = false;
        std::optional<McpServer::FinishReason> reason;
        for (McpCall& call : calls) {
            call.onCancelled([&callbacksRun, &restoredDuringTheCallbacks] {
                const bool first = callbacksRun++ == 0;
                QEventLoop loop;
                if (first) {
                    QTimer::singleShot(0, [] {
                        ::kill(::getpid(), SIGTERM);
                    });
                }
                QTimer::singleShot(200, &loop, &QEventLoop::quit);
                loop.exec();
                if (first) {
                    restoredDuringTheCallbacks = restored();
                }
            });
        }
        connect(server, &McpServer::finished, server, [&, server](McpServer::FinishReason finishReason) {
            reason = finishReason;
            callbacksBeforeFinished = callbacksRun;
            server->deleteLater();
        });
        peer.closeWrite();
        QVERIFY(spinUntil([&] {
            return alive.isNull();
        }));
        QCOMPARE(callbacksBeforeFinished, 2);
        QVERIFY(reason.has_value());
        QCOMPARE(*reason, McpServer::FinishReason::Terminated);
        QVERIFY(restoredDuringTheCallbacks);
        QVERIFY(restored());
    }

    void oneServerAtATime()
    {
        auto first = std::make_unique<McpServer>();
        McpServer second;
        QVERIFY(first->handleTerminationSignals());
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("another server handles the signals")));
        QVERIFY(!second.handleTerminationSignals());
        first.reset();
        QVERIFY(restored());
        QVERIFY(second.handleTerminationSignals());
        QVERIFY(hasServerHandler(SIGTERM));
    }

    void anIgnoredSignalStaysIgnored()
    {
        // As under nohup, which ignores SIGHUP.
        struct sigaction ignore = {};
        ignore.sa_handler = SIG_IGN;
        struct sigaction old;
        sigaction(SIGHUP, &ignore, &old);
        const auto restore = qScopeGuard([&] {
            sigaction(SIGHUP, &old, nullptr);
        });
        {
            McpServer server;
            QVERIFY(server.handleTerminationSignals());
            QVERIFY(handlerIs(SIGHUP, SIG_IGN));
            QVERIFY(hasServerHandler(SIGTERM));
        }
        QVERIFY(handlerIs(SIGHUP, SIG_IGN));
        QVERIFY(sameAction(actionOf(SIGTERM), s_termAtStart));
    }

    void aHandlerInstalledSinceStays()
    {
        // Someone installs a handler for SIGTERM after the server took it.
        // The server's end does not put the old one back over it.
        const auto restore = qScopeGuard([] {
            sigaction(SIGTERM, &s_termAtStart, nullptr);
        });
        {
            McpServer server;
            QVERIFY(server.handleTerminationSignals());
            struct sigaction later = {};
            later.sa_handler = laterHandler;
            sigemptyset(&later.sa_mask);
            sigaction(SIGTERM, &later, nullptr);
        }
        QVERIFY(handlerIs(SIGTERM, laterHandler));
        QVERIFY(sameAction(actionOf(SIGHUP), s_hupAtStart));
    }

    void signalBeforeStart()
    {
        // The client may send SIGTERM before the server runs: it still ends.
        McpServer server;
        QSignalSpy finished(&server, &McpServer::finished);
        QVERIFY(server.handleTerminationSignals());
        QVERIFY(hasServerHandler(SIGTERM));
        QCOMPARE(::kill(::getpid(), SIGTERM), 0);
        QVERIFY(spinUntil([&] {
            return finished.count() == 1;
        }));
        QCOMPARE(finished.first().first().value<McpServer::FinishReason>(), McpServer::FinishReason::Terminated);
        // An ended server does not start.
        Peer peer;
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("started or ended before")));
        QVERIFY(!server.start(peer.serverFd(), peer.serverFd()));
    }

    void forkedChildKeepsItsOwnSignals()
    {
        // A child forked before exec inherits the handler and the pipe. Its
        // SIGTERM ends the child, with the disposition it had before the
        // server, and not the parent's server.
        struct sigaction defaultAction = {};
        defaultAction.sa_handler = SIG_DFL;
        sigemptyset(&defaultAction.sa_mask);
        sigaction(SIGTERM, &defaultAction, nullptr);
        const auto restore = qScopeGuard([] {
            sigaction(SIGTERM, &s_termAtStart, nullptr);
        });
        McpServer server;
        QSignalSpy finished(&server, &McpServer::finished);
        QVERIFY(server.handleTerminationSignals());
        QVERIFY(handlerOf(SIGTERM) != SIG_DFL);
        const pid_t child = ::fork();
        if (child == 0) {
            ::kill(::getpid(), SIGTERM);
            ::_exit(0);
        }
        QVERIFY(child > 0);
        int status = 0;
        QCOMPARE(::waitpid(child, &status, 0), child);
        QVERIFY2(WIFSIGNALED(status), qPrintable(QString::number(status)));
        QCOMPARE(WTERMSIG(status), SIGTERM);
        spinFor(100);
        QCOMPARE(finished.count(), 0);
    }

    void childCannotTakeTheParentsHandler()
    {
        // After the parent's call, the same server's call in a forked child
        // says false: the handler there is the parent's, and SIGTERM would
        // take the old disposition.
        McpServer server;
        QVERIFY(server.handleTerminationSignals());
        int report[2];
        QCOMPARE(::pipe(report), 0);
        const pid_t child = ::fork();
        if (child == 0) {
            ::close(report[0]);
            QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("cannot take its handler")));
            const char answer = server.handleTerminationSignals() ? '1' : '0';
            [[maybe_unused]] const ssize_t written = ::write(report[1], &answer, 1);
            ::_exit(0);
        }
        ::close(report[1]);
        const auto closeReport = qScopeGuard([&report] {
            ::close(report[0]);
        });
        QVERIFY(child > 0);
        char answer = 0;
        QVERIFY(readFromChild(report[0], answer));
        int status = 0;
        QCOMPARE(::waitpid(child, &status, 0), child);
        QCOMPARE(answer, '0');
        // The parent still holds them.
        QVERIFY(server.handleTerminationSignals());
        QVERIFY(hasServerHandler(SIGTERM));
    }

    void childLeavesTheParentsSignalsAlone()
    {
        // A child forked after the parent's call shares the self-pipe. While
        // the child runs its event loop, the parent takes a SIGTERM, and its
        // byte waits in the pipe. The child's copy of the server leaves it,
        // and the parent's server ends on it.
        McpServer server;
        QSignalSpy finished(&server, &McpServer::finished);
        QVERIFY(server.handleTerminationSignals());
        int report[2];
        QCOMPARE(::pipe(report), 0);
        const pid_t child = ::fork();
        if (child == 0) {
            ::close(report[0]);
            bool childFinished = false;
            QObject::connect(&server, &McpServer::finished, [&childFinished] {
                childFinished = true;
            });
            // The parent is blocked in poll(), so the child's loop sees the
            // byte first.
            ::kill(::getppid(), SIGTERM);
            spinFor(300);
            const char answer = childFinished ? '1' : '0';
            [[maybe_unused]] const ssize_t written = ::write(report[1], &answer, 1);
            ::_exit(0);
        }
        ::close(report[1]);
        const auto closeReport = qScopeGuard([&report] {
            ::close(report[0]);
        });
        QVERIFY(child > 0);
        char answer = 0;
        QVERIFY(readFromChild(report[0], answer));
        int status = 0;
        QCOMPARE(::waitpid(child, &status, 0), child);
        QCOMPARE(answer, '0');
        QVERIFY(spinUntil([&] {
            return finished.count() == 1;
        }));
        QCOMPARE(finished.first().first().value<McpServer::FinishReason>(), McpServer::FinishReason::Terminated);
    }

    void signalsFollowTheServerToAnotherThread()
    {
        // Moved to a worker thread after handleTerminationSignals(), the
        // server ends there: the cancel callbacks run on its thread.
        QThread worker;
        worker.start();
        auto* context = new QObject;
        context->moveToThread(&worker);
        const auto stop = qScopeGuard([&] {
            QMetaObject::invokeMethod(context, &QObject::deleteLater);
            worker.quit();
            worker.wait();
        });
        Peer peer;
        auto* server = new McpServer;
        // Touched on the worker only, until the server is gone.
        McpCall held;
        std::atomic<bool> holding{false};
        std::atomic<QThread*> cancelledOn{nullptr};
        QVERIFY(server->addTool(McpTool(QStringLiteral("hold")), [&](McpCall call) {
            held = std::move(call);
            held.onCancelled([&] {
                cancelledOn = QThread::currentThread();
            });
            holding = true;
        }));
        QVERIFY(server->handleTerminationSignals());
        server->moveToThread(&worker);
        bool started = false;
        QMetaObject::invokeMethod(
            context,
            [&] {
                started = server->start(peer.serverFd(), peer.serverFd());
            },
            Qt::BlockingQueuedConnection);
        QVERIFY(started);
        QVERIFY(peer.send(callRequest(1, QStringLiteral("hold"))));
        QVERIFY(spinUntil([&] {
            return holding.load();
        }));
        QVERIFY(hasServerHandler(SIGTERM));
        QCOMPARE(::kill(::getpid(), SIGTERM), 0);
        QVERIFY(spinUntil([&] {
            return cancelledOn.load() != nullptr;
        }));
        QCOMPARE(cancelledOn.load(), &worker);
        QMetaObject::invokeMethod(
            context,
            [&] {
                delete server;
                held = McpCall();
            },
            Qt::BlockingQueuedConnection);
        QVERIFY(restored());
    }

    void refusedOnAThreadWithoutAnEventDispatcher()
    {
        // The self-pipe's notifier would never fire there, and the signals
        // would end nothing.
        bool handled = true;
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("no event dispatcher")));
        std::thread([&] {
            McpServer server;
            handled = server.handleTerminationSignals();
        }).join();
        QVERIFY(!handled);
        QVERIFY(restored());
    }

    void closedOutputIsNotFatal_data()
    {
        QTest::addColumn<bool>("handleSignals");
        QTest::addColumn<int>("blocked");
        // 0: SIGPIPE unblocked. 1: the application blocks it, so a SIGPIPE
        // the transport left would stay pending, and show. 2: blocked, with
        // one pending before, which is the application's and must stay.
        for (const bool handleSignals : {true, false}) {
            for (const int blocked : {0, 1, 2}) {
                QTest::addRow("%s, %s", handleSignals ? "with handleTerminationSignals" : "without",
                              blocked == 0       ? "unblocked"
                                  : blocked == 1 ? "blocked"
                                                 : "blocked with one pending")
                    << handleSignals << blocked;
            }
        }
    }

    void closedOutputIsNotFatal()
    {
        // stdout is a pipe whose reader has gone, and SIGPIPE has its
        // default action, which ends the process. The reply to a request
        // fails with EPIPE, and the server ends. The process does not, and
        // the disposition stays the default all along.
        QFETCH(bool, handleSignals);
        QFETCH(int, blocked);
        QVERIFY(restored());
        // The default action, which ends the process, whatever the test
        // runner set.
        struct sigaction defaultAction = {};
        defaultAction.sa_handler = SIG_DFL;
        struct sigaction oldPipe;
        sigaction(SIGPIPE, &defaultAction, &oldPipe);
        sigset_t pipeSet;
        sigemptyset(&pipeSet);
        sigaddset(&pipeSet, SIGPIPE);
        sigset_t oldMask;
        pthread_sigmask(SIG_BLOCK, nullptr, &oldMask);
        if (blocked != 0) {
            pthread_sigmask(SIG_BLOCK, &pipeSet, nullptr);
        }
        if (blocked == 2) {
            QCOMPARE(::pthread_kill(::pthread_self(), SIGPIPE), 0);
        }
        int out[2] = {-1, -1};
        // In this order on every path: no SIGPIPE left pending when the mask
        // and the disposition come back, then the descriptors.
        const auto restore = qScopeGuard([&] {
            if (blocked != 0) {
                while (takePendingSigpipe()) { }
            }
            pthread_sigmask(SIG_SETMASK, &oldMask, nullptr);
            sigaction(SIGPIPE, &oldPipe, nullptr);
            for (const int fd : out) {
                if (fd >= 0) {
                    ::close(fd);
                }
            }
        });
        Peer input;
        QCOMPARE(::pipe2(out, O_CLOEXEC), 0);
        ::close(out[0]);
        out[0] = -1;
        {
            McpServer server;
            QSignalSpy finished(&server, &McpServer::finished);
            if (handleSignals) {
                QVERIFY(server.handleTerminationSignals());
                QVERIFY(handlerIs(SIGPIPE, SIG_DFL));
            }
            QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("has no tools")));
            QVERIFY(server.start(input.serverFd(), out[1]));
            QVERIFY(handlerIs(SIGPIPE, SIG_DFL));
            QVERIFY(input.send(request(1, QStringLiteral("server/discover"))));
            QVERIFY(spinUntil([&] {
                return finished.count() == 1;
            }));
            QCOMPARE(finished.first().first().value<McpServer::FinishReason>(), McpServer::FinishReason::OutputClosed);
            QVERIFY(handlerIs(SIGPIPE, SIG_DFL));
        }
        QVERIFY(handlerIs(SIGPIPE, SIG_DFL));
        sigset_t pending;
        sigemptyset(&pending);
        sigpending(&pending);
        QCOMPARE(sigismember(&pending, SIGPIPE), blocked == 2 ? 1 : 0);
    }
};

QTEST_GUILESS_MAIN(TestPhosphorMcpSignals)
#include "test_phosphor_mcp_signals.moc"
