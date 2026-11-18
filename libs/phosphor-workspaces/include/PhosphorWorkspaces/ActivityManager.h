// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <PhosphorWorkspaces/IActivityBackend.h>
#include "phosphorworkspaces_export.h"
#include <QObject>
#include <QString>
#include <QStringList>

#include <memory>

namespace PhosphorWorkspaces {

/**
 * @brief The desktop's activities, from an IActivityBackend (Plasma's is
 *        PhosphorWorkspaces::Plasma's). Without one the manager is
 *        unavailable and reports no activity.
 */
class PHOSPHORWORKSPACES_EXPORT ActivityManager : public QObject
{
    Q_OBJECT

public:
    /// No backend: unavailable.
    explicit ActivityManager(QObject* parent = nullptr);
    /// Activities from @p backend, owned from here on. Null is the same as
    /// the constructor above.
    explicit ActivityManager(std::unique_ptr<IActivityBackend> backend, QObject* parent = nullptr);
    ~ActivityManager() override;

    bool init();
    void start();
    void stop();

    /// The last activity the service reported. Kept through a service outage
    /// rather than cleared, so a restart of the service is not a switch.
    /// Empty only before the service has named one.
    QString currentActivity() const;
    /// Whether this manager has activities to report at all: it has a
    /// backend. Activities are a property of the desktop, not of the moment,
    /// so this stays true through an outage of the activity service.
    bool isAvailable() const;

    /// Returns @p manager's current activity, or an empty string when @p
    /// manager is null OR has no activities (isAvailable). Centralises the
    /// `(mgr && isAvailable()) ? mgr->currentActivity() : QString()` guard
    /// shared by the daemon's context-resolution sites.
    static QString currentActivityOrEmpty(const ActivityManager* manager);
    QStringList activities() const;
    QString activityName(const QString& activityId) const;
    QString activityIcon(const QString& activityId) const;

Q_SIGNALS:
    void currentActivityChanged(const QString& activityId);
    void activitiesChanged();

private Q_SLOTS:
    void onCurrentActivityChanged(const QString& activityId);
    void onActivityAdded(const QString& activityId);
    void onActivityRemoved(const QString& activityId);

private:
    void connectSignals();
    void disconnectSignals();
    /// The backend reported the service coming up or going away.
    void onSourceRunningChanged(bool running);
    /// Take the backend's current activity when it names one, notifying
    /// only on a real change (onCurrentActivityChanged).
    void adoptSourceActivity();

    std::unique_ptr<IActivityBackend> m_backend;
    /// init() connected the backend; guards a second init().
    bool m_initialized = false;
    bool m_running = false;
    QString m_currentActivity;
    bool m_activitiesAvailable = false;
    /// start() was called and stop() has not undone it. Lets the service
    /// status handler connect the signals when the service comes up late.
    bool m_startRequested = false;
};

} // namespace PhosphorWorkspaces
