<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team -->
<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->

# phosphor-fsloader

phosphor-fsloader is the generic base for registries that load their entries from directories of files. It walks
directories, watches files, runs debounced rescans and lets user files replace system files, with a scan strategy that
the consumer selects. It is the single owner of the live reload of a directory tree of user-editable JSON in the
suite.

## Responsibility

The library walks a set of directories and parses each file (flat `*.json`, or a metadata-pack subdirectory) into a
typed record. It watches the directories for live edits and debounces the rescans. A user file replaces a bundled file
with the same ID. The library emits one bulk-update signal for each scan.

The library divides the work into these parts:

- **`WatchedDirectorySet`** is the base mechanism. It owns the `QFileSystemWatcher` and debounces rescan triggers
  (50 ms). If a target directory does not exist yet, it watches the parent directory instead. It prevents a rescan
  from starting during a rescan. It arms individual file watches again after a scan. It also arms its watches again
  when a directory on the way to a registered directory is replaced or retargeted.
- **`IScanStrategy`** is the policy that the consumer selects. It enumerates the registered directories, parses each
  entry, and commits the results to the consumer's registry. The base decides *when* to scan, and the strategy decides
  *what scanning means*.
- **`DirectoryLoader`** is the specialization for flat `*.json` files. It works with an `IDirectoryLoaderSink`, which
  is the parse and commit strategy for one schema. The curve and profile loaders use it.
- **`MetadataPackScanStrategy<Payload>`** is the template specialization for packs that are subdirectories with a
  metadata JSON file. There is one pack in each top-level subfolder, validated by a `metadata.json`. The registry
  selects the payload type. `PhosphorRegistry::MetadataPackLoader<T>` hosts it: it owns the strategy and the watcher,
  and adds the fingerprint diff for each id. These registries use it:
  [`phosphor-shaders`](../phosphor-shaders/README.md)' `ShaderRegistry`,
  [`phosphor-animation`](../phosphor-animation/README.md)'s `AnimationShaderRegistry`, and
  `PhosphorSurfaceShaders::SurfaceShaderRegistry`.
- **`validateJsonEnvelope`** is the shared envelope validation. It parses the file and checks that the `"name"` field
  is not empty and matches the filename. It returns a `JsonEnvelope` with the rest of the JSON object for the sink's
  schema-specific `fromJson`.

A metadata-pack registry inherits nothing from this library. It COMPOSES a `PhosphorRegistry::MetadataPackLoader<T>`.
That loader owns the `WatchedDirectorySet` and the strategy, and gives the functions that manage search paths
(`addSearchPath`, `setUserPath`, `refresh`). Without it, each consumer must write these functions itself.

## Key types

| Type | Purpose |
|------|---------|
| `PhosphorFsLoader::WatchedDirectorySet`           | Base mechanism: watcher, debounce, parent watch, guard against a rescan during a rescan |
| `PhosphorFsLoader::WatchedFileSet`                | Watches an explicit list of files, some possibly missing, and reports only the files whose fingerprint (inode, size, mtime ns) changed |
| `PhosphorFsLoader::IScanStrategy`                 | The policy to enumerate, parse and commit |
| `PhosphorFsLoader::DirectoryLoader`               | Specialization for flat `*.json` files, driven by a sink |
| `PhosphorFsLoader::IDirectoryLoaderSink`          | Strategy for one schema: `parseFile()` and `commitBatch()` |
| `PhosphorFsLoader::ParsedEntry`                   | Value type for a parse result, with source-path metadata and a `std::any` payload |
| `PhosphorFsLoader::MetadataPackScanStrategy<P>`   | Strategy for subdirectories with a `metadata.json` |
| `PhosphorFsLoader::validateJsonEnvelope`          | Shared validator of the `"name"`-field envelope. Returns a `JsonEnvelope` |
| `PhosphorFsLoader::resolveWithinDirectory`        | Containment guard: resolves a declared path against a directory, and refuses each result outside it |
| `PhosphorFsLoader::readFileWithinDirectory`       | Bounded read of a declared path that validates the OPENED descriptor: regular file, size limit, real path inside the directory. Thus it refuses a symlink or FIFO put in its place after the check |
| `PhosphorFsLoader::AbsolutePathPolicy`            | Tells whether a declared path that is already absolute is refused (declared by a pack) or trusted (selected by the user at runtime) |
| `PhosphorFsLoader::SchemaValidator`               | JSON Schema check of a pack's `metadata.json` before the parse |

## Use

A profile loader sink (flat `*.json` mode):

