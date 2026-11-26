<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team -->
<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->

# phosphor-config

phosphor-config stores app settings with schema defaults, validation and versioned migrations. A schema-validated
`Store` reads and writes through a pluggable backend: the JSON-on-disk `JsonBackend` or a custom `IBackend`. A
versioned migration runner moves old files to the current schema.

## Responsibility

The library holds structured settings. A `(group, key)` pair addresses each setting. A group name is flat
(`General`) or a dot path (`Snapping.Behavior.ZoneSpan`). Values are typed (bool, int, double, color, string, map,
list), and each one has a default and validation. The library supplies these parts:

- **A `Store` front end.** It has `read<T>(group, key)`, `readVariant(group, key)` and `write(group, key, value)`
  over a pluggable `IBackend`. The library ships `JsonBackend`. Tests can supply their own in-memory `IBackend`.
- **Schema defaults and validation.** The `Schema` declares the groups. Each `KeyDef` in it has a default value, an
  optional expected type and an optional validator. The validator runs on read and on write.
- **Versioned migrations.** `MigrationRunner` runs the `MigrationStep` chain of the `Schema`
  (`v1 -> v2 -> v3 ...`) on the raw JSON root. Each bump of the schema version adds one migration step. Consumers
  never write per-key fallback reads.
- **Group-path resolution.** An optional `IGroupPathResolver` on the `Schema` changes group names into a lookup that
  the backend understands. This works for a backend that stores by path and for one that stores nested objects.

## Key types

| Type | Purpose |
|------|---------|
| `PhosphorConfig::Store`             | Front-end API: `read<T>()`, `readVariant()`, `write()`, `reset()`, `changed()` signal |
| `PhosphorConfig::IBackend`          | Abstract backend. The library ships `JsonBackend` |
| `PhosphorConfig::JsonBackend`       | JSON on disk. The consumer chooses the path (for example `$XDG_CONFIG_HOME/<app>/config.json`). `setOverlay()` puts read-only in-memory values over the file |
| `PhosphorConfig::QSettingsBackend`  | Read-only reader for a legacy KConfig-style INI file, for migration to `JsonBackend` |
| `PhosphorConfig::Schema`            | Declarative struct: groups of `KeyDef` (default, expected type, validator), version, migration chain |
| `PhosphorConfig::MigrationRunner`   | Runs the `MigrationStep` chain of the schema, one step for each schema bump |
| `PhosphorConfig::IGroupPathResolver`| Maps a group name to a backend key |
| `PhosphorConfig::HostDirectory`     | Finds the settings file that a settings module edits when more than one host can serve one API. It reads the installed descriptors and the record of the running host, and chooses between them |

## Use

```cpp
#include <PhosphorConfig/Store.h>
#include <PhosphorConfig/JsonBackend.h>
#include <PhosphorConfig/Schema.h>

using namespace PhosphorConfig;

// The backend is borrowed: the caller keeps it alive for the store's lifetime.
auto backend = std::make_unique<JsonBackend>(configPath);
Store settings(backend.get(), myAppSchema());   // consumer builds its own Schema

// Read a declared key; the schema default applies when it is unset.
bool zoneSpanEnabled =
    settings.read<bool>(QStringLiteral("Snapping.Behavior.ZoneSpan"),
                        QStringLiteral("enabled"));

// Write. The file is rewritten on the next sync() or commit(), not here.
settings.write(QStringLiteral("Snapping.Behavior.ZoneSpan"),
               QStringLiteral("enabled"), true);

// React to a change. Store::write() and Store::reset() say when one is announced.
connect(&settings, &Store::changed,
        this, [](const QString &group, const QString &key) {
            qDebug() << "changed:" << group << key;
        });
```

## Migrations

Declare migrations as `MigrationStep` entries on the `Schema`. `MigrationRunner` runs them, one step for each schema
bump:

```cpp
// Run once at startup, before opening the backend against the same path.
Schema schema = myAppSchema();
schema.version = 2;
schema.migrations.push_back(MigrationStep{
    .fromVersion = 1,
    .migrate = [](QJsonObject &root) {
        // v1 had a single "snap.enabled" flag; v2 splits into per-edge flags.
        bool was = root.take(QStringLiteral("snap.enabled")).toBool();
        QJsonObject snap;
        snap.insert(QStringLiteral("left"),  was);
        snap.insert(QStringLiteral("right"), was);
        snap.insert(QStringLiteral("top"),   was);
        snap.insert(QStringLiteral("bottom"), was);
        root.insert(QStringLiteral("snap"), snap);
        root.insert(QStringLiteral("_version"), 2);  // each step bumps the version
    },
});

MigrationRunner(schema).runOnFile(configPath);
```

## Overlays

Put command-line overrides in an overlay. Do not rewrite the file for them. The overlay has the shape of the
document. It wins over the file for every read, and nested objects merge key by key. It stays after
`reparseConfiguration()`, and the backend never writes it back:

```cpp
// afterglow -o Audio.enabled=false
backend->setOverlay(QJsonObject{
    {QStringLiteral("Audio"), QJsonObject{{QStringLiteral("enabled"), false}}},
});
settings.read<bool>(QStringLiteral("Audio"), QStringLiteral("enabled")); // false
```

