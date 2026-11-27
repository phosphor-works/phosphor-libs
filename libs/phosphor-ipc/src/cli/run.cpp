// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorIpcCli/PhosphorIpcCli.h>

#include "subcommands.h"

#include <PhosphorIpc/IpcRouter.h>

#include <QCoreApplication>
#include <QLatin1String>
#include <QStringList>
#include <QTextStream>

namespace PhosphorIpcCli {

namespace {

void printUsage(const Program& program, QTextStream& out)
{
    out << "usage: " << program.name << " [--socket PATH] <subcommand> [args]\n";
    if (!program.description.isEmpty()) {
        out << program.description << "\n";
    }
    out << "\nSubcommands:\n"
        << "  call <target>.<fn> [--arg name=value ...] [--timeout SECONDS]\n"
        << "      Invoke a function on a target and print its return value. Waits\n"
        << "      2 seconds for the answer by default, and without a limit for\n"
        << "      --timeout 0. Events the target sends to the call go to stderr.\n"
        << "  list\n"
        << "      Print every registered target id, one per line.\n"
        << "  schema <target>\n"
        << "      Pretty-print the target's JSON Schema.\n"
        << "  subscribe <target>.<signal>\n"
        << "      Stream JSON events for a signal until Ctrl+C.\n\n"
        << "Options:\n"
        << "  --socket PATH, -s PATH  Talk to the socket at PATH.\n";
    if (!program.socketHelp.isEmpty()) {
        out << "\n" << program.socketHelp << "\n";
    }
}

} // namespace

Program programForApp(const QString& programName, const QString& appName, const QString& version,
                      const QString& description)
{
    Program program;
    program.name = programName;
    program.version = version;
    program.description = description;
    program.defaultSocket = [appName] {
        return PhosphorIpc::IpcRouter::socketPathForApp(appName);
    };
    program.socketHelp = QStringLiteral("Without --socket, the socket is $%1, else $XDG_RUNTIME_DIR/%2.sock.")
                             .arg(PhosphorIpc::IpcRouter::socketEnvironmentVariable(appName), appName);
    return program;
}

int run(const Program& program, int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(program.name);
    QCoreApplication::setApplicationVersion(program.version);

    QTextStream out(stdout);
    QTextStream err(stderr);

    // We dispatch subcommands manually (QCommandLineParser treats
    // positional args weakly). stripSocketFlag pulls --socket / -s out of
    // the arg list wherever it appears, before OR after the subcommand, so
    // users don't have to remember the order.
    QStringList args = QCoreApplication::arguments();
    args.removeFirst();
    QString socketErr;
    QString socketPathCli = stripSocketFlag(args, &socketErr);
    if (!socketErr.isEmpty()) {
        err << programName() << ": " << socketErr << "\n";
        return 1;
    }

    if (args.isEmpty() || args.front() == QLatin1String("--help") || args.front() == QLatin1String("-h")
        || args.front() == QLatin1String("help")) {
        printUsage(program, args.isEmpty() ? err : out);
        return args.isEmpty() ? 1 : 0;
    }
    if (args.front() == QLatin1String("--version") || args.front() == QLatin1String("-v")) {
        out << program.name << " " << program.version << "\n";
        return 0;
    }

    const QString subcommand = args.takeFirst();
    // The flag may also follow the subcommand:
    // `<program> call foo.bar --socket /x`.
    socketErr.clear();
    const QString postSubcommandSocket = stripSocketFlag(args, &socketErr);
    if (!socketErr.isEmpty()) {
        err << programName() << ": " << socketErr << "\n";
        return 1;
    }
    if (!postSubcommandSocket.isEmpty()) {
        socketPathCli = postSubcommandSocket;
    }
    const QString socketPath =
        !socketPathCli.isEmpty() ? socketPathCli : (program.defaultSocket ? program.defaultSocket() : QString());

    if (subcommand == QLatin1String("call")) {
        return runCall(args, socketPath);
    }
    if (subcommand == QLatin1String("list")) {
        return runList(args, socketPath);
    }
    if (subcommand == QLatin1String("schema")) {
        return runSchema(args, socketPath);
    }
    if (subcommand == QLatin1String("subscribe")) {
        return runSubscribe(args, socketPath);
    }

    err << programName() << ": unknown subcommand '" << sanitiseForSingleLine(subcommand) << "'\n";
    printUsage(program, err);
    return 1;
}

} // namespace PhosphorIpcCli
