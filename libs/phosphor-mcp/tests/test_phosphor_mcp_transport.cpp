// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// McpTransport on its own: lines in, lines out, the line limit, the end of
// the input, a slow reader and a reader that went away.

#include "mcptesthelpers.h"

#include <PhosphorMcp/McpTransport.h>

#include <QByteArray>
#include <QList>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>

#include <csignal>

#include <fcntl.h>
#include <unistd.h>

using namespace PhosphorMcp;
using PhosphorMcpTests::Peer;
using PhosphorMcpTests::spinUntil;

class TestPhosphorMcpTransport : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void splitsLines()
    {
        Peer peer;
        QVERIFY(peer.isValid());
        McpTransport transport(peer.serverFd(), peer.serverFd());
        QVERIFY(transport.open());
        QList<QByteArray> lines;
        connect(&transport, &McpTransport::lineReceived, this, [&](const QByteArray& line) {
            lines.append(line);
        });

        // Two lines and the start of a third in one write, its end in the
        // next, and a carriage return before a newline.
        QVERIFY(peer.sendRaw("one\ntwo\r\nthr"));
        QVERIFY(spinUntil([&] {
            return lines.size() == 2;
        }));
        QVERIFY(peer.sendRaw("ee\n"));
        QVERIFY(spinUntil([&] {
            return lines.size() == 3;
        }));
        QCOMPARE(lines, (QList<QByteArray>{"one", "two", "three"}));
    }

    void dropsLinesOverTheLimit()
    {
        Peer peer;
        McpTransport transport(peer.serverFd(), peer.serverFd());
        transport.setMaxLineBytes(10);
        QVERIFY(transport.open());
        QList<QByteArray> lines;
        int tooLong = 0;
        connect(&transport, &McpTransport::lineReceived, this, [&](const QByteArray& line) {
            lines.append(line);
        });
        connect(&transport, &McpTransport::lineTooLong, this, [&] {
            ++tooLong;
        });

        // Exactly the limit passes. One over, whole in one write, does not.
        QVERIFY(peer.sendRaw("0123456789\n01234567890\nok\n"));
        QVERIFY(spinUntil([&] {
            return lines.size() == 2;
        }));
        QCOMPARE(lines, (QList<QByteArray>{"0123456789", "ok"}));
        QCOMPARE(tooLong, 1);

        // Over the limit before its newline came: dropped up to the newline,
        // however many writes it takes.
        QVERIFY(peer.sendRaw("abcdefghijklmnop"));
        QVERIFY(spinUntil([&] {
            return tooLong == 2;
        }));
        QVERIFY(peer.sendRaw("still the same long line"));
        QVERIFY(peer.sendRaw(" and its end\nnext\n"));
        QVERIFY(spinUntil([&] {
            return lines.size() == 3;
        }));
        QCOMPARE(lines.last(), QByteArray("next"));
        QCOMPARE(tooLong, 2);
    }

    void longLineInManyChunks()
    {
        // A line of 3 MiB that arrives in pieces: taken whole, once.
        Peer peer;
        McpTransport transport(peer.serverFd(), peer.serverFd());
        QVERIFY(transport.open());
        QList<QByteArray> lines;
        connect(&transport, &McpTransport::lineReceived, this, [&](const QByteArray& line) {
            lines.append(line);
        });
        const QByteArray big(3 * 1024 * 1024, 'x');
        QVERIFY(peer.sendRaw(big + "\nafter\n"));
        QVERIFY(spinUntil([&] {
            return lines.size() == 2;
        }));
        QCOMPARE(lines.first().size(), big.size());
        QCOMPARE(lines.last(), QByteArray("after"));
    }

    void endOfInputKeepsTheLastLine()
    {
        Peer peer;
        McpTransport transport(peer.serverFd(), peer.serverFd());
        QVERIFY(transport.open());
        QList<QByteArray> lines;
        QSignalSpy ended(&transport, &McpTransport::endOfInput);
        connect(&transport, &McpTransport::lineReceived, this, [&](const QByteArray& line) {
            // The line comes before the end.
            QCOMPARE(ended.count(), 0);
            lines.append(line);
        });
        QVERIFY(peer.sendRaw("first\nlast without newline"));
        peer.closeWrite();
        QVERIFY(spinUntil([&] {
            return ended.count() == 1;
        }));
        QCOMPARE(lines, (QList<QByteArray>{"first", "last without newline"}));
    }

    void refusesANewlineInsideALine()
    {
        Peer peer;
        McpTransport transport(peer.serverFd(), peer.serverFd());
        QVERIFY(!transport.writeLine("not open"));
        QVERIFY(transport.open());
        QVERIFY(!transport.open());
        QVERIFY(!transport.writeLine("two\nlines"));
        QVERIFY(transport.writeLine("one line"));
        QCOMPARE(peer.read(1).first().value(QLatin1StringView("unparsed")).toString(), QStringLiteral("one line"));
    }

    void slowReaderDoesNotBlock()
    {
        // 16 MiB to a reader that takes 64 KiB at a time. writeLine returns
        // at once, timers run while the queue drains, and drained comes when
        // the reader took everything.
        Peer peer;
        McpTransport transport(peer.serverFd(), peer.serverFd());
        QVERIFY(transport.open());
        QSignalSpy drained(&transport, &McpTransport::drained);

        const QByteArray big(16 * 1024 * 1024, 'y');
        QVERIFY(transport.writeLine(big));
        QVERIFY(transport.pendingWriteBytes() > 0);
        QVERIFY(transport.writeLine("behind"));

        int ticks = 0;
        QTimer timer;
        connect(&timer, &QTimer::timeout, this, [&] {
            ++ticks;
        });
        timer.start(1);
        QVERIFY(spinUntil(
            [&] {
                peer.pump(64 * 1024);
                return peer.bytesRead() == big.size() + 1 + 7;
            },
            30000));
        QVERIFY(ticks > 5);
        QVERIFY(spinUntil([&] {
            return drained.count() == 1;
        }));
        QCOMPARE(transport.pendingWriteBytes(), 0);
    }

    void closedReaderIsNotFatal()
    {
        // A pipe whose reader has gone, with SIGPIPE at its default action,
        // which would end the test: the write fails with EPIPE instead, and
        // no SIGPIPE stays pending.
        struct sigaction old;
        struct sigaction defaultAction = {};
        defaultAction.sa_handler = SIG_DFL;
        sigaction(SIGPIPE, &defaultAction, &old);

        int fds[2];
        QCOMPARE(::pipe(fds), 0);
        ::close(fds[0]);
        const int inputFd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        {
            McpTransport transport(inputFd, fds[1]);
            QVERIFY(transport.open());
            QSignalSpy closed(&transport, &McpTransport::outputClosed);
            // Queued, never from inside writeLine.
            QVERIFY(transport.writeLine("into the void"));
            QCOMPARE(closed.count(), 0);
            QVERIFY(spinUntil([&] {
                return closed.count() == 1;
            }));
            QCOMPARE(closed.first().first().toInt(), EPIPE);
            QVERIFY(!transport.writeLine("again"));
        }
        sigset_t pending;
        sigemptyset(&pending);
        sigpending(&pending);
        QCOMPARE(sigismember(&pending, SIGPIPE), 0);

        ::close(fds[1]);
        ::close(inputFd);
        sigaction(SIGPIPE, &old, nullptr);
    }

    void restoresTheWriteFlags()
    {
        Peer peer;
        const int before = ::fcntl(peer.serverFd(), F_GETFL);
        QCOMPARE(before & O_NONBLOCK, 0);
        {
            McpTransport transport(peer.serverFd(), peer.serverFd());
            QVERIFY(transport.open());
            QVERIFY(::fcntl(peer.serverFd(), F_GETFL) & O_NONBLOCK);
        }
        QCOMPARE(::fcntl(peer.serverFd(), F_GETFL), before);
    }

    void refusesAClosedDescriptor()
    {
        McpTransport transport(-1, -1);
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("read descriptor")));
        QVERIFY(!transport.open());
    }
};

QTEST_GUILESS_MAIN(TestPhosphorMcpTransport)
#include "test_phosphor_mcp_transport.moc"
