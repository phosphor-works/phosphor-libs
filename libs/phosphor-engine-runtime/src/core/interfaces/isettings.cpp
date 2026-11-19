// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorEngineRuntime/ISettings.h>

namespace PhosphorEngineRuntime {

// Out of line, so ISettings' vtable and type info are emitted once, in this
// library, rather than in every binary that includes the header.
ISettings::~ISettings() = default;

} // namespace PhosphorEngineRuntime
