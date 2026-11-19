// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// Calls that answer later: completion out of order, a slow reader of a large
// reply, cancellation, progress, calls dropped unanswered, request ids, and
// the end of the input while calls run.

#include "mcptesthelpers.h"

#include <PhosphorMcp/McpProtocol.h>
#include <PhosphorMcp/McpServer.h>
#include <PhosphorMcp/McpTransport.h>

#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>

#include <cmath>
#include <map>
#include <memory>
#include <type_traits>

using namespace PhosphorMcp;
using namespace PhosphorMcpTests;

static_assert(!std::is_copy_constructible_v<McpCall>);
static_assert(!std::is_copy_assignable_v<McpCall>);
static_assert(std::is_nothrow_move_constructible_v<McpCall>);
static_assert(std::is_nothrow_move_assignable_v<McpCall>);

// Named, not anonymous: the test class's lambdas capture a Fixture, and a
// type from an anonymous namespace in a member of a class outside it trips
// -Wsubobject-linkage.
namespace PhosphorMcpCallsTest {

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
};

QJsonObject metaWithToken(const QJsonValue& token)
{
    QJsonObject m = meta();
    m.insert(MetaProgressToken, token);
    return m;
}

} // namespace PhosphorMcpCallsTest

using namespace PhosphorMcpCallsTest;

class TestPhosphorMcpCalls : public QObject
{
    Q_OBJECT

private Q_SLOTS:
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
        McpTransport* transport = f.server->transport();
        QVERIFY(transport);
        QVERIFY(transport->pendingWriteBytes() > 8 * 1024 * 1024);

