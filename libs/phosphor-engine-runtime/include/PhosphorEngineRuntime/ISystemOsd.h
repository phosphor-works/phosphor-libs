// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

class QString;

namespace PhosphorEngineRuntime {

/**
 * @brief The desktop's own text OSD: a short notice with an icon, drawn by
 *        whatever shows the desktop's other OSDs
 *
 * plasmazonesd shows it through plasmashell (PlasmaSystemOsd). A host with no
 * such service draws it itself.
 */
class ISystemOsd
{
public:
    virtual ~ISystemOsd() = default;

    /// Show @p text with the icon named @p iconName, a theme icon the runtime
    /// picks for what the notice says (dialog-cancel, object-locked). An empty
    /// @p iconName asks for the host's own icon: the runtime does not know the
    /// host's name, so a notice about the placement mode itself leaves the
    /// choice to the host.
    virtual void showText(const QString& iconName, const QString& text) = 0;
};

} // namespace PhosphorEngineRuntime
