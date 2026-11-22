// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// The QPA plugin against a build of this library newer than its own.
//
// Qt loads the plugin from its plugin path, often the system's, into an
// application that may link its own build of libPhosphorWayland.so.0. The
// dynamic loader binds the plugin to the library already loaded under that
// soname, so the plugin runs against a library whose classes may have grown
// since the plugin was built. A plugin that allocated the integration itself
// allocated the size it was built with, and the library's constructor then
// wrote past the end of it: heap corruption, seen as "malloc(): unaligned
// tcache chunk" in LayerShellIntegration::initialize.
//
// This test links PhosphorWaylandGrownLayout, a build of the library whose
// LayerShellIntegration is larger than the one the plugin was built against,
// and loads the real plugin. Run it under ASan to see the overflow itself.

#include "qpa/layershellintegration.h"

#include <QtWaylandClient/private/qwaylandshellintegrationplugin_p.h>

#include <QPluginLoader>
#include <QTest>

#include <memory>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

#ifndef PHOSPHORWAYLAND_TEST_LAYOUT_GROWTH
#error "test_plugin_layout must link PhosphorWaylandGrownLayout"
#endif

using PhosphorWayland::LayerShellIntegration;

class TestPluginLayout : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void createsTheIntegrationAtTheLibrarysSize()
    {
        QPluginLoader loader(QStringLiteral(PHOSPHORWAYLAND_QPA_PLUGIN));
        QVERIFY2(loader.load(), qPrintable(loader.errorString()));
        auto* plugin = qobject_cast<QtWaylandClient::QWaylandShellIntegrationPlugin*>(loader.instance());
        QVERIFY(plugin);

        const std::unique_ptr<QtWaylandClient::QWaylandShellIntegration> integration(
            plugin->create(QStringLiteral("phosphorwayland"), QStringList()));
        QVERIFY(integration);
#if defined(__GLIBC__)
        // The allocation holds the class as this library has it, grown.
        auto* created = static_cast<LayerShellIntegration*>(integration.get());
        QVERIFY2(malloc_usable_size(created) >= sizeof(LayerShellIntegration),
                 qPrintable(QStringLiteral("%1 bytes allocated for a %2-byte LayerShellIntegration")
                                .arg(malloc_usable_size(created))
                                .arg(sizeof(LayerShellIntegration))));
#endif
    }
};

QTEST_GUILESS_MAIN(TestPluginLayout)
#include "test_plugin_layout.moc"
