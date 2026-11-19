// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorEngineRuntime/IOverlayService.h>

namespace PhosphorEngineRuntime {

// The out-of-line virtual destructor anchors IOverlayService's vtable in this
// translation unit (ISettings' is the engine runtime's, in phosphor-libs). Prevents ODR violations across
// shared-library boundaries.
//
// PhosphorZones::LayoutRegistry + PhosphorZones::IZoneDetector destructors live in
// libs/phosphor-zones/src/interfaces.cpp — those interfaces moved with
// the zones extraction.

IOverlayService::~IOverlayService() = default;

} // namespace PhosphorEngineRuntime
