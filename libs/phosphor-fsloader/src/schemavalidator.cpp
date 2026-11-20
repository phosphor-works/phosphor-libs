// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorFsLoader/SchemaValidator.h>

#include <QtCore/QCborValue>
#include <QtCore/QFile>
#include <QtCore/QIODevice>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QLoggingCategory>
#include <QtCore/QString>

// jsoncons is pinned (or found on the system) and confined to this
// translation unit, the one built with exceptions: it throws when a schema
// does not compile.
#include <jsoncons/json.hpp>
#include <jsoncons_ext/jsonschema/jsonschema.hpp>

#include <algorithm>
#include <cmath>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace PhosphorFsLoader {

namespace {

// Sorted members, so that an object of many members is built and searched in
// O(n log n) and two objects compare equal whatever order their members were
// written in (const, enum).
using Json = jsoncons::json;
namespace jsonschema = jsoncons::jsonschema;

/// The most errors validate() lists; the rest are counted in one more entry.
constexpr qsizetype kMaxErrors = 100;
/// The most characters of a path or a message an error keeps.
constexpr qsizetype kMaxTextLength = 500;

QString fromUtf8(std::string_view text)
{
    return QString::fromUtf8(text.data(), qsizetype(text.size()));
}

/// @p text cut to kMaxTextLength characters, never between the two halves of a
/// surrogate pair, with an ellipsis where it was cut.
QString clipped(QString text)
{
    if (text.size() <= kMaxTextLength) {
        return text;
    }
    qsizetype cut = kMaxTextLength;
    if (text.at(cut - 1).isHighSurrogate()) {
        --cut;
    }
    text.truncate(cut);
    text.append(QChar(0x2026));
    return text;
}

/// One JSON Pointer reference token, escaped as jsoncons escapes them ("~" to
/// "~0", "/" to "~1"). U+0000 and unpaired surrogates, which only appear in
/// the path of a name refused for holding them, are written as \uXXXX so the
/// log shows them.
QString pointerToken(const QString& name)
{
    QString token;
    token.reserve(name.size());
    for (qsizetype i = 0; i < name.size(); ++i) {
        const QChar c = name.at(i);
        if (c == u'~') {
            token.append(QLatin1StringView("~0"));
        } else if (c == u'/') {
            token.append(QLatin1StringView("~1"));
        } else if (c.isHighSurrogate() && i + 1 < name.size() && name.at(i + 1).isLowSurrogate()) {
            token.append(c);
            token.append(name.at(++i));
        } else if (c.unicode() == 0 || c.isSurrogate()) {
            token.append(QStringLiteral("\\u%1").arg(uint(c.unicode()), 4, 16, QChar(u'0')));
        } else {
            token.append(c);
        }
    }
    return token;
}

/// The errors of one schema check or one validation: the first kMaxErrors
/// kept, the rest only counted.
class ErrorList
{
public:
    /// False once kMaxErrors are kept: the caller then only calls countOne(),
    /// and does not build the text of an error nobody reads.
    bool wantsMore() const
    {
        return m_errors.size() < kMaxErrors;
    }

    void add(const QString& path, const QString& message)
    {
        ++m_total;
        if (wantsMore()) {
            m_errors.append(SchemaValidator::Error{clipped(path), clipped(message)});
        }
    }

    void countOne()
    {
        ++m_total;
    }

    bool isEmpty() const
    {
        return m_total == 0;
    }

    /// The kept errors, then, when some were only counted, one entry with an
    /// empty path that says how many.
    QList<SchemaValidator::Error> take()
    {
        QList<SchemaValidator::Error> errors = std::move(m_errors);
        if (m_total > errors.size()) {
            errors.append(SchemaValidator::Error{
                QString(), QStringLiteral("and %1 more errors not listed").arg(m_total - errors.size())});
        }
        m_errors.clear();
        m_total = 0;
        return errors;
    }

    /// The first error, for the one line a schema that failed reports.
    QString summary() const
    {
        if (m_errors.isEmpty()) {
            return QString();
        }
        const SchemaValidator::Error& first = m_errors.first();
        QString text =
            (first.path.isEmpty() ? QStringLiteral("(root)") : first.path) + QStringLiteral(": ") + first.message;
        if (m_total > 1) {
            text += QStringLiteral(" (and %1 more)").arg(m_total - 1);
        }
        return text;
    }

private:
    QList<SchemaValidator::Error> m_errors;
    qsizetype m_total = 0;
};

/// Where a walk over a QJsonValue is, as a JSON Pointer built only when an
/// error needs it.
class PointerPath
{
public:
    void push(const QString& name)
    {
        m_tokens.push_back(Token{name, -1});
    }
    void push(qsizetype index)
    {
        m_tokens.push_back(Token{QString(), index});
    }
    void pop()
    {
        m_tokens.pop_back();
    }

