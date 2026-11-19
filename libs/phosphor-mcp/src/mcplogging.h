// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <QLoggingCategory>

namespace PhosphorMcp {
// Diagnostics only ever go here, never to stdout, which carries the protocol
// and nothing else. Qt writes them to stderr when the process has a terminal
// or QT_FORCE_STDERR_LOGGING=1 is set, and otherwise to the systemd journal
// when Qt is built with it.
Q_DECLARE_LOGGING_CATEGORY(lcMcp)
} // namespace PhosphorMcp
