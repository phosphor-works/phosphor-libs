// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// The example server, run as an MCP client runs it: a child process with
// pipes for stdin and stdout. A smoke test of the whole path, from the
// command line to the exit.

#include "mcptesthelpers.h"

#include <PhosphorMcp/McpProtocol.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTest>

using namespace PhosphorMcp;
using namespace PhosphorMcpTests;

namespace {

QList<QJsonObject> readReplies(QProcess& process, int count)
{
    QList<QJsonObject> replies;
    QByteArray buffer;
    QDeadlineTimer deadline(10000);
    while (replies.size() < count && !deadline.hasExpired()) {
        if (!process.canReadLine() && !process.waitForReadyRead(100)) {
            continue;
        }
        while (process.canReadLine()) {
            const QByteArray line = process.readLine();
            replies.append(QJsonDocument::fromJson(line).object());
        }
    }
    return replies;
}

void write(QProcess& process, const QJsonObject& message)
{
    process.write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
}

} // namespace

class TestPhosphorMcpExample : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void answersAClient()
    {
        QProcess server;
        // Its diagnostics go to stderr, which the test does not read.
        server.setProcessChannelMode(QProcess::SeparateChannels);
        server.start(QStringLiteral(PHOSPHOR_MCP_EXAMPLE_PATH), QStringList());
        QVERIFY2(server.waitForStarted(5000), qPrintable(server.errorString()));

        write(server, request(1, QStringLiteral("server/discover")));
        write(server, request(2, QStringLiteral("tools/list")));
        write(server,
              callRequest(3, QStringLiteral("count_words"),
                          QJsonObject{{QLatin1StringView("text"), QStringLiteral("one two  three\nfour")}}));
        write(server, callRequest(4, QStringLiteral("count_words"), QJsonObject{{QLatin1StringView("text"), 5}}));
        const QList<QJsonObject> replies = readReplies(server, 4);
        QCOMPARE(replies.size(), 4);

        const QJsonObject discover = resultOf(replies.at(0));
        QCOMPARE(discover.value(QLatin1StringView("resultType")).toString(), QStringLiteral("complete"));
        QCOMPARE(discover.value(QLatin1StringView("supportedVersions")).toArray(),
                 QJsonArray{QStringLiteral("2026-07-28")});
        QCOMPARE(discover.value(QLatin1StringView("cacheScope")).toString(), QStringLiteral("public"));
        QCOMPARE(discover.value(QLatin1StringView("_meta"))
                     .toObject()
                     .value(MetaServerInfo)
                     .toObject()
                     .value(QLatin1StringView("name"))
                     .toString(),
                 QStringLiteral("phosphor-mcp-example"));

        const QJsonArray tools = resultOf(replies.at(1)).value(QLatin1StringView("tools")).toArray();
        QCOMPARE(tools.size(), 1);
        QCOMPARE(tools.first().toObject().value(QLatin1StringView("name")).toString(), QStringLiteral("count_words"));

        const QJsonObject counted = resultOf(replies.at(2));
        QCOMPARE(replies.at(2).value(QLatin1StringView("id")), QJsonValue(3));
        QCOMPARE(counted.value(QLatin1StringView("structuredContent")).toObject(),
                 (QJsonObject{{QLatin1StringView("words"), 4}, {QLatin1StringView("characters"), 19}}));

        // A bad argument is a tool error that names it.
        const QJsonObject refused = resultOf(replies.at(3));
        QCOMPARE(refused.value(QLatin1StringView("isError")).toBool(), true);
        QVERIFY(firstText(replies.at(3)).contains(QLatin1StringView("arguments/text")));

        // The end of stdin ends it, cleanly.
        server.closeWriteChannel();
        QVERIFY(server.waitForFinished(5000));
        QCOMPARE(server.exitStatus(), QProcess::NormalExit);
        QCOMPARE(server.exitCode(), 0);
        QVERIFY(server.readAllStandardOutput().isEmpty());
    }

    void endsOnSigterm()
    {
        QProcess server;
        server.start(QStringLiteral(PHOSPHOR_MCP_EXAMPLE_PATH), QStringList());
        QVERIFY(server.waitForStarted(5000));
        // Once it answers, its handler is in place.
        write(server, request(1, QStringLiteral("server/discover")));
        QCOMPARE(readReplies(server, 1).size(), 1);
        server.terminate();
        QVERIFY(server.waitForFinished(5000));
        QCOMPARE(server.exitStatus(), QProcess::NormalExit);
        QCOMPARE(server.exitCode(), 0);
    }
};

QTEST_GUILESS_MAIN(TestPhosphorMcpExample)
#include "test_phosphor_mcp_example.moc"
