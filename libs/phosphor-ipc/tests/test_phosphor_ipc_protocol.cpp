// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorIpc/IpcProtocol.h>

#include <QByteArray>
#include <QChar>
#include <QDateTime>
#include <QFloat16>
#include <QHash>
#include <QJSEngine>
#include <QJSValue>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QMap>
#include <QPointF>
#include <QTest>
#include <QTimeZone>
#include <QVariant>
#include <QVariantHash>
#include <QVariantList>
#include <QVariantMap>
#include <QtCore/qtclasshelpermacros.h>

#include <optional>

using namespace PhosphorIpc;

class TestPhosphorIpcProtocol : public QObject
{
    Q_OBJECT
public:
    Q_DISABLE_COPY_MOVE(TestPhosphorIpcProtocol)
    TestPhosphorIpcProtocol() = default;
private Q_SLOTS:
    void parseRequest_acceptsValidCall();
    void parseRequest_acceptsList();
    void parseRequest_acceptsSchema();
    void parseRequest_acceptsSubscribe();
    void parseRequest_rejectsMissingType();
    void parseRequest_rejectsMalformedJson();
    void parseRequest_rejectsNonObjectRoot();
    void parseRequest_rejectsNonArrayArgs();
    void parseRequest_argsDefaultEmpty();
    void buildReply_shape();
    void buildEvent_shape();
    void buildError_shape();
    void buildError_omitsZeroId();
    void writeLine_appendsNewline();
    void parseRequest_rejectsNonNumericId();
    void parseRequest_rejectsFractionalId();
    void parseRequest_rejectsOutOfRangeId();
    void parseRequest_givesTheIdOfARefusedRequest();
    void parseRequest_takesNullAsAbsent();
    void parseRequest_capsTheArguments();
    void integerId_data();
    void integerId();
    void parseRequest_rejectsEmptyTargetOnCall();
    void parseRequest_rejectsEmptyFnOnCall();
    void parseRequest_rejectsEmptySignalOnSubscribe();
    void parseRequest_rejectsUnsubscribeMissingSubscriptionId();
    void parseRequest_acceptsUnsubscribe();
    void variantToJson_basics();
    void variantToJson_nullAndJavaScript();
    void variantToJson_numbersAndText();
    void variantToJson_dateTime();
    void variantToJson_containers();
};

void TestPhosphorIpcProtocol::parseRequest_acceptsValidCall()
{
    const QByteArray line = R"({"type":"call","id":42,"target":"greet","fn":"sayHello","args":["nate"]})";
    QString err;
    const auto r = parseRequest(line, &err);
    QVERIFY(r.has_value());
    QVERIFY(err.isEmpty());
    QCOMPARE(r->type, QStringLiteral("call"));
    QCOMPARE(r->id, 42);
    QCOMPARE(r->target, QStringLiteral("greet"));
    QCOMPARE(r->fn, QStringLiteral("sayHello"));
    QCOMPARE(r->args.size(), 1);
    QCOMPARE(r->args.at(0).toString(), QStringLiteral("nate"));
}

void TestPhosphorIpcProtocol::parseRequest_acceptsList()
{
    const auto r = parseRequest(R"({"type":"list","id":1})", nullptr);
    QVERIFY(r.has_value());
    QCOMPARE(r->type, QStringLiteral("list"));
    QCOMPARE(r->id, 1);
}

void TestPhosphorIpcProtocol::parseRequest_acceptsSchema()
{
    const auto r = parseRequest(R"({"type":"schema","id":2,"target":"greet"})", nullptr);
    QVERIFY(r.has_value());
    QCOMPARE(r->type, QStringLiteral("schema"));
    QCOMPARE(r->target, QStringLiteral("greet"));
}

void TestPhosphorIpcProtocol::parseRequest_acceptsSubscribe()
{
    const auto r = parseRequest(R"({"type":"subscribe","id":3,"target":"count","signal":"countChanged"})", nullptr);
    QVERIFY(r.has_value());
    QCOMPARE(r->type, QStringLiteral("subscribe"));
    QCOMPARE(r->target, QStringLiteral("count"));
    QCOMPARE(r->signalName, QStringLiteral("countChanged"));
}

