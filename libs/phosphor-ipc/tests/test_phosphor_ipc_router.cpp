// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: LGPL-2.1-or-later

// In-process IpcRouter tests. Exercise registerTarget / unregisterTarget /
// invoke / listTargets / schemaFor against a hand-built QObject "target"
// that exposes a few Q_INVOKABLE methods. Socket-level wire roundtrips
// live in test_phosphor_ipc_e2e.cpp.

#include <PhosphorIpc/IpcCallContext.h>
#include <PhosphorIpc/IpcProtocol.h>
#include <PhosphorIpc/IpcRouter.h>
#include <PhosphorIpc/IpcTarget.h>

#include <QByteArray>
#include <QChar>
#include <QFloat16>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <QTest>
#include <QVariant>
#include <QVariantList>
#include <QtCore/qtclasshelpermacros.h>
#include <QtEnvironmentVariables>

#include <limits>

using namespace PhosphorIpc;

namespace {

class FakeTarget : public QObject
{
    Q_OBJECT
public:
    explicit FakeTarget(QObject* parent = nullptr)
        : QObject(parent)
    {
    }
    Q_DISABLE_COPY_MOVE(FakeTarget)

    Q_INVOKABLE QString sayHello(const QString& name)
    {
        return QStringLiteral("Hello, %1").arg(name);
    }
    Q_INVOKABLE int add(int a, int b)
    {
        return a + b;
    }
    Q_INVOKABLE void noReturn(const QString& s)
    {
        m_lastNoReturn = s;
    }
    Q_INVOKABLE QString readLast() const
    {
        return m_lastNoReturn;
    }
    // Two overloads of the same name. The router must dispatch by
    // arity (parameterCount), not by first-by-name.
    Q_INVOKABLE QString combine(const QString& a)
    {
        return QStringLiteral("one:%1").arg(a);
    }
    Q_INVOKABLE QString combine(const QString& a, const QString& b)
    {
        return QStringLiteral("two:%1+%2").arg(a, b);
    }

protected:
    // Protected Q_INVOKABLE — subclass-only convention. The router
    // and schema generator must NOT expose this on the wire.
    Q_INVOKABLE QString protectedSecret()
    {
        return QStringLiteral("forbidden");
    }

private:
    QString m_lastNoReturn;
};

// A class and a subclass that declares some of its methods again.
class LayeredBase : public QObject
{
    Q_OBJECT
public:
    explicit LayeredBase(QObject* parent = nullptr)
        : QObject(parent)
    {
    }
    Q_DISABLE_COPY_MOVE(LayeredBase)

    Q_INVOKABLE virtual QString overridden(int)
    {
        return QStringLiteral("base-overridden");
    }
    Q_INVOKABLE QString redeclared(int)
    {
        return QStringLiteral("base-redeclared");
    }
    Q_INVOKABLE QString grown(int)
    {
        return QStringLiteral("base-grown");
    }
    Q_INVOKABLE QString retyped(int)
    {
        return QStringLiteral("base-retyped");
    }
    Q_INVOKABLE QString inherited()
    {
        return QStringLiteral("base-inherited");
    }
    Q_INVOKABLE QString withdrawn()
    {
        return QStringLiteral("base-withdrawn");
    }
};

class LayeredDerived : public LayeredBase
{
    Q_OBJECT
public:
    explicit LayeredDerived(QObject* parent = nullptr)
        : LayeredBase(parent)
    {
    }
    Q_DISABLE_COPY_MOVE(LayeredDerived)

    Q_INVOKABLE QString overridden(int) override
    {
        return QStringLiteral("derived-overridden");
    }
    // Hides the method of the base, which is not virtual.
    Q_INVOKABLE QString redeclared(int)
    {
        return QStringLiteral("derived-redeclared");
    }
    // Another count of parameters, and another type for the same count.
    Q_INVOKABLE QString grown(int, int)
    {
        return QStringLiteral("derived-grown");
    }
    Q_INVOKABLE QString retyped(const QString&)
    {
        return QStringLiteral("derived-retyped");
    }

protected:
    // A subclass that takes a method off the wire.
    Q_INVOKABLE QString withdrawn()
    {
        return QStringLiteral("derived-withdrawn");
    }
};

// Methods that give their argument back, one for each number type.
class Numbers : public QObject
{
    Q_OBJECT
public:
    explicit Numbers(QObject* parent = nullptr)
        : QObject(parent)
    {
    }
    Q_DISABLE_COPY_MOVE(Numbers)

    Q_INVOKABLE int takesInt(int value)
    {
        return value;
    }
    Q_INVOKABLE uint takesUInt(uint value)
    {
        return value;
    }
    Q_INVOKABLE short takesShort(short value)
    {
        return value;
    }
    Q_INVOKABLE quint8 takesByte(quint8 value)
    {
        return value;
    }
    Q_INVOKABLE char takesChar(char value)
    {
        return value;
    }
    Q_INVOKABLE qint64 takesLong(qint64 value)
    {
        return value;
    }
    Q_INVOKABLE quint64 takesULong(quint64 value)
    {
        return value;
    }
    Q_INVOKABLE float takesFloat(float value)
    {
        return value;
    }
    Q_INVOKABLE double takesDouble(double value)
    {
        return value;
    }
    Q_INVOKABLE qfloat16 takesHalf(qfloat16 value)
    {
        return value;
    }
    Q_INVOKABLE qulonglong takesUnsignedLong(unsigned long value)
    {
        return value;
    }
};

// A target that fails its own calls through the call context.
class FailingTarget : public QObject, protected IpcCallContext
{
    Q_OBJECT
public:
    explicit FailingTarget(QObject* parent = nullptr)
        : QObject(parent)
    {
    }
    Q_DISABLE_COPY_MOVE(FailingTarget)

