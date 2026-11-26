// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: LGPL-2.1-or-later

// IpcClient against a real IpcRouter on a socket in a temporary directory,
// both in the test's thread: every answer comes while the event loop runs.
// What a router never does (a held connection, a frame in two parts, a
// line that is no frame) comes from a RawServer that the test drives.

#include "ipctesthelpers.h"

#include <PhosphorIpc/IpcCallContext.h>
#include <PhosphorIpc/IpcClient.h>
#include <PhosphorIpc/IpcPendingReply.h>
#include <PhosphorIpc/IpcProtocol.h>
#include <PhosphorIpc/IpcRouter.h>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QtCore/qtclasshelpermacros.h>

#include <functional>
#include <limits>
#include <list>
#include <memory>
#include <optional>
#include <utility>

using namespace PhosphorIpc;
using PhosphorIpcTests::RawServer;
using PhosphorIpcTests::RouterFixture;

namespace {

class Calc : public QObject, protected IpcCallContext
{
    Q_OBJECT
public:
    explicit Calc(IpcRouter& router)
        : m_router(router)
    {
    }
    Q_DISABLE_COPY_MOVE(Calc)

    Q_INVOKABLE int sum(int a, int b)
    {
        return a + b;
    }
    Q_INVOKABLE int size(const QString& text)
    {
        return int(text.size());
    }
    Q_INVOKABLE QString open(const QString& name)
    {
        sendErrorReply(QStringLiteral("no project %1").arg(name));
        return name;
    }
    // Answers when the test says so, through the pending reply it keeps.
    Q_INVOKABLE void later(int n)
    {
        m_waiting.push_back(Waiting{n, deferReply()});
    }
    // Sends an event to its caller ahead of the reply.
    Q_INVOKABLE QString install(const QString& name)
    {
        m_router.sendEvent(connectionId(), QStringLiteral("calc"), QStringLiteral("output"),
                           QJsonArray{QStringLiteral("fetching %1").arg(name)});
        return QStringLiteral("installed");
    }
    // Sends an event to the subscribers of countChanged ahead of the reply.
    Q_INVOKABLE void announce(int count)
    {
        m_router.broadcastEvent(QStringLiteral("calc"), QStringLiteral("countChanged"), QJsonArray{count});
    }

    struct Waiting
    {
        int n = 0;
        IpcPendingReply reply;
    };
    std::list<Waiting> m_waiting;

Q_SIGNALS:
    void countChanged(int count);
    void output(const QString& line);

private:
    IpcRouter& m_router;
};

// The replies of one client, in the order their callbacks ran.
struct Replies
{
    QList<qint64> ids;
    QList<IpcReply> replies;

    IpcClient::Callback take(qint64* idSlot)
    {
        return [this, idSlot](const IpcReply& reply) {
            ids.append(*idSlot);
            replies.append(reply);
        };
    }
};

// One answer, and whether it came.
struct Answer
{
    bool came = false;
    IpcReply reply;

    IpcClient::Callback take()
    {
        return [this](const IpcReply& answer) {
            came = true;
            reply = answer;
        };
    }
};

// A router with the calc target, and a client connected to it.
struct Setup
{
    RouterFixture fixture;
    Calc calc{fixture.router};
    IpcClient client;
    // Why start() failed.
    QString problem;

    bool start()
    {
        if (!fixture.valid()) {
            problem = QStringLiteral("no temporary directory");
            return false;
        }
        if (!fixture.router.registerTarget(QStringLiteral("calc"), &calc) || !fixture.router.start(fixture.sockPath)) {
            problem = QStringLiteral("the router did not start");
            return false;
        }
        QSignalSpy connected(&client, &IpcClient::connected);
        QSignalSpy closed(&client, &IpcClient::closed);
        if (!client.connectToServer(fixture.sockPath)) {
            problem = QStringLiteral("connectToServer() refused");
            return false;
        }
        if (connected.isEmpty() && !connected.wait(2000)) {
            problem = closed.isEmpty() ? QStringLiteral("no connection within 2 s") : closed.first().first().toString();
            return false;
        }
        return true;
    }
};

// A client connected to a RawServer.
struct RawSetup
{
    RawServer server;
    IpcClient client;
    // Why start() failed.
    QString problem;

    bool start()
    {
        if (!server.listen()) {
            problem = QStringLiteral("the raw server does not listen");
            return false;
        }
        QSignalSpy connected(&client, &IpcClient::connected);
        if (!client.connectToServer(server.path())) {
            problem = QStringLiteral("connectToServer() refused");
            return false;
        }
        // accept() spins the event loop, in which connected() may come.
        if (!server.accept()) {
            problem = QStringLiteral("no connection reached the raw server");
            return false;
        }
        if (connected.isEmpty() && !connected.wait(2000)) {
            problem = QStringLiteral("no connected() within 2 s");
            return false;
        }
        return true;
    }

    // The request that the client made last, as the server read it.
    bool takeRequest(qint64 id)
    {
        const std::optional<QJsonObject> request = server.readRequest();
        return request && request->value(QLatin1String("id")) == QJsonValue(id);
    }
};

// The line of a call as the client writes it, to know its size.
QByteArray callLine(qint64 id, const QString& target, const QString& fn, const QJsonArray& args)
{
    return writeLine(QJsonObject{{QStringLiteral("type"), QStringLiteral("call")},
                                 {QStringLiteral("id"), id},
                                 {QStringLiteral("target"), target},
                                 {QStringLiteral("fn"), fn},
                                 {QStringLiteral("args"), args}});
}

// Calls the client when it goes away, as an object that a callback holds
// can do from its destructor.
struct CallsTheClientAtItsEnd
{
    CallsTheClientAtItsEnd(IpcClient* client, bool* clientIsGone, int* wentWhileTheClientLived)
        : client(client)
        , clientIsGone(clientIsGone)
        , wentWhileTheClientLived(wentWhileTheClientLived)
    {
    }
    CallsTheClientAtItsEnd(const CallsTheClientAtItsEnd&) = delete;
    CallsTheClientAtItsEnd& operator=(const CallsTheClientAtItsEnd&) = delete;

    IpcClient* client = nullptr;
    bool* clientIsGone = nullptr;
    int* wentWhileTheClientLived = nullptr;

    ~CallsTheClientAtItsEnd()
    {
        if (!*clientIsGone) {
            ++*wentWhileTheClientLived;
        }
        client->unsubscribe(1);
        client->call(QStringLiteral("t"), QStringLiteral("f"), QJsonArray(), [](const IpcReply&) { });
        client->close();
        client->connectToServer(QStringLiteral("/nonexistent/phosphor-ipc-test.sock"));
    }
};

// An object for a callback to hold, which runs a function at its end.
struct RunsAtItsEnd
{
    explicit RunsAtItsEnd(std::function<void()> atEnd)
        : atEnd(std::move(atEnd))
    {
    }
    RunsAtItsEnd(const RunsAtItsEnd&) = delete;
    RunsAtItsEnd& operator=(const RunsAtItsEnd&) = delete;
    ~RunsAtItsEnd()
    {
        atEnd();
    }

    std::function<void()> atEnd;
};

} // namespace

