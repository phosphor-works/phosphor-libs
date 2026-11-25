// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorWayland/BackgroundEffect.h>

#include "backgroundeffect_internal.h"
#include "qpa/background_effect_protocol.h"
#include "qpa/layershellintegration.h"

#include <QCoreApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QPlatformSurfaceEvent>
#include <QPointer>
#include <QThread>
#include <QWindow>
#include <QtGui/private/qguiapplication_p.h>
#include <QtWaylandClient/private/qwaylanddisplay_p.h>
#include <QtWaylandClient/private/qwaylandintegration_p.h>
#include <QtWaylandClient/private/qwaylandsurface_p.h>
#include <QtWaylandClient/private/qwaylandwindow_p.h>

Q_LOGGING_CATEGORY(lcBackgroundEffect, "phosphorwayland.backgroundeffect")

namespace PhosphorWayland {

namespace {

QPointer<BackgroundEffect> s_instance;
// What blurSupportedChanged last said, so a capabilities event that changes
// nothing about blur (a future capability bit) emits nothing.
bool s_reportedSupport = false;

ext_background_effect_manager_v1* blurManager()
{
    auto* integration = LayerShellIntegration::instance();
    if (!integration) {
        return nullptr;
    }
    auto* manager = integration->backgroundEffectManager();
    if (!manager || !(integration->backgroundEffectCapabilities() & EXT_BACKGROUND_EFFECT_MANAGER_V1_CAPABILITY_BLUR)) {
        return nullptr;
    }
    return manager;
}

/// One window's blur request and the protocol object that carries it. A child
/// of the window, so it goes with it. Tracks the window's platform window and
/// that platform window's wl_surface, which Qt replaces on hide and show, and
/// keeps exactly one ext_background_effect_surface_v1 on the live surface
/// while there is a region to send: the protocol makes a second one on the
/// same surface a fatal error.
class WindowBlur : public QObject
{
    Q_OBJECT

public:
    explicit WindowBlur(QWindow* window)
        : QObject(window)
        , m_window(window)
    {
        window->installEventFilter(this);
        hookPlatformWindow();
    }

    ~WindowBlur() override
    {
        destroyEffect();
    }

    static WindowBlur* of(const QWindow* window)
    {
        return window->findChild<WindowBlur*>(QString(), Qt::FindDirectChildrenOnly);
    }

    QRegion region() const
    {
        return m_region;
    }

    void setRegion(const QRegion& region)
    {
        m_region = region;
        apply();
    }

    /// Bring the protocol state in line with the region and the compositor's
    /// support. Safe to call at any time, any number of times.
    void apply()
    {
        hookPlatformWindow();
        ::wl_surface* surface = currentSurface();
        if (m_effect && m_effectSurface != surface) {
            destroyEffect();
        }
        if (!surface) {
            return; // Sent from wlSurfaceCreated once there is one.
        }
        ext_background_effect_manager_v1* manager = blurManager();
        if (!manager || m_region.isEmpty()) {
            if (m_effect) {
                // Destroying the object removes the effect on the next commit.
                destroyEffect();
                m_window->requestUpdate();
            }
            return;
        }
        auto* integration = LayerShellIntegration::instance();
        QtWayland::wl_compositor* compositor = integration->display()->compositor();
        if (!compositor) {
            return;
        }
        if (!m_effect) {
            m_effect = ext_background_effect_manager_v1_get_background_effect(manager, surface);
            m_effectSurface = surface;
        }
        // Window-local to surface-local: a window with client-side decorations
        // has its content inset in the surface by the decoration margins.
        const QMargins margins = m_waylandWindow->windowContentMargins();
        ::wl_region* wlRegion = compositor->create_region();
        for (const QRect& rect : m_region) {
            wl_region_add(wlRegion, rect.x() + margins.left(), rect.y() + margins.top(), rect.width(), rect.height());
        }
        ext_background_effect_surface_v1_set_blur_region(m_effect, wlRegion);
        wl_region_destroy(wlRegion);
        qCDebug(lcBackgroundEffect) << "blur region" << m_region << "for" << m_window;
        // The region is double-buffered surface state: it takes effect on the
        // next commit, which the next frame makes.
        m_window->requestUpdate();
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (watched == m_window && event->type() == QEvent::PlatformSurface) {
            switch (static_cast<QPlatformSurfaceEvent*>(event)->surfaceEventType()) {
            case QPlatformSurfaceEvent::SurfaceCreated:
                apply();
                break;
            case QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed:
                destroyEffect();
                unhookPlatformWindow();
                break;
            }
        }
        return QObject::eventFilter(watched, event);
    }

private:
    ::wl_surface* currentSurface() const
    {
        // waylandSurface(), not wlSurface(): the latter takes the window's
        // surface lock, which Qt can hold while it emits the surface signals.
        QtWaylandClient::QWaylandSurface* surface = m_waylandWindow ? m_waylandWindow->waylandSurface() : nullptr;
        return surface ? surface->object() : nullptr;
    }

