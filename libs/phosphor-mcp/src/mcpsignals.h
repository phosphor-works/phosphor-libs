// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <QPointer>
#include <QtCore/qtclasshelpermacros.h>

#include <functional>
#include <memory>

QT_BEGIN_NAMESPACE
class QObject;
class QSocketNotifier;
QT_END_NAMESPACE

namespace PhosphorMcp {

// SIGTERM and SIGHUP through a self-pipe, for
// McpServer::handleTerminationSignals(). The handler only writes the
// signal's number to the pipe, which is all that is safe in a signal
// handler. The notifier on the other end calls back from the event loop.
//
// SIGPIPE is not touched: an ignored SIGPIPE survives exec into every
// program the application starts. A signal that the process ignored when
// install() ran (nohup ignores SIGHUP) stays ignored.
//
// Dispositions are global to the process, so one instance at a time holds
// them. release() puts back the ones it found, unless someone installed
// another handler since, and lets another instance install.
class McpTerminationSignals
{
public:
    // The notifier is a child of `context`, so it moves with it to another
    // thread, and calls back on the thread of `context`. nullptr, with a
    // warning, when another instance holds the signals, when the thread of
    // `context` has no event dispatcher, or when the pipe cannot be made.
    static std::unique_ptr<McpTerminationSignals> install(QObject* context, std::function<void(int signal)> callback);
    // Releases the signals, and closes the pipe.
    ~McpTerminationSignals();
    Q_DISABLE_COPY_MOVE(McpTerminationSignals)

    // Puts the old dispositions back and calls back no more. The object
    // stays, so that this may run from inside the callback. Once only, and a
    // later call does nothing.
    void release();

private:
    McpTerminationSignals() = default;
    void onReadable();

    std::function<void(int)> m_callback;
    QPointer<QSocketNotifier> m_notifier;
    int m_readFd = -1;
    int m_writeFd = -1;
    bool m_released = false;
};

} // namespace PhosphorMcp
