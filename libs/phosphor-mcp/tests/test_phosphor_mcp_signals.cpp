// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// handleTerminationSignals(): SIGTERM and SIGHUP end the server, SIGPIPE's
// disposition is never touched (an ignored SIGPIPE would survive exec into
// the application's children), one server at a time holds the signals, and
// the old dispositions come back. And a closed output never ends the
// process, with or without it.
//
// The signals go to this process, with the server's handler installed: the
// test checks the handler is there before it sends one, so a failure is a
// failed check, not a test binary killed by its own signal.

#include "mcptesthelpers.h"

#include <PhosphorMcp/McpServer.h>

#include <QRegularExpression>
#include <QSignalSpy>
#include <QTest>

#include <csignal>

#include <fcntl.h>
#include <unistd.h>

using namespace PhosphorMcp;
using namespace PhosphorMcpTests;

namespace {

// sa_handler and sa_sigaction share their storage, so this compares either.
void (*handlerOf(int signal))(int)
{
    struct sigaction current;
    sigaction(signal, nullptr, &current);
    return current.sa_handler;
}

bool handlerIs(int signal, void (*handler)(int))
{
    return handlerOf(signal) == handler;
}

// What the test found: QtTest installs its own handlers for SIGTERM and
// SIGHUP, among others, to report a crash.
void (*s_termAtStart)(int) = nullptr;
void (*s_hupAtStart)(int) = nullptr;
void (*s_pipeAtStart)(int) = nullptr;

bool hasServerHandler(int signal)
{
    const auto handler = handlerOf(signal);
    const auto atStart = signal == SIGTERM ? s_termAtStart : s_hupAtStart;
    return handler != SIG_DFL && handler != SIG_IGN && handler != atStart;
}

bool restored()
{
    return handlerIs(SIGTERM, s_termAtStart) && handlerIs(SIGHUP, s_hupAtStart) && handlerIs(SIGPIPE, s_pipeAtStart);
}

} // namespace

class TestPhosphorMcpSignals : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        s_termAtStart = handlerOf(SIGTERM);
        s_hupAtStart = handlerOf(SIGHUP);
        s_pipeAtStart = handlerOf(SIGPIPE);
        QVERIFY(s_pipeAtStart != SIG_IGN);
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
            QVERIFY(handlerIs(SIGPIPE, s_pipeAtStart));
            QVERIFY(server.start(peer.serverFd(), peer.serverFd()));

            QCOMPARE(::kill(::getpid(), signal), 0);
            QVERIFY(spinUntil([&] {
                return finished.count() == 1;
            }));
            QCOMPARE(finished.first().first().value<McpServer::FinishReason>(), reason);
            QVERIFY(!server.isRunning());
            // A second signal changes nothing.
            QCOMPARE(::kill(::getpid(), SIGTERM), 0);
            spinFor(50);
            QCOMPARE(finished.count(), 1);
        }
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

    void restoresTheDispositionsItFound()
    {
        struct sigaction ignore = {};
        ignore.sa_handler = SIG_IGN;
        struct sigaction old;
        sigaction(SIGHUP, &ignore, &old);
        {
            McpServer server;
            QVERIFY(server.handleTerminationSignals());
            QVERIFY(hasServerHandler(SIGHUP));
        }
        QVERIFY(handlerIs(SIGHUP, SIG_IGN));
        sigaction(SIGHUP, &old, nullptr);
    }

    void signalBeforeStart()
    {
        // The client may send SIGTERM before the server runs: it still ends.
        McpServer server;
        QSignalSpy finished(&server, &McpServer::finished);
        QVERIFY(server.handleTerminationSignals());
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

    void closedOutputIsNotFatal_data()
    {
        QTest::addColumn<bool>("handleSignals");
        QTest::newRow("with handleTerminationSignals") << true;
        QTest::newRow("without") << false;
    }

    void closedOutputIsNotFatal()
    {
        // stdout is a pipe whose reader has gone, and SIGPIPE has its
        // default action, which ends the process. The reply to a request
        // fails with EPIPE, and the server ends; the process does not, and
        // the disposition stays the default all along.
        QFETCH(bool, handleSignals);
        QVERIFY(restored());
        // The default action, which ends the process, whatever the test
        // runner set.
        struct sigaction defaultAction = {};
        defaultAction.sa_handler = SIG_DFL;
        struct sigaction oldPipe;
        sigaction(SIGPIPE, &defaultAction, &oldPipe);
        Peer input;
        int out[2];
        QCOMPARE(::pipe2(out, O_CLOEXEC), 0);
        ::close(out[0]);
        {
            McpServer server;
            QSignalSpy finished(&server, &McpServer::finished);
            if (handleSignals) {
                QVERIFY(server.handleTerminationSignals());
                QVERIFY(handlerIs(SIGPIPE, SIG_DFL));
            }
            QVERIFY(server.start(input.serverFd(), out[1]));
            QVERIFY(handlerIs(SIGPIPE, SIG_DFL));
            QVERIFY(input.send(request(1, QStringLiteral("server/discover"))));
            QVERIFY(spinUntil([&] {
                return finished.count() == 1;
            }));
            QCOMPARE(finished.first().first().value<McpServer::FinishReason>(), McpServer::FinishReason::OutputClosed);
            QVERIFY(handlerIs(SIGPIPE, SIG_DFL));
        }
        ::close(out[1]);
        QVERIFY(handlerIs(SIGPIPE, SIG_DFL));
        sigaction(SIGPIPE, &oldPipe, nullptr);
        sigset_t pending;
        sigemptyset(&pending);
        sigpending(&pending);
        QCOMPARE(sigismember(&pending, SIGPIPE), 0);
    }
};

QTEST_GUILESS_MAIN(TestPhosphorMcpSignals)
#include "test_phosphor_mcp_signals.moc"