```cpp
#include <PhosphorFsLoader/DirectoryLoader.h>
#include <PhosphorFsLoader/IDirectoryLoaderSink.h>

using namespace PhosphorFsLoader;

class CurveLoaderSink : public IDirectoryLoaderSink {
    std::optional<ParsedEntry> parseFile(const QString& filePath) override {
        // parse one curve file; nothing registry-side here.
    }
    void commitBatch(const QStringList& removedKeys,
                     const QList<ParsedEntry>& currentEntries) override {
        // single mutation point; emit one reloadAll() signal here.
    }
};

CurveLoaderSink sink{...};
DirectoryLoader loader(sink);  // sink is borrowed for the loader's lifetime
loader.loadFromDirectories({systemDir, userDir}, LiveReload::On);
// No requestRescan() needed: loadFromDirectories already ran a full
// synchronous scan and dispatched commitBatch + entriesChanged.
```

A metadata-pack registry (as the shader and animation-shader registries use):

```cpp
// `MyPack` must derive PhosphorRegistry::IFactoryBase — the loader
// static_asserts on it. The registry is OWNED by the consumer and BORROWED by
// the loader, so declare it first.
class MyPackRegistry : public QObject {
    Q_OBJECT
public:
    MyPackRegistry()
        : m_loader(std::make_unique<PhosphorRegistry::MetadataPackLoader<MyPack>>(
              &m_registry,
              [](const QString& subdir, const QJsonObject& root, bool isUser)
                  -> std::shared_ptr<MyPack> {
                  return parseMyPack(subdir, root, isUser);
              },
              myLogCategory()))
    {
        m_loader->setOnCommitted([this] { Q_EMIT packsChanged(); });
    }

    void addSearchPath(const QString& dir) { m_loader->addSearchPath(dir); }
    void setUserPath(const QString& dir) { m_loader->setUserPath(dir); }
    void refresh() { m_loader->refresh(); }
    // … expose payload-typed lookups over m_registry

Q_SIGNALS:
    void packsChanged();

private:
    PhosphorRegistry::Registry<MyPack> m_registry;
    std::unique_ptr<PhosphorRegistry::MetadataPackLoader<MyPack>> m_loader;
};
// Composition root then wires:
registry.addSearchPath(systemDir);
registry.setUserPath(userDir);
registry.refresh();
```

Watch a config file and the theme file that it names. Each file can be missing:

```cpp
#include <PhosphorFsLoader/WatchedFileSet.h>

auto* files = new PhosphorFsLoader::WatchedFileSet(this);
connect(files, &PhosphorFsLoader::WatchedFileSet::filesChanged, this,
        [this](const QStringList& changed) { reload(changed); });
files->setPaths({configPath, themePath});   // reconciles; call again when the theme changes
```

Read an asset that a pack declares, and do not let the path go outside the pack:

```cpp
#include <PhosphorFsLoader/ContainedFileReader.h>

QString error;
const auto mesh = PhosphorFsLoader::readFileWithinDirectory(
    declared, packDir, PhosphorFsLoader::AbsolutePathPolicy::Reject, 32 * 1024 * 1024, &error);
if (!mesh) {
    return error;
}
watch(mesh->path);
parse(mesh->bytes);
```

## Schema validation

`SchemaValidator` checks a document against a JSON Schema 2020-12 schema. `SchemaValidator.h` is the full contract.
The sections below give a summary.

### Schemas

- The validator reads a schema as Qt reads JSON: no comments, and of two members with one name, it keeps the last.
- The schema must be 2020-12. `$schema` is absent or `https://json-schema.org/draft/2020-12/schema` (a trailing `#`
  is allowed). It has no keyword of a different draft (`definitions`, `dependencies`, `additionalItems`,
  `$recursiveRef`, `$recursiveAnchor`, an array under `items`).
- The constructor checks the schema against the 2020-12 meta-schema. Thus it refuses `{"enum": "x"}` or a negative
  `maxItems`, with the most specific reason of the meta-schema. This works inside a maximum of four maps
  (`$defs`, `properties`, `patternProperties`, `dependentSchemas`), one inside the other. For a member of a fifth map,
  the reason is only that the member was invalid.
- A refused schema refuses every document, with the reason.

### References

- References stay within the schema. A `$ref` is `#`, `#/<JSON pointer>` or `#<anchor>`, not percent-encoded, and
  names one of the schema's subschemas.
- An `$anchor` is defined one time, `$id` is only at the root, and `$dynamicRef` and `$dynamicAnchor` are refused.
- A `$ref` that jsoncons cannot resolve is refused too. These are such references:
  - A reference into `contentSchema`, which jsoncons does not compile.
  - A reference by pointer below `items` next to `prefixItems`, which jsoncons files as `additionalItems`.
  - A reference by pointer below a member of `patternProperties` or `dependentSchemas`, which jsoncons files without
    the member's name. An `$anchor` below these resolves.