void TestPhosphorIpcProtocol::parseRequest_rejectsMissingType()
{
    QString err;
    QVERIFY(!parseRequest(R"({"id":1})", &err).has_value());
    QVERIFY(err.contains(QStringLiteral("'type'")));
}

void TestPhosphorIpcProtocol::parseRequest_rejectsMalformedJson()
{
    QString err;
    QVERIFY(!parseRequest("{ broken json", &err).has_value());
    QVERIFY(err.contains(QStringLiteral("malformed JSON")));
}

void TestPhosphorIpcProtocol::parseRequest_rejectsNonObjectRoot()
{
    QString err;
    QVERIFY(!parseRequest("[1,2,3]", &err).has_value());
    QVERIFY(err.contains(QStringLiteral("not a JSON object")));
}

void TestPhosphorIpcProtocol::parseRequest_rejectsNonArrayArgs()
{
    // Provide target/fn so the parser doesn't bail on the
    // per-type required-field check first; this test exists to
    // exercise the args-type validation branch specifically.
    QString err;
    QVERIFY(!parseRequest(R"({"type":"call","id":1,"target":"t","fn":"f","args":42})", &err).has_value());
    QVERIFY(err.contains(QStringLiteral("'args'")));
}

void TestPhosphorIpcProtocol::parseRequest_argsDefaultEmpty()
{
    // Missing args is fine (no-arg call).
    const auto r = parseRequest(R"({"type":"call","id":1,"target":"x","fn":"y"})", nullptr);
    QVERIFY(r.has_value());
    QCOMPARE(r->args.size(), 0);
}

void TestPhosphorIpcProtocol::buildReply_shape()
{
    const QJsonObject obj = buildReply(42, QStringLiteral("hello"));
    QCOMPARE(obj.value(QLatin1String("type")).toString(), QStringLiteral("reply"));
    QCOMPARE(obj.value(QLatin1String("id")).toInt(), 42);
    QCOMPARE(obj.value(QLatin1String("result")).toString(), QStringLiteral("hello"));
}

void TestPhosphorIpcProtocol::buildEvent_shape()
{
    QJsonArray args;
    args.append(7);
    const QJsonObject obj = buildEvent(99, args);
    QCOMPARE(obj.value(QLatin1String("type")).toString(), QStringLiteral("event"));
    QCOMPARE(obj.value(QLatin1String("subscriptionId")).toInt(), 99);
    QCOMPARE(obj.value(QLatin1String("args")).toArray().first().toInt(), 7);
}

void TestPhosphorIpcProtocol::buildError_shape()
{
    QJsonObject detail;
    detail.insert(QLatin1String("expected"), QStringLiteral("int"));
    const QJsonObject obj = buildError(5, QStringLiteral("INVALID_ARG"), QStringLiteral("bad"), detail);
    QCOMPARE(obj.value(QLatin1String("type")).toString(), QStringLiteral("error"));
    QCOMPARE(obj.value(QLatin1String("id")).toInt(), 5);
    QCOMPARE(obj.value(QLatin1String("code")).toString(), QStringLiteral("INVALID_ARG"));
    QCOMPARE(obj.value(QLatin1String("message")).toString(), QStringLiteral("bad"));
    QCOMPARE(obj.value(QLatin1String("detail")).toObject().value(QLatin1String("expected")).toString(),
             QStringLiteral("int"));
}

void TestPhosphorIpcProtocol::buildError_omitsZeroId()
{
    // id == 0 is reserved for "no client correlation" (e.g.,
    // malformed-line responses before we parsed an id). The error
    // shape should omit the id field rather than emit 0.
    const QJsonObject obj = buildError(0, QStringLiteral("X"), QStringLiteral("y"));
    QVERIFY(!obj.contains(QLatin1String("id")));
}

void TestPhosphorIpcProtocol::writeLine_appendsNewline()
{
    QJsonObject obj;
    obj.insert(QLatin1String("a"), 1);
    const QByteArray bytes = writeLine(obj);
    QVERIFY(bytes.endsWith('\n'));
    QVERIFY(!bytes.left(bytes.size() - 1).contains('\n'));
}

