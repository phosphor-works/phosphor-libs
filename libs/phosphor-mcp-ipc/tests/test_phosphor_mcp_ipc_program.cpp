// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: LGPL-2.1-or-later

// phosphor-mcp-ipc run as an MCP client runs it: a child with pipes for
// stdin and stdout, in a session of its own. The application's router runs
// in the test, so the test runs its event loop while it waits for the child.

#include "mcpipctesthelpers.h"
#include "mcptesthelpers.h"

#include <PhosphorMcp/McpProtocol.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QProcess>
#include <QScopeGuard>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>

#include <cerrno>

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

using namespace PhosphorMcpIpcTests;
using namespace PhosphorMcpTests;

namespace {

class Program : public ChildServer
{
public:
    Program()
        : ChildServer(QStringLiteral(PHOSPHOR_MCP_IPC_PATH), QStringLiteral("phosphor-mcp-ipc"))
    {
    }

    [[nodiscard]] bool start(const QString& socketPath, const QString& manifestPath)
    {
        return ChildServer::start({QStringLiteral("--socket"), socketPath, QStringLiteral("--manifest"), manifestPath});
    }
    using ChildServer::start;
};

bool writeFile(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

} // namespace

class TestPhosphorMcpIpcProgram : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void servesTheManifest()
    {
        Application application;
        QVERIFY(application.start());
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString manifest = QDir(dir.path()).filePath(QStringLiteral("mcp.json"));
        QVERIFY(writeFile(manifest, manifestJson()));

        Program program;
        QVERIFY(program.start(application.socketPath(), manifest));
        QVERIFY(program.write(request(1, QStringLiteral("server/discover"))));
        QVERIFY(program.write(request(2, QStringLiteral("tools/list"))));
        QVERIFY(program.write(callRequest(
            3, QStringLiteral("rename_pane"),
            QJsonObject{{QLatin1StringView("title"), QStringLiteral("logs")}, {QLatin1StringView("pane"), 2}})));
        QVERIFY(
            program.write(callRequest(4, QStringLiteral("close_pane"), QJsonObject{{QLatin1StringView("pane"), 5}})));
        const QList<QJsonObject> replies = program.readReplies(4);
        QCOMPARE(replies.size(), 4);

        // The name and the version that the server gives a client, on
        // every reply.
        const QJsonObject info{{QLatin1StringView("name"), QStringLiteral("phosphor-mcp-ipc")},
                               {QLatin1StringView("version"), QStringLiteral(PHOSPHORMCPIPC_VERSION)}};
        for (const QJsonObject& reply : replies) {
            QCOMPARE(serverInfo(reply), info);
        }
        QCOMPARE(resultOf(replies.at(1)).value(QLatin1StringView("tools")).toArray().size(), 8);
        QCOMPARE(firstText(replies.at(2)), QStringLiteral("pane 2 is now logs"));
        QVERIFY(resultOf(replies.at(3)).value(QLatin1StringView("isError")).toBool());
        QCOMPARE(firstText(replies.at(3)), QStringLiteral("CALL_FAILED: no pane 5"));

        // What it left out, on stderr, whatever the logging rules say.
        const QByteArray log = program.standardError();
        QVERIFY2(log.contains("phosphor-mcp-ipc: the socket has no target \"ghost\""), log.constData());
        QVERIFY2(log.contains("phosphor-mcp-ipc: tool \"missing\": the target \"panes\" has no method \"missing\""),
                 log.constData());

        // The end of stdin ends it, with 0.
        program.process().closeWriteChannel();
        QVERIFY(program.waitForExit());
        QCOMPARE(program.process().exitStatus(), QProcess::NormalExit);
        QCOMPARE(program.process().exitCode(), 0);
    }

    void noManifestGivesNoTools_data()
    {
        QTest::addColumn<QByteArray>("manifest");
        QTest::addColumn<QByteArray>("line");

        QTest::newRow("no file") << QByteArray() << QByteArray("no manifest at ");
        QTest::newRow("an empty list") << QByteArray(R"({"tools": []})") << QByteArray(" lists no tools");
    }

