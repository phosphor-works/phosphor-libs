// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <QtCore/qtclasshelpermacros.h>

#include <functional>
#include <memory>

QT_BEGIN_NAMESPACE
class QSocketNotifier;
QT_END_NAMESPACE

namespace PhosphorMcp {

// SIGTERM and SIGHUP through a self-pipe, for
// McpServer::handleTerminationSignals(). The handler only writes the
// signal's number to the pipe, which is all that is safe in a signal
// handler; the notifier on the other end calls back from the event loop.
//
// SIGPIPE is not touched: an ignored SIGPIPE survives exec into every
// program the application starts.
//
// Dispositions are global to the process, so one instance at a time holds
// them, and its destruction puts back the ones it found.
class McpTerminationSignals
{
public:
    // nullptr, with a warning, when another instance holds the signals or
    // the pipe cannot be made.
    static std::unique_ptr<McpTerminationSignals> install(std::function<void(int signal)> callback);
    ~McpTerminationSignals();
    Q_DISABLE_COPY_MOVE(McpTerminationSignals)

private:
    McpTerminationSignals() = default;
    void onReadable();

    std::function<void(int)> m_callback;
    QSocketNotifier* m_notifier = nullptr;
};

} // namespace PhosphorMcp
