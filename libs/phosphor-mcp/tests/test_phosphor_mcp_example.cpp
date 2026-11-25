// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: LGPL-2.1-or-later

// The example server, run as an MCP client runs it: a child process with
// pipes for stdin and stdout, in a session of its own without a terminal. A
// smoke test of the whole path, from the command line to the exit code, that
// checks each thing the example gives the library.

#include "mcptesthelpers.h"

#include <PhosphorMcp/McpProtocol.h>

#include <QDeadlineTimer>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTest>

#include <cerrno>
#include <cstdlib>

#include <fcntl.h>
#include <unistd.h>

using namespace PhosphorMcp;
using namespace PhosphorMcpTests;

namespace {

// The example's process. Its stderr goes to the test's output when a check
// fails, and the process is killed if it still runs at the end.
class Example
{
public:
    Example()
    {
        // Without a terminal and without QT_FORCE_STDERR_LOGGING, as a client
        // starts it. The example's diagnostics then reach stderr only because
        // it asks Qt for that itself.
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.remove(QStringLiteral("QT_FORCE_STDERR_LOGGING"));
        process.setProcessEnvironment(environment);
        process.setUnixProcessParameters(QProcess::UnixProcessFlag::CreateNewSession);
    }
    ~Example()
    {
        if (process.state() != QProcess::NotRunning) {
            process.kill();
            process.waitForFinished(5000);
        }
        if (QTest::currentTestFailed()) {
            qWarning().noquote() << "stderr of the example:\n" << QString::fromUtf8(standardError());
        }
    }
    Q_DISABLE_COPY_MOVE(Example)

    bool start()
    {
        process.start(QStringLiteral(PHOSPHOR_MCP_EXAMPLE_PATH), QStringList());
        return process.waitForStarted(5000);
    }

    void write(const QJsonObject& message)
    {
        process.write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
    }

    // Up to `count` replies, fewer when the process ends or 10 s pass first.
    QList<QJsonObject> readReplies(int count)
    {
        QList<QJsonObject> replies;
        const QDeadlineTimer deadline(10000);
        while (replies.size() < count) {
            while (process.canReadLine() && replies.size() < count) {
                replies.append(QJsonDocument::fromJson(process.readLine()).object());
            }
            if (replies.size() == count || deadline.hasExpired()) {
                break;
            }
            // False at once when the process has ended: then no more comes.
            if (!process.waitForReadyRead(int(qMax<qint64>(1, deadline.remainingTime())))
                && process.state() == QProcess::NotRunning && !process.canReadLine()) {
                break;
            }
        }
        return replies;
    }

    bool waitForExit()
    {
        return process.waitForFinished(5000);
    }

    // Everything the process wrote to stderr so far.
    QByteArray standardError()
    {
        m_standardError += process.readAllStandardError();
        return m_standardError;
    }

    QProcess process;

private:
    QByteArray m_standardError;
};

QJsonObject parse(const char* json)
{
    return QJsonDocument::fromJson(json).object();
}

QJsonObject serverInfo(const QJsonObject& reply)
{
    return resultOf(reply).value(QLatin1StringView("_meta")).toObject().value(MetaServerInfo).toObject();
}

} // namespace

