<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team -->
<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->

# phosphor-control

phosphor-control is a Qt 6, QML and Kirigami framework for settings apps. It gives the window chrome (sidebar,
breadcrumbs, footer and page host) and the orchestration (`PageController`, `PageRegistry`, `StagingDomain`,
`ApplicationController`) that a Kirigami settings app builds on. Each consumer writes only its own page controllers
and QML pages.

## Responsibility

A settings app on this framework declares a set of pages. A staging domain holds the pending edits of each page. The
shell does the rest: sidebar navigation, breadcrumbs, a global dirty flag, one Apply or Discard across all domains,
deep-link reveal and global search. The library supplies these parts:

| Part | What it does |
| --- | --- |
| Staging domains | `StagingDomain` is the unit of staged work: `isDirty()`, `apply()`, `discard()`, `resetToDefaults()`, and signals for asynchronous completion. `PageController` is a domain that also has a QML page and a sidebar row. A headless domain holds state that more than one page uses (for example, maps of assignments for each screen) and has no sidebar entry. |
| Page catalog | `PageRegistry` holds the page tree (top-level entries, and children that drill down or collapse). A stable page id is the key of each page. The registry drives the sidebar through serialized `Entry` views. |
| Top-level orchestration | `ApplicationController` owns the registry and the registered domains. It calculates the global dirty flag again when a domain changes. It runs `applyAll()` and `discardAll()` as one transaction (synchronous or asynchronous), sends `resetToDefaults()` to one page, and keeps deep-link anchors for `PageHost` to reveal. |
| Global search | `SearchController` builds a ranked result index from three sources: the page registry (Page entries), authored anchors (`addEntry()`) and dynamic content (`ISearchProvider`). `SearchRanker` is the scorer behind it. It is pure and headless, and it accepts typing errors. |
| QML chrome | `org.phosphor.control` supplies `SettingsAppWindow`, `Sidebar`, `Breadcrumbs`, `UnsavedChangesFooter`, `PageHost`, `DiscardChangesDialog`, `AboutPageShell`, `ModulePage`, `ExtensionSlot` and `ExtensionFrame`, connected to the controllers above. |
| Glue | `DBusBridge` calls a daemon. `LocalizedContext` gives bare-word `i18n()` in QML for builds with Qt only (no KF6). |

## Key types

| Type | Purpose |
| --- | --- |
| `PhosphorControl::ApplicationController` | The top-level orchestrator. It owns the registry and the domains, the global dirty flag, the batched `applyAllAsync` and `discardAllAsync`, the current page and the deep-link anchor. `bindPage` binds a page whose controller arrives late. One owned navigation hook (`setNavigationHook`) lets a plugin host load pages. Other modules add sections to a page through one owned extension provider (`setExtensionProvider`), behind `extensionsFor` and `extensionsChanged`. `attachDomainToPage` makes the domain of a section count toward the dirty mark and the reset of its page. |
| `PhosphorControl::StagingDomain` | The abstract unit of staged changes: `isDirty()`, `apply()`, `discard()`, `resetToDefaults()`, with the asynchronous signals `applyResult` and `discardResult`. |
| `PhosphorControl::PageController` | A `StagingDomain` that also has a QML page and a sidebar row, with a stable page id that is unique in the app. |
| `PhosphorControl::PageRegistry` | The catalog of registered pages as a tree (one `Entry` for each page). It grows at run time and never shrinks. `registerPage` adds a page (optionally at a sibling slot). `bindPage` replaces the controller and QML of a page in place. `setPageVisibility` sets a new tier. |
| `PhosphorControl::SearchController` | Global settings search. It ranks results from the page registry, the authored anchors and the registered providers. |
| `PhosphorControl::SearchRanker` | Scoring and ranking that is pure, stateless and accepts typing errors. It is headless and uses no Qt but `QString`. |
| `PhosphorControl::ISearchProvider` | A source of dynamic search entries that the app implements (rules, shaders, layouts and similar). |
| `PhosphorControl::SearchEntry` | One searchable item (Page, Section, Setting or Entity), with the address `"pageId"` or `"pageId#anchor"`. |
| `PhosphorControl::DBusBridge` | The configured endpoints for daemon calls: synchronous `call()` and asynchronous `asyncCall()`, each also with an explicit interface. |
| `PhosphorControl::LocalizedContext` | A QML i18n context that sends `i18n()`/`i18nc()`/`i18np()` through Qt translation, for consumers with Qt only and no KF6. |
| `PhosphorControl::IControlModule` | The interface that the root object of a control module implements (`registerInto(ModuleContext&)`), IID `org.phosphor_works.PhosphorControl.Module/1`. |
| `PhosphorControl::ModuleMetadata` | The embedded metadata of a module, parsed and validated for each locale: id, name, icon, category, parent module, the categories it defines, pages and their anchors, extension points, extensions, search entries, and if it registers search providers. |
| `PhosphorControl::ModuleContext` | What a module registers into: its page controllers by local page id, its headless staging domains, the domain and QML of each section it adds to the page of another module, and its search providers (`registerSearchProvider`). It also gives one config backend for each file, shared by all modules that write that file. |
| `PhosphorControl::ModuleHost` | Finds control modules through a `PhosphorRegistry::PluginCatalog` and fills the page registry with placeholders. It resolves extensions against their target pages. It loads a module the first time a user opens one of its pages, or a page it extends. While a search query is not blank, it loads each available module that has search providers (not loaded yet, not failed, and not turned off after a crash). With a `SearchController`, it lists the pages and search entries of all modules before they load. |
| `PhosphorControl::ModulePlaceholderPage` | The controller of a page that stands in for a module page while the module is not loaded. It holds the module name, if the module failed or was turned off after a crash, the reason from the host, and `retry()`. `ModuleHost` registers one for each placeholder, and `ModulePage` shows it. |
| `PhosphorControl::ExtensionSlotModel` | The sections in one slot of one page, for `ExtensionSlot`. It reads `extensionsFor` again on `extensionsChanged`, and keeps the row of a section that stays. |
| `PhosphorControl::ModuleTestHost` | Loads one module headless and checks it against the module contract, for the CI of the module: metadata, registration, staging, search providers, extension targets and anchor stability. It is in the separate `PhosphorControl::PhosphorControlTest` library. |
| `phosphor-control-metadata` | The build tool behind `phosphor_add_control_module_metadata()`. It validates the metadata of a module, adds `Key[<lang>]` translations from its `.ts` files, and writes its anchor list and lupdate strings. |

