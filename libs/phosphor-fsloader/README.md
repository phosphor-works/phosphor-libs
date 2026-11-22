<!-- SPDX-FileCopyrightText: 2026 fuddlesworth
     SPDX-License-Identifier: LGPL-2.1-or-later -->

# phosphor-fsloader

> Generic filesystem-backed registry skeleton: directory walking, file
> watching, debounced rescans, user-wins-over-system layering, and a
> pluggable scan strategy. The single owner of "live-reload a directory
> tree of user-editable JSON" across the suite.

## Responsibility

Walk a set of directories, parse each file (flat `*.json`, or a
metadata-pack subdirectory) into a typed record, watch the directories
for live edits, debounce rescans, apply user files that shadow bundled
ones with the same ID, and emit one bulk-update signal per scan.

The library factors the work into:

- **`WatchedDirectorySet`** is the base mechanism. It owns the
  `QFileSystemWatcher`, debounces rescan triggers (50 ms), promotes
  watches to a parent directory when the target doesn't exist yet,
  guards against rescan-during-rescan races, re-arms individual
  file watches after a scan, and re-arms its watches when a directory on
  the way to a registered one is replaced or retargeted.
- **`IScanStrategy`** is the pluggable policy for how to enumerate the
  registered directories, parse each entry, and commit results to the
  consumer's registry. The base owns *when* to scan, and the strategy owns
  *what scanning means*.
- **`DirectoryLoader`** is the flat `*.json` specialisation that pairs with
  an `IDirectoryLoaderSink` (the schema-specific parse + commit
  strategy). Used by curve and profile loaders.
- **`MetadataPackScanStrategy<Payload>`** is the templated specialisation for
  "subdirectory-with-metadata-json" packs. There is one pack per top-level
  subfolder, validated by a `metadata.json`, with the payload type chosen by the
  registry. Hosted by `PhosphorRegistry::MetadataPackLoader<T>`, which owns the
  strategy and the watcher and adds the per-id fingerprint diff. Used by
  [`phosphor-shaders`](../phosphor-shaders/README.md)' `ShaderRegistry`,
  [`phosphor-animation`](../phosphor-animation/README.md)'s
  `AnimationShaderRegistry`, and `PhosphorSurfaceShaders::SurfaceShaderRegistry`.
- **`validateJsonEnvelope`** is the shared envelope validation. It parses the
  file, checks the `"name"` field is non-empty and matches the
  filename, and returns a `JsonEnvelope` carrying the rest of the JSON
  object for the sink's schema-specific `fromJson`.

A metadata-pack registry does not inherit anything from this library: it
COMPOSES a `PhosphorRegistry::MetadataPackLoader<T>`, which owns the
`WatchedDirectorySet` plus the strategy and provides the search-path management
surface (`addSearchPath`, `setUserPath`, `refresh`) every consumer was otherwise
hand-rolling.

## Key types

| Type | Purpose |
|------|---------|
| `PhosphorFsLoader::WatchedDirectorySet`           | Base mechanism: watcher, debounce, parent-watch, race guard |
| `PhosphorFsLoader::WatchedFileSet`                | Watches an explicit list of files, some possibly missing, and reports only the ones whose fingerprint (inode, size, mtime ns) changed |
| `PhosphorFsLoader::IScanStrategy`                 | Pluggable enumerate / parse / commit policy |
| `PhosphorFsLoader::DirectoryLoader`               | Flat `*.json` specialisation (sink-driven) |
| `PhosphorFsLoader::IDirectoryLoaderSink`          | Per-schema strategy: `parseFile()` + `commitBatch()` |
| `PhosphorFsLoader::ParsedEntry`                   | Parse-result value type with source-path metadata and `std::any` payload |
| `PhosphorFsLoader::MetadataPackScanStrategy<P>`   | Subdirectory-with-`metadata.json` strategy |
| `PhosphorFsLoader::validateJsonEnvelope`          | Shared `"name"`-field envelope validator returning a `JsonEnvelope` |
| `PhosphorFsLoader::resolveWithinDirectory`        | Containment guard: resolves a declared path against a directory and refuses anything landing outside it |
| `PhosphorFsLoader::readFileWithinDirectory`       | Bounded read of a declared path that validates the OPENED descriptor: regular file, size cap, real path inside the directory, so a symlink or FIFO swapped in after the check is refused |
| `PhosphorFsLoader::AbsolutePathPolicy`            | Whether an already-absolute declared path is refused (pack-declared) or trusted (user-chosen at runtime) |
| `PhosphorFsLoader::SchemaValidator`               | JSON-Schema gate applied to a pack's `metadata.json` before it is parsed |

