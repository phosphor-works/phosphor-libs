<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team -->
<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->

# phosphor-paths

phosphor-paths names the folders where every Phosphor app keeps its files. It also moves a tree that an app kept in an
earlier place into its new place, one time.

## Responsibility

Each XDG root holds one folder for the whole suite, `phosphor-works`:

- **Shared trees** are directly below it, for the data that more than one app reads: `phosphor-works/layouts`,
  `phosphor-works/curves`, `phosphor-works/translations`, and every shader family under
  `phosphor-works/shaders/<family>`.
- **The files of one app** are in the folder of that app, `phosphor-works/<app>`. Its config is in the config root.
  Its state and cache are in their roots. The data that only it reads is in the data roots.

This library names those places, so no app or library spells the suite folder itself.

It depends on Qt6::Core only.

## Key types

| Function | Header | Purpose |
|----------|--------|---------|
| `suiteDirectoryName()` | `SuiteDirectories.h` | `phosphor-works` |
| `commonDataSubdir(tree)` | `SuiteDirectories.h` | A shared tree relative to the data roots, `phosphor-works/<tree>` |
| `setAppDirectoryName(name)`, `appDirectoryName()` | `SuiteDirectories.h` | The folder name of the app. Set it one time at the top of `main()`. The process aborts if the name is read before it is set, or if `isValidDirectoryName()` refuses the name |
| `appSubdir()` | `SuiteDirectories.h` | `phosphor-works/<app>`, relative to any XDG root |
| `appConfigDir()`, `appDataDir()`, `appStateDir()`, `appCacheDir()` | `SuiteDirectories.h` | The writable config, data, state and cache directories of the app |
| `moveTreeContents(from, to)` | `TreeMove.h` | Moves the files of a tree into a new place, and keeps the files that are already there |

## Use

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

An app that kept its files in `<root>/myapp` before moves them one time, if the new config is not there yet.

```cpp
if (!QFileInfo::exists(config)) {
    const QString root = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    PhosphorPaths::moveTreeContents(root + QStringLiteral("/myapp"), PhosphorPaths::appConfigDir());
}
```

## In CMake

Install destinations use the same name, never a literal: `PHOSPHOR_SUITE_DIRECTORY_NAME`. The phosphor-libs tree
defines it one time, in `cmake/PhosphorSuite.cmake`. That file has no dependency, so a build without Qt can include
it too. `find_package(PhosphorPaths)` sets the name for an installed copy.

```cmake
include("${PHOSPHOR_LIBS_CMAKE_DIR}/PhosphorSuite.cmake")
install(DIRECTORY data/layouts/ DESTINATION ${KDE_INSTALL_DATADIR}/${PHOSPHOR_SUITE_DIRECTORY_NAME}/layouts)
```

## Dependencies

- Qt6::Core
