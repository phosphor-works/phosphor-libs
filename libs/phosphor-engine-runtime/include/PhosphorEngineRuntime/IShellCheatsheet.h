// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <QString>

#include <functional>

namespace PhosphorEngineRuntime {

/**
 * @brief A desktop shell that can show the shortcut cheatsheet itself, in
 *        place of the runtime's own overlay
 *
 * plasmazonesd asks the Phosphor shell over its local socket when one runs
 * (PhosphorShellCheatsheet). A host with no such shell sets none, and every
 * request shows the runtime's overlay cheatsheet.
 *
 * Each toggle() ends in exactly one handler call, possibly later: accepted
 * when the shell took the request (the runtime then hides its own cheatsheet
 * and the pickers it would cover), or fallback when the shell cannot (none is
 * running, or it refused), with the screen the runtime should toggle its own
 * cheatsheet on instead. cancel() drops a request that has not answered yet.
 */
class IShellCheatsheet
{
public:
    struct Handlers
    {
        std::function<void()> accepted;
        std::function<void(const QString& screenId)> fallback;
    };

    virtual ~IShellCheatsheet() = default;

    /// Where the answers go. Set once, by the runtime, before the first toggle.
    virtual void setHandlers(Handlers handlers) = 0;
    /// Toggle the shell's cheatsheet on @p screenId (@p screenName is the
    /// output's name, which the shell knows it by).
    virtual void toggle(const QString& screenId, const QString& screenName) = 0;
    /// Drop a request that has not answered yet.
    virtual void cancel() = 0;
};

} // namespace PhosphorEngineRuntime
