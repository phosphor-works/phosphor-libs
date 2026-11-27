// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <PhosphorIpcCli/phosphoripccli_export.h>

#include <QString>
#include <QStringList>

#include <functional>

// The command-line client of a phosphor-ipc socket, as a library. Each app
// has its own small program (lambentctl, phosphorctl) that names the app and
// its socket, and calls run(). The subcommands are the same in each:
//
//   call <target>.<fn> [--arg name=value ...] [--timeout SECONDS]
//   list
//   schema <target>
//   subscribe <target>.<signal>
//
// --socket PATH (or -s PATH), before or after the subcommand, overrides the
// app's socket.
namespace PhosphorIpcCli {

struct Program
{
    // The name of the program, for the usage text and the messages.
    QString name;
    QString version;
    // What the program controls, in one line under the usage line.
    QString description;
    // The socket path when there is no --socket. Empty when none is found:
    // the program then fails with exit code 2.
    std::function<QString()> defaultSocket;
    // How defaultSocket finds the path, for --help.
    QString socketHelp;
};

// A Program for the app `appName`: its socket is
// IpcRouter::socketPathForApp(appName), $<APP>_SOCKET or
// $XDG_RUNTIME_DIR/<app>.sock.
[[nodiscard]] PHOSPHORIPCCLI_EXPORT Program programForApp(const QString& programName, const QString& appName,
                                                          const QString& version, const QString& description);

// Runs the program on the command line: makes the QCoreApplication, runs the
// subcommand and returns the exit code. 0: success. 1: a usage error. 2: a
// connection or I/O error. 3: an error from the server. 4: a setup failure.
[[nodiscard]] PHOSPHORIPCCLI_EXPORT int run(const Program& program, int argc, char** argv);

// Whether `command` is one of the generic subcommands: call, list, schema or
// subscribe.
[[nodiscard]] PHOSPHORIPCCLI_EXPORT bool isCommand(const QString& command);

// Runs one generic subcommand in a program that parses its own command line
// and has its QCoreApplication already, such as `afterglow ctl`. `args` are
// the words after the command. A --socket among them overrides `socketPath`.
// The messages start with `programName`, for example "afterglow ctl". The
// exit codes are those of run(). 1 for a command that isCommand() refuses.
[[nodiscard]] PHOSPHORIPCCLI_EXPORT int runCommand(const QString& programName, const QString& command,
                                                   const QStringList& args, const QString& socketPath);

} // namespace PhosphorIpcCli
