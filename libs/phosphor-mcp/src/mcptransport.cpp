// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorMcp/McpTransport.h>

#include "mcplogging.h"

#include <QMetaObject>
#include <QPointer>
#include <QSocketNotifier>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <ctime>

#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

namespace PhosphorMcp {

namespace {

// One read per wake-up: a client that sends without pause cannot keep the
// event loop in this function, so replies and timers run between reads.
constexpr qint64 ReadChunkBytes = 64 * 1024;

// write() with SIGPIPE blocked in this thread. A write to a pipe or socket
// whose reader has gone raises SIGPIPE in the writing thread, and the default
// action ends the process. Blocked, the signal stays pending and the write
// fails with EPIPE; the pending signal is then taken off, unless one was
// pending before the write, which belongs to someone else.
ssize_t writeWithoutSigpipe(int fd, const char* data, size_t size)
{
    sigset_t pipeSet;
    sigemptyset(&pipeSet);
    sigaddset(&pipeSet, SIGPIPE);

    sigset_t pending;
    sigemptyset(&pending);
    sigpending(&pending);
    const bool pendingBefore = sigismember(&pending, SIGPIPE) == 1;

    sigset_t oldMask;
    pthread_sigmask(SIG_BLOCK, &pipeSet, &oldMask);
    const ssize_t written = ::write(fd, data, size);
    const int writeError = errno;
    if (written < 0 && writeError == EPIPE && !pendingBefore) {
        const timespec noWait{0, 0};
        while (sigtimedwait(&pipeSet, nullptr, &noWait) < 0 && errno == EINTR) { }
    }
    pthread_sigmask(SIG_SETMASK, &oldMask, nullptr);
    errno = writeError;
    return written;
}

// EWOULDBLOCK is EAGAIN on Linux and the BSDs, and comparing with both trips
// -Wlogical-op there.
bool wouldBlock(int error)
{
#if EWOULDBLOCK != EAGAIN
    if (error == EWOULDBLOCK) {
        return true;
    }
#endif
    return error == EAGAIN;
}

} // namespace

McpTransport::McpTransport(int readFd, int writeFd, QObject* parent)
    : QObject(parent)
    , m_readFd(readFd)
    , m_writeFd(writeFd)
{
}

McpTransport::~McpTransport()
{
    close();
}

bool McpTransport::open()
{
    if (m_opened) {
        qCWarning(lcMcp, "McpTransport::open: opened before");
        return false;
    }
    if (fcntl(m_readFd, F_GETFL) < 0) {
        qCWarning(lcMcp, "McpTransport::open: the read descriptor %d is not open", m_readFd);
        return false;
    }
    const int flags = fcntl(m_writeFd, F_GETFL);
    if (flags < 0) {
        qCWarning(lcMcp, "McpTransport::open: the write descriptor %d is not open", m_writeFd);
        return false;
    }
    if ((flags & O_NONBLOCK) == 0 && fcntl(m_writeFd, F_SETFL, flags | O_NONBLOCK) < 0) {
        qCWarning(lcMcp, "McpTransport::open: cannot make the write descriptor non-blocking");
        return false;
    }
    // Restored at close: the flag belongs to the open file description,
    // which the process that started this one may share (a terminal).
    m_savedWriteFlags = flags;
    m_opened = true;

    // The read descriptor stays as it is. Each wake-up reads once, and the
    // notifier says that a read will not block.
    m_readNotifier = new QSocketNotifier(m_readFd, QSocketNotifier::Read, this);
    connect(m_readNotifier, &QSocketNotifier::activated, this, &McpTransport::onReadable);
    m_writeNotifier = new QSocketNotifier(m_writeFd, QSocketNotifier::Write, this);
    m_writeNotifier->setEnabled(false);
    connect(m_writeNotifier, &QSocketNotifier::activated, this, &McpTransport::onWritable);
    return true;
}

void McpTransport::close()
{
    if (!m_opened || m_closed) {
        return;
    }
    m_closed = true;
    delete m_readNotifier;
    m_readNotifier = nullptr;
    delete m_writeNotifier;
    m_writeNotifier = nullptr;
    m_queue.clear();
    m_queueOffset = 0;
    m_queuedBytes = 0;
    m_input.clear();
    if (m_savedWriteFlags >= 0) {
        fcntl(m_writeFd, F_SETFL, m_savedWriteFlags);
        m_savedWriteFlags = -1;
    }
}

bool McpTransport::isOpen() const
{
    return m_opened && !m_closed;
}

void McpTransport::stopReading()
{
    m_readStopped = true;
    updateReadNotifier();
}

bool McpTransport::isReadingPaused() const
{
    return m_readPaused;
}

qint64 McpTransport::maxLineBytes() const
{
    return m_maxLineBytes;
}

void McpTransport::setMaxLineBytes(qint64 bytes)
{
    m_maxLineBytes = qMax<qint64>(1, bytes);
}

qint64 McpTransport::maxPendingWriteBytes() const
{
    return m_maxPendingWriteBytes;
}

void McpTransport::setMaxPendingWriteBytes(qint64 bytes)
{
    m_maxPendingWriteBytes = qMax<qint64>(1, bytes);
}

bool McpTransport::writeLine(const QByteArray& line)
{
    if (!isOpen() || m_outputFailed || line.contains('\n')) {
        return false;
    }
    QByteArray framed;
    framed.reserve(line.size() + 1);
    framed.append(line);
    framed.append('\n');
    m_queuedBytes += framed.size();
    m_queue.append(std::move(framed));
    // Behind a queue that waits for the reader, a write now would only
    // fail with EAGAIN.
    if (!m_writeNotifier->isEnabled()) {
        flush();
    }
    // A client that sends requests and never reads the replies would grow
    // the queue without end. Past the mark, the requests wait in the pipe,
    // unread, until the client takes its replies. No reply is dropped.
    if (!m_readPaused && m_queuedBytes > m_maxPendingWriteBytes) {
        m_readPaused = true;
        updateReadNotifier();
    }
    return true;
}

qint64 McpTransport::pendingWriteBytes() const
{
    return m_queuedBytes;
}

void McpTransport::updateReadNotifier()
{
    if (!m_readNotifier) {
        return;
    }
    // Off while lines go out, too: a slot that runs a nested event loop must
    // not read and dispatch more lines inside the outer dispatch.
    m_readNotifier->setEnabled(isOpen() && !m_readStopped && !m_readPaused && !m_dispatching && !m_inputEnded);
}

void McpTransport::onReadable()
{
    if (m_dispatching) {
        return;
    }
    char buffer[ReadChunkBytes];
    const ssize_t got = ::read(m_readFd, buffer, sizeof buffer);
    if (got > 0) {
        qint64 start = 0;
        if (m_discarding) {
            const char* newline = static_cast<const char*>(memchr(buffer, '\n', size_t(got)));
            if (!newline) {
                return;
            }
            m_discarding = false;
            start = newline - buffer + 1;
        }
        m_input.append(buffer + start, got - start);
        dispatchLines();
        return;
    }
    if (got == 0) {
        m_inputEnded = true;
        updateReadNotifier();
        dispatchLines();
        return;
    }
    const int error = errno;
    if (error == EINTR || wouldBlock(error)) {
        return;
    }
    stopReading();
    Q_EMIT inputFailed(error);
}

void McpTransport::scheduleDispatch()
{
    if (m_dispatchQueued) {
        return;
    }
    m_dispatchQueued = true;
    QMetaObject::invokeMethod(
        this,
        [this] {
            m_dispatchQueued = false;
            dispatchLines();
        },
        Qt::QueuedConnection);
}

void McpTransport::dispatchLines()
{
    if (m_dispatching) {
        return;
    }
    const QPointer<McpTransport> self(this);
    m_dispatching = true;
    updateReadNotifier();
    const auto canDispatch = [this] {
        return isOpen() && !m_readStopped && !m_readPaused;
    };

    // Every line leaves m_input before its signal goes out, so the state is
    // whole whenever a slot runs, whatever the slot does.
    while (canDispatch()) {
        // m_scanned carries over the part of a long line already searched,
        // so a line of many chunks is searched once.
        const qint64 newline = m_input.indexOf('\n', m_scanned);
        if (newline < 0) {
            m_scanned = m_input.size();
            if (m_input.size() > m_maxLineBytes) {
                // No newline yet and already over: drop it now rather than
                // keep growing the buffer, and the rest up to the newline.
                m_input.clear();
                m_scanned = 0;
                m_discarding = !m_inputEnded;
                Q_EMIT lineTooLong();
                if (!self) {
                    return;
                }
            }
            break;
        }
        qint64 end = newline;
        if (end > 0 && m_input.at(end - 1) == '\r') {
            --end;
        }
        const QByteArray line = m_input.left(end);
        m_input.remove(0, newline + 1);
        m_scanned = 0;
        if (line.size() > m_maxLineBytes) {
            Q_EMIT lineTooLong();
        } else {
            Q_EMIT lineReceived(line);
        }
        if (!self) {
            return;
        }
    }

    // The end of the input comes after every line before it: a last line
    // without its newline still counts, as a client that writes one request
    // and closes would expect.
    if (m_inputEnded && !m_endReported && canDispatch()) {
        m_endReported = true;
        if (!m_discarding && !m_input.isEmpty()) {
            QByteArray line = m_input;
            m_input.clear();
            m_scanned = 0;
            if (line.endsWith('\r')) {
                line.chop(1);
            }
            if (line.size() > m_maxLineBytes) {
                Q_EMIT lineTooLong();
            } else {
                Q_EMIT lineReceived(line);
            }
            if (!self) {
                return;
            }
        }
        if (isOpen()) {
            Q_EMIT endOfInput();
            if (!self) {
                return;
            }
        }
    }

    m_dispatching = false;
    updateReadNotifier();
}

void McpTransport::onWritable()
{
    flush();
}

void McpTransport::flush()
{
    // Only a queue that had to wait for the reader drains: a line written
    // whole inside writeLine() was never pending for anyone.
    const bool waited = m_writeNotifier->isEnabled();
    while (!m_queue.isEmpty()) {
        const QByteArray& head = m_queue.first();
        const ssize_t written =
            writeWithoutSigpipe(m_writeFd, head.constData() + m_queueOffset, size_t(head.size() - m_queueOffset));
        if (written < 0) {
            const int error = errno;
            if (error == EINTR) {
                continue;
            }
            if (wouldBlock(error)) {
                m_writeNotifier->setEnabled(true);
                return;
            }
            failOutput(error);
            return;
        }
        m_queueOffset += written;
        m_queuedBytes -= written;
        if (m_queueOffset == head.size()) {
            m_queue.removeFirst();
            m_queueOffset = 0;
        }
        resumeReadingIfDrained();
    }
    m_writeNotifier->setEnabled(false);
    if (waited) {
        // From the write notifier, so from the event loop.
        Q_EMIT drained();
    }
}

void McpTransport::resumeReadingIfDrained()
{
    // Half the mark: reading does not flap on and off at each reply.
    if (m_readPaused && m_queuedBytes <= m_maxPendingWriteBytes / 2) {
        m_readPaused = false;
        updateReadNotifier();
        // The lines read before the pause and not yet dispatched. Queued:
        // this runs inside flush(), which may run inside a slot.
        scheduleDispatch();
    }
}

void McpTransport::failOutput(int error)
{
    m_outputFailed = true;
    m_writeNotifier->setEnabled(false);
    m_queue.clear();
    m_queueOffset = 0;
    m_queuedBytes = 0;
    resumeReadingIfDrained();
    QMetaObject::invokeMethod(
        this,
        [this, error] {
            if (isOpen()) {
                Q_EMIT outputClosed(error);
            }
        },
        Qt::QueuedConnection);
}

} // namespace PhosphorMcp