## Use

A consumer subclasses `ApplicationController` to declare its pages and subclasses `PageController` for each page.
Then it points the QML `SettingsAppWindow` at the controller. The caller translates the titles, because the library
does not supply a translation context for app strings.

```cpp
#include <PhosphorControl/ApplicationController.h>
#include <PhosphorControl/PageController.h>

using namespace PhosphorControl;

// Each page stages its own edits.
class GeneralPage : public PageController
{
    Q_OBJECT
public:
    explicit GeneralPage(QObject* parent = nullptr)
        : PageController(QStringLiteral("general"), parent) {}

    bool isDirty() const override { return m_dirty; }
    void apply() override   { /* persist */ m_dirty = false; Q_EMIT dirtyChanged(); Q_EMIT applyResult(true, {}); }
    void discard() override { /* reload  */ m_dirty = false; Q_EMIT dirtyChanged(); Q_EMIT discardResult(true, {}); }

private:
    bool m_dirty = false;
};

// The app wires its pages up at construction.
class MySettings : public ApplicationController
{
    // Q_OBJECT gives MySettings its own tr() context, which is the one
    // lupdate extracts the string below under.
    Q_OBJECT
public:
    explicit MySettings(QObject* parent = nullptr)
        : ApplicationController(parent)
    {
        auto* general = new GeneralPage(this);
        registerPage(general, {}, MySettings::tr("General"),
                     QUrl(QStringLiteral("qrc:/qt/qml/MyApp/qml/GeneralPage.qml")));
    }
};
```

```qml
// Main.qml — the chrome binds to the controller exposed from C++.
import org.phosphor.control

SettingsAppWindow {
    controller: appController   // your ApplicationController subclass
}
```

The shell controls the dirty and Apply cycle from the signals of the domains. `UnsavedChangesFooter` binds to
`ApplicationController::dirty`, `applying` and `discarding`. Thus a synchronous page only emits
`applyResult(true, {})` at the end of `apply()`, and the footer changes "Save" to "Saving…" automatically.

`examples/phosphor-control/minimal/` at the repository root is a complete app with two pages (General and About).
Build it with `-DPHOSPHOR_LIBS_BUILD_EXAMPLES=ON`.

## Control modules

An app on PhosphorControl can get its pages from control modules. A module is a Qt plugin in the `control` seam. Its
root object implements `IControlModule`, and its metadata (`ModuleMetadata`) lists its pages. `ModuleHost` reads the
metadata of each module without loading it, and fills the page registry before anything loads:

- It adds a node for each category in use (`@category/<id>`), a node for each module (`<module>`), and a page for
  each module page (`<module>:<page>`). The order is by `Order`, then by name. The pages of one module come parents
  first, and siblings in the order of declaration.
- Each module page shows `ModulePage.qml` until its module loads. Thus it is an ordinary page in the sidebar, in
  search and in keyboard navigation.
- A page can be under one of the pages of its module, under the page of another module (`<module>:<page>`), or
  directly in a category. It gets its tier, divider and counterpart from the metadata.

### Loading

When a user opens a page of a module, the navigation hook of `ApplicationController` loads the module before the page
change is announced. The `registerInto` of the module registers into a `ModuleContext`, which only records. Then the
host checks the full registration. If all is correct, it binds all pages (`ApplicationController::bindPage`) and
domains at once. If something is wrong, it keeps nothing and fails the module with the reason. These are wrong:

- a page or extension that the metadata does not declare
- a skipped page or extension, or one registered two times
- a controller with the wrong id
- a page or section without QML
- a search provider that is null, registered two times, registered without `"SearchProviders": true` in the
  metadata, in another thread, or deleted while the module registered

Only one call does more than record: `ModuleContext::configBackend` (see "Shared config files"). It shares the
backend of the file at once, and the backend stays shared when the host does not keep the registration. A module
that did not load keeps its placeholder page. The page says why, and offers a retry when a new load can help.

**The QML of a module page.** `PageHost` creates each page bound with `bindPage`, and the `ModulePage` placeholder
before it. It sets two properties before any bindings of the page run: `controller`, the `PageController` of the
page, and `settingsApp`, the `ApplicationController`. The root of the page must declare both, and can make them
`required`. A root that does not declare one still loads, but Qt logs "Cannot assign to non-existent property" for
it. That message can have no usable file location. `PageHost` does not create such a page while the registry has no
controller for it. The page area then says that the page could not be shown, and a later `bindPage` creates it.

If the process stops during the load of a module, the module is disabled at the next start and at each start after
it. It stays disabled until `retryModule` (or the retry button of the placeholder page) loads it. One host owns the
load record that remembers this (`ModuleHost::setSentinelPath`), so a second `ModuleHost` needs its own path. A module
that failed after its library loaded stays failed when its file is offered again. The process continues to run the
code it loaded, so the new file has an effect at the next start.

`ModuleTestHost` runs a module headless through the same host, for the CI of the module. It puts the config files
that the module opens under its temporary directory (`ModuleHost::setConfigRedirect`). Thus the module starts from
its defaults and the run does not write the real settings, if the factory of the module opens the path it gets.

### Extensions

A module can also add sections to the page of another module. Its metadata lists each section under `Extensions`,
with these keys:

| Key | Value |
| --- | --- |
| `Target` | The target page, `<module>:<page>`. |
| `Slot` | The slot on that page. |
| `TargetVersion` | The version of the slot that the extension was built for (1 by default). |
| `Name` | The name of the extension. |

Each page offers `top`, `bottom`, and `before:<anchor>` / `after:<anchor>` for each entry of its `Anchors` list. All
of them are at the version of the reserved `anchors` extension point of the page (1 when the page declares none). A
module adds named slots with its top-level `ExtensionPoints` list. Each entry has a `Page`, an `Id` and a `Version`
(1 by default).

`ModuleHost` shows an extension only when its target page offers that slot at that version. An extension for a module
that is not installed stays inactive. An extension is refused when the host could not place its target module, or
when its page, slot or version does not agree. `refusedExtensions()` lists it with the reason, and
`refusedExtensionsChanged` tells when that list changes.

When a user opens a page, the host loads all modules that extend it, after the own module of the page has loaded. It
also loads them when a section becomes active while its page is open, and when the module of the page loads while the
page is open (a retry). A section shows when its extension is active and the module of its page has loaded. From then
on, its staging domain is part of Apply and counts toward the dirty mark and the reset of that page. These two
conditions are never undone. Before that, nothing shows the section, so it is part of nothing.

`PageHost` puts the `top` and `bottom` slots around each page, outside the scrolling content of the page. The page
puts the other slots itself with `ExtensionSlot`. `ExtensionSlot` lists them through `ExtensionSlotModel` and puts
each one in an `ExtensionFrame`. The frame names the module that the section comes from. It creates the QML of the
section with its `domain` already set, which the section must declare (it can make it `required`). The frame shows a
card in place of the section when the module did not load, the domain of the section was deleted, or its QML failed.
When the module is the cause, the card has a button to the own page of the module.