    Q_INVOKABLE int doubled(int value)
    {
        if (value < 0) {
            sendErrorReply(QStringLiteral("negative: %1").arg(value));
            return -1;
        }
        return value * 2;
    }
    Q_INVOKABLE bool fromIpc() const
    {
        return calledFromIpc();
    }
    Q_INVOKABLE void failTwice()
    {
        sendErrorReply(QStringLiteral("first"));
        sendErrorReply(QStringLiteral("second"));
    }
    // Fails, then deletes its own target before the router clears the call.
    Q_INVOKABLE void failAndDelete()
    {
        sendErrorReply(QStringLiteral("gone"));
        delete this;
    }
    // A direct C++ call: no wire call runs.
    bool directCall()
    {
        sendErrorReply(QStringLiteral("ignored"));
        return calledFromIpc();
    }
    // Another context object, called from inside this one's wire call.
    Q_INVOKABLE bool otherSeesNoCall()
    {
        return m_other && !m_other->directCall();
    }
    FailingTarget* m_other = nullptr;
};

// The C++ shape of a QML IpcTarget with a function on it.
class FailingIpcTarget : public IpcTarget
{
    Q_OBJECT
public:
    Q_INVOKABLE QString mode(const QString& value)
    {
        if (value != QLatin1String("dark")) {
            sendErrorReply(QStringLiteral("mode is dark"));
        }
        return value;
    }
};

} // namespace

class TestPhosphorIpcRouter : public QObject
{
    Q_OBJECT
public:
    Q_DISABLE_COPY_MOVE(TestPhosphorIpcRouter)
    TestPhosphorIpcRouter() = default;
private Q_SLOTS:
    void register_addsAndEmits();
    void register_duplicateRejected();
    void register_idempotentSameObjectReturnsTrue();
    void register_nullObjectRejected();
    void register_emptyNameRejected();
    void unregister_removesAndEmits();
    void unregister_unknownNoOp();
    void unregister_ownershipCheckPreservesIncumbent();
    void listTargets_filtersDeletedObjects();
    void invoke_returnsStringResult();
    void invoke_returnsIntResult();
    void invoke_voidReturnSucceeds();
    void invoke_unknownTargetError();
    void invoke_unknownFnError();
    void invoke_argCountMismatchError();
    void invoke_argTypeCoercion();
    void invoke_argTypeCoercionFailure();
    void invoke_outcomeOkOnSuccess();
    void invoke_overloadDispatchesByArity();
    void invoke_overloadNoMatchingArityReturnsArgCountMismatch();
    void invoke_protectedQInvokableRejected();
    void invoke_derivedDeclarationHidesTheBase();
    void invoke_nameIsComparedAsText();
    void invoke_numberInRange_data();
    void invoke_numberInRange();
    void invoke_numberOutOfRange_data();
    void invoke_numberOutOfRange();
    void invoke_ipcTargetWrapperMethodsRejected();
    void invoke_callContextFailsTheCall();
    void invoke_callContextOnlyDuringTheCall();
    void invoke_callContextSurvivesDeletedTarget();
    void invoke_ipcTargetSendErrorReply();
    void schemaFor_unknownReturnsEmpty();
    void schemaFor_listsFunctions();
    void schemaFor_skipsProtectedQInvokable();
    void schemaFor_listsAHiddenNameOnce();
    void schemaFor_charTypesAreIntegers();
    void defaultSocketPath_isInTheRuntimeDirectory();
};

void TestPhosphorIpcRouter::register_addsAndEmits()
{
    IpcRouter router;
    QSignalSpy spy(&router, &IpcRouter::targetRegistered);
    FakeTarget t;
    QVERIFY(router.registerTarget(QStringLiteral("greet"), &t));
    QCOMPARE(router.listTargets(), QStringList{QStringLiteral("greet")});
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.first().at(0).toString(), QStringLiteral("greet"));
}

void TestPhosphorIpcRouter::register_duplicateRejected()
{
    IpcRouter router;
    FakeTarget t1;
    FakeTarget t2;
    QVERIFY(router.registerTarget(QStringLiteral("greet"), &t1));
    QTest::ignoreMessage(QtWarningMsg, "PhosphorIpc::IpcRouter::registerTarget: duplicate target 'greet' ignored");
    // Duplicate-name registration returns false; the existing
    // binding is preserved (first registration wins).
    QVERIFY(!router.registerTarget(QStringLiteral("greet"), &t2));
    QCOMPARE(router.listTargets().size(), 1);
    QCOMPARE(router.target(QStringLiteral("greet")), &t1);
}

void TestPhosphorIpcRouter::register_idempotentSameObjectReturnsTrue()
{
    // Re-registering the SAME QObject under the SAME name is a
    // silent idempotent success (no warning, no signal re-emit).
    // The bool return is documented to be true on this path so a
    // wrapper like IpcTarget can blindly re-call register and treat
    // the result as authoritative ownership.
    IpcRouter router;
    QSignalSpy spy(&router, &IpcRouter::targetRegistered);
    FakeTarget t;
    QVERIFY(router.registerTarget(QStringLiteral("greet"), &t));
    QVERIFY(router.registerTarget(QStringLiteral("greet"), &t));
    QCOMPARE(spy.count(), 1); // exactly one register emit, not two
}

void TestPhosphorIpcRouter::register_nullObjectRejected()
{
    IpcRouter router;
    QTest::ignoreMessage(QtWarningMsg, "PhosphorIpc::IpcRouter::registerTarget: null object for 'greet' ignored");
    QVERIFY(!router.registerTarget(QStringLiteral("greet"), nullptr));
    QVERIFY(router.listTargets().isEmpty());
}

void TestPhosphorIpcRouter::register_emptyNameRejected()
{
    IpcRouter router;
    FakeTarget t;
    QTest::ignoreMessage(QtWarningMsg, "PhosphorIpc::IpcRouter::registerTarget: empty name ignored");
    QVERIFY(!router.registerTarget(QString(), &t));
    QVERIFY(router.listTargets().isEmpty());
}

void TestPhosphorIpcRouter::unregister_removesAndEmits()
{
    IpcRouter router;
    FakeTarget t;
    QVERIFY(router.registerTarget(QStringLiteral("greet"), &t));
    QSignalSpy spy(&router, &IpcRouter::targetUnregistered);
    router.unregisterTarget(QStringLiteral("greet"));
    QVERIFY(router.listTargets().isEmpty());
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.first().at(0).toString(), QStringLiteral("greet"));
}

