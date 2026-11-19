// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// The settings model as a host embeds it: the host names its config
// directory and extends the schema with keys of its own, and a value saved
// by one Settings is what the next one reads.

#include <PhosphorEngineRuntime/AppDirectoryName.h>
#include <PhosphorEngineRuntime/ConfigDefaults.h>
#include <PhosphorEngineRuntime/Settings.h>
#include <PhosphorEngineRuntime/SettingsSchema.h>

#include <PhosphorConfig/Schema.h>

#include <QSignalSpy>
#include <QTest>

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

private Q_SLOTS:
    void initTestCase()
    {
        setAppDirectoryName(QStringLiteral("phosphor-engine-runtime-test"));
        setHostSettingsExtension({&appendTestHostSchema, &testHostMachineScoped, nullptr});
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
