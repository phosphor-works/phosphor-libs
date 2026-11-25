// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: LGPL-2.1-or-later

// BackgroundEffect without a compositor: the offscreen platform, where the
// phosphorwayland shell integration is not loaded. What this covers is the
// no-op contract and the per-window bookkeeping. The protocol traffic itself
// (the manager's capabilities, the per-surface object across a wl_surface
// recreation) needs a compositor that speaks ext-background-effect-v1, which
// the test environment does not have.

#include <PhosphorWayland/BackgroundEffect.h>

#include <QGuiApplication>
#include <QRegion>
#include <QSignalSpy>
#include <QTest>
#include <QWindow>

#include <memory>

using PhosphorWayland::BackgroundEffect;

class TestBackgroundEffect : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void instanceIsOnePerProcess()
    {
        BackgroundEffect* first = BackgroundEffect::instance();
        QVERIFY(first);
        QCOMPARE(BackgroundEffect::instance(), first);
        QCOMPARE(first->parent(), QCoreApplication::instance());
    }

    void blurIsUnsupportedWithoutTheShellIntegration()
    {
        QVERIFY(!BackgroundEffect::instance()->isBlurSupported());
        QVERIFY(!BackgroundEffect::instance()->property("blurSupported").toBool());
    }

    void regionIsRememberedWhileUnsupported()
    {
        // Unsupported is a no-op toward the compositor, not a refusal: the
        // region is kept, so it can be sent once blur becomes available.
        BackgroundEffect* effect = BackgroundEffect::instance();
        QSignalSpy changed(effect, &BackgroundEffect::blurSupportedChanged);
        QWindow window;
        const QRegion band = QRegion(0, 0, 400, 32) + QRegion(100, 32, 200, 200);

        effect->setBlurRegion(&window, band);
        QCOMPARE(effect->blurRegion(&window), band);

        effect->setBlurRegion(&window, QRegion(0, 0, 10, 10));
        QCOMPARE(effect->blurRegion(&window), QRegion(0, 0, 10, 10));

        effect->setBlurRegion(&window, QRegion());
        QVERIFY(effect->blurRegion(&window).isEmpty());
        QCOMPARE(changed.count(), 0);
    }

    void regionsAreKeptPerWindow()
    {
        BackgroundEffect* effect = BackgroundEffect::instance();
        QWindow a;
        QWindow b;
        effect->setBlurRegion(&a, QRegion(0, 0, 50, 50));
        QCOMPARE(effect->blurRegion(&a), QRegion(0, 0, 50, 50));
        QVERIFY(effect->blurRegion(&b).isEmpty());
    }

    void emptyRegionOnAnUntouchedWindowAddsNothing()
    {
        QWindow window;
        const int childrenBefore = window.children().size();
        BackgroundEffect::instance()->setBlurRegion(&window, QRegion());
        QCOMPARE(window.children().size(), childrenBefore);
    }

    void nullWindowIsIgnored()
    {
        BackgroundEffect::instance()->setBlurRegion(nullptr, QRegion(0, 0, 10, 10));
        QVERIFY(BackgroundEffect::instance()->blurRegion(nullptr).isEmpty());
    }

    void survivesThePlatformWindowComingAndGoing()
    {
        // A region set before the window has a platform window, then the
        // window shown, hidden, destroyed and shown again, then the window
        // itself destroyed: the platform-surface events the per-window state
        // follows all fire, with no Wayland window behind them here.
        BackgroundEffect* effect = BackgroundEffect::instance();
        auto window = std::make_unique<QWindow>();
        window->resize(200, 100);
        effect->setBlurRegion(window.get(), QRegion(0, 0, 200, 40));

        window->show();
        effect->setBlurRegion(window.get(), QRegion(0, 0, 200, 60));
        window->hide();
        window->destroy();
        window->show();
        QCOMPARE(effect->blurRegion(window.get()), QRegion(0, 0, 200, 60));
        window.reset();
    }
};

QTEST_MAIN(TestBackgroundEffect)
#include "test_backgroundeffect.moc"
