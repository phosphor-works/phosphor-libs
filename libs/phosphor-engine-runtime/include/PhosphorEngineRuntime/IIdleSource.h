// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include "phosphorengineruntime_export.h"

#include <QObject>

namespace PhosphorEngineRuntime {

/**
 * @brief The session's idle notification, from the host
 *
 * The runtime pauses the decoration chain while the user is away. plasmazonesd
 * answers through the compositor's ext-idle-notify-v1 (PhosphorServiceIdle); a
 * host that renders in the compositor itself knows idleness first-hand. A host
 * whose compositor cannot tell supplies none, and the pause never engages.
 */
class PHOSPHORENGINERUNTIME_EXPORT IIdleSource : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

    /// Report idle once the user has been inactive for @p timeoutMs. Zero
    /// disarms. Setting the timeout it already has changes nothing, so a
    /// caller that needs a fresh arm sets zero first.
    virtual void setIdleTimeout(int timeoutMs) = 0;
    /// Whether the timeout took. A source can speak the protocol and still
    /// fail to arm (no seat with input devices yet, at login), so the runtime
    /// retries until it does.
    virtual bool isArmed() const = 0;
    /// Whether the user is idle now.
    virtual bool isIdle() const = 0;

Q_SIGNALS:
    void idled();
    void resumed();
};

} // namespace PhosphorEngineRuntime
