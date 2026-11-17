// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// Where ScrollingTemplateStore looks: only under the subdirectory its owner
// passes, and nowhere at all without one. The rest of the store's contract
// (CRUD, shadowing, the delete refusal) is tested in PlasmaZones' suite.

#include <PhosphorZones/ScrollingTemplate.h>
#include <PhosphorZones/ScrollingTemplateStore.h>

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

using PhosphorZones::ScrollingTemplate;
using PhosphorZones::ScrollingTemplateStore;

namespace {

ScrollingTemplate makeTemplate()
{
    ScrollingTemplate templ;
    templ.id = QUuid::createUuid();
    templ.name = QStringLiteral("Test template");
    templ.columns = {{0.6, 0}, {0.4, 1}};
    return templ;
}

bool writeTemplate(const QString& dir, const ScrollingTemplate& templ)
{
    if (!QDir().mkpath(dir)) {
        return false;
    }
    QFile file(dir + QLatin1Char('/') + templ.id.toString(QUuid::WithoutBraces) + QStringLiteral(".json"));
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    const QByteArray bytes = QJsonDocument(templ.toJson()).toJson();
    return file.write(bytes) == bytes.size();
}

} // namespace

class TestScrollingTemplateStoreLocation : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        // The system half of the search: one data dir this test owns, set
        // before anything asks QStandardPaths. Test mode moves the user half
        // into Qt's own sandbox.
        QVERIFY(m_system.isValid());
        qputenv("XDG_DATA_DIRS", QFile::encodeName(m_system.path()));
        QStandardPaths::setTestModeEnabled(true);
        m_user = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    }

    void init()
    {
        QDir(m_user + QStringLiteral("/phosphor-test")).removeRecursively();
        QDir(m_system.path() + QStringLiteral("/phosphor-test")).removeRecursively();
    }

    void cleanupTestCase()
    {
        QDir(m_user + QStringLiteral("/phosphor-test")).removeRecursively();
    }

    void loadsFromTheSubdirectoryUnderEveryDataDir()
    {
        const ScrollingTemplate bundled = makeTemplate();
        const ScrollingTemplate own = makeTemplate();
        QVERIFY(writeTemplate(m_system.path() + QStringLiteral("/phosphor-test/scrolling-templates"), bundled));
        QVERIFY(writeTemplate(m_user + QStringLiteral("/phosphor-test/scrolling-templates"), own));

        ScrollingTemplateStore store(QStringLiteral("phosphor-test/scrolling-templates"));
        QCOMPARE(store.templateSubdirectory(), QStringLiteral("phosphor-test/scrolling-templates"));
        QCOMPARE(store.userTemplateDirectory(), m_user + QStringLiteral("/phosphor-test/scrolling-templates"));
        store.loadTemplates();

        QCOMPARE(store.count(), 2);
        QVERIFY(store.templateById(bundled.id).isSystem);
        QVERIFY(!store.templateById(own.id).isSystem);
    }

    void savesIntoTheSubdirectoryUnderTheUserDataDir()
    {
        ScrollingTemplateStore store(QStringLiteral("phosphor-test/scrolling-templates"));
        const QUuid id = store.saveTemplate(makeTemplate());
        QVERIFY(!id.isNull());
        QVERIFY(QFile::exists(m_user + QStringLiteral("/phosphor-test/scrolling-templates/")
                              + id.toString(QUuid::WithoutBraces) + QStringLiteral(".json")));
    }

    void withoutASubdirectoryLoadsNothingAndSavesNothing()
    {
        // Templates in the data dirs' ROOTS, which is what locateAll would
        // answer for an empty subdirectory. The store must not read them.
        QVERIFY(writeTemplate(m_system.path(), makeTemplate()));
        QVERIFY(writeTemplate(m_user, makeTemplate()));

        ScrollingTemplateStore store{QString()};
        QVERIFY(store.userTemplateDirectory().isEmpty());
        QSignalSpy changed(&store, &ScrollingTemplateStore::templatesChanged);
        store.loadTemplates();
        QCOMPARE(store.count(), 0);
        QCOMPARE(changed.count(), 0);

        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("no template subdirectory")));
        QVERIFY(store.saveTemplate(makeTemplate()).isNull());
        QCOMPARE(store.count(), 0);

        const QStringList rootFiles = QDir(m_system.path()).entryList({QStringLiteral("*.json")}, QDir::Files);
        for (const QString& file : rootFiles) {
            QFile::remove(m_system.path() + QLatin1Char('/') + file);
        }
        const QStringList userFiles = QDir(m_user).entryList({QStringLiteral("*.json")}, QDir::Files);
        for (const QString& file : userFiles) {
            QFile::remove(m_user + QLatin1Char('/') + file);
        }
    }

private:
    QTemporaryDir m_system;
    QString m_user;
};

QTEST_GUILESS_MAIN(TestScrollingTemplateStoreLocation)
#include "test_scrollingtemplatestore_location.moc"
