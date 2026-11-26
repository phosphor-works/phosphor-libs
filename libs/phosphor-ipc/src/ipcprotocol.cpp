// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorIpc/IpcProtocol.h>

#include <QCborValue>
#include <QDateTime>
#include <QJSValue>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLatin1String>
#include <QMetaType>
#include <QVariantHash>
#include <QVariantList>
#include <QVariantMap>

#include <cmath>

namespace PhosphorIpc {

std::optional<qint64> integerId(const QJsonValue& value)
{
    if (!value.isDouble()) {
        return std::nullopt;
    }
    // 2^53: up to it a double holds every integer, so a reader that keeps
    // JSON numbers as doubles has the same id as one that keeps integers.
    constexpr qint64 MaxExactInteger = qint64(1) << 53;
    // QJsonValue keeps a number written as an integer exactly, where
    // toDouble() would round one above 2^53 to a neighbor inside the test
    // below.
    const QCborValue number = QCborValue::fromJsonValue(value);
    if (number.isInteger()) {
        const qint64 integer = number.toInteger();
        if (integer < -MaxExactInteger || integer > MaxExactInteger) {
            return std::nullopt;
        }
        return integer;
    }
    // A number written with a fraction or an exponent, such as 5.0 or 1e2.
    const double d = value.toDouble();
    if (!std::isfinite(d) || std::trunc(d) != d || std::fabs(d) > static_cast<double>(MaxExactInteger)) {
        return std::nullopt;
    }
    return static_cast<qint64>(d);
}

std::optional<Request> parseRequest(const QByteArray& line, QString* parseError, qint64* requestId)
{
    if (requestId) {
        *requestId = 0;
    }
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(line, &err);
    if (err.error != QJsonParseError::NoError) {
        if (parseError) {
            *parseError = QStringLiteral("malformed JSON at offset %1: %2").arg(err.offset).arg(err.errorString());
        }
        return std::nullopt;
    }
    if (!doc.isObject()) {
        if (parseError) {
            *parseError = QStringLiteral("request root is not a JSON object");
        }
        return std::nullopt;
    }
    const QJsonObject obj = doc.object();

    Request req;
    // `id` correlates request to response. Absent or null gives 0: a
    // client that needs no correlation can leave it out, as a one-shot
    // CLI call does. An id of another type is a malformed request, not a
    // silent fallback to 0. Otherwise a client that sends a wrong id type
    // gets error frames without an id, since 0 is the "no id" value of
    // buildError.
    const QJsonValue idValue = obj.value(QLatin1String(Field::Id));
    if (idValue.isDouble()) {
        const auto parsed = integerId(idValue);
        if (!parsed) {
            if (parseError) {
                *parseError = QStringLiteral("'id' must be an integer from -2^53 to 2^53");
            }
            return std::nullopt;
        }
        req.id = *parsed;
        if (requestId) {
            *requestId = req.id;
        }
    } else if (!idValue.isUndefined() && !idValue.isNull()) {
        if (parseError) {
            *parseError = QStringLiteral("'id' must be a number if present");
        }
        return std::nullopt;
    }
    // After the id, so that the error for a line without a type can name
    // the request.
    req.type = obj.value(QLatin1String(Field::Type)).toString();
    if (req.type.isEmpty()) {
        if (parseError) {
            *parseError = QStringLiteral("missing or non-string 'type' field");
        }
        return std::nullopt;
    }
    req.target = obj.value(QLatin1String(Field::Target)).toString();
    req.fn = obj.value(QLatin1String(Field::Fn)).toString();
    req.signalName = obj.value(QLatin1String(Field::Signal)).toString();
    const QJsonValue subIdValue = obj.value(QLatin1String(Field::SubscriptionId));
    if (subIdValue.isDouble()) {
        const auto parsed = integerId(subIdValue);
        if (!parsed) {
            if (parseError) {
                *parseError = QStringLiteral("'subscriptionId' must be an integer from -2^53 to 2^53");
            }
            return std::nullopt;
        }
        req.subscriptionId = *parsed;
    } else if (!subIdValue.isUndefined() && !subIdValue.isNull()) {
        if (parseError) {
            *parseError = QStringLiteral("'subscriptionId' must be a number if present");
        }
        return std::nullopt;
    }
    // Per-type required-field validation. The router's dispatchers
    // would reject most of these with the correct error code at
    // runtime, but validating at parse-time surfaces them as
    // MALFORMED_REQUEST (the accurate shape) instead of e.g.
    // NO_SUCH_TARGET for an empty target string.
    if ((req.type == QLatin1String(RequestType::Call) || req.type == QLatin1String(RequestType::Schema)
         || req.type == QLatin1String(RequestType::Subscribe))
        && req.target.isEmpty()) {
        if (parseError) {
            *parseError = QStringLiteral("'%1' requires a non-empty 'target'").arg(req.type);
        }
        return std::nullopt;
    }
    if (req.type == QLatin1String(RequestType::Call) && req.fn.isEmpty()) {
        if (parseError) {
            *parseError = QStringLiteral("'call' requires a non-empty 'fn'");
        }
        return std::nullopt;
    }
    if (req.type == QLatin1String(RequestType::Subscribe) && req.signalName.isEmpty()) {
        if (parseError) {
            *parseError = QStringLiteral("'subscribe' requires a non-empty 'signal'");
        }
        return std::nullopt;
    }
    // Unsubscribe requires a non-zero `subscriptionId`. id=0 is the
    // protocol's "absent" sentinel; without this check an unsubscribe
    // with a missing field would parse successfully and reach the
    // dispatcher, which would then report NO_SUCH_SUBSCRIPTION for
    // "unknown subscriptionId 0" — accurate but the actual fault is
    // a malformed request.
    if (req.type == QLatin1String(RequestType::Unsubscribe) && req.subscriptionId == 0) {
        if (parseError) {
            *parseError = QStringLiteral("'unsubscribe' requires a non-zero 'subscriptionId'");
        }
        return std::nullopt;
    }
    // Args are absent or null (a call without arguments) or an array.
    // Any other value is a malformed request. The cap spares the router
    // the QVariantList of an array that no method could take.
    const QJsonValue argsValue = obj.value(QLatin1String(Field::Args));
    if (argsValue.isArray()) {
        const QJsonArray argsArray = argsValue.toArray();
        if (argsArray.size() > MaxRequestArgs) {
            if (parseError) {
                *parseError =
                    QStringLiteral("'args' length %1 exceeds limit of %2").arg(argsArray.size()).arg(MaxRequestArgs);
            }
            return std::nullopt;
        }
        req.args = argsArray.toVariantList();
    } else if (!argsValue.isUndefined() && !argsValue.isNull()) {
        if (parseError) {
            *parseError = QStringLiteral("'args' must be an array if present");
        }
        return std::nullopt;
    }
    return req;
}

QJsonObject buildReply(qint64 id, const QJsonValue& result)
{
    QJsonObject obj;
    obj.insert(QLatin1String(Field::Type), QLatin1String(ResponseType::Reply));
    obj.insert(QLatin1String(Field::Id), static_cast<double>(id));
    // QJsonObject::insert drops the key for an undefined value, and a
    // reply always carries `result`.
    obj.insert(QLatin1String(Field::Result), result.isUndefined() ? QJsonValue(QJsonValue::Null) : result);
    return obj;
}

QJsonObject buildEvent(qint64 subscriptionId, const QJsonValue& args)
{
    QJsonObject obj;
    obj.insert(QLatin1String(Field::Type), QLatin1String(ResponseType::Event));
    obj.insert(QLatin1String(Field::SubscriptionId), static_cast<double>(subscriptionId));
    obj.insert(QLatin1String(Field::Args), args);
    return obj;
}

QJsonObject buildEvent(const QString& target, const QString& signalName, const QJsonValue& args)
{
    QJsonObject obj;
    obj.insert(QLatin1String(Field::Type), QLatin1String(ResponseType::Event));
    obj.insert(QLatin1String(Field::Target), target);
    obj.insert(QLatin1String(Field::Signal), signalName);
    obj.insert(QLatin1String(Field::Args), args);
    return obj;
}

QJsonObject buildError(qint64 id, const QString& code, const QString& message, const QJsonObject& detail)
{
    QJsonObject obj;
    obj.insert(QLatin1String(Field::Type), QLatin1String(ResponseType::Error));
    if (id != 0) {
        obj.insert(QLatin1String(Field::Id), static_cast<double>(id));
    }
    obj.insert(QLatin1String(Field::Code), code);
    obj.insert(QLatin1String(Field::Message), message);
    if (!detail.isEmpty()) {
        obj.insert(QLatin1String(Field::Detail), detail);
    }
    return obj;
}

namespace {

QJsonArray listToJson(const QVariantList& list)
{
    QJsonArray arr;
    for (const QVariant& item : list) {
        arr.append(variantToJson(item));
    }
    return arr;
}

QJsonObject mapToJson(const QVariantMap& map)
{
    QJsonObject obj;
    // cbegin/cend, so that the implicitly shared map does not detach on
    // each event broadcast in the hot path.
    for (auto it = map.cbegin(); it != map.cend(); ++it) {
        obj.insert(it.key(), variantToJson(it.value()));
    }
    return obj;
}

} // namespace

QByteArray writeLine(const QJsonObject& obj)
{
    QByteArray bytes = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    bytes.append('\n');
    return bytes;
}

QJsonValue variantToJson(const QVariant& v)
{
    if (!v.isValid()) {
        return QJsonValue::Null;
    }
    // An untyped QML function returns a JavaScript value, and an object or
    // array from it would otherwise go out as its toString(), "".
    if (v.metaType() == QMetaType::fromType<QJSValue>()) {
        const QVariant converted = v.value<QJSValue>().toVariant(QJSValue::ConvertJSObjects);
        // The conversion gives a function back as it is. JSON has no form
        // for one, and converting it again would never end.
        if (converted.metaType() == QMetaType::fromType<QJSValue>()) {
            return QJsonValue::Null;
        }
        return variantToJson(converted);
    }
    switch (v.typeId()) {
    case QMetaType::Nullptr:
        // A JSON null that came in as an argument, or a JavaScript null.
        return QJsonValue::Null;
    case QMetaType::Bool:
        return v.toBool();
    case QMetaType::Int:
    case QMetaType::Short:
    case QMetaType::Long:
    case QMetaType::LongLong:
    case QMetaType::SChar:
    case QMetaType::Char:
        // Signed integer metatypes (≤64-bit). JSON numbers are IEEE
        // 754 doubles with 53 bits of integer precision; values
        // above 2^53 lose precision on the wire, but values that
        // fit in 53 bits round-trip exactly. A char is a number here,
        // as the router takes a number for a char parameter.
        return static_cast<double>(v.toLongLong());
    case QMetaType::UInt:
    case QMetaType::UShort:
    case QMetaType::ULong:
    case QMetaType::ULongLong:
    case QMetaType::UChar:
        // Unsigned variants must NOT route through toLongLong: on
        // LP64 systems ULong is 64-bit unsigned, and values above
        // LLONG_MAX wrap to negative when cast to qint64. Route
        // through the unsigned converter so the magnitude survives
        // (subject to the same 2^53 JSON precision ceiling).
        return static_cast<double>(v.toULongLong());
    case QMetaType::Double:
    case QMetaType::Float:
    case QMetaType::Float16:
        return v.toDouble();
    case QMetaType::QString:
        return v.toString();
    case QMetaType::QDateTime: {
        // QVariant::toString() gives local time with no offset, which a
        // reader in another time zone cannot place.
        const QDateTime dateTime = v.toDateTime();
        return dateTime.isValid() ? QJsonValue(dateTime.toUTC().toString(Qt::ISODateWithMs))
                                  : QJsonValue(QJsonValue::Null);
    }
    case QMetaType::QStringList: {
        QJsonArray arr;
        // toStringList() returns an owned copy, so range-for is safe
        // (no detach risk on a temporary).
        for (const QString& s : v.toStringList()) {
            arr.append(s);
        }
        return arr;
    }
    case QMetaType::QVariantList:
        return listToJson(v.toList());
    case QMetaType::QVariantMap:
        return mapToJson(v.toMap());
    case QMetaType::QVariantHash: {
        QJsonObject obj;
        const QVariantHash hash = v.toHash();
        for (auto it = hash.cbegin(); it != hash.cend(); ++it) {
            obj.insert(it.key(), variantToJson(it.value()));
        }
        return obj;
    }
    case QMetaType::QJsonValue:
        return v.toJsonValue();
    case QMetaType::QJsonObject:
        return v.toJsonObject();
    case QMetaType::QJsonArray:
        return v.toJsonArray();
    case QMetaType::QJsonDocument: {
        const QJsonDocument document = v.toJsonDocument();
        if (document.isObject()) {
            return document.object();
        }
        return document.isArray() ? QJsonValue(document.array()) : QJsonValue(QJsonValue::Null);
    }
    case QMetaType::QByteArray:
    case QMetaType::QChar:
    case QMetaType::Char16:
    case QMetaType::Char32:
        // Text. The container test below would take a QByteArray for a
        // list of its bytes.
        return v.toString();
    default:
        break;
    }
    // A container that Qt can iterate, such as QList<int> or
    // QMap<QString, int>. Without this it would go out as "".
    if (v.canConvert<QVariantList>()) {
        return listToJson(v.toList());
    }
    if (v.canConvert<QVariantMap>()) {
        return mapToJson(v.toMap());
    }
    // Another type: the string form, so that the wire gains no JSON shape
    // of its own for each metatype.
    return v.toString();
}

} // namespace PhosphorIpc
