// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "mcpschema.h"

#include "mcpschemacheck.h"
#include "mcpschemanode.h"

namespace PhosphorMcp {

struct McpSchema::Private
{
    Schema::CompiledSchema schema;
    bool compiled = false;
};

McpSchema::McpSchema()
    : d(std::make_unique<Private>())
{
}

McpSchema::~McpSchema() = default;

QString McpSchema::compile(const QJsonObject& schema, Root root)
{
    if (d->compiled) {
        return QStringLiteral(": the schema was compiled before");
    }
    const QString error = Schema::compileSchema(schema, root == Root::Object, d->schema);
    d->compiled = error.isEmpty();
    return error;
}

QStringList McpSchema::validate(const QJsonValue& value) const
{
    if (!d->compiled) {
        return {QStringLiteral(": the schema did not compile")};
    }
    return Schema::checkValue(d->schema, value);
}

} // namespace PhosphorMcp
