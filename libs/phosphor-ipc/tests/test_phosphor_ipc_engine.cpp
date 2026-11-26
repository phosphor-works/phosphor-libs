// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: LGPL-2.1-or-later

// IpcEngine attaches an IpcRouter to a QQmlEngine via a dynamic
// property; IpcTarget instances look the router up at
// componentComplete. The unit here exercises the install / uninstall
// / routerFor surface against bogus inputs (null engine, null router,
// replacement, foreign property values) so misconfiguration surfaces
// as a qWarning rather than a silent dead binding. The last tests call
// functions that an IpcTarget declares in QML through the router.

#include <PhosphorIpc/IpcEngine.h>
#include <PhosphorIpc/IpcProtocol.h>
#include <PhosphorIpc/IpcRouter.h>
#include <PhosphorIpc/IpcTarget.h>

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QMetaObject>
#include <QObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QStringList>
#include <QVariantMap>
#include <QTest>
#include <QVariant>
#include <QtCore/qtclasshelpermacros.h>

#include <memory>

using namespace PhosphorIpc;

class TestPhosphorIpcEngine : public QObject
{
    Q_OBJECT
public:
    Q_DISABLE_COPY_MOVE(TestPhosphorIpcEngine)
    TestPhosphorIpcEngine() = default;
private Q_SLOTS:
    void initTestCase();
    void install_storesRouterOnEngine();
    void install_idempotentSameRouterSameEngine();
    void install_replacesPriorRouterWithWarning();
    void install_rejectsNullEngine();
    void install_rejectsNullRouter();
    void uninstall_clearsBinding();
    void uninstall_nullEngineIsNoOp();
    void routerFor_nullEngineReturnsNullptr();
    void routerFor_returnsNullptrForForeignProperty();
    void routerFor_returnsNullptrAfterRouterDestroyed();
    void install_afterUninstallStillWatchesTheRouter();
    void qmlFunctionFailsItsCall();
    void qmlUntypedAndTypedFunctionsAreCallable();
    void qmlFunctionOfADerivedComponentHidesTheBase();
};

void TestPhosphorIpcEngine::initTestCase()
{
    // The type that the QML tests import, one time for the process.
    qmlRegisterType<IpcTarget>("PhosphorIpcTest", 1, 0, "IpcTarget");
}

void TestPhosphorIpcEngine::install_storesRouterOnEngine()
{
    QQmlEngine engine;
    IpcRouter router;
    IpcEngine::install(&engine, &router);
    QCOMPARE(IpcEngine::routerFor(&engine), &router);
}

void TestPhosphorIpcEngine::install_idempotentSameRouterSameEngine()
{
    QQmlEngine engine;
    IpcRouter router;
    IpcEngine::install(&engine, &router);
    // Re-installing the SAME router on the SAME engine must NOT
    // emit any warning; if it did, repeated startup paths (e.g.
    // engine reload) would spam the console.
    QTest::failOnWarning();
    IpcEngine::install(&engine, &router);
    QCOMPARE(IpcEngine::routerFor(&engine), &router);
}

void TestPhosphorIpcEngine::install_replacesPriorRouterWithWarning()
{
    QQmlEngine engine;
    IpcRouter routerA;
    IpcRouter routerB;
    IpcEngine::install(&engine, &routerA);
    QTest::ignoreMessage(QtWarningMsg, "PhosphorIpc::IpcEngine::install: replacing existing router on engine");
    IpcEngine::install(&engine, &routerB);
    QCOMPARE(IpcEngine::routerFor(&engine), &routerB);
}

void TestPhosphorIpcEngine::install_rejectsNullEngine()
{
    IpcRouter router;
    QTest::ignoreMessage(QtWarningMsg, "PhosphorIpc::IpcEngine::install: null engine ignored");
    IpcEngine::install(nullptr, &router);
    // No crash, no side effects we can observe directly; the
    // ignoreMessage above pins the diagnostic surface.
}

