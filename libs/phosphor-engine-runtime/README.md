# PhosphorEngineRuntime

The placement engine runtime a host embeds: the snap, tile and scroll engines' host, its services, its overlays
and its settings. PlasmaZones' daemon (`plasmazonesd`) hosts it beside KWin today, and the Phosphor compositor
will host it in process.

It arrives here area by area from PlasmaZones. This first part is the **settings model**.

## Responsibility

- The settings schema every engine setting is declared in (keys, defaults, types, coercion, choices), and the
  versioned migrations that bring an older config file up to it.
- `Settings`, the store-backed model the runtime reads and writes, with a NOTIFY per key and change
  announcements on reload, discard and reset.
- The host settings extension, through which a host adds keys of its own to the same file, names the keys that
  belong to one machine, and labels its token keys.
- The host's config directory name, which every config and data path is composed from.

## Key types

| Type | Header | What it is |
| --- | --- | --- |
| `Settings` | `Settings.h` | The store-backed settings model (`ISettings` and the engines' settings interfaces) |
| `ISettings` | `ISettings.h` | The interface the runtime and its services read settings through |
| `ConfigDefaults` | `ConfigDefaults.h` | Default values and the config group and key accessors |
| `HostSettingsExtension` | `SettingsSchema.h` | A host's own keys, machine-scoped keys and labels |
| `ConfigMigration` | `ConfigMigration.h` | The schema-version migration chain run at startup |
| `PerScreenResolver` | `PerScreenResolver.h` | Per-screen overrides resolved over the global values |
| `setAppDirectoryName` | `AppDirectoryName.h` | The host's directory under the XDG config and data roots |

## Typical use

```cpp
// At the top of main(), before anything reads a path or builds a Settings.
PhosphorEngineRuntime::setAppDirectoryName(QStringLiteral("myhost"));
PhosphorEngineRuntime::setHostSettingsExtension({&appendMyHostSchema, &isMyMachineScopedKey, &myHostLabels});

PhosphorEngineRuntime::Settings settings;
settings.setWorkspacesEnabled(true);
settings.save();
```

A host's own keys go through `Settings::hostValue` and `setHostValue`, and `hostValueChanged` announces a change
however it came about. PlasmaZones serves its KWin choices and its rendering pin that way (`HostSettings`).

## Design notes

- **No default directory name.** Reading the app directory name before a host sets it aborts, rather than handing
  out a path at the root of the user's config directory.
- **Install the extension first.** A store built without it deletes the host's keys as undeclared on its next
  save, so the host installs it before the first `Settings`.
- **Logging** is under `phosphor.engineruntime.*` (`phosphor.engineruntime.config` for the model).

## Dependencies

Qt Core and Gui, and PhosphorConfig (the store), PhosphorAnimation (motion profiles), PhosphorSurface and
PhosphorRegistry (decoration and overlay trees), PhosphorRules (per-mode disable rules), PhosphorZones,
PhosphorScreens, PhosphorEngine and the three engines' settings interfaces, PhosphorTiles, PhosphorShaders,
PhosphorCompositor, PhosphorIdentity, PhosphorLayoutApi and PhosphorProtocol, whose types the settings expose.