    void noManifestGivesNoTools()
    {
        QFETCH(QByteArray, manifest);
        QFETCH(QByteArray, line);

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString manifestPath = QDir(dir.path()).filePath(QStringLiteral("mcp.json"));
        if (!manifest.isEmpty()) {
            QVERIFY(writeFile(manifestPath, manifest));
        }
        Program program;
        // The socket does not exist: with no tools nothing connects.
        QVERIFY(program.start(QDir(dir.path()).filePath(QStringLiteral("none.sock")), manifestPath));
        QVERIFY(program.write(request(1, QStringLiteral("tools/list"))));
        const QList<QJsonObject> replies = program.readReplies(1);
        QCOMPARE(replies.size(), 1);
        // The member itself: a reply without it would convert to an empty
        // array too.
        QCOMPARE(resultOf(replies.at(0)).value(QLatin1StringView("tools")), QJsonValue(QJsonArray()));
        const QByteArray log = program.standardError();
        QVERIFY2(log.contains(line), log.constData());
        program.process().closeWriteChannel();
        QVERIFY(program.waitForExit());
        QCOMPARE(program.process().exitStatus(), QProcess::NormalExit);
        QCOMPARE(program.process().exitCode(), 0);
    }

    void overloadWithoutParamsFailsAtStart()
    {
        Application application;
        QVERIFY(application.start());
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString manifest = QDir(dir.path()).filePath(QStringLiteral("mcp.json"));
        QVERIFY(writeFile(
            manifest,
            QByteArrayLiteral(R"({"tools": [{"call": "panes.split", "name": "split", "description": "d"}]})")));

        Program program;
        QVERIFY(program.start(application.socketPath(), manifest));
        QVERIFY(program.waitForExit());
        QCOMPARE(program.process().exitStatus(), QProcess::NormalExit);
        QCOMPARE(program.process().exitCode(), 1);
        const QByteArray log = program.standardError();
        QVERIFY2(log.contains("panes.split has 2 overloads"), log.constData());
        QVERIFY(program.process().readAllStandardOutput().isEmpty());
    }