void TestPhosphorIpcRouter::unregister_ownershipCheckPreservesIncumbent()
{
    // unregisterTarget(name, obj) must only tear down the binding
    // when the registry currently binds `name` to `obj`. This is
    // the safety net used by IpcTarget's destructor when its own
    // registerTarget was rejected as a duplicate; without it,
    // destruction would unregister the legitimate owner.
    IpcRouter router;
    FakeTarget t1;
    FakeTarget t2;
    QVERIFY(router.registerTarget(QStringLiteral("greet"), &t1));
    QTest::ignoreMessage(QtWarningMsg, "PhosphorIpc::IpcRouter::registerTarget: duplicate target 'greet' ignored");
    QVERIFY(!router.registerTarget(QStringLiteral("greet"), &t2));

    // Pretend t2 "thinks" it owns the slot and unregisters by name+ptr.
    // The registry must reject the call (mismatched ownership) and
    // keep t1's binding intact.
    QSignalSpy spy(&router, &IpcRouter::targetUnregistered);
    router.unregisterTarget(QStringLiteral("greet"), &t2);
    QCOMPARE(spy.count(), 0);
    QCOMPARE(router.target(QStringLiteral("greet")), &t1);

    // The 1-arg administrative overload (or 2-arg with nullptr)
    // tears down unconditionally.
    router.unregisterTarget(QStringLiteral("greet"));
    QCOMPARE(spy.count(), 1);
    QVERIFY(router.listTargets().isEmpty());
}

void TestPhosphorIpcRouter::unregister_unknownNoOp()
{
    IpcRouter router;
    QSignalSpy spy(&router, &IpcRouter::targetUnregistered);
    router.unregisterTarget(QStringLiteral("ghost"));
    QCOMPARE(spy.count(), 0);
}

void TestPhosphorIpcRouter::listTargets_filtersDeletedObjects()
{
    IpcRouter router;
    {
        FakeTarget t;
        QVERIFY(router.registerTarget(QStringLiteral("scoped"), &t));
        QCOMPARE(router.listTargets().size(), 1);
    }
    // t went out of scope; the QPointer auto-clears so listTargets
    // should report 0. Also pin the target() accessor to nullptr
    // so a future refactor that caches names independently of the
    // QPointer state would be caught.
    QVERIFY(router.listTargets().isEmpty());
    QCOMPARE(router.target(QStringLiteral("scoped")), nullptr);
}

void TestPhosphorIpcRouter::invoke_returnsStringResult()
{
    IpcRouter router;
    FakeTarget t;
    QVERIFY(router.registerTarget(QStringLiteral("greet"), &t));
    QString err;
    const QVariant r = router.invoke(QStringLiteral("greet"), QStringLiteral("sayHello"),
                                     QVariantList{QStringLiteral("nate")}, nullptr, &err);
    QVERIFY(err.isEmpty());
    QCOMPARE(r.toString(), QStringLiteral("Hello, nate"));
}

void TestPhosphorIpcRouter::invoke_returnsIntResult()
{
    IpcRouter router;
    FakeTarget t;
    QVERIFY(router.registerTarget(QStringLiteral("math"), &t));
    QString err;
    const QVariant r = router.invoke(QStringLiteral("math"), QStringLiteral("add"), QVariantList{3, 4}, nullptr, &err);
    QVERIFY(err.isEmpty());
    QCOMPARE(r.toInt(), 7);
}

void TestPhosphorIpcRouter::invoke_voidReturnSucceeds()
{
    IpcRouter router;
    FakeTarget t;
    QVERIFY(router.registerTarget(QStringLiteral("store"), &t));
    QString err;
    const QVariant r = router.invoke(QStringLiteral("store"), QStringLiteral("noReturn"),
                                     QVariantList{QStringLiteral("payload")}, nullptr, &err);
    QVERIFY(err.isEmpty());
    QVERIFY(!r.isValid()); // void return → invalid QVariant
    // Verify the side effect happened.
    const QVariant r2 =
        router.invoke(QStringLiteral("store"), QStringLiteral("readLast"), QVariantList{}, nullptr, &err);
    QCOMPARE(r2.toString(), QStringLiteral("payload"));
}

void TestPhosphorIpcRouter::invoke_unknownTargetError()
{
    // Assert on the structured InvokeOutcome rather than the
    // human-readable message. Message text is i18n / refactor
    // surface; the enum is the wire contract.
    IpcRouter router;
    QString err;
    IpcRouter::InvokeOutcome outcome = IpcRouter::InvokeOutcome::Ok;
    const QVariant r = router.invoke(QStringLiteral("ghost"), QStringLiteral("fn"), {}, &outcome, &err);
    QVERIFY(!r.isValid());
    QCOMPARE(outcome, IpcRouter::InvokeOutcome::NoSuchTarget);
    QVERIFY(!err.isEmpty()); // message is populated, but content not asserted
}

void TestPhosphorIpcRouter::invoke_unknownFnError()
{
    IpcRouter router;
    FakeTarget t;
    QVERIFY(router.registerTarget(QStringLiteral("greet"), &t));
    QString err;
    IpcRouter::InvokeOutcome outcome = IpcRouter::InvokeOutcome::Ok;
    const QVariant r = router.invoke(QStringLiteral("greet"), QStringLiteral("ghostFn"), {}, &outcome, &err);
    QVERIFY(!r.isValid());
    QCOMPARE(outcome, IpcRouter::InvokeOutcome::NoSuchFn);
    QVERIFY(!err.isEmpty());
}

void TestPhosphorIpcRouter::invoke_argCountMismatchError()
{
    IpcRouter router;
    FakeTarget t;
    QVERIFY(router.registerTarget(QStringLiteral("math"), &t));
    QString err;
    IpcRouter::InvokeOutcome outcome = IpcRouter::InvokeOutcome::Ok;
    // add() expects 2 args; pass 1.
    const QVariant r = router.invoke(QStringLiteral("math"), QStringLiteral("add"), QVariantList{1}, &outcome, &err);
    QVERIFY(!r.isValid());
    QCOMPARE(outcome, IpcRouter::InvokeOutcome::ArgCountMismatch);
    QVERIFY(!err.isEmpty());
}

