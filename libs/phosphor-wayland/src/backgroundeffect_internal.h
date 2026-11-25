// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

namespace PhosphorWayland {

/// Called by LayerShellIntegration on the GUI thread whenever what it knows
/// of ext_background_effect_manager_v1 changes: the global bound or removed,
/// or a capabilities event. Resends every window's blur region and emits
/// BackgroundEffect::blurSupportedChanged() when blur support flipped.
void backgroundEffectCapabilitiesChanged();

} // namespace PhosphorWayland