The writes, resets and imports of `Store` compare against the file (`IBackend::persistedGroup()`). Thus a write of a
value that is equal to the overlay value still goes into the file.

## Unreadable files

The backend never overwrites a file that exists but does not parse, or whose root is not an object. In that case:

- `loadFailed()` turns on.
- The backend reads as the last document that it read or wrote cleanly. On a first load, that document is empty.
- `sync()`, `commit()` and `applyMigration()` refuse to write over the file of the user.
- The overlay stays on top of the fallback document.

A flush also compares the size and mtime of the file with the last read or write of the backend. Thus the backend
also refuses a file that a person broke by hand after the backend read it.

A later `reparseConfiguration()` or `reload()` that reads the file cleanly clears the flag. `reload()` does the same
reload for a caller that reports problems itself. It logs nothing and returns the reason that the file was not
readable.

A file that holds only white space reads as an empty document, the same as a missing file. `blankFile()` tells such
a file from a file that holds `{}`. This matters because an editor that saves in place empties the file before it
writes.

A caller that reloads immediately constructs the backend with `JsonBackend::Load::Deferred`. The backend then reads
and reports the file one time. Until that first reload, the backend is empty and refuses writes. For a
machine-written file with no data worth keeping, call `setOverwriteUnreadable(true)` to turn this protection off.

## Hosts of a settings API

More than one host can serve some settings, and each host keeps them in its own file. For example, the placement
settings belong to PlasmaZones on KWin. A settings module edits the correct file whether the host runs or not:

- The package of each host installs a descriptor. This is a `*.json` file in
  `<datadir>/phosphor-works/<api>-hosts/`. It holds the id, name, bus name, backend, config path and the desktops
  that the host serves. The config path starts with `$XDG_CONFIG_HOME`, `$XDG_DATA_HOME`, `$XDG_STATE_HOME` or
  `$HOME`. `HostDirectory::descriptors()` reads the descriptors. A per-user descriptor overrides a system one with
  the same id. The key of a descriptor is the `Id` in the file. The library does not compare the file name with it.
- A host calls `writeRecord(id, configPath, pid)` when it starts. The record is at
  `$XDG_STATE_HOME/phosphor-works/<api>-host.json`. The record stays after the host stops.
- `HostDirectory::resolve()` chooses a host. It uses the first rule in the table that applies.
- A record's host serves the session when one of the desktops of the session is in the descriptor's list. Case does
  not matter. A descriptor without `Desktops` passes that test for a record. But the desktop match never chooses or
  offers such a descriptor, so only a record reaches that host.
- When a record chose the host, the file is the record's path as the host wrote it. That path is stale if the host
  moved its file after that. In all other cases, the file is the resolved path of the descriptor.
- `running` says only that some process owns the bus name. That process is known to be the chosen host only under
  the running host's own record.

| Order | Rule |
|-------|------|
| 1 | The running host's own record. The process that wrote it owns the bus name now. This rule wins on every desktop. |
| 2 | A choice of the user. This is a record with pid 0 whose host serves this session. It applies whether a process owns the name or not. |
| 3 | A host's record. This is a record with any other pid, when no process owns the name and its host serves this session. |
| 4 | The one descriptor that matches `XDG_CURRENT_DESKTOP`. If more than one matches, `resolve()` lists them. The module can then ask the user and record the answer with pid 0. If none matches, there is no host and nothing to offer. |

```cpp
const PhosphorConfig::HostDirectory hosts(QStringLiteral("placement"));
const QList<PhosphorConfig::HostDescriptor> descriptors = hosts.descriptors();
// resolve() takes the pid of one owner, so the hosts of one API are expected
// to share a bus name and any descriptor's will do. The pid stays 0 when
// nothing owns the name, when no host is installed, or when there is no
// session bus to ask.
qint64 owner = 0;
QDBusConnectionInterface* bus = QDBusConnection::sessionBus().interface();
if (bus && !descriptors.isEmpty()) {
    owner = bus->servicePid(descriptors.constFirst().busName).value();
}
const auto chosen = PhosphorConfig::HostDirectory::resolve(descriptors, hosts.record(), owner,
                                                           PhosphorConfig::HostDirectory::currentDesktops());
```

## Design notes

- **No ad hoc backward compatibility.** The library enforces one migration for each schema bump and nothing else.
  There are no per-key fallback reads outside migration functions. If a key gets a new name in the same schema
  version, users get the default for the new key. Nothing rescues the old value. This keeps the code that reads the
  configuration simple.
- **Per-key validators.** Each `KeyDef` can have a validator, which runs on read and on write. It gives consumers
  one place to clamp ranges (`qBound`), normalize enum-style strings or go back to the default on incorrect input.
  Validators must be idempotent, so that `write()` can skip a save that changes nothing.
- **The schema holds the defaults.** Each `KeyDef` declares its own default value. A settings UI can read the
  schema to make widgets, and the read path can use the default without a second declaration at the call site.
- **Backends are fully mockable.** `Store` borrows an `IBackend*`. Tests construct a `Store` over their own
  in-memory `IBackend` and do not touch the disk.

## Dependencies

- `QtCore`, `QtGui`. There are no Phosphor dependencies. This is a leaf library.
