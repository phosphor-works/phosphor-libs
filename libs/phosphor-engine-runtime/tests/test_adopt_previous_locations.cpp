// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// The first start without a config.json under phosphor-works moves 3.x's
// config directory and user data trees into the new places once, and the old
// places are gone afterwards.

#include <PhosphorEngineRuntime/AppDirectoryName.h>
#include <PhosphorEngineRuntime/ConfigDefaults.h>
#include <PhosphorEngineRuntime/ConfigKeys.h>
#include <PhosphorEngineRuntime/ConfigMigration.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTest>

using namespace PhosphorEngineRuntime;

namespace {

bool writeFile(const QString& path, const QByteArray& content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(content) == content.size();
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

} // namespace

class TestAdoptPreviousLocations : public QObject
{
    Q_OBJECT

private:
    QString m_configRoot;
    QString m_dataRoot;

private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        setAppDirectoryName(QStringLiteral("adopttest"));
        m_configRoot = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
        m_dataRoot = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    }

    void init()
    {
        QDir(m_configRoot + QStringLiteral("/adopttest")).removeRecursively();
        QDir(m_configRoot + QStringLiteral("/phosphor-works")).removeRecursively();
        QDir(m_dataRoot + QStringLiteral("/adopttest")).removeRecursively();
        QDir(m_dataRoot + QStringLiteral("/phosphor-works")).removeRecursively();
        ConfigMigration::resetMigrationGuardForTesting();
    }

    void movesTheOldConfigAndDataOnce()
    {
        const QByteArray config =
            QByteArrayLiteral("{\"_version\": ") + QByteArray::number(ConfigSchemaVersion) + QByteArrayLiteral("}");
        QVERIFY(writeFile(m_configRoot + QStringLiteral("/adopttest/config.json"), config));
        QVERIFY(writeFile(m_configRoot + QStringLiteral("/adopttest/rules.json"), QByteArrayLiteral("{}")));
        QVERIFY(
            writeFile(m_dataRoot + QStringLiteral("/adopttest/layouts/mine.json"), QByteArrayLiteral("{\"id\":1}")));

        QVERIFY(ConfigMigration::ensureJsonConfig());

        QVERIFY(QFileInfo::exists(ConfigDefaults::configFilePath()));
        QCOMPARE(ConfigDefaults::configFilePath(),
                 m_configRoot + QStringLiteral("/phosphor-works/adopttest/config.json"));
        QVERIFY(QFileInfo::exists(ConfigDefaults::rulesFilePath()));
        const QString movedLayout =
            m_dataRoot + QLatin1Char('/') + ConfigKeys::layoutsSubdir() + QStringLiteral("/mine.json");
        QCOMPARE(readFile(movedLayout), QByteArrayLiteral("{\"id\":1}"));

        QVERIFY2(!QFileInfo::exists(m_configRoot + QStringLiteral("/adopttest")), "the old config directory is gone");
        QVERIFY2(!QFileInfo::exists(m_dataRoot + QStringLiteral("/adopttest")), "the old data directory is gone");
    }

    void leavesANewConfigAlone()
    {
        const QByteArray current = QByteArrayLiteral("{\"_version\": ") + QByteArray::number(ConfigSchemaVersion)
            + QByteArrayLiteral(", \"marker\": \"new\"}");
        QVERIFY(writeFile(ConfigDefaults::configFilePath(), current));
        QVERIFY(writeFile(m_configRoot + QStringLiteral("/adopttest/config.json"),
                          QByteArrayLiteral("{\"marker\":\"old\"}")));

        QVERIFY(ConfigMigration::ensureJsonConfig());

        QVERIFY(QFileInfo::exists(ConfigDefaults::configFilePath()));
        QVERIFY2(QFileInfo::exists(m_configRoot + QStringLiteral("/adopttest/config.json")),
                 "with a config.json in the new place, the old one is not read or moved");
    }
};

QTEST_GUILESS_MAIN(TestAdoptPreviousLocations)
#include "test_adopt_previous_locations.moc"
