// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorEngineRuntime/ConfigMigration.h>

#include <PhosphorEngineRuntime/ConfigKeys.h>
#include <PhosphorEngineRuntime/ConfigMigrationUtil.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QLatin1String>
#include <QString>

namespace PhosphorEngineRuntime {

// ── v8 → v9: structured keys hold native JSON ──────────────────────────────
//
// The settings store reads a key typed as a JSON object or array from the
// native node only. Some configs still hold such a key as a JSON-ENCODED
// STRING:
//   - migrateV1ToV2 writes Animations/Profile that way, so every config that
//     came up from v1 (an INI or an early JSON config) carries one until the
//     settings app saves;
//   - builds from before the store wrote structured values natively stored
//     trigger lists and the per-algorithm settings as strings, and a v1 JSON
//     config carried those strings straight through migrateV1ToV2's moveKey.
// Until v9 the store parsed such a string on read. v9 converts it once, here,
// so the store no longer has to.
//
// Only a string that parses to the shape the key expects is rewritten. Any
// other value is left exactly as found: an absent key (nothing to do), a value
// that is already native, and a string that is not the right JSON (the store's
// absent-vs-malformed handling owns it, as before).
//
// Schema-migration freeze policy (mirrors migrateV6ToV7): the (group, key)
// list is the v8 schema's structured keys, frozen here as literals so later
// schema changes cannot retarget this step. It covers every key the v8 schema
// declares as QVariantMap or QVariantList; the v8 schema declares no
// QStringList key.

namespace {

enum class Shape {
    Object,
    Array,
};

struct StructuredKey
{
    QLatin1String group;
    QLatin1String key;
    Shape shape;
};

// Every key the v8 settings schema types as a JSON object (QVariantMap) or
// array (QVariantList), by its v8 group and key spelling.
constexpr StructuredKey kV8StructuredKeys[] = {
    // Objects.
    {QLatin1String("Animations"), QLatin1String("Profile"), Shape::Object},
    {QLatin1String("Animations"), QLatin1String("ShaderProfileTree"), Shape::Object},
    {QLatin1String("Animations"), QLatin1String("MotionProfileTree"), Shape::Object},
    {QLatin1String("Decorations"), QLatin1String("DecorationProfileTree"), Shape::Object},
    {QLatin1String("Overlays"), QLatin1String("OverlayShaderTree"), Shape::Object},
    {QLatin1String("Tiling.Algorithm"), QLatin1String("PerAlgorithmSettings"), Shape::Object},
    // Arrays.
    {QLatin1String("Snapping.Behavior"), QLatin1String("Triggers"), Shape::Array},
    {QLatin1String("Snapping.Behavior.ZoneSpan"), QLatin1String("Triggers"), Shape::Array},
    {QLatin1String("Snapping.Behavior.SnapAssist"), QLatin1String("Triggers"), Shape::Array},
    {QLatin1String("Tiling.Behavior"), QLatin1String("Triggers"), Shape::Array},
    {QLatin1String("Scrolling.Behavior"), QLatin1String("Triggers"), Shape::Array},
    {QLatin1String("Scrolling.Wheel.Focus"), QLatin1String("Triggers"), Shape::Array},
    {QLatin1String("Scrolling.Wheel.View"), QLatin1String("Triggers"), Shape::Array},
    {QLatin1String("Workspaces.Named"), QLatin1String("Entries"), Shape::Array},
};

/// The native value @p encoded spells, when it is JSON of @p shape; an
/// undefined value otherwise.
QJsonValue nativeValueFor(const QString& encoded, Shape shape)
{
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(encoded.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError) {
        return QJsonValue(QJsonValue::Undefined);
    }
    if (shape == Shape::Object) {
        return doc.isObject() ? QJsonValue(doc.object()) : QJsonValue(QJsonValue::Undefined);
    }
    return doc.isArray() ? QJsonValue(doc.array()) : QJsonValue(QJsonValue::Undefined);
}

} // namespace

void ConfigMigration::migrateV8ToV9(QJsonObject& root)
{
    // Defense-in-depth idempotency guard, mirroring the earlier steps.
    if (root.value(ConfigKeys::versionKey()).toInt(0) >= 9) {
        return;
    }

    for (const StructuredKey& entry : kV8StructuredKeys) {
        const QString groupPath(entry.group);
        QJsonObject group = groupObjectAtPath(root, groupPath);
        const QJsonValue stored = group.value(entry.key);
        if (!stored.isString()) {
            continue;
        }
        const QJsonValue native = nativeValueFor(stored.toString(), entry.shape);
        if (native.isUndefined()) {
            continue;
        }
        group.insert(entry.key, native);
        setGroupAtSegments(root, groupPath.split(QLatin1Char('.')), group);
    }

    // Stamp the literal, not ConfigSchemaVersion — the historical step's
    // output must stay frozen when the chain grows again.
    root[ConfigKeys::versionKey()] = 9;
}

} // namespace PhosphorEngineRuntime
