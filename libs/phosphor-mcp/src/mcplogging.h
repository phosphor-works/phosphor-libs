// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <QLoggingCategory>

namespace PhosphorMcp {
// Diagnostics only ever go here, and so to stderr: stdout carries the
// protocol and nothing else.
Q_DECLARE_LOGGING_CATEGORY(lcMcp)
} // namespace PhosphorMcp
