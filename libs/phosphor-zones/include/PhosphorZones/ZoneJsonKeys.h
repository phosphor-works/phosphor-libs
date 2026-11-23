// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <PhosphorLayoutApi/ZoneJsonKeys.h>

namespace PhosphorZones {

/**
 * @brief JSON wire-format keys for zone & layout serialisation.
 *
 * Defined in phosphor-layout-api (PhosphorLayout::ZoneJsonKeys), so a reader
 * of layout files needs no placement engine. This alias keeps every
 * PhosphorZones::ZoneJsonKeys spelling working.
 */
namespace ZoneJsonKeys = PhosphorLayout::ZoneJsonKeys;

} // namespace PhosphorZones