void TestPhosphorIpcEngine::install_rejectsNullRouter()
{
    const char* const warning =
        "PhosphorIpc::IpcEngine::install: null router; call uninstall() to drop the binding explicitly";
    QQmlEngine engine;
    QTest::ignoreMessage(QtWarningMsg, warning);
    IpcEngine::install(&engine, nullptr);
    QCOMPARE(IpcEngine::routerFor(&engine), nullptr);

    // Null-router install must NOT silently uninstall a previously
    // installed binding.
    IpcRouter router;
    IpcEngine::install(&engine, &router);
    QTest::ignoreMessage(QtWarningMsg, warning);
    IpcEngine::install(&engine, nullptr);
    QCOMPARE(IpcEngine::routerFor(&engine), &router);
}

void TestPhosphorIpcEngine::uninstall_clearsBinding()
{
    QQmlEngine engine;
    IpcRouter router;
    IpcEngine::install(&engine, &router);
    IpcEngine::uninstall(&engine);
    QCOMPARE(IpcEngine::routerFor(&engine), nullptr);
}

void TestPhosphorIpcEngine::uninstall_nullEngineIsNoOp()
{
    IpcEngine::uninstall(nullptr); // must not crash
}

void TestPhosphorIpcEngine::routerFor_nullEngineReturnsNullptr()
{
    QCOMPARE(IpcEngine::routerFor(nullptr), nullptr);
}

void TestPhosphorIpcEngine::routerFor_returnsNullptrForForeignProperty()
{
    // Simulate a different consumer scribbling something else into
    // the dynamic-property slot. routerFor must qobject_cast back to
    // IpcRouter and return nullptr on a mismatch rather than crashing.
    // Declare `foreign` BEFORE `engine` so destruction order has the
    // QObject* held in the QVariant outlive the engine that holds
    // it (defensive against any Qt-internal property cleanup that
    // might deref the held pointer at engine teardown).
    QObject foreign;
    QQmlEngine engine;
    // The name of the slot is the library's own. install() shows it, so
    // the test still writes the right slot after a rename.
    const QList<QByteArray> before = engine.dynamicPropertyNames();
    QByteArray slot;
    {
        IpcRouter router;
        IpcEngine::install(&engine, &router);
        const QList<QByteArray> during = engine.dynamicPropertyNames();
        for (const QByteArray& name : during) {
            if (!before.contains(name)) {
                slot = name;
            }
        }
        IpcEngine::uninstall(&engine);
    }
    QVERIFY(!slot.isEmpty());
    engine.setProperty(slot.constData(), QVariant::fromValue<QObject*>(&foreign));
    QCOMPARE(engine.property(slot.constData()).value<QObject*>(), &foreign);
    QCOMPARE(IpcEngine::routerFor(&engine), nullptr);
}

void TestPhosphorIpcEngine::routerFor_returnsNullptrAfterRouterDestroyed()
{
    // install() wires a destroyed-signal watcher on the router so
    // that when the router dies before uninstall() is called, the
    // engine property is auto-cleared. Without that wire, the
    // QVariant<QObject*> would hold a dangling pointer and
    // routerFor() would qobject_cast freed memory (UB).
    QQmlEngine engine;
    auto router = std::make_unique<IpcRouter>();
    IpcEngine::install(&engine, router.get());
    QCOMPARE(IpcEngine::routerFor(&engine), router.get());
    router.reset(); // ~IpcRouter() fires destroyed → cleanup
    QCOMPARE(IpcEngine::routerFor(&engine), nullptr);
}

void TestPhosphorIpcEngine::install_afterUninstallStillWatchesTheRouter()
{
    // After an install/uninstall cycle the destroyed-watcher contract
    // continues to work: a router destroyed after the second install
    // still clears the engine property.
    QQmlEngine engine;
    {
        auto routerA = std::make_unique<IpcRouter>();
        IpcEngine::install(&engine, routerA.get());
        IpcEngine::uninstall(&engine);
    }
    auto routerB = std::make_unique<IpcRouter>();
    IpcEngine::install(&engine, routerB.get());
    QCOMPARE(IpcEngine::routerFor(&engine), routerB.get());
    routerB.reset();
    QCOMPARE(IpcEngine::routerFor(&engine), nullptr);
}