void TestPhosphorIpcProtocol::parseRequest_rejectsNonNumericId()
{
    QString err;
    QVERIFY(!parseRequest(R"({"type":"call","id":"forty-two","target":"x","fn":"y"})", &err).has_value());
    QVERIFY(err.contains(QStringLiteral("'id'")));
}

void TestPhosphorIpcProtocol::parseRequest_rejectsFractionalId()
{
    // JSON numbers are doubles. `"id": 1.5` would silently truncate
    // to id=1 if accepted; reject it so the wire protocol enforces
    // the documented "id is integer" contract.
    QString err;
    QVERIFY(!parseRequest(R"({"type":"call","id":1.5,"target":"x","fn":"y"})", &err).has_value());
    QVERIFY(err.contains(QStringLiteral("'id'")));
}

void TestPhosphorIpcProtocol::parseRequest_rejectsOutOfRangeId()
{
    // 1e30 exceeds INT64_MAX and would be UB to cast. The parser
    // must reject it as malformed, not truncate.
    QString err;
    QVERIFY(!parseRequest(R"({"type":"call","id":1e30,"target":"x","fn":"y"})", &err).has_value());
    QVERIFY(err.contains(QStringLiteral("'id'")));

    // Above 2^53 a reader that keeps numbers as doubles rounds, and the
    // reply would carry another id than the request.
    err.clear();
    QVERIFY(!parseRequest(R"({"type":"list","id":9007199254740993})", &err).has_value());
    QVERIFY2(err.contains(QStringLiteral("'id'")), qPrintable(err));
    err.clear();
    QVERIFY(!parseRequest(R"({"type":"unsubscribe","id":1,"subscriptionId":9007199254740993})", &err).has_value());
    QVERIFY2(err.contains(QStringLiteral("'subscriptionId'")), qPrintable(err));

    const auto atTheLimit = parseRequest(R"({"type":"list","id":9007199254740992})", &err);
    QVERIFY(atTheLimit.has_value());
    QCOMPARE(atTheLimit->id, qint64(9007199254740992));
    const auto negative = parseRequest(R"({"type":"list","id":-9007199254740992})", &err);
    QVERIFY(negative.has_value());
    QCOMPARE(negative->id, qint64(-9007199254740992));
}

void TestPhosphorIpcProtocol::parseRequest_givesTheIdOfARefusedRequest()
{
    // The router answers a refused request under this id, so that the
    // client can tell which request failed.
    QString err;
    qint64 id = -1;
    QVERIFY(!parseRequest(R"({"type":"call","id":7,"target":"","fn":"y"})", &err, &id).has_value());
    QCOMPARE(id, qint64(7));

    id = -1;
    QVERIFY(!parseRequest(R"({"type":"unsubscribe","id":8})", &err, &id).has_value());
    QCOMPARE(id, qint64(8));

    // A line without a type has its id all the same.
    id = -1;
    QVERIFY(!parseRequest(R"({"id":6,"target":"x","fn":"y"})", &err, &id).has_value());
    QCOMPARE(id, qint64(6));
    QCOMPARE(err, QStringLiteral("missing or non-string 'type' field"));

    // No valid id in the line: 0, the value that buildError leaves out.
    id = -1;
    QVERIFY(!parseRequest(R"({"type":"call","id":"seven","target":"x","fn":"y"})", &err, &id).has_value());
    QCOMPARE(id, qint64(0));
    id = -1;
    QVERIFY(!parseRequest("not json", &err, &id).has_value());
    QCOMPARE(id, qint64(0));

    // A request that parses gives its id too.
    id = -1;
    QVERIFY(parseRequest(R"({"type":"list","id":9})", &err, &id).has_value());
    QCOMPARE(id, qint64(9));
}

