// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorShaders/ShaderPresetStore.h>

#include <PhosphorFsLoader/FileLimits.h>
#include <PhosphorFsLoader/IDirectoryLoaderSink.h>
#include <PhosphorFsLoader/ParsedEntry.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLoggingCategory>
#include <QStandardPaths>
#include <QStringList>

#include <algorithm>
#include <any>

namespace PhosphorShaders {

namespace {
Q_LOGGING_CATEGORY(lcPresetStore, "phosphorshaders.presetstore")

/// Every family, in one place, so adding one cannot leave a slot unconsidered.
/// Sized by ShaderFamilyCount and static_asserted below, so this list and the
/// enum cannot drift apart silently.
constexpr std::array<ShaderFamily, ShaderFamilyCount> kAllFamilies{
    ShaderFamily::Animation,
    ShaderFamily::Surface,
    ShaderFamily::Pointer,
    ShaderFamily::Overlay,
};
// A braced list shorter than the array default-initialises the tail, which would
// make a missing family read as Animation rather than fail to compile. Checking
// the last slot catches that: adding an enumerator anywhere moves the highest
// value and leaves this slot holding a default.
static_assert(kAllFamilies.back() == static_cast<ShaderFamily>(ShaderFamilyCount - 1),
              "kAllFamilies must list every ShaderFamily, in enum order");
} // namespace

QString standardUserPresetRoot(const QString& subdirectory)
{
    if (subdirectory.isEmpty()) {
        return QString();
    }
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QLatin1Char('/') + subdirectory;
}

QString userPresetDirectory(const QString& root, ShaderFamily family)
{
    return root + QLatin1Char('/') + shaderFamilyToken(family);
}

/// Turns one preset file into a `ShaderPreset` and commits each rescan batch to
/// the registry. The registry does its own diffing and per-pack signalling, so
/// this sink keeps no snapshot of its own — unlike `CurveLoader::Sink`, which
/// has to diff because its registry is replace-semantic and unconditionally
/// noisy.
class ShaderPresetStore::Sink : public PhosphorFsLoader::IDirectoryLoaderSink
{
public:
    Sink(ShaderPresetRegistry& reg, ShaderFamily fam)
        : registry(reg)
        , family(fam)
    {
    }

    /// A plain reference, and that is the point of the shape around it. This
    /// used to be a QPointer guarding against the registry being destroyed
    /// first, because both it and the owning loader were QObject children of the
    /// store and Qt frees children in insertion order. The store now owns the
    /// registry as a by-value member declared BEFORE these sinks, so ordinary
    /// member-destruction order makes "destroyed first" unreachable and there is
    /// nothing for a weak pointer to catch. The destructor retracts nothing, so that
    /// declaration order is the whole guarantee.
    ShaderPresetRegistry& registry;
    ShaderFamily family;

    std::optional<PhosphorFsLoader::ParsedEntry> parseFile(const QString& filePath) override
    {
        QFile file(filePath);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            qCWarning(lcPresetStore) << "Skipping preset file that will not open:" << filePath << file.errorString();
            return std::nullopt;
        }

        QJsonParseError parseError{};
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError) {
            qCWarning(lcPresetStore) << "Skipping malformed preset file:" << filePath << parseError.errorString();
            return std::nullopt;
        }
        if (!doc.isObject()) {
            qCWarning(lcPresetStore) << "Skipping preset file whose root is not an object:" << filePath;
            return std::nullopt;
        }

        // The filename stem is the fallback identity, so a hand-written file
        // with no `id` still gets a stable one an assignment can reference.
        const QString fallbackId = QFileInfo(filePath).completeBaseName();
        ShaderPreset preset = ShaderPreset::fromJson(doc.object(), fallbackId);
        preset.sourcePath = filePath;
        preset.readOnly = false;

