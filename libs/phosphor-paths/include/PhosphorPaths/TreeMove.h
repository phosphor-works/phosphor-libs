// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <PhosphorPaths/phosphorpaths_export.h>

#include <QString>

namespace PhosphorPaths {

/// Move every file under @p from into the same place under @p to, then remove
/// what is left empty of @p from. A file that already exists under @p to stays
/// as it is, and so does its source. A rename where the two share a
/// filesystem, a copy and delete otherwise. Nothing happens when @p from is
/// not a directory.
///
/// For an app's one-time move of a tree it kept somewhere else before into its
/// place under `phosphor-works`: run it once, when the new place has nothing
/// yet, and never read the old place again.
PHOSPHORPATHS_EXPORT void moveTreeContents(const QString& from, const QString& to);

} // namespace PhosphorPaths