```qml
ExtensionSlot {
    Layout.fillWidth: true
    settingsApp: page.settingsApp
    pageId: "placement:snapping/behavior"
    slot: "after:drag-modifier"
}
```

### Build a module

A module embeds generated metadata, not its source file, so that the build can validate it and add translations. It
links `PhosphorControl` and installs into the `control` seam of a Qt plugin directory,
`<Qt plugin dir>/phosphor-works/control/`. `ModuleHost` looks there by default (`PhosphorRegistry::PluginCatalog`
lists the full search path). `KDE_INSTALL_QTPLUGINDIR` below comes from `KDEInstallDirs6` of ECM.

```cmake
find_package(Qt6 REQUIRED COMPONENTS Core)
find_package(PhosphorControl REQUIRED)
# AUTOMOC, which Q_PLUGIN_METADATA needs.
qt_standard_project_setup()

qt_add_plugin(control_module_mine)
target_sources(control_module_mine PRIVATE mymodule.cpp)
target_link_libraries(control_module_mine PRIVATE PhosphorControl::PhosphorControl)
phosphor_add_control_module_metadata(control_module_mine
    METADATA metadata.json
    TS_FILES translations/control-module-mine_de.ts translations/control-module-mine_fr.ts)
install(TARGETS control_module_mine DESTINATION ${KDE_INSTALL_QTPLUGINDIR}/phosphor-works/control)
```

```cpp
Q_PLUGIN_METADATA(IID PhosphorControlModule_iid FILE "module_metadata.json")
```

Metadata that is not valid fails the build. A warning, such as a key that this PhosphorControl does not know, shows
one time and never fails the build. Thus metadata written for a newer PhosphorControl still builds. For a cross
build, set `PHOSPHOR_CONTROL_METADATA_EXECUTABLE` to a `phosphor-control-metadata` that runs on the build machine.

The source that names `module_metadata.json` is compiled again in the same build when the metadata changes. Moc must
embed the generated file and nothing else, so these rules apply:

- `METADATA` must have another name.
- No other `module_metadata.json` can be next to that source, or in the source or binary directory of the target.
- The dependency is kept for each directory. Thus two modules built in one directory cannot compile the same plugin
  source file. Give each module its own path to it, for example a symbolic link.

`phosphor_add_control_module_metadata()` stops the configure in each of these cases.

### Translations

The translations come from the catalogs of the module. The build adds each finished translation in the context
`PhosphorControlModule` as `Key[<lang>]`. The key of a catalog is its language and the script of that language. Thus
`de_DE` becomes `Key[de_Latn]` and `zh_TW` becomes `Key[zh_Hant]`. Each locale lists that form in its UI languages.
Thus a user in `de_AT` gets the `de_DE` catalog, but a user in `zh_TW` never gets the Simplified Chinese of `zh_CN`.

| Case | Key |
| --- | --- |
| Two catalogs for one language and script (`de` and `de_AT`) | Each catalog gets its own language, with `-` changed to `_`. |
| A language that Qt has locales for in more than one script (`zh`, `sr`, `uz`, `pa`, `az`, and `en` for Deseret and Shavian) | Never its code alone, because each locale of the language lists that code for all scripts. A catalog for the bare code goes under the script that Qt reads it in: `zh` becomes `Key[zh_Hans]` and `sr` becomes `Key[sr_Cyrl]`. This is also true next to a regional catalog such as `zh_CN`, which keeps `Key[zh_CN]`. A `zh` catalog next to a `zh_Hans` one fails the build as two catalogs for `zh_Hans`. |
| A language with one script | It keeps its bare key (`Key[de]` next to `Key[de_AT]`). |
| A language code | The code that Qt gives the language. Thus a `no_NO` catalog serves `nb` users. |
| An old code for a language with more than one script, such as `sh` | The code does not tell the script, so name the script (`sr_Latn`). A catalog with only such a code stays under that code, which no locale lists, and the build gives a warning. |

A `Key[<lang>]` that you write by hand in the metadata wins over the catalog, unless it is empty. It wins at these
keys:

- at the key of the catalog
- at the language and script, when the catalog keeps its own longer key (`Key[de_Latn]` next to `de` and `de_AT`
  catalogs)
- at the bare language, when the script of the catalog is the usual script of the language (`Key[de]`, but not
  `Key[zh]` next to a `zh_TW` catalog)

