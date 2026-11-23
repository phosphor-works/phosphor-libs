// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <QRectF>

namespace PhosphorLayout {

/// @p fixedGeometry (a zone's fixedGeometry from a layout file) in logical
/// units: every component divided by @p divisor, a non-positive one reading as
/// 1.0. The one place the division is written, shared by
/// PhosphorZones::Zone::fixedGeometryToLogical (the absolute, normalized,
/// bounding-box and gap-edge readers) and the shell's placement-map parser
/// (PhosphorShell::PlacementMapParser::parseSnappingLayout), so none of them
/// can resolve the unit differently.
inline QRectF fixedGeometryToLogical(const QRectF& fixedGeometry, qreal divisor)
{
    // A non-positive divisor is no unit at all and reads as logical.
    const qreal d = divisor > 0.0 ? divisor : 1.0;
    return QRectF(fixedGeometry.x() / d, fixedGeometry.y() / d, fixedGeometry.width() / d, fixedGeometry.height() / d);
}

} // namespace PhosphorLayout