    void badManifestFailsAtStart()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString manifest = QDir(dir.path()).filePath(QStringLiteral("mcp.json"));
        QVERIFY(writeFile(manifest, QByteArrayLiteral(R"({"tools": [{"call": "panes.split"}]})")));
        Program program;
        QVERIFY(program.start(QDir(dir.path()).filePath(QStringLiteral("none.sock")), manifest));
        QVERIFY(program.waitForExit());
        QCOMPARE(program.process().exitStatus(), QProcess::NormalExit);
        QCOMPARE(program.process().exitCode(), 1);
        const QByteArray log = program.standardError();
        QVERIFY2(log.contains("mcp.json: tools[0]: there is no \"name\""), log.constData());
    }

    void unreachableSocketFailsAtStart()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString manifest = QDir(dir.path()).filePath(QStringLiteral("mcp.json"));
        QVERIFY(writeFile(manifest, manifestJson()));
        Program program;
        QVERIFY(program.start(QDir(dir.path()).filePath(QStringLiteral("none.sock")), manifest));
        QVERIFY(program.waitForExit());
        QCOMPARE(program.process().exitStatus(), QProcess::NormalExit);
        QCOMPARE(program.process().exitCode(), 1);
        const QByteArray log = program.standardError();
        QVERIFY2(log.contains("phosphor-mcp-ipc: cannot connect to "), log.constData());
    }

    void wrongCommandLine_data()
    {
        QTest::addColumn<QStringList>("arguments");
        QTest::addColumn<QByteArray>("message");

        const QString socket = QStringLiteral("--socket");
        const QString manifest = QStringLiteral("--manifest");
        const QString path = QStringLiteral("/nonexistent/app.sock");
        const QString file = QStringLiteral("/nonexistent/mcp.json");
        QTest::newRow("nothing") << QStringList() << QByteArray("--socket is missing");
        QTest::newRow("no manifest") << QStringList{socket, path} << QByteArray("--manifest is missing");
        QTest::newRow("no socket") << QStringList{manifest, file} << QByteArray("--socket is missing");
        QTest::newRow("an argument without an option")
            << QStringList{socket, path, manifest, file, QStringLiteral("extra")}
            << QByteArray("unexpected argument \"extra\"");
        QTest::newRow("an option that the program does not have")
            << QStringList{socket, path, manifest, file, QStringLiteral("--verbose")} << QByteArray("verbose");
        QTest::newRow("an option two times") << QStringList{socket, path, socket, path, manifest, file}
                                             << QByteArray("--socket is given more than one time");
        // QLocalSocket would take this name as a file in the temporary
        // directory, where another user can listen.
        QTest::newRow("a socket path that is not absolute")
            << QStringList{socket, QStringLiteral("app.sock"), manifest, file}
            << QByteArray("--socket needs an absolute path, and got \"app.sock\"");
        QTest::newRow("an empty manifest path")
            << QStringList{socket, path, manifest, QString()} << QByteArray("--manifest needs a path");
    }

    void wrongCommandLine()
    {
        QFETCH(QStringList, arguments);
        QFETCH(QByteArray, message);

        Program program;
        QVERIFY(program.start(arguments));
        QVERIFY(program.waitForExit());
        QCOMPARE(program.process().exitStatus(), QProcess::NormalExit);
        // 2, where 1 is a server that cannot run.
        QCOMPARE(program.process().exitCode(), 2);
        const QByteArray log = program.standardError();
        QVERIFY2(log.contains(message), log.constData());
        QVERIFY2(log.contains("usage: phosphor-mcp-ipc --socket PATH --manifest FILE"), log.constData());
        QVERIFY(program.process().readAllStandardOutput().isEmpty());
    }

    void helpAndVersion()
    {
        Program version;
        QVERIFY(version.start({QStringLiteral("--version")}));
        QVERIFY(version.waitForExit());
        QCOMPARE(version.process().exitStatus(), QProcess::NormalExit);
        QCOMPARE(version.process().exitCode(), 0);
        QCOMPARE(version.process().readAllStandardOutput(),
                 QByteArray("phosphor-mcp-ipc " PHOSPHORMCPIPC_VERSION "\n"));

        Program help;
        QVERIFY(help.start({QStringLiteral("--help")}));
        QVERIFY(help.waitForExit());
        QCOMPARE(help.process().exitStatus(), QProcess::NormalExit);
        QCOMPARE(help.process().exitCode(), 0);
        // --help-all, which Qt adds with --help, is the same help.
        Program helpAll;
        QVERIFY(helpAll.start({QStringLiteral("--help-all")}));
        QVERIFY(helpAll.waitForExit());
        QCOMPARE(helpAll.process().exitStatus(), QProcess::NormalExit);
        QCOMPARE(helpAll.process().exitCode(), 0);
        QVERIFY(helpAll.process().readAllStandardOutput().contains("--socket <path>"));

        const QByteArray text = help.process().readAllStandardOutput();
        QVERIFY2(text.contains("--socket <path>"), text.constData());
        QVERIFY2(text.contains("--manifest <file>"), text.constData());
        QVERIFY2(text.contains("(required)"), text.constData());
    }

    void signalEndsTheServer()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Program program;
        QVERIFY(program.start(QDir(dir.path()).filePath(QStringLiteral("none.sock")),
                              QDir(dir.path()).filePath(QStringLiteral("none.json"))));
        QVERIFY(program.write(request(1, QStringLiteral("server/discover"))));
        QCOMPARE(program.readReplies(1).size(), 1);
        program.process().terminate();
        QVERIFY(program.waitForExit());
        // The server handles the signal and ends by itself, with 0.
        QCOMPARE(program.process().exitStatus(), QProcess::NormalExit);
        QCOMPARE(program.process().exitCode(), 0);
    }

    void failedOutputEndsTheServerWithAnError()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Program program;
        // A device that refuses every byte.
        program.process().setStandardOutputFile(QStringLiteral("/dev/full"));
        QVERIFY(program.start(QDir(dir.path()).filePath(QStringLiteral("none.sock")),
                              QDir(dir.path()).filePath(QStringLiteral("none.json"))));
        QVERIFY(program.write(request(1, QStringLiteral("server/discover"))));
        QVERIFY(program.waitForExit());
        QCOMPARE(program.process().exitStatus(), QProcess::NormalExit);
        QCOMPARE(program.process().exitCode(), 1);
        // The cause: a server that cannot start ends with 1 too.
        const QByteArray log = program.standardError();
        QVERIFY2(log.contains("writing the output failed"), log.constData());
    }

    void failedInputEndsTheServerWithAnError()
    {
        // The master of a pseudo-terminal whose other end is closed. Each
        // read of it fails with EIO.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        int master = ::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
        QVERIFY(master >= 0);
        // Also when a check below fails.
        const auto closeMaster = qScopeGuard([&master] {
            if (master >= 0) {
                ::close(master);
            }
        });
        char name[128];
        QVERIFY(::grantpt(master) == 0 && ::unlockpt(master) == 0 && ::ptsname_r(master, name, sizeof name) == 0);
        const int peer = ::open(name, O_RDWR | O_NOCTTY | O_CLOEXEC);
        QVERIFY(peer >= 0);
        ::close(peer);

        Program program;
        program.process().setChildProcessModifier([&program, master] {
            if (::dup2(master, STDIN_FILENO) < 0) {
                program.process().failChildProcessModifier("dup2", errno);
            }
        });
        const bool started = program.start(QDir(dir.path()).filePath(QStringLiteral("none.sock")),
                                           QDir(dir.path()).filePath(QStringLiteral("none.json")));
        ::close(master);
        master = -1;
        QVERIFY(started);
        QVERIFY(program.waitForExit());
        QCOMPARE(program.process().exitStatus(), QProcess::NormalExit);
        QCOMPARE(program.process().exitCode(), 1);
        const QByteArray log = program.standardError();
        QVERIFY2(log.contains("reading the input failed"), log.constData());
    }

    void failedStartStillReportsWhatWasLeftOut()
    {
        // One tool on a target that the socket lacks, and one that cannot
        // choose an overload. The second fails the start. The line about
        // the first must still reach stderr.
        Application application;
        QVERIFY(application.start());
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString manifest = QDir(dir.path()).filePath(QStringLiteral("mcp.json"));
        QVERIFY(writeFile(manifest, QByteArrayLiteral(R"({"tools": [
            {"call": "ghost.haunt", "name": "haunt", "description": "d"},
            {"call": "panes.split", "name": "split", "description": "d"}
        ]})")));
        Program program;
        QVERIFY(program.start(application.socketPath(), manifest));
        QVERIFY(program.waitForExit());
        QCOMPARE(program.process().exitStatus(), QProcess::NormalExit);
        QCOMPARE(program.process().exitCode(), 1);
        const QByteArray log = program.standardError();
        QVERIFY2(log.contains("phosphor-mcp-ipc: the socket has no target \"ghost\""), log.constData());
        QVERIFY2(log.contains("panes.split has 2 overloads"), log.constData());
    }

    void serverThatCannotStartEndsWithAnError()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Program program;
        // A directory as stdin. The server takes only a pipe, a socket, a
        // character device or a regular file.
        program.process().setStandardInputFile(dir.path());
        QVERIFY(program.start(QDir(dir.path()).filePath(QStringLiteral("none.sock")),
                              QDir(dir.path()).filePath(QStringLiteral("none.json"))));
        QVERIFY(program.waitForExit());
        QCOMPARE(program.process().exitStatus(), QProcess::NormalExit);
        QCOMPARE(program.process().exitCode(), 1);
        const QByteArray log = program.standardError();
        QVERIFY2(log.contains("phosphor-mcp-ipc: cannot start the MCP server on stdin and stdout"), log.constData());
    }

    void endOfInputDropsACallThatWaits()
    {
        // A client that writes a call and closes stdin gets no answer to
        // it. The end of the input ends the server, which sends only what
        // it has written. The call may have reached the application.
        Application application;
        QVERIFY(application.start());
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString manifest = QDir(dir.path()).filePath(QStringLiteral("mcp.json"));
        QVERIFY(writeFile(manifest, manifestJson()));

        Program program;
        QVERIFY(program.start(application.socketPath(), manifest));
        QVERIFY(program.write(request(1, QStringLiteral("server/discover"))));
        QCOMPARE(program.readReplies(1).size(), 1);
        QVERIFY(
            program.write(callRequest(2, QStringLiteral("wait_pane"), QJsonObject{{QLatin1StringView("pane"), 1}})));
        QVERIFY(spinUntil([&] {
            return application.panes().waiting().size() == 1;
        }));
        program.process().closeWriteChannel();
        QVERIFY(program.waitForExit());
        QCOMPARE(program.process().exitStatus(), QProcess::NormalExit);
        QCOMPARE(program.process().exitCode(), 0);
        QVERIFY(program.process().readAllStandardOutput().isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestPhosphorMcpIpcProgram)
#include "test_phosphor_mcp_ipc_program.moc"
