// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: LGPL-2.1-or-later

// The Phosphor.Ipc QML module as a program links it: the static module and
// its plugin. A document that imports the module makes an IpcTarget, and
// the router of the engine serves it. The other suites register IpcTarget
// by hand under a name of their own, so only this one loads the module.

#include <PhosphorIpc/IpcEngine.h>
#include <PhosphorIpc/IpcRouter.h>

#include <QObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlEngineExtensionPlugin>
#include <QString>
#include <QTest>
#include <QUrl>
#include <QVariant>

#include <memory>

Q_IMPORT_QML_PLUGIN(Phosphor_IpcPlugin)

using namespace PhosphorIpc;

class TestPhosphorIpcQmlModule : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void importGivesIpcTarget()
    {
        IpcRouter router;
        QQmlEngine engine;
        IpcEngine::install(&engine, &router);
        QQmlComponent component(&engine);
        component.setData(R"(import Phosphor.Ipc
IpcTarget {
    target: "theme"
    function mode() { return "dark" }
})",
                          QUrl());
        std::unique_ptr<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));
        QCOMPARE(router.target(QStringLiteral("theme")), object.get());

        IpcRouter::InvokeOutcome outcome = IpcRouter::InvokeOutcome::NoSuchFn;
        QString message;
        const QVariant mode = router.invoke(QStringLiteral("theme"), QStringLiteral("mode"), {}, &outcome, &message);
        QVERIFY2(outcome == IpcRouter::InvokeOutcome::Ok, qPrintable(message));
        QCOMPARE(mode.toString(), QStringLiteral("dark"));
    }

    void versionedImportGivesIpcTarget()
    {
        // The version that the module declares.
        IpcRouter router;
        QQmlEngine engine;
        IpcEngine::install(&engine, &router);
        QQmlComponent component(&engine);
        component.setData("import Phosphor.Ipc 1.0\nIpcTarget { target: \"versioned\" }", QUrl());
        std::unique_ptr<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));
        QCOMPARE(router.target(QStringLiteral("versioned")), object.get());
    }
};

QTEST_GUILESS_MAIN(TestPhosphorIpcQmlModule)
#include "test_phosphor_ipc_qmlmodule.moc"
