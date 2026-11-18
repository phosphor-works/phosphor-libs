// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// ActivityManager against a scripted IActivityBackend: availability, what it
// reports while started and stopped, and what it keeps through an outage of
// the activity service. test_activitymanager drives the same rules through
// Plasma's backend and a fake kactivitymanagerd.

#include <PhosphorWorkspaces/ActivityManager.h>
#include <PhosphorWorkspaces/IActivityBackend.h>

#include <QSignalSpy>
#include <QTest>

#include <memory>

using PhosphorWorkspaces::ActivityManager;
using PhosphorWorkspaces::IActivityBackend;

namespace {

class FakeBackend : public IActivityBackend
{
public:
    bool running = false;
    QString current;
    QStringList list;
    int connects = 0;

    void connectToSource() override
    {
        ++connects;
    }
    bool isRunning() const override
    {
        return running;
    }
    QString currentActivity() const override
    {
        return current;
    }
    QStringList activities() const override
    {
        return list;
    }
    QString activityName(const QString& activityId) const override
    {
        return QStringLiteral("name of ") + activityId;
    }
    QString activityIcon(const QString& activityId) const override
    {
        return QStringLiteral("icon of ") + activityId;
    }

    void setRunning(bool nowRunning)
    {
        running = nowRunning;
        Q_EMIT runningChanged(nowRunning);
    }
    void switchTo(const QString& activityId)
    {
        current = activityId;
        Q_EMIT currentActivityChanged(activityId);
    }
};

struct Rig
{
    FakeBackend* backend = nullptr;
    std::unique_ptr<ActivityManager> manager;

    explicit Rig(bool running, const QString& current = QString())
    {
        auto fake = std::make_unique<FakeBackend>();
        fake->running = running;
        fake->current = current;
        fake->list = {QStringLiteral("A"), QStringLiteral("B")};
        backend = fake.get();
        manager = std::make_unique<ActivityManager>(std::move(fake));
    }
};

} // namespace

class TestActivityManagerBackend : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void noBackend_isUnavailableAndReportsNothing()
    {
        ActivityManager manager;
        QVERIFY(!manager.isAvailable());
        QVERIFY(manager.init());
        manager.start();
        QVERIFY(manager.activities().isEmpty());
        QVERIFY(manager.activityName(QStringLiteral("A")).isEmpty());
        QVERIFY(ActivityManager::currentActivityOrEmpty(&manager).isEmpty());
        QVERIFY(ActivityManager::currentActivityOrEmpty(nullptr).isEmpty());
    }

    void runningService_isReadThroughTheBackend()
    {
        Rig rig(true, QStringLiteral("A"));
        QVERIFY(rig.manager->isAvailable());
        QVERIFY(rig.manager->init());
        rig.manager->init();
        QCOMPARE(rig.backend->connects, 1);
        QCOMPARE(rig.manager->currentActivity(), QStringLiteral("A"));
        QCOMPARE(ActivityManager::currentActivityOrEmpty(rig.manager.get()), QStringLiteral("A"));
        QCOMPARE(rig.manager->activities(), QStringList({QStringLiteral("A"), QStringLiteral("B")}));
        QCOMPARE(rig.manager->activityName(QStringLiteral("B")), QStringLiteral("name of B"));
        QCOMPARE(rig.manager->activityIcon(QStringLiteral("B")), QStringLiteral("icon of B"));
    }

    void lateService_isConnectedWhenItComesUp()
    {
        Rig rig(false);
        QVERIFY(rig.manager->init());
        QSignalSpy changed(rig.manager.get(), &ActivityManager::currentActivityChanged);
        QSignalSpy listChanged(rig.manager.get(), &ActivityManager::activitiesChanged);
        rig.manager->start();
        QVERIFY(rig.manager->activities().isEmpty());

        rig.backend->current = QStringLiteral("A");
        rig.backend->setRunning(true);
        QCOMPARE(listChanged.count(), 1);
        QCOMPARE(changed.count(), 1);
        QCOMPARE(changed.first().at(0).toString(), QStringLiteral("A"));

        // Connected: a later switch is seen.
        rig.backend->switchTo(QStringLiteral("B"));
        QCOMPARE(rig.manager->currentActivity(), QStringLiteral("B"));
    }

    void outage_keepsTheActivityAndTheReturnIsNoSwitch()
    {
        Rig rig(true, QStringLiteral("A"));
        QVERIFY(rig.manager->init());
        rig.manager->start();
        QSignalSpy changed(rig.manager.get(), &ActivityManager::currentActivityChanged);
        QSignalSpy listChanged(rig.manager.get(), &ActivityManager::activitiesChanged);

        rig.backend->setRunning(false);
        QCOMPARE(listChanged.count(), 1);
        QCOMPARE(rig.manager->currentActivity(), QStringLiteral("A"));
        QVERIFY(rig.manager->activities().isEmpty());

        rig.backend->setRunning(true);
        QCOMPARE(listChanged.count(), 2);
        QCOMPARE(changed.count(), 0);
    }

    void stoppedManager_isSilent()
    {
        Rig rig(true, QStringLiteral("A"));
        QVERIFY(rig.manager->init());
        rig.manager->start();
        rig.manager->stop();
        QSignalSpy changed(rig.manager.get(), &ActivityManager::currentActivityChanged);
        QSignalSpy listChanged(rig.manager.get(), &ActivityManager::activitiesChanged);

        rig.backend->switchTo(QStringLiteral("B"));
        rig.backend->setRunning(false);
        rig.backend->setRunning(true);
        QCOMPARE(changed.count(), 0);
        QCOMPARE(listChanged.count(), 0);

        // start() catches up on the switch made meanwhile.
        rig.manager->start();
        QCOMPARE(changed.count(), 1);
        QCOMPARE(rig.manager->currentActivity(), QStringLiteral("B"));
    }
};

QTEST_GUILESS_MAIN(TestActivityManagerBackend)
#include "test_activitymanager_backend.moc"
