// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

// PlasmaZones-side configuration backend factories.
//
// Each factory materializes a PhosphorConfig::JsonBackend pointed at the
// relevant PZ config file, with PZ's per-screen path resolver attached and
// the current schema version wired for fresh-install stamping.

#include "phosphorengineruntime_export.h"

#include <PhosphorConfig/IBackend.h>
#include <PhosphorConfig/JsonBackend.h>

#include <memory>

namespace PhosphorEngineRuntime {

/// Primary user config (~/.config/phosphor-works/plasmazones/config.json).
PHOSPHORENGINERUNTIME_EXPORT std::unique_ptr<PhosphorConfig::IBackend> createDefaultConfigBackend();

/// Ephemeral window-tracking session state (~/.config/phosphor-works/plasmazones/session.json).
/// Separate file from config.json to avoid write contention.
PHOSPHORENGINERUNTIME_EXPORT std::unique_ptr<PhosphorConfig::IBackend> createSessionBackend();

} // namespace PhosphorEngineRuntime
