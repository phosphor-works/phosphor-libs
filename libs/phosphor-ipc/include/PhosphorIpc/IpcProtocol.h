// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <PhosphorIpc/phosphoripc_export.h>

#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QVariant>
#include <QVariantList>
#include <QtTypes>

#include <optional>

// Wire-format definitions for the JSON-over-Unix-socket protocol the
// IpcRouter speaks. The router, IpcClient and the phosphorctl program
// share them, so the constants and parsers stay in lockstep.
// Mirrors the split of libs/phosphor-protocol (PhosphorProtocol/
// WindowTypes.h and WindowMarshalling.h). JSON is self-describing, so
// the header holds the type constants only, no Qt-marshalling code.
//
// Wire shape: one JSON object per line, '\n'-terminated, UTF-8. A line
// holds at most 1024 levels of nested arrays and objects, which is the
// limit of the JSON parser on both sides.
//
// CMake mirrors the protocol-version constant via the
// PHOSPHOR_IPC_PROTOCOL_VERSION compile definition so this header
// and the build agree at compile time. Bumped whenever the
// JSON shape on the wire changes incompatibly.

namespace PhosphorIpc {

constexpr int ProtocolVersion = 1;

// The longest request line that the router reads, its newline included.
// The router closes a connection that sends a longer one, which fails
// every request of that connection, so a client refuses such a request
// before it sends it. IpcClient does.
inline constexpr qint64 MaxRequestLineBytes = 1024 * 1024;

// The most arguments of a call. A method with more parameters cannot be
// called through the router, which answers such a call with INVALID_ARG.
inline constexpr int MaxCallArgs = 10;

// The most entries that the `args` of a request holds. A line with more
// is malformed. No valid call comes near it, see MaxCallArgs.
inline constexpr qsizetype MaxRequestArgs = 4096;

#ifdef PHOSPHOR_IPC_PROTOCOL_VERSION
static_assert(PHOSPHOR_IPC_PROTOCOL_VERSION == ProtocolVersion,
              "CMake's PHOSPHOR_IPC_PROTOCOL_VERSION must match IpcProtocol.h's ProtocolVersion");
#endif

// Field names used across the wire. Declared as constexpr string
// literals so usage sites can pass them to QJsonObject without
// constructing QString, and the compiler catches typos at the
// definition site instead of letting them silently land in the
// wire format.
namespace Field {
constexpr auto Type = "type";
constexpr auto Id = "id";
constexpr auto Target = "target";
constexpr auto Fn = "fn";
constexpr auto Args = "args";
constexpr auto Signal = "signal";
constexpr auto SubscriptionId = "subscriptionId";
constexpr auto Result = "result";
constexpr auto Code = "code";
constexpr auto Message = "message";
constexpr auto Detail = "detail";
} // namespace Field

// Request `type` discriminator values. Sent by the client.
namespace RequestType {
constexpr auto Call = "call";
constexpr auto List = "list";
constexpr auto Schema = "schema";
constexpr auto Subscribe = "subscribe";
constexpr auto Unsubscribe = "unsubscribe";
} // namespace RequestType

// Response `type` discriminator values. Sent by the server.
namespace ResponseType {
constexpr auto Reply = "reply"; // call / list / schema result, subscribe / unsubscribe ack
// Pushed signal payload: to a subscriber (carries `subscriptionId`), or
// to one connection through IpcRouter::sendEvent (carries `target` and
// `signal` in place of a subscriptionId).
constexpr auto Event = "event";
// A failure. It has the id of the request that failed. It has no id for
// a line that the router could not read. Under the id of a subscription
// it ends that subscription.
constexpr auto Error = "error";
} // namespace ResponseType

// Error `code` strings the server returns. Stable across protocol
// version 1, adding new codes is safe; renaming or removing one
// is a protocol break. A deferred call may also fail with a code its
// target picked (IpcPendingReply::fail), which is not in this list.
namespace ErrorCode {
constexpr auto NoSuchTarget = "NO_SUCH_TARGET";
constexpr auto NoSuchFn = "NO_SUCH_FN";
constexpr auto NoSuchSignal = "NO_SUCH_SIGNAL";
constexpr auto NoSuchSubscription = "NO_SUCH_SUBSCRIPTION";
constexpr auto InvalidArg = "INVALID_ARG";
constexpr auto InvocationFailed = "INVOCATION_FAILED";
constexpr auto MalformedRequest = "MALFORMED_REQUEST";
// The call ran and failed: the target called
// IpcCallContext::sendErrorReply, or IpcPendingReply::fail without a
// code. Also a deferral past the router's cap on waiting deferred calls.
constexpr auto CallFailed = "CALL_FAILED";
// The target deferred the call's reply and dropped the IpcPendingReply
// without answering it.
constexpr auto CallAbandoned = "CALL_ABANDONED";
} // namespace ErrorCode

// Parsed request shape, populated by parseRequest from a single
// JSON line. A field that the line does not hold keeps its default.
// parseRequest checks the fields that each known type needs. The caller
// checks only that `type` is one it knows.
struct PHOSPHORIPC_EXPORT Request
{
    QString type;
    qint64 id = 0;
    QString target;
    QString fn;
    // Spelled `signalName`, not `signal`, to stay clear of the Qt keyword
    // `signals` and of signal() from <csignal>.
    QString signalName;
    qint64 subscriptionId = 0;
    QVariantList args;
};

// Parse one NDJSON line into a Request. Returns std::nullopt and
// populates parseError on malformed input: invalid JSON, missing
// required `type` field, missing per-type required fields (`target`
// for call/schema/subscribe, `fn` for call, `signal` for subscribe,
// non-zero `subscriptionId` for unsubscribe), an `id` or
// `subscriptionId` that integerId() refuses, or an `args` value that is
// not an array of at most MaxRequestArgs entries. An `id`, a
// `subscriptionId` or `args` that is absent or null takes its default.
// Callers are expected to reply with a MALFORMED_REQUEST error.
//
// `requestId`, when given, gets the id of the line, also when a later
// check refuses the request, so that the error can name the request it
// answers. It is 0 for a line without a valid id.
//
// A subscribe needs no id. A subscription made without one has the id 0,
// which no unsubscribe can name: it ends with its connection.
[[nodiscard]] PHOSPHORIPC_EXPORT std::optional<Request> parseRequest(const QByteArray& line, QString* parseError,
                                                                     qint64* requestId = nullptr);

// The integer that a JSON `id` or `subscriptionId` holds. Nothing for a
// value that is not a number, has a fraction, or is outside the range
// from -2^53 to 2^53. In that range every JSON reader has the same
// integer for a number. Outside it a reader that keeps numbers as
// doubles rounds, and two ids become one.
[[nodiscard]] PHOSPHORIPC_EXPORT std::optional<qint64> integerId(const QJsonValue& value);

// Encoders, build the QJsonObject body for each response variant.
// The router serialises the object to bytes via writeLine() below.
[[nodiscard]] PHOSPHORIPC_EXPORT QJsonObject buildReply(qint64 id, const QJsonValue& result);
[[nodiscard]] PHOSPHORIPC_EXPORT QJsonObject buildEvent(qint64 subscriptionId, const QJsonValue& args);
// An event addressed to one connection, not to a subscription: named by
// `target` and `signal` (IpcRouter::sendEvent).
[[nodiscard]] PHOSPHORIPC_EXPORT QJsonObject buildEvent(const QString& target, const QString& signalName,
                                                        const QJsonValue& args);
[[nodiscard]] PHOSPHORIPC_EXPORT QJsonObject buildError(qint64 id, const QString& code, const QString& message,
                                                        const QJsonObject& detail = {});

// Serialise a QJsonObject to a single NDJSON line, compact JSON
// followed by '\n'. Output is UTF-8 bytes ready to push into a
// QLocalSocket.
[[nodiscard]] PHOSPHORIPC_EXPORT QByteArray writeLine(const QJsonObject& obj);

// Convert a QVariant to a JSON value. The router's call-return path and
// IpcTarget's emitEvent path share it, so the wire JSON has one shape.
//
// | Value | JSON |
// | --- | --- |
// | bool | boolean |
// | every integer type, char, signed char and unsigned char included | number |
// | float, double, qfloat16 | number, and null for NaN and the infinities |
// | QString, QChar, char16_t, char32_t | string |
// | QByteArray | string, the bytes read as UTF-8 |
// | QDateTime | string, ISO 8601 in UTC with milliseconds and a `Z` |
// | QStringList, QVariantList, a list or set that Qt can iterate | array |
// | QVariantMap, QVariantHash, a map with string keys | object |
// | QJsonValue, QJsonObject, QJsonArray, QJsonDocument | themselves |
// | QJSValue (what an untyped QML function returns) | its JavaScript value |
// | an invalid QVariant, std::nullptr_t | null |
// | any other type | string, QVariant::toString() |
//
// A JavaScript object becomes an object, an array becomes an array, and
// a function becomes null. QVariant::toString() is empty for a type without a string form, such
// as a QObject pointer, a QPointF or a QRegularExpression. An integer
// above 2^53 loses precision, as every JSON number does.
[[nodiscard]] PHOSPHORIPC_EXPORT QJsonValue variantToJson(const QVariant& v);

} // namespace PhosphorIpc
