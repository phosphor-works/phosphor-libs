// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "PlasmaActivityBackend.h"

#if __has_include(<PlasmaActivities/plasmaactivities/controller.h>)
#include <PlasmaActivities/plasmaactivities/controller.h>
#include <PlasmaActivities/plasmaactivities/info.h>
#elif __has_include(<plasmaactivities/controller.h>)
#include <plasmaactivities/controller.h>
#include <plasmaactivities/info.h>
#else
#include <KActivities/Controller>
#include <KActivities/Info>
#endif

namespace PhosphorWorkspaces::Plasma {

PlasmaActivityBackend::PlasmaActivityBackend(QObject* parent)
    : IActivityBackend(parent)
{
}

PlasmaActivityBackend::~PlasmaActivityBackend() = default;

void PlasmaActivityBackend::connectToSource()
{
    if (m_controller) {
        return;
    }
    m_controller = new KActivities::Controller(this);
    connect(m_controller, &KActivities::Controller::serviceStatusChanged, this,
            [this](KActivities::Controller::ServiceStatus status) {
                Q_EMIT runningChanged(status == KActivities::Controller::Running);
            });
    connect(m_controller, &KActivities::Controller::currentActivityChanged, this,
            &PlasmaActivityBackend::currentActivityChanged);
    connect(m_controller, &KActivities::Controller::activityAdded, this, &PlasmaActivityBackend::activityAdded);
    connect(m_controller, &KActivities::Controller::activityRemoved, this, &PlasmaActivityBackend::activityRemoved);
}

bool PlasmaActivityBackend::isRunning() const
{
    return m_controller && m_controller->serviceStatus() == KActivities::Controller::Running;
}

QString PlasmaActivityBackend::currentActivity() const
{
    return m_controller ? m_controller->currentActivity() : QString();
}

QStringList PlasmaActivityBackend::activities() const
{
    return m_controller ? m_controller->activities() : QStringList();
}

QString PlasmaActivityBackend::activityName(const QString& activityId) const
{
    return KActivities::Info(activityId).name();
}

QString PlasmaActivityBackend::activityIcon(const QString& activityId) const
{
    return KActivities::Info(activityId).icon();
}

} // namespace PhosphorWorkspaces::Plasma
