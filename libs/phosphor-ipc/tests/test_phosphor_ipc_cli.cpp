// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: LGPL-2.1-or-later

// PhosphorIpcCli against a real router: the fixture program
// (phosphor_ipc_cli_fixture.cpp) runs as a child process, and the router
// of this process answers it. The test waits with the event loop running,
// so that the router can answer.

#include "ipctesthelpers.h"

#include <PhosphorIpc/IpcRouter.h>
#include <PhosphorIpcCli/PhosphorIpcCli.h>

#include <QProcess>
#include <QProcessEnvironment>
#include <QSignalSpy>
#include <QTest>

using namespace PhosphorIpc;
using PhosphorIpcTests::RouterFixture;

namespace {

class Greeter : public QObject
{
    Q_OBJECT
public:
    Q_INVOKABLE QString greet(const QString& name)
    {
        return QStringLiteral("hello %1").arg(name);
    }
};

struct Result
{
    int exitCode = -1;
    QString out;
    QString err;
};

// Runs the fixture with `args`, with IPC_CLI_TEST_SOCKET set to `socket`
// (unset when empty) and XDG_RUNTIME_DIR set to `runtime`.
Result runCli(const QStringList& args, const QString& socket, const QString& runtime)
{
    QProcess process;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.remove(QStringLiteral("IPC_CLI_TEST_SOCKET"));
    if (!socket.isEmpty()) {
        env.insert(QStringLiteral("IPC_CLI_TEST_SOCKET"), socket);
    }
    env.insert(QStringLiteral("XDG_RUNTIME_DIR"), runtime);
    process.setProcessEnvironment(env);
    QSignalSpy finished(&process, &QProcess::finished);
    process.start(QStringLiteral(PHOSPHOR_IPC_CLI_FIXTURE), args);
    if (!process.waitForStarted(5000) || !finished.wait(10000)) {
        process.kill();
        process.waitForFinished();
        return {};
    }
    return {process.exitCode(), QString::fromUtf8(process.readAllStandardOutput()),
            QString::fromUtf8(process.readAllStandardError())};
}

} // namespace

