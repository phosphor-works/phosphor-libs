// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <functional>
#include <memory>
#include <utility>
#include <vector>
#include <QCoreApplication>
#include <QThread>
#include <QtWaylandClient/private/qwaylandshellintegration_p.h>
#include <phosphorwayland_export.h>
#include "wlr_layer_shell_protocol.h"
#include "single_pixel_buffer_protocol.h"
#include "idle_notify_protocol.h"
#include "idle_inhibit_protocol.h"
#include "xdg_toplevel_drag_protocol.h"
#include "foreign_toplevel_protocol.h"
#include "data_control_protocol.h"
#include "session_lock_protocol.h"
#include "background_effect_protocol.h"

namespace PhosphorWayland {

/// QPA shell integration plugin that binds zwlr_layer_shell_v1 and creates
/// layer surfaces for windows marked with the _ps_layer_shell property.
/// For regular (non-layer-shell) windows, delegates to Qt's built-in xdg-shell
/// integration so they get proper xdg_toplevel/xdg_popup roles.
class PHOSPHORWAYLAND_EXPORT LayerShellIntegration : public QtWaylandClient::QWaylandShellIntegration
{
public:
    Q_DISABLE_COPY_MOVE(LayerShellIntegration)

    /// A new integration, for the plugin to hand to Qt, which deletes it.
    ///
    /// The plugin is a separate module that links this library, and is often
    /// installed apart from it: a system plugin and an application's own copy
    /// of the library, under the same soname, end up in one process. Were the
    /// plugin to call `new LayerShellIntegration`, the size of this class as
    /// the plugin's headers had it would be built into the plugin, and this
    /// library's constructor, which initialises the class as it is now, would
    /// write past the end of a smaller allocation. Allocating here keeps the
    /// size and the constructor in the same library, so the plugin knows
    /// nothing of the layout.
    static LayerShellIntegration* create();

    ~LayerShellIntegration() override;

    bool initialize(QtWaylandClient::QWaylandDisplay* display) override;
    QtWaylandClient::QWaylandShellSurface* createShellSurface(QtWaylandClient::QWaylandWindow* window) override;

    struct zwlr_layer_shell_v1* layerShell() const
    {
        return m_globalAvailable ? m_layerShell : nullptr;
    }

    /// The protocol version we negotiated with the compositor (1-5).
    /// Callers should check this before using version-gated features:
    ///   v2: set_layer (runtime layer changes)
    ///   v3: destroy request
    ///   v4: on_demand keyboard interactivity
    ///   v5: set_exclusive_edge
    uint32_t boundVersion() const
    {
        return m_boundVersion;
    }

    /// Access the singleton instance (available after Qt loads the plugin).
    /// Must only be called from the GUI thread.
    static LayerShellIntegration* instance()
    {
        Q_ASSERT(!qApp || QThread::currentThread() == qApp->thread());
        return s_instance;
    }

    // Public for C callback struct initialization
    static void registryHandler(void* data, struct wl_registry* registry, uint32_t id, const char* interface,
                                uint32_t version);
    static void registryRemoveHandler(void* data, struct wl_registry* registry, uint32_t id);

    /// Register a callback invoked when the compositor removes the
    /// zwlr_layer_shell_v1 global (compositor crash/restart).
    /// Returns an opaque ID that can be passed to removeGlobalRemovedCallback()
    /// to prevent use-after-free when the caller is destroyed before the event fires.
    using GlobalRemovedCallback = std::function<void()>;
    using CallbackId = uint64_t;
    /// Note: if the global has already been removed (m_globalAvailable == false),
    /// the callback will never fire. Callers registering after removal should
    /// check layerShell() == nullptr and handle the "already gone" case directly.
    CallbackId addGlobalRemovedCallback(GlobalRemovedCallback cb);
    void removeGlobalRemovedCallback(CallbackId id);

    /// Drain and invoke every registered global-removed callback exactly
    /// once. `registryRemoveHandler` calls this from the layer-shell branch
    /// when the compositor removes the zwlr_layer_shell_v1 global.
    /// Exposed publicly so unit tests can drive dispatch without standing
    /// up a real Wayland connection — the previous test path (calling
    /// `registryRemoveHandler` with `id=0` on the assumption that
    /// `m_layerShellId == 0` would match) aliased with every other
    /// optional-protocol id field that also defaults to 0, so the
    /// single-pixel-buffer branch matched first and the callbacks were
    /// never reached.
    void fireGlobalRemovedCallbacks();

    struct wp_single_pixel_buffer_manager_v1* singlePixelBufferManager() const
    {
        return m_singlePixelBufferAvailable ? m_singlePixelBufferManager : nullptr;
    }