- Some member names of `properties` or `$defs` are refused. jsoncons writes such a name into its location as a `%7E`
  or `%7e` that is followed by neither `0` nor `1` after decoding (`a%7E2`, or a U+0007 then an `E` or `e` at the end,
  which it encodes so). jsoncons then reads the location as the name of an anchor, and compiles it apart from the
  schema. A literal `~` is kept, because jsoncons writes it as `~0`.
- A `$ref` that leads back to its own schema without a descent into the document is refused. This is also true in a
  `$defs` entry that nothing uses, because jsoncons would recurse on it until the stack overflowed. Such a `$ref`
  goes back through `allOf`, `anyOf`, `oneOf`, `not`, `if`, `dependentSchemas`, other `$ref`s, or `then` and `else`
  next to an `if` (2020-12 ignores them without one). `{"$ref": "#"}` is an example. A `$ref` back through
  `properties`, `items` or another keyword that applies to a member or an item is allowed.

### Values

- `format` is asserted for date-time, date, time, email, ipv4, ipv6, regex, json-pointer, uri and uri-reference, as
  jsoncons reads each one. It accepts the year 0000 and the ipv4 address 256.1.1.1. It refuses an empty
  uri-reference and the uri `a:`. A schema that names a different format is refused, because jsoncons would ignore
  it. `hostname` is also refused, because its jsoncons check refuses the digit 9 and reads past the end of an empty
  string.
- These values are refused at their path before jsoncons sees them: a string or member name that holds U+0000 or an
  unpaired UTF-16 surrogate, and a number outside int64 (below -2^63, or 2^63 and above). This applies wherever they
  are in the document, also where the schema accepts all values. The reason is that the integer check of jsoncons is
  undefined behavior on such a number, and can run at any node. The Python 2020-12 validator that CI runs accepts such
  numbers. Thus a data file that holds one passes CI and is refused at load.
- Integers within int64 are exact. `minimum`, `maximum` and their exclusive forms are also exact between them.
- `multipleOf` works in doubles with a tolerance of one ulp. Thus 0.07 is a multiple of 0.01 here, but not in Python.
  Its message prints the divisor with six decimals (`0.075 is not a multiple of
  0.010000`).
- `pattern` is an ECMAScript `std::regex` matched against UTF-8 bytes. Thus a dot, a class or a quantifier applies to
  one byte of a character past U+007F (`^.$` does not match "é").

### Errors

| Limit | Value | At the limit |
| --- | --- | --- |
| Errors returned by `validate()` | 100 | One more entry, with an empty path, counts the rest. |
| Length of a path or a message, escapes included | 500 UTF-16 code units | The text keeps the first 500 (a few fewer, so that it does not split a surrogate pair or an escape) and gets an ellipsis. |
| Subschemas compiled as a copy in one `validate()` | 100 | Each recheck follows a kept error. After 100 errors are kept, no new recheck starts. |
| Compiled copies kept for later calls | 16 | The validator keeps the first 16 that it compiles, for calls from any thread. Each `validate()` that needs a different copy compiles it again. |

- The reasons that an `anyOf`, `oneOf` or `allOf` failed follow its error, with the branch as a prefix
  (`anyOf/1: ...`).
- The reasons that a member failed an `additionalProperties` subschema also follow, although jsoncons does not give
  them. The validator checks the member against that subschema again, down to four such subschemas one inside the
  other. Each reason gets the prefix `additionalProperties: `. For this, the validator compiles each subschema as a
  copy of the whole schema. The documentation of `validate()` gives the cost.
- A failed `dependentSchemas` subschema is reported below the member that triggered it (`/a/c` for member `c`, when
  `a` triggered it), as jsoncons reports it. There, a member that fails an `additionalProperties` subschema keeps only
  the line of jsoncons, because that location does not name the validated object. The same applies to a member that
  fails a subschema that jsoncons files under the same location as a different subschema, because jsoncons keeps only
  the first of them. Two members of `patternProperties` that hold `additionalProperties` share one subschema. Two
  names that jsoncons percent-encodes in the same way (`a{b` and `a%7Bb`) also share one.
- In a path or a message, the validator writes these characters as `\uXXXX`: each C0 and C1 control character, DEL,
  U+2028, U+2029 and each bidirectional control (U+202A to U+202E, U+2066 to U+2069). Thus a member name or a quoted
  string from a data file cannot start a log line, send a terminal escape sequence or change the order of the text
  around it.
- `logSchemaErrors()` logs each error as `  <path>: <message>`, without quotes.

## Design notes