void TestPhosphorIpcProtocol::parseRequest_takesNullAsAbsent()
{
    QString err;
    const auto request = parseRequest(R"({"type":"call","id":null,"target":"x","fn":"y","args":null})", &err);
    QVERIFY2(request.has_value(), qPrintable(err));
    QCOMPARE(request->id, qint64(0));
    QVERIFY(request->args.isEmpty());

    // A subscribe needs no id. Its subscription then has the id 0.
    const auto subscribe = parseRequest(R"({"type":"subscribe","target":"x","signal":"s"})", &err);
    QVERIFY2(subscribe.has_value(), qPrintable(err));
    QCOMPARE(subscribe->id, qint64(0));
}

void TestPhosphorIpcProtocol::parseRequest_capsTheArguments()
{
    const auto lineWith = [](qsizetype count) {
        QJsonArray args;
        for (qsizetype i = 0; i < count; ++i) {
            args.append(0);
        }
        return writeLine(QJsonObject{{QStringLiteral("type"), QStringLiteral("call")},
                                     {QStringLiteral("id"), 1},
                                     {QStringLiteral("target"), QStringLiteral("x")},
                                     {QStringLiteral("fn"), QStringLiteral("y")},
                                     {QStringLiteral("args"), args}});
    };
    QString err;
    const auto atTheCap = parseRequest(lineWith(MaxRequestArgs), &err);
    QVERIFY2(atTheCap.has_value(), qPrintable(err));
    QCOMPARE(atTheCap->args.size(), MaxRequestArgs);
    QVERIFY(!parseRequest(lineWith(MaxRequestArgs + 1), &err).has_value());
    QVERIFY2(err.contains(QStringLiteral("'args'")), qPrintable(err));
}

void TestPhosphorIpcProtocol::integerId_data()
{
    QTest::addColumn<QByteArray>("json");
    QTest::addColumn<bool>("valid");
    QTest::addColumn<qint64>("id");

    QTest::newRow("integer") << QByteArray("5") << true << qint64(5);
    QTest::newRow("zero") << QByteArray("0") << true << qint64(0);
    QTest::newRow("negative") << QByteArray("-3") << true << qint64(-3);
    QTest::newRow("integer written as a decimal") << QByteArray("5.0") << true << qint64(5);
    QTest::newRow("integer written with an exponent") << QByteArray("1e2") << true << qint64(100);
    QTest::newRow("2^53") << QByteArray("9007199254740992") << true << qint64(9007199254740992);
    QTest::newRow("-2^53") << QByteArray("-9007199254740992") << true << qint64(-9007199254740992);
    QTest::newRow("2^53 + 1") << QByteArray("9007199254740993") << false << qint64(0);
    QTest::newRow("-2^53 - 1") << QByteArray("-9007199254740993") << false << qint64(0);
    QTest::newRow("2^63 - 1") << QByteArray("9223372036854775807") << false << qint64(0);
    QTest::newRow("2^63") << QByteArray("9223372036854775808") << false << qint64(0);
    QTest::newRow("1e30") << QByteArray("1e30") << false << qint64(0);
    QTest::newRow("fraction") << QByteArray("1.5") << false << qint64(0);
    QTest::newRow("string") << QByteArray("\"5\"") << false << qint64(0);
    QTest::newRow("null") << QByteArray("null") << false << qint64(0);
    QTest::newRow("boolean") << QByteArray("true") << false << qint64(0);
}

void TestPhosphorIpcProtocol::integerId()
{
    QFETCH(QByteArray, json);
    QFETCH(bool, valid);
    QFETCH(qint64, id);

    // Through the parser, as an id comes off the wire.
    const QJsonDocument document = QJsonDocument::fromJson("[" + json + "]");
    QVERIFY(document.isArray());
    const std::optional<qint64> parsed = PhosphorIpc::integerId(document.array().at(0));
    QCOMPARE(parsed.has_value(), valid);
    if (valid) {
        QCOMPARE(*parsed, id);
    }
}

