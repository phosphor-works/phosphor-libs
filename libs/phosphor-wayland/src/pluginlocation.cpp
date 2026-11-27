// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "PhosphorWayland/LayerShellPluginLoader.h"

#include <QFileInfo>
#include <QString>

#include <dlfcn.h>

namespace PhosphorWayland {

QString bundledPluginDirectory()
{
    // The file this library was loaded from. An app that embeds the libraries
    // installs the plugin below it, in plugins/wayland-shell-integration, and so
    // does the build tree; a developer install puts the plugin in Qt's own plugin
    // directory instead, where Qt finds it without help.
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void*>(&bundledPluginDirectory), &info) == 0 || !info.dli_fname) {
        return {};
    }
    const QString plugins =
        QFileInfo(QString::fromLocal8Bit(info.dli_fname)).absolutePath() + QStringLiteral("/plugins");
    if (!QFileInfo::exists(plugins + QStringLiteral("/wayland-shell-integration"))) {
        return {};
    }
    return plugins;
}

} // namespace PhosphorWayland