void TestPhosphorIpcRouter::invoke_argTypeCoercion()
{
    IpcRouter router;
    FakeTarget t;
    QVERIFY(router.registerTarget(QStringLiteral("math"), &t));
    QString err;
    // add expects int; pass a string-shaped int, QVariant::convert
    // promotes "3" → 3 via QString::toInt, so this should succeed.
    const QVariant r = router.invoke(QStringLiteral("math"), QStringLiteral("add"),
                                     QVariantList{QStringLiteral("3"), QStringLiteral("4")}, nullptr, &err);
    QVERIFY(err.isEmpty());
    QCOMPARE(r.toInt(), 7);
}

void TestPhosphorIpcRouter::invoke_argTypeCoercionFailure()
{
    // Negative path for arg coercion: passing a non-convertible
    // QVariant for an int-typed param surfaces ArgConvertFailed,
    // which the wire dispatcher maps to INVALID_ARG.
    IpcRouter router;
    FakeTarget t;
    QVERIFY(router.registerTarget(QStringLiteral("math"), &t));
    QString err;
    IpcRouter::InvokeOutcome outcome = IpcRouter::InvokeOutcome::Ok;
    const QVariant r =
        router.invoke(QStringLiteral("math"), QStringLiteral("add"), QVariantList{QVariantMap{}, 4}, &outcome, &err);
    QVERIFY(!r.isValid());
    QCOMPARE(outcome, IpcRouter::InvokeOutcome::ArgConvertFailed);
    QVERIFY(!err.isEmpty());
}

void TestPhosphorIpcRouter::invoke_outcomeOkOnSuccess()
{
    IpcRouter router;
    FakeTarget t;
    QVERIFY(router.registerTarget(QStringLiteral("math"), &t));
    IpcRouter::InvokeOutcome outcome = IpcRouter::InvokeOutcome::InvokeFailed;
    router.invoke(QStringLiteral("math"), QStringLiteral("add"), QVariantList{1, 2}, &outcome);
    QCOMPARE(outcome, IpcRouter::InvokeOutcome::Ok);
}

void TestPhosphorIpcRouter::schemaFor_unknownReturnsEmpty()
{
    // Unknown target produces the empty-shaped schema document:
    // {target: "<name>", functions: [], signals: []}. Same contract
    // IpcSchemaGenerator::schemaFor(name, nullptr) honors, so
    // router-direct callers and the schema generator agree on the
    // shape (the wire dispatcher still returns NO_SUCH_TARGET error
    // frames for `schema` requests on unknown targets; see the
    // dispatcher tests).
    IpcRouter router;
    const QJsonObject s = router.schemaFor(QStringLiteral("ghost"));
    QCOMPARE(s.value(QStringLiteral("target")).toString(), QStringLiteral("ghost"));
    QCOMPARE(s.value(QStringLiteral("functions")).toArray().size(), 0);
    QCOMPARE(s.value(QStringLiteral("signals")).toArray().size(), 0);
}

void TestPhosphorIpcRouter::schemaFor_listsFunctions()
{
    IpcRouter router;
    FakeTarget t;
    QVERIFY(router.registerTarget(QStringLiteral("greet"), &t));
    const QJsonObject schema = router.schemaFor(QStringLiteral("greet"));
    QCOMPARE(schema.value(QStringLiteral("target")).toString(), QStringLiteral("greet"));
    QVERIFY(schema.value(QStringLiteral("functions")).isArray());
    QCOMPARE(schema.value(QStringLiteral("signals")).toArray().size(), 0);
    // Public Q_INVOKABLE methods: sayHello, add, noReturn, readLast,
    // combine(QString), combine(QString,QString) = 6 functions. The
    // protectedSecret method MUST NOT appear (filter-to-Public
    // pinned by schemaFor_skipsProtectedQInvokable below).
    QCOMPARE(schema.value(QStringLiteral("functions")).toArray().size(), 6);
}

void TestPhosphorIpcRouter::invoke_overloadDispatchesByArity()
{
    // FakeTarget declares Q_INVOKABLE combine(QString) and
    // combine(QString, QString). The router picks the overload
    // whose parameterCount matches args.size(); a regression that
    // returned the first-by-name match would dispatch combine(a)
    // for the 2-arg call (and surface ArgCountMismatch) or vice
    // versa.
    IpcRouter router;
    FakeTarget t;
    QVERIFY(router.registerTarget(QStringLiteral("c"), &t));
    QString err;

    const QVariant r1 = router.invoke(QStringLiteral("c"), QStringLiteral("combine"),
                                      QVariantList{QStringLiteral("alpha")}, nullptr, &err);
    QVERIFY(err.isEmpty());
    QCOMPARE(r1.toString(), QStringLiteral("one:alpha"));

    const QVariant r2 = router.invoke(QStringLiteral("c"), QStringLiteral("combine"),
                                      QVariantList{QStringLiteral("alpha"), QStringLiteral("beta")}, nullptr, &err);
    QVERIFY(err.isEmpty());
    QCOMPARE(r2.toString(), QStringLiteral("two:alpha+beta"));
}