        // Nothing read yet: the next call still runs, and timers still fire.
        QVERIFY(f.peer.send(callRequest(2, QStringLiteral("now"))));
        int ticks = 0;
        QTimer timer;
        connect(&timer, &QTimer::timeout, this, [&] {
            ++ticks;
        });
        timer.start(1);
        QVERIFY(spinUntil([&] {
            return f.nowCalls == 1;
        }));
        const qint64 readWhenAnswered = f.peer.bytesRead();
        QCOMPARE(readWhenAnswered, 0);

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
            60000));
        QVERIFY(ticks > 5);
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
        // near the mark; when the client reads, the rest are read and
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
        const QList<QJsonObject> replies = f.peer.read(Calls + 1, 30000);
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
        QVERIFY(f.callJob(5, QStringLiteral("a")));
        McpCall& call = f.jobs.at(QStringLiteral("a"));
        int cancelled = 0;
        call.onCancelled([&] {
            ++cancelled;
        });
        QVERIFY(call.isValid());
        QVERIFY(!call.isCancelled());

        QVERIFY(f.peer.send(cancelNotification(5)));
        QVERIFY(spinUntil([&] {
            return cancelled == 1;
        }));
        QVERIFY(call.isCancelled());
        QVERIFY(!call.isValid());
        // Nothing more for the request: no reply, no progress, ever.
        QVERIFY(!call.finish(McpResult::text(QStringLiteral("late"))));
        QVERIFY(!call.fail(QStringLiteral("late")));
        QVERIFY(!call.progress(1));
        QVERIFY(f.peer.readFor(150).isEmpty());

        // A callback set after the cancellation runs at once.
        int late = 0;
        call.onCancelled([&] {
            ++late;
        });
        QCOMPARE(late, 1);
        // A second cancel of the same request changes nothing.
        QVERIFY(f.peer.send(cancelNotification(5)));
        spinFor(50);
        QCOMPARE(cancelled, 1);

        // Its id is free again.
        f.jobs.clear();
        QVERIFY(f.callJob(5, QStringLiteral("again")));
        QVERIFY(f.jobs.at(QStringLiteral("again")).finish(McpResult::text(QStringLiteral("ok"))));
        QCOMPARE(f.peer.readOne().value(QLatin1StringView("id")), QJsonValue(5));
    }

    void cancelsThatMatchNothingAreIgnored()
    {
        Fixture f;
        QVERIFY(f.start());
        QVERIFY(f.callJob(QStringLiteral("s1"), QStringLiteral("a")));
        int cancelled = 0;
        f.jobs.at(QStringLiteral("a")).onCancelled([&] {
            ++cancelled;
        });
        // Another id, the same id as a number, no id, and a call that
        // already answered.
        QVERIFY(f.peer.send(cancelNotification(99)));
        QVERIFY(f.peer.send(cancelNotification(QStringLiteral("s2"))));
        QVERIFY(f.peer.send(notification(QStringLiteral("notifications/cancelled"))));
        QVERIFY(f.peer.send(callRequest(7, QStringLiteral("now"))));
        QCOMPARE(f.peer.readOne().value(QLatin1StringView("id")), QJsonValue(7));
        QVERIFY(f.peer.send(cancelNotification(7)));
        QVERIFY(f.peer.readFor(100).isEmpty());
        QCOMPARE(cancelled, 0);
        QVERIFY(f.jobs.at(QStringLiteral("a")).finish(McpResult::text(QStringLiteral("ok"))));
        QCOMPARE(f.peer.readOne().value(QLatin1StringView("id")), QJsonValue(QStringLiteral("s1")));
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

        // It must grow.
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("progress must grow")));
        QVERIFY(!call.progress(1));
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("progress must grow")));
        QVERIFY(!call.progress(0.5));
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("not a finite number")));
        QVERIFY(!call.progress(std::nan("")));
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
        QVERIFY(f.callJob(3, QStringLiteral("b")));
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("dropped a call")));
        f.jobs.at(QStringLiteral("a")) = std::move(f.jobs.at(QStringLiteral("b")));
        reply = f.peer.readOne();
        QCOMPARE(reply.value(QLatin1StringView("id")), QJsonValue(2));
        QCOMPARE(errorCode(reply), ErrorCode::InternalError);

        // The moved-from call holds nothing, and its destruction sends
        // nothing.
        QVERIFY(!f.jobs.at(QStringLiteral("b")).isValid());
        QVERIFY(!f.jobs.at(QStringLiteral("b")).finish(McpResult::text(QStringLiteral("x"))));
        f.jobs.erase(QStringLiteral("b"));
        QVERIFY(f.peer.readFor(100).isEmpty());
        QVERIFY(f.jobs.at(QStringLiteral("a")).finish(McpResult::text(QStringLiteral("b's answer"))));
        reply = f.peer.readOne();
        QCOMPARE(reply.value(QLatin1StringView("id")), QJsonValue(3));
        QCOMPARE(firstText(reply), QStringLiteral("b's answer"));
    }

    void oneAnswerOnly()
    {
        Fixture f;
        QVERIFY(f.start());
        QVERIFY(f.callJob(1, QStringLiteral("a")));
        McpCall& call = f.jobs.at(QStringLiteral("a"));
        QVERIFY(call.finish(McpResult::text(QStringLiteral("first"))));
        QVERIFY(!call.isValid());
        QVERIFY(!call.finish(McpResult::text(QStringLiteral("second"))));
        QVERIFY(!call.fail(QStringLiteral("third")));
        QCOMPARE(f.peer.readFor(150).size(), 1);
        QVERIFY(!McpCall().finish(McpResult()));
        QVERIFY(!McpCall().isValid());
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
        QVERIFY(!f.jobs.at(QStringLiteral("a")).finish(McpResult::text(QStringLiteral("late"))));
        QVERIFY(f.peer.readFor(100).isEmpty());
    }

    void endOfInputWaitsForQueuedReplies()
    {
        Fixture f;
        QSignalSpy finished(f.server.get(), &McpServer::finished);
        QVERIFY(f.start());
        QVERIFY(f.callJob(1, QStringLiteral("a")));
        const QString reply(4 * 1024 * 1024, QLatin1Char('r'));
        QVERIFY(f.jobs.at(QStringLiteral("a")).finish(McpResult::text(reply)));
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
        f.peer.closeWrite();
        QElapsedTimer elapsed;
        elapsed.start();
        QVERIFY(spinUntil(
            [&] {
                return finished.count() == 1;
            },
            McpServer::DrainTimeoutMs + 3000));
        QVERIFY(elapsed.elapsed() >= McpServer::DrainTimeoutMs - 100);
    }

    void callsOutliveTheServer()
    {
        Fixture f;
        QVERIFY(f.start());
        QVERIFY(f.callJob(1, QStringLiteral("a")));
        int cancelled = 0;
        f.jobs.at(QStringLiteral("a")).onCancelled([&] {
            ++cancelled;
        });
        f.server.reset();
        McpCall& call = f.jobs.at(QStringLiteral("a"));
        QVERIFY(!call.isValid());
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
        owned->onCancelled([owned] { });
        McpCall* raw = owned.get();
        owned.reset();
        QVERIFY(raw->isValid());
        f.server.reset();
        QVERIFY(f.peer.readFor(100).isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestPhosphorMcpCalls)
#include "test_phosphor_mcp_calls.moc"
