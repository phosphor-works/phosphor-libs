// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "mcpsignals.h"

#include "mcplogging.h"

#include <QAbstractEventDispatcher>
#include <QObject>
#include <QSocketNotifier>
#include <QThread>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <thread>

#include <fcntl.h>
#include <unistd.h>

namespace PhosphorMcp {

namespace {

// The handler may run on any thread, so what it reads is atomic, and atomics
// that are lock-free are safe in a signal handler.
static_assert(std::atomic<int>::is_always_lock_free);
static_assert(std::atomic<pid_t>::is_always_lock_free);

// The write end of the pipe, -1 while no instance holds the signals.
std::atomic<int> s_writeFd{-1};
// Handlers between their read of s_writeFd and their write. release() waits
// for them, so that no handler writes to a descriptor after it closed and
// its number went to another file.
std::atomic<int> s_handlersRunning{0};
// The process that installed the handlers. A child forked before exec
// inherits them, and the pipe with them.
std::atomic<pid_t> s_pid{0};
McpTerminationSignals* s_owner = nullptr;

// Written by install() before the handler goes in, and only read after.
struct sigaction s_oldTerm;
struct sigaction s_oldHup;
bool s_termHandled = false;
bool s_hupHandled = false;

extern "C" void onTerminationSignal(int signal)
{
    const int savedErrno = errno;
    if (::getpid() != s_pid.load()) {
        // A child forked before exec. The signal is the child's, not the
        // server's, so the child gets the disposition that it would have had
        // without the server. The signal is blocked while this runs, so the
        // raised one comes to that disposition when this returns. A child
        // that destroys its copy and installs its own handler at the moment
        // another of its threads runs this can lose the new handler to this
        // sigaction(). The window is a few instructions wide, and nothing
        // narrows it without a lock that a signal handler cannot take.
        sigaction(signal, signal == SIGTERM ? &s_oldTerm : &s_oldHup, nullptr);
        raise(signal);
        errno = savedErrno;
        return;
    }
    s_handlersRunning.fetch_add(1);
    const int fd = s_writeFd.load();
    if (fd >= 0) {
        const unsigned char byte = static_cast<unsigned char>(signal);
        // A full pipe already holds a byte that ends the server.
        [[maybe_unused]] const ssize_t written = ::write(fd, &byte, 1);
    }
    s_handlersRunning.fetch_sub(1);
    errno = savedErrno;
}

bool isOurs(const struct sigaction& action)
{
    return (action.sa_flags & SA_SIGINFO) == 0 && action.sa_handler == onTerminationSignal;
}

// Installs the handler for `signal`, unless the process ignores it. Keeps the
// disposition it found in `old`.
bool installUnlessIgnored(int signal, struct sigaction& old)
{
    sigaction(signal, nullptr, &old);
    if ((old.sa_flags & SA_SIGINFO) == 0 && old.sa_handler == SIG_IGN) {
        return false;
    }
    struct sigaction action = {};
    action.sa_handler = onTerminationSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    sigaction(signal, &action, nullptr);
    return true;
}

// Puts `old` back, unless someone installed another handler since: theirs
// stays.
void restoreIfOurs(int signal, bool handled, const struct sigaction& old)
{
    if (!handled) {
        return;
    }
    struct sigaction current;
    sigaction(signal, nullptr, &current);
    if (isOurs(current)) {
        sigaction(signal, &old, nullptr);
    }
}

} // namespace

std::unique_ptr<McpTerminationSignals> McpTerminationSignals::install(QObject* context,
                                                                      std::function<void(int)> callback)
{
    if (s_owner) {
        if (::getpid() != s_pid.load()) {
            qCWarning(lcMcp).noquote()
                << "McpServer::handleTerminationSignals: a server in the parent took the signals before the fork. "
                   "The child can take them once it has destroyed its copy of that server.";
        } else {
            qCWarning(lcMcp).noquote() << "McpServer::handleTerminationSignals: another server handles the signals";
        }
        return nullptr;
    }
    if (!context || !QAbstractEventDispatcher::instance(context->thread())) {
        // The notifier would never fire, and the signals would end nothing.
        qCWarning(lcMcp).noquote() << "McpServer::handleTerminationSignals: the server's thread has no event"
                                   << "dispatcher, so create the QCoreApplication first";
        return nullptr;
    }
    int fds[2];
    if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0) {
        qCWarning(lcMcp).noquote() << "McpServer::handleTerminationSignals: cannot make the self-pipe";
        return nullptr;
    }
    std::unique_ptr<McpTerminationSignals> self(new McpTerminationSignals);
    self->m_callback = std::move(callback);
    self->m_readFd = fds[0];
    self->m_writeFd = fds[1];
    if (::getpid() != s_pid.load()) {
        // A child forked while a handler ran on another of the parent's
        // threads keeps that handler's count, which nothing in the child
        // lowers. No handler counts here until s_pid names this process.
        s_handlersRunning.store(0);
    }
    s_pid.store(::getpid());
    s_writeFd.store(fds[1]);
    s_owner = self.get();

    s_termHandled = installUnlessIgnored(SIGTERM, s_oldTerm);
    s_hupHandled = installUnlessIgnored(SIGHUP, s_oldHup);

    self->m_notifier = new QSocketNotifier(fds[0], QSocketNotifier::Read, context);
    McpTerminationSignals* raw = self.get();
    QObject::connect(self->m_notifier, &QSocketNotifier::activated, self->m_notifier, [raw] {
        raw->onReadable();
    });
    return self;
}

McpTerminationSignals::~McpTerminationSignals()
{
    release();
    delete m_notifier.data();
    ::close(m_readFd);
    ::close(m_writeFd);
}

bool McpTerminationSignals::belongsToThisProcess() const
{
    return ::getpid() == s_pid.load();
}

void McpTerminationSignals::release()
{
    if (m_released) {
        return;
    }
    m_released = true;
    // The old dispositions first, so that a signal from here on no longer
    // reaches the pipe.
    restoreIfOurs(SIGTERM, s_termHandled, s_oldTerm);
    restoreIfOurs(SIGHUP, s_hupHandled, s_oldHup);
    s_termHandled = false;
    s_hupHandled = false;
    s_writeFd.store(-1);
    // A handler on another thread may have read the descriptor just before.
    // Its write is one non-blocking byte, so this wait is short. In a child
    // forked after install() no handler counts, and a count there is the
    // parent's, from a handler that ran when it forked.
    while (belongsToThisProcess() && s_handlersRunning.load() != 0) {
        std::this_thread::yield();
    }
    if (m_notifier) {
        m_notifier->setEnabled(false);
    }
    s_owner = nullptr;
}

void McpTerminationSignals::onReadable()
{
    if (!belongsToThisProcess()) {
        // A child forked after install() shares the pipe, and the bytes in
        // it are the parent's signals. The child leaves them to the parent.
        if (m_notifier) {
            m_notifier->setEnabled(false);
        }
        return;
    }
    unsigned char bytes[16];
    const ssize_t got = ::read(m_readFd, bytes, sizeof bytes);
    if (got <= 0 || m_released) {
        return;
    }
    // The first signal ends the server, which releases the signals before
    // its cancel callbacks run. A signal while the replies drain, after the
    // end of the input, cuts that wait short.
    m_callback(bytes[0]);
}

} // namespace PhosphorMcp