## Typical use

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

A metadata-pack registry (used by shader / animation-shader registries):

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

Watching a config file and the theme file it names, either of which may not
exist yet:

```cpp
#include <PhosphorFsLoader/WatchedFileSet.h>

auto* files = new PhosphorFsLoader::WatchedFileSet(this);
connect(files, &PhosphorFsLoader::WatchedFileSet::filesChanged, this,
        [this](const QStringList& changed) { reload(changed); });
files->setPaths({configPath, themePath});   // reconciles; call again when the theme changes
```

Reading an asset a pack declares, without letting it escape the pack:

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

## Design notes

- **Watcher is opt-in.** `LiveReload::On` installs a
  `QFileSystemWatcher` on every scanned directory (or its parent, if
  the target doesn't exist yet, so fresh installs that create the
  user-data dir later still pick up edits without a restart). The enable is
  a set-wide ONE-WAY latch: a later `LiveReload::Off` call never disarms an
  already-armed set. A test that must not watch should never pass `On`, or
  should destroy and rebuild the set.
- **Path containment is one shared function.** Every pack parser that resolves a
  declared path routes through `resolveWithinDirectory`. There is exactly one
  correct way to write that check: `QDir::filePath` returns an absolute argument
  unchanged and never normalises `..`, a lexical compare cannot see a symlinked
  component, and canonicalisation returns empty for a leaf that does not exist
  yet, which is the ordinary live-reload case. The guard canonicalises the
  deepest EXISTING ancestor and re-appends the missing tail, so the two domains
  are never mixed, and it fails closed when nothing on the chain exists.
- **Single files report by fingerprint.** `WatchedFileSet` watches each file
  and every directory on the way to it, from `/` down, so creation, deletion
  and rename-replacement are seen wherever they happen. A directory event
  alone proves nothing, so after the debounce each path is `stat()`ed and
  only those whose identity or content stamp moved are reported. An
  unrelated write beside a watched file or in one of its ancestors emits
  nothing.
- **Watches follow identity, not path strings.** An inotify watch stays on
  the inode its path named when it was added, and Qt's directory watch does
  not report its own rename. So a symlinked directory that is retargeted
  (Home Manager, stow, chezmoi) or a parent replaced by rename would leave
  every watch below it on the old tree. Both sets record an identity chain
  for each watched path: the device and inode of every directory walked from
  `/` and the text of every symlink followed, resolved component by
  component. An event in an ancestor costs a walk of the chains, a few
  `lstat()` calls. Only a chain that moved re-arms its watches, and in
  `WatchedDirectorySet` only a chain that moved runs a rescan.
  `WatchedDirectorySet` chains its registered directories, and also each
  path its strategy reports that goes through a symlink below its
  registered directory: a linked file, a linked folder, a file inside one.
  A watch under the link's own spelling would stay on the old target when
  the link is pointed elsewhere, and see nothing while it dangles, so such a
  path is watched through its chain, its target under its physical path.
  Reported paths that go through no link keep their plain watch, because
  the chain of their registered directory already covers everything above
  them.
