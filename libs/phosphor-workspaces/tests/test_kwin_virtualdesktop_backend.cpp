// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// KWinVirtualDesktopBackend's side of KWin's signals: the D-Bus payloads it is
// handed off the bus, malformed ones included, and the wire types it narrows.
// No KWin: the slots are invoked directly with built messages, which is also
// why a well-formed s(iss) payload is not here (a QDBusArgument only
// demarshals once it has crossed a bus).

#include <PhosphorWorkspaces/Plasma/KWinVirtualDesktopBackend.h>

#include <QDBusMessage>
#include <QSignalSpy>
#include <QTest>

#include <limits>

using PhosphorWorkspaces::KWinVirtualDesktopBackend;

namespace {

QDBusMessage kwinSignal(const QString& name)
{
    return QDBusMessage::createSignal(QStringLiteral("/VirtualDesktopManager"),
                                      QStringLiteral("org.kde.KWin.VirtualDesktopManager"), name);
}

} // namespace

class TestKWinVirtualDesktopBackend : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void unreadableCreate_reportsTheIdWhenReadable()
    {
        KWinVirtualDesktopBackend backend;
        QSignalSpy created(&backend, &KWinVirtualDesktopBackend::desktopCreated);
        QSignalSpy unreadable(&backend, &KWinVirtualDesktopBackend::desktopCreatedUnreadable);
        const auto deliver = [&backend](const QDBusMessage& message) {
            QVERIFY(QMetaObject::invokeMethod(&backend, "onDesktopCreated", Q_ARG(QDBusMessage, message)));
        };

        // The id is there but the desktop data is not.
        QDBusMessage idOnly = kwinSignal(QStringLiteral("desktopCreated"));
        idOnly << QStringLiteral("x");
        deliver(idOnly);
        // Nothing readable, an id that is not a string, an empty id.
        deliver(kwinSignal(QStringLiteral("desktopCreated")));
        QDBusMessage notAnId = kwinSignal(QStringLiteral("desktopCreated"));
        notAnId << 7;
        deliver(notAnId);
        QDBusMessage emptyId = kwinSignal(QStringLiteral("desktopCreated"));
        emptyId << QString();
        deliver(emptyId);

        QCOMPARE(created.count(), 0);
        QCOMPARE(unreadable.count(), 4);
        QCOMPARE(unreadable.at(0).at(0).toString(), QStringLiteral("x"));
        for (int i = 1; i < unreadable.count(); ++i) {
            QVERIFY(unreadable.at(i).at(0).toString().isEmpty());
        }
    }

    void unreadableDataChange_isReported()
    {
        KWinVirtualDesktopBackend backend;
        QSignalSpy changed(&backend, &KWinVirtualDesktopBackend::desktopDataChanged);
        QSignalSpy unreadable(&backend, &KWinVirtualDesktopBackend::desktopDataChangedUnreadable);
        QDBusMessage idOnly = kwinSignal(QStringLiteral("desktopDataChanged"));
        idOnly << QStringLiteral("x");
        QVERIFY(QMetaObject::invokeMethod(&backend, "onDesktopDataChanged", Q_ARG(QDBusMessage, idOnly)));
        QCOMPARE(changed.count(), 0);
        QCOMPARE(unreadable.count(), 1);
    }

    void wireCounts_areNarrowedAtTheBoundary()
    {
        KWinVirtualDesktopBackend backend;
        QSignalSpy counts(&backend, &KWinVirtualDesktopBackend::countChanged);
        QSignalSpy rows(&backend, &KWinVirtualDesktopBackend::rowsChanged);
        const uint huge = std::numeric_limits<uint>::max();

        QVERIFY(QMetaObject::invokeMethod(&backend, "onCountChanged", Q_ARG(uint, 3u)));
        // No count KWin sends: dropped, not wrapped to a negative.
        QVERIFY(QMetaObject::invokeMethod(&backend, "onCountChanged", Q_ARG(uint, huge)));
        QCOMPARE(counts.count(), 1);
        QCOMPARE(counts.first().at(0).toInt(), 3);

        // Rows saturate instead.
        QVERIFY(QMetaObject::invokeMethod(&backend, "onRowsChanged", Q_ARG(uint, huge)));
        QCOMPARE(rows.count(), 1);
        QCOMPARE(rows.first().at(0).toInt(), std::numeric_limits<int>::max());
    }

    void unconnected_readsNothing()
    {
        KWinVirtualDesktopBackend backend;
        QVERIFY(!backend.isReachable());
        QVERIFY(!backend.readSnapshot().has_value());
        bool answered = false;
        bool hadSnapshot = true;
        backend.readSnapshotAsync([&](std::optional<KWinVirtualDesktopBackend::Snapshot> snapshot) {
            answered = true;
            hadSnapshot = snapshot.has_value();
        });
        QVERIFY(answered);
        QVERIFY(!hadSnapshot);
    }
};

QTEST_GUILESS_MAIN(TestKWinVirtualDesktopBackend)
#include "test_kwin_virtualdesktop_backend.moc"
