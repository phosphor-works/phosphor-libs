<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team -->
<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->

# phosphor-registry

phosphor-registry is a generic registry of factories by name. It serves the five UI extension seams of phosphor-shell
(bar widgets, control center tiles, launcher providers, OSDs and desktop widgets). It also has the plugin catalog that
all plugin seams of the suite share, and the bar-widget plugin loader built on that catalog.

## Responsibility

`phosphor-registry` puts the registry pattern into a single template. These registries store their entries in a
`Registry<T>`: `phosphor-layout-api/ILayoutSourceFactory`, `phosphor-tiles/AlgorithmRegistry`,
`phosphor-shaders/ShaderRegistry` and `phosphor-animation/AnimationShaderRegistry`. The shell makes one instance of
the same template for each UI seam.

- **One `Registry<T>` template, five interface specializations.** The shell owns one `Registry<IBarWidgetFactory>`,
  one `Registry<IControlCenterTileFactory>`, and so on. Each one is independent. Tests and the demos make their own
  local registries with fakes.
- **Built-ins register explicitly.** The shell's composition root calls `registry.registerFactory(...)` for each
  built-in. There is no registration during static initialization, and no global pending list. Plugins use the same
  pattern: the `PluginLoader` calls the same `registerFactory`.
- **One plugin mechanism for the suite.** `PluginCatalog` finds the plugins of one seam (a kind of plugin, such as bar
  widgets). It reads the metadata of each plugin without loading it, and loads a plugin only on request. A plugin is a
  Qt plugin (`qt_add_plugin` + `Q_PLUGIN_METADATA`). Its IID is the IID of the seam, and its metadata has `"Id"` and
  `"ApiVersion"`. `PluginLoader` is the bar-widget seam on top of the catalog.
- **Live reload on a disk change.** A `WatchedDirectorySet` (from phosphor-fsloader) watches the search paths and
  runs a debounced rescan. The catalog offers a new plugin and withdraws a removed plugin. If a search path does not
  exist yet, the catalog finds it when it appears. The exception is when its nearest existing ancestor is a directory
  that the watcher refuses to watch, such as `~/.local/share` on a fresh account or `/tmp`. Then the next rescan finds
  it.
- **Nothing is unloaded.** The library of a withdrawn plugin stays mapped, so that the objects it created continue to
  work. A loaded plugin runs the code that it was loaded from until the process restarts. Qt also continues to report
  the metadata of that code for its path. Replace a loaded plugin by rename (`install`, `mv`). Never write over it in
  place, because that changes the code that the process runs.
- **A plugin that is not loaded is offered again when its file changes.** A plugin whose library failed to load, or
  that was never loaded, stays on offer. `pluginAdded` announces it again in these cases: its file is written again,
  renamed over, shadowed by a copy with a higher priority, or removed while a different copy takes its place. A
  rebuild is sufficient. A loaded plugin is never offered again.
- **Capabilities are advisory.** Each factory declares a `capabilities()` list. The loader does not enforce it.

## Key types