    struct ext_idle_notifier_v1* idleNotifier() const
    {
        return m_idleNotifierAvailable ? m_idleNotifier : nullptr;
    }

    struct zwp_idle_inhibit_manager_v1* idleInhibitManager() const
    {
        return m_idleInhibitManagerAvailable ? m_idleInhibitManager : nullptr;
    }

    struct xdg_toplevel_drag_manager_v1* toplevelDragManager() const
    {
        return m_toplevelDragManagerAvailable ? m_toplevelDragManager : nullptr;
    }

    struct zwlr_foreign_toplevel_manager_v1* foreignToplevelManager() const
    {
        return m_foreignToplevelManagerAvailable ? m_foreignToplevelManager : nullptr;
    }
    /// Raw proxy accessor that bypasses the availability gate. Used
    /// by ~ForeignToplevelManager to null its listener user_data even
    /// when the global has already been removed — without this path,
    /// the manager's destructor can't reach the proxy and a delayed
    /// `finished` / `toplevel` event would dereference the freed
    /// Private*. Returns nullptr only when the proxy itself was never
    /// bound or has already been cleared by a previous destruction.
    struct zwlr_foreign_toplevel_manager_v1* rawForeignToplevelManagerProxy() const
    {
        return m_foreignToplevelManager;
    }
    /// Mirror null-out: ~ForeignToplevelManager calls this after
    /// nulling its listener data so a subsequent stop() in the
    /// integration's destructor doesn't redo work on an effectively-
    /// torn-down proxy. Idempotent.
    void clearForeignToplevelManager()
    {
        m_foreignToplevelManager = nullptr;
        m_foreignToplevelManagerAvailable = false;
    }
    /// Negotiated version (1-3). Callers gate version-specific features:
    ///   v2: set_fullscreen / unset_fullscreen
    ///   v3: parent event
    uint32_t foreignToplevelManagerVersion() const
    {
        return m_foreignToplevelManagerVersion;
    }

    /// Client-side clipboard manager of the standard protocol
    /// (`ext_data_control_manager_v1`). ClipboardDevice uses it when the
    /// compositor advertises it. Returns nullptr when it does not.
    struct ext_data_control_manager_v1* extDataControlManager() const
    {
        return m_extDataControlManagerAvailable ? m_extDataControlManager : nullptr;
    }

    /// Client-side clipboard manager of the older wlroots protocol
    /// (`zwlr_data_control_manager_v1`), which upstream deprecates in favor of
    /// the ext protocol. ClipboardDevice uses it only when the compositor has no
    /// ext manager. Bound at version 1 (clipboard selection only;
    /// primary-selection is a v2 feature we do not use). Returns nullptr when
    /// the compositor does not advertise it.
    struct zwlr_data_control_manager_v1* wlrDataControlManager() const
    {
        return m_dataControlManagerAvailable ? m_dataControlManager : nullptr;
    }

    /// Client-side session-lock manager (`ext_session_lock_manager_v1`). Bound at
    /// version 1. Returns nullptr when the compositor does not advertise it.
    struct ext_session_lock_manager_v1* sessionLockManager() const
    {
        return m_sessionLockManagerAvailable ? m_sessionLockManager : nullptr;
    }

    /// The `ext_session_lock_v1` SessionLock currently holds (requested or
    /// granted), or nullptr. Lock surfaces are children of that object, so
    /// `createShellSurface` reads it for a window marked with
    /// `LockSurfaceProps::IsSessionLock`. SessionLock publishes the object
    /// when it sends `lock` and retracts it on release; the integration never
    /// owns it.
    struct ext_session_lock_v1* activeSessionLock() const
    {
        return m_activeSessionLock;
    }
    void setActiveSessionLock(struct ext_session_lock_v1* lock)
    {
        m_activeSessionLock = lock;
        if (!lock) {
            m_activeSessionLockGranted = false;
        }
    }

    /// Whether the compositor has already sent `locked` for
    /// `activeSessionLock()`. Kept beside the object because it decides which
    /// destructor the protocol allows (unlock_and_destroy once granted,
    /// destroy before), and a SessionLock rebuilt by a QML hot reload has to
    /// recover it along with the object — see adoptActiveSessionLock().
    bool activeSessionLockGranted() const
    {
        return m_activeSessionLockGranted;
    }
    void setActiveSessionLockGranted(bool granted)
    {
        m_activeSessionLockGranted = granted;
    }