        if (!preset.isValid()) {
            // isValid() is id + packId. A preset naming no pack cannot be
            // offered anywhere, because parameter ids mean nothing across
            // packs — so this is a refusal, not a degraded load.
            qCWarning(lcPresetStore) << "Skipping preset file with no pack id:" << filePath;
            return std::nullopt;
        }
        if (preset.name.isEmpty()) {
            // A nameless preset would render as a blank row in the picker.
            // Fall back to the id, which is at worst a UUID the user can
            // rename, rather than dropping a preset an assignment may point at.
            preset.name = preset.id;
        }

        PhosphorFsLoader::ParsedEntry parsed;
        parsed.key = preset.id;
        parsed.sourcePath = filePath;
        parsed.payload = std::move(preset);
        return parsed;
    }

    void commitBatch(const QStringList& removedKeys,
                     const QList<PhosphorFsLoader::ParsedEntry>& currentEntries) override
    {
        // `removedKeys` needs no handling of its own: the registry takes the
        // whole family's current set and replaces it, so a key that is no
        // longer in `currentEntries` is dropped by construction. Reading it
        // would be a second, redundant source of truth for the same fact.
        Q_UNUSED(removedKeys);

        QList<ShaderPreset> presets;
        presets.reserve(currentEntries.size());
        for (const auto& parsed : currentEntries) {
            const auto* preset = std::any_cast<ShaderPreset>(&parsed.payload);
            if (!preset) {
                qCWarning(lcPresetStore) << "commitBatch: payload type-mismatch for" << parsed.key;
                continue;
            }
            presets.append(*preset);
        }

        // No null check on the registry, and none is owed now: it is a reference
        // to a member that outlives every sink. The check this replaces existed
        // only because the pointer could dangle.
        registry.setUserPresets(family, presets);
    }
};

ShaderPresetStore::ShaderPresetStore(QObject* parent)
    : QObject(parent)
{
}

ShaderPresetStore::~ShaderPresetStore()
{
    // Tear the publishers down in an order this class chose, and retract
    // NOTHING. See the note inside the loop for why the retraction that used to
    // be here was the hazard rather than the safeguard.
    //
    // The registry needs no protecting either way: it is a member declared
    // before the slots, so it outlives every line below under ordinary
    // member-destruction order.
    for (const ShaderFamily family : kAllFamilies) {
        Publisher& publisher = m_publishers[slotOf(family)];
        if (!publisher.active()) {
            // Nothing to do, and nothing below would misbehave on an empty slot
            // either — resetting a null unique_ptr is a no-op. Skipping is for
            // the reader, not for correctness.
            continue;
        }
        // NO retraction here, and that absence is the point.
        //
        // This used to hand the registry an empty set per family, carried over
        // from the shape where the registry OUTLIVED the loaders. Its stated
        // reason — stop a sequential store inheriting presets no file backs —
        // cannot apply any more: the registry is this store's own by-value
        // member and dies with it, so the next store gets a fresh empty one and
        // has nothing to inherit through.
        //
        // Keeping it was actively harmful rather than merely redundant.
        // `setUserPresets` EMITS `presetsChanged` when the family had any
        // presets, and in the KWin effect that signal is connected with the
        // effect itself as context. Qt severs a QObject's connections in
        // ~QObject, which runs AFTER its members are destroyed — so the emission
        // landed in a handler that then touched `m_pointerPass`, a member
        // declared after the shader manager and therefore already destroyed.
        // That is a write into freed memory holding GL handles: the same fault
        // class this refactor removed from the library, re-created one layer up
        // by the one line that did not need to be here.
        //
        // Loader before sink: the loader's watcher can still call into the sink,
        // so the thing that calls must die before the thing it calls. The field
        // order in Publisher already gives this, but resetting explicitly means
        // the ordering does not silently depend on it.
        publisher.loader.reset();
        publisher.sink.reset();
    }
}

