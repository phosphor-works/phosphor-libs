// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <PhosphorZoneRendering/ZoneShaderItem.h>

#include <QObject>
#include <QtQml/qqmlregistration.h>

namespace PhosphorZoneRendering {

/// Registers ZoneShaderItem in the org.phosphor.zonerendering QML module
/// without compiling the item a second time into the static module: the class
/// stays in the shared library, and this names it as a foreign type.
struct ZoneShaderItemForeign
{
    Q_GADGET
    QML_FOREIGN(PhosphorZoneRendering::ZoneShaderItem)
    QML_NAMED_ELEMENT(ZoneShaderItem)
};

} // namespace PhosphorZoneRendering