void TestPhosphorIpcEngine::qmlFunctionFailsItsCall()
{
    // A function declared in QML runs through the QML engine's own
    // metaobject. sendErrorReply must still reach the router's call.
    IpcRouter router;
    QQmlEngine engine;
    IpcEngine::install(&engine, &router);
    QQmlComponent component(&engine);
    component.setData(R"(import PhosphorIpcTest
IpcTarget {
    target: "theme"
    function setMode(mode) {
        if (mode !== "dark") {
            sendErrorReply("mode is dark, not " + mode)
            return "dropped"
        }
        return mode
    }
})",
                      QUrl());
    std::unique_ptr<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    QCOMPARE(router.target(QStringLiteral("theme")), object.get());

    IpcRouter::InvokeOutcome outcome = IpcRouter::InvokeOutcome::Ok;
    QString message;
    const QVariant failed =
        router.invoke(QStringLiteral("theme"), QStringLiteral("setMode"), {QStringLiteral("blue")}, &outcome, &message);
    QCOMPARE(outcome, IpcRouter::InvokeOutcome::CallFailed);
    QCOMPARE(message, QStringLiteral("mode is dark, not blue"));
    QVERIFY(!failed.isValid());

    const QVariant ok =
        router.invoke(QStringLiteral("theme"), QStringLiteral("setMode"), {QStringLiteral("dark")}, &outcome);
    QCOMPARE(outcome, IpcRouter::InvokeOutcome::Ok);
    QCOMPARE(ok.toString(), QStringLiteral("dark"));
}

void TestPhosphorIpcEngine::qmlUntypedAndTypedFunctionsAreCallable()
{
    // An untyped QML function takes and returns QVariant. The router hands
    // the incoming variant over as it is and reads the returned one.
    IpcRouter router;
    QQmlEngine engine;
    IpcEngine::install(&engine, &router);
    QQmlComponent component(&engine);
    component.setData(R"(import PhosphorIpcTest
IpcTarget {
    target: "calc"
    function untyped(a, b) { return { sum: a + b, kinds: [typeof a, typeof b], none: null } }
    function typed(a: int, b: int): int { return a * b }
    function nothing(text) { }
})",
                      QUrl());
    std::unique_ptr<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));

    IpcRouter::InvokeOutcome outcome = IpcRouter::InvokeOutcome::NoSuchFn;
    QString message;
    const QVariant returned =
        router.invoke(QStringLiteral("calc"), QStringLiteral("untyped"), {2, 3.5}, &outcome, &message);
    QVERIFY2(outcome == IpcRouter::InvokeOutcome::Ok, qPrintable(message));
    // The reply on the wire carries the JavaScript object as JSON.
    QCOMPARE(variantToJson(returned),
             QJsonValue(
                 QJsonObject{{QStringLiteral("sum"), 5.5},
                             {QStringLiteral("kinds"), QJsonArray{QStringLiteral("number"), QStringLiteral("number")}},
                             {QStringLiteral("none"), QJsonValue::Null}}));
    const QVariantMap untyped = returned.toMap();
    QCOMPARE(untyped.value(QStringLiteral("sum")).toDouble(), 5.5);
    QCOMPARE(untyped.value(QStringLiteral("kinds")).toStringList(),
             (QStringList{QStringLiteral("number"), QStringLiteral("number")}));

    QCOMPARE(router.invoke(QStringLiteral("calc"), QStringLiteral("typed"), {4, 5}, &outcome).toInt(), 20);
    QCOMPARE(outcome, IpcRouter::InvokeOutcome::Ok);

    router.invoke(QStringLiteral("calc"), QStringLiteral("nothing"), {QStringLiteral("x")}, &outcome, &message);
    QVERIFY2(outcome == IpcRouter::InvokeOutcome::Ok, qPrintable(message));
}

