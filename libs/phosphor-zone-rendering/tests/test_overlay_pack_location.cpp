// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// The trusted overlay roots follow the app's overlay pack directory
// (setOverlayPackDataSubdir): none until the app names it, then that directory
// under each data location that has it.

#include <PhosphorZoneRendering/OverlayPackLocation.h>
#include <PhosphorZoneRendering/ZoneShaderNodeRhi.h>

#include <QDir>
#include <QStandardPaths>
#include <QTest>

class TestOverlayPackLocation : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
    }

    void noTrustedRootUntilTheAppNamesOne()
    {
        PhosphorZoneRendering::setOverlayPackDataSubdir(QString());
        QVERIFY(PhosphorZoneRendering::trustedShaderRoots().isEmpty());
    }

    void trustsTheNamedDirectoryUnderTheDataLocation()
    {
        const QString dataHome = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
        const QString root = dataHome + QStringLiteral("/overlaypacktest/overlays");
        QVERIFY(QDir().mkpath(root));

        PhosphorZoneRendering::setOverlayPackDataSubdir(QStringLiteral("overlaypacktest/overlays"));
        QCOMPARE(PhosphorZoneRendering::overlayPackDataSubdir(), QStringLiteral("overlaypacktest/overlays"));
        const QStringList roots = PhosphorZoneRendering::trustedShaderRoots();
        QVERIFY2(roots.contains(QDir::cleanPath(root)) || roots.contains(QDir::cleanPath(root) + QLatin1Char('/')),
                 qPrintable(roots.join(QLatin1String(", "))));

        QDir(dataHome + QStringLiteral("/overlaypacktest")).removeRecursively();
        PhosphorZoneRendering::setOverlayPackDataSubdir(QString());
    }
};

QTEST_GUILESS_MAIN(TestOverlayPackLocation)
#include "test_overlay_pack_location.moc"
