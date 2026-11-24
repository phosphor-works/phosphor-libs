// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// phosphor-mcp-ipc run as an MCP client runs it: a child with pipes for
// stdin and stdout, in a session of its own. The application's router runs
// in the test, so the test runs its event loop while it waits for the child.

#include "mcpipctesthelpers.h"
#include "mcptesthelpers.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTest>

using namespace PhosphorMcpIpcTests;
using namespace PhosphorMcpTests;

namespace {

class Program
{
public:
    Program()
    {
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.remove(QStringLiteral("QT_FORCE_STDERR_LOGGING"));
        process.setProcessEnvironment(environment);
        process.setUnixProcessParameters(QProcess::UnixProcessFlag::CreateNewSession);
    }
    ~Program()
    {
        if (process.state() != QProcess::NotRunning) {
            process.kill();
            process.waitForFinished(5000);
        }
        if (QTest::currentTestFailed()) {
            qWarning().noquote() << "stderr of phosphor-mcp-ipc:\n" << QString::fromUtf8(standardError());
        }
    }
    Q_DISABLE_COPY_MOVE(Program)

    bool start(const QString& socketPath, const QString& manifestPath)
    {
        process.start(QStringLiteral(PHOSPHOR_MCP_IPC_PATH),
                      {QStringLiteral("--socket"), socketPath, QStringLiteral("--manifest"), manifestPath});
        return process.waitForStarted(5000);
    }

    void write(const QJsonObject& message)
    {
        process.write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
    }

    // Up to `count` replies. The event loop runs meanwhile, for the router.
    QList<QJsonObject> readReplies(int count)
    {
        QList<QJsonObject> replies;
        spinUntil(
            [&] {
                while (process.canReadLine() && replies.size() < count) {
                    replies.append(QJsonDocument::fromJson(process.readLine()).object());
                }
                return replies.size() == count || process.state() == QProcess::NotRunning;
            },
            10000);
        return replies;
    }

    bool waitForExit()
    {
        return spinUntil(
            [&] {
                return process.state() == QProcess::NotRunning;
            },
            10000);
    }

    QByteArray standardError()
    {
        m_standardError += process.readAllStandardError();
        return m_standardError;
    }

    QProcess process;

private:
    QByteArray m_standardError;
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
        program.write(request(1, QStringLiteral("server/discover")));
        program.write(request(2, QStringLiteral("tools/list")));
        program.write(callRequest(
            3, QStringLiteral("rename_pane"),
            QJsonObject{{QLatin1StringView("title"), QStringLiteral("logs")}, {QLatin1StringView("pane"), 2}}));
        program.write(callRequest(4, QStringLiteral("close_pane"), QJsonObject{{QLatin1StringView("pane"), 5}}));
        const QList<QJsonObject> replies = program.readReplies(4);
        QCOMPARE(replies.size(), 4);

        QCOMPARE(resultOf(replies.at(0))
                     .value(QLatin1StringView("_meta"))
                     .toObject()
                     .value(PhosphorMcp::MetaServerInfo)
                     .toObject()
                     .value(QLatin1StringView("name"))
                     .toString(),
                 QStringLiteral("phosphor-mcp-ipc"));
        QCOMPARE(resultOf(replies.at(1)).value(QLatin1StringView("tools")).toArray().size(), 8);
        QCOMPARE(firstText(replies.at(2)), QStringLiteral("pane 2 is now logs"));
        QVERIFY(resultOf(replies.at(3)).value(QLatin1StringView("isError")).toBool());
        QCOMPARE(firstText(replies.at(3)), QStringLiteral("CALL_FAILED: no pane 5"));

        // What it left out, on stderr.
        QVERIFY(program.standardError().contains("the socket has no target \"ghost\""));

        // The end of stdin ends it, with 0.
        program.process.closeWriteChannel();
        QVERIFY(program.waitForExit());
        QCOMPARE(program.process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(program.process.exitCode(), 0);
    }

    void noManifestGivesNoTools()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Program program;
        // Neither the manifest nor the socket exists: with no tools nothing
        // connects.
        QVERIFY(program.start(QDir(dir.path()).filePath(QStringLiteral("none.sock")),
                              QDir(dir.path()).filePath(QStringLiteral("none.json"))));
        program.write(request(1, QStringLiteral("tools/list")));
        const QList<QJsonObject> replies = program.readReplies(1);
        QCOMPARE(replies.size(), 1);
        QCOMPARE(resultOf(replies.at(0)).value(QLatin1StringView("tools")).toArray(), QJsonArray());
        QVERIFY(program.standardError().contains("no manifest at "));
        program.process.closeWriteChannel();
        QVERIFY(program.waitForExit());
        QCOMPARE(program.process.exitCode(), 0);
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
        QCOMPARE(program.process.exitCode(), 1);
        QVERIFY(program.standardError().contains("panes.split has 2 overloads"));
        QVERIFY(program.process.readAllStandardOutput().isEmpty());
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
        QCOMPARE(program.process.exitCode(), 1);
        QVERIFY(program.standardError().contains("\"name\" is not a string"));
    }
};

QTEST_GUILESS_MAIN(TestPhosphorMcpIpcProgram)
#include "test_phosphor_mcp_ipc_program.moc"
