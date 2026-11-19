# PhosphorEngineRuntime

The placement engine runtime a host embeds: the snap, tile and scroll engines' host, its services, its overlays
and its settings. PlasmaZones' daemon (`plasmazonesd`) hosts it beside KWin today, and the Phosphor compositor
will host it in process.

It arrives here area by area from PlasmaZones. So far: the **settings model**, the **core** the runtime is built on, **window tracking and drag**, and the **services** with the host
interfaces.

## Responsibility

- The settings schema every engine setting is declared in (keys, defaults, types, coercion, choices), and the
  versioned migrations that bring an older config file up to it.
- `Settings`, the store-backed model the runtime reads and writes, with a NOTIFY per key and change
  announcements on reload, discard and reset.
- The host settings extension, through which a host adds keys of its own to the same file, names the keys that
  belong to one machine, and labels its token keys.
- The host's config directory name, which every config and data path is composed from.
- The core: the overlay interface the runtime presents through (`IOverlayService`), the screen-mode router and the
  cross-surface and geometry resolvers, geometry and layout-list helpers, the zone-selector and strip-selector
  layouts, the animation profile bootstrap, the zone-overlay shader registry, the runtime's logging categories and
  its translation catalog (`RuntimeI18n`, context `PhosphorEngineRuntime`).
- Window tracking (`WindowTracker`): every managed window's placement, its per-mode float state, persistence
  across restarts, evacuation when an output leaves, and the rules applied as windows open. The drag resolver
  (`DragResolver`): what a drag with a trigger held does, from zone highlighting to the drop, and the drop proxy.
- The services a host binds to its transport (plasmazonesd binds them to D-Bus): layouts, settings, snapping,
  tiling, autotile, scrolling, rules, shaders, overlays, zone queries and the convenience control API. And the
  host interfaces the runtime asks the host through: `IWorkspaceHost` (per-output desktops and the compositor's
  desktop shortcuts), `ISystemOsd` (the desktop's own text OSD) and `IHostApps` (the host's settings app and
  layout editor).
- The overlays (`OverlayService`): the zone overlay, the zone selector, snap assist, the layout picker, the OSDs, the
  cheatsheet and the scroll drop indicator, as slots of one passive layer-shell surface per screen, with their QML
  (`qrc:/phosphor-engine-runtime/overlay/`). The host presents them through a layer-shell transport and an
  `ISurfacePresentationHost` (the render backend and any GPU thumbnail import). Their layer-shell namespaces are
  the runtime's own, `phosphor-<family>` (`phosphor-osd`), under every host.
- The controllers: global shortcuts (`ShortcutManager`, over the host's PhosphorShortcuts backend), dynamic
  per-monitor workspaces (`WorkspaceController`), the unified layout list and mode toggle
  (`UnifiedLayoutController`), the engines' construction (`createEngines`) and the context resolver's adapters.

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
| `IOverlayService` | `IOverlayService.h` | The overlays, zone selector, snap assist, OSDs and pickers the runtime shows |
| `ScreenModeRouter` | `ScreenModeRouter.h` | Which placement mode each screen runs, per desktop and activity |
| `AnimationBootstrap` | `AnimationBootstrap.h` | Loads the curve and motion-profile registries the overlays animate with |
| `ShaderRegistry` | `ShaderRegistry.h` | The zone-overlay shader packs |
| `RuntimeI18n` | `RuntimeI18n.h` | The runtime's translation context and `loadRuntimeTranslations` |
| `WindowTracker` | `WindowTracker.h` | Window placement tracking, float state, persistence and evacuation |
| `DragResolver` | `DragResolver.h` | A drag's activation, zone highlight and drop |
| `WindowLifecycleRelay` | `WindowLifecycleRelay.h` | The window lifecycle signals the services listen to |
| `LayoutService`, `SettingsService`, `SnapService`, ... | `<Name>Service.h` | The services a host binds to its transport |
| `IWorkspaceHost`, `ISystemOsd`, `IHostApps` | `IWorkspaceHost.h`, `ISystemOsd.h`, `IHostApps.h` | What the runtime needs from its host |
| `OverlayService` | `OverlayService.h` | The overlays, their surfaces, animation and QML |
| `ISurfacePresentationHost` | `ISurfacePresentationHost.h` | The host's render backend and GPU thumbnail import |
| `ShortcutManager` | `ShortcutManager.h` | The runtime's global shortcuts and the cheatsheet model |
| `WorkspaceController` | `WorkspaceController.h` | Dynamic per-monitor workspaces |
| `UnifiedLayoutController` | `UnifiedLayoutController.h` | The unified layout list and the mode toggle |

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
- **Logging** is under `phosphor.engineruntime.*`: `.config` for the model, `.core`, `.daemon` and its `.overlay` and
  `.shortcuts`, and `.dbus.*` for the services by the interface each serves.

## Dependencies

Qt Core, Gui, Qml and Quick, and PhosphorConfig (the store), PhosphorAnimation (motion profiles), PhosphorSurface and
PhosphorRegistry (decoration and overlay trees), PhosphorRules (per-mode disable rules), PhosphorZones,
PhosphorScreens, PhosphorEngine and the three engines' settings interfaces, PhosphorTiles, PhosphorShaders,
PhosphorCompositor, PhosphorIdentity, PhosphorLayoutApi and PhosphorProtocol, whose types the settings expose;
PhosphorGeometry, PhosphorPlacement, PhosphorWorkspaces and PhosphorAudio for the core; PhosphorContextResolver and
PhosphorShortcuts for tracking, drag and the shortcuts; PhosphorLayer, PhosphorShellPatterns, PhosphorOverlay and,
privately, PhosphorSurfaces, PhosphorRendering and PhosphorZoneRendering for the overlays. The overlay QML also
imports org.phosphor.zonerendering, org.phosphor.animation, org.phosphor.surface, Phosphor.Components
(phosphor-components) and Kirigami at run time, which the host process provides.