- **The watcher is opt-in.** `LiveReload::On` puts a `QFileSystemWatcher` on every scanned directory. If the target
  does not exist yet, it watches the parent. Thus a fresh install that creates the user-data directory later still
  sees edits without a restart. The enable is a ONE-WAY latch for the whole set: a later `LiveReload::Off` call never
  disarms a set that is already armed. A test that must not watch must never pass `On`, or must destroy and build the
  set again.
- **Path containment is one shared function.** Every pack parser that resolves a declared path calls
  `resolveWithinDirectory`. There is exactly one correct way to write that check, for these reasons:
  - `QDir::filePath` returns an absolute argument unchanged and never normalizes `..`.
  - A lexical compare cannot see a symlinked component.
  - Canonicalization returns empty for a leaf that does not exist yet, which is the usual live-reload case.

  The guard canonicalizes the deepest EXISTING ancestor and appends the missing tail again. Thus the two domains are
  never mixed. If nothing on the chain exists, the guard fails closed.
- **Single files report by fingerprint.** `WatchedFileSet` watches each file and every directory on the way to it,
  from `/` down. Thus it sees creation, deletion and replacement by rename wherever they occur. A directory event
  alone proves nothing. Thus after the debounce, it runs `stat()` on each path, and reports only the paths whose
  identity or content stamp changed. An unrelated write next to a watched file, or in one of its ancestors, emits
  nothing.
- **Watches follow identity, not path strings.** An inotify watch stays on the inode that its path named when the
  watch was added. Qt's directory watch does not report its own rename. Thus a symlinked directory that is retargeted
  (Home Manager, stow, chezmoi), or a parent replaced by rename, would leave every watch below it on the old tree.
  Both sets record an identity chain for each watched path. The chain is the device and inode of every directory
  walked from `/`, and the text of every symlink followed, resolved one component at a time. An event in an ancestor
  costs a walk of the chains, which is a few `lstat()` calls. Only a chain that changed arms its watches again. In
  `WatchedDirectorySet`, only a chain that changed runs a rescan.

  `WatchedDirectorySet` makes chains for its registered directories. It also makes a chain for each path that its
  strategy reports through a symlink below its registered directory: a linked file, a linked folder, or a file inside
  one. A watch under the link's own spelling would stay on the old target when the link points to a different place,
  and would see nothing while the link dangles. Thus such a path is watched through its chain, with its target under
  its physical path. A reported path that goes through no link keeps its plain watch, because the chain of its
  registered directory already covers everything above it.
- **Containment of what was opened, not what was named.** `readFileWithinDirectory` runs `resolveWithinDirectory`
  first. Then it opens with `O_NONBLOCK`, runs `fstat()` on the descriptor, and checks the `/proc/self/fd` real path of
  the descriptor against the directory. A different file can replace a checked path between the check and the open.
  It cannot replace an open descriptor.
- **`commitBatch` is the one mutation point.** The sink changes its target registry only inside `commitBatch`. Thus
  bulk signals (for example a QML `reloadAll`) become one emit for each scan.
- **The user file wins on a collision.** When a system file and a user file have the same ID, the user file commits.
  The consumer selects the search-path order, and the loader obeys it.
- **Type-erased payloads.** `ParsedEntry::payload` is `std::any`, so the loader does not depend on a schema. The sink
  makes the payload and the sink reads it. Nothing between them looks at it. The metadata-pack variant adds a typed
  `Payload` template parameter for registries that do not need the erasure.

## Dependencies

- `QtCore`
- jsoncons 1.10.0 (BSL-1.0), header-only. By default it comes from the archive that
  `cmake/PhosphorDependencies.cmake` pins, and it is compiled into the library. The symbols of the library are hidden,
  except its own exports, so no jsoncons symbol is exported. Its license installs as
  `licenses/phosphor-libs/LICENSE.jsoncons`, so the notice ships with each distribution. That file also holds the MIT
  notices of the two compiled jsoncons headers that have their own notice, grisu3 and a5hash.
- With `-DPHOSPHORFSLOADER_USE_SYSTEM_JSONCONS=ON`, the build uses the system's headers instead, found through
  `JSONCONS_INCLUDE_DIR`. The configure step checks that they compile against the JSON Schema API that the library
  uses, which needs jsoncons 1.7.0 or later. The check runs again when `JSONCONS_INCLUDE_DIR` changes, and after it
  fails.

## See also

- [`phosphor-animation`](../phosphor-animation/README.md): `ProfileLoader`, `CurveLoader` and `AnimationShaderRegistry` are clients.
- [`phosphor-shaders`](../phosphor-shaders/README.md): `ShaderRegistry` composes a `MetadataPackLoader` over the strategy of this library.
