// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include "phosphorworkspaces_export.h"

#include <QObject>
#include <QString>
#include <QStringList>

namespace PhosphorWorkspaces {

/**
 * @brief Where an ActivityManager's activities come from.
 *
 * The manager keeps the rules (when it reports, what it keeps through an
 * outage of the activity service); a backend only reads and reports. Plasma's
 * backend wraps PlasmaActivities (PhosphorWorkspaces::Plasma).
 */
class PHOSPHORWORKSPACES_EXPORT IActivityBackend : public QObject
{
    Q_OBJECT

public:
    explicit IActivityBackend(QObject* parent = nullptr)
        : QObject(parent)
    {
    }
    ~IActivityBackend() override = default;

    /// Reach the activity service. Called once.
    virtual void connectToSource() = 0;
    /// Whether the activity service is running now.
    virtual bool isRunning() const = 0;
    /// The current activity, empty while the service has not settled on one.
    virtual QString currentActivity() const = 0;
    virtual QStringList activities() const = 0;
    virtual QString activityName(const QString& activityId) const = 0;
    virtual QString activityIcon(const QString& activityId) const = 0;

Q_SIGNALS:
    /// The service came up or went away. May repeat a state.
    void runningChanged(bool running);
    void currentActivityChanged(const QString& activityId);
    void activityAdded(const QString& activityId);
    void activityRemoved(const QString& activityId);
};

} // namespace PhosphorWorkspaces