void TestPhosphorIpcProtocol::parseRequest_rejectsEmptyTargetOnCall()
{
    // Empty target on a `call` is malformed at the wire level; the
    // router would otherwise surface NO_SUCH_TARGET, which is the
    // wrong shape (the fault is the request, not the registry).
    // Both shapes are pinned — ABSENT field and explicit "" literal —
    // so a future parser refactor that distinguishes missing from
    // empty can't silently start accepting the empty-literal form.
    QString err;
    QVERIFY(!parseRequest(R"({"type":"call","id":1,"fn":"y"})", &err).has_value());
    QVERIFY(err.contains(QStringLiteral("'target'")));
    QVERIFY(!parseRequest(R"({"type":"call","id":1,"target":"","fn":"y"})", &err).has_value());
    QVERIFY(err.contains(QStringLiteral("'target'")));
}

void TestPhosphorIpcProtocol::parseRequest_rejectsEmptyFnOnCall()
{
    // Absent and explicit-empty forms, same rationale as the target test.
    QString err;
    QVERIFY(!parseRequest(R"({"type":"call","id":1,"target":"x"})", &err).has_value());
    QVERIFY(err.contains(QStringLiteral("'fn'")));
    QVERIFY(!parseRequest(R"({"type":"call","id":1,"target":"x","fn":""})", &err).has_value());
    QVERIFY(err.contains(QStringLiteral("'fn'")));
}

void TestPhosphorIpcProtocol::parseRequest_rejectsEmptySignalOnSubscribe()
{
    // Absent and explicit-empty forms, same rationale as the target test.
    QString err;
    QVERIFY(!parseRequest(R"({"type":"subscribe","id":1,"target":"x"})", &err).has_value());
    QVERIFY(err.contains(QStringLiteral("'signal'")));
    QVERIFY(!parseRequest(R"({"type":"subscribe","id":1,"target":"x","signal":""})", &err).has_value());
    QVERIFY(err.contains(QStringLiteral("'signal'")));
}

void TestPhosphorIpcProtocol::parseRequest_rejectsUnsubscribeMissingSubscriptionId()
{
    // Unsubscribe with no subscriptionId (or an explicit zero) must
    // surface as MALFORMED_REQUEST, not as NO_SUCH_SUBSCRIPTION for
    // "unknown subscriptionId 0".
    QString err;
    QVERIFY(!parseRequest(R"({"type":"unsubscribe","id":1})", &err).has_value());
    QVERIFY(err.contains(QStringLiteral("subscriptionId")));
}

void TestPhosphorIpcProtocol::parseRequest_acceptsUnsubscribe()
{
    const auto r = parseRequest(R"({"type":"unsubscribe","id":3,"subscriptionId":42})", nullptr);
    QVERIFY(r.has_value());
    QCOMPARE(r->type, QStringLiteral("unsubscribe"));
    QCOMPARE(r->id, 3);
    QCOMPARE(r->subscriptionId, 42);
}

void TestPhosphorIpcProtocol::variantToJson_basics()
{
    QCOMPARE(variantToJson(QVariant()), QJsonValue(QJsonValue::Null));
    QCOMPARE(variantToJson(QVariant(true)), QJsonValue(true));
    QCOMPARE(variantToJson(QVariant(42)).toInt(), 42);
    QCOMPARE(variantToJson(QVariant(1.5)).toDouble(), 1.5);
    QCOMPARE(variantToJson(QVariant(QStringLiteral("hi"))).toString(), QStringLiteral("hi"));

    QVariantList list;
    list.append(1);
    list.append(QStringLiteral("two"));
    const QJsonArray arr = variantToJson(list).toArray();
    QCOMPARE(arr.size(), 2);
    QCOMPARE(arr.at(0).toInt(), 1);
    QCOMPARE(arr.at(1).toString(), QStringLiteral("two"));

    QVariantMap map;
    map.insert(QStringLiteral("k"), QStringLiteral("v"));
    const QJsonObject obj = variantToJson(map).toObject();
    QCOMPARE(obj.value(QStringLiteral("k")).toString(), QStringLiteral("v"));
}

