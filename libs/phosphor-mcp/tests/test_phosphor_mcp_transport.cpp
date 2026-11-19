// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// McpTransport on its own: lines in, lines out, the line limit, the end of
// the input, a slow reader and a reader that went away.

#include "mcptesthelpers.h"

#include <PhosphorMcp/McpTransport.h>

#include <QByteArray>
#include <QDir>
#include <QList>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace PhosphorMcp;
using PhosphorMcpTests::Peer;
using PhosphorMcpTests::spinFor;
using PhosphorMcpTests::spinUntil;

namespace {

volatile sig_atomic_t g_sigpipeHandled = 0;

void countSigpipe(int)
{
    g_sigpipeHandled = g_sigpipeHandled + 1;
}

bool sigpipePending()
{
    sigset_t pending;
    sigemptyset(&pending);
    sigpending(&pending);
    return sigismember(&pending, SIGPIPE) == 1;
}

// SIGPIPE goes to countSigpipe until the guard that this returns is gone. A
// SIGPIPE that a write left pending and unblocked then shows as a count
// instead of ending the test.
auto countSigpipes()
{
    g_sigpipeHandled = 0;
    struct sigaction counting = {};
    counting.sa_handler = countSigpipe;
    sigemptyset(&counting.sa_mask);
    struct sigaction old = {};
    sigaction(SIGPIPE, &counting, &old);
    return qScopeGuard([old] {
        sigaction(SIGPIPE, &old, nullptr);
    });
}

// A pipe whose descriptors close with the guard.
struct Pipe
{
    int fds[2] = {-1, -1};
    Pipe()
    {
        if (::pipe2(fds, O_CLOEXEC) != 0) {
            fds[0] = fds[1] = -1;
        }
    }
    ~Pipe()
    {
        closeRead();
        closeWrite();
    }
    Q_DISABLE_COPY_MOVE(Pipe)
    [[nodiscard]] bool isValid() const
    {
        return fds[0] >= 0 && fds[1] >= 0;
    }
    [[nodiscard]] int readFd() const
    {
        return fds[0];
    }
    [[nodiscard]] int writeFd() const
    {
        return fds[1];
    }
    void closeRead()
    {
        if (fds[0] >= 0) {
            ::close(fds[0]);
            fds[0] = -1;
        }
    }
    void closeWrite()
    {
        if (fds[1] >= 0) {
            ::close(fds[1]);
            fds[1] = -1;
        }
    }
};

int openDevNull()
{
    return ::open("/dev/null", O_RDONLY | O_CLOEXEC);
}

qsizetype openDescriptorCount()
{
    return QDir(QStringLiteral("/proc/self/fd"))
        .entryList(QDir::AllEntries | QDir::System | QDir::NoDotAndDotDot)
        .size();
}

} // namespace

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
        QVERIFY(peer.isValid());
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
        // however many reads it takes. The middle part is read on its own,
        // without a newline, while the transport discards.
        QVERIFY(peer.sendRaw("abcdefghijklmnop"));
        QVERIFY(spinUntil([&] {
            return tooLong == 2;
        }));
        QVERIFY(peer.sendRaw("still the same long line"));
        QVERIFY(peer.waitUntilServerRead());
        QVERIFY(peer.sendRaw(" and its end\nnext\n"));
        QVERIFY(spinUntil([&] {
            return lines.size() == 3;
        }));
        QCOMPARE(lines.last(), QByteArray("next"));
        QCOMPARE(tooLong, 2);
    }

    void carriageReturnAtTheLimit()
    {
        // A line of exactly the limit, whose CR LF is cut between two reads,
        // still passes: the carriage return may be the start of its CR LF.
        Peer peer;
        QVERIFY(peer.isValid());
        McpTransport transport(peer.serverFd(), peer.serverFd());
        transport.setMaxLineBytes(10);
        QVERIFY(transport.open());
        QList<QByteArray> lines;
        QSignalSpy tooLong(&transport, &McpTransport::lineTooLong);
        QSignalSpy ended(&transport, &McpTransport::endOfInput);
        connect(&transport, &McpTransport::lineReceived, this, [&](const QByteArray& line) {
            lines.append(line);
        });

        QVERIFY(peer.sendRaw("0123456789\r"));
        QVERIFY(peer.waitUntilServerRead());
        spinFor(20);
        QVERIFY(peer.sendRaw("\n"));
        QVERIFY(spinUntil([&] {
            return lines.size() == 1;
        }));
        QCOMPARE(lines.first(), QByteArray("0123456789"));

        // The same at the end of the input, without a newline: the carriage
        // return goes, and the line is the limit.
        QVERIFY(peer.sendRaw("abcdefghij\r"));
        peer.closeWrite();
        QVERIFY(spinUntil([&] {
            return ended.count() == 1;
        }));
        QCOMPARE(lines, (QList<QByteArray>{"0123456789", "abcdefghij"}));
        QCOMPARE(tooLong.count(), 0);
    }

    void overTheLimitAtTheEnd()
    {
        // A last line without a newline and over the limit is too long, and
        // the end still comes after it.
        Peer peer;
        QVERIFY(peer.isValid());
        McpTransport transport(peer.serverFd(), peer.serverFd());
        transport.setMaxLineBytes(10);
        QVERIFY(transport.open());
        QSignalSpy lines(&transport, &McpTransport::lineReceived);
        QSignalSpy tooLong(&transport, &McpTransport::lineTooLong);
        QSignalSpy ended(&transport, &McpTransport::endOfInput);
        QVERIFY(peer.sendRaw("01234567890"));
        peer.closeWrite();
        QVERIFY(spinUntil([&] {
            return ended.count() == 1;
        }));
        QCOMPARE(tooLong.count(), 1);
        QCOMPARE(lines.count(), 0);
    }

    void longLineInManyChunks()
    {
        // A line of 3 MiB that arrives in pieces: taken whole, once.
        Peer peer;
        QVERIFY(peer.isValid());
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
        QVERIFY(peer.isValid());
        McpTransport transport(peer.serverFd(), peer.serverFd());
        QVERIFY(transport.open());
        QList<QByteArray> lines;
        QSignalSpy ended(&transport, &McpTransport::endOfInput);
        connect(&transport, &McpTransport::lineReceived, this, [&](const QByteArray& line) {
            // The line comes before the end.
            QCOMPARE(ended.count(), 0);
            lines.append(line);
        });
        QVERIFY(peer.sendRaw("first\nlast without newline\r"));
        peer.closeWrite();
        QVERIFY(spinUntil([&] {
            return ended.count() == 1;
        }));
        QCOMPARE(lines, (QList<QByteArray>{"first", "last without newline"}));
    }

    void stopReadingInsideALine()
    {
        // A slot that stops reading gets no more lines, and no end, even when
        // they were read in the same chunk.
        Peer peer;
        QVERIFY(peer.isValid());
        McpTransport transport(peer.serverFd(), peer.serverFd());
        QVERIFY(transport.open());
        QList<QByteArray> lines;
        QSignalSpy ended(&transport, &McpTransport::endOfInput);
        connect(&transport, &McpTransport::lineReceived, this, [&](const QByteArray& line) {
            lines.append(line);
            transport.stopReading();
        });
        QVERIFY(peer.sendRaw("first\nsecond\n"));
        peer.closeWrite();
        QVERIFY(spinUntil([&] {
            return !lines.isEmpty();
        }));
        spinFor(100);
        QCOMPARE(lines, (QList<QByteArray>{"first"}));
        QCOMPARE(ended.count(), 0);
        // Writing goes on.
        QVERIFY(transport.writeLine("still writing"));
        QCOMPARE(peer.readOne().value(QLatin1StringView("unparsed")).toString(), QStringLiteral("still writing"));
    }

    void stopReadingInsideTheLastLine()
    {
        // The last line, without a newline, at the end of the input: a slot
        // that stops reading there gets no end.
        Peer peer;
        QVERIFY(peer.isValid());
        McpTransport transport(peer.serverFd(), peer.serverFd());
        QVERIFY(transport.open());
        QList<QByteArray> lines;
        QSignalSpy ended(&transport, &McpTransport::endOfInput);
        connect(&transport, &McpTransport::lineReceived, this, [&](const QByteArray& line) {
            lines.append(line);
            if (line == "last") {
                transport.stopReading();
            }
        });
        QVERIFY(peer.sendRaw("first\nlast"));
        peer.closeWrite();
        QVERIFY(spinUntil([&] {
            return lines.size() == 2;
        }));
        spinFor(100);
        QCOMPARE(ended.count(), 0);
    }

    void pauseInsideTheLastLineDefersTheEnd()
    {
        // A slot of the last line whose reply fills the queue past its mark
        // pauses reading. The end waits for the queue too, and comes when
        // the reader took the reply.
        Peer peer;
        QVERIFY(peer.isValid());
        McpTransport transport(peer.serverFd(), peer.serverFd());
        transport.setMaxPendingWriteBytes(1024);
        QVERIFY(transport.open());
        QSignalSpy ended(&transport, &McpTransport::endOfInput);
        const QByteArray reply(1024 * 1024, 'r');
        connect(&transport, &McpTransport::lineReceived, this, [&](const QByteArray&) {
            QVERIFY(transport.writeLine(reply));
        });
        QVERIFY(peer.sendRaw("last"));
        peer.closeWrite();
        QVERIFY(spinUntil([&] {
            return transport.isReadingPaused();
        }));
        spinFor(100);
        QCOMPARE(ended.count(), 0);
        QVERIFY(spinUntil(
            [&] {
                peer.pump();
                return ended.count() == 1;
            },
            10000));
        QCOMPARE(peer.bytesRead(), reply.size() + 1);
    }

    void endOfInputWhilePaused()
    {
        // The queue is past its mark when the client sends its last lines and
        // closes. They wait unread, and come, then the end, once the reader
        // took the queue down to half the mark.
        Peer peer;
        QVERIFY(peer.isValid());
        McpTransport transport(peer.serverFd(), peer.serverFd());
        transport.setMaxPendingWriteBytes(1024);
        QVERIFY(transport.open());
        QList<QByteArray> lines;
        QSignalSpy ended(&transport, &McpTransport::endOfInput);
        connect(&transport, &McpTransport::lineReceived, this, [&](const QByteArray& line) {
            QCOMPARE(ended.count(), 0);
            lines.append(line);
        });
        const QByteArray reply(1024 * 1024, 'r');
        QVERIFY(transport.writeLine(reply));
        QVERIFY(transport.isReadingPaused());
        QVERIFY(peer.sendRaw("x\ny\n"));
        peer.closeWrite();
        spinFor(100);
        QVERIFY(lines.isEmpty());
        QCOMPARE(ended.count(), 0);
        QVERIFY(spinUntil(
            [&] {
                peer.pump();
                return ended.count() == 1;
            },
            10000));
        QCOMPARE(lines, (QList<QByteArray>{"x", "y"}));
        QVERIFY(!transport.isReadingPaused());
    }

    void stopReadingBeforeOpen()
    {
        Peer peer;
        QVERIFY(peer.isValid());
        McpTransport transport(peer.serverFd(), peer.serverFd());
        transport.stopReading();
        QVERIFY(transport.open());
        QSignalSpy lines(&transport, &McpTransport::lineReceived);
        QVERIFY(peer.sendRaw("unread\n"));
        spinFor(100);
        QCOMPARE(lines.count(), 0);
        int unread = 0;
        QCOMPARE(::ioctl(peer.serverFd(), FIONREAD, &unread), 0);
        QCOMPARE(unread, 7);
    }

    void refusesANewlineInsideALine()
    {
        Peer peer;
        QVERIFY(peer.isValid());
        McpTransport transport(peer.serverFd(), peer.serverFd());
        QVERIFY(!transport.writeLine("not open"));
        QVERIFY(transport.open());
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("opened before")));
        QVERIFY(!transport.open());
        QVERIFY(!transport.writeLine("two\nlines"));
        QVERIFY(transport.writeLine("one line"));
        const QList<QJsonObject> messages = peer.read(1);
        QCOMPARE(messages.size(), 1);
        QCOMPARE(messages.first().value(QLatin1StringView("unparsed")).toString(), QStringLiteral("one line"));
    }

    void slowReaderDoesNotBlock()
    {
        // 4 MiB to a reader that takes 64 KiB at a time. writeLine returns
        // at once, timers run while the queue waits, and drained comes when
        // the last byte is written.
        Peer peer;
        QVERIFY(peer.isValid());
        McpTransport transport(peer.serverFd(), peer.serverFd());
        QVERIFY(transport.open());
        QSignalSpy drained(&transport, &McpTransport::drained);

        const QByteArray big(4 * 1024 * 1024, 'y');
        QVERIFY(transport.writeLine(big));
        QVERIFY(transport.writeLine("behind"));
        const qint64 queued = transport.pendingWriteBytes();
        QVERIFY(queued > big.size() / 2);

        // Before the reader takes anything: the event loop runs timers, and
        // the queue still waits.
        int ticks = 0;
        QTimer timer;
        connect(&timer, &QTimer::timeout, this, [&] {
            ++ticks;
        });
        timer.start(1);
        QVERIFY(spinUntil([&] {
            return ticks > 5;
        }));
        QCOMPARE(transport.pendingWriteBytes(), queued);
        QCOMPARE(drained.count(), 0);

        QVERIFY(spinUntil(
            [&] {
                peer.pump(64 * 1024);
                return peer.bytesRead() == big.size() + 1 + 7;
            },
            30000));
        QVERIFY(spinUntil([&] {
            return drained.count() == 1;
        }));
        QCOMPARE(transport.pendingWriteBytes(), 0);
    }

    void reportsWriteProgress()
    {
        // bytesWritten tells each step of a queue that waited, never a line
        // written whole inside writeLine(), and adds up to what waited.
        Peer peer;
        QVERIFY(peer.isValid());
        McpTransport transport(peer.serverFd(), peer.serverFd());
        QVERIFY(transport.open());
        QSignalSpy progress(&transport, &McpTransport::bytesWritten);
        QSignalSpy drained(&transport, &McpTransport::drained);

        QVERIFY(transport.writeLine("small"));
        QCOMPARE(transport.pendingWriteBytes(), 0);
        QCOMPARE(peer.read(1).size(), 1);
        QCOMPARE(progress.count(), 0);
        QCOMPARE(drained.count(), 0);

        const QByteArray big(1024 * 1024, 'p');
        QVERIFY(transport.writeLine(big));
        const qint64 queued = transport.pendingWriteBytes();
        QVERIFY(queued > 0);
        QCOMPARE(progress.count(), 0);
        QVERIFY(spinUntil(
            [&] {
                peer.pump(64 * 1024);
                return drained.count() == 1;
            },
            10000));
        QVERIFY(progress.count() > 1);
        qint64 total = 0;
        for (const QList<QVariant>& step : std::as_const(progress)) {
            QVERIFY(step.first().toLongLong() > 0);
            total += step.first().toLongLong();
        }
        QCOMPARE(total, queued);
    }

    void lineLargerThanTheMarkIsWrittenWhole()
    {
        Peer peer;
        QVERIFY(peer.isValid());
        McpTransport transport(peer.serverFd(), peer.serverFd());
        transport.setMaxPendingWriteBytes(1024);
        QVERIFY(transport.open());
        QByteArray big(1024 * 1024, 'w');
        big[0] = 'a';
        big[big.size() - 1] = 'z';
        QVERIFY(transport.writeLine(big));
        QVERIFY(transport.isReadingPaused());
        QByteArray received;
        QVERIFY(spinUntil(
            [&] {
                QByteArray chunk(64 * 1024, Qt::Uninitialized);
                const ssize_t got = ::read(peer.clientFd(), chunk.data(), size_t(chunk.size()));
                if (got > 0) {
                    received.append(chunk.constData(), got);
                }
                return received.size() == big.size() + 1;
            },
            10000));
        QCOMPARE(received, big + '\n');
        QVERIFY(spinUntil([&] {
            return !transport.isReadingPaused();
        }));
        QCOMPARE(transport.pendingWriteBytes(), 0);
    }

    void newMarkAppliesAtOnce()
    {
        // A raised mark resumes reading without the reader taking anything,
        // and a lowered one pauses it again.
        Peer peer;
        QVERIFY(peer.isValid());
        McpTransport transport(peer.serverFd(), peer.serverFd());
        transport.setMaxPendingWriteBytes(1024);
        QVERIFY(transport.open());
        QSignalSpy lines(&transport, &McpTransport::lineReceived);
        QVERIFY(transport.writeLine(QByteArray(1024 * 1024, 'm')));
        QVERIFY(transport.isReadingPaused());
        QVERIFY(peer.sendRaw("waiting\n"));
        spinFor(100);
        QCOMPARE(lines.count(), 0);

        transport.setMaxPendingWriteBytes(64 * 1024 * 1024);
        QCOMPARE(transport.maxPendingWriteBytes(), 64 * 1024 * 1024);
        QVERIFY(!transport.isReadingPaused());
        QVERIFY(spinUntil([&] {
            return lines.count() == 1;
        }));

        transport.setMaxPendingWriteBytes(1024);
        QVERIFY(transport.isReadingPaused());
        QVERIFY(peer.sendRaw("waiting again\n"));
        spinFor(100);
        QCOMPARE(lines.count(), 1);
    }

    void limitsAreAtLeastOne()
    {
        McpTransport transport(-1, -1);
        QCOMPARE(transport.maxLineBytes(), McpTransport::DefaultMaxLineBytes);
        QCOMPARE(transport.maxPendingWriteBytes(), McpTransport::DefaultMaxPendingWriteBytes);
        transport.setMaxLineBytes(0);
        transport.setMaxPendingWriteBytes(-5);
        QCOMPARE(transport.maxLineBytes(), 1);
        QCOMPARE(transport.maxPendingWriteBytes(), 1);
    }

    void closeDropsTheQueue()
    {
        // close() with lines still queued drops them, and leaves the
        // descriptor open: the reader gets part of the line and no end.
        Peer peer;
        QVERIFY(peer.isValid());
        McpTransport transport(peer.serverFd(), peer.serverFd());
        QVERIFY(transport.open());
        QSignalSpy drained(&transport, &McpTransport::drained);
        const QByteArray big(1024 * 1024, 'c');
        QVERIFY(transport.writeLine(big));
        QVERIFY(transport.pendingWriteBytes() > 0);
        transport.close();
        QVERIFY(!transport.isOpen());
        QCOMPARE(transport.pendingWriteBytes(), 0);
        QVERIFY(!transport.writeLine("after close"));
        QVERIFY(::fcntl(peer.serverFd(), F_GETFD) >= 0);

        spinUntil(
            [&] {
                return peer.pump() == 0;
            },
            1000);
        spinFor(100);
        peer.pump();
        QVERIFY(peer.bytesRead() > 0);
        QVERIFY(peer.bytesRead() < big.size() + 1);
        QVERIFY(!peer.ended());
        QCOMPARE(drained.count(), 0);
    }

    void readErrorGivesInputFailed()
    {
        // A client that goes away with a reply unread resets the socket: the
        // next read fails with ECONNRESET, and reading stops.
        Peer peer;
        QVERIFY(peer.isValid());
        McpTransport transport(peer.serverFd(), peer.serverFd());
        QVERIFY(transport.open());
        QSignalSpy failed(&transport, &McpTransport::inputFailed);
        QSignalSpy ended(&transport, &McpTransport::endOfInput);
        QVERIFY(transport.writeLine("never read"));
        QCOMPARE(transport.pendingWriteBytes(), 0);
        peer.closeClient();
        QVERIFY(spinUntil([&] {
            return failed.count() == 1;
        }));
        QCOMPARE(failed.first().first().toInt(), ECONNRESET);
        spinFor(50);
        QCOMPARE(failed.count(), 1);
        QCOMPARE(ended.count(), 0);
    }

    void twoPipes()
    {
        // The shape McpServer::start() gives: stdin and stdout, two pipes.
        Pipe input;
        Pipe output;
        QVERIFY(input.isValid() && output.isValid());
        const int inputFlags = ::fcntl(input.readFd(), F_GETFL);
        QCOMPARE(::fcntl(output.writeFd(), F_GETFL) & O_NONBLOCK, 0);
        QList<QByteArray> lines;
        {
            McpTransport transport(input.readFd(), output.writeFd());
            QVERIFY(transport.open());
            QSignalSpy ended(&transport, &McpTransport::endOfInput);
            connect(&transport, &McpTransport::lineReceived, this, [&](const QByteArray& line) {
                lines.append(line);
                QVERIFY(transport.writeLine("reply to " + line));
            });
            QCOMPARE(::fcntl(input.readFd(), F_GETFL), inputFlags);
            QVERIFY(::fcntl(output.writeFd(), F_GETFL) & O_NONBLOCK);
            QCOMPARE(::write(input.writeFd(), "request\n", 8), 8);
            input.closeWrite();
            QVERIFY(spinUntil([&] {
                return ended.count() == 1;
            }));
            QCOMPARE(lines, (QList<QByteArray>{"request"}));
            char buffer[64];
            const ssize_t got = ::read(output.readFd(), buffer, sizeof buffer);
            QCOMPARE(QByteArray(buffer, int(qMax<ssize_t>(got, 0))), QByteArray("reply to request\n"));
        }
        QCOMPARE(::fcntl(output.writeFd(), F_GETFL) & O_NONBLOCK, 0);
        QCOMPARE(::fcntl(input.readFd(), F_GETFL), inputFlags);
    }

    void writeFailureStopsTheLines()
    {
        // The first line's reply fails: the lines read after it are dropped,
        // and no handler runs for them before outputClosed comes.
        Peer input;
        Peer output;
        QVERIFY(input.isValid() && output.isValid());
        output.closeClient();
        QVERIFY(input.sendRaw("a\nb\nc\n"));
        McpTransport transport(input.serverFd(), output.serverFd());
        QVERIFY(transport.open());
        QList<QByteArray> lines;
        QSignalSpy closed(&transport, &McpTransport::outputClosed);
        QSignalSpy ended(&transport, &McpTransport::endOfInput);
        connect(&transport, &McpTransport::lineReceived, this, [&](const QByteArray& line) {
            lines.append(line);
            transport.writeLine("reply");
        });
        QVERIFY(spinUntil([&] {
            return closed.count() == 1;
        }));
        QCOMPARE(closed.first().first().toInt(), EPIPE);
        QCOMPARE(lines, (QList<QByteArray>{"a"}));
        QVERIFY(input.sendRaw("d\n"));
        input.closeWrite();
        spinFor(100);
        QCOMPARE(lines, (QList<QByteArray>{"a"}));
        QCOMPARE(ended.count(), 0);
    }

    void closedPipeReaderIsNotFatal()
    {
        // A pipe whose reader has gone, with SIGPIPE at its default action,
        // which would end the test: the write fails with EPIPE instead, and
        // no SIGPIPE stays pending.
        struct sigaction old = {};
        struct sigaction defaultAction = {};
        defaultAction.sa_handler = SIG_DFL;
        sigaction(SIGPIPE, &defaultAction, &old);
        const auto restore = qScopeGuard([old] {
            sigaction(SIGPIPE, &old, nullptr);
        });

        Pipe output;
        QVERIFY(output.isValid());
        output.closeRead();
        const int inputFd = openDevNull();
        QVERIFY(inputFd >= 0);
        const auto closeInput = qScopeGuard([inputFd] {
            ::close(inputFd);
        });
        {
            McpTransport transport(inputFd, output.writeFd());
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
        QVERIFY(!sigpipePending());
    }

    void closedSocketReaderIsNotFatal()
    {
        // A socket is written with MSG_NOSIGNAL: no SIGPIPE at all.
        const auto restore = countSigpipes();
        Peer peer;
        QVERIFY(peer.isValid());
        McpTransport transport(peer.serverFd(), peer.serverFd());
        QVERIFY(transport.open());
        QSignalSpy closed(&transport, &McpTransport::outputClosed);
        peer.closeClient();
        QVERIFY(transport.writeLine("into the void"));
        QVERIFY(spinUntil([&] {
            return closed.count() == 1;
        }));
        QCOMPARE(closed.first().first().toInt(), EPIPE);
        QCOMPARE(int(g_sigpipeHandled), 0);
        QVERIFY(!sigpipePending());
    }

    void partialWriteTakesItsSigpipe()
    {
        // Someone else cleared O_NONBLOCK on the pipe's shared description.
        // The write then blocks with part of the line copied, and the reader
        // goes away: write() returns a count and SIGPIPE is pending. The
        // transport takes it back, and the process never sees it.
        const auto restore = countSigpipes();
        Pipe output;
        QVERIFY(output.isValid());
        const int inputFd = openDevNull();
        QVERIFY(inputFd >= 0);
        const auto closeInput = qScopeGuard([inputFd] {
            ::close(inputFd);
        });
        McpTransport transport(inputFd, output.writeFd());
        QVERIFY(transport.open());
        QSignalSpy closed(&transport, &McpTransport::outputClosed);
        const int flags = ::fcntl(output.writeFd(), F_GETFL);
        QCOMPARE(::fcntl(output.writeFd(), F_SETFL, flags & ~O_NONBLOCK), 0);

        // Closes the reader once the write has filled the pipe and blocks.
        const int readFd = output.readFd();
        std::thread closer([readFd] {
            for (int i = 0; i < 5000; ++i) {
                int unread = 0;
                if (::ioctl(readFd, FIONREAD, &unread) == 0 && unread > 0) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            ::close(readFd);
        });
        const auto join = qScopeGuard([&] {
            closer.join();
            output.fds[0] = -1;
        });
        QVERIFY(transport.writeLine(QByteArray(1024 * 1024, 's')));
        QVERIFY(spinUntil([&] {
            return closed.count() == 1;
        }));
        QCOMPARE(closed.first().first().toInt(), EPIPE);
        QCOMPARE(int(g_sigpipeHandled), 0);
        QVERIFY(!sigpipePending());
    }

    void foreignSigpipeStaysPending()
    {
        // A SIGPIPE that was pending before the write is someone else's: the
        // transport leaves it.
        const auto restore = countSigpipes();
        sigset_t pipeSet;
        sigemptyset(&pipeSet);
        sigaddset(&pipeSet, SIGPIPE);
        sigset_t oldMask;
        QCOMPARE(pthread_sigmask(SIG_BLOCK, &pipeSet, &oldMask), 0);
        const auto unblock = qScopeGuard([&] {
            const timespec noWait{0, 0};
            while (sigtimedwait(&pipeSet, nullptr, &noWait) > 0) { }
            pthread_sigmask(SIG_SETMASK, &oldMask, nullptr);
        });
        QCOMPARE(::raise(SIGPIPE), 0);
        QVERIFY(sigpipePending());

        Pipe output;
        QVERIFY(output.isValid());
        output.closeRead();
        const int inputFd = openDevNull();
        QVERIFY(inputFd >= 0);
        const auto closeInput = qScopeGuard([inputFd] {
            ::close(inputFd);
        });
        McpTransport transport(inputFd, output.writeFd());
        QVERIFY(transport.open());
        QSignalSpy closed(&transport, &McpTransport::outputClosed);
        QVERIFY(transport.writeLine("into the void"));
        QVERIFY(spinUntil([&] {
            return closed.count() == 1;
        }));
        QVERIFY(sigpipePending());
        QCOMPARE(int(g_sigpipeHandled), 0);
    }

    void socketFlagsNeverChange()
    {
        Peer peer;
        QVERIFY(peer.isValid());
        const int before = ::fcntl(peer.serverFd(), F_GETFL);
        QCOMPARE(before & O_NONBLOCK, 0);
        {
            McpTransport transport(peer.serverFd(), peer.serverFd());
            QVERIFY(transport.open());
            QCOMPARE(::fcntl(peer.serverFd(), F_GETFL), before);
        }
        QCOMPARE(::fcntl(peer.serverFd(), F_GETFL), before);
    }

    void pipeFlagsComeBack()
    {
        // close() clears the O_NONBLOCK that open() set, and nothing else:
        // a flag that another holder of the description set meanwhile stays.
        Pipe output;
        QVERIFY(output.isValid());
        const int inputFd = openDevNull();
        QVERIFY(inputFd >= 0);
        const auto closeInput = qScopeGuard([inputFd] {
            ::close(inputFd);
        });
        const int before = ::fcntl(output.writeFd(), F_GETFL);
        QCOMPARE(before & (O_NONBLOCK | O_APPEND), 0);
        {
            McpTransport transport(inputFd, output.writeFd());
            QVERIFY(transport.open());
            const int during = ::fcntl(output.writeFd(), F_GETFL);
            QVERIFY(during & O_NONBLOCK);
            QCOMPARE(::fcntl(output.writeFd(), F_SETFL, during | O_APPEND), 0);
        }
        QCOMPARE(::fcntl(output.writeFd(), F_GETFL), before | O_APPEND);

        // Non-blocking before open(): it stays so.
        QCOMPARE(::fcntl(output.writeFd(), F_SETFL, before | O_NONBLOCK), 0);
        {
            McpTransport transport(inputFd, output.writeFd());
            QVERIFY(transport.open());
        }
        QCOMPARE(::fcntl(output.writeFd(), F_GETFL), before | O_NONBLOCK);
    }

    void terminalKeepsItsFlags()
    {
        // A terminal's description is shared with the shell. The transport
        // writes through a description of its own, so the shared one never
        // turns non-blocking, not even for a moment.
        const int master = ::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
        if (master < 0 || ::grantpt(master) != 0 || ::unlockpt(master) != 0) {
            if (master >= 0) {
                ::close(master);
            }
            QSKIP("no pseudo-terminal here");
        }
        const auto closeMaster = qScopeGuard([master] {
            ::close(master);
        });
        const int terminal = ::open(::ptsname(master), O_RDWR | O_NOCTTY | O_CLOEXEC);
        QVERIFY(terminal >= 0);
        const auto closeTerminal = qScopeGuard([terminal] {
            ::close(terminal);
        });
        const int before = ::fcntl(terminal, F_GETFL);
        QCOMPARE(before & O_NONBLOCK, 0);
        const qsizetype descriptors = openDescriptorCount();
        {
            McpTransport transport(terminal, terminal);
            QVERIFY(transport.open());
            QCOMPARE(::fcntl(terminal, F_GETFL), before);
            QCOMPARE(openDescriptorCount(), descriptors + 1);
            QVERIFY(transport.writeLine("hello"));
            pollfd ready{master, POLLIN, 0};
            QCOMPARE(::poll(&ready, 1, 5000), 1);
            char buffer[64];
            const ssize_t got = ::read(master, buffer, sizeof buffer);
            QVERIFY(QByteArray(buffer, int(qMax<ssize_t>(got, 0))).startsWith("hello"));
        }
        QCOMPARE(::fcntl(terminal, F_GETFL), before);
        QCOMPARE(openDescriptorCount(), descriptors);
    }

    void ptyMasterIsWrittenAsAPipe()
    {
        // Opened again through /proc, a pty master is /dev/ptmx, which hands
        // out a new pty pair that nobody reads. The transport writes the
        // master itself, non-blocking until it closes.
        const int master = ::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
        if (master < 0 || ::grantpt(master) != 0 || ::unlockpt(master) != 0) {
            if (master >= 0) {
                ::close(master);
            }
            QSKIP("no pseudo-terminal here");
        }
        const auto closeMaster = qScopeGuard([master] {
            ::close(master);
        });
        const int terminal = ::open(::ptsname(master), O_RDWR | O_NOCTTY | O_CLOEXEC);
        QVERIFY(terminal >= 0);
        const auto closeTerminal = qScopeGuard([terminal] {
            ::close(terminal);
        });
        const int before = ::fcntl(master, F_GETFL);
        QCOMPARE(before & O_NONBLOCK, 0);
        {
            McpTransport transport(master, master);
            QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("is a pty master")));
            QVERIFY(transport.open());
            QCOMPARE(::fcntl(master, F_GETFL), before | O_NONBLOCK);
            QVERIFY(transport.writeLine("hello"));
            pollfd ready{terminal, POLLIN, 0};
            QCOMPARE(::poll(&ready, 1, 5000), 1);
            char buffer[64];
            const ssize_t got = ::read(terminal, buffer, sizeof buffer);
            QVERIFY(QByteArray(buffer, int(qMax<ssize_t>(got, 0))).startsWith("hello"));
        }
        QCOMPARE(::fcntl(master, F_GETFL), before);
    }

    void terminalThatCannotBeOpenedAgainIsWrittenAsAPipe()
    {
        // A terminal that another user owns cannot be opened again, but the
        // inherited descriptor still writes. The server runs, writing the
        // shared description non-blocking until it closes.
        const int master = ::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
        if (master < 0 || ::grantpt(master) != 0 || ::unlockpt(master) != 0) {
            if (master >= 0) {
                ::close(master);
            }
            QSKIP("no pseudo-terminal here");
        }
        const auto closeMaster = qScopeGuard([master] {
            ::close(master);
        });
        const int terminal = ::open(::ptsname(master), O_RDWR | O_NOCTTY | O_CLOEXEC);
        QVERIFY(terminal >= 0);
        struct stat mode = {};
        QCOMPARE(::fstat(terminal, &mode), 0);
        QCOMPARE(::fchmod(terminal, 0), 0);
        const auto restore = qScopeGuard([terminal, mode] {
            ::fchmod(terminal, mode.st_mode & 07777);
            ::close(terminal);
        });
        // A process that may ignore file modes (root, CAP_DAC_OVERRIDE) opens
        // it anyway, and then there is nothing to test.
        const QByteArray path = "/proc/self/fd/" + QByteArray::number(terminal);
        const int probe = ::open(path.constData(), O_WRONLY | O_NOCTTY | O_CLOEXEC);
        if (probe >= 0) {
            ::close(probe);
            QSKIP("this process opens the terminal whatever its mode");
        }
        const int before = ::fcntl(terminal, F_GETFL);
        QCOMPARE(before & O_NONBLOCK, 0);
        {
            McpTransport transport(terminal, terminal);
            QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("cannot open the terminal")));
            QVERIFY(transport.open());
            QCOMPARE(::fcntl(terminal, F_GETFL), before | O_NONBLOCK);
            QVERIFY(transport.writeLine("hello"));
            pollfd ready{master, POLLIN, 0};
            QCOMPARE(::poll(&ready, 1, 5000), 1);
            char buffer[64];
            const ssize_t got = ::read(master, buffer, sizeof buffer);
            QVERIFY(QByteArray(buffer, int(qMax<ssize_t>(got, 0))).startsWith("hello"));
        }
        QCOMPARE(::fcntl(terminal, F_GETFL), before);
    }

    void refusesAClosedDescriptor()
    {
        McpTransport transport(-1, -1);
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("read descriptor -1 is not open")));
        QVERIFY(!transport.open());

        Peer peer;
        QVERIFY(peer.isValid());
        McpTransport writeless(peer.serverFd(), -1);
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("write descriptor -1 is not open")));
        QVERIFY(!writeless.open());
        QVERIFY(!writeless.isOpen());
    }

    void refusesWhatCarriesNoLines()
    {
        // stdin or stdout closed when the application started: the
        // QCoreApplication's eventfd takes the number. It never gives a line,
        // and a reply written into it is lost.
        Peer peer;
        QVERIFY(peer.isValid());
        const int event = ::eventfd(0, EFD_CLOEXEC);
        QVERIFY(event >= 0);
        const auto closeEvent = qScopeGuard([event] {
            ::close(event);
        });
        McpTransport reading(event, peer.serverFd());
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("read descriptor \\d+ is not a pipe")));
        QVERIFY(!reading.open());
        McpTransport writing(peer.serverFd(), event);
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("write descriptor \\d+ is not a pipe")));
        QVERIFY(!writing.open());

        const int directory = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        QVERIFY(directory >= 0);
        const auto closeDirectory = qScopeGuard([directory] {
            ::close(directory);
        });
        McpTransport fromDirectory(directory, peer.serverFd());
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("read descriptor \\d+ is not a pipe")));
        QVERIFY(!fromDirectory.open());
    }

    void refusesAThreadWithoutAnEventDispatcher()
    {
        // A thread that Qt did not start has no event dispatcher, as the main
        // thread before QCoreApplication: the notifiers would never fire.
        Peer peer;
        QVERIFY(peer.isValid());
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("no event dispatcher")));
        bool opened = true;
        std::thread worker([&] {
            McpTransport transport(peer.serverFd(), peer.serverFd());
            opened = transport.open();
        });
        worker.join();
        QVERIFY(!opened);
    }
};

QTEST_GUILESS_MAIN(TestPhosphorMcpTransport)
#include "test_phosphor_mcp_transport.moc"