| Type | Purpose |
|------|---------|
| `PhosphorRegistry::Registry<T>` | Template registry. Stores factories of type `T` by the key `T::id()`. One instance for each composition root. Emits `factoryRegistered` / `factoryUnregistered` through its `RegistryNotifier`. |
| `PhosphorRegistry::RegistryNotifier` | The QObject that carries the signals for the template. The template cannot be Q_OBJECT, so the signals go through this object. Get it with `registry.notifier()`. |
| `PhosphorRegistry::IFactoryBase` | Common base of the five interface families. Declares `id()`, `displayName()`, `capabilities()`. |
| `PhosphorRegistry::IBarWidgetFactory` | Factory for one top-bar widget (clock, workspaces, tray, and others). Adds `createWidget(QQmlEngine*, QObject*)`. |
| `PhosphorRegistry::IControlCenterTileFactory` | Factory for one tile in the Control Center popout. Adds `createTile(QQmlEngine*, QObject*)`. `phosphor-shell-libs/libs/phosphor-shell-control-center` uses it, over the registry that `phosphor-shell/src/ControlCenterController` fills. Give the returned item `JavaScriptOwnership`, because the surface destroys the tiles that it built. |
| `PhosphorRegistry::ILauncherProvider` | The query contract of the launcher. Each provider implements it. Declares `setQuery()`, `results()`, `resultsChanged()` and `activate(id, Primary\|Alternate)`. If `activate` returns false, the surface stays open. |
| `PhosphorRegistry::ILauncherProviderFactory` | Factory for a launcher query provider (apps, calculator, and others). Adds `createProvider(QObject*) -> ILauncherProvider*`. `phosphor-shell-libs/libs/phosphor-shell-launcher` uses it, over the registry that `phosphor-shell/src/LauncherController` fills. |
| `PhosphorRegistry::IOSDFactory` | Factory for an on-screen display (volume / mic / brightness). Adds `createOSD(QQmlEngine*, QObject*)`. **No surface uses it.** |
| `PhosphorRegistry::IDesktopWidgetFactory` | Factory for a desktop card / widget. Adds `createWidget(QQmlEngine*, QObject*)`. **No surface uses it.** |
| `PhosphorRegistry::PluginCatalog` | Finds the plugins of one `PluginSeam` on the search paths. Offers their `PluginMetadata` without loading them, records each `RefusedPlugin` with the reason, and loads one plugin on `load(id)`. Reloads when the filesystem changes. |
| `PhosphorRegistry::IBarWidgetPlugin` | The interface that the root object of a bar-widget plugin implements: `createFactory() -> std::unique_ptr<IBarWidgetFactory>`. |
| `PhosphorRegistry::PluginLoader` | The bar-widget seam (`barWidgetSeam()`). Loads each bar-widget plugin when it finds it, and registers its factory in a `Registry<IBarWidgetFactory>`. Unregisters the factory when the catalog withdraws the plugin. |

## Use

### Shell composition root

The C++ composition root registers the built-ins explicitly. Then it starts the plugin loader for third-party
plugins.

```cpp
#include <PhosphorRegistry/PhosphorRegistry.h>

using namespace PhosphorRegistry;

Registry<IBarWidgetFactory> barRegistry;

// Built-ins: explicit register
barRegistry.registerFactory(std::make_shared<ClockFactory>());
barRegistry.registerFactory(std::make_shared<WorkspacesFactory>());
barRegistry.registerFactory(std::make_shared<MediaFactory>());

// Connect first: the first scan loads every plugin before it returns.
QObject::connect(barRegistry.notifier(), &RegistryNotifier::factoryRegistered,
                 [&](const QString& id) { qDebug() << "registered" << id; });

// Plugins, into the same registry:
PluginLoader loader(&barRegistry);  // searches PluginCatalog::defaultSearchPaths("bar-widget")
loader.scanAndLoad();
```

### Glue layer

The shell puts the registry in a `QObject` controller. The controller exposes the factory ids as a
`Q_PROPERTY(QStringList)` and the widget construction as a `Q_INVOKABLE`. The library does not use QML, so each
consumer owns its facade. Both phosphor-registry demos use this controller pattern.

The snippets below call the controller `BarController`, as in the production shell (a top-bar widget controller).
`phosphor-shell-libs/examples/phosphor-registry-demo/Main.qml` uses the same pattern under the context-property name
`demoController`, so that the QML of the demo is easy to read.

