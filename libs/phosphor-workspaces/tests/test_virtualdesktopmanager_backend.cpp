// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// VirtualDesktopManager against a scripted IVirtualDesktopBackend: what the
// manager asks of its backend, when, and what it does with the answers. The
// rules about the desktops themselves are test_virtualdesktopmanager's.

#include <PhosphorWorkspaces/IVirtualDesktopBackend.h>
#include <PhosphorWorkspaces/VirtualDesktopManager.h>

#include <QSignalSpy>
#include <QTest>

#include <memory>

using PhosphorWorkspaces::DesktopEntry;
using PhosphorWorkspaces::IVirtualDesktopBackend;
using PhosphorWorkspaces::VirtualDesktopManager;

namespace {

class FakeBackend : public IVirtualDesktopBackend
{
public:
    bool connectResult = true;
    bool reachable = true;
    int connects = 0;
    int subscribes = 0;
    int unsubscribes = 0;
    int blockingReads = 0;
    std::optional<Snapshot> blockingAnswer;
    QList<SnapshotCallback> pendingReads;
    QStringList switches;
    QList<std::pair<uint, QString>> creates;
    QList<AnswerCallback> createAnswers;
    QStringList removes;
    QList<AnswerCallback> removeAnswers;
    QList<std::pair<QString, QString>> renames;

    bool connectToSource() override
    {
        ++connects;
        return connectResult;
    }
    bool isReachable() const override
    {
        return reachable;
    }
    void subscribe() override
    {
        ++subscribes;
    }
    void unsubscribe() override
    {
        ++unsubscribes;
    }
    std::optional<Snapshot> readSnapshot() override
    {
        ++blockingReads;
        return blockingAnswer;
    }
    void readSnapshotAsync(SnapshotCallback done) override
    {
        pendingReads.append(std::move(done));
    }
    void setCurrentDesktop(const QString& desktopId) override
    {
        switches.append(desktopId);
    }
    void createDesktop(uint position, const QString& name, AnswerCallback done) override
    {
        creates.append({position, name});
        createAnswers.append(std::move(done));
    }
    void removeDesktop(const QString& desktopId, AnswerCallback done) override
    {
        removes.append(desktopId);
        removeAnswers.append(std::move(done));
    }
    void setDesktopName(const QString& desktopId, const QString& name) override
    {
        renames.append({desktopId, name});
    }

    /// Answer the oldest outstanding asynchronous read.
    void answerRead(std::optional<Snapshot> snapshot)
    {
        QVERIFY(!pendingReads.isEmpty());
        pendingReads.takeFirst()(std::move(snapshot));
    }
};

IVirtualDesktopBackend::Snapshot snapshot(const char* letters, const QString& currentId = QString(),
                                          std::optional<int> rows = 1, const QStringList& names = {})
{
    IVirtualDesktopBackend::Snapshot out;
    int i = 0;
    for (const char* c = letters; *c; ++c, ++i) {
        out.desktops.append(DesktopEntry{i, QString(QLatin1Char(*c)), names.value(i)});
    }
    out.currentId = currentId;
    out.rows = rows;
    return out;
}

/// A manager over a fake backend, and the backend it owns.
struct Rig
{
    FakeBackend* backend = nullptr;
    std::unique_ptr<VirtualDesktopManager> manager;

    Rig()
    {
        auto fake = std::make_unique<FakeBackend>();
        backend = fake.get();
        manager = std::make_unique<VirtualDesktopManager>(std::move(fake));
    }
};

} // namespace

