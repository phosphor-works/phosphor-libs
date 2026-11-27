// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <QString>
#include <QStringList>

namespace PhosphorIpcCli {

// Strip `--socket PATH` / `--socket=PATH` / `-s PATH` from the
// passed-in arg list and return the resolved path (or empty if no
// flag was supplied). Called by both the top-level dispatcher and
// the per-subcommand handlers so `<program> --socket /x call ...`
// AND `<program> call ... --socket /x` both work.
//
// On usage error (`--socket` with no following value, or
// `--socket=` with an empty value), populates `*errorMessage` (if
// non-null) and returns a sentinel "" with `args` left in a
// partially-consumed state — callers detect the error by checking
// errorMessage and bailing out rather than treating the dangling
// `--socket` as a positional argument.
[[nodiscard]] QString stripSocketFlag(QStringList& args, QString* errorMessage = nullptr);

// The name of the running program (Program::name), for messages.
[[nodiscard]] QString programName();

// Sanitise a server-supplied string for safe stdout printing.
// Server-side targets may return / broadcast strings that originated
// from untrusted sources (window titles, app-ids, user input
// reflected back). Writing those raw to a terminal would allow
// ANSI escape injection (cursor moves, OSC title rewrites, screen
// clears). Replaces ASCII control characters (0x00-0x1F) except
// '\t' and '\n' with their `\xNN` two-digit hex escape, preserving
// the visible text payload while neutralising the terminal-attack
// vector. Use this for multi-line text contexts (raw string
// results, broadcast payloads) where newlines are semantically OK.
[[nodiscard]] QString sanitiseForTerminal(const QString& s);

// Strict single-line variant: also escapes '\n', '\r', and '\t' so
// a server-supplied string can't break out of a one-line stderr
// diagnostic. Used in `<program> <sub>: <code>: <message>`-style
// error reporting, where a newline in `message` would visually
// split the error across what looks like two log entries.
[[nodiscard]] QString sanitiseForSingleLine(const QString& s);

// Subcommand entry points. Each takes the post-subcommand argument
// list (after the subcommand name itself), plus the resolved socket
// path. Returns the process exit code:
//   0 - success
//   1 - usage error
//   2 - connection / I/O error
//   3 - server-side error (NO_SUCH_TARGET, etc.)
//   4 - internal setup failure (e.g. signal-handler install)
[[nodiscard]] int runCall(QStringList args, QString socketPath);
[[nodiscard]] int runList(QStringList args, QString socketPath);
[[nodiscard]] int runSchema(QStringList args, QString socketPath);
[[nodiscard]] int runSubscribe(QStringList args, QString socketPath);

} // namespace PhosphorIpcCli