void TestPhosphorIpcProtocol::variantToJson_nullAndJavaScript()
{
    // A JSON null that came in as an argument.
    QCOMPARE(variantToJson(QVariant::fromValue(nullptr)), QJsonValue(QJsonValue::Null));

    // What an untyped QML function returns, and what emitEvent gets from
    // QML: JavaScript values.
    const auto json = [](const QJSValue& value) {
        return variantToJson(QVariant::fromValue(value));
    };
    QCOMPARE(json(QJSValue(QJSValue::NullValue)), QJsonValue(QJsonValue::Null));
    QCOMPARE(json(QJSValue()), QJsonValue(QJsonValue::Null));
    QCOMPARE(json(QJSValue(5)), QJsonValue(5));
    QCOMPARE(json(QJSValue(QStringLiteral("x"))), QJsonValue(QStringLiteral("x")));
    QCOMPARE(json(QJSValue(true)), QJsonValue(true));

    QJSEngine engine;
    const QJSValue object = engine.evaluate(QStringLiteral("({ a: 1, list: [1, \"two\", null], none: null })"));
    QVERIFY(!object.isError());
    QCOMPARE(json(object),
             QJsonValue(QJsonObject{
                 {QStringLiteral("a"), 1},
                 {QStringLiteral("list"), QJsonArray{1, QStringLiteral("two"), QJsonValue::Null}},
                 {QStringLiteral("none"), QJsonValue::Null},
             }));

    // JSON has no form for a function, as JSON.stringify has none. The
    // conversion of a JavaScript value gives a function back as it is, so
    // one more conversion of it would never end.
    const QJSValue function = engine.evaluate(QStringLiteral("(function() { return 1; })"));
    QVERIFY(function.isCallable());
    QCOMPARE(json(function), QJsonValue(QJsonValue::Null));
    QCOMPARE(json(engine.evaluate(QStringLiteral("(() => 1)"))), QJsonValue(QJsonValue::Null));

    const QJSValue holder = engine.evaluate(QStringLiteral("({ sum: 1, describe() { return \"x\"; } })"));
    QVERIFY(!holder.isError());
    QCOMPARE(json(holder),
             QJsonValue(QJsonObject{
                 {QStringLiteral("sum"), 1},
                 {QStringLiteral("describe"), QJsonValue::Null},
             }));
    QCOMPARE(json(engine.evaluate(QStringLiteral("[1, function() {}]"))), QJsonValue(QJsonArray{1, QJsonValue::Null}));

    // The arguments of IpcTarget.emitEvent arrive as a list that holds
    // JavaScript values.
    QCOMPARE(variantToJson(QVariantList{QVariant::fromValue(function), QVariant::fromValue(holder)}),
             QJsonValue(QJsonArray{QJsonValue::Null,
                                   QJsonObject{
                                       {QStringLiteral("sum"), 1},
                                       {QStringLiteral("describe"), QJsonValue::Null},
                                   }}));

    // An object that holds itself. Qt cuts the cycle.
    const QJSValue cycle =
        engine.evaluate(QStringLiteral("(function() { var o = { x: 1 }; o.self = o; return o; })()"));
    QVERIFY(!cycle.isError());
    QCOMPARE(json(cycle).toObject().value(QStringLiteral("x")), QJsonValue(1));
}

void TestPhosphorIpcProtocol::variantToJson_numbersAndText()
{
    // A char is a number, as the router takes a number for a char
    // parameter.
    QCOMPARE(variantToJson(QVariant::fromValue<quint8>(100)), QJsonValue(100));
    QCOMPARE(variantToJson(QVariant::fromValue<qint8>(-100)), QJsonValue(-100));
    QCOMPARE(variantToJson(QVariant::fromValue<char>('A')), QJsonValue(65));
    QCOMPARE(variantToJson(QVariant::fromValue<quint16>(65535)), QJsonValue(65535));
    QCOMPARE(variantToJson(QVariant::fromValue<qint16>(-32768)), QJsonValue(-32768));
    QCOMPARE(variantToJson(QVariant::fromValue<quint64>(4294967296ULL)), QJsonValue(4294967296.0));
    QCOMPARE(variantToJson(QVariant::fromValue(qfloat16(1.5f))), QJsonValue(1.5));
    QCOMPARE(variantToJson(QVariant::fromValue(2.5f)), QJsonValue(2.5));

    // Text types stay strings.
    QCOMPARE(variantToJson(QVariant::fromValue(QChar(u'q'))), QJsonValue(QStringLiteral("q")));
    QCOMPARE(variantToJson(QVariant::fromValue<char16_t>(u'x')), QJsonValue(QStringLiteral("x")));
    QCOMPARE(variantToJson(QVariant::fromValue<char32_t>(U'y')), QJsonValue(QStringLiteral("y")));
    QCOMPARE(variantToJson(QVariant(QByteArray("bytes"))), QJsonValue(QStringLiteral("bytes")));

    // A type without a JSON form goes out as its string form, which is
    // empty for a type that has none.
    QCOMPARE(variantToJson(QVariant(QPointF(1, 2))), QJsonValue(QString()));
}

