// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorPaths/SuiteDirectories.h>

#include <QDir>
#include <QStandardPaths>
#include <QtGlobal>

namespace PhosphorPaths {

namespace {
QString& storedName()
{
    static QString name;
    return name;
}

/// A generic writable location, or @p homeFallback under the home directory
/// when QStandardPaths has none (a sandbox with no HOME-derived roots).
QString genericLocation(QStandardPaths::StandardLocation location, const char* homeFallback)
{
    const QString dir = QStandardPaths::writableLocation(location);
    return dir.isEmpty() ? QDir::homePath() + QLatin1Char('/') + QLatin1String(homeFallback) : dir;
}
} // namespace

QString suiteDirectoryName()
{
    return QStringLiteral(PHOSPHORPATHS_SUITE_DIRECTORY_NAME);
}

QString commonDataSubdir(const QString& tree)
{
    return suiteDirectoryName() + QLatin1Char('/') + tree;
}

void setAppDirectoryName(const QString& name)
{
    // One path segment: empty would put the files at the root of the user's
    // config directory, and a separator would reach outside it.
    if (name.isEmpty() || name.contains(QLatin1Char('/')) || name == QLatin1String(".")
        || name == QLatin1String("..")) {
        qFatal("setAppDirectoryName: \"%s\" is not a single directory name", qPrintable(name));
    }
    storedName() = name;
}

QString appDirectoryName()
{
    const QString& name = storedName();
    if (name.isEmpty()) {
        qFatal("appDirectoryName() read before setAppDirectoryName(): call it at the top of main()");
    }
    return name;
}

QString appSubdir()
{
    return suiteDirectoryName() + QLatin1Char('/') + appDirectoryName();
}

QString appConfigDir()
{
    return genericLocation(QStandardPaths::GenericConfigLocation, ".config") + QLatin1Char('/') + appSubdir();
}

QString appDataDir()
{
    return genericLocation(QStandardPaths::GenericDataLocation, ".local/share") + QLatin1Char('/') + appSubdir();
}

QString appStateDir()
{
    return genericLocation(QStandardPaths::GenericStateLocation, ".local/state") + QLatin1Char('/') + appSubdir();
}

QString appCacheDir()
{
    return genericLocation(QStandardPaths::GenericCacheLocation, ".cache") + QLatin1Char('/') + appSubdir();
}

} // namespace PhosphorPaths
