// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorEngineRuntime/Logging.h>

namespace PhosphorEngineRuntime {

// Core: layouts, screens and the shared helpers
Q_LOGGING_CATEGORY(lcCore, "phosphor.engineruntime.core", QtInfoMsg)
Q_LOGGING_CATEGORY(lcScreen, "phosphor.engineruntime.core.screen", QtInfoMsg)

// The runtime: its lifecycle, overlays and shortcuts
Q_LOGGING_CATEGORY(lcDaemon, "phosphor.engineruntime.daemon", QtInfoMsg)
Q_LOGGING_CATEGORY(lcOverlay, "phosphor.engineruntime.daemon.overlay", QtInfoMsg)
Q_LOGGING_CATEGORY(lcShortcuts, "phosphor.engineruntime.daemon.shortcuts", QtInfoMsg)

// The runtime's services, by the interface each serves
Q_LOGGING_CATEGORY(lcDbus, "phosphor.engineruntime.dbus", QtInfoMsg)
Q_LOGGING_CATEGORY(lcDbusLayout, "phosphor.engineruntime.dbus.layout", QtInfoMsg)
Q_LOGGING_CATEGORY(lcDbusWindow, "phosphor.engineruntime.dbus.window", QtInfoMsg)
Q_LOGGING_CATEGORY(lcDbusSettings, "phosphor.engineruntime.dbus.settings", QtInfoMsg)
Q_LOGGING_CATEGORY(lcDbusRules, "phosphor.engineruntime.dbus.rules", QtInfoMsg)
Q_LOGGING_CATEGORY(lcDbusAutotile, "phosphor.engineruntime.dbus.autotile", QtInfoMsg)
Q_LOGGING_CATEGORY(lcDbusTiling, "phosphor.engineruntime.dbus.tiling", QtInfoMsg)
Q_LOGGING_CATEGORY(lcDbusScrolling, "phosphor.engineruntime.dbus.scrolling", QtInfoMsg)

} // namespace PhosphorEngineRuntime