void TestPhosphorIpcEngine::qmlFunctionOfADerivedComponentHidesTheBase()
{
    // A component that declares a function of its base component again
    // replaces it for every caller in QML, whatever the parameters. The
    // router must call the same function, not the one of the base.
    IpcRouter router;
    QQmlEngine engine;
    IpcEngine::install(&engine, &router);
    QQmlComponent component(&engine);
    component.setData(R"(import QtQml
import PhosphorIpcTest
QtObject {
    component Base: IpcTarget {
        function same(pane) { return "base-same" }
        function retyped(pane: int): string { return "base-retyped" }
        function grown(a) { return "base-grown" }
        function inherited() { return "base-inherited" }
    }
    component Derived: Base {
        function same(pane) { return "derived-same" }
        function retyped(pane: string): string { return "derived-retyped" }
        function grown(a, b) { return "derived-grown" }
    }
    property Derived item: Derived { target: "layers" }
    function viaQml() { return [item.same(1), item.retyped("x"), item.grown(1, 2), item.inherited()] }
})",
                      QUrl());
    std::unique_ptr<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    QVERIFY(router.target(QStringLiteral("layers")));

    // What QML itself calls.
    QVariant viaQml;
    QVERIFY(QMetaObject::invokeMethod(object.get(), "viaQml", Q_RETURN_ARG(QVariant, viaQml)));
    const QStringList expected{QStringLiteral("derived-same"), QStringLiteral("derived-retyped"),
                               QStringLiteral("derived-grown"), QStringLiteral("base-inherited")};
    QCOMPARE(variantToJson(viaQml).toVariant().toStringList(), expected);

    // The router calls the same functions.
    IpcRouter::InvokeOutcome outcome = IpcRouter::InvokeOutcome::NoSuchFn;
    QString message;
    const auto call = [&](const QString& fn, const QVariantList& args) {
        return router.invoke(QStringLiteral("layers"), fn, args, &outcome, &message).toString();
    };
    QCOMPARE(call(QStringLiteral("same"), {1}), expected.at(0));
    QVERIFY2(outcome == IpcRouter::InvokeOutcome::Ok, qPrintable(message));
    QCOMPARE(call(QStringLiteral("retyped"), {QStringLiteral("x")}), expected.at(1));
    QVERIFY2(outcome == IpcRouter::InvokeOutcome::Ok, qPrintable(message));
    QCOMPARE(call(QStringLiteral("grown"), {1, 2}), expected.at(2));
    QVERIFY2(outcome == IpcRouter::InvokeOutcome::Ok, qPrintable(message));
    QCOMPARE(call(QStringLiteral("inherited"), {}), expected.at(3));
    QVERIFY2(outcome == IpcRouter::InvokeOutcome::Ok, qPrintable(message));

    // The function of the base with one parameter is hidden, so one
    // argument matches nothing: the router does not call a function that
    // QML itself would never reach.
    call(QStringLiteral("grown"), {1});
    QCOMPARE(outcome, IpcRouter::InvokeOutcome::ArgCountMismatch);

    // The schema lists each name one time, with the parameters of the
    // derived component.
    QHash<QString, int> parameterCounts;
    const QJsonArray functions =
        router.schemaFor(QStringLiteral("layers")).value(QStringLiteral("functions")).toArray();
    for (const QJsonValue function : functions) {
        const QString name = function.toObject().value(QStringLiteral("name")).toString();
        QVERIFY2(!parameterCounts.contains(name), qPrintable(name));
        parameterCounts.insert(name, int(function.toObject().value(QStringLiteral("params")).toArray().size()));
    }
    QCOMPARE(parameterCounts,
             (QHash<QString, int>{{QStringLiteral("same"), 1},
                                  {QStringLiteral("retyped"), 1},
                                  {QStringLiteral("grown"), 2},
                                  {QStringLiteral("inherited"), 0}}));
}

QTEST_GUILESS_MAIN(TestPhosphorIpcEngine)
#include "test_phosphor_ipc_engine.moc"
