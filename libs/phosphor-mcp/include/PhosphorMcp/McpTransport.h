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
// message per line. McpServer makes one on stdin and stdout, and a test
// gives it a socket pair.
//
// A QSocketNotifier on the read descriptor reads the input. Each complete
// line (without its newline, and without a carriage return before it) comes
// out as lineReceived. A line longer than maxLineBytes() is not kept. The
// input up to the next newline is dropped, and lineTooLong comes instead. At
// the end of the input, a last line without a newline comes out as a line,
// and then endOfInput.
//
// The transport reads nothing while it sends lines out. A slot that runs a
// nested event loop sees no new line until the lines already read have all
// gone out, each once, in order.
//
// Writing never blocks the event loop. A socket is written with
// MSG_DONTWAIT, and its flags stay as they are. A terminal is opened again,
// for a file description of the transport's own that is non-blocking, so
// the terminal that a shell shares with the program keeps its flags even
// when the program is killed. A terminal that cannot be opened again (one
// that another user owns, no /proc, a pty master), or that opens as another
// device or one it cannot identify, is treated as a pipe, with a warning.
// Any other write descriptor (a pipe, as an MCP client gives) is made
// non-blocking, and close() clears O_NONBLOCK again if open() set it. writeLine() writes what the descriptor takes now, and
// queues the rest, which a write notifier drains when the reader takes
// more. So a reply of many megabytes to a slow reader does not stop the
// program from answering other requests meanwhile. Their lines wait behind
// it in the queue.
//
// The queue is not dropped, and has a high-water mark instead
// (maxPendingWriteBytes). Past it, the transport stops reading, and goes on
// when the queue is down to half the mark. A client that sends requests and
// never reads the replies then fills its own pipe, not this process's
// memory. A single line larger than the mark is still written whole.
//
// A write to a closed pipe raises SIGPIPE, which ends the process unless
// the signal is ignored. The transport blocks SIGPIPE in its own thread
// around each write to a descriptor that is not a socket, and discards the
// one that the write raised, so a reader that went away gives outputClosed
// and never ends the process. A socket is written with MSG_NOSIGNAL. The
// signal's disposition, which is global to the process, is not changed.
//
// The transport is single-threaded. Use it on the thread that owns it, which
// needs an event dispatcher (a QCoreApplication on the main thread, or a
// QThread). The descriptors stay the caller's, and the transport never
// closes them.
class PHOSPHORMCP_EXPORT McpTransport : public QObject
{
    Q_OBJECT
public:
    static constexpr qint64 DefaultMaxLineBytes = 16 * 1024 * 1024;
    static constexpr qint64 DefaultMaxPendingWriteBytes = 64 * 1024 * 1024;

    McpTransport(int readFd, int writeFd, QObject* parent = nullptr);
    ~McpTransport() override;
    Q_DISABLE_COPY_MOVE(McpTransport)

    // Starts reading and writing. False, with a warning and nothing
    // changed, when the transport was opened before, when its thread has no
    // event dispatcher, when a descriptor is not open, when a descriptor is
    // not a pipe, a socket, a character device (a terminal) or a regular
    // file (an eventfd that took the number of a closed stdin or stdout, a
    // directory), and when the write descriptor cannot be made non-blocking.
    // A terminal that cannot be opened again is written as a pipe instead,
    // with a warning.
    bool open();
    // Stops reading and writing, drops the unwritten lines and puts the
    // write descriptor's O_NONBLOCK back as open() found it. Signals stop.
    // The transport cannot be opened again.
    void close();
    [[nodiscard]] bool isOpen() const;

    // Stops reading, and keeps writing what is queued. Before open(), the
    // transport opens without reading.
    void stopReading();
    // True while the write queue is past the high-water mark, and reading
    // waits for it to drain.
    [[nodiscard]] bool isReadingPaused() const;

    // The longest line that the transport takes, without its newline and a
    // carriage return before it. At least 1.
    [[nodiscard]] qint64 maxLineBytes() const;
    void setMaxLineBytes(qint64 bytes);

    // The high-water mark of the write queue, at least 1. Past it, reading
    // pauses until the queue is down to half of it. A new mark applies at
    // once: raised, it can resume reading, and lowered, it can pause it.
    [[nodiscard]] qint64 maxPendingWriteBytes() const;
    void setMaxPendingWriteBytes(qint64 bytes);

    // Queues `line` and a newline. False, and nothing queued, when the
    // transport is not open, after the output closed, and when `line`
    // holds a newline. The newline is appended to `line` itself, so pass a
    // temporary or std::move it.
    bool writeLine(QByteArray line);
    // Bytes queued and not yet written.
    [[nodiscard]] qint64 pendingWriteBytes() const;

Q_SIGNALS:
    void lineReceived(const QByteArray& line);
    void lineTooLong();
    // After the last line. Not after stopReading(), after inputFailed or
    // after outputClosed, and not while reading is paused, until it resumes.
    void endOfInput();
    // A read failed with an error other than EINTR or EAGAIN. Reading
    // stopped.
    void inputFailed(int error);
    // A write failed. The reader went away (EPIPE), or another error.
    // Writing and reading stopped, the lines read and not yet sent out are
    // dropped, and so is the queue. Sent from the event loop, never from
    // inside writeLine().
    void outputClosed(int error);
    // The write queue became empty after it had to wait for the reader. A
    // line written whole inside writeLine() never waited, and gives none.
    // Sent from the event loop, never from inside writeLine().
    void drained();
    // The reader took `bytes` more of the queue. Sent from the event loop
    // when the write notifier drains a queue that had to wait, never from
    // inside writeLine().
    void bytesWritten(qint64 bytes);

private:
    bool openOutput();
    // A non-blocking descriptor of the transport's own on the terminal that
    // m_writeFd writes to, or -1, with a warning, when there is none.
    [[nodiscard]] int reopenTerminal() const;
    void onReadable();
    void flush();
    void dispatchLines();
    [[nodiscard]] bool canDispatch() const;
    void scheduleDispatch();
    void updateReadNotifier();
    void updateReadPause();
    void failOutput(int error);

    int m_readFd;
    int m_writeFd;
    // The descriptor written. It is m_writeFd, or the transport's own
    // descriptor of a terminal, which close() closes.
    int m_outputFd = -1;
    bool m_ownsOutputFd = false;
    bool m_outputIsSocket = false;
    // open() set O_NONBLOCK on m_writeFd's file description, and close()
    // clears it.
    bool m_clearNonBlock = false;
    bool m_opened = false;
    bool m_closed = false;
    bool m_outputFailed = false;
    qint64 m_maxLineBytes = DefaultMaxLineBytes;
    qint64 m_maxPendingWriteBytes = DefaultMaxPendingWriteBytes;

    // Why the read notifier is off. These are stopReading(), the write queue
    // past its mark, lines going out, the input at its end and the output
    // failed.
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
