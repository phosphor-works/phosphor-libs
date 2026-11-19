// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// The runtime's pure core helpers, as a host or the runtime calls them: the
// screen-scale test a compositor's pixel-size report is accepted by, and the
// directional adjacency search keyboard navigation moves by.

#include <PhosphorEngineRuntime/ScreenScale.h>
#include <PhosphorEngineRuntime/SpatialAdjacency.h>

#include <QTest>

using namespace PhosphorEngineRuntime;

class TestCoreHelpers : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    /// The mode of a 1920x1080-logical output at a fractional scale passes,
    /// rounding either way; another monitor's mode and a bad scale do not.
    void pixelSizeMatchesLogicalSizeAtFractionalScale()
    {
        const QSize logical(1920, 1080);
        QVERIFY(pixelSizeMatchesLogicalSize(QSize(2400, 1350), logical, 1.25));
        QVERIFY(pixelSizeMatchesLogicalSize(QSize(2401, 1349), logical, 1.25));
        QVERIFY(!pixelSizeMatchesLogicalSize(QSize(2560, 1440), logical, 1.25));
        QVERIFY(!pixelSizeMatchesLogicalSize(QSize(2400, 1350), logical, 0.0));
        QVERIFY(!pixelSizeMatchesLogicalSize(QSize(), logical, 1.25));
    }

    /// Of three side-by-side rects, right of the first is the second, left of
    /// the first is nothing.
    void findAdjacentRectFollowsTheDirection()
    {
        const QList<QRectF> candidates{QRectF(0, 0, 100, 100), QRectF(100, 0, 100, 100), QRectF(200, 0, 100, 100)};
        QCOMPARE(SpatialAdjacency::findAdjacentRect(candidates.at(0), candidates, QStringLiteral("right")), 1);
        QCOMPARE(SpatialAdjacency::findAdjacentRect(candidates.at(0), candidates, QStringLiteral("left")), -1);
        QCOMPARE(SpatialAdjacency::findAdjacentRect(candidates.at(2), candidates, QStringLiteral("left")), 1);
    }
};

QTEST_MAIN(TestCoreHelpers)
#include "test_core_helpers.moc"
