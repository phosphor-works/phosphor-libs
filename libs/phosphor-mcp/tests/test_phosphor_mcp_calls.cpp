// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// Calls that answer later: completion out of order, a slow reader of a large
// reply, back-pressure on a client that reads nothing, nested event loops in
// handlers and cancel callbacks, cancellation and cancel callbacks that
// destroy calls, progress, calls dropped unanswered or overwritten by a move,
// request ids, the end of the input while calls run and the drain of the
// replies after it, a failed read or write, the destruction of the server
// with calls held, the shapes of results and their structured content, and
// the use of a call off the server's thread.

#include "mcptesthelpers.h"

#include <PhosphorMcp/McpProtocol.h>
#include <PhosphorMcp/McpServer.h>
#include <PhosphorMcp/McpTransport.h>

#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>

#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <thread>
#include <type_traits>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace PhosphorMcp;
using namespace PhosphorMcpTests;

static_assert(!std::is_copy_constructible_v<McpCall>);
static_assert(!std::is_copy_assignable_v<McpCall>);
static_assert(std::is_nothrow_move_constructible_v<McpCall>);
static_assert(std::is_nothrow_move_assignable_v<McpCall>);

namespace {

// A server whose "job" tool keeps each call under the name its "job"
// argument gives, for the test to answer, and whose "now" tool answers at
// once.
struct Fixture
{
    Peer peer;
    std::unique_ptr<McpServer> server = std::make_unique<McpServer>();
    std::map<QString, McpCall> jobs;
    int nowCalls = 0;

    Fixture()
    {
        server->addTool(McpTool(QStringLiteral("job"))
                            .setInputSchema(QJsonObject{
                                {QLatin1StringView("type"), QStringLiteral("object")},
                                {QLatin1StringView("properties"),
                                 QJsonObject{{QLatin1StringView("job"),
                                              QJsonObject{{QLatin1StringView("type"), QStringLiteral("string")}}}}},
                                {QLatin1StringView("required"), QJsonArray{QStringLiteral("job")}},
                            }),
                        [this](McpCall call) {
                            const QString name = call.arguments().value(QLatin1StringView("job")).toString();
                            jobs.insert_or_assign(name, std::move(call));
                        });
        server->addTool(McpTool(QStringLiteral("now")), [this](McpCall call) {
            ++nowCalls;
            call.finish(McpResult::text(QStringLiteral("now")));
        });
    }
    // Adds a tool that answers at once with `bytes` of text.
    void addBlob(qint64 bytes)
    {
        server->addTool(McpTool(QStringLiteral("blob")), [this, bytes](McpCall call) {
            ++blobCalls;
            call.finish(McpResult::text(QString(bytes, QLatin1Char('b'))));
            maxQueued = qMax(maxQueued, server->transport()->pendingWriteBytes());
        });
    }
    int blobCalls = 0;
    qint64 maxQueued = 0;
    bool start()
    {
        return server->start(peer.serverFd(), peer.serverFd());
    }
    bool callJob(const QJsonValue& id, const QString& job, const QJsonObject& requestMeta = meta())
    {
        return peer.send(
                   callRequest(id, QStringLiteral("job"), QJsonObject{{QLatin1StringView("job"), job}}, requestMeta))
            && spinUntil([&] {
                   return jobs.count(job) == 1;
               });
    }
    // Answers a "now" call, so that everything the client sent before it
    // was handled.
    bool sync(int id)
    {
        return peer.send(callRequest(id, QStringLiteral("now")))
            && peer.readOne().value(QLatin1StringView("id")) == QJsonValue(id);
    }
};

QJsonObject metaWithToken(const QJsonValue& token)
{
    QJsonObject m = meta();
    m.insert(MetaProgressToken, token);
    return m;
}

QJsonObject errorOf(const QJsonObject& message)
{
    return message.value(QLatin1StringView("error")).toObject();
}

// Sets a flag when it is destroyed, to show what a callback owned went.
struct DestroyFlag
{
    explicit DestroyFlag(bool* flag)
        : m_flag(flag)
    {
    }
    ~DestroyFlag()
    {
        *m_flag = true;
    }
    Q_DISABLE_COPY_MOVE(DestroyFlag)
    bool* m_flag;
};

QJsonObject typed(const char* type)
{
    return QJsonObject{{QLatin1StringView("type"), QString::fromLatin1(type)}};
}

} // namespace

