// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorFsLoader/SchemaValidator.h>

#include <QtCore/QCborValue>
#include <QtCore/QFile>
#include <QtCore/QIODevice>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QLoggingCategory>
#include <QtCore/QString>

// jsoncons is pinned (or found on the system) and confined to this
// translation unit, the one built with exceptions: it throws when a schema
// does not compile.
#include <jsoncons/json.hpp>
#include <jsoncons_ext/jsonschema/jsonschema.hpp>

#include <exception>
#include <optional>
#include <string>
#include <utility>

namespace PhosphorFsLoader {

namespace {

using Json = jsoncons::ojson;
namespace jsonschema = jsoncons::jsonschema;

// The document as jsoncons holds it. ojson keeps the members in their order.
// A number stays an integer when Qt's parser kept it as one, and a double
// otherwise, so "integer" means what it meant in the file.
Json toJson(const QJsonValue& value)
{
    switch (value.type()) {
    case QJsonValue::Bool:
        return Json(value.toBool());
    case QJsonValue::Double: {
        const QCborValue number = QCborValue::fromJsonValue(value);
        if (number.isInteger()) {
            return Json(static_cast<int64_t>(number.toInteger()));
        }
        return Json(value.toDouble());
    }
    case QJsonValue::String: {
        const QByteArray utf8 = value.toString().toUtf8();
        return Json(std::string(utf8.constData(), std::size_t(utf8.size())));
    }
    case QJsonValue::Array: {
        const QJsonArray array = value.toArray();
        Json result(jsoncons::json_array_arg);
        result.reserve(std::size_t(array.size()));
        for (const QJsonValue& item : array) {
            result.push_back(toJson(item));
        }
        return result;
    }
    case QJsonValue::Object: {
        const QJsonObject object = value.toObject();
        Json result(jsoncons::json_object_arg);
        for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
            const QByteArray key = it.key().toUtf8();
            result.insert_or_assign(std::string(key.constData(), std::size_t(key.size())), toJson(it.value()));
        }
        return result;
    }
    case QJsonValue::Null:
    case QJsonValue::Undefined:
        break;
    }
    return Json::null();
}

QString fromUtf8(const std::string& text)
{
    return QString::fromUtf8(text.data(), qsizetype(text.size()));
}

} // namespace

struct SchemaValidator::Private
{
    // Compiled once at construction and reused by every validate(). Empty when
    // the schema did not compile, and then every validate() fails closed.
    std::optional<jsonschema::json_schema<Json>> schema;

    // Why the schema did not compile, reported by every validate().
    QString compileError;
};

SchemaValidator::SchemaValidator(const QByteArray& schemaJson)
    : d(std::make_unique<Private>())
{
    // jsoncons throws on text that is not JSON and on a schema it cannot
    // compile, which includes a `pattern` that is not a valid ECMAScript
    // regex. The schemas compiled here are our own embedded resources, checked
    // in CI, so this is a build-time guarantee in practice. A bad one still
    // degrades to fail-closed validation rather than ending the process.
    try {
        Json schema = Json::parse(std::string_view(schemaJson.constData(), std::size_t(schemaJson.size())));
        if (!schema.is_object()) {
            d->compileError = QStringLiteral("schema root is not a JSON object");
            return;
        }
        // 2020-12 unless the schema names another draft in $schema. `format`
        // is asserted, not only annotated: a date that is not a date fails.
        const auto options = jsonschema::evaluation_options{}
                                 .default_version(jsonschema::schema_version::draft202012())
                                 .require_format_validation(true);
        d->schema.emplace(jsonschema::make_json_schema(std::move(schema), options));
    } catch (const jsoncons::ser_error& e) {
        d->compileError = QStringLiteral("schema is not valid JSON: %1").arg(QString::fromUtf8(e.what()));
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
    return d->schema.has_value();
}

std::optional<QList<SchemaValidator::Error>> SchemaValidator::validate(const QJsonValue& document) const
{
    if (!d->schema) {
        return QList<Error>{Error{QString(), d->compileError}};
    }

    // Validation with a reporter does not throw by design, but std::regex can
    // (a match too complex for it). This translation unit is built with
    // exceptions and every caller is not, so nothing may leave it: fail closed
    // instead, as the constructor does.
    try {
        QList<Error> errors;
        d->schema->validate(toJson(document), [&errors](const jsonschema::validation_message& message) {
            errors.append(Error{fromUtf8(message.instance_location().to_string()), fromUtf8(message.message())});
            return jsonschema::walk_state::advance;
        });
        if (errors.isEmpty()) {
            return std::nullopt;
        }
        return errors;
    } catch (const std::exception& e) {
        return QList<Error>{
            Error{QString(), QStringLiteral("schema validation error: %1").arg(QString::fromUtf8(e.what()))}};
    } catch (...) {
        return QList<Error>{Error{QString(), QStringLiteral("schema validation error: unknown")}};
    }
}

void logSchemaErrors(const QLoggingCategory& category, const QList<SchemaValidator::Error>& errors)
{
    for (const auto& error : errors) {
        qCWarning(category).nospace() << "  " << (error.path.isEmpty() ? QStringLiteral("(root)") : error.path) << ": "
                                      << error.message;
    }
}

} // namespace PhosphorFsLoader