    QString toString() const
    {
        QString path;
        for (const Token& token : m_tokens) {
            path.append(QChar(u'/'));
            path.append(token.index < 0 ? pointerToken(token.name) : QString::number(token.index));
        }
        return path;
    }

private:
    struct Token
    {
        QString name;
        qsizetype index;
    };
    std::vector<Token> m_tokens;
};

// Converts a QJsonValue to the jsoncons value the validator reads, and refuses
// what jsoncons would read differently from the text: a string or member name
// with U+0000 (jsoncons matches `pattern` and looks names up through a C
// string, which ends there), one with an unpaired surrogate (UTF-8 has no
// encoding for it, so names would collapse into one), and a number of
// magnitude 2^63 or more (jsoncons' integer check converts it to int64, which
// is undefined behaviour). Each refusal is an error at the node's path.
//
// A number that Qt holds as an integer stays one. Qt holds every integral
// number within int64 as one, 3.0 and 3e0 included, which is what 2020-12
// means by an integer. Any other number is a double: a fraction, or a
// magnitude past int64, refused here.
class Converter
{
public:
    explicit Converter(ErrorList& errors)
        : m_errors(errors)
    {
    }

    Json convert(const QJsonValue& value)
    {
        switch (value.type()) {
        case QJsonValue::Bool:
            return Json(value.toBool());
        case QJsonValue::Double: {
            const QCborValue number = QCborValue::fromJsonValue(value);
            if (number.isInteger()) {
                return Json(static_cast<int64_t>(number.toInteger()));
            }
            const double real = value.toDouble();
            if (!(std::fabs(real) < 0x1p63)) {
                refuse(QStringLiteral("number %1 is out of range: magnitudes of 2^63 and more are refused").arg(real));
                return Json::null();
            }
            return Json(real);
        }
        case QJsonValue::String:
            return Json(text(value.toString(), false));
        case QJsonValue::Array: {
            const QJsonArray array = value.toArray();
            Json result(jsoncons::json_array_arg);
            result.reserve(std::size_t(array.size()));
            for (qsizetype i = 0; i < array.size(); ++i) {
                m_path.push(i);
                result.push_back(convert(array.at(i)));
                m_path.pop();
            }
            return result;
        }
        case QJsonValue::Object: {
            const QJsonObject object = value.toObject();
            std::vector<std::pair<std::string, Json>> members;
            members.reserve(std::size_t(object.size()));
            for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
                const QString name = it.key();
                m_path.push(name);
                std::string key = text(name, true);
                members.emplace_back(std::move(key), convert(it.value()));
                m_path.pop();
            }
            // Qt orders names by UTF-16 code unit and jsoncons by UTF-8 byte,
            // which differ once a name holds a character past U+FFFF. Sorted
            // the jsoncons way, each insertion is an append.
            std::sort(members.begin(), members.end(), [](const auto& a, const auto& b) {
                return a.first < b.first;
            });
            Json result(jsoncons::json_object_arg);
            result.reserve(members.size());
            for (auto& member : members) {
                result.insert_or_assign(member.first, std::move(member.second));
            }
            return result;
        }
        case QJsonValue::Null:
        case QJsonValue::Undefined:
            break;
        }
        return Json::null();
    }

private:
    std::string text(const QString& text, bool isName)
    {
        if (text.contains(QChar(u'\0'))) {
            refuse(isName ? QStringLiteral("member name contains U+0000, which is refused")
                          : QStringLiteral("string contains U+0000, which is refused"));
        } else if (!text.isValidUtf16()) {
            refuse(isName ? QStringLiteral("member name contains an unpaired UTF-16 surrogate, which is refused")
                          : QStringLiteral("string contains an unpaired UTF-16 surrogate, which is refused"));
        }
        return text.toStdString();
    }

    void refuse(const QString& message)
    {
        if (m_errors.wantsMore()) {
            m_errors.add(m_path.toString(), message);
        } else {
            m_errors.countOne();
        }
    }

    ErrorList& m_errors;
    PointerPath m_path;
};

// The $schema values read as 2020-12: the meta-schema's URI, with or without
// an empty fragment, as Python's jsonschema reads it too.
bool is202012(const QJsonValue& value)
{
    return value == QLatin1StringView("https://json-schema.org/draft/2020-12/schema")
        || value == QLatin1StringView("https://json-schema.org/draft/2020-12/schema#");
}