```cpp
// barcontroller.h
class BarController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QStringList factoryIds READ factoryIds NOTIFY factoryIdsChanged)
public:
    explicit BarController(QObject* parent = nullptr);
    void setEngine(QQmlEngine* engine); // call once from main()
    Q_INVOKABLE QQuickItem* createWidgetFor(const QString& id, QQuickItem* parent);
    [[nodiscard]] QStringList factoryIds() const;
Q_SIGNALS:
    void factoryIdsChanged();
private:
    QPointer<QQmlEngine> m_engine;
    PhosphorRegistry::Registry<PhosphorRegistry::IBarWidgetFactory> m_registry;
};

// barcontroller.cpp: forward the registry's signals as one factoryIdsChanged
QObject::connect(m_registry.notifier(),
                 &PhosphorRegistry::RegistryNotifier::factoryRegistered,
                 this, &BarController::factoryIdsChanged);
QObject::connect(m_registry.notifier(),
                 &PhosphorRegistry::RegistryNotifier::factoryUnregistered,
                 this, &BarController::factoryIdsChanged);

QStringList BarController::factoryIds() const
{
    // Registry::ids() returns registration (insertion) order, which is
    // already deterministic. We sort here only to present an alphabetical
    // bar layout (a real shell would persist a user-configured order; the
    // demos sort alphabetically for simplicity).
    QStringList ids = m_registry.ids();
    ids.sort();
    return ids;
}

QQuickItem* BarController::createWidgetFor(const QString& id, QQuickItem* parent)
{
    if (!m_engine) return nullptr;
    auto factory = m_registry.factory(id);
    if (!factory) return nullptr;
    return factory->createWidget(m_engine.data(), parent);
}
```

### Bar QML

A `Repeater` uses `factoryIds` as its model, and puts each widget under its own delegate `Item`. The Repeater
destroys old delegates when the model changes. The QObject parent then destroys the widgets that C++ owns with them.
Do not call `child.destroy()`: it fails on QQuickItems that C++ owns, with "indestructible object" errors.

```qml
Row {
    spacing: 8

    Repeater {
        model: barController ? barController.factoryIds : []

        delegate: Item {
            id: slot
            required property string modelData
            property Item widget: null

            implicitWidth: widget ? widget.implicitWidth : 0
            implicitHeight: widget ? widget.implicitHeight : 0

            Component.onCompleted: {
                slot.widget = barController.createWidgetFor(slot.modelData, slot)
            }
        }
    }
}
```

The bar updates automatically when plugins load or unload, because `factoryIdsChanged` evaluates the `model` binding
of the Repeater again. `phosphor-shell-libs/examples/phosphor-registry-demo/` shows the variant in the process.
`phosphor-shell-libs/examples/phosphor-registry-plugin-demo/` shows the same pattern with a third widget loaded from a
separate plugin.

### Write a plugin

A plugin is a Qt plugin whose root object implements `IBarWidgetPlugin`. Install it in a `bar-widget` directory on
the search path.

```cpp
// myplugin.cpp
#include <PhosphorRegistry/IBarWidgetPlugin.h>

#include <new>

class MyFactory : public PhosphorRegistry::IBarWidgetFactory { /* id() returns "my-plugin" */ };

class MyPlugin : public QObject, public PhosphorRegistry::IBarWidgetPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID PhosphorRegistryBarWidgetPlugin_iid FILE "metadata.json")
    Q_INTERFACES(PhosphorRegistry::IBarWidgetPlugin)
public:
    std::unique_ptr<PhosphorRegistry::IBarWidgetFactory> createFactory() override
    {
        // createFactory must not throw, so allocate without throwing and
        // return nullptr when that fails.
        return std::unique_ptr<PhosphorRegistry::IBarWidgetFactory>(new (std::nothrow) MyFactory);
    }
};

#include "myplugin.moc"
```

```json
{
    "Id": "my-plugin",
    "ApiVersion": 1
}
```

```cmake
qt_add_plugin(my-plugin SHARED myplugin.cpp)
target_link_libraries(my-plugin PRIVATE PhosphorRegistry::PhosphorRegistry Qt6::Core Qt6::Quick)
install(TARGETS my-plugin LIBRARY DESTINATION ${KDE_INSTALL_PLUGINDIR}/phosphor-works/bar-widget)
```

Only the owner can have write permission on the library and its directory. A umask of 002 is the default for
user-private groups on some distributions. It makes both group-writable, and the catalog then refuses the plugin. Run
`chmod go-w` on both. Do this in a build tree too, if the build tree is on `PHOSPHOR_WORKS_PLUGIN_PATH`. The linker
writes a new file at each relink, so a build tree needs this after each build.

