// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

// Umbrella header — pulls every PZ service interface in one include.
//
// The four interfaces were split out into per-contract headers so
// zones-related code can depend on just PhosphorZones::IZoneDetector / PhosphorZones::LayoutRegistry
// without pulling ISettings / IOverlayService along. This header exists
// to keep the ~45 legacy `#include "core/interfaces.h"` sites compiling
// without touching every consumer; new code should prefer the narrower
// header matching the interface it actually uses.

#include <PhosphorZones/LayoutRegistry.h>
#include <PhosphorEngineRuntime/IOverlayService.h>
#include <PhosphorEngineRuntime/ISettings.h>
#include <PhosphorZones/IZoneDetector.h>
