// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: LGPL-2.1-or-later

// The library against the surface packs the apps ship, from the
// phosphor-surface-packs commit cmake/PhosphorDependencies.cmake pins. Built
// only when that tree is present (PHOSPHOR_SURFACE_PACKS_DIR).

#include <PhosphorSurface/SurfacePackLocation.h>
#include <PhosphorSurface/SurfaceShaderEffect.h>
#include <PhosphorSurface/SurfaceShaderRegistry.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest/QtTest>

using namespace PhosphorSurfaceShaders;

namespace {

QString packsDir()
{
    return QFileInfo(QStringLiteral(PHOSPHOR_SURFACE_PACKS_DIR)).canonicalFilePath();
}

QString packsSharedDir()
{
    return packsDir() + QStringLiteral("/shared");
}

/// Every `builtin:` token the registry resolves, with the file each names in
/// the packs' shared/ directory.
QList<std::pair<QString, QString>> builtinTokens()
{
    return {
        {QStringLiteral("builtin:gaussian-h"), QStringLiteral("gaussian_h.frag")},
        {QStringLiteral("builtin:gaussian-v"), QStringLiteral("gaussian_v.frag")},
        {QStringLiteral("builtin:kawase-down-0"), QStringLiteral("kawase_down_0.frag")},
        {QStringLiteral("builtin:kawase-down-1"), QStringLiteral("kawase_down_1.frag")},
        {QStringLiteral("builtin:kawase-down-2"), QStringLiteral("kawase_down_2.frag")},
        {QStringLiteral("builtin:kawase-down-3"), QStringLiteral("kawase_down_3.frag")},
        {QStringLiteral("builtin:kawase-up-0"), QStringLiteral("kawase_up_0.frag")},
        {QStringLiteral("builtin:kawase-up-1"), QStringLiteral("kawase_up_1.frag")},
        {QStringLiteral("builtin:kawase-up-2"), QStringLiteral("kawase_up_2.frag")},
    };
}

} // namespace

class TestSurfacePacks : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY2(QDir(packsSharedDir()).exists(),
                 qPrintable(QStringLiteral("no shared/ in the pinned packs: %1").arg(packsDir())));
    }

    void cleanup()
    {
        setSurfacePackDataSubdir(QString());
    }

    void every_pack_registers()
    {
        SurfaceShaderRegistry registry;
        registry.addSearchPaths(QStringList{packsDir()}, PhosphorFsLoader::LiveReload::Off);

        const QStringList entries = QDir(packsDir()).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        int packs = 0;
        for (const QString& entry : entries) {
            if (!QFile::exists(packsDir() + QLatin1Char('/') + entry + QStringLiteral("/metadata.json"))) {
                continue; // shared/
            }
            ++packs;
            QVERIFY2(registry.hasEffect(entry), qPrintable(QStringLiteral("pack did not register: %1").arg(entry)));
        }
        QVERIFY(packs > 0);
        QCOMPARE(registry.effectIds().size(), packs);
    }

    void multipass_packs_resolve_their_builtin_passes_from_the_shared_dir()
    {
        SurfaceShaderRegistry registry;
        registry.addSearchPaths(QStringList{packsDir()}, PhosphorFsLoader::LiveReload::Off);

        int multipass = 0;
        const QList<SurfaceShaderEffect> effects = registry.availableEffects();
        for (const SurfaceShaderEffect& effect : effects) {
            if (!effect.isMultipass) {
                continue;
            }
            ++multipass;
            QVERIFY2(!effect.bufferShaderPaths.isEmpty(), qPrintable(effect.id));
            for (const QString& path : effect.bufferShaderPaths) {
                QVERIFY2(
                    QFileInfo(path).canonicalFilePath().startsWith(packsSharedDir() + QLatin1Char('/')),
                    qPrintable(QStringLiteral("%1 resolved a buffer pass outside shared/: %2").arg(effect.id, path)));
            }
        }
        // The blur family, which every glass pack builds on.
        QVERIFY(multipass > 0);
    }

    void every_builtin_token_names_a_shipped_file()
    {
        // Resolved through the sibling probe, as for a pack inside the tree.
        const QString effectDir = packsDir() + QStringLiteral("/blur");
        for (const auto& [token, file] : builtinTokens()) {
            const QString resolved = SurfaceShaderRegistry::resolveBuiltinBufferShader(token, effectDir);
            QCOMPARE(resolved, packsSharedDir() + QLatin1Char('/') + file);
        }
    }

    void installed_packs_resolve_through_the_data_subdir()
    {
        // The packs installed the way phosphor_install_surface_packs() lays
        // them out, under a data dir, with the app naming the subdir it set at
        // install time. A pack with no shared/ beside it (a user pack) then
        // finds the builtin passes through that subdir.
        QTemporaryDir dataDir;
        QVERIFY(dataDir.isValid());
        const QString subdir = QStringLiteral(PHOSPHOR_SURFACE_PACKS_DATA_SUBDIR);
        const QString installed = dataDir.path() + QLatin1Char('/') + subdir;
        QVERIFY(QDir().mkpath(QFileInfo(installed).absolutePath()));
        QVERIFY(QFile::link(packsDir(), installed));

        const bool hadDataDirs = qEnvironmentVariableIsSet("XDG_DATA_DIRS");
        const QByteArray savedDataDirs = qgetenv("XDG_DATA_DIRS");
        qputenv("XDG_DATA_DIRS", QFile::encodeName(dataDir.path()));
        const QString userPack = dataDir.path() + QStringLiteral("/elsewhere/user-pack");

        const QString unset =
            SurfaceShaderRegistry::resolveBuiltinBufferShader(QStringLiteral("builtin:kawase-down-0"), userPack);
        setSurfacePackDataSubdir(subdir);
        const QString set =
            SurfaceShaderRegistry::resolveBuiltinBufferShader(QStringLiteral("builtin:kawase-down-0"), userPack);
        if (hadDataDirs) {
            qputenv("XDG_DATA_DIRS", savedDataDirs);
        } else {
            qunsetenv("XDG_DATA_DIRS");
        }

        QVERIFY2(unset.isEmpty(), qPrintable(unset));
        QCOMPARE(set, packsSharedDir() + QStringLiteral("/kawase_down_0.frag"));
    }
};

QTEST_GUILESS_MAIN(TestSurfacePacks)
#include "test_surfacepacks.moc"
