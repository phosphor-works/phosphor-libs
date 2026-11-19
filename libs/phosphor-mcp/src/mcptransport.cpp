// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorMcp/McpTransport.h>

#include "mcplogging.h"

#include <QAbstractEventDispatcher>
#include <QByteArray>
#include <QMetaObject>
#include <QPointer>
#include <QSocketNotifier>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <ctime>

#include <fcntl.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

// The library is Linux-only (sigtimedwait, MSG_NOSIGNAL, /proc/self/fd).
// There EWOULDBLOCK is EAGAIN, so the code tests EAGAIN alone.

namespace PhosphorMcp {

namespace {

// One read per wake-up: a client that sends without pause cannot keep the
// event loop in this function, so replies and timers run between reads.
constexpr qint64 ReadChunkBytes = 64 * 1024;

// Blocks SIGPIPE in this thread around a write to a pipe, and takes back a
// SIGPIPE that the write raised, so that a reader that went away is an
// EPIPE and not the end of the process. The signal disposition, which the
// whole process shares, stays as it is.
//
// Whether the write raised it is decided by the pending set, not by what
// write() returned. A blocking write that copied part of its data before the
// reader went away returns a count and still raises SIGPIPE. A SIGPIPE that
// was pending before the write belongs to someone else and stays.
//
// A copy of phosphor-channel's SigpipeBlock (libs/phosphor-channel/src/
// Stream.cpp). The two libraries do not depend on each other, so each has
// its own. Keep the two in step.
class SigpipeBlock
{
public:
    SigpipeBlock()
    {
        sigemptyset(&m_pipe);
        sigaddset(&m_pipe, SIGPIPE);
        sigset_t pending;
        sigpending(&pending);
        m_wasPending = sigismember(&pending, SIGPIPE) == 1;
        m_blocked = pthread_sigmask(SIG_BLOCK, &m_pipe, &m_old) == 0;
    }
    ~SigpipeBlock()
    {
        if (!m_blocked) {
            return;
        }
        if (!m_wasPending) {
            sigset_t pending;
            sigpending(&pending);
            if (sigismember(&pending, SIGPIPE) == 1) {
                const timespec now{0, 0};
                while (sigtimedwait(&m_pipe, nullptr, &now) < 0 && errno == EINTR) { }
            }
        }
        pthread_sigmask(SIG_SETMASK, &m_old, nullptr);
    }
    Q_DISABLE_COPY_MOVE(SigpipeBlock)

private:
    sigset_t m_pipe;
    sigset_t m_old;
    bool m_wasPending = false;
    bool m_blocked = false;
};

// Writes without blocking and without SIGPIPE. errno is write()'s.
ssize_t writeOut(int fd, bool isSocket, const char* data, size_t size)
{
    if (isSocket) {
        return ::send(fd, data, size, MSG_NOSIGNAL | MSG_DONTWAIT);
    }
    ssize_t written = -1;
    int error = 0;
    {
        const SigpipeBlock block;
        written = ::write(fd, data, size);
        error = errno;
    }
    errno = error;
    return written;
}

// What a line of the input can be read from, and its replies written to.
// The file type of an eventfd or another anonymous inode is 0. One takes the
// number of stdin or stdout when the application started with that closed:
// QCoreApplication's own eventfd, which would never give a line and would
// take the replies.
bool isStreamType(mode_t mode)
{
    return S_ISFIFO(mode) || S_ISSOCK(mode) || S_ISCHR(mode) || S_ISREG(mode);
}

// Checks one descriptor for open(). False, with a warning, when it is not
// open or cannot carry lines.
bool checkDescriptor(int fd, const char* role, struct stat* info)
{
    if (fd < 0 || ::fstat(fd, info) < 0) {
        qCWarning(lcMcp).nospace() << "McpTransport::open: the " << role << " descriptor " << fd << " is not open";
        return false;
    }
    if (!isStreamType(info->st_mode)) {
        qCWarning(lcMcp).nospace() << "McpTransport::open: the " << role << " descriptor " << fd
                                   << " is not a pipe, a socket, a character device or a regular file";
        return false;
    }
    return true;
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
        qCWarning(lcMcp) << "McpTransport::open: opened before";
        return false;
    }
    // The notifiers are dead without one: the transport would never read,
    // and a queued write would never go out.
    if (!QAbstractEventDispatcher::instance(thread())) {
        qCWarning(lcMcp) << "McpTransport::open: the thread has no event dispatcher (create the "
                            "QCoreApplication first)";
        return false;
    }
    struct stat readInfo{};
    struct stat writeInfo{};
    if (!checkDescriptor(m_readFd, "read", &readInfo) || !checkDescriptor(m_writeFd, "write", &writeInfo)) {
        return false;
    }
    m_outputIsSocket = S_ISSOCK(writeInfo.st_mode);
    if (!openOutput()) {
        return false;
    }
    m_opened = true;

