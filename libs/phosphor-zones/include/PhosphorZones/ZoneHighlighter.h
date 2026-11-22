// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include "phosphorzones_export.h"
#include <QObject>
#include <QVector>

namespace PhosphorZones {

class Zone;

// The meta-object of the class below has Zone* in its signatures. moc's
// metatype for that pointer asks whether Zone is a QObject, which needs
// the definition: without it the answer is "not complete", and a later
// definition in the same translation unit contradicts it.
Q_MOC_INCLUDE("PhosphorZones/Zone.h")

/**
 * @brief Manages zone highlighting state
 *
 * Separates UI state from detection logic. ZoneDetector runs the detection
 * algorithms; this class manages the visual highlighting.
 */
class PHOSPHORZONES_EXPORT ZoneHighlighter : public QObject
{
    Q_OBJECT

public:
    explicit ZoneHighlighter(QObject* parent = nullptr);
    ~ZoneHighlighter() override = default;

    /**
     * @brief Highlight a single zone
     * @param zone Zone to highlight
     */
    Q_INVOKABLE void highlightZone(Zone* zone);

    /**
     * @brief Highlight multiple zones
     * @param zones Zones to highlight
     */
    Q_INVOKABLE void highlightZones(const QVector<Zone*>& zones);

    /**
     * @brief Clear all highlights
     */
    Q_INVOKABLE void clearHighlights();

Q_SIGNALS:
    void zoneHighlighted(Zone* zone);
    void zonesHighlighted(const QVector<Zone*>& zones);
    void highlightsCleared();

private:
    QVector<Zone*> m_highlightedZones;
};

} // namespace PhosphorZones
