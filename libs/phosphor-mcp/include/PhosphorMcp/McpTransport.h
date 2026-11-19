// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <PhosphorMcp/phosphormcp_export.h>

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QtCore/qtclasshelpermacros.h>

QT_BEGIN_NAMESPACE
class QSocketNotifier;
QT_END_NAMESPACE

namespace PhosphorMcp {

// Lines on two file descriptors: the stdio transport of MCP, one JSON-RPC
// message per line. McpServer makes one on stdin and stdout; a test gives it
// a socket pair.
//
// Reading: a QSocketNotifier on the read descriptor. Each complete line
// (without its newline, and without a carriage return before it) comes out
// as lineReceived. A line longer than maxLineBytes() is not kept: the input
// up to the next newline is dropped, and lineTooLong comes instead. At the
// end of the input, a last line without a newline comes out as a line, and
// then endOfInput.
//
// The transport reads nothing while it sends lines out: a slot that runs a
// nested event loop sees no new line until the lines already read have all
// gone out, each once, in order.
//
// Writing never blocks the event loop. open() makes the write descriptor
// non-blocking (close() restores its flags). O_NONBLOCK belongs to the open
// file description: on a terminal that stdin and stderr share, they see it
// too while the transport is open. An MCP client gives pipes. writeLine() writes what the
// descriptor takes now, and queues the rest, which a write notifier drains
// when the reader takes more. So a reply of many megabytes to a slow reader
// does not stop the program from answering other requests meanwhile: their
// lines wait behind it in the queue.
//
// The queue is not dropped, and has a high-water mark instead
// (maxPendingWriteBytes). Past it, the transport stops reading, and goes on
// when the queue is down to half the mark. A client that sends requests and
// never reads the replies then fills its own pipe, not this process's
// memory. A single line larger than the mark is still written whole.
//
// A write to a closed pipe or socket raises SIGPIPE, which ends the process
// unless the signal is ignored. The transport blocks SIGPIPE in its own
// thread around each write and discards the one that the write raised, so a
// reader that went away gives outputClosed and never ends the process. The
// signal's disposition, which is global to the process, is not changed.
//
// Single-threaded: use it on the thread that owns it. The descriptors stay
// the caller's: the transport never closes them.
class PHOSPHORMCP_EXPORT McpTransport : public QObject
{
    Q_OBJECT
public:
    static constexpr qint64 DefaultMaxLineBytes = 16 * 1024 * 1024;
    static constexpr qint64 DefaultMaxPendingWriteBytes = 64 * 1024 * 1024;

    McpTransport(int readFd, int writeFd, QObject* parent = nullptr);
    ~McpTransport() override;
    Q_DISABLE_COPY_MOVE(McpTransport)

    // Starts reading and writing. False, with a warning, when a descriptor
    // is not open, or when the transport was opened before.
    bool open();
    // Stops reading and writing, drops the unwritten lines and restores the
    // write descriptor's flags. Signals stop. The transport cannot be opened
    // again.
    void close();
    [[nodiscard]] bool isOpen() const;

    // Stops reading, and keeps writing what is queued.
    void stopReading();
    // True while the write queue is past the high-water mark, and reading
    // waits for it to drain.
    [[nodiscard]] bool isReadingPaused() const;

    // The longest line that the transport takes, without its newline. At
    // least 1.
    [[nodiscard]] qint64 maxLineBytes() const;
    void setMaxLineBytes(qint64 bytes);

    // The high-water mark of the write queue, at least 1. Past it, reading
    // pauses until the queue is down to half of it.
    [[nodiscard]] qint64 maxPendingWriteBytes() const;
    void setMaxPendingWriteBytes(qint64 bytes);

    // Queues `line` and a newline. False, and nothing queued, when the
    // transport is not open, after the output closed, and when `line`
    // holds a newline.
    bool writeLine(const QByteArray& line);
    // Bytes queued and not yet written.
    [[nodiscard]] qint64 pendingWriteBytes() const;

Q_SIGNALS:
    void lineReceived(const QByteArray& line);
    void lineTooLong();
    void endOfInput();
    // A read failed with an error other than EINTR or EAGAIN. Reading
    // stopped.
    void inputFailed(int error);
    // A write failed: the reader went away (EPIPE) or another error.
    // Writing stopped and the queue was dropped. Sent from the event loop,
    // never from inside writeLine().
    void outputClosed(int error);
    // The write queue became empty.
    void drained();
    // The reader took `bytes` more of the queue. Sent from the event loop
    // when the write notifier drains a queue that had to wait, never from
    // inside writeLine().
    void bytesWritten(qint64 bytes);

private:
    void onReadable();
    void onWritable();
    void flush();
    void dispatchLines();
    void scheduleDispatch();
    void updateReadNotifier();
    void resumeReadingIfDrained();
    void failOutput(int error);

    int m_readFd;
    int m_writeFd;
    int m_savedWriteFlags = -1;
    bool m_opened = false;
    bool m_closed = false;
    bool m_outputFailed = false;
    qint64 m_maxLineBytes = DefaultMaxLineBytes;
    qint64 m_maxPendingWriteBytes = DefaultMaxPendingWriteBytes;

    // Why the read notifier is off: stopReading(), the write queue past its
    // mark, lines going out, the input at its end.
    bool m_readStopped = false;
    bool m_readPaused = false;
    bool m_dispatching = false;
    bool m_inputEnded = false;
    bool m_endReported = false;
    bool m_dispatchQueued = false;

    QSocketNotifier* m_readNotifier = nullptr;
    QSocketNotifier* m_writeNotifier = nullptr;

    // Input not yet sent out as lines, and how far it holds no newline.
    QByteArray m_input;
    qint64 m_scanned = 0;
    // Dropping input up to the next newline, after a line over the limit.
    bool m_discarding = false;

    QList<QByteArray> m_queue;
    // Bytes of m_queue.first() already written.
    qint64 m_queueOffset = 0;
    qint64 m_queuedBytes = 0;
};

} // namespace PhosphorMcp
