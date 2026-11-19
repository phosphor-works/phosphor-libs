// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// The entry scaffold a zone pack without its own main() is assembled with:
// the prologue goes on top, the recognised entry's main() goes underneath,
// and a pack that writes its own main() is left as it is.

#include <PhosphorZoneRendering/ZoneEntryScaffold.h>

#include <QTest>

using namespace PhosphorZoneRendering;

class TestZoneEntryScaffold : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void prologueOpensWithTheVersion()
    {
        QVERIFY(zoneEntryPrologue().startsWith(QLatin1String("#version")));
        QVERIFY(zoneEntryPrologue().endsWith(QLatin1Char('\n')));
    }

    void entryOnlyPackGetsPrologueAndMain()
    {
        const QString body = QStringLiteral("vec4 pZone(ZoneCtx z) { return vec4(1.0); }\n");
        const QString assembled = assembleZoneEntrySource(body);
        QVERIFY(assembled.startsWith(zoneEntryPrologue()));
        QVERIFY(assembled.contains(body));
        QVERIFY(assembled.contains(QLatin1String("void main()")));
    }

    void packWithItsOwnMainIsUnchanged()
    {
        const QString own = QStringLiteral("#version 450\nvoid main() { }\n");
        QCOMPARE(assembleZoneEntrySource(own), own);
    }
};

QTEST_MAIN(TestZoneEntryScaffold)
#include "test_zone_entry_scaffold.moc"
