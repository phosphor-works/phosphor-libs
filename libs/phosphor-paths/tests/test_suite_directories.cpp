// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorPaths/SuiteDirectories.h>

#include <QStandardPaths>
#include <QTest>

class TestSuiteDirectories : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        PhosphorPaths::setAppDirectoryName(QStringLiteral("pathstest"));
    }

    void onlyOneDirectoryNameIsValid()
    {
        QVERIFY(PhosphorPaths::isValidDirectoryName(QStringLiteral("plasmazones")));
        QVERIFY(PhosphorPaths::isValidDirectoryName(QStringLiteral("my app.v2")));
        const QStringList refused{QString(),
                                  QStringLiteral("   "),
                                  QStringLiteral("."),
                                  QStringLiteral(".."),
                                  QStringLiteral("../etc/passwd"),
                                  QStringLiteral("foo/bar"),
                                  QStringLiteral("/etc"),
                                  QStringLiteral("a\\b"),
                                  QStringLiteral("line\nbreak"),
                                  QString(QStringLiteral("nul")) + QChar(0)};
        for (const QString& name : refused) {
            QVERIFY2(!PhosphorPaths::isValidDirectoryName(name), qPrintable(name));
        }
    }

    void sharedTreesSitUnderTheSuiteFolder()
    {
        QCOMPARE(PhosphorPaths::suiteDirectoryName(), QStringLiteral("phosphor-works"));
        QCOMPARE(PhosphorPaths::commonDataSubdir(QStringLiteral("layouts")), QStringLiteral("phosphor-works/layouts"));
        QCOMPARE(PhosphorPaths::commonDataSubdir(QStringLiteral("shaders/surface")),
                 QStringLiteral("phosphor-works/shaders/surface"));
    }

    void appFilesSitInTheAppsFolder()
    {
        QCOMPARE(PhosphorPaths::appDirectoryName(), QStringLiteral("pathstest"));
        QCOMPARE(PhosphorPaths::appSubdir(), QStringLiteral("phosphor-works/pathstest"));
        const QString suffix = QStringLiteral("/phosphor-works/pathstest");
        QCOMPARE(PhosphorPaths::appConfigDir(),
                 QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + suffix);
        QCOMPARE(PhosphorPaths::appDataDir(),
                 QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + suffix);
        QCOMPARE(PhosphorPaths::appStateDir(),
                 QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) + suffix);
        QCOMPARE(PhosphorPaths::appCacheDir(),
                 QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + suffix);
    }
};

QTEST_GUILESS_MAIN(TestSuiteDirectories)
#include "test_suite_directories.moc"
