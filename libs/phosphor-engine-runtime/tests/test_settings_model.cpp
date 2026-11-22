// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// The settings model as a host embeds it: the host names its config
// directory and extends the schema with keys of its own, and a value saved
// by one Settings is what the next one reads.

#include <PhosphorPaths/SuiteDirectories.h>
#include <PhosphorEngineRuntime/ConfigDefaults.h>
#include <PhosphorEngineRuntime/ConfigMigration.h>
#include <PhosphorEngineRuntime/Settings.h>
#include <PhosphorEngineRuntime/SettingsSchema.h>

#include <PhosphorConfig/Schema.h>

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

using namespace PhosphorEngineRuntime;

namespace {
QString hostGroup()
{
    return QStringLiteral("TestHost");
}
QString hostKey()
{
    return QStringLiteral("Flag");
}
void appendTestHostSchema(PhosphorConfig::Schema& schema)
{
    schema.groups[hostGroup()] = {{hostKey(), false, QMetaType::Bool, QStringLiteral("A test host's own flag.")}};
}
bool testHostMachineScoped(const QString& group, const QString& key)
{
    return group == hostGroup() && key == hostKey();
}
} // namespace

class TestSettingsModel : public QObject
{
    Q_OBJECT

private:
    // Each test function's own config, data, state and cache roots. The
    // functions save values and expect the defaults before they do, so they
    // need an empty config. The XDG sandbox ctest gives them lives in the
    // build directory and keeps what an earlier run saved, and run by hand
    // there is no sandbox at all.
    std::unique_ptr<QTemporaryDir> m_home;

private Q_SLOTS:
    void initTestCase()
    {
        PhosphorPaths::setAppDirectoryName(QStringLiteral("phosphor-engine-runtime-test"));
        setHostSettingsExtension({&appendTestHostSchema, &testHostMachineScoped, nullptr});
    }

    void init()
    {
        m_home = std::make_unique<QTemporaryDir>();
        QVERIFY(m_home->isValid());
        for (const char* variable : {"XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME", "XDG_CACHE_HOME"}) {
            const QString root = m_home->filePath(QString::fromLatin1(variable));
            QVERIFY(QDir().mkpath(root));
            qputenv(variable, QFile::encodeName(root));
        }
        // The guard would skip the migration that sets up the new config
        // directory, since an earlier function's Settings already ran it.
        ConfigMigration::resetMigrationGuardForTesting();
    }

    void cleanup()
    {
        m_home.reset();
    }

    void savedValueReadsBackInANewSettings()
    {
        {
            Settings settings;
            QVERIFY(!settings.workspacesEnabled());
            settings.setWorkspacesEnabled(true);
            QVERIFY(settings.save());
        }
        Settings again;
        QVERIFY(again.workspacesEnabled());
    }

    void hostKeysAreDeclaredStoredAndAnnounced()
    {
        QVERIFY(hostSchemaKeys().contains(std::pair{hostGroup(), hostKey()}));
        QVERIFY(Settings::isMachineScopedKey(hostGroup(), hostKey()));
        QVERIFY(!Settings::isMachineScopedKey(ConfigDefaults::workspacesBehaviorGroup(), ConfigDefaults::enabledKey()));

        Settings settings;
        QSignalSpy changed(&settings, &Settings::hostValueChanged);
        QVERIFY(settings.setHostValue(hostGroup(), hostKey(), true));
        QCOMPARE(changed.count(), 1);
        QVERIFY(settings.hostValue(hostGroup(), hostKey()).toBool());
        QVERIFY(!settings.setHostValue(hostGroup(), QStringLiteral("Undeclared"), true));
        QVERIFY(settings.save());

        Settings again;
        QVERIFY(again.hostValue(hostGroup(), hostKey()).toBool());
    }
};

QTEST_MAIN(TestSettingsModel)
#include "test_settings_model.moc"