void ShaderPresetStore::load(const QString& root, const QList<ShaderFamily>& families)
{
    // An empty root would resolve against the process working directory:
    // QDir(QString()) is QDir("."), so the loaders would scan the CWD, and
    // userPresetDirectory() would hand out filesystem-root paths like
    // "/animation". A relative root
    // is the same hazard with a mkpath that succeeds.
    if (root.isEmpty() || !QDir::isAbsolutePath(root)) {
        qCWarning(lcPresetStore) << "Refusing to load presets from a root that is not an absolute path:" << root;
        return;
    }

    // Refuse a SECOND root once anything is publishing, rather than overwriting
    // m_root. The publisher slots are left alone below when they are already
    // occupied, so a changed root would have moved what `directoryFor()` hands to
    // the bridge while the watcher stayed on the old directory: a save would land
    // in the new tree, the rescan would re-read the old one, and the preset would
    // come back invisible with `rescanNow()` still answering true. Nothing calls
    // load() twice with different roots today, which is exactly why this needs to
    // be stated rather than assumed.
    const bool anyPublishing = std::any_of(m_publishers.cbegin(), m_publishers.cend(), [](const Publisher& p) {
        return p.active();
    });
    if (anyPublishing && root != m_root) {
        qCWarning(lcPresetStore) << "Refusing to re-root an already-publishing preset store; keeping" << m_root
                                 << "and ignoring" << root;
        return;
    }

    m_root = root;

    // An empty list means every family, so an existing caller keeps the old
    // behaviour and a new one opts in to less.
    const QList<ShaderFamily> wanted =
        families.isEmpty() ? QList<ShaderFamily>(kAllFamilies.cbegin(), kAllFamilies.cend()) : families;

    for (const ShaderFamily family : wanted) {
        Publisher& publisher = m_publishers[slotOf(family)];
        if (publisher.active()) {
            // Already publishing: leave it exactly as it is. A caller naming a
            // family twice, or calling load() twice, cannot produce a second
            // publisher — the slot is the publisher, so there is nowhere for a
            // second one to go.
            continue;
        }
        publisher.sink = std::make_unique<Sink>(m_registry, family);
        publisher.loader = std::make_unique<PhosphorFsLoader::DirectoryLoader>(*publisher.sink);
        // CREATE the family directory before the watch is registered. The parent
        // promotion the watcher falls back to cannot carry a fresh install in the
        // real XDG layout: the nearest existing ancestor there is the data home
        // itself, which WatchedDirectorySet refuses as a forbidden watch root, so
        // the watch never arms and the first preset the user saves stays invisible
        // until the process restarts. Verified in a nested compositor — a preset
        // written into a freshly created tree never reached the running effect.
        //
        // Creating it is not speculative: this is the store's own directory, and it
        // is where a save writes. A failure to create is not fatal, since the loader
        // handles an absent directory and the next save creates it anyway.
        const QString dir = userPresetDirectory(root, family);
        if (!QDir().mkpath(dir)) {
            qCWarning(lcPresetStore) << "Could not create the preset directory" << dir
                                     << "— presets saved later will not be seen until the next restart";
        }
        // LiveReload::On is the point of the whole design: a preset retuned on
        // disk has to reach every process without a restart.
        publisher.loader->loadFromDirectory(dir, LiveReload::On);
    }
}

ShaderPresetRegistry& ShaderPresetStore::registry()
{
    return m_registry;
}

const ShaderPresetRegistry& ShaderPresetStore::registry() const
{
    return m_registry;
}

bool ShaderPresetStore::publishes(ShaderFamily family) const
{
    return m_publishers[slotOf(family)].active();
}

bool ShaderPresetStore::rescanNow(ShaderFamily family)
{
    Publisher& publisher = m_publishers[slotOf(family)];
    if (!publisher.active()) {
        return false;
    }
    publisher.loader->rescanNow();
    return true;
}

QString ShaderPresetStore::directoryFor(ShaderFamily family) const
{
    // EMPTY when load() never ran, or refused a non-absolute root. Without this the
    // answer was `userPresetDirectory("", family)` — a filesystem-root path like
    // "/animation" — so load()'s refusal was invisible to the one caller that turns
    // this into a write (ShaderPresetBridge::commit, through presetDirectory). It
    // failed loudly on mkpath rather than writing anywhere bad, but a refusal the
    // caller cannot see is the asymmetric half of a defensive pair.
    if (m_root.isEmpty()) {
        return QString();
    }
    return userPresetDirectory(m_root, family);
}

} // namespace PhosphorShaders
