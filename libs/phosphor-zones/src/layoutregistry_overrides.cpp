// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Per-algorithm autotile layout overrides.
// Autotile entries are NOT separate from manual layouts: they live in the same
// layout-settings.json sidecar (LayoutSettingsStore), keyed by the canonical
// "autotile:<id>" form so manual and autotile per-id settings share one store.
// This file owns the autotile-facing accessors and the fresh-install seed.

#include <PhosphorZones/LayoutRegistry.h>
#include <PhosphorZones/LayoutSettingsStore.h>

#include <PhosphorLayoutApi/LayoutId.h>

#include "zoneslogging.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace PhosphorZones {

QJsonObject LayoutRegistry::loadAutotileOverrides(const QString& algorithmId) const
{
    return m_layoutSettings.settingsFor(PhosphorLayout::LayoutId::makeAutotileId(algorithmId));
}

void LayoutRegistry::saveAutotileOverrides(const QString& algorithmId, const QJsonObject& overrides)
{
    // setSettingsFor clears the entry when overrides is empty, so a fully-reset
    // algorithm collapses back to its defaults rather than persisting an empty
    // object.
    m_layoutSettings.setSettingsFor(PhosphorLayout::LayoutId::makeAutotileId(algorithmId), overrides);
    if (!m_layoutSettings.saveToFile(layoutSettingsFilePath())) {
        qCWarning(lcZonesLib) << "Failed to persist autotile overrides for" << algorithmId;
    }
}

void LayoutRegistry::seedDefaultLayoutSettingsIfFresh(const QJsonObject& defaults)
{
    if (defaults.isEmpty()) {
        return;
    }
    // "Fresh" = the sidecar does not exist yet. A user with one is an existing
    // install and keeps every layout/algorithm visible — the curated default
    // seeds new configs only.
    const QString settingsPath = layoutSettingsFilePath();
    if (QFile::exists(settingsPath)) {
        return;
    }
    for (auto it = defaults.constBegin(); it != defaults.constEnd(); ++it) {
        if (it.value().isObject()) {
            m_layoutSettings.setSettingsFor(it.key(), it.value().toObject());
        }
    }
    // Seeding runs at daemon init, before anything else has created the config
    // directory — ensure it exists so the QSaveFile write doesn't silently fail.
    // Persistence is disk-mediated: the caller's loadLayouts() reloads the
    // sidecar from disk immediately after (clearing the in-memory map), so the
    // on-disk copy written here is authoritative. A failed write degrades
    // benignly to "everything visible" (logged below), not a crash.
    QDir().mkpath(QFileInfo(settingsPath).absolutePath());
    if (!m_layoutSettings.saveToFile(settingsPath)) {
        qCWarning(lcZonesLib) << "Failed to seed default layout-settings.json";
    }
}

} // namespace PhosphorZones