class TestPhosphorIpcClient : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void callGetsItsReply()
    {
        Setup setup;
        QVERIFY2(setup.start(), qPrintable(setup.problem));
        QCOMPARE(setup.client.state(), IpcClient::State::Connected);
        QCOMPARE(setup.client.serverPath(), setup.fixture.sockPath);

        Answer answer;
        setup.client.call(QStringLiteral("calc"), QStringLiteral("sum"), QJsonArray{2, 3}, answer.take());
        QTRY_VERIFY(answer.came);
        QVERIFY(!answer.reply.isError());
        QVERIFY(!answer.reply.fromClient);
        QCOMPARE(answer.reply.result, QJsonValue(5));
    }

    void errorsCarryCodeAndMessage()
    {
        Setup setup;
        QVERIFY2(setup.start(), qPrintable(setup.problem));

        QList<IpcReply> got;
        const auto keep = [&](const IpcReply& reply) {
            got.append(reply);
        };
        setup.client.call(QStringLiteral("calc"), QStringLiteral("open"), QJsonArray{QStringLiteral("x")}, keep);
        setup.client.call(QStringLiteral("calc"), QStringLiteral("nothing"), QJsonArray(), keep);
        setup.client.call(QStringLiteral("calc"), QStringLiteral("sum"), QJsonArray{QStringLiteral("a"), 2}, keep);
        QTRY_COMPARE(got.size(), 3);
        QCOMPARE(got.at(0).errorCode, QString::fromLatin1(ErrorCode::CallFailed));
        QCOMPARE(got.at(0).errorMessage, QStringLiteral("no project x"));
        QVERIFY(got.at(0).result.isNull());
        // The server's errors, not the client's.
        QVERIFY(!got.at(0).fromClient);
        QCOMPARE(got.at(1).errorCode, QString::fromLatin1(ErrorCode::NoSuchFn));
        QCOMPARE(got.at(2).errorCode, QString::fromLatin1(ErrorCode::InvalidArg));
    }

    void callsInFlightAnswerInAnyOrder()
    {
        Setup setup;
        QVERIFY2(setup.start(), qPrintable(setup.problem));

        Replies replies;
        qint64 first = 0;
        qint64 second = 0;
        qint64 third = 0;
        first = setup.client.call(QStringLiteral("calc"), QStringLiteral("later"), QJsonArray{1}, replies.take(&first));
        second =
            setup.client.call(QStringLiteral("calc"), QStringLiteral("later"), QJsonArray{2}, replies.take(&second));
        third =
            setup.client.call(QStringLiteral("calc"), QStringLiteral("sum"), QJsonArray{20, 22}, replies.take(&third));
        // Ids only grow.
        QVERIFY(first > 0);
        QCOMPARE(second, first + 1);
        QCOMPARE(third, second + 1);

        // The call that is not deferred answers first.
        QTRY_COMPARE(replies.ids.size(), 1);
        QCOMPARE(replies.ids.at(0), third);
        QCOMPARE(replies.replies.at(0).result, QJsonValue(42));

        QCOMPARE(setup.calc.m_waiting.size(), std::size_t(2));
        // Then the second, then the first.
        QVERIFY(setup.calc.m_waiting.back().reply.finish(QJsonValue(QStringLiteral("two"))));
        QTRY_COMPARE(replies.ids.size(), 2);
        QVERIFY(setup.calc.m_waiting.front().reply.finish(QJsonValue(QStringLiteral("one"))));
        QTRY_COMPARE(replies.ids.size(), 3);
        QCOMPARE(replies.ids, (QList<qint64>{third, second, first}));
        QCOMPARE(replies.replies.at(1).result, QJsonValue(QStringLiteral("two")));
        QCOMPARE(replies.replies.at(2).result, QJsonValue(QStringLiteral("one")));
    }

    void listAndSchema()
    {
        Setup setup;
        QVERIFY2(setup.start(), qPrintable(setup.problem));

        Answer listed;
        Answer schema;
        Answer missing;
        setup.client.list(listed.take());
        setup.client.schema(QStringLiteral("calc"), schema.take());
        setup.client.schema(QStringLiteral("none"), missing.take());
        QTRY_VERIFY(missing.came);
        QCOMPARE(missing.reply.errorCode, QString::fromLatin1(ErrorCode::NoSuchTarget));
        QVERIFY(listed.came);
        QCOMPARE(listed.reply.result, QJsonValue(QJsonArray{QStringLiteral("calc")}));

        QVERIFY(schema.came);
        const QJsonObject object = schema.reply.result.toObject();
        QCOMPARE(object.value(QLatin1String("target")).toString(), QStringLiteral("calc"));
        QJsonObject sum;
        const QJsonArray functions = object.value(QLatin1String("functions")).toArray();
        for (const QJsonValue function : functions) {
            if (function.toObject().value(QLatin1String("name")).toString() == QLatin1String("sum")) {
                sum = function.toObject();
            }
        }
        const QJsonArray params = sum.value(QLatin1String("params")).toArray();
        QCOMPARE(params.size(), 2);
        QCOMPARE(params.at(0).toObject().value(QLatin1String("name")).toString(), QStringLiteral("a"));
        QCOMPARE(params.at(0).toObject().value(QLatin1String("type")).toString(), QStringLiteral("integer"));
    }

    void requestFromInsideACallback()
    {
        Setup setup;
        QVERIFY2(setup.start(), qPrintable(setup.problem));

        // The second inner request has no target, so the client fails it
        // itself. That failure, too, must come from the event loop and not
        // from inside the call that made the request.
        Answer inner;
        Answer refused;
        bool cameInside = true;
        setup.client.call(QStringLiteral("calc"), QStringLiteral("sum"), QJsonArray{1, 2}, [&](const IpcReply&) {
            setup.client.call(QStringLiteral("calc"), QStringLiteral("sum"), QJsonArray{3, 4}, inner.take());
            setup.client.call(QString(), QStringLiteral("sum"), QJsonArray(), refused.take());
            cameInside = inner.came || refused.came;
        });
        QTRY_VERIFY(inner.came && refused.came);
        QVERIFY(!cameInside);
        QCOMPARE(inner.reply.result, QJsonValue(7));
        QCOMPARE(refused.reply.errorCode, QString::fromLatin1(ClientErrorCode::InvalidRequest));
    }

    void subscriptionStreamsEvents()
    {
        Setup setup;
        QVERIFY2(setup.start(), qPrintable(setup.problem));
        QSignalSpy events(&setup.client, &IpcClient::eventReceived);

        Answer acknowledged;
        const qint64 subscription =
            setup.client.subscribe(QStringLiteral("calc"), QStringLiteral("countChanged"), acknowledged.take());
        QTRY_VERIFY(acknowledged.came);
        QVERIFY(!acknowledged.reply.isError());

        setup.fixture.router.broadcastEvent(QStringLiteral("calc"), QStringLiteral("countChanged"), QJsonArray{7});
        setup.fixture.router.broadcastEvent(QStringLiteral("calc"), QStringLiteral("countChanged"), QJsonArray{8});
        QTRY_COMPARE(events.size(), 2);
        QCOMPARE(events.at(0).at(0).toLongLong(), subscription);
        QCOMPARE(events.at(0).at(1).toString(), QStringLiteral("calc"));
        QCOMPARE(events.at(0).at(2).toString(), QStringLiteral("countChanged"));
        QCOMPARE(events.at(0).at(3).toJsonArray(), QJsonArray{7});
        QCOMPARE(events.at(1).at(3).toJsonArray(), QJsonArray{8});

        // No event after unsubscribe(), also one already on its way.
        setup.fixture.router.broadcastEvent(QStringLiteral("calc"), QStringLiteral("countChanged"), QJsonArray{9});
        Answer unsubscribed;
        setup.client.unsubscribe(subscription, unsubscribed.take());
        QTRY_VERIFY(unsubscribed.came);
        QVERIFY(!unsubscribed.reply.isError());
        QVERIFY(!setup.fixture.router.hasSubscribers(QStringLiteral("calc"), QStringLiteral("countChanged")));
        QCOMPARE(events.size(), 2);

        // The router refuses a signal that the target does not have.
        Answer refused;
        setup.client.subscribe(QStringLiteral("calc"), QStringLiteral("nothing"), refused.take());
        QTRY_VERIFY(refused.came);
        QCOMPARE(refused.reply.errorCode, QString::fromLatin1(ErrorCode::NoSuchSignal));
    }

    void unsubscribeBeforeTheAnswerStartsNoStream()
    {
        // The subscribe, a call that makes the target send an event, and
        // the unsubscribe go out in one turn. The router answers in order:
        // the subscription, the event, the call. The client has ended the
        // subscription by then, and must not show the event.
        Setup setup;
        QVERIFY2(setup.start(), qPrintable(setup.problem));
        QSignalSpy events(&setup.client, &IpcClient::eventReceived);

        Answer subscribed;
        Answer announced;
        Answer unsubscribed;
        const qint64 subscription =
            setup.client.subscribe(QStringLiteral("calc"), QStringLiteral("countChanged"), subscribed.take());
        setup.client.call(QStringLiteral("calc"), QStringLiteral("announce"), QJsonArray{5}, announced.take());
        setup.client.unsubscribe(subscription, unsubscribed.take());
        QTRY_VERIFY(unsubscribed.came);
        QVERIFY(subscribed.came);
        QVERIFY(!subscribed.reply.isError());
        QVERIFY(announced.came);
        QVERIFY(!announced.reply.isError());
        QVERIFY(!unsubscribed.reply.isError());
        QCOMPARE(events.size(), 0);
    }

    void eventsToTheCallerHaveNoSubscription()
    {
        Setup setup;
        QVERIFY2(setup.start(), qPrintable(setup.problem));
        QSignalSpy events(&setup.client, &IpcClient::eventReceived);

        int eventsBeforeReply = -1;
        IpcReply got;
        setup.client.call(QStringLiteral("calc"), QStringLiteral("install"), QJsonArray{QStringLiteral("foo")},
                          [&](const IpcReply& reply) {
                              eventsBeforeReply = int(events.size());
                              got = reply;
                          });
        QTRY_COMPARE(eventsBeforeReply, 1);
        QCOMPARE(got.result, QJsonValue(QStringLiteral("installed")));
        QCOMPARE(events.at(0).at(0).toLongLong(), 0);
        QCOMPARE(events.at(0).at(1).toString(), QStringLiteral("calc"));
        QCOMPARE(events.at(0).at(2).toString(), QStringLiteral("output"));
        QCOMPARE(events.at(0).at(3).toJsonArray(), QJsonArray{QStringLiteral("fetching foo")});
    }

    void targetGoneEndsSubscription()
    {
        Setup setup;
        QVERIFY2(setup.start(), qPrintable(setup.problem));
        QSignalSpy ended(&setup.client, &IpcClient::subscriptionEnded);

        Answer subscribed;
        const qint64 subscription =
            setup.client.subscribe(QStringLiteral("calc"), QStringLiteral("countChanged"), subscribed.take());
        QTRY_VERIFY(subscribed.came);
        setup.fixture.router.unregisterTarget(QStringLiteral("calc"));
        QTRY_COMPARE(ended.size(), 1);
        QCOMPARE(ended.at(0).at(0).toLongLong(), subscription);
        const IpcReply error = ended.at(0).at(1).value<IpcReply>();
        QCOMPARE(error.errorCode, QString::fromLatin1(ErrorCode::NoSuchTarget));
        QVERIFY(!error.fromClient);
    }

    void serverGoingAwayFailsPendingCalls()
    {
        Setup setup;
        QVERIFY2(setup.start(), qPrintable(setup.problem));
        QSignalSpy closed(&setup.client, &IpcClient::closed);
        QSignalSpy ended(&setup.client, &IpcClient::subscriptionEnded);

        Answer subscribed;
        setup.client.subscribe(QStringLiteral("calc"), QStringLiteral("countChanged"), subscribed.take());
        QList<IpcReply> got;
        const auto keep = [&](const IpcReply& reply) {
            got.append(reply);
        };
        setup.client.call(QStringLiteral("calc"), QStringLiteral("later"), QJsonArray{1}, keep);
        const qint64 before = setup.client.call(QStringLiteral("calc"), QStringLiteral("later"), QJsonArray{2}, keep);
        QTRY_COMPARE(setup.calc.m_waiting.size(), std::size_t(2));
        QVERIFY(subscribed.came);

        setup.fixture.router.stop();
        QTRY_COMPARE(closed.size(), 1);
        QCOMPARE(closed.at(0).at(0).toString(), QStringLiteral("the server closed the connection"));
        QCOMPARE(got.size(), 2);
        for (const IpcReply& reply : std::as_const(got)) {
            QCOMPARE(reply.errorCode, QString::fromLatin1(ClientErrorCode::ConnectionClosed));
            QVERIFY(reply.fromClient);
        }
        QCOMPARE(setup.client.state(), IpcClient::State::Unconnected);
        // The end of the connection ends the subscription without a word.
        QCOMPARE(ended.size(), 0);

        // Without a connection a request fails immediately, from the event
        // loop.
        Answer late;
        setup.client.call(QStringLiteral("calc"), QStringLiteral("sum"), QJsonArray{1, 2}, late.take());
        QVERIFY(!late.came);
        QTRY_VERIFY(late.came);
        QCOMPARE(late.reply.errorCode, QString::fromLatin1(ClientErrorCode::NotConnected));
        QVERIFY(late.reply.fromClient);
        // A second closed() would be a queued call of this thread.
        QCoreApplication::processEvents();
        QCOMPARE(closed.size(), 1);

        // The client connects again when the router is back. Its ids go on.
        QVERIFY(setup.fixture.router.start(setup.fixture.sockPath));
        QSignalSpy connected(&setup.client, &IpcClient::connected);
        QVERIFY(setup.client.connectToServer(setup.fixture.sockPath));
        Answer again;
        const qint64 after =
            setup.client.call(QStringLiteral("calc"), QStringLiteral("sum"), QJsonArray{1, 2}, again.take());
        QVERIFY(after > before);
        QTRY_VERIFY(again.came);
        QCOMPARE(again.reply.result, QJsonValue(3));
        QCOMPARE(connected.size(), 1);

        // The subscription of the first connection is gone on the server's
        // side. subscriptionsOnTheWire has the client's side.
        QSignalSpy events(&setup.client, &IpcClient::eventReceived);
        Answer announced;
        setup.client.call(QStringLiteral("calc"), QStringLiteral("announce"), QJsonArray{1}, announced.take());
        QTRY_VERIFY(announced.came);
        QCOMPARE(events.size(), 0);
    }

    void closeFailsPendingCalls()
    {
        Setup setup;
        QVERIFY2(setup.start(), qPrintable(setup.problem));
        QSignalSpy closed(&setup.client, &IpcClient::closed);

        Answer answer;
        qsizetype closedBeforeTheCallback = -1;
        setup.client.call(QStringLiteral("calc"), QStringLiteral("later"), QJsonArray{1}, [&](const IpcReply& reply) {
            closedBeforeTheCallback = closed.size();
            answer.take()(reply);
        });
        QTRY_COMPARE(setup.calc.m_waiting.size(), std::size_t(1));
        setup.client.close();
        QCOMPARE(setup.client.state(), IpcClient::State::Unconnected);
        QVERIFY(!answer.came);
        QTRY_COMPARE(closed.size(), 1);
        QVERIFY(answer.came);
        QCOMPARE(answer.reply.errorCode, QString::fromLatin1(ClientErrorCode::ConnectionClosed));
        QCOMPARE(answer.reply.errorMessage, QStringLiteral("the client closed the connection"));
        // The callbacks run before closed().
        QCOMPARE(closedBeforeTheCallback, 0);
        QCoreApplication::processEvents();
        QCOMPARE(closed.size(), 1);

        // Nothing happens without a connection.
        setup.client.close();
        QCoreApplication::processEvents();
        QCOMPARE(closed.size(), 1);
    }

    void closeRightAfterConnectGivesNoConnected()
    {
        // The connection is made inside connectToServer(), and connected()
        // waits for the event loop. A close() before that ends the
        // connection, and connected() would tell of one that is gone.
        Setup setup;
        QVERIFY2(setup.start(), qPrintable(setup.problem));
        IpcClient client;
        QStringList order;
        connect(&client, &IpcClient::connected, this, [&] {
            order.append(QStringLiteral("connected"));
        });
        connect(&client, &IpcClient::closed, this, [&] {
            order.append(QStringLiteral("closed"));
        });
        QVERIFY(client.connectToServer(setup.fixture.sockPath));
        QCOMPARE(client.state(), IpcClient::State::Connected);
        client.close();
        QCOMPARE(client.state(), IpcClient::State::Unconnected);
        QTRY_COMPARE(order.size(), 1);
        QCoreApplication::processEvents();
        QCOMPARE(order, QStringList{QStringLiteral("closed")});

        // A new connection can start as soon as the state is Unconnected,
        // also before closed() of the last one came.
        order.clear();
        QVERIFY(client.connectToServer(setup.fixture.sockPath));
        client.close();
        QVERIFY(client.connectToServer(setup.fixture.sockPath));
        QTRY_COMPARE(order.size(), 2);
        QCOMPARE(order, (QStringList{QStringLiteral("closed"), QStringLiteral("connected")}));
        QCOMPARE(client.state(), IpcClient::State::Connected);
    }

    void missingSocketLeavesTheClientUnconnected()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        IpcClient client;
        QSignalSpy closed(&client, &IpcClient::closed);
        QSignalSpy connected(&client, &IpcClient::connected);
        QVERIFY(client.connectToServer(QDir(dir.path()).filePath(QStringLiteral("none.sock"))));
        // A missing socket fails inside connectToServer() already.
        QCOMPARE(client.state(), IpcClient::State::Unconnected);
        Answer answer;
        client.list(answer.take());
        QTRY_COMPARE(closed.size(), 1);
        const QString reason = closed.at(0).at(0).toString();
        QVERIFY2(reason.startsWith(QLatin1String("cannot connect to ")), qPrintable(reason));
        QTRY_VERIFY(answer.came);
        QCOMPARE(answer.reply.errorCode, QString::fromLatin1(ClientErrorCode::NotConnected));
        QCOMPARE(connected.size(), 0);
    }

    void requestsWaitForTheConnection()
    {
#ifndef Q_OS_LINUX
        QSKIP("Only Linux holds a client back when the queue of a listening socket is full.");
#endif
        RawServer server;
        QVERIFY(server.listen(0));
        QVERIFY(server.holdNextConnection());
        IpcClient client;
        QSignalSpy connected(&client, &IpcClient::connected);
        QVERIFY(client.connectToServer(server.path()));
        QCOMPARE(client.state(), IpcClient::State::Connecting);
        QTest::ignoreMessage(QtWarningMsg,
                             "PhosphorIpc::IpcClient::connectToServer: the client is connecting or connected already");
        QVERIFY(!client.connectToServer(server.path()));

        Answer answer;
        const qint64 id = client.call(QStringLiteral("t"), QStringLiteral("f"), QJsonArray{1}, answer.take());
        QCoreApplication::processEvents();
        QVERIFY(!answer.came);
        QCOMPARE(connected.size(), 0);
        QCOMPARE(client.state(), IpcClient::State::Connecting);

        // The request goes out when the connection is made.
        QVERIFY(server.acceptHeld());
        const std::optional<QJsonObject> request = server.readRequest();
        QVERIFY(request.has_value());
        QCOMPARE(request->value(QLatin1String("id")), QJsonValue(id));
        QCOMPARE(request->value(QLatin1String("fn")).toString(), QStringLiteral("f"));
        QCOMPARE(client.state(), IpcClient::State::Connected);
        QVERIFY(server.reply(id, 7));
        QTRY_VERIFY(answer.came);
        QCOMPARE(answer.reply.result, QJsonValue(7));
        QCOMPARE(connected.size(), 1);
    }

    void failedConnectFailsTheRequestsThatWait()
    {
#ifndef Q_OS_LINUX
        QSKIP("Only Linux holds a client back when the queue of a listening socket is full.");
#endif
        RawServer server;
        QVERIFY(server.listen(0));
        QVERIFY(server.holdNextConnection());
        IpcClient client;
        QSignalSpy closed(&client, &IpcClient::closed);
        QSignalSpy connected(&client, &IpcClient::connected);
        QVERIFY(client.connectToServer(server.path()));
        QCOMPARE(client.state(), IpcClient::State::Connecting);
        Answer answer;
        client.call(QStringLiteral("t"), QStringLiteral("f"), QJsonArray(), answer.take());

        server.closeListener();
        QTRY_COMPARE(closed.size(), 1);
        const QString reason = closed.at(0).at(0).toString();
        QVERIFY2(reason.startsWith(QLatin1String("cannot connect to ")), qPrintable(reason));
        QVERIFY(answer.came);
        // The request waited for the connection, so it had one to lose.
        QCOMPARE(answer.reply.errorCode, QString::fromLatin1(ClientErrorCode::ConnectionClosed));
        QCOMPARE(answer.reply.errorMessage, reason);
        QCOMPARE(connected.size(), 0);
        QCOMPARE(client.state(), IpcClient::State::Unconnected);
    }

    void connectTimeoutEndsTheAttempt()
    {
#ifndef Q_OS_LINUX
        QSKIP("Only Linux holds a client back when the queue of a listening socket is full.");
#endif
        // A server that listens and never accepts. Qt gives up without a
        // signal, so only the client's own limit ends the attempt.
        RawServer server;
        QVERIFY(server.listen(0));
        QVERIFY(server.holdNextConnection());
        IpcClient client;
        QCOMPARE(client.connectTimeout(), IpcClient::DefaultConnectTimeoutMs);
        QSignalSpy closed(&client, &IpcClient::closed);

        // The limit of an attempt that ended does not reach the next one.
        client.setConnectTimeout(100);
        QVERIFY(client.connectToServer(server.path()));
        client.close();
        client.setConnectTimeout(60000);
        QCOMPARE(client.connectTimeout(), 60000);
        QVERIFY(client.connectToServer(server.path()));
        QTest::qWait(300);
        QCOMPARE(client.state(), IpcClient::State::Connecting);
        QCOMPARE(closed.size(), 1);
        client.close();
        QTRY_COMPARE(closed.size(), 2);

        // 0 or less waits without a limit.
        client.setConnectTimeout(0);
        QVERIFY(client.connectToServer(server.path()));
        QTest::qWait(300);
        QCOMPARE(client.state(), IpcClient::State::Connecting);
        client.close();
        QTRY_COMPARE(closed.size(), 3);
        client.setConnectTimeout(-5);
        QVERIFY(client.connectToServer(server.path()));
        QTest::qWait(300);
        QCOMPARE(client.state(), IpcClient::State::Connecting);
        client.close();
        QTRY_COMPARE(closed.size(), 4);

        client.setConnectTimeout(100);
        QVERIFY(client.connectToServer(server.path()));
        Answer answer;
        client.call(QStringLiteral("t"), QStringLiteral("f"), QJsonArray(), answer.take());
        QCOMPARE(client.state(), IpcClient::State::Connecting);

        QTRY_COMPARE(closed.size(), 5);
        const QString reason = closed.at(4).at(0).toString();
        QVERIFY2(reason.endsWith(QLatin1String("the server did not accept the connection within 100 ms")),
                 qPrintable(reason));
        QVERIFY(answer.came);
        QCOMPARE(answer.reply.errorCode, QString::fromLatin1(ClientErrorCode::ConnectionClosed));
        QCOMPARE(answer.reply.errorMessage, reason);
        QCOMPARE(client.state(), IpcClient::State::Unconnected);
    }

    void closeWhileConnecting()
    {
#ifndef Q_OS_LINUX
        QSKIP("Only Linux holds a client back when the queue of a listening socket is full.");
#endif
        RawServer server;
        QVERIFY(server.listen(0));
        QVERIFY(server.holdNextConnection());
        IpcClient client;
        QSignalSpy closed(&client, &IpcClient::closed);
        QSignalSpy connected(&client, &IpcClient::connected);
        QVERIFY(client.connectToServer(server.path()));
        Answer answer;
        client.call(QStringLiteral("t"), QStringLiteral("f"), QJsonArray(), answer.take());
        client.close();
        QCOMPARE(client.state(), IpcClient::State::Unconnected);
        QTRY_COMPARE(closed.size(), 1);
        QVERIFY(answer.came);
        QCOMPARE(answer.reply.errorCode, QString::fromLatin1(ClientErrorCode::ConnectionClosed));
        QCOMPARE(answer.reply.errorMessage, QStringLiteral("the client closed the connection"));
        QCOMPARE(connected.size(), 0);
    }

    void writeQueueIsBoundedWhileConnecting()
    {
#ifndef Q_OS_LINUX
        QSKIP("Only Linux holds a client back when the queue of a listening socket is full.");
#endif
        // Every byte waits in the client, so the request that the cap
        // refuses is known exactly.
        RawServer server;
        QVERIFY(server.listen(0));
        QVERIFY(server.holdNextConnection());
        IpcClient client;
        QCOMPARE(client.maxPendingWriteBytes(), IpcClient::DefaultMaxPendingWriteBytes);
        const qint64 cap = 8 * 1024;
        client.setMaxPendingWriteBytes(cap);
        QCOMPARE(client.maxPendingWriteBytes(), cap);
        QVERIFY(client.connectToServer(server.path()));
        QCOMPARE(client.state(), IpcClient::State::Connecting);

        const QJsonArray args{QString(1000, QLatin1Char('x'))};
        IpcReply refused;
        bool refusedInsideTheCall = false;
        int sent = 0;
        int expected = 0;
        qint64 queued = 0;
        while (refused.errorCode.isEmpty() && sent < 100) {
            const qint64 id = client.call(QStringLiteral("t"), QStringLiteral("f"), args, [&](const IpcReply& reply) {
                refused = reply;
            });
            ++sent;
            // Never from inside the call that made the request.
            refusedInsideTheCall = refusedInsideTheCall || !refused.errorCode.isEmpty();
            const qint64 size = callLine(id, QStringLiteral("t"), QStringLiteral("f"), args).size();
            if (expected == 0) {
                if (queued + size > cap) {
                    expected = sent;
                } else {
                    queued += size;
                }
            }
            QCoreApplication::processEvents();
        }
        QCOMPARE(refused.errorCode, QString::fromLatin1(ClientErrorCode::WriteQueueFull));
        QVERIFY(refused.fromClient);
        QVERIFY(!refusedInsideTheCall);
        QVERIFY(expected > 2);
        QCOMPARE(sent, expected);
    }

    void writeQueueIsBoundedWhenTheServerDoesNotRead()
    {
        // The kernel takes what its buffers hold. Then the bytes wait in
        // the client, and the cap refuses a request.
        RawSetup setup;
        QVERIFY2(setup.start(), qPrintable(setup.problem));
        const qint64 cap = 16 * 1024;
        setup.client.setMaxPendingWriteBytes(cap);
        const QJsonArray args{QString(4000, QLatin1Char('x'))};
        const qint64 lineSize = callLine(100000, QStringLiteral("t"), QStringLiteral("f"), args).size();

        IpcReply refused;
        int sent = 0;
        while (refused.errorCode.isEmpty() && sent < 20000) {
            setup.client.call(QStringLiteral("t"), QStringLiteral("f"), args, [&](const IpcReply& reply) {
                if (refused.errorCode.isEmpty()) {
                    refused = reply;
                }
            });
            ++sent;
            QCoreApplication::processEvents();
        }
        QCOMPARE(refused.errorCode, QString::fromLatin1(ClientErrorCode::WriteQueueFull));
        // No request is refused while the queue is under the cap.
        QVERIFY2(sent > cap / lineSize, qPrintable(QString::number(sent)));
        QCOMPARE(setup.client.state(), IpcClient::State::Connected);
    }

    void requestLineAtTheCapOfTheRouter()
    {
        Setup setup;
        QVERIFY2(setup.start(), qPrintable(setup.problem));

        Answer waiting;
        const qint64 first =
            setup.client.call(QStringLiteral("calc"), QStringLiteral("later"), QJsonArray{1}, waiting.take());
        QTRY_COMPARE(setup.calc.m_waiting.size(), std::size_t(1));

        // A line of exactly the router's cap, its newline included.
        const auto lineSize = [](qint64 id, qsizetype length) {
            return callLine(id, QStringLiteral("calc"), QStringLiteral("size"),
                            QJsonArray{QString(length, QLatin1Char('x'))})
                .size();
        };
        const qsizetype fits = qsizetype(MaxRequestLineBytes) - lineSize(first + 1, 0);
        QCOMPARE(lineSize(first + 1, fits), qsizetype(MaxRequestLineBytes));
        QCOMPARE(lineSize(first + 2, fits + 1), qsizetype(MaxRequestLineBytes) + 1);

        Answer atTheCap;
        setup.client.call(QStringLiteral("calc"), QStringLiteral("size"), QJsonArray{QString(fits, QLatin1Char('x'))},
                          atTheCap.take());
        QTRY_VERIFY(atTheCap.came);
        QVERIFY2(!atTheCap.reply.isError(), qPrintable(atTheCap.reply.errorMessage));
        QCOMPARE(atTheCap.reply.result, QJsonValue(qint64(fits)));

        // One byte more would make the router close the connection, and
        // every other request of the client would fail. The client refuses
        // the request itself.
        Answer tooLarge;
        setup.client.call(QStringLiteral("calc"), QStringLiteral("size"),
                          QJsonArray{QString(fits + 1, QLatin1Char('x'))}, tooLarge.take());
        QVERIFY(!tooLarge.came);
        QTRY_VERIFY(tooLarge.came);
        QCOMPARE(tooLarge.reply.errorCode, QString::fromLatin1(ClientErrorCode::RequestTooLarge));
        QVERIFY(tooLarge.reply.fromClient);
        QCOMPARE(setup.client.state(), IpcClient::State::Connected);

        // The deferred call still gets its answer.
        QVERIFY(setup.calc.m_waiting.front().reply.finish(QJsonValue(QStringLiteral("done"))));
        QTRY_VERIFY(waiting.came);
        QCOMPARE(waiting.reply.result, QJsonValue(QStringLiteral("done")));
    }

    void requestsThatTheRouterWouldRefuse()
    {
        // The router answers such a line with an error, and counts it as a
        // bad line: 17 in a row close the connection. The client sends
        // none of them.
        Setup setup;
        QVERIFY2(setup.start(), qPrintable(setup.problem));

        QJsonArray tooMany;
        for (qsizetype i = 0; i <= MaxRequestArgs; ++i) {
            tooMany.append(0);
        }
        QJsonValue nested = 1;
        for (int i = 0; i < 1100; ++i) {
            nested = QJsonArray{nested};
        }

        QList<IpcReply> got;
        const auto keep = [&](const IpcReply& reply) {
            got.append(reply);
        };
        for (int round = 0; round < 3; ++round) {
            setup.client.call(QString(), QStringLiteral("f"), QJsonArray(), keep);
            setup.client.call(QStringLiteral("t"), QString(), QJsonArray(), keep);
            setup.client.schema(QString(), keep);
            setup.client.subscribe(QString(), QStringLiteral("s"), keep);
            setup.client.subscribe(QStringLiteral("t"), QString(), keep);
            setup.client.unsubscribe(0, keep);
            setup.client.call(QStringLiteral("calc"), QStringLiteral("sum"), tooMany, keep);
            setup.client.call(QStringLiteral("calc"), QStringLiteral("sum"), QJsonArray{nested}, keep);
        }
        // Never from inside the call that made the request.
        QVERIFY(got.isEmpty());
        QTRY_COMPARE(got.size(), 24);
        for (const IpcReply& reply : std::as_const(got)) {
            QCOMPARE(reply.errorCode, QString::fromLatin1(ClientErrorCode::InvalidRequest));
            QVERIFY(reply.fromClient);
            QVERIFY(!reply.errorMessage.isEmpty());
        }

        // The connection is as it was.
        QCOMPARE(setup.client.state(), IpcClient::State::Connected);
        Answer answer;
        setup.client.call(QStringLiteral("calc"), QStringLiteral("sum"), QJsonArray{1, 2}, answer.take());
        QTRY_VERIFY(answer.came);
        QCOMPARE(answer.reply.result, QJsonValue(3));
    }

    void requestFromAnotherThreadIsRefused()
    {
        Setup setup;
        QVERIFY2(setup.start(), qPrintable(setup.problem));

        Answer answer;
        QThread* callbackThread = nullptr;
        qint64 id = -1;
        bool connectedFromThere = true;
        QTest::ignoreMessage(QtWarningMsg,
                             "PhosphorIpc::IpcClient::call: called from another thread than the client's");
        QTest::ignoreMessage(QtWarningMsg,
                             "PhosphorIpc::IpcClient::connectToServer: called from another thread than the client's");
        const std::unique_ptr<QThread> thread(QThread::create([&] {
            id = setup.client.call(QStringLiteral("calc"), QStringLiteral("sum"), QJsonArray{1, 2},
                                   [&](const IpcReply& reply) {
                                       callbackThread = QThread::currentThread();
                                       answer.take()(reply);
                                   });
            connectedFromThere = setup.client.connectToServer(setup.fixture.sockPath);
        }));
        thread->start();
        QVERIFY(thread->wait(5000));
        QCOMPARE(id, qint64(0));
        QVERIFY(!connectedFromThere);
        // The callback runs in the client's thread.
        QVERIFY(!answer.came);
        QTRY_VERIFY(answer.came);
        QCOMPARE(callbackThread, QThread::currentThread());
        QCOMPARE(answer.reply.errorCode, QString::fromLatin1(ClientErrorCode::InvalidRequest));
        QVERIFY(answer.reply.fromClient);
    }

    void callbackThatClosesTheClient()
    {
        RawSetup setup;
        QVERIFY2(setup.start(), qPrintable(setup.problem));
        QSignalSpy closed(&setup.client, &IpcClient::closed);

        bool firstRan = false;
        Answer second;
        const qint64 a =
            setup.client.call(QStringLiteral("t"), QStringLiteral("f"), QJsonArray(), [&](const IpcReply&) {
                firstRan = true;
                setup.client.close();
            });
        const qint64 b = setup.client.call(QStringLiteral("t"), QStringLiteral("g"), QJsonArray(), second.take());
        QVERIFY(setup.takeRequest(a));
        QVERIFY(setup.takeRequest(b));
        // Both answers in one write, so that the client holds the second
        // when the callback of the first closes it.
        QVERIFY(setup.server.write(writeLine(buildReply(a, 1)) + writeLine(buildReply(b, 2))));

        QTRY_COMPARE(closed.size(), 1);
        QVERIFY(firstRan);
        QCOMPARE(closed.at(0).at(0).toString(), QStringLiteral("the client closed the connection"));
        QCOMPARE(setup.client.state(), IpcClient::State::Unconnected);
        // An answer that came before the end is still an answer.
        QVERIFY(second.came);
        QVERIFY2(!second.reply.isError(), qPrintable(second.reply.errorMessage));
        QCOMPARE(second.reply.result, QJsonValue(2));
        QVERIFY(setup.server.clientClosed());
    }

    void callbackThatConnectsAgain()
    {
        // The callback closes the client, connects again and makes a call,
        // and then runs an event loop in which the answer to that call
        // comes. The loop on the stack belongs to the old connection.
        RawSetup setup;
        QVERIFY2(setup.start(), qPrintable(setup.problem));
        QSignalSpy closed(&setup.client, &IpcClient::closed);

        Answer third;
        bool served = false;
        bool thirdCameInside = true;
        const qint64 a =
            setup.client.call(QStringLiteral("t"), QStringLiteral("f"), QJsonArray(), [&](const IpcReply&) {
                setup.client.close();
                if (!setup.client.connectToServer(setup.server.path())) {
                    return;
                }
                const qint64 c =
                    setup.client.call(QStringLiteral("t"), QStringLiteral("h"), QJsonArray(), third.take());
                // These spin the event loop.
                served = setup.server.accept() && setup.takeRequest(c) && setup.server.reply(c, 3);
                QTest::qWait(50);
                thirdCameInside = third.came;
            });
        QVERIFY(setup.takeRequest(a));
        QVERIFY(setup.server.reply(a, 1));

        // No further byte of the server is needed for the answer.
        QTRY_VERIFY(third.came);
        QVERIFY(served);
        QVERIFY(!thirdCameInside);
        QCOMPARE(third.reply.result, QJsonValue(3));
        QCOMPARE(closed.size(), 1);
        QCOMPARE(setup.client.state(), IpcClient::State::Connected);
    }

    void callbackThatDeletesTheClientLater()
    {
        RawServer server;
        QVERIFY(server.listen());
        // deleteLater() takes the client. The owner takes it when the test
        // fails before that.
        QObject owner;
        const QPointer<IpcClient> client(new IpcClient(&owner));
        QVERIFY(client->connectToServer(server.path()));
        QVERIFY(server.accept());

        int ran = 0;
        const qint64 a = client->call(QStringLiteral("t"), QStringLiteral("f"), QJsonArray(), [&](const IpcReply&) {
            ++ran;
            client->deleteLater();
        });
        const qint64 b = client->call(QStringLiteral("t"), QStringLiteral("g"), QJsonArray(), [&](const IpcReply&) {
            ++ran;
        });
        QVERIFY(server.readRequest().has_value());
        QVERIFY(server.readRequest().has_value());
        QVERIFY(server.write(writeLine(buildReply(a, 1)) + writeLine(buildReply(b, 2))));
        QTRY_VERIFY(client.isNull());
        // The client lived until the event loop took it, so both ran.
        QCOMPARE(ran, 2);
        QVERIFY(server.clientClosed());
    }

    void answersWaitWhileACallbackRunsAnEventLoop()
    {
        // The answer to the second call comes while the callback of the
        // first runs an event loop. The socket reports nothing then,
        // because its signal is on the stack. The client must read it when
        // the callback returns, with no further byte of the server.
        RawSetup setup;
        QVERIFY2(setup.start(), qPrintable(setup.problem));

        Answer second;
        qint64 b = 0;
        bool wrote = false;
        bool secondCameInside = true;
        const qint64 a =
            setup.client.call(QStringLiteral("t"), QStringLiteral("f"), QJsonArray(), [&](const IpcReply&) {
                wrote = setup.server.reply(b, 2);
                QTest::qWait(100);
                secondCameInside = second.came;
            });
        b = setup.client.call(QStringLiteral("t"), QStringLiteral("g"), QJsonArray(), second.take());
        QVERIFY(setup.takeRequest(a));
        QVERIFY(setup.takeRequest(b));
        QVERIFY(setup.server.reply(a, 1));

        QTRY_VERIFY(second.came);
        QVERIFY(wrote);
        QVERIFY(!secondCameInside);
        QCOMPARE(second.reply.result, QJsonValue(2));
    }

    void connectionThatEndsWhileACallbackRunsAnEventLoop()
    {
        // The server answers the second call and closes while the callback
        // of the first runs an event loop. The end does not wait for that
        // callback: the state changes, and the answer that came, the
        // failure of the third call and closed() come inside its loop. The
        // socket must outlive its own signal, which is on the stack.
        RawSetup setup;
        QVERIFY2(setup.start(), qPrintable(setup.problem));
        QSignalSpy closed(&setup.client, &IpcClient::closed);

        Answer second;
        Answer third;
        qint64 b = 0;
        bool wrote = false;
        IpcClient::State stateInside = IpcClient::State::Connected;
        bool secondCameInside = false;
        bool thirdCameInside = false;
        const qint64 a =
            setup.client.call(QStringLiteral("t"), QStringLiteral("f"), QJsonArray(), [&](const IpcReply&) {
                wrote = setup.server.reply(b, 2);
                setup.server.closeConnection();
                QElapsedTimer timer;
                timer.start();
                while (closed.isEmpty() && timer.elapsed() < 5000) {
                    QTest::qWait(10);
                }
                stateInside = setup.client.state();
                secondCameInside = second.came;
                thirdCameInside = third.came;
            });
        b = setup.client.call(QStringLiteral("t"), QStringLiteral("g"), QJsonArray(), second.take());
        const qint64 c = setup.client.call(QStringLiteral("t"), QStringLiteral("h"), QJsonArray(), third.take());
        QVERIFY(setup.takeRequest(a));
        QVERIFY(setup.takeRequest(b));
        QVERIFY(setup.takeRequest(c));
        QVERIFY(setup.server.reply(a, 1));

        QTRY_COMPARE(closed.size(), 1);
        QVERIFY(wrote);
        QCOMPARE(stateInside, IpcClient::State::Unconnected);
        QVERIFY(secondCameInside);
        QVERIFY(thirdCameInside);
        QVERIFY(second.came);
        QVERIFY2(!second.reply.isError(), qPrintable(second.reply.errorMessage));
        QCOMPARE(second.reply.result, QJsonValue(2));
        // The call that got no answer.
        QVERIFY(third.came);
        QCOMPARE(third.reply.errorCode, QString::fromLatin1(ClientErrorCode::ConnectionClosed));
        QCoreApplication::processEvents();
        QCOMPARE(closed.size(), 1);
    }

    void manyRepliesInOneWrite()
    {
        // More lines than the client handles in one pass. The client must
        // go on by itself: the server sends nothing more that could wake
        // it, and the client has no timeout for an answer.
        RawSetup setup;
        QVERIFY2(setup.start(), qPrintable(setup.problem));
        const int count = 200;
        QList<qint64> ids;
        QList<qint64> answered;
        for (int i = 0; i < count; ++i) {
            const auto slot = std::make_shared<qint64>(0);
            *slot =
                setup.client.call(QStringLiteral("t"), QStringLiteral("f"), QJsonArray(), [&, slot](const IpcReply& r) {
                    if (!r.isError() && r.result == QJsonValue(double(*slot))) {
                        answered.append(*slot);
                    }
                });
            ids.append(*slot);
        }
        QByteArray replies;
        for (const qint64 id : std::as_const(ids)) {
            QVERIFY(setup.takeRequest(id));
            replies += writeLine(buildReply(id, double(id)));
        }
        QVERIFY(setup.server.write(replies));
        QTRY_COMPARE(answered.size(), count);
        QCOMPARE(answered, ids);
    }

    void callbackWhoseObjectClosesTheClient_data()
    {
        QTest::addColumn<bool>("connectsAgain");
        QTest::newRow("closes") << false;
        QTest::newRow("closes and connects again") << true;
    }

    void callbackWhoseObjectClosesTheClient()
    {
        // The callback of an answer holds an object, and the destructor of
        // that object closes the client. The client destroys the callback
        // after it ran, in the middle of its read loop, which must then
        // not read from the connection that is gone.
        QFETCH(bool, connectsAgain);
        // Before the client: the object that the client's callback holds
        // writes them in its destructor, also when a check below fails.
        bool ran = false;
        bool connectedAgain = false;
        Answer second;
        RawSetup setup;
        QVERIFY2(setup.start(), qPrintable(setup.problem));
        QSignalSpy closed(&setup.client, &IpcClient::closed);

        qint64 a = 0;
        {
            const auto held = std::make_shared<RunsAtItsEnd>([&] {
                setup.client.close();
                if (connectsAgain) {
                    connectedAgain = setup.client.connectToServer(setup.server.path());
                }
            });
            a = setup.client.call(QStringLiteral("t"), QStringLiteral("f"), QJsonArray(),
                                  [held, &ran](const IpcReply&) {
                                      ran = true;
                                  });
        }
        const qint64 b = setup.client.call(QStringLiteral("t"), QStringLiteral("g"), QJsonArray(), second.take());
        QVERIFY(setup.takeRequest(a));
        QVERIFY(setup.takeRequest(b));
        // Both answers in one write, so that the second is in the buffer
        // of the client when the object goes.
        QVERIFY(setup.server.write(writeLine(buildReply(a, 1)) + writeLine(buildReply(b, 2))));

        QTRY_COMPARE(closed.size(), 1);
        QVERIFY(ran);
        QCOMPARE(closed.at(0).at(0).toString(), QStringLiteral("the client closed the connection"));
        // The answer that the client had received still reaches its call.
        QVERIFY(second.came);
        QCOMPARE(second.reply.result, QJsonValue(2));
        if (!connectsAgain) {
            QCOMPARE(setup.client.state(), IpcClient::State::Unconnected);
            return;
        }
        QVERIFY(connectedAgain);
        QVERIFY(setup.server.accept());
        Answer third;
        const qint64 c = setup.client.call(QStringLiteral("t"), QStringLiteral("h"), QJsonArray(), third.take());
        QVERIFY(setup.takeRequest(c));
        QVERIFY(setup.server.reply(c, 3));
        QTRY_VERIFY(third.came);
        QCOMPARE(third.reply.result, QJsonValue(3));
        QCOMPARE(closed.size(), 1);
    }

    void limitsKeepTheirRange()
    {
        IpcClient client;
        QCOMPARE(client.maxReplyLineBytes(), IpcClient::DefaultMaxReplyLineBytes);
        QCOMPARE(client.maxPendingWriteBytes(), IpcClient::DefaultMaxPendingWriteBytes);
        client.setMaxReplyLineBytes(0);
        QCOMPARE(client.maxReplyLineBytes(), 1);
        client.setMaxReplyLineBytes(-5);
        QCOMPARE(client.maxReplyLineBytes(), 1);
        client.setMaxPendingWriteBytes(0);
        QCOMPARE(client.maxPendingWriteBytes(), 1);
        client.setConnectTimeout(-5);
        QCOMPARE(client.connectTimeout(), -5);

        // The largest cap of a line. The client adds 1 to the cap where it
        // reads, which must not overflow. It must not ask Qt for an array
        // of the size of the cap either.
        RawSetup setup;
        setup.client.setMaxReplyLineBytes(std::numeric_limits<qint64>::max());
        QCOMPARE(setup.client.maxReplyLineBytes(), std::numeric_limits<qint64>::max() - 1);
        QVERIFY2(setup.start(), qPrintable(setup.problem));
        Answer answer;
        const qint64 a = setup.client.call(QStringLiteral("t"), QStringLiteral("f"), QJsonArray(), answer.take());
        QVERIFY(setup.takeRequest(a));
        QVERIFY(setup.server.reply(a, 1));
        QTRY_VERIFY(answer.came);
        QCOMPARE(answer.reply.result, QJsonValue(1));
    }

    void endDeliversEveryLineThatCame()
    {
        // The cap is on one line. When the connection ends, the client
        // can hold more than one line: some in its own buffer, and more in
        // the buffer of Qt. Each of them must reach its receiver. Here the
        // client closes inside a callback, with three lines of under 64
        // bytes that are over 64 bytes together.
        RawSetup setup;
        setup.client.setMaxReplyLineBytes(64);
        QVERIFY2(setup.start(), qPrintable(setup.problem));
        QSignalSpy closed(&setup.client, &IpcClient::closed);

        Answer second;
        Answer third;
        const qint64 a =
            setup.client.call(QStringLiteral("t"), QStringLiteral("f"), QJsonArray(), [&](const IpcReply&) {
                // Until the rest of the server's bytes are in the buffer of
                // Qt. The client reads no line meanwhile.
                QTest::qWait(100);
                setup.client.close();
            });
        const qint64 b = setup.client.call(QStringLiteral("t"), QStringLiteral("g"), QJsonArray(), second.take());
        const qint64 c = setup.client.call(QStringLiteral("t"), QStringLiteral("h"), QJsonArray(), third.take());
        QVERIFY(setup.takeRequest(a));
        QVERIFY(setup.takeRequest(b));
        QVERIFY(setup.takeRequest(c));
        const QByteArray lines =
            writeLine(buildReply(a, 1)) + writeLine(buildReply(b, 2)) + writeLine(buildReply(c, 3));
        QVERIFY(lines.size() > 65);
        QVERIFY(setup.server.write(lines));
        QTRY_COMPARE(closed.size(), 1);
        QVERIFY(second.came);
        QCOMPARE(second.reply.result, QJsonValue(2));
        QVERIFY(third.came);
        QVERIFY2(!third.reply.isError(), qPrintable(third.reply.errorMessage));
        QCOMPARE(third.reply.result, QJsonValue(3));
    }

    void unsubscribeAfterTheEndStopsTheEventsThatWait()
    {
        // An event that the client had received when the connection ended
        // waits in the queue of the event loop. An unsubscribe() before
        // its turn still ends the stream: no event comes after it.
        RawSetup setup;
        QVERIFY2(setup.start(), qPrintable(setup.problem));
        QSignalSpy events(&setup.client, &IpcClient::eventReceived);
        QSignalSpy closed(&setup.client, &IpcClient::closed);

        Answer subscribed;
        const qint64 s = setup.client.subscribe(QStringLiteral("t"), QStringLiteral("s"), subscribed.take());
        QVERIFY(setup.takeRequest(s));
        QVERIFY(setup.server.reply(s, QJsonValue::Null));
        QTRY_VERIFY(subscribed.came);
        Answer kept;
        const qint64 k = setup.client.subscribe(QStringLiteral("t"), QStringLiteral("other"), kept.take());
        QVERIFY(setup.takeRequest(k));
        QVERIFY(setup.server.reply(k, QJsonValue::Null));
        QTRY_VERIFY(kept.came);

        bool wrote = false;
        const qint64 a =
            setup.client.call(QStringLiteral("t"), QStringLiteral("f"), QJsonArray(), [&](const IpcReply&) {
                wrote = setup.server.write(writeLine(buildEvent(s, QJsonArray{1}))
                                           + writeLine(buildEvent(k, QJsonArray{2})));
                // Until the events are in the buffer of Qt.
                QTest::qWait(100);
                setup.client.close();
                setup.client.unsubscribe(s);
            });
        QVERIFY(setup.takeRequest(a));
        QVERIFY(setup.server.reply(a, 1));
        QTRY_COMPARE(closed.size(), 1);
        QVERIFY(wrote);
        // The event of the subscription that still lived comes.
        QCOMPARE(events.size(), 1);
        QCOMPARE(events.at(0).at(0).toLongLong(), k);
    }

    void connectWhileConnectedIsRefused()
    {
        RawSetup setup;
        QVERIFY2(setup.start(), qPrintable(setup.problem));
        QSignalSpy closed(&setup.client, &IpcClient::closed);
        QTest::ignoreMessage(QtWarningMsg,
                             "PhosphorIpc::IpcClient::connectToServer: the client is connecting or connected already");
        QVERIFY(!setup.client.connectToServer(setup.server.path()));
        QCOMPARE(setup.client.state(), IpcClient::State::Connected);
        // The connection that is open still works.
        Answer answer;
        const qint64 a = setup.client.call(QStringLiteral("t"), QStringLiteral("f"), QJsonArray(), answer.take());
        QVERIFY(setup.takeRequest(a));
        QVERIFY(setup.server.reply(a, 1));
        QTRY_VERIFY(answer.came);
        QCOMPARE(closed.size(), 0);
    }

    void numbersThatAreNotFiniteAreNullOnTheWire()
    {
        RawSetup setup;
        QVERIFY2(setup.start(), qPrintable(setup.problem));
        const qint64 a = setup.client.call(
            QStringLiteral("t"), QStringLiteral("f"),
            QJsonArray{std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(), 1.5});
        const std::optional<QJsonObject> request = setup.server.readRequest();
        QVERIFY(request.has_value());
        QCOMPARE(request->value(QLatin1String("id")), QJsonValue(a));
        QCOMPARE(request->value(QLatin1String("args")),
                 QJsonValue(QJsonArray{QJsonValue::Null, QJsonValue::Null, 1.5}));
    }

    void unsubscribeThatIsNotSentLeavesTheServerSubscribed()
    {
        Setup setup;
        QVERIFY2(setup.start(), qPrintable(setup.problem));
        QSignalSpy events(&setup.client, &IpcClient::eventReceived);
        Answer subscribed;
        const qint64 s =
            setup.client.subscribe(QStringLiteral("calc"), QStringLiteral("countChanged"), subscribed.take());
        QTRY_VERIFY(subscribed.came);
        QVERIFY2(!subscribed.reply.isError(), qPrintable(subscribed.reply.errorMessage));

        // No room for one request: the client cannot send the unsubscribe.
        setup.client.setMaxPendingWriteBytes(1);
        Answer unsubscribed;
        setup.client.unsubscribe(s, unsubscribed.take());
        QTRY_VERIFY(unsubscribed.came);
        QCOMPARE(unsubscribed.reply.errorCode, QString::fromLatin1(ClientErrorCode::WriteQueueFull));
        QVERIFY(unsubscribed.reply.fromClient);
        setup.client.setMaxPendingWriteBytes(IpcClient::DefaultMaxPendingWriteBytes);

        // The server still holds the subscription, so it refuses a second
        // one to the same signal. The client gives the events of the first
        // to nobody.
        Answer again;
        setup.client.subscribe(QStringLiteral("calc"), QStringLiteral("countChanged"), again.take());
        QTRY_VERIFY(again.came);
        QCOMPARE(again.reply.errorCode, QString::fromLatin1(ErrorCode::MalformedRequest));
        Answer announced;
        setup.client.call(QStringLiteral("calc"), QStringLiteral("announce"), QJsonArray{1}, announced.take());
        QTRY_VERIFY(announced.came);
        QCOMPARE(events.size(), 0);
    }

    void replyInTwoParts()
    {
        RawSetup setup;
        QVERIFY2(setup.start(), qPrintable(setup.problem));
        Answer answer;
        const qint64 a = setup.client.call(QStringLiteral("t"), QStringLiteral("f"), QJsonArray(), answer.take());
        QVERIFY(setup.takeRequest(a));
        const QByteArray line = writeLine(buildReply(a, QStringLiteral("whole")));
        QVERIFY(setup.server.write(line.left(10)));
        // The bytes are in the socket when write() returns.
        QCoreApplication::processEvents();
        QVERIFY(!answer.came);
        QVERIFY(setup.server.write(line.mid(10)));
        QTRY_VERIFY(answer.came);
        QCOMPARE(answer.reply.result, QJsonValue(QStringLiteral("whole")));
    }

    void framesThatAreForNobody()
    {
        RawSetup setup;
        QVERIFY2(setup.start(), qPrintable(setup.problem));
        QSignalSpy events(&setup.client, &IpcClient::eventReceived);
        QSignalSpy ended(&setup.client, &IpcClient::subscriptionEnded);
        QSignalSpy closed(&setup.client, &IpcClient::closed);

        Answer answer;
        const qint64 a = setup.client.call(QStringLiteral("t"), QStringLiteral("f"), QJsonArray(), answer.take());
        // A call whose answer does not matter.
        const qint64 b = setup.client.call(QStringLiteral("t"), QStringLiteral("g"), QJsonArray());
        QVERIFY(setup.takeRequest(a));
        QVERIFY(setup.takeRequest(b));

        // A blank line, an answer to no request, an error for no request,
        // a frame of a kind that the client does not know, an event that
        // names nothing, and an event of no subscription.
        QVERIFY(setup.server.write(QByteArray("\n  \n")));
        QVERIFY(setup.server.reply(9999, 1));
        QVERIFY(setup.server.write(buildError(9998, QStringLiteral("X"), QStringLiteral("nobody"))));
        // The frame of the unknown kind has a result that the reply to the
        // same id below does not have, so a client that took it shows.
        QVERIFY(setup.server.write(QJsonObject{{QStringLiteral("type"), QStringLiteral("later-kind")},
                                               {QStringLiteral("id"), a},
                                               {QStringLiteral("result"), 99}}));
        QVERIFY(setup.server.write(QJsonObject{{QStringLiteral("type"), QStringLiteral("event")}}));
        QVERIFY(setup.server.write(buildEvent(9997, QJsonArray{1})));
        QVERIFY(setup.server.reply(b, 2));
        // A reply without a result reads as null.
        QVERIFY(setup.server.write(
            QJsonObject{{QStringLiteral("type"), QStringLiteral("reply")}, {QStringLiteral("id"), a}}));

        QTRY_VERIFY(answer.came);
        QVERIFY(!answer.reply.isError());
        QVERIFY(answer.reply.result.isNull());
        QVERIFY(!answer.reply.result.isUndefined());
        QCOMPARE(events.size(), 0);
        QCOMPARE(ended.size(), 0);
        QCOMPARE(closed.size(), 0);
        QCOMPARE(setup.client.state(), IpcClient::State::Connected);
    }

    void errorFrames()
    {
        RawSetup setup;
        QVERIFY2(setup.start(), qPrintable(setup.problem));
        Answer withDetail;
        Answer withoutCode;
        const qint64 a = setup.client.call(QStringLiteral("t"), QStringLiteral("f"), QJsonArray(), withDetail.take());
        const qint64 b = setup.client.call(QStringLiteral("t"), QStringLiteral("g"), QJsonArray(), withoutCode.take());
        QVERIFY(setup.takeRequest(a));
        QVERIFY(setup.takeRequest(b));

        const QJsonObject detail{{QStringLiteral("line"), 3}};
        QVERIFY(setup.server.write(buildError(a, QStringLiteral("PARSE"), QStringLiteral("bad input"), detail)));
        QVERIFY(setup.server.write(QJsonObject{{QStringLiteral("type"), QStringLiteral("error")},
                                               {QStringLiteral("id"), b},
                                               {QStringLiteral("message"), QStringLiteral("no code")}}));
        QTRY_VERIFY(withoutCode.came);
        QVERIFY(withDetail.came);
        QCOMPARE(withDetail.reply.errorCode, QStringLiteral("PARSE"));
        QCOMPARE(withDetail.reply.errorMessage, QStringLiteral("bad input"));
        QCOMPARE(withDetail.reply.errorDetail, detail);
        QVERIFY(!withDetail.reply.fromClient);

        // An error must have a code. The client gives it one of its own.
        QCOMPARE(withoutCode.reply.errorCode, QString::fromLatin1(ClientErrorCode::MalformedReply));
        QCOMPARE(withoutCode.reply.errorMessage, QStringLiteral("no code"));
        QVERIFY(withoutCode.reply.fromClient);
    }

    void lineThatIsNoFrameEndsTheConnection_data()
    {
        QTest::addColumn<QByteArray>("line");
        QTest::addColumn<QString>("reason");

        QTest::newRow("not JSON") << QByteArray("this is not JSON\n")
                                  << QStringLiteral("the server sent a line that is not a JSON object");
        QTest::newRow("an array") << QByteArray("[1, 2]\n")
                                  << QStringLiteral("the server sent a line that is not a JSON object");
        QTest::newRow("an answer with a fraction as its id")
            << QByteArray("{\"type\":\"reply\",\"id\":1.5,\"result\":1}\n")
            << QStringLiteral("the server sent an answer with an id that is not an integer");
        QTest::newRow("an answer with a string as its id")
            << QByteArray("{\"type\":\"error\",\"id\":\"1\",\"code\":\"X\"}\n")
            << QStringLiteral("the server sent an answer with an id that is not an integer");
        QTest::newRow("an event with a string as its subscription")
            << QByteArray("{\"type\":\"event\",\"subscriptionId\":\"7\",\"args\":[]}\n")
            << QStringLiteral("the server sent an event with a subscription id that is not an integer");
    }

    void lineThatIsNoFrameEndsTheConnection()
    {
        // The client cannot tell which request such a line answers, and it
        // has no timeout, so that request would wait without end.
        QFETCH(QByteArray, line);
        QFETCH(QString, reason);

        RawSetup setup;
        QVERIFY2(setup.start(), qPrintable(setup.problem));
        QSignalSpy closed(&setup.client, &IpcClient::closed);
        Answer first;
        Answer second;
        const qint64 a = setup.client.call(QStringLiteral("t"), QStringLiteral("f"), QJsonArray(), first.take());
        const qint64 b = setup.client.call(QStringLiteral("t"), QStringLiteral("g"), QJsonArray(), second.take());
        QVERIFY(setup.takeRequest(a));
        QVERIFY(setup.takeRequest(b));
        // The answer before the line is good.
        QVERIFY(setup.server.write(writeLine(buildReply(a, 1)) + line));

        QTRY_COMPARE(closed.size(), 1);
        QCOMPARE(closed.at(0).at(0).toString(), reason);
        QVERIFY(first.came);
        QCOMPARE(first.reply.result, QJsonValue(1));
        QVERIFY(second.came);
        QCOMPARE(second.reply.errorCode, QString::fromLatin1(ClientErrorCode::ConnectionClosed));
        QCOMPARE(second.reply.errorMessage, reason);
        QCOMPARE(setup.client.state(), IpcClient::State::Unconnected);
        QVERIFY(setup.server.clientClosed());
    }

    void lineLongerThanTheCapEndsTheConnection_data()
    {
        QTest::addColumn<bool>("ended");

        QTest::newRow("a line that never ends") << false;
        QTest::newRow("a whole line") << true;
    }

    void lineLongerThanTheCapEndsTheConnection()
    {
        QFETCH(bool, ended);

        RawSetup setup;
        QVERIFY2(setup.start(), qPrintable(setup.problem));
        QCOMPARE(setup.client.maxReplyLineBytes(), IpcClient::DefaultMaxReplyLineBytes);
        setup.client.setMaxReplyLineBytes(64);
        QCOMPARE(setup.client.maxReplyLineBytes(), qint64(64));
        QSignalSpy closed(&setup.client, &IpcClient::closed);
        Answer first;
        Answer second;
        const qint64 a = setup.client.call(QStringLiteral("t"), QStringLiteral("f"), QJsonArray(), first.take());
        const qint64 b = setup.client.call(QStringLiteral("t"), QStringLiteral("g"), QJsonArray(), second.take());
        QVERIFY(setup.takeRequest(a));
        QVERIFY(setup.takeRequest(b));

        // A line of exactly the cap is taken.
        QByteArray atTheCap = writeLine(buildReply(a, QString()));
        const qsizetype padding = 64 + 1 - atTheCap.size();
        QVERIFY(padding > 0);
        atTheCap = writeLine(buildReply(a, QString(padding, QLatin1Char('p'))));
        QCOMPARE(atTheCap.size(), qsizetype(64 + 1));
        QByteArray tooLong(65, 'x');
        if (ended) {
            tooLong.append('\n');
        }
        QVERIFY(setup.server.write(atTheCap + tooLong));

        QTRY_COMPARE(closed.size(), 1);
        QCOMPARE(closed.at(0).at(0).toString(), QStringLiteral("the server sent a line longer than 64 bytes"));
        // The whole line before it was an answer.
        QVERIFY(first.came);
        QVERIFY2(!first.reply.isError(), qPrintable(first.reply.errorMessage));
        QCOMPARE(first.reply.result.toString().size(), padding);
        QVERIFY(second.came);
        QCOMPARE(second.reply.errorCode, QString::fromLatin1(ClientErrorCode::ConnectionClosed));
    }

    void errorForNoRequestIsTheReasonOfTheClose()
    {
        // The router sends an error that names no request before it closes
        // a connection, with the reason.
        RawSetup setup;
        QVERIFY2(setup.start(), qPrintable(setup.problem));
        QSignalSpy closed(&setup.client, &IpcClient::closed);
        Answer answer;
        const qint64 a = setup.client.call(QStringLiteral("t"), QStringLiteral("f"), QJsonArray(), answer.take());
        QVERIFY(setup.takeRequest(a));

        // A text of the server is cut, and loses its control characters.
        const QString message = QStringLiteral("too many\nbad lines \x1b[31m") + QString(400, QLatin1Char('z'));
        QVERIFY(setup.server.write(buildError(0, QString::fromLatin1(ErrorCode::MalformedRequest), message)));
        setup.server.closeConnection();

        QTRY_COMPARE(closed.size(), 1);
        const QString reason = closed.at(0).at(0).toString();
        const QString prefix = QStringLiteral("the server closed the connection: too many bad lines  [31mzzz");
        QVERIFY2(reason.startsWith(prefix), qPrintable(reason));
        QCOMPARE(reason.size(), QStringLiteral("the server closed the connection: ").size() + 256);
        QVERIFY(answer.came);
        QCOMPARE(answer.reply.errorCode, QString::fromLatin1(ClientErrorCode::ConnectionClosed));
        QCOMPARE(answer.reply.errorMessage, reason);
    }

    void subscriptionsOnTheWire()
    {
        RawSetup setup;
        QVERIFY2(setup.start(), qPrintable(setup.problem));
        QSignalSpy events(&setup.client, &IpcClient::eventReceived);
        QSignalSpy ended(&setup.client, &IpcClient::subscriptionEnded);

        // A subscription that the server refused starts no stream, also
        // when the server sends an event under its id.
        Answer refused;
        const qint64 r = setup.client.subscribe(QStringLiteral("t"), QStringLiteral("s"), refused.take());
        QVERIFY(setup.takeRequest(r));
        QVERIFY(setup.server.write(buildError(r, QString::fromLatin1(ErrorCode::NoSuchSignal), QStringLiteral("no"))));
        QVERIFY(setup.server.write(buildEvent(r, QJsonArray{1})));

        // A subscription that lives gets its events.
        Answer taken;
        const qint64 s = setup.client.subscribe(QStringLiteral("t"), QStringLiteral("s"), taken.take());
        QVERIFY(setup.takeRequest(s));
        QVERIFY(setup.server.reply(s, QJsonValue::Null));
        QVERIFY(setup.server.write(buildEvent(s, QJsonArray{2})));
        QTRY_COMPARE(events.size(), 1);
        QVERIFY(refused.came);
        QCOMPARE(refused.reply.errorCode, QString::fromLatin1(ErrorCode::NoSuchSignal));
        QCOMPARE(events.at(0).at(0).toLongLong(), s);
        QCOMPARE(events.at(0).at(3).toJsonArray(), QJsonArray{2});

        // After a new connection the server knows no subscription of the
        // old one. An event under the old id is for nobody.
        setup.client.close();
        QVERIFY(setup.client.connectToServer(setup.server.path()));
        QVERIFY(setup.server.accept());
        QVERIFY(setup.server.write(buildEvent(s, QJsonArray{3})));
        Answer marker;
        const qint64 m = setup.client.call(QStringLiteral("t"), QStringLiteral("f"), QJsonArray(), marker.take());
        QVERIFY(setup.takeRequest(m));
        QVERIFY(setup.server.reply(m, 1));
        QTRY_VERIFY(marker.came);
        QCOMPARE(events.size(), 1);
        QCOMPARE(ended.size(), 0);
    }

    void destroyingDropsCallbacks()
    {
        Setup setup;
        QVERIFY2(setup.start(), qPrintable(setup.problem));
        bool ran = false;
        auto client = std::make_unique<IpcClient>();
        QSignalSpy connected(client.get(), &IpcClient::connected);
        QVERIFY(client->connectToServer(setup.fixture.sockPath));
        QVERIFY(connected.wait(2000));
        client->call(QStringLiteral("calc"), QStringLiteral("later"), QJsonArray{1}, [&](const IpcReply&) {
            ran = true;
        });
        QTRY_COMPARE(setup.calc.m_waiting.size(), std::size_t(1));
        client.reset();
        // A callback would be a queued call of this thread.
        QCoreApplication::processEvents();
        QVERIFY(!ran);
    }

    void destroyingWithACallbackThatCallsTheClient_data()
    {
        QTest::addColumn<bool>("connected");

        // The callback waits for an answer of the server.
        QTest::newRow("a request in flight") << true;
        // The callback waits in the event queue, with its failure.
        QTest::newRow("a request that failed") << false;
    }

    void destroyingWithACallbackThatCallsTheClient()
    {
        // A callback can hold an object whose destructor calls the client.
        // The client drops its callbacks while it is whole, and drops what
        // such a destructor asks for.
        QFETCH(bool, connected);

        Setup setup;
        QVERIFY2(setup.start(), qPrintable(setup.problem));
        // Before the client: the object that the client's callback holds
        // uses them in its destructor, also when a check below fails.
        bool clientIsGone = false;
        int wentWhileTheClientLived = 0;
        bool ran = false;
        auto client = std::make_unique<IpcClient>();
        if (connected) {
            QSignalSpy spy(client.get(), &IpcClient::connected);
            QVERIFY(client->connectToServer(setup.fixture.sockPath));
            QVERIFY(spy.wait(2000));
        }
        {
            const auto held =
                std::make_shared<CallsTheClientAtItsEnd>(client.get(), &clientIsGone, &wentWhileTheClientLived);
            client->call(QStringLiteral("calc"), QStringLiteral("later"), QJsonArray{1}, [held, &ran](const IpcReply&) {
                ran = true;
            });
        }
        if (connected) {
            QTRY_COMPARE(setup.calc.m_waiting.size(), std::size_t(1));
        }
        QCOMPARE(wentWhileTheClientLived, 0);

        // The object goes inside the destructor of the client, not after.
        client.reset();
        clientIsGone = true;
        QCOMPARE(wentWhileTheClientLived, 1);
        QCoreApplication::processEvents();
        QVERIFY(!ran);
    }
};

QTEST_GUILESS_MAIN(TestPhosphorIpcClient)
#include "test_phosphor_ipc_client.moc"