class TestPhosphorMcpExample : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void answersAClient()
    {
        Example example;
        QVERIFY(example.start());

        example.write(request(1, QStringLiteral("server/discover")));
        example.write(request(2, QStringLiteral("tools/list")));
        // Words split at every Unicode space, and a character outside the
        // BMP (two UTF-16 code units) counts once.
        example.write(
            callRequest(3, QStringLiteral("count_words"),
                        QJsonObject{{QLatin1StringView("text"), QStringLiteral("one two　three \U0001F600 four\n")}}));
        example.write(callRequest(4, QStringLiteral("count_words"), QJsonObject()));
        example.write(callRequest(5, QStringLiteral("count_words"),
                                  QJsonObject{{QLatin1StringView("text"), QStringLiteral("a")},
                                              {QLatin1StringView("lang"), QStringLiteral("en")}}));
        example.write(callRequest(6, QStringLiteral("count_words"), QJsonObject{{QLatin1StringView("text"), 5}}));
        const QList<QJsonObject> replies = example.readReplies(6);
        QCOMPARE(replies.size(), 6);

        // Every result names the server, with the library's version.
        const QJsonObject info{
            {QLatin1StringView("name"), QStringLiteral("wordcount")},
            {QLatin1StringView("version"), QStringLiteral(PHOSPHORMCP_VERSION)},
        };
        for (const QJsonObject& reply : replies) {
            QCOMPARE(serverInfo(reply), info);
        }

        QJsonObject discover = resultOf(replies.at(0));
        discover.remove(QLatin1StringView("_meta"));
        QCOMPARE(discover, parse(R"({
            "resultType": "complete",
            "supportedVersions": ["2026-07-28"],
            "capabilities": {"tools": {}},
            "instructions": "count_words counts the words and characters of a text.",
            "ttlMs": 3600000,
            "cacheScope": "public"
        })"));

        QJsonObject list = resultOf(replies.at(1));
        list.remove(QLatin1StringView("_meta"));
        QCOMPARE(list, parse(R"({
            "resultType": "complete",
            "ttlMs": 3600000,
            "cacheScope": "public",
            "tools": [{
                "name": "count_words",
                "title": "Count words",
                "description": "Counts the words (runs of characters that are not spaces) and the characters (Unicode code points) of a text.",
                "inputSchema": {
                    "type": "object",
                    "properties": {"text": {"type": "string", "description": "The text to count", "maxLength": 1000000}},
                    "required": ["text"],
                    "additionalProperties": false
                },
                "outputSchema": {
                    "type": "object",
                    "properties": {"words": {"type": "integer"}, "characters": {"type": "integer"}},
                    "required": ["words", "characters"]
                },
                "annotations": {"readOnlyHint": true, "destructiveHint": false, "idempotentHint": true, "openWorldHint": false}
            }]
        })"));

        QCOMPARE(replies.at(2).value(QLatin1StringView("id")), QJsonValue(3));
        const QJsonObject counted = resultOf(replies.at(2));
        QCOMPARE(counted.value(QLatin1StringView("structuredContent")).toObject(),
                 (QJsonObject{{QLatin1StringView("words"), 5}, {QLatin1StringView("characters"), 21}}));
        QCOMPARE(firstText(replies.at(2)), QStringLiteral("{\"characters\":21,\"words\":5}"));
        QVERIFY(!counted.contains(QLatin1StringView("isError")));

        // Arguments that do not match are a tool error that names each one.
        const auto refused = [&](qsizetype index) {
            return resultOf(replies.at(index)).value(QLatin1StringView("isError")).toBool();
        };
        QVERIFY(refused(3));
        QCOMPARE(firstText(replies.at(3)),
                 QStringLiteral("Invalid arguments for tool count_words:\n- arguments/text: is required"));
        QVERIFY(refused(4));
        QCOMPARE(firstText(replies.at(4)),
                 QStringLiteral("Invalid arguments for tool count_words:\n- arguments/lang: is not a known property"));
        QVERIFY(refused(5));
        QCOMPARE(firstText(replies.at(5)),
                 QStringLiteral("Invalid arguments for tool count_words:\n- arguments/text: must be of type string"));

        // The end of stdin ends it, cleanly, and stdout carried nothing else.
        example.process.closeWriteChannel();
        QVERIFY(example.waitForExit());
        QCOMPARE(example.process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(example.process.exitCode(), 0);
        QVERIFY(example.process.readAllStandardOutput().isEmpty());
    }

    void endsOnSigterm()
    {
        Example example;
        QVERIFY(example.start());
        // Once it answers, its handler is in place.
        example.write(request(1, QStringLiteral("server/discover")));
        QCOMPARE(example.readReplies(1).size(), 1);
        example.process.terminate();
        QVERIFY(example.waitForExit());
        QCOMPARE(example.process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(example.process.exitCode(), 0);
    }

    // A write that fails for another reason than a closed reader is an
    // error: exit code 1, and the reason on stderr.
    void exitsWithOneWhenOutputFails()
    {
        Example example;
        // Every write to /dev/full fails with ENOSPC.
        example.process.setStandardOutputFile(QStringLiteral("/dev/full"));
        QVERIFY(example.start());
        example.write(request(1, QStringLiteral("server/discover")));
        QVERIFY(example.waitForExit());
        QCOMPARE(example.process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(example.process.exitCode(), 1);
        QVERIFY(example.standardError().contains("writing the output failed"));
    }

    // A read that fails is an error too.
    void exitsWithOneWhenInputFails()
    {
        // A pseudo-terminal's master whose other end is closed: each read
        // fails with EIO.
        const int master = ::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
        QVERIFY(master >= 0);
        char name[128];
        QVERIFY(::grantpt(master) == 0 && ::unlockpt(master) == 0 && ::ptsname_r(master, name, sizeof name) == 0);
        const int peer = ::open(name, O_RDWR | O_NOCTTY | O_CLOEXEC);
        QVERIFY(peer >= 0);
        ::close(peer);

        Example example;
        example.process.setChildProcessModifier([&example, master] {
            if (::dup2(master, STDIN_FILENO) < 0) {
                example.process.failChildProcessModifier("dup2", errno);
            }
        });
        const bool started = example.start();
        ::close(master);
        QVERIFY(started);
        QVERIFY(example.waitForExit());
        QCOMPARE(example.process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(example.process.exitCode(), 1);
        QVERIFY(example.standardError().contains("reading the input failed"));
    }

    // A server that cannot start exits with 1 at once. A directory is no
    // stdin for it.
    void exitsWithOneWhenItCannotStart()
    {
        Example example;
        example.process.setStandardInputFile(QDir::currentPath());
        QVERIFY(example.start());
        QVERIFY(example.waitForExit());
        QCOMPARE(example.process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(example.process.exitCode(), 1);
        QVERIFY(example.standardError().contains("is not a pipe"));
    }
};

QTEST_GUILESS_MAIN(TestPhosphorMcpExample)
#include "test_phosphor_mcp_example.moc"
