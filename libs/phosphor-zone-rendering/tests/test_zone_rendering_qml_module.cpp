// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// The org.phosphor.zonerendering QML module: ZoneShaderItem resolves from the
// import as the shared library's own class (a foreign registration, not a
// second copy), and ZoneShaderRenderer.qml loads and wraps one.

#include <PhosphorZoneRendering/ZoneShaderItem.h>

#include <PhosphorEngineRuntime/AppDirectoryName.h>

#include <QQmlComponent>
#include <QQmlEngine>
#include <QTest>
#include <QtQml/qqmlextensionplugin.h>

#include <memory>

Q_IMPORT_QML_PLUGIN(org_phosphor_zonerenderingPlugin)

class TestZoneRenderingQmlModule : public QObject
{
    Q_OBJECT

private:
    static std::unique_ptr<QObject> create(QQmlEngine& engine, const QByteArray& qml, QString* errors)
    {
        QQmlComponent component(&engine);
        component.setData(qml, QUrl(QStringLiteral("inline.qml")));
        std::unique_ptr<QObject> object(component.create());
        *errors = component.errorString();
        return object;
    }

private Q_SLOTS:
    void initTestCase()
    {
        // A host names its app directory at the top of main(); the item's
        // trusted shader roots live under it.
        PhosphorEngineRuntime::setAppDirectoryName(QStringLiteral("zonerenderingtest"));
    }

    void zoneShaderItemIsTheLibraryClass()
    {
        QQmlEngine engine;
        QString errors;
        const auto object =
            create(engine, QByteArrayLiteral("import org.phosphor.zonerendering\nZoneShaderItem {}\n"), &errors);
        QVERIFY2(object, qPrintable(errors));
        QVERIFY(qobject_cast<PhosphorZoneRendering::ZoneShaderItem*>(object.get()));
    }

    void zoneShaderRendererWrapsAnItem()
    {
        QQmlEngine engine;
        QString errors;
        const auto object = create(
            engine, QByteArrayLiteral("import org.phosphor.zonerendering\nZoneShaderRenderer { config: null }\n"),
            &errors);
        QVERIFY2(object, qPrintable(errors));
        QVERIFY(object->findChild<PhosphorZoneRendering::ZoneShaderItem*>());
    }
};

QTEST_MAIN(TestZoneRenderingQmlModule)
#include "test_zone_rendering_qml_module.moc"
