// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// IpcClient against a real IpcRouter on a socket in a temporary directory,
// both in the test's thread: every answer comes while the event loop runs.

#include "ipctesthelpers.h"

#include <PhosphorIpc/IpcCallContext.h>
#include <PhosphorIpc/IpcClient.h>
#include <PhosphorIpc/IpcPendingReply.h>
#include <PhosphorIpc/IpcProtocol.h>
#include <PhosphorIpc/IpcRouter.h>

#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QLocalServer>
#include <QLocalSocket>
#include <QObject>
#include <QSignalSpy>
#include <QString>
#include <QTemporaryDir>
#include <QTest>
#include <QtCore/qtclasshelpermacros.h>

#include <list>
#include <memory>

using namespace PhosphorIpc;
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

// A router with the calc target, and a client connected to it.
struct Setup
{
    RouterFixture fixture;
    Calc calc{fixture.router};
    IpcClient client;

    bool start()
    {
        if (!fixture.valid() || !fixture.router.registerTarget(QStringLiteral("calc"), &calc)
            || !fixture.router.start(fixture.sockPath)) {
            return false;
        }
        QSignalSpy connected(&client, &IpcClient::connected);
        return client.connectToServer(fixture.sockPath) && connected.wait(2000);
    }
};

} // namespace

class TestPhosphorIpcClient : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void callGetsItsReply()
    {
        Setup setup;
        QVERIFY(setup.start());
        QCOMPARE(setup.client.state(), IpcClient::State::Connected);
        QCOMPARE(setup.client.serverPath(), setup.fixture.sockPath);

        bool answered = false;
        IpcReply got;
        setup.client.call(QStringLiteral("calc"), QStringLiteral("sum"), QJsonArray{2, 3}, [&](const IpcReply& reply) {
            answered = true;
            got = reply;
        });
        // Never from inside the call that made the request.
        QVERIFY(!answered);
        QTRY_VERIFY(answered);
        QVERIFY(!got.isError());
        QCOMPARE(got.result, QJsonValue(5));
    }

    void errorsCarryCodeAndMessage()
    {
        Setup setup;
        QVERIFY(setup.start());

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
        QCOMPARE(got.at(1).errorCode, QString::fromLatin1(ErrorCode::NoSuchFn));
        QCOMPARE(got.at(2).errorCode, QString::fromLatin1(ErrorCode::InvalidArg));
    }

    void callsInFlightAnswerInAnyOrder()
    {
        Setup setup;
        QVERIFY(setup.start());

        Replies replies;
        qint64 first = 0;
        qint64 second = 0;
        qint64 third = 0;
        first = setup.client.call(QStringLiteral("calc"), QStringLiteral("later"), QJsonArray{1}, replies.take(&first));
        second =
            setup.client.call(QStringLiteral("calc"), QStringLiteral("later"), QJsonArray{2}, replies.take(&second));
        third =
            setup.client.call(QStringLiteral("calc"), QStringLiteral("sum"), QJsonArray{20, 22}, replies.take(&third));
        QVERIFY(first != second && second != third && first != third);

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
        QVERIFY(setup.start());

        IpcReply listed;
        IpcReply schema;
        IpcReply missing;
        setup.client.list([&](const IpcReply& reply) {
            listed = reply;
        });
        setup.client.schema(QStringLiteral("calc"), [&](const IpcReply& reply) {
            schema = reply;
        });
        setup.client.schema(QStringLiteral("none"), [&](const IpcReply& reply) {
            missing = reply;
        });
        QTRY_VERIFY(!missing.errorCode.isEmpty());
        QCOMPARE(missing.errorCode, QString::fromLatin1(ErrorCode::NoSuchTarget));
        QCOMPARE(listed.result, QJsonValue(QJsonArray{QStringLiteral("calc")}));

        const QJsonObject object = schema.result.toObject();
        QCOMPARE(object.value(QLatin1String("target")).toString(), QStringLiteral("calc"));
        QJsonObject sum;
        for (const QJsonValue function : object.value(QLatin1String("functions")).toArray()) {
            if (function.toObject().value(QLatin1String("name")).toString() == QLatin1String("sum")) {
                sum = function.toObject();
            }
        }
        const QJsonArray params = sum.value(QLatin1String("params")).toArray();
        QCOMPARE(params.size(), 2);
        QCOMPARE(params.at(0).toObject().value(QLatin1String("name")).toString(), QStringLiteral("a"));
        QCOMPARE(params.at(0).toObject().value(QLatin1String("type")).toString(), QStringLiteral("integer"));
    }

    void subscriptionStreamsEvents()
    {
        Setup setup;
        QVERIFY(setup.start());
        QSignalSpy events(&setup.client, &IpcClient::eventReceived);

        IpcReply acknowledged;
        bool answered = false;
        const qint64 subscription =
            setup.client.subscribe(QStringLiteral("calc"), QStringLiteral("countChanged"), [&](const IpcReply& reply) {
                answered = true;
                acknowledged = reply;
            });
        QTRY_VERIFY(answered);
        QVERIFY(!acknowledged.isError());

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
        bool unsubscribed = false;
        setup.client.unsubscribe(subscription, [&](const IpcReply& reply) {
            unsubscribed = !reply.isError();
        });
        QTRY_VERIFY(unsubscribed);
        QVERIFY(!setup.fixture.router.hasSubscribers(QStringLiteral("calc"), QStringLiteral("countChanged")));
        QCOMPARE(events.size(), 2);

        // A refused subscription starts no stream.
        IpcReply refused;
        setup.client.subscribe(QStringLiteral("calc"), QStringLiteral("nothing"), [&](const IpcReply& reply) {
            refused = reply;
        });
        QTRY_COMPARE(refused.errorCode, QString::fromLatin1(ErrorCode::NoSuchSignal));
    }

    void eventsToTheCallerHaveNoSubscription()
    {
        Setup setup;
        QVERIFY(setup.start());
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
        QVERIFY(setup.start());
        QSignalSpy ended(&setup.client, &IpcClient::subscriptionEnded);

        bool answered = false;
        const qint64 subscription =
            setup.client.subscribe(QStringLiteral("calc"), QStringLiteral("countChanged"), [&](const IpcReply&) {
                answered = true;
            });
        QTRY_VERIFY(answered);
        setup.fixture.router.unregisterTarget(QStringLiteral("calc"));
        QTRY_COMPARE(ended.size(), 1);
        QCOMPARE(ended.at(0).at(0).toLongLong(), subscription);
        QCOMPARE(ended.at(0).at(1).value<IpcReply>().errorCode, QString::fromLatin1(ErrorCode::NoSuchTarget));
    }

    void serverGoingAwayFailsPendingCalls()
    {
        Setup setup;
        QVERIFY(setup.start());
        QSignalSpy closed(&setup.client, &IpcClient::closed);

        QList<IpcReply> got;
        const auto keep = [&](const IpcReply& reply) {
            got.append(reply);
        };
        setup.client.call(QStringLiteral("calc"), QStringLiteral("later"), QJsonArray{1}, keep);
        setup.client.call(QStringLiteral("calc"), QStringLiteral("later"), QJsonArray{2}, keep);
        QTRY_COMPARE(setup.calc.m_waiting.size(), std::size_t(2));

        setup.fixture.router.stop();
        QTRY_COMPARE(closed.size(), 1);
        QCOMPARE(got.size(), 2);
        for (const IpcReply& reply : std::as_const(got)) {
            QCOMPARE(reply.errorCode, QString::fromLatin1(ClientErrorCode::ConnectionClosed));
        }
        QCOMPARE(setup.client.state(), IpcClient::State::Unconnected);

        // Without a connection a request fails at once, from the event loop.
        IpcReply late;
        setup.client.call(QStringLiteral("calc"), QStringLiteral("sum"), QJsonArray{1, 2}, [&](const IpcReply& reply) {
            late = reply;
        });
        QVERIFY(late.errorCode.isEmpty());
        QTRY_COMPARE(late.errorCode, QString::fromLatin1(ClientErrorCode::NotConnected));
        QTest::qWait(50);
        QCOMPARE(closed.size(), 1);

        // The client connects again once the router is back.
        QVERIFY(setup.fixture.router.start(setup.fixture.sockPath));
        QSignalSpy connected(&setup.client, &IpcClient::connected);
        QVERIFY(setup.client.connectToServer(setup.fixture.sockPath));
        IpcReply again;
        setup.client.call(QStringLiteral("calc"), QStringLiteral("sum"), QJsonArray{1, 2}, [&](const IpcReply& reply) {
            again = reply;
        });
        QTRY_COMPARE(again.result, QJsonValue(3));
        QCOMPARE(connected.size(), 1);
    }

    void closeFailsPendingCalls()
    {
        Setup setup;
        QVERIFY(setup.start());
        QSignalSpy closed(&setup.client, &IpcClient::closed);

        IpcReply got;
        setup.client.call(QStringLiteral("calc"), QStringLiteral("later"), QJsonArray{1}, [&](const IpcReply& reply) {
            got = reply;
            // The callbacks run before closed().
            QCOMPARE(closed.size(), 0);
        });
        QTRY_COMPARE(setup.calc.m_waiting.size(), std::size_t(1));
        setup.client.close();
        QCOMPARE(setup.client.state(), IpcClient::State::Unconnected);
        QVERIFY(got.errorCode.isEmpty());
        QTRY_COMPARE(closed.size(), 1);
        QCOMPARE(got.errorCode, QString::fromLatin1(ClientErrorCode::ConnectionClosed));
        QTest::qWait(50);
        QCOMPARE(closed.size(), 1);
    }

    void connectFailureFailsQueuedRequests()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        IpcClient client;
        QSignalSpy closed(&client, &IpcClient::closed);
        QSignalSpy connected(&client, &IpcClient::connected);
        QVERIFY(client.connectToServer(QDir(dir.path()).filePath(QStringLiteral("none.sock"))));
        IpcReply got;
        client.list([&](const IpcReply& reply) {
            got = reply;
        });
        QTRY_COMPARE(closed.size(), 1);
        QVERIFY(closed.at(0).at(0).toString().startsWith(QLatin1String("cannot connect to ")));
        // A missing socket fails inside connectToServer() already, and the
        // request then finds no connection.
        QVERIFY(got.errorCode == QLatin1String(ClientErrorCode::ConnectionClosed)
                || got.errorCode == QLatin1String(ClientErrorCode::NotConnected));
        QCOMPARE(connected.size(), 0);
        QCOMPARE(client.state(), IpcClient::State::Unconnected);
    }

    void requestsWaitForTheConnection()
    {
        // Requests made right after connectToServer() go out once connected.
        RouterFixture fixture;
        Calc calc{fixture.router};
        QVERIFY(fixture.router.registerTarget(QStringLiteral("calc"), &calc));
        QVERIFY(fixture.router.start(fixture.sockPath));
        IpcClient client;
        QVERIFY(client.connectToServer(fixture.sockPath));
        QTest::ignoreMessage(QtWarningMsg,
                             "PhosphorIpc::IpcClient::connectToServer: the client is connecting or connected already");
        QVERIFY(!client.connectToServer(fixture.sockPath));
        IpcReply got;
        client.call(QStringLiteral("calc"), QStringLiteral("sum"), QJsonArray{4, 5}, [&](const IpcReply& reply) {
            got = reply;
        });
        QTRY_COMPARE(got.result, QJsonValue(9));
    }

    void writeQueueIsBounded()
    {
        // A server that accepts and never reads: the requests pile up in the
        // client until the cap refuses one.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QLocalServer server;
        QVERIFY(server.listen(QDir(dir.path()).filePath(QStringLiteral("deaf.sock"))));
        IpcClient client;
        QSignalSpy connected(&client, &IpcClient::connected);
        QVERIFY(client.connectToServer(server.fullServerName()));
        QVERIFY(connected.wait(2000));

        const QString blob(1024 * 1024, QLatin1Char('x'));
        QString code;
        int sent = 0;
        while (code.isEmpty() && sent < 64) {
            client.call(QStringLiteral("t"), QStringLiteral("f"), QJsonArray{blob}, [&](const IpcReply& reply) {
                if (code.isEmpty()) {
                    code = reply.errorCode;
                }
            });
            ++sent;
            QCoreApplication::processEvents();
        }
        QCOMPARE(code, QString::fromLatin1(ClientErrorCode::WriteQueueFull));
        QVERIFY(sent > 1);
        QVERIFY(sent <= 18);
    }

    void destroyingDropsCallbacks()
    {
        Setup setup;
        QVERIFY(setup.start());
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
        QTest::qWait(50);
        QVERIFY(!ran);
    }
};

QTEST_GUILESS_MAIN(TestPhosphorIpcClient)
#include "test_phosphor_ipc_client.moc"