## Search order

The catalog searches these directories, highest priority first:

1. Each directory in `PHOSPHOR_WORKS_PLUGIN_PATH`, plus `/<seam>`. This is for development, for example the plugin
   output of a build tree.
2. `$XDG_DATA_HOME/phosphor-works/plugins/<seam>`.
3. Each Qt plugin directory (`QT_PLUGIN_PATH`, then Qt's own), plus `/phosphor-works/<seam>`. Packages install here.

The catalog skips relative entries. An earlier plugin overrides a later plugin with the same id, and the catalog logs
the override.

## Arbitration and lifecycle

- **Duplicate ids are rejected.** If `registerFactory` gets an id that is already in the registry, it does nothing
  and writes a qWarning. The first registration wins. The plugin loader asks the registry first, and does not load a
  plugin whose id is already registered.
- **The factory id must match the `"Id"` of the metadata.** The loader compares the factory's `id()` with the
  plugin's metadata. If they are different, the loader logs the mismatch and drops the factory. Thus a renamed plugin
  cannot silently take the place of a different plugin. `PluginCatalog::refused()` does not list this case, because
  it holds only the problems that the catalog found with the file itself.
- **Symlinked plugin files are refused.** The catalog refuses a plugin file that is a symlink, with a reason, and
  never follows it. Thus a link in a search path cannot point the loader at code outside it. The catalog follows a
  searched directory that is a symlink, or that is below a symlink.
- **Untrusted files are refused.** The catalog refuses a plugin file, or a plugin file in a directory, in these
  cases: a user other than this user or root owns it, or it is group-writable or world-writable. OpenSSH and sudo
  apply the same StrictModes rules to the files that they trust. Each case would let a different local user replace
  the code that the shell is about to run. The catalog also refuses a plugin with a `<file>.avx2` file next to it,
  because Qt loads that file instead on a CPU with the Haswell feature set. The catalog checks the file again just
  before it maps it, and every symbol must resolve at load time. The catalog checks only the plugin file and its
  directory. The libraries that the plugin links (also through its RUNPATH) or opens are its own trust decision. The
  catalog does not verify signatures or origin.
- **A wrong seam or version is refused before the load.** The catalog refuses a plugin from its metadata alone in
  these cases: its IID is not the seam's, it is built against a Qt that this process cannot load, its metadata has no
  valid `"Id"`, or its integer `"ApiVersion"` is not the seam's. The catalog logs it one time and lists it in
  `PluginCatalog::refused()` with the reason. The catalog never maps its library. For each change to the interface or
  metadata of a seam that a plugin relies on, increase the seam's `apiVersion` and the `/N` of its IID together. A
  plugin built for the old version then fails the IID check first.
- **Unload only drops the registry entry.** When the catalog withdraws a plugin (its file is removed or now refused),
  the loader unregisters its factory, unless a different owner has replaced it since. The library stays mapped for
  the lifetime of the process. Thus the widgets that the withdrawn plugin made continue to work until they are
  destroyed normally.
- **Capabilities are advisory.** Every factory has the `capabilities()` list, but the loader does not enforce it.

## Dependencies

- `QtCore`. The library does not link `QtGui`, `QtQuick` or Qt QML. The consumers that build widgets link them.
- `phosphor-fsloader` (PUBLIC link). `WatchedDirectorySet` + `IScanStrategy` give the live reload of the plugin
  catalog. They also support the header-only `MetadataPackLoader<T>` template, which exposes fsloader types in its
  public interface. Thus the dependency is transitive (PUBLIC) for consumers.

## See also

- `phosphor-shell-libs/examples/phosphor-registry-demo/`: a toy bar with two built-in widgets registered explicitly.
  It shows the registry seam.
- `phosphor-shell-libs/examples/phosphor-registry-plugin-demo/`: the same toy bar, plus a third widget loaded from a
  separate plugin. It shows the plugin seam, and the live reload of plugins added or removed while it runs.
- `phosphor-layout-api/ILayoutSourceFactory`: the earlier registry pattern that this library makes generic.