void TestPhosphorIpcProtocol::variantToJson_dateTime()
{
    // Local time with no offset would mean another moment to a reader in
    // another time zone.
    const QDateTime moment(QDate(2026, 3, 4), QTime(5, 6, 7, 89), QTimeZone::fromSecondsAheadOfUtc(2 * 3600));
    QCOMPARE(variantToJson(moment), QJsonValue(QStringLiteral("2026-03-04T03:06:07.089Z")));
    QCOMPARE(variantToJson(QVariantMap{{QStringLiteral("at"), moment}}),
             QJsonValue(QJsonObject{{QStringLiteral("at"), QStringLiteral("2026-03-04T03:06:07.089Z")}}));
    QCOMPARE(variantToJson(QVariant(QDateTime())), QJsonValue(QJsonValue::Null));
}

void TestPhosphorIpcProtocol::variantToJson_containers()
{
    QCOMPARE(variantToJson(QVariant::fromValue(QList<int>{1, 2, 3})), QJsonValue(QJsonArray{1, 2, 3}));
    QCOMPARE(variantToJson(QVariant::fromValue(QList<double>{0.5})), QJsonValue(QJsonArray{0.5}));
    QCOMPARE(variantToJson(QVariant::fromValue(QList<QVariantMap>{{{QStringLiteral("k"), 1}}})),
             QJsonValue(QJsonArray{QJsonObject{{QStringLiteral("k"), 1}}}));
    QCOMPARE(variantToJson(QVariant::fromValue(QMap<QString, int>{{QStringLiteral("a"), 1}})),
             QJsonValue(QJsonObject{{QStringLiteral("a"), 1}}));
    QCOMPARE(variantToJson(QVariant::fromValue(QHash<QString, int>{{QStringLiteral("a"), 1}})),
             QJsonValue(QJsonObject{{QStringLiteral("a"), 1}}));
    QCOMPARE(variantToJson(QVariantHash{{QStringLiteral("h"), QStringLiteral("v")}}),
             QJsonValue(QJsonObject{{QStringLiteral("h"), QStringLiteral("v")}}));
    QCOMPARE(variantToJson(QVariant(QStringList{QStringLiteral("a"), QStringLiteral("b")})),
             QJsonValue(QJsonArray{QStringLiteral("a"), QStringLiteral("b")}));

    QCOMPARE(variantToJson(QVariant(QJsonDocument(QJsonObject{{QStringLiteral("d"), 1}}))),
             QJsonValue(QJsonObject{{QStringLiteral("d"), 1}}));
    QCOMPARE(variantToJson(QVariant(QJsonDocument(QJsonArray{1}))), QJsonValue(QJsonArray{1}));
    QCOMPARE(variantToJson(QVariant(QJsonDocument())), QJsonValue(QJsonValue::Null));

    // A string and a byte array are text, not lists of their characters.
    QCOMPARE(variantToJson(QVariant(QStringLiteral("abc"))), QJsonValue(QStringLiteral("abc")));
    QCOMPARE(variantToJson(QVariant(QByteArray("abc"))), QJsonValue(QStringLiteral("abc")));
}

QTEST_GUILESS_MAIN(TestPhosphorIpcProtocol)
#include "test_phosphor_ipc_protocol.moc"