Thus a `Key[zh]` written by hand counts as Simplified Chinese. It stops a `zh`, `zh_CN` or `zh_Hans` catalog, but a
`zh_TW` catalog still serves its users. A key written by hand uses the code that Qt gives the language (`Key[nb]`),
because `Key[no]` is never selected.

The build leaves out an unfinished translation, but `lrelease` ships it unless you give `-nounfinished`. Give that
option, so that the pages of a module use the same language as its sidebar entry.

`MODULE_STRINGS_FILE` holds the strings of the metadata for `lupdate`, in the context `PhosphorControlModule`. The
build writes it (the target `<target>_module_metadata`), so the `lupdate` target depends on that target:

```cmake
get_target_property(moduleStrings control_module_mine MODULE_STRINGS_FILE)
qt_add_lupdate(SOURCE_TARGETS control_module_mine SOURCES ${moduleStrings}
    TS_FILES translations/control-module-mine_de.ts translations/control-module-mine_fr.ts
    LUPDATE_TARGET control_module_mine_lupdate)
add_dependencies(control_module_mine_lupdate control_module_mine_module_metadata)
```

### Anchor stability

The generated `anchors.json` (the `MODULE_ANCHORS_FILE` of the target) records the anchors of each page, its
`anchors` version, and the version of each of its named extension points. Ship it with each release. The CI of the
next release gives it to `ModuleTestHost::setPreviousAnchors()`. The check fails in these cases, because other modules
can extend the page:

- a page is removed
- an anchor is removed and the `anchors` version of the page did not increase
- that version decreased
- a named extension point is removed, or its version decreased

### Test a module

`ModuleTestHost(path).run()` loads the module headless from a private copy. It returns each problem that it finds, on
one line each (`ModuleTestHost.h` lists the checks). It reports all that `ModuleHost` refuses, and also these
problems:

- each metadata warning, as `metadata: <warning>`, because the module is built against the PhosphorControl that
  checks it
- a module whose metadata says `"SearchProviders": true` but that registers no provider
- a page controller or domain that code of the module deleted while the module loaded
- a domain that does not answer apply or discard exactly one time

`addTargetModule()` adds a module that this module extends, so that the test checks its extensions against the slots
of that module. The copies have the names `module.so` and `target-000.so` and up. Each problem names a module by the
path that it got.

### Search before loading

At start, the host indexes each module from its metadata (`ModuleHost::setSearchController`). The index holds:

- each page with its `Keywords`, the `Keywords` of the module and the name of the module
- each `Search` entry (a setting or section at one of the `Anchors` of the page)
- each active extension, as a section of its page, at the anchor `<module>:<extension>`. The `ExtensionFrame` of the
  section registers that anchor with the reveal registry of the page.

The entries are in the sidebar order of their pages. This order decides ties and what a result limit removes. A page
whose module failed, or was turned off after a crash, keeps only its own page result. That result opens the
placeholder, which says why.

Content that only code can list, such as rules, layouts or shaders, comes from an `ISearchProvider`. The module
registers it through `ModuleContext::registerSearchProvider`, with `"SearchProviders": true` in its metadata. When the
query is not blank, the host loads each such module that has not loaded yet. This includes a query typed before start
and a module installed while a query stays. The host asks each provider for its entries one time while it records
the load of the module, so a provider that crashes then disables its module. It asks again at each index rebuild,
while the `ModuleHost` exists. The entries of a provider must be on the own pages of the module or on the pages that
its extensions target. The host leaves out the others, with a warning.

### Shared config files

Modules that keep settings in one file share its backend. `ModuleContext::configBackend(path, create)` creates the
backend for the first module that opens the file, and gives the same backend to each later module. Thus one Apply
writes the keys of all modules into one document, and no module overwrites the keys of another. Each module puts the
backend in its own `PhosphorConfig::Store` or settings object. A module whose settings belong to a host that may not
be running finds the file with `PhosphorConfig::HostDirectory`.

The path must be absolute. Paths that go to one file through symbolic links, `.` or `..` get one backend, also when
the file or its directory does not exist yet. `create` gets the resolved path. A shared backend sets three rules for
the modules:

| Rule | Detail |
| --- | --- |
| Same resolver and stamp | Give each `Store` on the backend the same path resolver object and the same version stamp. `Schema::pathResolver` is compared by pointer, so an equal copy counts as another object. The settings of the first `Store` stay. The host ignores a different setting from a later `Store`, with a warning. The migrations of a `Store` run on the full document. |
| One live group | A `JsonBackend` allows one live group at a time, for all modules. A second group asserts in a debug build and is read-only in a release build, so its writes are lost. Open a group, use it and release it without running code of other modules between these steps. |
| Whole document | `sync()`, `commit()` and `reparseConfiguration()` act on the full document. A write also saves what the other modules have not saved yet, and a reparse drops it. |

The backends belong to the `ApplicationController`. `ModuleHost` keeps them in an object whose parent is the
controller. Thus they stay after the host is gone, and a domain can still apply. The controller destroys them before
the page controllers and domains that it adopted from modules, and a `JsonBackend` then writes what was not saved.
Thus a module must not use its backend from a destructor, and must not keep one of its groups in a member. A second
`ModuleHost` on the same controller keeps its own backends, and the backends of the first host stay with the
controller. Thus, when the modules of two hosts open one file, the hosts hold two documents for it, and each write
drops the keys that only the other document holds.

`ModuleHost::configBackend(path)` returns the backend that shares a file, for a host application or a test that must
examine it. Modules get their backend from the context. `ModuleHost::setConfigRedirect(directory)` puts each such
file under `directory`, at its own absolute path below it. Use it for a test that must not change the real settings.

## Design notes

- **Staging is the contract, not persistence.** `ApplicationController` calls `apply()` / `discard()` only on domains
  whose `isDirty()` is true. Thus an implementation must not rely on side effects that must run while it is clean
  (time stamps, notifications). Each domain must emit `applyResult` / `discardResult` exactly one time in each batch.
  The pending counter of the asynchronous driver keeps the "Saving…" state of the chrome until the result comes or
  the 60 s timeout occurs.
- **The boundary with phosphor-config and phosphor-shortcuts.** This library defines no configuration store and no
  shortcut backend. Consumers connect their own `ISettings` to [`phosphor-config`](../phosphor-config/README.md),
  and get global shortcuts from [`phosphor-shortcuts`](../phosphor-shortcuts/README.md). The library links
  `phosphor-config` for one purpose: `ModuleHost` keeps the `IBackend` of each config file that control modules
  share. The first module that opens the file creates it (`ModuleContext::configBackend`).
- **A C++ core that works headless.** The `PhosphorControl` library does not use `Qt6::Quick`. Thus headless tests
  can construct `ApplicationController`, `PageController` and `StagingDomain` without a QML engine. The QML module is
  a separate target. `Qt6::Gui` is different: the library links it privately, but `phosphor-config` links it
  publicly, so it gets to each consumer through that dependency.
- **Search has layers.** `SearchRanker` is pure and has its own unit tests. It scores the best tier of each field,
  with the weights title > keywords > subtitle, and uses edit distance only as a "did you mean" fallback.
  `SearchController` combines it with the page registry and the providers. The app supplies translated strings, and
  the index stores them as they are.
- **STATIC QML module in the tree, SHARED when you ask for it.** `org.phosphor.control` builds STATIC for consumers
  in the tree, which link it directly. Consumers outside the tree that use `find_package(PhosphorControl)` need the
  SHARED runtime plugin. Set `-DPHOSPHOR_CONTROL_QML_INSTALL=ON` to build it (the same as phosphor-animation).

## Dependencies

| Dependency | Why |
| --- | --- |
| `QtCore`, `QtDBus`, `QtQml` | Linked publicly. |
| `QtGui` | The library links it privately. Consumers get it through `phosphor-config`. |
| `QtQuick` | The QML module adds it. At run time, the QML imports `QtQuick.Controls`, `QtQuick.Layouts` and KF6 Kirigami (`org.kde.kirigami`). |
| [`phosphor-animation`](../phosphor-animation/README.md) | The QML chrome imports `org.phosphor.animation` for the spring-physics motion profiles (panel fade and slide, accordion expand and collapse, widget fade, hover and tint). |
| [`phosphor-registry`](../phosphor-registry/README.md) | `PluginCatalog`, the plugin loader that `ModuleHost` uses to find and load control modules. |
| [`phosphor-config`](../phosphor-config/README.md) | `IBackend`, the config backend that `ModuleContext::configBackend` gives to control modules, one for each file. |

## See also

- [`phosphor-config`](../phosphor-config/README.md): the configuration store that the `ISettings` implementation of
  an app connects to, and the backends that control modules share. This library does not copy that contract.
- [`phosphor-shortcuts`](../phosphor-shortcuts/README.md): global-shortcut backends, which this library does not
  hold, by design.
