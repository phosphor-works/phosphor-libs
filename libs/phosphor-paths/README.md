<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team
     SPDX-License-Identifier: LGPL-2.1-or-later -->

# phosphor-paths

> Where every Phosphor app's files live.

## Responsibility

Each XDG root holds one folder for the whole suite, `phosphor-works`:

- **Shared trees** sit directly beneath it, for whatever more than one app reads: `phosphor-works/layouts`,
  `phosphor-works/curves`, `phosphor-works/translations`, and every shader family under
  `phosphor-works/shaders/<family>`.
- **An app's own files** sit in its folder there, `phosphor-works/<app>`: its config in the config root, its state
  and cache in theirs, and data only it reads in the data roots.

This library names those places, so no app or library spells the suite folder itself. It also moves a tree an app
kept somewhere else before into its new place, once.

It depends on Qt6::Core only.

## Key API

| Function | Header | Purpose |
|----------|--------|---------|
| `suiteDirectoryName()` | `SuiteDirectories.h` | `phosphor-works` |
| `commonDataSubdir(tree)` | `SuiteDirectories.h` | A shared tree relative to the data roots, `phosphor-works/<tree>` |
| `setAppDirectoryName(name)`, `appDirectoryName()` | `SuiteDirectories.h` | The app's folder name; set once at the top of `main()`, aborts when read unset or given a name `isValidDirectoryName()` refuses |
| `appSubdir()` | `SuiteDirectories.h` | `phosphor-works/<app>`, relative to any XDG root |
| `appConfigDir()`, `appDataDir()`, `appStateDir()`, `appCacheDir()` | `SuiteDirectories.h` | The app's writable config, data, state and cache directories |
| `moveTreeContents(from, to)` | `TreeMove.h` | Move a tree's files into a new place, keeping what is already there |

## Typical use

```cpp
#include <PhosphorPaths/SuiteDirectories.h>
#include <PhosphorPaths/TreeMove.h>

int main(int argc, char** argv)
{
    PhosphorPaths::setAppDirectoryName(QStringLiteral("myapp"));
    QCoreApplication app(argc, argv);

    // ~/.config/phosphor-works/myapp/config.json
    const QString config = PhosphorPaths::appConfigDir() + QStringLiteral("/config.json");
    // phosphor-works/shaders/surface in every XDG data root
    const QStringList packs = QStandardPaths::locateAll(QStandardPaths::GenericDataLocation,
        PhosphorPaths::commonDataSubdir(QStringLiteral("shaders/surface")), QStandardPaths::LocateDirectory);
}
```

An app that kept its files in `<root>/myapp` before moves them once, when the new config is not there yet:

```cpp
if (!QFileInfo::exists(config)) {
    const QString root = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    PhosphorPaths::moveTreeContents(root + QStringLiteral("/myapp"), PhosphorPaths::appConfigDir());
}
```

## In CMake

Install destinations use the same name, never a literal: `PHOSPHOR_SUITE_DIRECTORY_NAME`. The phosphor-libs tree
defines it once, in `cmake/PhosphorSuite.cmake`, which has no dependency, so a build without Qt can include it too.
`find_package(PhosphorPaths)` sets it for an installed copy.

```cmake
include("${PHOSPHOR_LIBS_CMAKE_DIR}/PhosphorSuite.cmake")
install(DIRECTORY data/layouts/ DESTINATION ${KDE_INSTALL_DATADIR}/${PHOSPHOR_SUITE_DIRECTORY_NAME}/layouts)
```

## Dependencies

- Qt6::Core