// The formats jsoncons checks and that this validator lets a schema use.
// jsoncons ignores every other format, which would leave a schema that names
// one checking nothing, and its "hostname" check is wrong (it refuses the digit
// 9 and reads out of bounds on an empty string), so those are refused.
bool isAssertedFormat(const QString& format)
{
    static const QStringList formats{
        QStringLiteral("date-time"),     QStringLiteral("date"),         QStringLiteral("time"),
        QStringLiteral("email"),         QStringLiteral("ipv4"),         QStringLiteral("ipv6"),
        QStringLiteral("regex"),         QStringLiteral("json-pointer"), QStringLiteral("uri"),
        QStringLiteral("uri-reference"),
    };
    return formats.contains(format);
}

// Keywords of other drafts that 2020-12 does not know, and so would ignore
// without a word: a schema written with them would check less than its
// author meant. Refused, with what 2020-12 says instead.
QString otherDraftKeyword(const QString& keyword)
{
    if (keyword == QLatin1StringView("definitions")) {
        return QStringLiteral("is Draft 7's; 2020-12 names it $defs");
    }
    if (keyword == QLatin1StringView("dependencies")) {
        return QStringLiteral("is Draft 7's; 2020-12 splits it into dependentRequired and dependentSchemas");
    }
    if (keyword == QLatin1StringView("additionalItems")) {
        return QStringLiteral("is Draft 7's; 2020-12 uses items beside prefixItems");
    }
    if (keyword == QLatin1StringView("$recursiveRef") || keyword == QLatin1StringView("$recursiveAnchor")) {
        return QStringLiteral("is Draft 2019-09's; 2020-12 uses $dynamicRef and $dynamicAnchor");
    }
    return QString();
}

bool isSubschemaKeyword(const QString& keyword)
{
    static const QStringList keywords{
        QStringLiteral("additionalProperties"),
        QStringLiteral("contains"),
        QStringLiteral("contentSchema"),
        QStringLiteral("else"),
        QStringLiteral("if"),
        QStringLiteral("items"),
        QStringLiteral("not"),
        QStringLiteral("propertyNames"),
        QStringLiteral("then"),
        QStringLiteral("unevaluatedItems"),
        QStringLiteral("unevaluatedProperties"),
    };
    return keywords.contains(keyword);
}

bool isSubschemaMapKeyword(const QString& keyword)
{
    static const QStringList keywords{
        QStringLiteral("$defs"),
        QStringLiteral("dependentSchemas"),
        QStringLiteral("patternProperties"),
        QStringLiteral("properties"),
    };
    return keywords.contains(keyword);
}

bool isSubschemaArrayKeyword(const QString& keyword)
{
    static const QStringList keywords{
        QStringLiteral("allOf"),
        QStringLiteral("anyOf"),
        QStringLiteral("oneOf"),
        QStringLiteral("prefixItems"),
    };
    return keywords.contains(keyword);
}

// What the meta-schema allows but this validator refuses, found by walking
// every subschema: $schema naming another dialect, other drafts' keywords,
// the array form of items, and a format jsoncons does not check. Values the
// meta-schema refuses (a subschema that is not one) are left to it.
void checkDialect(const QJsonValue& schema, PointerPath& path, ErrorList& errors)
{
    if (!schema.isObject()) {
        return;
    }
    const QJsonObject object = schema.toObject();
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        const QString keyword = it.key();
        const QJsonValue value = it.value();
        path.push(keyword);
        if (keyword == QLatin1StringView("$schema")) {
            if (!is202012(value)) {
                errors.add(path.toString(),
                           QStringLiteral("names a dialect other than 2020-12 "
                                          "(https://json-schema.org/draft/2020-12/schema), the only one supported"));
            }
        } else if (const QString why = otherDraftKeyword(keyword); !why.isEmpty()) {
            errors.add(path.toString(), keyword + QLatin1Char(' ') + why);
        } else if (keyword == QLatin1StringView("format")) {
            if (value.isString() && !isAssertedFormat(value.toString())) {
                errors.add(path.toString(),
                           QStringLiteral("format \"%1\" is not one this validator checks").arg(value.toString()));
            }
        } else if (keyword == QLatin1StringView("items") && value.isArray()) {
            errors.add(path.toString(), QStringLiteral("an array under items is Draft 7's; 2020-12 uses prefixItems"));
        } else if (isSubschemaKeyword(keyword)) {
            checkDialect(value, path, errors);
        } else if (isSubschemaMapKeyword(keyword) && value.isObject()) {
            const QJsonObject members = value.toObject();
            for (auto member = members.constBegin(); member != members.constEnd(); ++member) {
                path.push(member.key());
                checkDialect(member.value(), path, errors);
                path.pop();
            }
        } else if (isSubschemaArrayKeyword(keyword) && value.isArray()) {
            const QJsonArray items = value.toArray();
            for (qsizetype i = 0; i < items.size(); ++i) {
                path.push(i);
                checkDialect(items.at(i), path, errors);
                path.pop();
            }
        }
        path.pop();
    }
}