    void hookPlatformWindow()
    {
        auto* waylandWindow = dynamic_cast<QtWaylandClient::QWaylandWindow*>(m_window->handle());
        if (waylandWindow == m_waylandWindow) {
            return;
        }
        unhookPlatformWindow();
        m_waylandWindow = waylandWindow;
        if (!waylandWindow) {
            return;
        }
        m_surfaceCreated = connect(waylandWindow, &QtWaylandClient::QWaylandWindow::wlSurfaceCreated, this,
                                   &WindowBlur::apply, Qt::DirectConnection);
        // Direct, and nothing but the destroy: Qt emits this while it still
        // holds the surface, which the effect object must not outlive.
        m_surfaceDestroyed = connect(waylandWindow, &QtWaylandClient::QWaylandWindow::wlSurfaceDestroyed, this,
                                     &WindowBlur::destroyEffect, Qt::DirectConnection);
    }

    void unhookPlatformWindow()
    {
        QObject::disconnect(m_surfaceCreated);
        QObject::disconnect(m_surfaceDestroyed);
        m_waylandWindow = nullptr;
    }

    void destroyEffect()
    {
        if (!m_effect) {
            return;
        }
        ext_background_effect_surface_v1_destroy(m_effect);
        m_effect = nullptr;
        m_effectSurface = nullptr;
    }

    QWindow* m_window;
    QRegion m_region;
    QPointer<QtWaylandClient::QWaylandWindow> m_waylandWindow;
    QMetaObject::Connection m_surfaceCreated;
    QMetaObject::Connection m_surfaceDestroyed;
    ext_background_effect_surface_v1* m_effect = nullptr;
    ::wl_surface* m_effectSurface = nullptr;
};

} // namespace

BackgroundEffect::BackgroundEffect(QObject* parent)
    : QObject(parent)
{
    // Qt loads the shell integration with the first window, and only it binds
    // the manager. Load it now, so blurSupported is already true or false for
    // real when the app first asks, rather than false until a window exists.
    if (auto* wayland =
            dynamic_cast<QtWaylandClient::QWaylandIntegration*>(QGuiApplicationPrivate::platformIntegration())) {
        wayland->shellIntegration();
    }
    s_reportedSupport = isBlurSupported();
}

BackgroundEffect* BackgroundEffect::instance()
{
    Q_ASSERT_X(qobject_cast<QGuiApplication*>(QCoreApplication::instance()), "BackgroundEffect::instance",
               "requires a QGuiApplication");
    Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());
    if (!s_instance) {
        s_instance = new BackgroundEffect(QCoreApplication::instance());
    }
    return s_instance;
}

bool BackgroundEffect::isBlurSupported() const
{
    return blurManager() != nullptr;
}

void BackgroundEffect::setBlurRegion(QWindow* window, const QRegion& region)
{
    if (!window) {
        return;
    }
    WindowBlur* blur = WindowBlur::of(window);
    if (!blur) {
        if (region.isEmpty()) {
            return;
        }
        blur = new WindowBlur(window);
    }
    blur->setRegion(region);
}

QRegion BackgroundEffect::blurRegion(const QWindow* window) const
{
    const WindowBlur* blur = window ? WindowBlur::of(window) : nullptr;
    return blur ? blur->region() : QRegion();
}

void backgroundEffectCapabilitiesChanged()
{
    if (!s_instance) {
        // Nothing has asked for blur, so no window holds a region to resend.
        return;
    }
    // Resent rather than trusted to survive: a compositor that drops the
    // capability stops applying the effect, and the protocol does not say
    // that regaining it restores the old regions.
    const QWindowList windows = QGuiApplication::allWindows();
    for (QWindow* window : windows) {
        if (WindowBlur* blur = WindowBlur::of(window)) {
            blur->apply();
        }
    }
    const bool supported = s_instance->isBlurSupported();
    if (supported != s_reportedSupport) {
        s_reportedSupport = supported;
        Q_EMIT s_instance->blurSupportedChanged(supported);
    }
}

} // namespace PhosphorWayland

#include "backgroundeffect.moc"