void TestPhosphorIpcRouter::invoke_overloadNoMatchingArityReturnsArgCountMismatch()
{
    // FakeTarget declares combine(QString) and combine(QString, QString)
    // — but no 3-arg combine. A call with 3 args must surface
    // ArgCountMismatch citing the parameter count of the name-only
    // fallback metamethod (whichever overload findInvokableMethod
    // returned). Pins the "no overload matches" branch so a
    // regression that silently succeeded with arg truncation or
    // returned NoSuchFn (the wrong code) is caught.
    IpcRouter router;
    FakeTarget t;
    QVERIFY(router.registerTarget(QStringLiteral("c"), &t));
    QString err;
    IpcRouter::InvokeOutcome outcome = IpcRouter::InvokeOutcome::Ok;
    const QVariant r =
        router.invoke(QStringLiteral("c"), QStringLiteral("combine"),
                      QVariantList{QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")}, &outcome, &err);
    QVERIFY(!r.isValid());
    QCOMPARE(outcome, IpcRouter::InvokeOutcome::ArgCountMismatch);
    QVERIFY(!err.isEmpty());
}

void TestPhosphorIpcRouter::invoke_ipcTargetWrapperMethodsRejected()
{
    // IpcTarget declares its own Q_INVOKABLE methods (`emitEvent`)
    // and Q_PROPERTY signals (`targetChanged`) that are wrapper-
    // internal — they exist for QML to inject events / observe
    // property changes, NOT for remote wire callers. Without the
    // IpcTarget-aware introspection floor, a client could call
    // `phosphorctl call mytarget.emitEvent --arg signal=fake
    // --arg args=[...]` and inject arbitrary events into every
    // subscriber. The schema generator never advertises these
    // methods; the dispatcher MUST refuse to dispatch them so the
    // wire-callable surface matches what the schema advertised.
    IpcRouter router;
    IpcTarget t; // standalone, no QML — register manually below
    QVERIFY(router.registerTarget(QStringLiteral("wrapped"), &t));

    QString err;
    IpcRouter::InvokeOutcome outcome = IpcRouter::InvokeOutcome::Ok;
    const QVariant r = router.invoke(QStringLiteral("wrapped"), QStringLiteral("emitEvent"),
                                     QVariantList{QStringLiteral("targetChanged"), QVariantList{}}, &outcome, &err);
    QVERIFY(!r.isValid());
    QCOMPARE(outcome, IpcRouter::InvokeOutcome::NoSuchFn);
    QVERIFY(!err.isEmpty());

    // Same defense on the schema side: the IpcTarget wrapper's
    // methods MUST NOT appear in the advertised schema.
    const QJsonObject schema = router.schemaFor(QStringLiteral("wrapped"));
    const QJsonArray functions = schema.value(QStringLiteral("functions")).toArray();
    QStringList fnNames;
    for (const QJsonValue& v : functions) {
        fnNames.append(v.toObject().value(QStringLiteral("name")).toString());
    }
    QVERIFY2(!fnNames.contains(QStringLiteral("emitEvent")),
             "IpcTarget::emitEvent must not appear in the wire-visible schema");

    const QJsonArray signals_ = schema.value(QStringLiteral("signals")).toArray();
    QStringList sigNames;
    for (const QJsonValue& v : signals_) {
        sigNames.append(v.toObject().value(QStringLiteral("name")).toString());
    }
    QVERIFY2(!sigNames.contains(QStringLiteral("targetChanged")),
             "IpcTarget::targetChanged must not appear in the wire-visible schema");
}

void TestPhosphorIpcRouter::invoke_protectedQInvokableRejected()
{
    // FakeTarget::protectedSecret is `protected: Q_INVOKABLE`. The
    // router filters to QMetaMethod::Public, so it must NOT be
    // callable on the wire even though the metaobject knows about
    // it. The expected outcome is NoSuchFn (the wire couldn't find
    // it among the Public methods), not InvokeFailed.
    IpcRouter router;
    FakeTarget t;
    QVERIFY(router.registerTarget(QStringLiteral("t"), &t));
    QString err;
    IpcRouter::InvokeOutcome outcome = IpcRouter::InvokeOutcome::Ok;
    const QVariant r = router.invoke(QStringLiteral("t"), QStringLiteral("protectedSecret"), {}, &outcome, &err);
    QVERIFY(!r.isValid());
    QCOMPARE(outcome, IpcRouter::InvokeOutcome::NoSuchFn);
    QVERIFY(!err.isEmpty());
}

void TestPhosphorIpcRouter::schemaFor_skipsProtectedQInvokable()
{
    // Pin the schema-side filter: protected Q_INVOKABLE methods
    // must not appear in the generated functions array.
    IpcRouter router;
    FakeTarget t;
    QVERIFY(router.registerTarget(QStringLiteral("t"), &t));
    const QJsonObject schema = router.schemaFor(QStringLiteral("t"));
    const QJsonArray fns = schema.value(QStringLiteral("functions")).toArray();
    QStringList names;
    for (const QJsonValue& v : fns) {
        names.append(v.toObject().value(QStringLiteral("name")).toString());
    }
    QVERIFY2(!names.contains(QStringLiteral("protectedSecret")),
             "protected Q_INVOKABLE must not appear in the wire-visible schema");
}

void TestPhosphorIpcRouter::invoke_callContextFailsTheCall()
{
    IpcRouter router;
    FailingTarget t;
    QVERIFY(router.registerTarget(QStringLiteral("f"), &t));

    IpcRouter::InvokeOutcome outcome = IpcRouter::InvokeOutcome::Ok;
    QString message;
    const QVariant failed = router.invoke(QStringLiteral("f"), QStringLiteral("doubled"), {-3}, &outcome, &message);
    QCOMPARE(outcome, IpcRouter::InvokeOutcome::CallFailed);
    QCOMPARE(message, QStringLiteral("negative: -3"));
    // The return value of a failed call is dropped.
    QVERIFY(!failed.isValid());

    // The next call starts clean.
    message.clear();
    const QVariant ok = router.invoke(QStringLiteral("f"), QStringLiteral("doubled"), {4}, &outcome, &message);
    QCOMPARE(outcome, IpcRouter::InvokeOutcome::Ok);
    QCOMPARE(ok.toInt(), 8);
    QVERIFY(message.isEmpty());

    // The last message wins.
    router.invoke(QStringLiteral("f"), QStringLiteral("failTwice"), {}, &outcome, &message);
    QCOMPARE(outcome, IpcRouter::InvokeOutcome::CallFailed);
    QCOMPARE(message, QStringLiteral("second"));
}

void TestPhosphorIpcRouter::invoke_callContextOnlyDuringTheCall()
{
    IpcRouter router;
    FailingTarget t;
    QVERIFY(router.registerTarget(QStringLiteral("f"), &t));
    QCOMPARE(router.invoke(QStringLiteral("f"), QStringLiteral("fromIpc"), {}).toBool(), true);
    // Outside a wire call, the context reports no call and ignores errors.
    QVERIFY(!t.fromIpc());
    QVERIFY(!t.directCall());
    IpcRouter::InvokeOutcome outcome = IpcRouter::InvokeOutcome::CallFailed;
    router.invoke(QStringLiteral("f"), QStringLiteral("doubled"), {1}, &outcome);
    QCOMPARE(outcome, IpcRouter::InvokeOutcome::Ok);

    // The call belongs to its target: another context object that the
    // method calls neither sees it nor fails it.
    FailingTarget other;
    t.m_other = &other;
    QCOMPARE(router.invoke(QStringLiteral("f"), QStringLiteral("otherSeesNoCall"), {}, &outcome).toBool(), true);
    QCOMPARE(outcome, IpcRouter::InvokeOutcome::Ok);
}

void TestPhosphorIpcRouter::invoke_callContextSurvivesDeletedTarget()
{
    IpcRouter router;
    auto* t = new FailingTarget;
    QVERIFY(router.registerTarget(QStringLiteral("f"), t));
    IpcRouter::InvokeOutcome outcome = IpcRouter::InvokeOutcome::Ok;
    QString message;
    router.invoke(QStringLiteral("f"), QStringLiteral("failAndDelete"), {}, &outcome, &message);
    QCOMPARE(outcome, IpcRouter::InvokeOutcome::CallFailed);
    QCOMPARE(message, QStringLiteral("gone"));
    QCOMPARE(router.target(QStringLiteral("f")), nullptr);
}

void TestPhosphorIpcRouter::invoke_ipcTargetSendErrorReply()
{
    IpcRouter router;
    FailingIpcTarget t;
    QVERIFY(router.registerTarget(QStringLiteral("q"), &t));
    IpcRouter::InvokeOutcome outcome = IpcRouter::InvokeOutcome::Ok;
    QString message;
    router.invoke(QStringLiteral("q"), QStringLiteral("mode"), {QStringLiteral("blue")}, &outcome, &message);
    QCOMPARE(outcome, IpcRouter::InvokeOutcome::CallFailed);
    QCOMPARE(message, QStringLiteral("mode is dark"));
    QCOMPARE(router.invoke(QStringLiteral("q"), QStringLiteral("mode"), {QStringLiteral("dark")}, &outcome).toString(),
             QStringLiteral("dark"));
    QCOMPARE(outcome, IpcRouter::InvokeOutcome::Ok);
    // sendErrorReply is IpcTarget's own method: not callable over the wire.
    router.invoke(QStringLiteral("q"), QStringLiteral("sendErrorReply"), {QStringLiteral("x")}, &outcome);
    QCOMPARE(outcome, IpcRouter::InvokeOutcome::NoSuchFn);
}

void TestPhosphorIpcRouter::invoke_derivedDeclarationHidesTheBase()
{
    // A method that a subclass declares again hides every declaration of
    // that name in its base classes, as in C++. The router calls what a
    // caller of the object itself would reach.
    IpcRouter router;
    LayeredDerived target;
    QVERIFY(router.registerTarget(QStringLiteral("layers"), &target));
    const auto call = [&](const QString& fn, const QVariantList& args, IpcRouter::InvokeOutcome* outcome) {
        QString message;
        const QVariant result = router.invoke(QStringLiteral("layers"), fn, args, outcome, &message);
        return result.isValid() ? result.toString() : message;
    };
    IpcRouter::InvokeOutcome outcome = IpcRouter::InvokeOutcome::NoSuchFn;

    QCOMPARE(call(QStringLiteral("overridden"), {1}, &outcome), QStringLiteral("derived-overridden"));
    QCOMPARE(outcome, IpcRouter::InvokeOutcome::Ok);
    QCOMPARE(call(QStringLiteral("redeclared"), {1}, &outcome), QStringLiteral("derived-redeclared"));
    QCOMPARE(outcome, IpcRouter::InvokeOutcome::Ok);
    QCOMPARE(call(QStringLiteral("retyped"), {QStringLiteral("x")}, &outcome), QStringLiteral("derived-retyped"));
    QCOMPARE(outcome, IpcRouter::InvokeOutcome::Ok);

    // The base declaration with one parameter is hidden, so one argument
    // matches nothing.
    QCOMPARE(call(QStringLiteral("grown"), {1, 2}, &outcome), QStringLiteral("derived-grown"));
    QCOMPARE(outcome, IpcRouter::InvokeOutcome::Ok);
    const QString mismatch = call(QStringLiteral("grown"), {1}, &outcome);
    QCOMPARE(outcome, IpcRouter::InvokeOutcome::ArgCountMismatch);
    QVERIFY2(mismatch.contains(QStringLiteral("expects 2, got 1")), qPrintable(mismatch));

    // A method that the subclass does not declare stays callable.
    QCOMPARE(call(QStringLiteral("inherited"), {}, &outcome), QStringLiteral("base-inherited"));
    QCOMPARE(outcome, IpcRouter::InvokeOutcome::Ok);

    // A protected declaration in the subclass takes the method off the
    // wire, although the base declares it public.
    call(QStringLiteral("withdrawn"), {}, &outcome);
    QCOMPARE(outcome, IpcRouter::InvokeOutcome::NoSuchFn);

    // An object of the base class alone has its own methods.
    LayeredBase base;
    QVERIFY(router.registerTarget(QStringLiteral("base"), &base));
    QCOMPARE(router.invoke(QStringLiteral("base"), QStringLiteral("grown"), {1}, &outcome).toString(),
             QStringLiteral("base-grown"));
    QCOMPARE(router.invoke(QStringLiteral("base"), QStringLiteral("withdrawn"), {}, &outcome).toString(),
             QStringLiteral("base-withdrawn"));
}

void TestPhosphorIpcRouter::invoke_nameIsComparedAsText()
{
    // A name with a lone surrogate has the same UTF-8 bytes as the name
    // without it once the surrogate is dropped. It is another name.
    IpcRouter router;
    FakeTarget target;
    QVERIFY(router.registerTarget(QStringLiteral("greet"), &target));
    IpcRouter::InvokeOutcome outcome = IpcRouter::InvokeOutcome::Ok;
    QString name = QStringLiteral("readLast");
    name.append(QChar(0xD800));
    router.invoke(QStringLiteral("greet"), name, {}, &outcome);
    QCOMPARE(outcome, IpcRouter::InvokeOutcome::NoSuchFn);
    router.invoke(QStringLiteral("greet"), QStringLiteral("readLast"), {}, &outcome);
    QCOMPARE(outcome, IpcRouter::InvokeOutcome::Ok);
}

void TestPhosphorIpcRouter::invoke_numberInRange_data()
{
    QTest::addColumn<QString>("fn");
    QTest::addColumn<QVariant>("argument");
    QTest::addColumn<QVariant>("expected");

    // JSON numbers arrive as qlonglong or double.
    QTest::newRow("int max") << QStringLiteral("takesInt") << QVariant(qlonglong(2147483647)) << QVariant(2147483647);
    QTest::newRow("int min") << QStringLiteral("takesInt") << QVariant(qlonglong(-2147483647) - 1)
                             << QVariant(-2147483647 - 1);
    QTest::newRow("int from a whole double") << QStringLiteral("takesInt") << QVariant(7.0) << QVariant(7);
    QTest::newRow("uint max") << QStringLiteral("takesUInt") << QVariant(qlonglong(4294967295LL))
                              << QVariant(4294967295U);
    QTest::newRow("uint zero") << QStringLiteral("takesUInt") << QVariant(qlonglong(0)) << QVariant(0U);
    QTest::newRow("short max") << QStringLiteral("takesShort") << QVariant(qlonglong(32767)) << QVariant(32767);
    QTest::newRow("byte max") << QStringLiteral("takesByte") << QVariant(qlonglong(255)) << QVariant(255);
    QTest::newRow("char") << QStringLiteral("takesChar") << QVariant(qlonglong(65)) << QVariant(65);
    QTest::newRow("long 2^53") << QStringLiteral("takesLong") << QVariant(qlonglong(9007199254740992LL))
                               << QVariant(qlonglong(9007199254740992LL));
    QTest::newRow("ulong 2^53") << QStringLiteral("takesULong") << QVariant(qlonglong(9007199254740992LL))
                                << QVariant(qulonglong(9007199254740992ULL));
    QTest::newRow("float") << QStringLiteral("takesFloat") << QVariant(1.5) << QVariant(1.5);
    // A JSON number above the largest qint64 arrives as a double. QVariant
    // alone would make 2^63 - 1 of each of these.
    QTest::newRow("ulong 2^63 from a double") << QStringLiteral("takesULong") << QVariant(9223372036854775808.0)
                                              << QVariant(qulonglong(9223372036854775808ULL));
    QTest::newRow("ulong 2^63 from a float") << QStringLiteral("takesULong") << QVariant(9223372036854775808.0f)
                                             << QVariant(qulonglong(9223372036854775808ULL));
    QTest::newRow("ulong 1e19 from a double")
        << QStringLiteral("takesULong") << QVariant(1e19) << QVariant(qulonglong(10000000000000000000ULL));
    QTest::newRow("ulong under 2^64 from a double") << QStringLiteral("takesULong") << QVariant(18446744073709549568.0)
                                                    << QVariant(qulonglong(18446744073709549568ULL));
    if (sizeof(unsigned long) == sizeof(qulonglong)) {
        QTest::newRow("unsigned long 1e19 from a double")
            << QStringLiteral("takesUnsignedLong") << QVariant(1e19) << QVariant(qulonglong(10000000000000000000ULL));
    }
    QTest::newRow("float from a large double") << QStringLiteral("takesFloat") << QVariant(1e30) << QVariant(1e30f);
    QTest::newRow("half") << QStringLiteral("takesHalf") << QVariant(1.5) << QVariant(1.5);
    QTest::newRow("half from an integer") << QStringLiteral("takesHalf") << QVariant(qlonglong(3)) << QVariant(3.0);
    // The least qint64 as a double, which a JSON number with a fraction
    // or an exponent is. Half a unit below it is the same double.
    QTest::newRow("long min from a double") << QStringLiteral("takesLong") << QVariant(-9223372036854775808.0)
                                            << QVariant(std::numeric_limits<qlonglong>::min());
    QTest::newRow("double") << QStringLiteral("takesDouble") << QVariant(1e300) << QVariant(1e300);
    // A string that holds a number converts as before.
    QTest::newRow("int from a string") << QStringLiteral("takesInt") << QVariant(QStringLiteral("12")) << QVariant(12);
}

void TestPhosphorIpcRouter::invoke_numberInRange()
{
    QFETCH(QString, fn);
    QFETCH(QVariant, argument);
    QFETCH(QVariant, expected);

    IpcRouter router;
    Numbers target;
    QVERIFY(router.registerTarget(QStringLiteral("numbers"), &target));
    IpcRouter::InvokeOutcome outcome = IpcRouter::InvokeOutcome::NoSuchFn;
    QString message;
    const QVariant result = router.invoke(QStringLiteral("numbers"), fn, {argument}, &outcome, &message);
    QVERIFY2(outcome == IpcRouter::InvokeOutcome::Ok, qPrintable(message));
    // Through the wire form, where every number is a JSON number.
    QCOMPARE(variantToJson(result), variantToJson(expected));
    // The wire form is a double, which has 2^63 for 2^63 - 1 too. An
    // unsigned result of 64 bits is compared as it is.
    if (expected.typeId() == QMetaType::ULongLong) {
        QCOMPARE(result.toULongLong(), expected.toULongLong());
    }
}

void TestPhosphorIpcRouter::invoke_numberOutOfRange_data()
{
    QTest::addColumn<QString>("fn");
    QTest::addColumn<QVariant>("argument");

    // Each of these converted with success before, to another number: 0,
    // 4294967295, 4464, 44, and infinity for the float.
    QTest::newRow("int above") << QStringLiteral("takesInt") << QVariant(qlonglong(1099511627776LL));
    QTest::newRow("int just above") << QStringLiteral("takesInt") << QVariant(qlonglong(2147483648LL));
    QTest::newRow("int just below") << QStringLiteral("takesInt") << QVariant(qlonglong(-2147483649LL));
    QTest::newRow("int from a large double") << QStringLiteral("takesInt") << QVariant(1e30);
    QTest::newRow("uint negative") << QStringLiteral("takesUInt") << QVariant(qlonglong(-1));
    QTest::newRow("uint above") << QStringLiteral("takesUInt") << QVariant(qlonglong(4294967296LL));
    QTest::newRow("short above") << QStringLiteral("takesShort") << QVariant(qlonglong(70000));
    QTest::newRow("byte above") << QStringLiteral("takesByte") << QVariant(qlonglong(300));
    QTest::newRow("byte negative") << QStringLiteral("takesByte") << QVariant(qlonglong(-1));
    QTest::newRow("char above") << QStringLiteral("takesChar") << QVariant(qlonglong(300));
    QTest::newRow("ulong negative") << QStringLiteral("takesULong") << QVariant(qlonglong(-1));
    QTest::newRow("long from a large double") << QStringLiteral("takesLong") << QVariant(1e19);
    QTest::newRow("float above") << QStringLiteral("takesFloat") << QVariant(1e300);
    QTest::newRow("ulong 2^64 from a double") << QStringLiteral("takesULong") << QVariant(18446744073709551616.0);
    QTest::newRow("half above") << QStringLiteral("takesHalf") << QVariant(1e10);
    QTest::newRow("half above from an integer") << QStringLiteral("takesHalf") << QVariant(qlonglong(100000));
    QTest::newRow("long 2^63 from a double") << QStringLiteral("takesLong") << QVariant(9223372036854775808.0);
    QTest::newRow("long below from a double") << QStringLiteral("takesLong") << QVariant(-9223372036854777856.0);
}

void TestPhosphorIpcRouter::invoke_numberOutOfRange()
{
    QFETCH(QString, fn);
    QFETCH(QVariant, argument);

    IpcRouter router;
    Numbers target;
    QVERIFY(router.registerTarget(QStringLiteral("numbers"), &target));
    IpcRouter::InvokeOutcome outcome = IpcRouter::InvokeOutcome::Ok;
    QString message;
    const QVariant result = router.invoke(QStringLiteral("numbers"), fn, {argument}, &outcome, &message);
    QVERIFY2(!result.isValid(), qPrintable(result.toString()));
    QCOMPARE(outcome, IpcRouter::InvokeOutcome::ArgConvertFailed);
    QVERIFY2(message.contains(QStringLiteral("out of the range")), qPrintable(message));
}

void TestPhosphorIpcRouter::schemaFor_listsAHiddenNameOnce()
{
    // The schema lists what the router calls: one entry for a method that
    // a subclass declares again, and none for one that it withdrew.
    IpcRouter router;
    LayeredDerived target;
    QVERIFY(router.registerTarget(QStringLiteral("layers"), &target));
    QStringList signatures;
    const QJsonArray functions =
        router.schemaFor(QStringLiteral("layers")).value(QStringLiteral("functions")).toArray();
    for (const QJsonValue function : functions) {
        const QJsonObject entry = function.toObject();
        QStringList types;
        const QJsonArray params = entry.value(QStringLiteral("params")).toArray();
        for (const QJsonValue param : params) {
            types.append(param.toObject().value(QStringLiteral("type")).toString());
        }
        signatures.append(
            QStringLiteral("%1(%2)").arg(entry.value(QStringLiteral("name")).toString(), types.join(QLatin1Char(','))));
    }
    signatures.sort();
    QCOMPARE(signatures,
             (QStringList{
                 QStringLiteral("grown(integer,integer)"),
                 QStringLiteral("inherited()"),
                 QStringLiteral("overridden(integer)"),
                 QStringLiteral("redeclared(integer)"),
                 QStringLiteral("retyped(string)"),
             }));
}

void TestPhosphorIpcRouter::schemaFor_charTypesAreIntegers()
{
    IpcRouter router;
    Numbers target;
    QVERIFY(router.registerTarget(QStringLiteral("numbers"), &target));
    QHash<QString, QJsonObject> byName;
    const QJsonArray functions =
        router.schemaFor(QStringLiteral("numbers")).value(QStringLiteral("functions")).toArray();
    for (const QJsonValue function : functions) {
        byName.insert(function.toObject().value(QStringLiteral("name")).toString(), function.toObject());
    }
    const QJsonObject integer{{QStringLiteral("type"), QStringLiteral("integer")}};
    const QJsonObject integerValue{{QStringLiteral("type"), QStringLiteral("integer")},
                                   {QStringLiteral("name"), QStringLiteral("value")}};
    for (const QString& fn : {QStringLiteral("takesByte"), QStringLiteral("takesChar"), QStringLiteral("takesShort")}) {
        QVERIFY2(byName.contains(fn), qPrintable(fn));
        QCOMPARE(byName.value(fn).value(QStringLiteral("returns")).toObject(), integer);
        QCOMPARE(byName.value(fn).value(QStringLiteral("params")).toArray(), QJsonArray{integerValue});
    }
}

void TestPhosphorIpcRouter::defaultSocketPath_isInTheRuntimeDirectory()
{
    // The variable of this process, which the last lines put back.
    const bool wasSet = qEnvironmentVariableIsSet("XDG_RUNTIME_DIR");
    const QByteArray before = qgetenv("XDG_RUNTIME_DIR");

    qputenv("XDG_RUNTIME_DIR", QByteArrayLiteral("/run/user/phosphor-ipc-test"));
    const QString set = IpcRouter::defaultSocketPath();
    qunsetenv("XDG_RUNTIME_DIR");
    const QString unset = IpcRouter::defaultSocketPath();
    // A router has no path to start on then.
    IpcRouter router;
    QTest::ignoreMessage(QtWarningMsg,
                         "PhosphorIpc::IpcRouter::start: no socket path (XDG_RUNTIME_DIR unset and no explicit path)");
    const bool started = router.start();

    if (wasSet) {
        qputenv("XDG_RUNTIME_DIR", before);
    }
    QCOMPARE(set, QStringLiteral("/run/user/phosphor-ipc-test/phosphor.sock"));
    QCOMPARE(unset, QString());
    QVERIFY(!started);
}

QTEST_GUILESS_MAIN(TestPhosphorIpcRouter)
#include "test_phosphor_ipc_router.moc"