class TestVirtualDesktopManagerBackend : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init_commitsTheBlockingReadAndSubscribesOnce()
    {
        Rig rig;
        rig.backend->blockingAnswer = snapshot("abc", QStringLiteral("b"), 2);
        QVERIFY(rig.manager->init());
        QVERIFY(rig.manager->isAvailable());
        QCOMPARE(rig.backend->blockingReads, 1);
        QCOMPARE(rig.backend->subscribes, 1);
        QCOMPARE(rig.manager->desktopIds(),
                 QStringList({QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")}));
        QCOMPARE(rig.manager->desktopCount(), 3);
        QCOMPARE(rig.manager->currentDesktop(), 2);
        QCOMPARE(rig.manager->desktopRows(), 2);

        // A second init() connects and subscribes nothing again.
        QVERIFY(rig.manager->init());
        QCOMPARE(rig.backend->connects, 1);
        QCOMPARE(rig.backend->subscribes, 1);
        // Neither does the first start(), which only reads.
        rig.manager->start();
        QCOMPARE(rig.backend->subscribes, 1);
        QCOMPARE(rig.backend->pendingReads.size(), 1);
    }

    void init_withNoDesktopsToReachStaysUnavailable()
    {
        Rig rig;
        rig.backend->connectResult = false;
        QVERIFY(rig.manager->init());
        QVERIFY(!rig.manager->isAvailable());
        QCOMPARE(rig.backend->subscribes, 0);
        QCOMPARE(rig.backend->blockingReads, 0);
        QCOMPARE(rig.manager->desktopCount(), 1);
        rig.manager->start();
        QCOMPARE(rig.backend->pendingReads.size(), 0);
    }

    void snapshotWithoutRows_keepsTheRowsTheManagerHas()
    {
        Rig rig;
        rig.backend->blockingAnswer = snapshot("ab", QStringLiteral("a"), 2);
        QVERIFY(rig.manager->init());
        rig.manager->start();
        rig.backend->answerRead(snapshot("abc", QStringLiteral("a"), std::nullopt));
        QCOMPARE(rig.manager->desktopCount(), 3);
        QCOMPARE(rig.manager->desktopRows(), 2);
    }

    void runningManager_readsOneAtATime()
    {
        Rig rig;
        rig.backend->blockingAnswer = snapshot("ab", QStringLiteral("a"));
        QVERIFY(rig.manager->init());
        rig.manager->start();
        QCOMPARE(rig.backend->pendingReads.size(), 1);

        // A rename while the read is in flight is covered by it: no second read.
        Q_EMIT rig.backend->desktopDataChanged(QStringLiteral("b"), 1, QStringLiteral("Web"));
        QCOMPARE(rig.backend->pendingReads.size(), 1);

        QSignalSpy renamed(rig.manager.get(), &VirtualDesktopManager::desktopRenamed);
        rig.backend->answerRead(snapshot("ab", QStringLiteral("a"), 1, {QString(), QStringLiteral("Web")}));
        QCOMPARE(renamed.count(), 1);
        QCOMPARE(renamed.first().at(1).toString(), QStringLiteral("Web"));

        // With nothing in flight, the next change reads again, and so does an
        // asked-for refresh() while that read is out, not at all.
        Q_EMIT rig.backend->desktopDataChangedUnreadable();
        QCOMPARE(rig.backend->pendingReads.size(), 1);
        rig.manager->refresh();
        QCOMPARE(rig.backend->pendingReads.size(), 1);
        rig.backend->answerRead(snapshot("ab", QStringLiteral("a"), 1, {QString(), QStringLiteral("Web")}));
        rig.manager->refresh();
        QCOMPARE(rig.backend->pendingReads.size(), 1);
    }

    void failedReads_retryThenGiveTheChangeUp()
    {
        Rig rig;
        rig.backend->blockingAnswer = snapshot("abc", QStringLiteral("a"));
        QVERIFY(rig.manager->init());
        rig.manager->start();
        rig.backend->answerRead(snapshot("abc", QStringLiteral("a")));

        QSignalSpy changing(rig.manager.get(), &VirtualDesktopManager::desktopListChanging);
        QSignalSpy settled(rig.manager.get(), &VirtualDesktopManager::desktopListChangeSettled);
        Q_EMIT rig.backend->desktopRemoved(QStringLiteral("c"));
        QCOMPARE(changing.count(), 1);

        // The read and its three retries all fail, and the change is given up.
        for (int attempt = 0; attempt < 4; ++attempt) {
            QTRY_COMPARE(rig.backend->pendingReads.size(), 1);
            QCOMPARE(settled.count(), 0);
            rig.backend->answerRead(std::nullopt);
        }
        QCOMPARE(settled.count(), 1);
        QCOMPARE(rig.manager->desktopIds().size(), 3);
    }

    void unreachableSource_commitsTheCountAsReported()
    {
        Rig rig;
        rig.backend->blockingAnswer = snapshot("abc", QStringLiteral("a"));
        QVERIFY(rig.manager->init());
        rig.manager->start();
        rig.backend->answerRead(snapshot("abc", QStringLiteral("a")));

        // KWin off the bus: nothing to read, so a count goes out as reported,
        // a shrinking one too (which a reachable source leaves to the list
        // read), and no change is opened that no read could settle.
        rig.backend->reachable = false;
        QSignalSpy counts(rig.manager.get(), &VirtualDesktopManager::desktopCountChanged);
        QSignalSpy changing(rig.manager.get(), &VirtualDesktopManager::desktopListChanging);
        Q_EMIT rig.backend->countChanged(2);
        QCOMPARE(counts.count(), 1);
        QCOMPARE(counts.first().at(0).toInt(), 2);
        QCOMPARE(changing.count(), 0);
        QCOMPARE(rig.backend->pendingReads.size(), 0);
    }

    void stoppedManager_ignoresTheSource()
    {
        Rig rig;
        rig.backend->blockingAnswer = snapshot("abc", QStringLiteral("a"));
        QVERIFY(rig.manager->init());
        rig.manager->start();
        rig.backend->answerRead(snapshot("abc", QStringLiteral("a")));
        rig.manager->stop();
        QCOMPARE(rig.backend->unsubscribes, 1);

        QSignalSpy counts(rig.manager.get(), &VirtualDesktopManager::desktopCountChanged);
        QSignalSpy currents(rig.manager.get(), &VirtualDesktopManager::currentDesktopChanged);
        Q_EMIT rig.backend->countChanged(5);
        Q_EMIT rig.backend->currentChanged(QStringLiteral("b"));
        QCOMPARE(counts.count(), 0);
        QCOMPARE(currents.count(), 0);

        // start() subscribes again, once.
        rig.manager->start();
        QCOMPARE(rig.backend->subscribes, 2);
    }

    void commands_goToTheBackendAndTheAnswersAreRelayed()
    {
        Rig rig;
        rig.backend->blockingAnswer = snapshot("ab", QStringLiteral("a"));
        QVERIFY(rig.manager->init());

        rig.manager->setCurrentDesktopById(QStringLiteral("b"));
        rig.manager->setCurrentDesktopById(QStringLiteral("unknown"));
        rig.manager->setCurrentDesktop(1);
        QCOMPARE(rig.backend->switches, QStringList({QStringLiteral("b"), QStringLiteral("a")}));

        QSignalSpy createAnswers(rig.manager.get(), &VirtualDesktopManager::createDesktopAnswered);
        QSignalSpy removeAnswers(rig.manager.get(), &VirtualDesktopManager::removeDesktopAnswered);
        rig.manager->createDesktop(1, QStringLiteral("New"));
        rig.manager->removeDesktop(QStringLiteral("a"));
        rig.manager->setDesktopName(QStringLiteral("b"), QStringLiteral("Mail"));
        QCOMPARE(rig.backend->creates.size(), 1);
        QCOMPARE(rig.backend->removes, QStringList{QStringLiteral("a")});
        QCOMPARE(rig.backend->renames.size(), 1);

        rig.backend->createAnswers.takeFirst()(false);
        QCOMPARE(createAnswers.count(), 1);
        QCOMPARE(createAnswers.first().at(2).value<VirtualDesktopManager::CallAnswer>(),
                 VirtualDesktopManager::CallAnswer::Unanswered);
        rig.backend->removeAnswers.takeFirst()(true);
        QCOMPARE(removeAnswers.count(), 1);
        QCOMPARE(removeAnswers.first().at(1).value<VirtualDesktopManager::CallAnswer>(),
                 VirtualDesktopManager::CallAnswer::Answered);
    }
};

QTEST_GUILESS_MAIN(TestVirtualDesktopManagerBackend)
#include "test_virtualdesktopmanager_backend.moc"