    /// Background effect manager (`ext_background_effect_manager_v1`). Bound at
    /// version 1. Returns nullptr when the compositor does not advertise it.
    struct ext_background_effect_manager_v1* backgroundEffectManager() const
    {
        return m_backgroundEffectManagerAvailable ? m_backgroundEffectManager : nullptr;
    }

    /// The `ext_background_effect_manager_v1.capability` flags the compositor
    /// last sent. 0 until it sends any, and while the global is not bound.
    uint32_t backgroundEffectCapabilities() const
    {
        return m_backgroundEffectManagerAvailable ? m_backgroundEffectCapabilities : 0;
    }

    // Public for C callback struct initialization
    static void backgroundEffectCapabilitiesHandler(void* data, struct ext_background_effect_manager_v1* manager,
                                                    uint32_t flags);

    /// Access the Wayland display for explicit flushing after surface creation.
    QtWaylandClient::QWaylandDisplay* display() const
    {
        return m_display;
    }

private:
    /// Only through create(), so that no other module allocates one.
    LayerShellIntegration();

    /// Fallback xdg-shell integration for regular (non-layer-shell) windows.
    /// Loaded lazily on first use via Qt's shell integration factory.
    std::unique_ptr<QtWaylandClient::QWaylandShellIntegration> m_xdgShell;
    QtWaylandClient::QWaylandDisplay* m_display = nullptr;

    struct zwlr_layer_shell_v1* m_layerShell = nullptr;
    struct wl_registry* m_registry = nullptr;
    uint32_t m_layerShellId = 0;
    uint32_t m_boundVersion = 0;
    bool m_globalAvailable = false;

    struct wp_single_pixel_buffer_manager_v1* m_singlePixelBufferManager = nullptr;
    uint32_t m_singlePixelBufferManagerId = 0;
    bool m_singlePixelBufferAvailable = false;

    struct ext_idle_notifier_v1* m_idleNotifier = nullptr;
    uint32_t m_idleNotifierId = 0;
    bool m_idleNotifierAvailable = false;

    struct zwp_idle_inhibit_manager_v1* m_idleInhibitManager = nullptr;
    uint32_t m_idleInhibitManagerId = 0;
    bool m_idleInhibitManagerAvailable = false;

    struct xdg_toplevel_drag_manager_v1* m_toplevelDragManager = nullptr;
    uint32_t m_toplevelDragManagerId = 0;
    bool m_toplevelDragManagerAvailable = false;

    struct zwlr_foreign_toplevel_manager_v1* m_foreignToplevelManager = nullptr;
    uint32_t m_foreignToplevelManagerId = 0;
    uint32_t m_foreignToplevelManagerVersion = 0;
    bool m_foreignToplevelManagerAvailable = false;

    struct zwlr_data_control_manager_v1* m_dataControlManager = nullptr;
    uint32_t m_dataControlManagerId = 0;
    bool m_dataControlManagerAvailable = false;
    struct ext_data_control_manager_v1* m_extDataControlManager = nullptr;
    uint32_t m_extDataControlManagerId = 0;
    bool m_extDataControlManagerAvailable = false;

    struct ext_session_lock_manager_v1* m_sessionLockManager = nullptr;
    uint32_t m_sessionLockManagerId = 0;
    bool m_sessionLockManagerAvailable = false;
    struct ext_session_lock_v1* m_activeSessionLock = nullptr;
    bool m_activeSessionLockGranted = false;

    struct ext_background_effect_manager_v1* m_backgroundEffectManager = nullptr;
    uint32_t m_backgroundEffectManagerId = 0;
    uint32_t m_backgroundEffectCapabilities = 0;
    bool m_backgroundEffectManagerAvailable = false;

    std::vector<std::pair<CallbackId, GlobalRemovedCallback>> m_globalRemovedCallbacks;
    CallbackId m_nextCallbackId = 1;

    // Layer-shell proxies parked when the compositor re-advertises
    // the global. Destroying them immediately on re-advertise is
    // unsafe because in-flight events may still be dispatching; the
    // destructor reaps them after dispatch has drained.
    std::vector<struct wl_proxy*> m_staleProxies;

    // GUI-thread-only singleton. All access (initialize(), instance(), destructor)
    // happens on Qt's main thread. Do NOT access from worker threads.
    static LayerShellIntegration* s_instance;

#ifdef PHOSPHORWAYLAND_TEST_LAYOUT_GROWTH
    // test_plugin_layout builds a copy of this library with the class this
    // many bytes larger, to stand for a library newer than the plugin.
    [[maybe_unused]] unsigned char m_testLayoutGrowth[PHOSPHORWAYLAND_TEST_LAYOUT_GROWTH] = {};
#endif
};

} // namespace PhosphorWayland