class TestPhosphorMcpCalls : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init()
    {
        // Every warning a test expects, it names with ignoreMessage(). One
        // more is a failure: a call left unanswered at the end of a test, or
        // a drain cut short.
        QTest::failOnWarning();
    }

    void answersInTheOrderTheyFinish()
    {
        Fixture f;
        QVERIFY(f.start());
        QVERIFY(f.callJob(1, QStringLiteral("a")));
        QVERIFY(f.callJob(2, QStringLiteral("b")));
        QVERIFY(f.peer.readFor(100).isEmpty());

        // A call that answers at once does not wait for the two before it.
        QVERIFY(f.peer.send(callRequest(3, QStringLiteral("now"))));
        QCOMPARE(f.peer.readOne().value(QLatin1StringView("id")), QJsonValue(3));

        QVERIFY(f.jobs.at(QStringLiteral("b")).finish(McpResult::text(QStringLiteral("b done"))));
        QVERIFY(f.jobs.at(QStringLiteral("a")).finish(McpResult::text(QStringLiteral("a done"))));
        const QList<QJsonObject> replies = f.peer.read(2);
        QCOMPARE(replies.size(), 2);
        QCOMPARE(replies.at(0).value(QLatin1StringView("id")), QJsonValue(2));
        QCOMPARE(firstText(replies.at(0)), QStringLiteral("b done"));
        QCOMPARE(replies.at(1).value(QLatin1StringView("id")), QJsonValue(1));
        QCOMPARE(firstText(replies.at(1)), QStringLiteral("a done"));
    }

    void slowReaderOfALargeReply()
    {
        // A reply of 16 MiB to a client that reads 64 KiB at a time. finish()
        // returns at once with most of it queued, and the server goes on
        // reading and running calls while the queue drains.
        Fixture f;
        QVERIFY(f.start());
        QVERIFY(f.callJob(1, QStringLiteral("big")));
        const QString big(16 * 1024 * 1024, QLatin1Char('x'));
        QVERIFY(f.jobs.at(QStringLiteral("big")).finish(McpResult::text(big)));
        const McpTransport* transport = f.server->transport();
        QVERIFY(transport);
        QVERIFY(transport->pendingWriteBytes() > 8 * 1024 * 1024);

        // Nothing read yet: the next call still runs, and timers still fire.
        int ticks = 0;
        QTimer timer;
        connect(&timer, &QTimer::timeout, this, [&] {
            ++ticks;
        });
        timer.start(1);
        QVERIFY(f.peer.send(callRequest(2, QStringLiteral("now"))));
        QVERIFY(spinUntil([&] {
            return f.nowCalls == 1;
        }));
        spinFor(50);
        // Sampled while the client still reads nothing, and most of the
        // reply waits in the queue.
        QVERIFY2(ticks > 5, qPrintable(QString::number(ticks)));
        QVERIFY(transport->pendingWriteBytes() > 8 * 1024 * 1024);

        QList<QJsonObject> replies;
        QVERIFY(spinUntil(
            [&] {
                f.peer.pump(64 * 1024);
                if (transport->pendingWriteBytes() == 0) {
                    replies = f.peer.read(2, 2000);
                    return true;
                }
                return false;
            },
            20000));
        QCOMPARE(replies.size(), 2);
        // Finished first, sent first: the large reply, then the small one
        // that waited behind it.
        QCOMPARE(replies.at(0).value(QLatin1StringView("id")), QJsonValue(1));
        QCOMPARE(firstText(replies.at(0)).size(), big.size());
        QCOMPARE(replies.at(1).value(QLatin1StringView("id")), QJsonValue(2));
    }

    void backpressureOnANonReadingClient()
    {
        // Many calls with large results, and a client that reads nothing.
        // Past the mark the server stops reading requests, so its queue stays
        // near the mark. When the client reads, the rest are read and
        // answered, each once and in order.
        constexpr qint64 Mark = 4 * 1024 * 1024;
        constexpr qint64 Blob = 1024 * 1024;
        constexpr int Calls = 30;
        Fixture f;
        f.addBlob(Blob);
        f.server->setMaxPendingWriteBytes(Mark);
        QVERIFY(f.start());
        QByteArray requests;
        for (int id = 1; id <= Calls; ++id) {
            requests += QJsonDocument(callRequest(id, QStringLiteral("blob"))).toJson(QJsonDocument::Compact) + '\n';
        }
        // All in the socket at once: the server has them all to read.
        QVERIFY(f.peer.sendRaw(requests));
        QVERIFY(spinUntil([&] {
            return f.server->transport()->isReadingPaused();
        }));
        spinFor(200);
        const int handledWhilePaused = f.blobCalls;
        QVERIFY2(handledWhilePaused < Calls, qPrintable(QString::number(handledWhilePaused)));
        QVERIFY(f.server->transport()->isReadingPaused());
        QVERIFY(f.maxQueued <= Mark + Blob + 4096);

        // A request sent now is not read either.
        QVERIFY(f.peer.send(callRequest(Calls + 1, QStringLiteral("now"))));
        spinFor(100);
        QCOMPARE(f.nowCalls, 0);
        QCOMPARE(f.blobCalls, handledWhilePaused);

        // The client reads: everything is answered.
        const QList<QJsonObject> replies = f.peer.read(Calls + 1, 20000);
        QCOMPARE(replies.size(), Calls + 1);
        for (int i = 0; i < Calls; ++i) {
            QCOMPARE(replies.at(i).value(QLatin1StringView("id")), QJsonValue(i + 1));
            QCOMPARE(firstText(replies.at(i)).size(), Blob);
        }
        QCOMPARE(replies.at(Calls).value(QLatin1StringView("id")), QJsonValue(Calls + 1));
        QCOMPARE(f.blobCalls, Calls);
        QCOMPARE(f.nowCalls, 1);
        QVERIFY(f.peer.readFor(100).isEmpty());
        QVERIFY(!f.server->transport()->isReadingPaused());
    }

    void progressIsDroppedWhileTheClientReadsNothing()
    {
        // Progress is a hint, and is not queued past the high-water mark.
        constexpr qint64 Mark = 64 * 1024;
        Fixture f;
        f.server->setMaxPendingWriteBytes(Mark);
        QVERIFY(f.start());
        QVERIFY(f.callJob(1, QStringLiteral("a"), metaWithToken(QStringLiteral("tok"))));
        QVERIFY(f.callJob(2, QStringLiteral("big")));
        QVERIFY(f.jobs.at(QStringLiteral("big")).finish(McpResult::text(QString(4 * 1024 * 1024, QLatin1Char('x')))));
        const McpTransport* transport = f.server->transport();
        const qint64 queued = transport->pendingWriteBytes();
        // The precondition: the reply waits for the client, past the mark.
        QVERIFY2(queued > Mark, qPrintable(QString::number(queued)));

        McpCall& call = f.jobs.at(QStringLiteral("a"));
        QVERIFY(!call.progress(1));
        QCOMPARE(transport->pendingWriteBytes(), queued);

        // The client reads. The dropped value was never sent, so it may go
        // now.
        const QJsonObject reply = f.peer.readOne(10000);
        QCOMPARE(reply.value(QLatin1StringView("id")), QJsonValue(2));
        QVERIFY(spinUntil([&] {
            return transport->pendingWriteBytes() == 0;
        }));
        QVERIFY(call.progress(1));
        const QJsonObject note = f.peer.readOne();
        QCOMPARE(note.value(QLatin1StringView("method")).toString(), QStringLiteral("notifications/progress"));
        QCOMPARE(note.value(QLatin1StringView("params")).toObject().value(QLatin1StringView("progress")),
                 QJsonValue(1));
        QVERIFY(call.finish(McpResult::text(QStringLiteral("done"))));
        QCOMPARE(f.peer.readOne().value(QLatin1StringView("id")), QJsonValue(1));
    }

    void progressUnderAMarkBelowOne()
    {
        // A mark under 1 counts as 1, so progress on an idle server still
        // goes out.
        Fixture f;
        f.server->setMaxPendingWriteBytes(-1);
        QVERIFY(f.start());
        QCOMPARE(f.server->transport()->maxPendingWriteBytes(), 1);
        QVERIFY(f.callJob(1, QStringLiteral("a"), metaWithToken(QStringLiteral("tok"))));
        McpCall& call = f.jobs.at(QStringLiteral("a"));
        QCOMPARE(f.server->transport()->pendingWriteBytes(), 0);
        QVERIFY(call.progress(1));
        const QJsonObject note = f.peer.readOne();
        QCOMPARE(note.value(QLatin1StringView("method")).toString(), QStringLiteral("notifications/progress"));
        QVERIFY(call.finish(McpResult::text(QStringLiteral("done"))));
        QCOMPARE(f.peer.readOne().value(QLatin1StringView("id")), QJsonValue(1));
    }

    void nestedEventLoopInAHandler_data()
    {
        QTest::addColumn<bool>("duringNestedLoop");
        // All three requests in the socket before the first handler runs.
        QTest::newRow("in one read") << false;
        // The other two arrive while the handler's nested loop runs, which
        // is when a reader that is still on would take them.
        QTest::newRow("arriving during the nested loop") << true;
    }

    void nestedEventLoopInAHandler()
    {
        // The handler of the first request runs a nested event loop. The
        // other two wait until it returns, and each of the three is answered
        // once, in order.
        QFETCH(bool, duringNestedLoop);
        Fixture f;
        QByteArray later;
        for (const QJsonObject& message :
             {callRequest(2, QStringLiteral("now")), request(3, QStringLiteral("server/discover"))}) {
            later += QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
        }
        int nowDuringNested = -1;
        QVERIFY(f.server->addTool(McpTool(QStringLiteral("nested")), [&](McpCall call) {
            if (duringNestedLoop) {
                f.peer.sendRaw(later);
            }
            QEventLoop loop;
            QTimer::singleShot(100, &loop, &QEventLoop::quit);
            loop.exec();
            nowDuringNested = f.nowCalls;
            call.finish(McpResult::text(QStringLiteral("nested")));
        }));
        QVERIFY(f.start());
        QByteArray first =
            QJsonDocument(callRequest(1, QStringLiteral("nested"))).toJson(QJsonDocument::Compact) + '\n';
        if (!duringNestedLoop) {
            first += later;
        }
        QVERIFY(f.peer.sendRaw(first));
        const QList<QJsonObject> replies = f.peer.read(3);
        QCOMPARE(replies.size(), 3);
        QCOMPARE(nowDuringNested, 0);
        QCOMPARE(replies.at(0).value(QLatin1StringView("id")), QJsonValue(1));
        QCOMPARE(firstText(replies.at(0)), QStringLiteral("nested"));
        QCOMPARE(replies.at(1).value(QLatin1StringView("id")), QJsonValue(2));
        QCOMPARE(firstText(replies.at(1)), QStringLiteral("now"));
        QCOMPARE(replies.at(2).value(QLatin1StringView("id")), QJsonValue(3));
        QVERIFY(replies.at(2)
                    .value(QLatin1StringView("result"))
                    .toObject()
                    .contains(QLatin1StringView("supportedVersions")));
        QVERIFY(f.peer.readFor(150).isEmpty());
        QCOMPARE(f.nowCalls, 1);
    }

    void nestedEventLoopInACancelCallback_data()
    {
        QTest::addColumn<bool>("duringNestedLoop");
        QTest::newRow("in one read") << false;
        QTest::newRow("arriving during the nested loop") << true;
    }

    void nestedEventLoopInACancelCallback()
    {
        QFETCH(bool, duringNestedLoop);
        Fixture f;
        QVERIFY(f.start());
        QVERIFY(f.callJob(1, QStringLiteral("a")));
        QByteArray later;
        for (const QJsonObject& message :
             {callRequest(2, QStringLiteral("now")), request(3, QStringLiteral("server/discover"))}) {
            later += QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
        }
        int nowDuringNested = -1;
        f.jobs.at(QStringLiteral("a")).onCancelled([&] {
            if (duringNestedLoop) {
                f.peer.sendRaw(later);
            }
            QEventLoop loop;
            QTimer::singleShot(100, &loop, &QEventLoop::quit);
            loop.exec();
            nowDuringNested = f.nowCalls;
        });
        QByteArray first = QJsonDocument(cancelNotification(1)).toJson(QJsonDocument::Compact) + '\n';
        if (!duringNestedLoop) {
            first += later;
        }
        QVERIFY(f.peer.sendRaw(first));
        const QList<QJsonObject> replies = f.peer.read(2);
        QCOMPARE(replies.size(), 2);
        QCOMPARE(nowDuringNested, 0);
        QCOMPARE(replies.at(0).value(QLatin1StringView("id")), QJsonValue(2));
        QCOMPARE(replies.at(1).value(QLatin1StringView("id")), QJsonValue(3));
        QVERIFY(f.peer.readFor(150).isEmpty());
        QCOMPARE(f.nowCalls, 1);
    }

    void integerIdsPastTwoToThe53()
    {
        // As doubles the two ids are equal. They are two calls.
        Fixture f;
        QVERIFY(f.start());
        const auto rawCall = [](const char* id, const char* job) -> QByteArray {
            return QByteArray("{\"jsonrpc\":\"2.0\",\"id\":") + id
                + ",\"method\":\"tools/call\",\"params\":{\"name\":\"job\",\"arguments\":{\"job\":\"" + job
                + "\"},\"_meta\":" + QJsonDocument(meta()).toJson(QJsonDocument::Compact) + "}}\n";
        };
        QVERIFY(f.peer.sendRaw(rawCall("9007199254740992", "low")));
        QVERIFY(f.peer.sendRaw(rawCall("9007199254740993", "high")));
        QVERIFY(spinUntil([&] {
            return f.jobs.size() == 2;
        }));
        QVERIFY(f.peer.readFor(100).isEmpty());

        int lowCancelled = 0;
        int highCancelled = 0;
        f.jobs.at(QStringLiteral("low")).onCancelled([&] {
            ++lowCancelled;
        });
        f.jobs.at(QStringLiteral("high")).onCancelled([&] {
            ++highCancelled;
        });
        QVERIFY(
            f.peer.sendRaw("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled\","
                           "\"params\":{\"requestId\":9007199254740992}}\n"));
        QVERIFY(spinUntil([&] {
            return lowCancelled == 1;
        }));
        QCOMPARE(highCancelled, 0);

        // The reply carries the id with all its digits.
        QVERIFY(f.jobs.at(QStringLiteral("high")).finish(McpResult::text(QStringLiteral("high"))));
        const QJsonObject reply = f.peer.readOne();
        QCOMPARE(reply.value(QLatin1StringView("id")).toInteger(), Q_INT64_C(9007199254740993));
        QCOMPARE(firstText(reply), QStringLiteral("high"));
        QVERIFY(f.peer.readFor(100).isEmpty());
    }

    void cancellation()
    {
        Fixture f;
        QVERIFY(f.start());
        QVERIFY(f.callJob(5, QStringLiteral("a"), metaWithToken(QStringLiteral("tok"))));
        McpCall& call = f.jobs.at(QStringLiteral("a"));
        int cancelled = 0;
        call.onCancelled([&] {
            ++cancelled;
        });
        QVERIFY(call.isValid());
        QVERIFY(!call.isCancelled());
        // The request has a token, so progress goes out while it runs.
        QVERIFY(call.progress(1));
        QCOMPARE(f.peer.readOne().value(QLatin1StringView("method")).toString(),
                 QStringLiteral("notifications/progress"));

        QVERIFY(f.peer.send(cancelNotification(5)));
        QVERIFY(spinUntil([&] {
            return cancelled == 1;
        }));
        QVERIFY(call.isCancelled());
        QVERIFY(!call.isValid());
        // Nothing more for the request: no reply, no progress, ever.
        QVERIFY(!call.finish(McpResult::text(QStringLiteral("late"))));
        QVERIFY(!call.fail(QStringLiteral("late")));
        QVERIFY(!call.progress(2));
        QVERIFY(f.peer.readFor(150).isEmpty());

        // A callback set after the cancellation runs at once.
        int late = 0;
        call.onCancelled([&] {
            ++late;
        });
        QCOMPARE(late, 1);
        // A second cancel of the same request changes nothing. The call
        // after it is answered once the server handled the cancel.
        QVERIFY(f.peer.send(cancelNotification(5)));
        QVERIFY(f.sync(6));
        QCOMPARE(cancelled, 1);
        QCOMPARE(late, 1);
        QVERIFY(call.isCancelled());

        // Its id is free again, for a call that answers as any other.
        f.jobs.clear();
        QVERIFY(f.callJob(5, QStringLiteral("again")));
        QVERIFY(f.jobs.at(QStringLiteral("again")).finish(McpResult::text(QStringLiteral("ok"))));
        const QJsonObject reply = f.peer.readOne();
        QCOMPARE(reply.value(QLatin1StringView("id")), QJsonValue(5));
        QVERIFY(!reply.contains(QLatin1StringView("error")));
        QCOMPARE(firstText(reply), QStringLiteral("ok"));
    }

    void cancelReachesTheRunningCallOfAReusedId()
    {
        // The cancelled call is still held, and a new call takes its id. A
        // cancel of that id is for the new call.
        Fixture f;
        QVERIFY(f.start());
        QVERIFY(f.callJob(5, QStringLiteral("old")));
        int oldRuns = 0;
        f.jobs.at(QStringLiteral("old")).onCancelled([&] {
            ++oldRuns;
        });
        QVERIFY(f.peer.send(cancelNotification(5)));
        QVERIFY(spinUntil([&] {
            return oldRuns == 1;
        }));

        QVERIFY(f.callJob(5, QStringLiteral("new")));
        int newRuns = 0;
        f.jobs.at(QStringLiteral("new")).onCancelled([&] {
            ++newRuns;
        });
        QVERIFY(f.peer.send(cancelNotification(5)));
        QVERIFY(f.sync(6));
        QCOMPARE(newRuns, 1);
        QCOMPARE(oldRuns, 1);
        QVERIFY(f.jobs.at(QStringLiteral("old")).isCancelled());
        QVERIFY(f.jobs.at(QStringLiteral("new")).isCancelled());
    }

    void cancelsThatMatchNothingAreIgnored()
    {
        Fixture f;
        QVERIFY(f.start());
        // A string id, and a number id with the same text as another string.
        QVERIFY(f.callJob(QStringLiteral("1"), QStringLiteral("a")));
        QVERIFY(f.callJob(2, QStringLiteral("b")));
        int cancelled = 0;
        for (const char* job : {"a", "b"}) {
            f.jobs.at(QString::fromLatin1(job)).onCancelled([&] {
                ++cancelled;
            });
        }
        // Another id, the number 1 for the string "1", the string "2" for the
        // number 2, a requestId that is no id, no requestId, params that are
        // not an object, and a call that already answered.
        QVERIFY(f.peer.send(cancelNotification(99)));
        QVERIFY(f.peer.send(cancelNotification(1)));
        QVERIFY(f.peer.send(cancelNotification(QStringLiteral("2"))));
        QVERIFY(f.peer.send(cancelNotification(QJsonObject())));
        QVERIFY(f.peer.send(cancelNotification(1.5)));
        QVERIFY(f.peer.send(notification(QStringLiteral("notifications/cancelled"))));
        QJsonObject arrayParams = notification(QStringLiteral("notifications/cancelled"));
        arrayParams.insert(QLatin1StringView("params"), QJsonArray{QStringLiteral("1")});
        QVERIFY(f.peer.send(arrayParams));
        QVERIFY(f.sync(7));
        QVERIFY(f.peer.send(cancelNotification(7)));
        QVERIFY(f.sync(8));
        QVERIFY(f.peer.readFor(100).isEmpty());
        QCOMPARE(cancelled, 0);
        QVERIFY(f.jobs.at(QStringLiteral("a")).finish(McpResult::text(QStringLiteral("ok"))));
        QCOMPARE(f.peer.readOne().value(QLatin1StringView("id")), QJsonValue(QStringLiteral("1")));
        QVERIFY(f.jobs.at(QStringLiteral("b")).finish(McpResult::text(QStringLiteral("ok"))));
        QCOMPARE(f.peer.readOne().value(QLatin1StringView("id")), QJsonValue(2));
    }

    void cancelCallbackMayDestroyTheCall()
    {
        Fixture f;
        QVERIFY(f.start());
        QVERIFY(f.callJob(1, QStringLiteral("a")));
        bool ran = false;
        f.jobs.at(QStringLiteral("a")).onCancelled([&] {
            ran = true;
            f.jobs.erase(QStringLiteral("a"));
        });
        QVERIFY(f.peer.send(cancelNotification(1)));
        QVERIFY(spinUntil([&] {
            return ran;
        }));
        QVERIFY(f.jobs.empty());
        QVERIFY(f.peer.readFor(100).isEmpty());
    }

    void callbackThatRunsAtOnceMayDestroyTheCall()
    {
        // On a cancelled call the callback runs inside onCancelled(), which
        // must not touch the call after it.
        Fixture f;
        QVERIFY(f.start());
        QVERIFY(f.callJob(1, QStringLiteral("a")));
        QVERIFY(f.peer.send(cancelNotification(1)));
        QVERIFY(spinUntil([&] {
            return f.jobs.at(QStringLiteral("a")).isCancelled();
        }));
        bool ran = false;
        f.jobs.at(QStringLiteral("a")).onCancelled([&] {
            ran = true;
            f.jobs.erase(QStringLiteral("a"));
        });
        QVERIFY(ran);
        QVERIFY(f.jobs.empty());
        QVERIFY(f.peer.readFor(100).isEmpty());
    }

    void finishInsideACancelCallback()
    {
        Fixture f;
        QVERIFY(f.start());
        QVERIFY(f.callJob(1, QStringLiteral("a")));
        std::optional<bool> finished;
        f.jobs.at(QStringLiteral("a")).onCancelled([&] {
            finished = f.jobs.at(QStringLiteral("a")).finish(McpResult::text(QStringLiteral("late")));
        });
        QVERIFY(f.peer.send(cancelNotification(1)));
        QVERIFY(spinUntil([&] {
            return finished.has_value();
        }));
        QVERIFY(!*finished);
        QVERIFY(f.peer.readFor(100).isEmpty());
    }

    void onCancelledTwice()
    {
        // The second callback replaces the first, which goes without running.
        Fixture f;
        QVERIFY(f.start());
        QVERIFY(f.callJob(1, QStringLiteral("a")));
        McpCall& call = f.jobs.at(QStringLiteral("a"));
        bool firstGone = false;
        int first = 0;
        int second = 0;
        call.onCancelled([&first, guard = std::make_shared<DestroyFlag>(&firstGone)] {
            ++first;
        });
        QVERIFY(!firstGone);
        call.onCancelled([&] {
            ++second;
        });
        QVERIFY(firstGone);
        // An empty callback changes nothing.
        call.onCancelled(std::function<void()>());
        QVERIFY(f.peer.send(cancelNotification(1)));
        QVERIFY(spinUntil([&] {
            return second == 1;
        }));
        QCOMPARE(first, 0);

        // On an answered call it is dropped at once, and never runs.
        QVERIFY(f.callJob(2, QStringLiteral("b")));
        QVERIFY(f.jobs.at(QStringLiteral("b")).finish(McpResult::text(QStringLiteral("b"))));
        QCOMPARE(f.peer.readOne().value(QLatin1StringView("id")), QJsonValue(2));
        bool answeredGone = false;
        int answeredRuns = 0;
        f.jobs.at(QStringLiteral("b"))
            .onCancelled([&answeredRuns, guard = std::make_shared<DestroyFlag>(&answeredGone)] {
                ++answeredRuns;
            });
        QVERIFY(answeredGone);
        QCOMPARE(answeredRuns, 0);
    }

    void endMarksEveryCallCancelledBeforeAnyCallbackRuns()
    {
        // The first callback to run destroys every call, its own included.
        // It finds the others cancelled already, and theirs never run.
        Fixture f;
        QSignalSpy finished(f.server.get(), &McpServer::finished);
        QVERIFY(f.start());
        const QStringList names{QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")};
        for (qsizetype i = 0; i < names.size(); ++i) {
            QVERIFY(f.callJob(int(i + 1), names.at(i)));
        }
        int runs = 0;
        bool othersCancelled = true;
        for (const QString& name : names) {
            f.jobs.at(name).onCancelled([&, name] {
                ++runs;
                for (const QString& other : names) {
                    if (other != name && !f.jobs.at(other).isCancelled()) {
                        othersCancelled = false;
                    }
                }
                f.jobs.clear();
            });
        }
        f.peer.closeWrite();
        QVERIFY(spinUntil([&] {
            return finished.count() == 1;
        }));
        QCOMPARE(runs, 1);
        QVERIFY(othersCancelled);
        QVERIFY(f.jobs.empty());
        QVERIFY(f.peer.readFor(100).isEmpty());
    }

    void callbackSetDuringTheEndReplacesTheOneNotYetRun()
    {
        // One callback per call, at the end too. The first callback to run
        // gives the other call a new one, which runs at once, since that
        // call is cancelled already. The other call's first callback, which
        // the end has not reached yet, is replaced and never runs. Each call
        // gets exactly one callback, whichever the end reaches first.
        Fixture f;
        QSignalSpy finished(f.server.get(), &McpServer::finished);
        QVERIFY(f.start());
        const QStringList names{QStringLiteral("a"), QStringLiteral("b")};
        for (qsizetype i = 0; i < names.size(); ++i) {
            QVERIFY(f.callJob(int(i + 1), names.at(i)));
        }
        std::map<QString, int> runs;
        for (const QString& name : names) {
            f.jobs.at(name).onCancelled([&, name] {
                ++runs[name];
                for (const QString& other : names) {
                    if (other != name) {
                        f.jobs.at(other).onCancelled([&, other] {
                            ++runs[other];
                        });
                    }
                }
            });
        }
        f.peer.closeWrite();
        QVERIFY(spinUntil([&] {
            return finished.count() == 1;
        }));
        QCOMPARE(runs[QStringLiteral("a")], 1);
        QCOMPARE(runs[QStringLiteral("b")], 1);
    }

    void progress()
    {
        Fixture f;
        QVERIFY(f.start());
        QVERIFY(f.callJob(1, QStringLiteral("a"), metaWithToken(QStringLiteral("tok"))));
        McpCall& call = f.jobs.at(QStringLiteral("a"));
        QVERIFY(call.hasProgressToken());

        QVERIFY(call.progress(1, 10, QStringLiteral("first line")));
        QJsonObject note = f.peer.readOne();
        QCOMPARE(note.value(QLatin1StringView("method")).toString(), QStringLiteral("notifications/progress"));
        QVERIFY(!note.contains(QLatin1StringView("id")));
        QCOMPARE(note.value(QLatin1StringView("params")).toObject(),
                 (QJsonObject{{QLatin1StringView("progressToken"), QStringLiteral("tok")},
                              {QLatin1StringView("progress"), 1},
                              {QLatin1StringView("total"), 10},
                              {QLatin1StringView("message"), QStringLiteral("first line")}}));

        // It must grow, and be finite, and so must the total.
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("does not grow past")));
        QVERIFY(!call.progress(1));
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("does not grow past")));
        QVERIFY(!call.progress(0.5));
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("not a finite number")));
        QVERIFY(!call.progress(std::nan("")));
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("not a finite number")));
        QVERIFY(!call.progress(2, std::numeric_limits<double>::infinity()));
        // The refused 2 was not sent, so 1.5 still grows, and is the next
        // notification.
        QVERIFY(call.progress(1.5));
        note = f.peer.readOne();
        QCOMPARE(note.value(QLatin1StringView("params")).toObject(),
                 (QJsonObject{{QLatin1StringView("progressToken"), QStringLiteral("tok")},
                              {QLatin1StringView("progress"), 1.5}}));

        // Nothing after the result.
        QVERIFY(call.finish(McpResult::text(QStringLiteral("done"))));
        QVERIFY(!call.progress(2));
        const QList<QJsonObject> rest = f.peer.readFor(150);
        QCOMPARE(rest.size(), 1);
        QCOMPARE(rest.first().value(QLatin1StringView("id")), QJsonValue(1));
    }

    void progressNeedsAToken()
    {
        Fixture f;
        QVERIFY(f.start());
        QVERIFY(f.callJob(1, QStringLiteral("none")));
        QVERIFY(!f.jobs.at(QStringLiteral("none")).hasProgressToken());
        QVERIFY(!f.jobs.at(QStringLiteral("none")).progress(1));
        QVERIFY(f.peer.readFor(100).isEmpty());

        // An integer token comes back as an integer.
        QVERIFY(f.callJob(2, QStringLiteral("int"), metaWithToken(42)));
        QVERIFY(f.jobs.at(QStringLiteral("int")).progress(0));
        const QJsonObject note = f.peer.readOne();
        QCOMPARE(note.value(QLatin1StringView("params")).toObject().value(QLatin1StringView("progressToken")),
                 QJsonValue(42));

        QVERIFY(f.jobs.at(QStringLiteral("none")).finish(McpResult::text(QStringLiteral("none"))));
        QVERIFY(f.jobs.at(QStringLiteral("int")).finish(McpResult::text(QStringLiteral("int"))));
        QCOMPARE(f.peer.read(2).size(), 2);
    }

    void droppedCallsAnswerWithAnError()
    {
        Fixture f;
        QVERIFY(f.server->addTool(McpTool(QStringLiteral("drop")), [](McpCall call) {
            Q_UNUSED(call)
        }));
        QVERIFY(f.start());
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("dropped a call")));
        QVERIFY(f.peer.send(callRequest(1, QStringLiteral("drop"))));
        QJsonObject reply = f.peer.readOne();
        QCOMPARE(reply.value(QLatin1StringView("id")), QJsonValue(1));
        QCOMPARE(errorCode(reply), ErrorCode::InternalError);

        // Overwritten by a move: the call it held is abandoned.
        QVERIFY(f.callJob(2, QStringLiteral("a")));
        QVERIFY(f.callJob(3, QStringLiteral("b"), metaWithToken(QStringLiteral("tok"))));
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("dropped a call")));
        f.jobs.at(QStringLiteral("a")) = std::move(f.jobs.at(QStringLiteral("b")));
        reply = f.peer.readOne();
        QCOMPARE(reply.value(QLatin1StringView("id")), QJsonValue(2));
        QCOMPARE(errorCode(reply), ErrorCode::InternalError);

        // The moved-from call holds nothing of either call, and its
        // destruction sends nothing.
        const McpCall& movedFrom = f.jobs.at(QStringLiteral("b"));
        QVERIFY(!movedFrom.isValid());
        QVERIFY(!movedFrom.isCancelled());
        QVERIFY(movedFrom.toolName().isEmpty());
        QVERIFY(movedFrom.arguments().isEmpty());
        QVERIFY(movedFrom.requestId().isNull());
        QVERIFY(movedFrom.requestMeta().isEmpty());
        QVERIFY(!movedFrom.hasProgressToken());
        QVERIFY(!f.jobs.at(QStringLiteral("b")).finish(McpResult::text(QStringLiteral("x"))));
        f.jobs.erase(QStringLiteral("b"));
        QVERIFY(f.peer.readFor(100).isEmpty());
        const McpCall& moved = f.jobs.at(QStringLiteral("a"));
        QCOMPARE(moved.requestId(), QJsonValue(3));
        QVERIFY(moved.hasProgressToken());
        QVERIFY(f.jobs.at(QStringLiteral("a")).finish(McpResult::text(QStringLiteral("b's answer"))));
        reply = f.peer.readOne();
        QCOMPARE(reply.value(QLatin1StringView("id")), QJsonValue(3));
        QCOMPARE(firstText(reply), QStringLiteral("b's answer"));
    }

    void moveAssignOverACallWhoseCallbackOwnsTheSource()
    {
        // The old call's cancel callback owns the call moved in. The move
        // reads it before the old call is abandoned, which destroys it.
        Fixture f;
        QVERIFY(f.start());
        QVERIFY(f.callJob(1, QStringLiteral("a")));
        QVERIFY(f.callJob(2, QStringLiteral("b")));
        auto target = std::make_shared<McpCall>(std::move(f.jobs.at(QStringLiteral("a"))));
        auto source = std::make_shared<McpCall>(std::move(f.jobs.at(QStringLiteral("b"))));
        f.jobs.clear();
        McpCall* sourceCall = source.get();
        target->onCancelled([source] { });
        source.reset();

        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("dropped a call")));
        *target = std::move(*sourceCall);
        QJsonObject reply = f.peer.readOne();
        QCOMPARE(reply.value(QLatin1StringView("id")), QJsonValue(1));
        QCOMPARE(errorCode(reply), ErrorCode::InternalError);
        QCOMPARE(target->requestId(), QJsonValue(2));
        QVERIFY(target->finish(McpResult::text(QStringLiteral("b"))));
        reply = f.peer.readOne();
        QCOMPARE(reply.value(QLatin1StringView("id")), QJsonValue(2));
        QCOMPARE(firstText(reply), QStringLiteral("b"));
    }

    void moveAssignOverACallWhoseCallbackOwnsTheTarget()
    {
        // The old call's cancel callback owns the object assigned to, so
        // abandoning the old call destroys it, and the call moved in with it.
        Fixture f;
        QVERIFY(f.start());
        QVERIFY(f.callJob(1, QStringLiteral("a")));
        QVERIFY(f.callJob(2, QStringLiteral("b")));
        auto target = std::make_shared<McpCall>(std::move(f.jobs.at(QStringLiteral("a"))));
        target->onCancelled([target] { });
        McpCall* targetCall = target.get();
        target.reset();

        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("dropped a call")));
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("dropped a call")));
        *targetCall = std::move(f.jobs.at(QStringLiteral("b")));
        const QList<QJsonObject> replies = f.peer.read(2);
        QCOMPARE(replies.size(), 2);
        QCOMPARE(replies.at(0).value(QLatin1StringView("id")), QJsonValue(1));
        QCOMPARE(errorCode(replies.at(0)), ErrorCode::InternalError);
        QCOMPARE(replies.at(1).value(QLatin1StringView("id")), QJsonValue(2));
        QCOMPARE(errorCode(replies.at(1)), ErrorCode::InternalError);
    }

    void answerBeforeTheCallsItsCallbackOwned()
    {
        // The answered call's cancel callback owns another call. That one is
        // destroyed after the answer goes out, not inside the server's table.
        Fixture f;
        QVERIFY(f.start());
        QVERIFY(f.callJob(1, QStringLiteral("a")));
        QVERIFY(f.callJob(2, QStringLiteral("b")));
        auto owned = std::make_shared<McpCall>(std::move(f.jobs.at(QStringLiteral("b"))));
        f.jobs.at(QStringLiteral("a")).onCancelled([owned] { });
        owned.reset();
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("dropped a call")));
        QVERIFY(f.jobs.at(QStringLiteral("a")).finish(McpResult::text(QStringLiteral("a"))));
        const QList<QJsonObject> replies = f.peer.read(2);
        QCOMPARE(replies.size(), 2);
        QCOMPARE(replies.at(0).value(QLatin1StringView("id")), QJsonValue(1));
        QCOMPARE(firstText(replies.at(0)), QStringLiteral("a"));
        QCOMPARE(replies.at(1).value(QLatin1StringView("id")), QJsonValue(2));
        QCOMPARE(errorCode(replies.at(1)), ErrorCode::InternalError);
    }

    void oneAnswerOnly()
    {
        Fixture f;
        QVERIFY(f.start());
        QVERIFY(f.callJob(1, QStringLiteral("a")));
        McpCall& call = f.jobs.at(QStringLiteral("a"));
        QVERIFY(call.finish(McpResult::text(QStringLiteral("first"))));
        QVERIFY(!call.isValid());
        QVERIFY(!call.isCancelled());
        QVERIFY(!call.finish(McpResult::text(QStringLiteral("second"))));
        QVERIFY(!call.fail(QStringLiteral("third")));
        QCOMPARE(f.peer.readFor(150).size(), 1);
        QVERIFY(!McpCall().finish(McpResult()));
        QVERIFY(!McpCall().isValid());
        QVERIFY(!McpCall().isCancelled());
    }

    void finishIsTrueWhenQueuedNotWhenRead()
    {
        // The client reads nothing more. The first answer is queued, and its
        // write then fails: true said only that it was queued. After that
        // failure nothing more is queued.
        Fixture f;
        QSignalSpy finished(f.server.get(), &McpServer::finished);
        QVERIFY(f.start());
        QVERIFY(f.callJob(1, QStringLiteral("a")));
        QVERIFY(f.callJob(2, QStringLiteral("b")));
        ::shutdown(f.peer.clientFd(), SHUT_RD);
        QVERIFY(f.jobs.at(QStringLiteral("a")).finish(McpResult::text(QStringLiteral("a"))));
        QVERIFY(!f.jobs.at(QStringLiteral("b")).finish(McpResult::text(QStringLiteral("b"))));
        QVERIFY(spinUntil([&] {
            return finished.count() == 1;
        }));
        QCOMPARE(finished.first().first().value<McpServer::FinishReason>(), McpServer::FinishReason::OutputClosed);
    }

    void requestIdsInFlight()
    {
        Fixture f;
        QVERIFY(f.start());
        QVERIFY(f.callJob(1, QStringLiteral("a")));
        // The same id while the first runs is refused. The string "1" is
        // another id.
        QVERIFY(f.peer.send(callRequest(1, QStringLiteral("now"))));
        QJsonObject reply = f.peer.readOne();
        QCOMPARE(errorCode(reply), ErrorCode::InvalidRequest);
        QCOMPARE(reply.value(QLatin1StringView("id")), QJsonValue(1));
        QVERIFY(f.peer.send(callRequest(QStringLiteral("1"), QStringLiteral("now"))));
        QCOMPARE(f.peer.readOne().value(QLatin1StringView("id")), QJsonValue(QStringLiteral("1")));

        QVERIFY(f.jobs.at(QStringLiteral("a")).finish(McpResult::text(QStringLiteral("a"))));
        QCOMPARE(f.peer.readOne().value(QLatin1StringView("id")), QJsonValue(1));
        QVERIFY(f.peer.send(callRequest(1, QStringLiteral("now"))));
        QCOMPARE(firstText(f.peer.readOne()), QStringLiteral("now"));
    }

    void endOfInputCancelsRunningCalls()
    {
        Fixture f;
        QSignalSpy finished(f.server.get(), &McpServer::finished);
        QVERIFY(f.start());
        QVERIFY(f.callJob(1, QStringLiteral("a")));
        int cancelled = 0;
        f.jobs.at(QStringLiteral("a")).onCancelled([&] {
            // Before finished.
            QCOMPARE(finished.count(), 0);
            ++cancelled;
        });
        f.peer.closeWrite();
        QVERIFY(spinUntil([&] {
            return finished.count() == 1;
        }));
        QCOMPARE(cancelled, 1);
        McpCall& call = f.jobs.at(QStringLiteral("a"));
        QVERIFY(!call.finish(McpResult::text(QStringLiteral("late"))));
        QVERIFY(f.peer.readFor(100).isEmpty());
        // It stays cancelled, after the finish and after the server.
        QVERIFY(call.isCancelled());
        f.server.reset();
        QVERIFY(call.isCancelled());
    }

    void endOfInputWaitsForQueuedReplies()
    {
        Fixture f;
        QSignalSpy finished(f.server.get(), &McpServer::finished);
        QVERIFY(f.start());
        QVERIFY(f.callJob(1, QStringLiteral("a")));
        const QString reply(4 * 1024 * 1024, QLatin1Char('r'));
        QVERIFY(f.jobs.at(QStringLiteral("a")).finish(McpResult::text(reply)));
        // The precondition: the reply waits for the client.
        QVERIFY(f.server->transport()->pendingWriteBytes() > 0);
        f.peer.closeWrite();
        // The client has not read: the server waits for it.
        spinFor(200);
        QCOMPARE(finished.count(), 0);
        const QList<QJsonObject> replies = f.peer.read(1, 10000);
        QCOMPARE(replies.size(), 1);
        QCOMPARE(firstText(replies.first()).size(), reply.size());
        QVERIFY(spinUntil([&] {
            return finished.count() == 1;
        }));
        QCOMPARE(finished.first().first().value<McpServer::FinishReason>(), McpServer::FinishReason::EndOfInput);
    }

    void endOfInputGivesUpOnAReaderThatNeverReads()
    {
        Fixture f;
        QSignalSpy finished(f.server.get(), &McpServer::finished);
        QVERIFY(f.start());
        QVERIFY(f.callJob(1, QStringLiteral("a")));
        QVERIFY(f.jobs.at(QStringLiteral("a")).finish(McpResult::text(QString(4 * 1024 * 1024, QLatin1Char('r')))));
        QVERIFY(f.server->transport()->pendingWriteBytes() > 0);
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("read none of its replies")));
        f.peer.closeWrite();
        QElapsedTimer elapsed;
        elapsed.start();
        QVERIFY(spinUntil(
            [&] {
                return finished.count() == 1;
            },
            McpServer::DrainTimeoutMs + 3000));
        QVERIFY(elapsed.elapsed() >= McpServer::DrainTimeoutMs - 100);
        QCOMPARE(finished.first().first().value<McpServer::FinishReason>(), McpServer::FinishReason::EndOfInput);
    }

    void drainWaitsForASlowReader()
    {
        // A client that reads 64 KiB each 50 ms takes longer than
        // DrainTimeoutMs for the whole reply, but never stops reading for
        // that long: it gets the reply whole.
        Fixture f;
        QSignalSpy finished(f.server.get(), &McpServer::finished);
        QVERIFY(f.start());
        QVERIFY(f.callJob(1, QStringLiteral("a")));
        const QString reply(6 * 1024 * 1024, QLatin1Char('r'));
        QVERIFY(f.jobs.at(QStringLiteral("a")).finish(McpResult::text(reply)));
        QVERIFY(f.server->transport()->pendingWriteBytes() > 0);
        f.peer.closeWrite();
        QElapsedTimer elapsed;
        elapsed.start();
        const QDeadlineTimer deadline(30000);
        while (finished.count() == 0 && !deadline.hasExpired()) {
            f.peer.pump(64 * 1024);
            spinFor(50);
        }
        // The precondition: the reading took longer than the timeout.
        QVERIFY2(elapsed.elapsed() > McpServer::DrainTimeoutMs, qPrintable(QString::number(elapsed.elapsed())));
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished.first().first().value<McpServer::FinishReason>(), McpServer::FinishReason::EndOfInput);
        const QList<QJsonObject> replies = f.peer.read(1, 5000);
        QCOMPARE(replies.size(), 1);
        QCOMPARE(firstText(replies.first()).size(), reply.size());
    }

    void outputClosedDuringTheDrain()
    {
        // The client goes away while the replies drain: the server ends at
        // once, for that reason.
        Fixture f;
        QSignalSpy finished(f.server.get(), &McpServer::finished);
        QVERIFY(f.start());
        QVERIFY(f.callJob(1, QStringLiteral("a")));
        QVERIFY(f.jobs.at(QStringLiteral("a")).finish(McpResult::text(QString(4 * 1024 * 1024, QLatin1Char('r')))));
        QVERIFY(f.server->transport()->pendingWriteBytes() > 0);
        f.peer.closeWrite();
        spinFor(100);
        QCOMPARE(finished.count(), 0);
        f.peer.closeClient();
        QElapsedTimer elapsed;
        elapsed.start();
        QVERIFY(spinUntil([&] {
            return finished.count() == 1;
        }));
        QVERIFY(elapsed.elapsed() < McpServer::DrainTimeoutMs);
        QCOMPARE(finished.first().first().value<McpServer::FinishReason>(), McpServer::FinishReason::OutputClosed);
    }

    void inputFailedDrainsTheReplies()
    {
        // A failed read leaves stdout as it was: the replies already queued
        // still go out. The input is a pty master: the request comes from
        // its slave, and once the slave is closed the next read fails with
        // EIO. (A reset socket would not do: ECONNRESET is the client's end,
        // as EOF is.)
        const int master = ::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
        if (master < 0 || ::grantpt(master) != 0 || ::unlockpt(master) != 0) {
            if (master >= 0) {
                ::close(master);
            }
            QSKIP("no pseudo-terminal here");
        }
        const auto closeMaster = qScopeGuard([master] {
            ::close(master);
        });
        int slave = ::open(::ptsname(master), O_RDWR | O_NOCTTY | O_CLOEXEC);
        QVERIFY(slave >= 0);
        const auto closeSlave = qScopeGuard([&slave] {
            if (slave >= 0) {
                ::close(slave);
            }
        });
        Fixture f;
        Peer output;
        QSignalSpy finished(f.server.get(), &McpServer::finished);
        QVERIFY(f.server->start(master, output.serverFd()));
        const QByteArray request =
            QJsonDocument(
                callRequest(1, QStringLiteral("job"), QJsonObject{{QLatin1StringView("job"), QStringLiteral("a")}}))
                .toJson(QJsonDocument::Compact)
            + '\n';
        QCOMPARE(::write(slave, request.constData(), size_t(request.size())), ssize_t(request.size()));
        QVERIFY(spinUntil([&] {
            return f.jobs.count(QStringLiteral("a")) == 1;
        }));
        const QString reply(4 * 1024 * 1024, QLatin1Char('r'));
        QVERIFY(f.jobs.at(QStringLiteral("a")).finish(McpResult::text(reply)));
        QVERIFY(f.server->transport()->pendingWriteBytes() > 0);
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("reading the input failed")));
        ::close(slave);
        slave = -1;
        const QList<QJsonObject> replies = output.read(1, 10000);
        QCOMPARE(replies.size(), 1);
        QCOMPARE(firstText(replies.first()).size(), reply.size());
        QVERIFY(spinUntil([&] {
            return finished.count() == 1;
        }));
        QCOMPARE(finished.first().first().value<McpServer::FinishReason>(), McpServer::FinishReason::InputFailed);
    }

    void callsOutliveTheServer()
    {
        Fixture f;
        QVERIFY(f.start());
        QVERIFY(f.callJob(1, QStringLiteral("a")));
        QVERIFY(f.callJob(2, QStringLiteral("answered")));
        QVERIFY(f.jobs.at(QStringLiteral("answered")).finish(McpResult::text(QStringLiteral("done"))));
        int cancelled = 0;
        f.jobs.at(QStringLiteral("a")).onCancelled([&] {
            ++cancelled;
        });
        f.server.reset();
        McpCall& call = f.jobs.at(QStringLiteral("a"));
        QVERIFY(!call.isValid());
        // The server went while the call ran: it reads cancelled, though no
        // callback ran. An answered call does not.
        QVERIFY(call.isCancelled());
        QVERIFY(!f.jobs.at(QStringLiteral("answered")).isCancelled());
        QVERIFY(!call.finish(McpResult::text(QStringLiteral("late"))));
        QVERIFY(!call.progress(1));
        QCOMPARE(cancelled, 0);
        f.jobs.clear();
    }

    void serverDestroyedWithACallbackThatOwnsACall()
    {
        // The pending table goes before the calls its callbacks own, whose
        // destructors then find nothing to answer.
        Fixture f;
        QVERIFY(f.start());
        QVERIFY(f.callJob(1, QStringLiteral("a")));
        auto owned = std::make_shared<McpCall>(std::move(f.jobs.at(QStringLiteral("a"))));
        f.jobs.clear();
        bool callbackGone = false;
        owned->onCancelled([owned, guard = std::make_shared<DestroyFlag>(&callbackGone)] { });
        McpCall* raw = owned.get();
        owned.reset();
        QVERIFY(raw->isValid());
        f.server.reset();
        // The callback went with the server, and the call it owned with it.
        QVERIFY(callbackGone);
    }

    void serverDestroyedWithAHandlerThatOwnsACall()
    {
        // A handler that keeps its call in its own captures goes in the
        // server's destruction. The call, never answered, reads isCancelled()
        // true from its holder's destructor there. The order in which the
        // server's own state goes cannot be seen on libstdc++, where the
        // emptied table reads as empty even once destroyed. This test and an
        // ASan run guard the outcome.
        struct Holder
        {
            std::optional<McpCall> call;
            bool* cancelledWhenGone = nullptr;
            ~Holder()
            {
                if (call) {
                    *cancelledWhenGone = call->isCancelled();
                }
            }
        };
        Fixture f;
        bool handlerGone = false;
        bool cancelledWhenGone = false;
        Holder* held = nullptr;
        {
            auto holder = std::make_shared<Holder>();
            holder->cancelledWhenGone = &cancelledWhenGone;
            held = holder.get();
            QVERIFY(f.server->addTool(McpTool(QStringLiteral("hold")),
                                      [holder, guard = std::make_shared<DestroyFlag>(&handlerGone)](McpCall call) {
                                          holder->call.emplace(std::move(call));
                                      }));
        }
        QVERIFY(f.start());
        QVERIFY(f.peer.send(callRequest(1, QStringLiteral("hold"))));
        QVERIFY(spinUntil([&] {
            return held->call.has_value();
        }));
        QVERIFY(held->call->isValid());
        QVERIFY(!held->call->isCancelled());
        f.server.reset();
        QVERIFY(handlerGone);
        QVERIFY(cancelledWhenGone);
    }

    void finishACallThatOnlyItsCallbackOwns()
    {
        // The answer takes the call out of the table, and with it the
        // callback that owns the McpCall, which is destroyed while its own
        // finish() is still on the stack. finish() touches nothing after.
        Fixture f;
        QVERIFY(f.start());
        QVERIFY(f.callJob(1, QStringLiteral("a")));
        auto owned = std::make_shared<McpCall>(std::move(f.jobs.at(QStringLiteral("a"))));
        f.jobs.clear();
        bool callbackGone = false;
        owned->onCancelled([owned, guard = std::make_shared<DestroyFlag>(&callbackGone)] { });
        McpCall* raw = owned.get();
        owned.reset();
        QVERIFY(raw->finish(McpResult::text(QStringLiteral("done"))));
        QVERIFY(callbackGone);
        const QJsonObject reply = f.peer.readOne();
        QCOMPARE(reply.value(QLatin1StringView("id")), QJsonValue(1));
        QCOMPARE(firstText(reply), QStringLiteral("done"));
    }

    void contentAndResultShapes()
    {
        const McpContent text = McpContent::text(QStringLiteral("hi"));
        QCOMPARE(text.type(), McpContent::Type::Text);
        QCOMPARE(text.text(), QStringLiteral("hi"));
        QVERIFY(text.bytes().isEmpty());
        QVERIFY(text.mimeType().isEmpty());
        QCOMPARE(text.toJson(),
                 (QJsonObject{{QLatin1StringView("type"), QStringLiteral("text")},
                              {QLatin1StringView("text"), QStringLiteral("hi")}}));

        const QByteArray png("\x89PNG\r\n", 6);
        const McpContent image = McpContent::image(png, QStringLiteral("image/png"));
        QCOMPARE(image.type(), McpContent::Type::Image);
        QCOMPARE(image.bytes(), png);
        QCOMPARE(image.mimeType(), QStringLiteral("image/png"));
        QVERIFY(image.text().isEmpty());
        QCOMPARE(image.toJson(),
                 (QJsonObject{{QLatin1StringView("type"), QStringLiteral("image")},
                              {QLatin1StringView("data"), QString::fromLatin1(png.toBase64())},
                              {QLatin1StringView("mimeType"), QStringLiteral("image/png")}}));

        const QByteArray wav("RIFF", 4);
        const McpContent audio = McpContent::audio(wav, QStringLiteral("audio/wav"));
        QCOMPARE(audio.type(), McpContent::Type::Audio);
        QCOMPARE(audio.toJson(),
                 (QJsonObject{{QLatin1StringView("type"), QStringLiteral("audio")},
                              {QLatin1StringView("data"), QString::fromLatin1(wav.toBase64())},
                              {QLatin1StringView("mimeType"), QStringLiteral("audio/wav")}}));

        QJsonObject link = typed("resource_link");
        link.insert(QLatin1StringView("uri"), QStringLiteral("file:///tmp/x"));
        link.insert(QLatin1StringView("name"), QStringLiteral("x"));
        const McpContent raw = McpContent::fromJson(link);
        QCOMPARE(raw.type(), McpContent::Type::Json);
        QCOMPARE(raw.toJson(), link);

        McpResult result;
        result.addText(QStringLiteral("hi")).addImage(png, QStringLiteral("image/png"));
        result.addAudio(wav, QStringLiteral("audio/wav")).addContent(raw);
        // Blocks a client would refuse are not added.
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("needs bytes and a MIME type")));
        result.addImage(QByteArray(), QStringLiteral("image/png"));
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("needs bytes and a MIME type")));
        result.addAudio(wav, QString());
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("needs a string \"type\"")));
        result.addContent(McpContent::fromJson(QJsonObject{{QLatin1StringView("uri"), QStringLiteral("x")}}));
        QCOMPARE(result.content().size(), 4);
        QCOMPARE(result.content().at(1).type(), McpContent::Type::Image);
        QVERIFY(!result.hasStructuredContent());
        QVERIFY(result.structuredContent().isUndefined());
        QVERIFY(!result.isError());
        const QJsonObject json = result.toJson();
        QCOMPARE(json.value(QLatin1StringView("content")).toArray(),
                 (QJsonArray{text.toJson(), image.toJson(), audio.toJson(), link}));
        QVERIFY(!json.contains(QLatin1StringView("structuredContent")));
        QVERIFY(!json.contains(QLatin1StringView("isError")));

        // Structured content of any JSON type, with its compact text last.
        const McpResult list = McpResult::structured(QJsonArray{1, 2});
        QVERIFY(list.hasStructuredContent());
        QCOMPARE(list.structuredContent(), QJsonValue(QJsonArray{1, 2}));
        QCOMPARE(list.toJson().value(QLatin1StringView("structuredContent")), QJsonValue(QJsonArray{1, 2}));
        QCOMPARE(list.toJson()
                     .value(QLatin1StringView("content"))
                     .toArray()
                     .last()
                     .toObject()
                     .value(QLatin1StringView("text")),
                 QJsonValue(QStringLiteral("[1,2]")));
        QCOMPARE(McpResult::structured(QJsonValue(3)).toJson().value(QLatin1StringView("content")).toArray(),
                 (QJsonArray{McpContent::text(QStringLiteral("3")).toJson()}));
        // An object, as most tools give.
        McpResult object = McpResult::structured(QJsonObject{{QLatin1StringView("n"), 1}});
        QCOMPARE(object.structuredContent(), QJsonValue(QJsonObject{{QLatin1StringView("n"), 1}}));
        object.setStructuredContent(QJsonValue(QJsonValue::Undefined));
        QVERIFY(!object.hasStructuredContent());
        QVERIFY(McpResult::error(QStringLiteral("no")).toJson().value(QLatin1StringView("isError")).toBool());
    }

    void structuredContentAgainstAnOutputSchema()
    {
        Fixture f;
        // Any root type for an output schema.
        const QJsonObject integers{
            {QLatin1StringView("type"), QStringLiteral("array")},
            {QLatin1StringView("items"), typed("integer")},
        };
        QString error;
        QVERIFY2(f.server->addTool(
                     McpTool(QStringLiteral("list")).setInputSchema(typed("object")).setOutputSchema(integers),
                     [](McpCall call) {
                         const QJsonValue value = call.arguments().value(QLatin1StringView("value"));
                         call.finish(McpResult::structured(value));
                     },
                     &error),
                 qPrintable(error));
        QJsonObject counted = typed("object");
        counted.insert(QLatin1StringView("properties"), QJsonObject{{QLatin1StringView("count"), typed("number")}});
        QVERIFY(f.server->addTool(McpTool(QStringLiteral("count")).setOutputSchema(counted), [&f](McpCall call) {
            f.jobs.insert_or_assign(QStringLiteral("count"), std::move(call));
        }));
        QVERIFY(f.server->addTool(McpTool(QStringLiteral("free")), [&f](McpCall call) {
            f.jobs.insert_or_assign(QStringLiteral("free"), std::move(call));
        }));
        QVERIFY(f.start());

        const auto callList = [&](int id, const QJsonValue& value) {
            return f.peer.send(
                       callRequest(id, QStringLiteral("list"), QJsonObject{{QLatin1StringView("value"), value}}))
                ? f.peer.readOne()
                : QJsonObject();
        };
        QJsonObject reply = callList(1, QJsonArray{1, 2});
        QCOMPARE(resultOf(reply).value(QLatin1StringView("structuredContent")), QJsonValue(QJsonArray{1, 2}));
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("does not match its output schema")));
        reply = callList(2, QJsonArray{QStringLiteral("x")});
        QCOMPARE(errorCode(reply), ErrorCode::InternalError);

        const auto callCount = [&](int id) {
            return f.peer.send(callRequest(id, QStringLiteral("count"))) && spinUntil([&] {
                       return f.jobs.count(QStringLiteral("count")) == 1;
                   });
        };
        // A failed result is checked too when it has structured content.
        QVERIFY(callCount(3));
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("does not match its output schema")));
        QVERIFY(!f.jobs.at(QStringLiteral("count"))
                     .finish(McpResult::error(QStringLiteral("bad"))
                                 .setStructuredContent(QJsonObject{{QLatin1StringView("count"), true}})));
        reply = f.peer.readOne();
        QCOMPARE(reply.value(QLatin1StringView("id")), QJsonValue(3));
        QCOMPARE(errorCode(reply), ErrorCode::InternalError);
        f.jobs.erase(QStringLiteral("count"));
        // A failed result without structured content need not have it.
        QVERIFY(callCount(4));
        QVERIFY(f.jobs.at(QStringLiteral("count")).fail(QStringLiteral("no count")));
        reply = f.peer.readOne();
        QCOMPARE(reply.value(QLatin1StringView("id")), QJsonValue(4));
        QVERIFY(resultOf(reply).value(QLatin1StringView("isError")).toBool());
        f.jobs.erase(QStringLiteral("count"));

        // A number that is not finite matches "number" for the validator,
        // and would go out as null: refused, with its place.
        QVERIFY(callCount(5));
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("does not match its output schema")));
        QVERIFY(!f.jobs.at(QStringLiteral("count"))
                     .finish(McpResult::structured(
                         QJsonObject{{QLatin1StringView("count"), std::numeric_limits<double>::infinity()}})));
        reply = f.peer.readOne();
        QCOMPARE(reply.value(QLatin1StringView("id")), QJsonValue(5));
        QCOMPARE(errorCode(reply), ErrorCode::InternalError);
        const QJsonArray errors =
            errorOf(reply).value(QLatin1StringView("data")).toObject().value(QLatin1StringView("errors")).toArray();
        QVERIFY2(errors.first().toString().startsWith(QLatin1StringView("structuredContent/count: ")),
                 qPrintable(errors.first().toString()));
        QVERIFY(errors.first().toString().contains(QLatin1StringView("not finite")));
        f.jobs.erase(QStringLiteral("count"));

        // Without a schema it goes out, as null, with a warning.
        QVERIFY(f.peer.send(callRequest(6, QStringLiteral("free"))));
        QVERIFY(spinUntil([&] {
            return f.jobs.count(QStringLiteral("free")) == 1;
        }));
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("not finite, which go out as null")));
        QVERIFY(f.jobs.at(QStringLiteral("free"))
                    .finish(McpResult::structured(QJsonObject{{QLatin1StringView("a~/b"), QJsonArray{std::nan("")}}})));
        reply = f.peer.readOne();
        QCOMPARE(reply.value(QLatin1StringView("id")), QJsonValue(6));
        QCOMPARE(resultOf(reply).value(QLatin1StringView("structuredContent")).toObject(),
                 (QJsonObject{{QLatin1StringView("a~/b"), QJsonArray{QJsonValue::Null}}}));
    }

    void loneSurrogatesBecomeReplacementCharacters()
    {
        // A lone surrogate has no UTF-8 form. It goes out as U+FFFD, not as
        // an escape that strict parsers refuse. A backslash followed by the
        // text of such an escape is text, and stays.
        Fixture f;
        QVERIFY(f.start());
        QVERIFY(f.callJob(1, QStringLiteral("a")));
        QString text;
        text += QChar(0xD83D);
        text += QStringLiteral("x\\ud83d");
        text += QChar(0xDE00);
        text += QString::fromUtf8("\xF0\x9F\x98\x80");
        QVERIFY(f.jobs.at(QStringLiteral("a")).finish(McpResult::text(text)));
        QString expected;
        expected += QChar(0xFFFD);
        expected += QStringLiteral("x\\ud83d");
        expected += QChar(0xFFFD);
        expected += QString::fromUtf8("\xF0\x9F\x98\x80");
        QCOMPARE(firstText(f.peer.readOne()), expected);
    }

    void loneSurrogateKeysDoNotRepeat()
    {
        // Two keys that differ only in an unpaired surrogate both become
        // "a�". The line holds that key once, not twice. Peer's parser
        // would hide a repeat, so the raw line is read.
        Fixture f;
        QVERIFY(f.start());
        QVERIFY(f.callJob(1, QStringLiteral("a")));
        QString first = QStringLiteral("a");
        first += QChar(0xD800);
        QString second = QStringLiteral("a");
        second += QChar(0xD801);
        const QJsonObject value{{first, 1}, {second, 2}};
        QVERIFY(f.jobs.at(QStringLiteral("a")).finish(McpResult::structured(value)));
        QByteArray raw;
        QVERIFY(spinUntil([&] {
            char buffer[4096];
            const ssize_t got = ::recv(f.peer.clientFd(), buffer, sizeof buffer, MSG_DONTWAIT);
            if (got > 0) {
                raw.append(buffer, got);
            }
            return raw.contains('\n');
        }));
        // In the structuredContent object the key is written bare. In the
        // text block, which is a JSON string, its quotes are escaped. A key
        // rebuilt in the object goes out as UTF-8, and one fixed only on the
        // line as the escape �: both spellings are counted.
        const QByteArray asUtf8 = "\"a" + QString(QChar(QChar::ReplacementCharacter)).toUtf8() + "\":";
        const QByteArray asEscape = "\"a\\ufffd\":";
        QCOMPARE(raw.count(asUtf8) + raw.count(asEscape), 1);
    }

    void offTheServersThreadACallDoesNothing()
    {
#ifndef QT_NO_DEBUG
        QSKIP("In a debug build, a call used off the server's thread asserts");
#else
        Fixture f;
        QVERIFY(f.start());
        QVERIFY(f.callJob(1, QStringLiteral("a")));
        McpCall& call = f.jobs.at(QStringLiteral("a"));
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("off the server's thread")));
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("off the server's thread")));
        bool finished = true;
        bool valid = true;
        std::thread([&] {
            finished = call.finish(McpResult::text(QStringLiteral("off")));
            valid = call.isValid();
        }).join();
        QVERIFY(!finished);
        QVERIFY(!valid);
        QVERIFY(f.peer.readFor(100).isEmpty());
        // On its thread the call still answers.
        QVERIFY(call.finish(McpResult::text(QStringLiteral("on"))));
        QCOMPARE(firstText(f.peer.readOne()), QStringLiteral("on"));
#endif
    }
};

QTEST_GUILESS_MAIN(TestPhosphorMcpCalls)
#include "test_phosphor_mcp_calls.moc"