jsonschema::evaluation_options evaluationOptions()
{
    // 2020-12 unless the schema names another draft in $schema (which
    // checkDialect refuses). `format` is asserted, not only annotated: a date
    // that is not a date fails.
    return jsonschema::evaluation_options{}
        .default_version(jsonschema::schema_version::draft202012())
        .require_format_validation(true);
}

// The 2020-12 meta-schema jsoncons carries, compiled once per process, the
// first time a schema is checked against it.
const jsonschema::json_schema<Json>& metaSchema()
{
    static const jsonschema::json_schema<Json> schema = jsonschema::make_json_schema(
        jsonschema::draft202012::schema_draft202012<Json>::get_schema(), evaluationOptions());
    return schema;
}

// Which jsoncons messages addMessage() adds: every one, or only those that
// carry no reasons of their own (for the one line a refused schema reports,
// which should be the specific reason and not "found unmatched schemas").
enum class Messages {
    All,
    Leaves
};

// Adds one jsoncons message, then the reasons it carries: why each branch of
// an anyOf, oneOf or allOf failed, each at its own node and prefixed with the
// branch.
void addMessage(ErrorList& errors, const jsonschema::validation_message& message, const QString& prefix, Messages which)
{
    if (which == Messages::Leaves && !message.details().empty()) {
        // Only its reasons, below.
    } else if (!errors.wantsMore()) {
        errors.countOne();
    } else {
        QString path = fromUtf8(message.instance_location().to_string());
        const std::string& text = message.message();
        // An additionalProperties subschema that fails is reported at the
        // object and names the member; the error belongs at the member.
        constexpr std::string_view head = "Additional property '";
        constexpr std::string_view tail = "' found but was invalid.";
        if (message.keyword() == "additionalProperties" && text.size() >= head.size() + tail.size()
            && text.compare(0, head.size(), head) == 0
            && text.compare(text.size() - tail.size(), tail.size(), tail) == 0) {
            path += QChar(u'/')
                + pointerToken(
                        fromUtf8(std::string_view(text).substr(head.size(), text.size() - head.size() - tail.size())));
        }
        errors.add(path, prefix + fromUtf8(text));
    }
    if (message.details().empty()) {
        return;
    }
    const std::string parent = message.eval_path().to_string();
    for (const jsonschema::validation_message& detail : message.details()) {
        QString branch = fromUtf8(message.keyword());
        const std::string eval = detail.eval_path().to_string();
        if (eval.size() > parent.size() + 1 && eval.compare(0, parent.size(), parent) == 0
            && eval[parent.size()] == '/') {
            const std::size_t end = eval.find('/', parent.size() + 1);
            const std::string token = eval.substr(parent.size() + 1, end - parent.size() - 1);
            if (!token.empty() && std::all_of(token.begin(), token.end(), [](char c) {
                    return c >= '0' && c <= '9';
                })) {
                branch += QChar(u'/') + fromUtf8(token);
            }
        }
        // The branches of the meta-schema mean nothing to a schema's author.
        addMessage(errors, detail, which == Messages::All ? prefix + branch + QStringLiteral(": ") : prefix, which);
    }
}

} // namespace

struct SchemaValidator::Private
{
    // Compiled once at construction and reused by every validate(). Empty when
    // the schema was refused or did not compile, and then every validate()
    // fails closed.
    std::optional<jsonschema::json_schema<Json>> schema;

    // Why the schema was refused, reported by every validate().
    QString compileError;
};