- **Containment of what was opened, not what was named.**
  `readFileWithinDirectory` runs `resolveWithinDirectory` first, then opens
  with `O_NONBLOCK`, `fstat()`s the descriptor and checks its
  `/proc/self/fd` real path against the directory. A path checked before
  opening can be swapped between check and open; the descriptor cannot.
- **`commitBatch` is the one mutation point.** The sink only touches
  its target registry inside `commitBatch`, so bulk signals (e.g. a
  QML `reloadAll`) coalesce to one emit per scan.
- **User wins on collision.** When a system file and a user file share
  an ID, the user file commits. Search-path order is consumer-chosen, and
  the loader honours it.
- **Type-erased payloads.** `ParsedEntry::payload` is `std::any` so the
  loader stays schema-agnostic. The sink produces it, the sink
  consumes it, nobody in between peeks. The metadata-pack variant adds
  a typed `Payload` template parameter for registries that don't need
  the erasure.

## Dependencies

- `QtCore`
- jsoncons 1.10.0 (BSL-1.0), header-only. By default it comes from the
  archive that `cmake/PhosphorDependencies.cmake` pins, is compiled into the
  library (whose symbols are hidden but for its own exports, so none of
  jsoncons' leaves it), and its licence installs as
  `licenses/phosphor-libs/LICENSE.jsoncons`, so the notice ships with any
  distribution. That file also holds the MIT notices of the two jsoncons
  headers compiled in that carry their own, grisu3 and a5hash. With `-DPHOSPHORFSLOADER_USE_SYSTEM_JSONCONS=ON` the system's
  headers are used instead, found through `JSONCONS_INCLUDE_DIR`, and the
  configure checks that they compile against the JSON Schema API the library
  uses, which needs jsoncons 1.7.0 or later. The check runs again when
  `JSONCONS_INCLUDE_DIR` changes and after it fails.

## Schema validation

`SchemaValidator` checks a document against a JSON Schema 2020-12 schema.
`SchemaValidator.h` is the full contract. In short:

- A schema is read as Qt reads JSON (no comments, the last of two members
  with one name kept) and must be 2020-12, with `$schema` absent or
  `https://json-schema.org/draft/2020-12/schema` (a trailing `#` allowed)
  and no keyword of another draft (`definitions`, `dependencies`,
  `additionalItems`, `$recursiveRef`, `$recursiveAnchor`, an array under
  `items`). It is checked against the 2020-12 meta-schema when it is
  constructed, so `{"enum": "x"}` or a negative `maxItems` is refused there,
  with the meta-schema's most specific reason, inside up to four maps
  (`$defs`, `properties`, `patternProperties`, `dependentSchemas`) one inside
  another. For a member of a fifth, the reason is only that it was invalid.
  A refused schema refuses every document, with the reason.
- References stay within the schema. A `$ref` is `#`, `#/<JSON pointer>` or
  `#<anchor>`, not percent-encoded, and names one of its subschemas. An
  `$anchor` is defined once, `$id` appears at the root only, and
  `$dynamicRef` and `$dynamicAnchor` are refused. A `$ref` that jsoncons
  cannot resolve is refused too. Such is one into `contentSchema`, which
  jsoncons does not compile. Such is also one by pointer below `items`
  beside `prefixItems`, which jsoncons files as `additionalItems`, or below
  a member of `patternProperties` or `dependentSchemas`, which it files
  without the member's name. An `$anchor` below those resolves. A member of
  `properties` or `$defs` whose name jsoncons writes into its location as a
  `%7E` or `%7e` followed by neither `0` nor `1` once decoded (`a%7E2`, or a
  U+0007 then an `E` or `e` at the end, which it encodes so) is refused,
  since jsoncons then reads its location as an anchor's name and compiles it
  apart from the schema. A literal `~` is kept, since jsoncons writes it as
  `~0`.
- A `$ref` that leads back to its own schema without descending into the
  document is refused, even in a `$defs` entry nothing uses, since jsoncons
  would recurse on it until the stack overflowed. That is a `$ref` back
  through `allOf`, `anyOf`, `oneOf`, `not`, `if`, `dependentSchemas`, other
  `$ref`s, and `then` and `else` beside an `if` (2020-12 ignores them
  without one), as `{"$ref": "#"}` is. A `$ref` back through `properties`,
  `items` or another keyword that applies to a member or an item is
  allowed.
- `format` is asserted for date-time, date, time, email, ipv4, ipv6, regex,
  json-pointer, uri and uri-reference, as jsoncons reads each. It takes the
  year 0000 and the ipv4 address 256.1.1.1, and refuses an empty
  uri-reference and the uri `a:`. A schema naming another format is refused,
  since jsoncons would ignore it, and so is `hostname`, whose jsoncons check
  refuses the digit 9 and reads past the end of an empty string.
- A string or member name holding U+0000 or an unpaired UTF-16 surrogate,
  and a number outside int64 (below -2^63, or 2^63 and above), are refused
  at their path before jsoncons sees them, wherever they sit in the
  document, also where the schema accepts any value, since jsoncons' integer
  check is undefined behaviour on such a number and may run at any node.
  Python's 2020-12, which CI runs, accepts such numbers, so a data file
  holding one passes CI and is refused at load.
- Integers within int64 are exact, and so are `minimum`, `maximum` and
  their exclusive forms between them. `multipleOf` works in doubles with a
  tolerance of one ulp, so 0.07 is a multiple of 0.01 here, not in Python. Its
  message prints the divisor with six decimals (`0.075 is not a multiple of
  0.010000`).
- `pattern` is an ECMAScript `std::regex` matched against UTF-8 bytes, so a
  dot, a class or a quantifier applies to one byte of a character past
  U+007F (`^.$` does not match "é").
- `validate()` returns at most 100 errors, then one entry with an empty path
  that counts the rest. A path or message longer than 500 UTF-16 code units,
  escapes included, keeps the first 500 (a few fewer rather than split a
  surrogate pair or an escape) and gains an ellipsis.
- The reasons an `anyOf`, `oneOf` or `allOf` failed follow its error,
  prefixed with the branch (`anyOf/1: ...`). So do the reasons a member
  failed an `additionalProperties` subschema, which jsoncons does not give.
  The member is checked against that subschema again, down to four such
  subschemas one inside another, and each reason is prefixed
  `additionalProperties: `. Each subschema is compiled for that as a copy of
  the whole schema, at most 100 times in one `validate()`. The validator
  keeps the first 16 it compiles for later calls, from any thread.
  `validate()`'s documentation gives the cost.
- A failed `dependentSchemas` subschema is reported below the member that
  triggered it (`/a/c` for member `c`, when `a` triggered it), as jsoncons
  reports it. A member that fails an `additionalProperties` subschema there
  keeps jsoncons' line only, since that location does not name the object
  validated. So does one that fails a subschema that jsoncons files under
  the same location as another, since it keeps only the first of them.
  Two members of `patternProperties` holding `additionalProperties` share
  one, and so do two names it percent-encodes alike (`a{b` and `a%7Bb`).
- In a path or a message, each C0 and C1 control character, DEL, U+2028,
  U+2029 and each bidirectional control (U+202A to U+202E, U+2066 to U+2069)
  is written as `\uXXXX`, so a member name or a quoted string from a data
  file cannot start a log line, send a terminal escape sequence or reorder
  the text around it. `logSchemaErrors()` logs each error as
  `  <path>: <message>`, unquoted.

## See also

- [`phosphor-animation`](../phosphor-animation/README.md) — `ProfileLoader`, `CurveLoader`, and `AnimationShaderRegistry` are clients.
- [`phosphor-shaders`](../phosphor-shaders/README.md) — `ShaderRegistry` composes a `MetadataPackLoader` over this library's strategy.
