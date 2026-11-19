// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// The engine runtime's own translations: its C++ strings go through
// RuntimeI18n::tr() under the "PhosphorEngineRuntime" context, extracted into
// the phosphor-engine-runtime catalog rather than an application's. A host
// loads that catalog with loadRuntimeTranslations().

#pragma once

#include "phosphorengineruntime_export.h"

#include <QCoreApplication>
#include <QString>

class RuntimeI18n
{
    Q_DECLARE_TR_FUNCTIONS(PhosphorEngineRuntime)
};

namespace PhosphorEngineRuntime {

/// Install the runtime's catalog for the current locale on @p app, from the
/// installed share/phosphor-works/translations or the build tree.
/// Call after the QCoreApplication exists.
PHOSPHORENGINERUNTIME_EXPORT void loadRuntimeTranslations(QCoreApplication* app);

} // namespace PhosphorEngineRuntime
