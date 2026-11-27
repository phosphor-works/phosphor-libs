// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: LGPL-2.1-or-later

// A program of an app, as lambentctl is one, for test_phosphor_ipc_cli. Its
// app is "ipc-cli-test", so its socket is $IPC_CLI_TEST_SOCKET.

#include <PhosphorIpcCli/PhosphorIpcCli.h>

int main(int argc, char** argv)
{
    return PhosphorIpcCli::run(PhosphorIpcCli::programForApp(QStringLiteral("ipc-cli-test-ctl"),
                                                             QStringLiteral("ipc-cli-test"), QStringLiteral("1.2.3"),
                                                             QStringLiteral("Controls the test router.")),
                               argc, argv);
}