    // The read descriptor stays as it is. Each wake-up reads once, and the
    // notifier says that a read will not block.
    m_readNotifier = new QSocketNotifier(m_readFd, QSocketNotifier::Read, this);
    connect(m_readNotifier, &QSocketNotifier::activated, this, &McpTransport::onReadable);
    // A stopReading() before open() holds.
    updateReadNotifier();
    m_writeNotifier = new QSocketNotifier(m_outputFd, QSocketNotifier::Write, this);
    m_writeNotifier->setEnabled(false);
    connect(m_writeNotifier, &QSocketNotifier::activated, this, &McpTransport::flush);
    return true;
}

int McpTransport::reopenTerminal() const
{
    // A pty master opened again through /proc is /dev/ptmx, which hands out
    // a new pty pair: replies would go to a terminal that nobody reads. Only
    // a master answers TIOCGPTN.
    unsigned int ptyNumber = 0;
    if (::ioctl(m_writeFd, TIOCGPTN, &ptyNumber) == 0) {
        qCWarning(lcMcp).nospace() << "McpTransport::open: the write descriptor " << m_writeFd
                                   << " is a pty master, so it is made non-blocking, and the flag comes back at close";
        return -1;
    }
    const QByteArray path = "/proc/self/fd/" + QByteArray::number(m_writeFd);
    const int fd = ::open(path.constData(), O_WRONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        qCWarning(lcMcp).nospace() << "McpTransport::open: cannot open the terminal " << m_writeFd << " again ("
                                   << strerror(errno)
                                   << "), so its shared description is made non-blocking until the transport closes";
        return -1;
    }
    // A device that hands out another terminal on open (/dev/tty0 after a
    // console switch, a master that TIOCGPTN did not name) is caught here.
    // fstat() would not do: it gives the number of the device node, which
    // both descriptors share. TIOCGDEV gives the terminal's own.
    unsigned int inherited = 0;
    unsigned int reopened = 0;
    if (::ioctl(m_writeFd, TIOCGDEV, &inherited) != 0 || ::ioctl(fd, TIOCGDEV, &reopened) != 0
        || inherited != reopened) {
        ::close(fd);
        qCWarning(lcMcp).nospace() << "McpTransport::open: the terminal " << m_writeFd
                                   << " opens as another device, so its shared description is made non-blocking"
                                   << " until the transport closes";
        return -1;
    }
    return fd;
}

bool McpTransport::openOutput()
{
    // send() takes MSG_DONTWAIT, so the socket's flags never change.
    if (m_outputIsSocket) {
        m_outputFd = m_writeFd;
        return true;
    }
    // A terminal's file description is shared with the shell that started
    // the program and with stdin and stderr. O_NONBLOCK there would stay
    // after the program is killed, and break the shell's next reads. A
    // description of the transport's own leaves it alone. When the terminal
    // cannot be opened again, the shared description is made non-blocking
    // as for a pipe, and close() puts the flag back: the inherited
    // descriptor still writes, so the server still runs.
    if (::isatty(m_writeFd)) {
        const int fd = reopenTerminal();
        if (fd >= 0) {
            m_outputFd = fd;
            m_ownsOutputFd = true;
            return true;
        }
    }
    const int flags = ::fcntl(m_writeFd, F_GETFL);
    if (flags < 0 || ((flags & O_NONBLOCK) == 0 && ::fcntl(m_writeFd, F_SETFL, flags | O_NONBLOCK) < 0)) {
        qCWarning(lcMcp).nospace() << "McpTransport::open: cannot make the write descriptor " << m_writeFd
                                   << " non-blocking (" << strerror(errno) << ")";
        return false;
    }
    m_outputFd = m_writeFd;
    m_clearNonBlock = (flags & O_NONBLOCK) == 0;
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
    if (m_ownsOutputFd) {
        ::close(m_outputFd);
        m_ownsOutputFd = false;
    } else if (m_clearNonBlock) {
        // Only the bit that open() set: whatever else changed on the shared
        // description since then is someone else's.
        const int flags = ::fcntl(m_writeFd, F_GETFL);
        if (flags >= 0 && (flags & O_NONBLOCK) != 0) {
            ::fcntl(m_writeFd, F_SETFL, flags & ~O_NONBLOCK);
        }
        m_clearNonBlock = false;
    }
    m_outputFd = -1;
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
    updateReadPause();
}

bool McpTransport::writeLine(QByteArray line)
{
    if (!isOpen() || m_outputFailed || line.contains('\n')) {
        return false;
    }
    line.append('\n');
    m_queuedBytes += line.size();
    m_queue.append(std::move(line));
    // Behind a queue that waits for the reader, a write now would only
    // fail with EAGAIN.
    if (!m_writeNotifier->isEnabled()) {
        flush();
    }
    // A client that sends requests and never reads the replies would grow
    // the queue without end. Past the mark, the requests wait in the pipe,
    // unread, until the client takes its replies. No reply is dropped.
    updateReadPause();
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
    m_readNotifier->setEnabled(canDispatch() && !m_dispatching && !m_inputEnded);
}

bool McpTransport::canDispatch() const
{
    // After a failed write, a line would only run a handler whose reply
    // cannot go out.
    return isOpen() && !m_readStopped && !m_readPaused && !m_outputFailed;
}

void McpTransport::updateReadPause()
{
    if (!m_readPaused && m_queuedBytes > m_maxPendingWriteBytes) {
        m_readPaused = true;
        updateReadNotifier();
    } else if (m_readPaused && m_queuedBytes <= m_maxPendingWriteBytes / 2) {
        // Half the mark: reading does not flap on and off at each reply.
        m_readPaused = false;
        updateReadNotifier();
        // The lines read before the pause and not yet dispatched. Queued:
        // this runs inside flush() or a setter, which may run inside a slot.
        scheduleDispatch();
    }
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
    if (error == EINTR || error == EAGAIN) {
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

    // Every line leaves m_input before its signal goes out, so the state is
    // whole whenever a slot runs, whatever the slot does.
    while (canDispatch()) {
        // m_scanned carries over the part of a long line already searched,
        // so a line of many chunks is searched once.
        const qint64 newline = m_input.indexOf('\n', m_scanned);
        qint64 taken = newline + 1;
        if (newline < 0) {
            m_scanned = m_input.size();
            // A carriage return at the end may be the start of the line's
            // CR LF, and does not count against the limit.
            const qint64 length = m_input.size() - (m_input.endsWith('\r') ? 1 : 0);
            if (length > m_maxLineBytes) {
                // No newline yet and already over: drop it now rather than
                // keep growing the buffer, and the rest up to the newline.
                m_input.clear();
                m_scanned = 0;
                m_discarding = !m_inputEnded;
                Q_EMIT lineTooLong();
                if (!self) {
                    return;
                }
                continue;
            }
            // At the end of the input, a last line without its newline still
            // counts, as a client that writes one request and closes would
            // expect.
            if (!m_inputEnded || m_input.isEmpty()) {
                break;
            }
            taken = m_input.size();
        }
        qint64 end = newline < 0 ? taken : newline;
        if (end > 0 && m_input.at(end - 1) == '\r') {
            --end;
        }
        const QByteArray line = m_input.left(end);
        m_input.remove(0, taken);
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

    // The end of the input comes after every line before it, and only when
    // a slot of the last one did not stop or pause reading. A pause that
    // ends dispatches again, and the end comes then.
    if (m_inputEnded && !m_endReported && canDispatch()) {
        m_endReported = true;
        Q_EMIT endOfInput();
        if (!self) {
            return;
        }
    }

    m_dispatching = false;
    updateReadNotifier();
}

void McpTransport::flush()
{
    // A queue that had to wait for the reader has the write notifier on, and
    // this runs from the notifier. From writeLine() the notifier is off, and
    // a line written whole there was never pending for anyone.
    const bool waited = m_writeNotifier->isEnabled();
    bool blocked = false;
    qint64 progress = 0;
    while (!m_queue.isEmpty()) {
        const QByteArray& head = m_queue.first();
        const ssize_t written = writeOut(m_outputFd, m_outputIsSocket, head.constData() + m_queueOffset,
                                         size_t(head.size() - m_queueOffset));
        if (written < 0) {
            const int error = errno;
            if (error == EINTR) {
                continue;
            }
            if (error == EAGAIN) {
                blocked = true;
                break;
            }
            failOutput(error);
            return;
        }
        progress += written;
        m_queueOffset += written;
        m_queuedBytes -= written;
        if (m_queueOffset == head.size()) {
            m_queue.removeFirst();
            m_queueOffset = 0;
        }
        updateReadPause();
    }
    m_writeNotifier->setEnabled(blocked);
    if (!waited) {
        return;
    }
    // From the write notifier, so from the event loop. A slot may write
    // more lines, close the transport or destroy it.
    const QPointer<McpTransport> self(this);
    if (progress > 0) {
        Q_EMIT bytesWritten(progress);
        if (!self || !isOpen()) {
            return;
        }
    }
    if (m_queue.isEmpty()) {
        Q_EMIT drained();
    }
}

void McpTransport::failOutput(int error)
{
    m_outputFailed = true;
    m_writeNotifier->setEnabled(false);
    m_queue.clear();
    m_queueOffset = 0;
    m_queuedBytes = 0;
    m_input.clear();
    m_scanned = 0;
    updateReadNotifier();
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