SchemaValidator::SchemaValidator(const QByteArray& schemaJson)
    : d(std::make_unique<Private>())
{
    // Qt reads the text, as it reads the documents: comments are refused, and
    // of two members with one name the last is kept, as Python's json module
    // keeps it in CI.
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(schemaJson, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        d->compileError = QStringLiteral("schema is not valid JSON: %1 at offset %2")
                              .arg(parseError.errorString())
                              .arg(parseError.offset);
        return;
    }
    if (!document.isObject()) {
        d->compileError = QStringLiteral("schema root is not a JSON object");
        return;
    }
    const QJsonObject root = document.object();

    // jsoncons throws on a schema it cannot compile, which includes a
    // `pattern` that is not a valid ECMAScript regex. The schemas compiled
    // here are our own embedded resources, checked in CI, so this is a
    // build-time guarantee in practice. A bad one still degrades to
    // fail-closed validation rather than ending the process.
    try {
        ErrorList problems;
        PointerPath path;
        checkDialect(root, path, problems);
        if (!problems.isEmpty()) {
            d->compileError = QStringLiteral("schema refused at %1").arg(problems.summary());
            return;
        }
        Json schema = Converter(problems).convert(root);
        if (!problems.isEmpty()) {
            d->compileError = QStringLiteral("schema refused at %1").arg(problems.summary());
            return;
        }
        // jsoncons compiles what it understands and passes over the rest, so
        // a keyword with a value of the wrong kind ({"enum": "x"}, a negative
        // maxItems) would compile and then fail, or check nothing, at every
        // validate(). The meta-schema refuses it here.
        metaSchema().validate(schema, [&problems](const jsonschema::validation_message& message) {
            addMessage(problems, message, QString(), Messages::Leaves);
            return jsonschema::walk_state::advance;
        });
        if (!problems.isEmpty()) {
            d->compileError =
                QStringLiteral("schema does not conform to the 2020-12 meta-schema at %1").arg(problems.summary());
            return;
        }
        d->schema.emplace(jsonschema::make_json_schema(std::move(schema), evaluationOptions()));
    } catch (const std::exception& e) {
        d->compileError = QStringLiteral("schema failed to compile: %1").arg(QString::fromUtf8(e.what()));
    } catch (...) {
        d->compileError = QStringLiteral("schema failed to compile: unknown error");
    }
}

SchemaValidator SchemaValidator::fromResource(const QString& resourcePath, const QLoggingCategory& category)
{
    QFile file(resourcePath);
    if (!file.open(QIODevice::ReadOnly)) {
        // A missing embedded resource is a build error, not user input. Fail
        // closed: an empty-bytes validator reports invalid and rejects every
        // document, rather than silently passing them through unvalidated.
        qCWarning(category) << "JSON schema resource missing:" << resourcePath
                            << "- all documents will be rejected (fail-closed)";
        return SchemaValidator(QByteArray());
    }
    return SchemaValidator(file.readAll());
}

SchemaValidator::~SchemaValidator() = default;
SchemaValidator::SchemaValidator(SchemaValidator&&) noexcept = default;
SchemaValidator& SchemaValidator::operator=(SchemaValidator&&) noexcept = default;

bool SchemaValidator::isValid() const
{
    return d && d->schema.has_value();
}

std::optional<QList<SchemaValidator::Error>> SchemaValidator::validate(const QJsonValue& document) const
{
    if (!d) {
        return QList<Error>{Error{QString(), QStringLiteral("schema validator was moved from")}};
    }
    if (!d->schema) {
        return QList<Error>{Error{QString(), d->compileError}};
    }

    // Validation with a reporter does not throw by design, but std::regex can
    // (a match too complex for it). This translation unit is built with
    // exceptions and every caller is not, so nothing may leave it: fail closed
    // instead, as the constructor does.
    try {
        ErrorList errors;
        const Json instance = Converter(errors).convert(document);
        if (errors.isEmpty()) {
            d->schema->validate(instance, [&errors](const jsonschema::validation_message& message) {
                addMessage(errors, message, QString(), Messages::All);
                return jsonschema::walk_state::advance;
            });
        }
        if (errors.isEmpty()) {
            return std::nullopt;
        }
        return errors.take();
    } catch (const std::exception& e) {
        return QList<Error>{
            Error{QString(), QStringLiteral("schema validation error: %1").arg(QString::fromUtf8(e.what()))}};
    } catch (...) {
        return QList<Error>{Error{QString(), QStringLiteral("schema validation error: unknown")}};
    }
}

void logSchemaErrors(const QLoggingCategory& category, const QList<SchemaValidator::Error>& errors)
{
    for (qsizetype i = 0; i < errors.size(); ++i) {
        const SchemaValidator::Error& error = errors.at(i);
        // The entry past the last kept error, which validate() adds when it
        // only counted the rest, is a count and has no node.
        if (i == kMaxErrors && error.path.isEmpty()) {
            qCWarning(category).nospace() << "  " << error.message;
            continue;
        }
        qCWarning(category).nospace() << "  " << (error.path.isEmpty() ? QStringLiteral("(root)") : error.path) << ": "
                                      << error.message;
    }
}

} // namespace PhosphorFsLoader
