// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include "phosphorwayland_export.h"

#include <QObject>
#include <QRegion>

QT_BEGIN_NAMESPACE
class QWindow;
QT_END_NAMESPACE

namespace PhosphorWayland {

/// Compositor blur behind a region of a window, through the standard
/// ext-background-effect-v1 protocol.
///
/// A Wayland client cannot read what lies behind its own surface, so a
/// translucent material over live windows needs the compositor to blur that
/// backdrop. This asks for it on any QWindow whose platform window comes from
/// the phosphorwayland shell integration: layer surfaces, and the regular
/// windows it hands to xdg-shell. The window then paints its own tint over
/// the blurred backdrop.
///
/// Process-wide and GUI-thread only. Every call is a no-op toward the
/// compositor while blur is unsupported: when the phosphorwayland shell
/// integration is not loaded, when the compositor does not advertise
/// ext_background_effect_manager_v1, or when it does not report the blur
/// capability. The region a window asks for is remembered regardless, and
/// sent once the window has a surface and blur is supported, so a caller sets
/// it once and updates it when its shape changes. It is sent again whenever
/// Qt recreates the window's wl_surface (as it does on hide and show) and
/// whenever the compositor reports the blur capability again after dropping
/// it.
class PHOSPHORWAYLAND_EXPORT BackgroundEffect : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool blurSupported READ isBlurSupported NOTIFY blurSupportedChanged FINAL)

public:
    /// The process's instance, created on first use as a child of the
    /// application object. Requires a QGuiApplication. On Wayland, creating it
    /// loads Qt's shell integration if no window has yet, so blurSupported is
    /// known from the start.
    static BackgroundEffect* instance();

    /// Whether the compositor applies blur requested through this class right
    /// now: the protocol is advertised and its blur capability is reported.
    /// Can change at runtime; blurSupportedChanged() says when.
    [[nodiscard]] bool isBlurSupported() const;

    /// Blur the backdrop behind @p region of @p window, in window-local
    /// logical coordinates (the coordinates a QQuickItem in that window maps
    /// to). An empty region removes the effect. Applied with the window's
    /// next frame: this requests one.
    void setBlurRegion(QWindow* window, const QRegion& region);

    /// The region last set for @p window, whether or not it is applied.
    [[nodiscard]] QRegion blurRegion(const QWindow* window) const;

Q_SIGNALS:
    void blurSupportedChanged(bool supported);

private:
    explicit BackgroundEffect(QObject* parent);
};

} // namespace PhosphorWayland