class TestPhosphorIpcCli : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void callAndList_reachTheAppSocket()
    {
        RouterFixture fixture;
        QVERIFY(fixture.valid());
        Greeter greeter;
        QVERIFY(fixture.router.registerTarget(QStringLiteral("greeter"), &greeter));
        QVERIFY(fixture.router.start(fixture.sockPath));

        const Result call = runCli({QStringLiteral("call"), QStringLiteral("greeter.greet"), QStringLiteral("--arg"),
                                    QStringLiteral("name=ipc")},
                                   fixture.sockPath, fixture.dir.path());
        QCOMPARE(call.exitCode, 0);
        QVERIFY2(call.out.contains(QStringLiteral("hello ipc")), qPrintable(call.out + call.err));

        const Result list = runCli({QStringLiteral("list")}, fixture.sockPath, fixture.dir.path());
        QCOMPARE(list.exitCode, 0);
        QCOMPARE(list.out.trimmed(), QStringLiteral("greeter"));
    }

    void defaultSocket_isInTheRuntimeDirectory()
    {
        // Without the variable, the program finds <app>.sock there.
        RouterFixture fixture;
        QVERIFY(fixture.valid());
        Greeter greeter;
        QVERIFY(fixture.router.registerTarget(QStringLiteral("greeter"), &greeter));
        QVERIFY(fixture.router.start(QDir(fixture.dir.path()).filePath(QStringLiteral("ipc-cli-test.sock"))));

        const Result list = runCli({QStringLiteral("list")}, QString(), fixture.dir.path());
        QCOMPARE(list.exitCode, 0);
        QCOMPARE(list.out.trimmed(), QStringLiteral("greeter"));
    }

    void socketOption_overridesTheApp()
    {
        RouterFixture fixture;
        QVERIFY(fixture.valid());
        Greeter greeter;
        QVERIFY(fixture.router.registerTarget(QStringLiteral("greeter"), &greeter));
        QVERIFY(fixture.router.start(fixture.sockPath));

        const QString elsewhere = QDir(fixture.dir.path()).filePath(QStringLiteral("none.sock"));
        // After the subcommand, too.
        const Result list = runCli({QStringLiteral("list"), QStringLiteral("--socket"), fixture.sockPath}, elsewhere,
                                   fixture.dir.path());
        QCOMPARE(list.exitCode, 0);
        QCOMPARE(list.out.trimmed(), QStringLiteral("greeter"));
    }

    void errors_haveTheirExitCodes()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString none = QDir(dir.path()).filePath(QStringLiteral("none.sock"));

        const Result noServer = runCli({QStringLiteral("list")}, none, dir.path());
        QCOMPARE(noServer.exitCode, 2);
        QVERIFY2(noServer.err.startsWith(QStringLiteral("ipc-cli-test-ctl list: ")), qPrintable(noServer.err));

        const Result usage = runCli({}, none, dir.path());
        QCOMPARE(usage.exitCode, 1);
        QVERIFY(usage.err.startsWith(QStringLiteral("usage: ipc-cli-test-ctl [--socket PATH]")));
        QVERIFY(usage.err.contains(QStringLiteral("Controls the test router.")));
        QVERIFY(usage.err.contains(QStringLiteral("$IPC_CLI_TEST_SOCKET")));

        const Result version = runCli({QStringLiteral("--version")}, none, dir.path());
        QCOMPARE(version.exitCode, 0);
        QCOMPARE(version.out.trimmed(), QStringLiteral("ipc-cli-test-ctl 1.2.3"));

        const Result unknown = runCli({QStringLiteral("frobnicate")}, none, dir.path());
        QCOMPARE(unknown.exitCode, 1);
        QVERIFY(unknown.err.startsWith(QStringLiteral("ipc-cli-test-ctl: unknown subcommand 'frobnicate'")));
    }

    void runCommand_worksInAnAppThatParsesItsOwnLine()
    {
        // As afterglow ctl uses it: in this process, with its own
        // QCoreApplication. The client waits in a local event loop, in which
        // the router of this process answers.
        QVERIFY(PhosphorIpcCli::isCommand(QStringLiteral("call")));
        QVERIFY(PhosphorIpcCli::isCommand(QStringLiteral("subscribe")));
        QVERIFY(!PhosphorIpcCli::isCommand(QStringLiteral("ls")));

        RouterFixture fixture;
        QVERIFY(fixture.valid());
        Greeter greeter;
        QVERIFY(fixture.router.registerTarget(QStringLiteral("greeter"), &greeter));
        QVERIFY(fixture.router.start(fixture.sockPath));

        QCOMPARE(PhosphorIpcCli::runCommand(
                     QStringLiteral("host ctl"), QStringLiteral("call"),
                     {QStringLiteral("greeter.greet"), QStringLiteral("--arg"), QStringLiteral("name=x")},
                     fixture.sockPath),
                 0);
        // --socket in the words overrides the path that the program found.
        QCOMPARE(PhosphorIpcCli::runCommand(QStringLiteral("host ctl"), QStringLiteral("list"),
                                            {QStringLiteral("--socket"), fixture.sockPath},
                                            QDir(fixture.dir.path()).filePath(QStringLiteral("none.sock"))),
                 0);
        QCOMPARE(PhosphorIpcCli::runCommand(QStringLiteral("host ctl"), QStringLiteral("list"), {},
                                            QDir(fixture.dir.path()).filePath(QStringLiteral("none.sock"))),
                 2);
        QCOMPARE(PhosphorIpcCli::runCommand(QStringLiteral("host ctl"), QStringLiteral("ls"), {}, fixture.sockPath), 1);
    }
};

QTEST_GUILESS_MAIN(TestPhosphorIpcCli)
#include "test_phosphor_ipc_cli.moc"
