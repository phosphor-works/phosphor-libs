// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorEngineRuntime/RuntimeI18n.h>

#include <PhosphorEngineRuntime/Logging.h>

#include <QLocale>
#include <QTranslator>

#include <memory>

namespace PhosphorEngineRuntime {

void loadRuntimeTranslations(QCoreApplication* app)
{
    Q_ASSERT(app && "loadRuntimeTranslations() must be called after QCoreApplication is created");
    if (!app) {
        return;
    }
    // Owned here until it loads, then by the application it is installed on.
    auto translator = std::make_unique<QTranslator>();
    const QLocale locale;
    // Installed beside the binary's share/, then the build tree root, where
    // qt_add_lrelease writes the .qm files.
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList searchDirs = {
        appDir + QStringLiteral("/../share/phosphor-works/translations"),
        appDir + QStringLiteral("/.."),
    };
    for (const QString& dir : searchDirs) {
        if (translator->load(locale, QStringLiteral("phosphor-engine-runtime"), QStringLiteral("_"), dir)) {
            translator->setParent(app);
            app->installTranslator(translator.release());
            qCDebug(lcCore) << "Loaded engine runtime translations from" << dir << "for locale" << locale.name();
            return;
        }
    }
    qCDebug(lcCore) << "No engine runtime translations found for locale" << locale.name();
}

} // namespace PhosphorEngineRuntime
