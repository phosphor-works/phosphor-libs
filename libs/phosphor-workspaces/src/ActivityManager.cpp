// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorWorkspaces/ActivityManager.h>

#include <utility>

namespace PhosphorWorkspaces {

ActivityManager::ActivityManager(QObject* parent)
    : ActivityManager(nullptr, parent)
{
}

ActivityManager::ActivityManager(std::unique_ptr<IActivityBackend> backend, QObject* parent)
    : QObject(parent)
    , m_backend(std::move(backend))
{
    if (m_backend) {
        // Owned through m_backend, so no Qt parent to delete it a second time.
        m_backend->setParent(nullptr);
    }
}

ActivityManager::~ActivityManager()
{
    stop();
}

bool ActivityManager::isAvailable() const
{
    return m_backend != nullptr;
}

bool ActivityManager::init()
{
    // A second init would connect the backend again and stack a second
    // status handler on it, each announcing every service bounce.
    if (!m_backend || m_initialized) {
        return true;
    }
    m_initialized = true;
    m_backend->connectToSource();
    connect(m_backend.get(), &IActivityBackend::runningChanged, this, &ActivityManager::onSourceRunningChanged);
    m_activitiesAvailable = m_backend->isRunning();
    if (m_activitiesAvailable) {
        m_currentActivity = m_backend->currentActivity();
    }
    return true;
}

void ActivityManager::onSourceRunningChanged(bool running)
{
    const bool wasAvailable = m_activitiesAvailable;
    m_activitiesAvailable = running;
    // A manager that was never started, or was stopped, tracks availability
    // only. It reports nothing, and start() picks up whatever changed in the
    // meantime.
    if (!m_startRequested) {
        return;
    }
    if (m_activitiesAvailable && !wasAvailable) {
        // A start() that ran while the service was still down returned without
        // connecting the activity signals. Connect them now, or no later
        // activity switch is ever seen.
        if (!m_running) {
            m_running = true;
            connectSignals();
        }
        Q_EMIT activitiesChanged();
        // Notifies only on a real change. A service that merely restarted
        // reports the activity kept through its outage, so the return is not
        // announced as a switch. The first activity known after a late start
        // is a change, and is.
        adoptSourceActivity();
    } else if (!m_activitiesAvailable && wasAvailable) {
        // The last known activity is kept through the outage. Clearing it
        // would resolve every placement made meanwhile under "every activity"
        // while the layout registry and the engines still hold the last one,
        // and the service's return would then announce a switch to an activity
        // that never changed.
        Q_EMIT activitiesChanged();
    }
}

void ActivityManager::start()
{
    // Remembered so a service that comes up after this call still gets its
    // signals connected (onSourceRunningChanged).
    m_startRequested = true;
    if (m_running || !m_activitiesAvailable) {
        return;
    }
    m_running = true;
    connectSignals();
    // The status handler reports nothing while the manager is stopped, and
    // the activity signals were disconnected, so catch up on a switch made in
    // the meantime (or on a service that came up between init() and here).
    adoptSourceActivity();
}

void ActivityManager::adoptSourceActivity()
{
    if (!m_backend) {
        return;
    }
    // An empty answer is the service not having settled on one yet. The
    // backend's own currentActivityChanged reports it once it has, so the
    // kept activity is not cleared for it.
    const QString activityId = m_backend->currentActivity();
    if (!activityId.isEmpty()) {
        onCurrentActivityChanged(activityId);
    }
}

void ActivityManager::stop()
{
    m_startRequested = false;
    if (!m_running) {
        return;
    }
    m_running = false;
    disconnectSignals();
}

QString ActivityManager::currentActivity() const
{
    return m_currentActivity;
}

QString ActivityManager::currentActivityOrEmpty(const ActivityManager* manager)
{
    return (manager && manager->isAvailable()) ? manager->currentActivity() : QString();
}

QStringList ActivityManager::activities() const
{
    if (!m_activitiesAvailable || !m_backend) {
        return {};
    }
    return m_backend->activities();
}

QString ActivityManager::activityName(const QString& activityId) const
{
    if (!m_activitiesAvailable || !m_backend || activityId.isEmpty()) {
        return {};
    }
    return m_backend->activityName(activityId);
}

QString ActivityManager::activityIcon(const QString& activityId) const
{
    if (!m_activitiesAvailable || !m_backend || activityId.isEmpty()) {
        return {};
    }
    return m_backend->activityIcon(activityId);
}

void ActivityManager::onCurrentActivityChanged(const QString& activityId)
{
    if (m_currentActivity == activityId) {
        return;
    }
    m_currentActivity = activityId;
    Q_EMIT currentActivityChanged(activityId);
}

void ActivityManager::onActivityAdded(const QString& activityId)
{
    Q_UNUSED(activityId)
    Q_EMIT activitiesChanged();
}

void ActivityManager::onActivityRemoved(const QString& activityId)
{
    Q_UNUSED(activityId)
    Q_EMIT activitiesChanged();
}

void ActivityManager::connectSignals()
{
    if (!m_backend) {
        return;
    }
    connect(m_backend.get(), &IActivityBackend::currentActivityChanged, this,
            &ActivityManager::onCurrentActivityChanged);
    connect(m_backend.get(), &IActivityBackend::activityAdded, this, &ActivityManager::onActivityAdded);
    connect(m_backend.get(), &IActivityBackend::activityRemoved, this, &ActivityManager::onActivityRemoved);
}

void ActivityManager::disconnectSignals()
{
    // Only the signals connectSignals() made. A blanket disconnect would also
    // drop init()'s runningChanged handler, and a service restart after
    // stop() and start() would then go unnoticed.
    if (!m_backend) {
        return;
    }
    disconnect(m_backend.get(), &IActivityBackend::currentActivityChanged, this,
               &ActivityManager::onCurrentActivityChanged);
    disconnect(m_backend.get(), &IActivityBackend::activityAdded, this, &ActivityManager::onActivityAdded);
    disconnect(m_backend.get(), &IActivityBackend::activityRemoved, this, &ActivityManager::onActivityRemoved);
}

} // namespace PhosphorWorkspaces
