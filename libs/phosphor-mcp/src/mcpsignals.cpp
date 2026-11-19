// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "mcpsignals.h"

#include "mcplogging.h"

#include <QSocketNotifier>

#include <atomic>
#include <cerrno>
#include <csignal>

#include <fcntl.h>
#include <unistd.h>

namespace PhosphorMcp {

namespace {

// Read by the handler, which may run on any thread, so atomic. -1 while no
// instance is installed.
std::atomic<int> s_writeFd{-1};
int s_readFd = -1;
McpTerminationSignals* s_owner = nullptr;

struct sigaction s_oldTerm;
struct sigaction s_oldHup;

extern "C" void onTerminationSignal(int signal)
{
    const int savedErrno = errno;
    const int fd = s_writeFd.load(std::memory_order_relaxed);
    if (fd >= 0) {
        const unsigned char byte = static_cast<unsigned char>(signal);
        // A full pipe already holds a byte that ends the server.
        [[maybe_unused]] const ssize_t written = ::write(fd, &byte, 1);
    }
    errno = savedErrno;
}

} // namespace

std::unique_ptr<McpTerminationSignals> McpTerminationSignals::install(std::function<void(int)> callback)
{
    if (s_owner) {
        qCWarning(lcMcp, "McpServer::handleTerminationSignals: another server handles the signals");
        return nullptr;
    }
    int fds[2];
    if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0) {
        qCWarning(lcMcp, "McpServer::handleTerminationSignals: cannot make the self-pipe");
        return nullptr;
    }
    std::unique_ptr<McpTerminationSignals> self(new McpTerminationSignals);
    self->m_callback = std::move(callback);
    s_readFd = fds[0];
    s_writeFd.store(fds[1]);
    s_owner = self.get();

    struct sigaction action = {};
    action.sa_handler = onTerminationSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    sigaction(SIGTERM, &action, &s_oldTerm);
    sigaction(SIGHUP, &action, &s_oldHup);

    self->m_notifier = new QSocketNotifier(s_readFd, QSocketNotifier::Read);
    McpTerminationSignals* raw = self.get();
    QObject::connect(self->m_notifier, &QSocketNotifier::activated, self->m_notifier, [raw] {
        raw->onReadable();
    });
    return self;
}

McpTerminationSignals::~McpTerminationSignals()
{
    // The old dispositions first, so that a signal from here on no longer
    // reaches a pipe that is about to close.
    sigaction(SIGTERM, &s_oldTerm, nullptr);
    sigaction(SIGHUP, &s_oldHup, nullptr);
    const int writeFd = s_writeFd.exchange(-1);
    delete m_notifier;
    ::close(s_readFd);
    ::close(writeFd);
    s_readFd = -1;
    s_owner = nullptr;
}

void McpTerminationSignals::onReadable()
{
    unsigned char bytes[16];
    const ssize_t got = ::read(s_readFd, bytes, sizeof bytes);
    if (got <= 0) {
        return;
    }
    // The first signal ends the server; any after it change nothing.
    m_callback(bytes[0]);
}

} // namespace PhosphorMcp
