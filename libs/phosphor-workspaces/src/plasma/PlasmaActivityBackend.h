// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <PhosphorWorkspaces/IActivityBackend.h>

namespace KActivities {
class Controller;
}

namespace PhosphorWorkspaces::Plasma {

/// Plasma's activities over PlasmaActivities' Controller, which talks to
/// kactivitymanagerd. Built only with PlasmaActivities (createActivityBackend).
class PlasmaActivityBackend : public IActivityBackend
{
    Q_OBJECT

public:
    explicit PlasmaActivityBackend(QObject* parent = nullptr);
    ~PlasmaActivityBackend() override;

    void connectToSource() override;
    bool isRunning() const override;
    QString currentActivity() const override;
    QStringList activities() const override;
    QString activityName(const QString& activityId) const override;
    QString activityIcon(const QString& activityId) const override;

private:
    KActivities::Controller* m_controller = nullptr;
};

} // namespace PhosphorWorkspaces::Plasma
