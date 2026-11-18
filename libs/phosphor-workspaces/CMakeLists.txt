# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# PhosphorWorkspaces — Virtual desktop / workspace management.
#
# Two libraries:
#   * PhosphorWorkspaces (PhosphorWorkspaces::PhosphorWorkspaces): the desktop
#     and activity managers, the workspace map and the reconciler. Reaches no
#     compositor itself: the managers take an IVirtualDesktopBackend and an
#     IActivityBackend.
#   * PhosphorWorkspacesPlasma (PhosphorWorkspaces::Plasma): the KDE Plasma
#     session's backends, KWin's virtual desktops over the session bus and
#     Plasma's activities over PlasmaActivities.

cmake_minimum_required(VERSION 3.16)

# 0.2.0: WorkspaceMap and WorkspaceReconciler joined the public API and
# VirtualDesktopManager's interface changed.
# 0.3.0: the managers take backends, KWin's and Plasma's moved to
# PhosphorWorkspaces::Plasma, and ActivityManager::isAvailable() is per
# manager. SOVERSION stays 0 below, like the rest of the phosphor-* catalog,
# until a deliberate ABI break.
set(PHOSPHORWORKSPACES_VERSION "0.3.0")

# The Qt floor this library requires AND advertises in its generated
# PhosphorWorkspacesConfig.cmake. Taken from the top-level QT_MIN_VERSION when built in-tree so
# the two cannot drift, with its own value for a STANDALONE build, where that
# variable does not exist. The fallback is not optional: the Config template
# substitutes this, and an undefined variable expands to nothing and ships a config
# whose find_dependency(Qt6 ...) carries no version floor at all.
if(DEFINED QT_MIN_VERSION)
    set(PHOSPHORWORKSPACES_QT_MIN ${QT_MIN_VERSION})
else()
    set(PHOSPHORWORKSPACES_QT_MIN "6.10.0")
endif()

if(NOT PROJECT_VERSION)
    project(PhosphorWorkspaces VERSION ${PHOSPHORWORKSPACES_VERSION} LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

include(GenerateExportHeader)

# Unconditional, like phosphor-zones: in-tree this re-find is a cache hit,
# and a standalone configure has no other source for the Qt targets the
# library links below.
find_package(Qt6 ${PHOSPHORWORKSPACES_QT_MIN} REQUIRED COMPONENTS Core DBus)

set(CMAKE_AUTOMOC ON)

# Sibling Phosphor libraries this one links. Each lookup is GUARDED rather than
# unconditional: in-tree the parent add_subdirectory()s the sibling before this
# directory, so its ALIAS target already exists and the find_package is skipped.
# A STANDALONE configure (the project() guard above says this directory supports
# one) has no such target, and without these it failed at target_link_libraries
# with "links to X::X but the target was not found".

# the shared placement policy every engine builds on
if(NOT TARGET PhosphorEngine::PhosphorEngine)
    find_package(PhosphorEngine CONFIG REQUIRED)
endif()

# the window-identity types the placement API keys on
if(NOT TARGET PhosphorIdentity::PhosphorIdentity)
    find_package(PhosphorIdentity CONFIG REQUIRED)
endif()

add_library(PhosphorWorkspaces SHARED
    include/PhosphorWorkspaces/IVirtualDesktopBackend.h
    include/PhosphorWorkspaces/IActivityBackend.h
    include/PhosphorWorkspaces/VirtualDesktopManager.h
    include/PhosphorWorkspaces/ActivityManager.h
    include/PhosphorWorkspaces/WorkspaceMap.h
    include/PhosphorWorkspaces/WorkspaceReconciler.h
    src/VirtualDesktopManager.cpp
    src/ActivityManager.cpp
    src/WorkspaceMap.cpp
    src/WorkspaceReconciler.cpp
    src/WorkspaceReconcilerNamed.cpp
    src/WorkspaceReconcilerScreens.cpp
)
add_library(PhosphorWorkspaces::PhosphorWorkspaces ALIAS PhosphorWorkspaces)

generate_export_header(PhosphorWorkspaces
    EXPORT_FILE_NAME phosphorworkspaces_export.h
    EXPORT_MACRO_NAME PHOSPHORWORKSPACES_EXPORT
)

# Resolve KDE_INSTALL_INCLUDEDIR before target_include_directories expands it
# into $<INSTALL_INTERFACE:>. Until it was set here, a non-KDE configure
# exported no install include directory while the headers still went to
# GNUInstallDirs' includedir.
if(NOT DEFINED KDE_INSTALL_INCLUDEDIR)
    include(GNUInstallDirs)
    set(KDE_INSTALL_INCLUDEDIR ${CMAKE_INSTALL_INCLUDEDIR})
endif()

target_include_directories(PhosphorWorkspaces
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
        $<INSTALL_INTERFACE:${KDE_INSTALL_INCLUDEDIR}>
)

target_compile_features(PhosphorWorkspaces PUBLIC cxx_std_20)

target_link_libraries(PhosphorWorkspaces
    PUBLIC
        Qt6::Core
        PhosphorEngine::PhosphorEngine
    PRIVATE
        PhosphorIdentity::PhosphorIdentity
)

# ═══════════════════════════════════════════════════════════════════════════════
# PhosphorWorkspacesPlasma Library (PhosphorWorkspaces::Plasma)
# ═══════════════════════════════════════════════════════════════════════════════

add_library(PhosphorWorkspacesPlasma SHARED
    include/PhosphorWorkspaces/Plasma/Backends.h
    include/PhosphorWorkspaces/Plasma/KWinVirtualDesktopBackend.h
    src/plasma/Backends.cpp
    src/plasma/KWinVirtualDesktopBackend.cpp
)
add_library(PhosphorWorkspaces::Plasma ALIAS PhosphorWorkspacesPlasma)

generate_export_header(PhosphorWorkspacesPlasma
    EXPORT_FILE_NAME ${CMAKE_CURRENT_BINARY_DIR}/PhosphorWorkspaces/Plasma/phosphorworkspacesplasma_export.h
    EXPORT_MACRO_NAME PHOSPHORWORKSPACESPLASMA_EXPORT
)

target_include_directories(PhosphorWorkspacesPlasma
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
        $<INSTALL_INTERFACE:${KDE_INSTALL_INCLUDEDIR}>
)

target_compile_features(PhosphorWorkspacesPlasma PUBLIC cxx_std_20)

# The public headers forward-declare the QDBus types they name, so QtDBus
# stays PRIVATE: a consumer of the backends gets no D-Bus it did not ask for.
target_link_libraries(PhosphorWorkspacesPlasma
    PUBLIC
        PhosphorWorkspaces::PhosphorWorkspaces
    PRIVATE
        Qt6::DBus
)

# Optional activities support — PlasmaActivities is the canonical KDE
# Plasma 6 package; KF6Activities is the older KF6 name still shipped on
# some 6.x distros. KF5 KActivities is intentionally not consulted —
# Plasma 6 dropped KF5 entirely.
find_package(PlasmaActivities QUIET)
if(NOT PlasmaActivities_FOUND)
    find_package(KF6Activities QUIET)
endif()

# Gate the compile-define on the actual IMPORTED target existing, not
# on _FOUND. A legacy Config that exposes only variables (no target)
# would otherwise wire `HAVE_KACTIVITIES` without a link dependency,
# producing undefined-symbol errors in the activity backend.
# This is the only activities lookup in the tree: the app tier links nothing
# from it directly, and it runs whatever USE_KDE_FRAMEWORKS says, since the
# library knows no app-tier option. Without it createActivityBackend()
# answers null.
set(_phosphorworkspaces_activities_target "")
if(TARGET Plasma::Activities)
    set(_phosphorworkspaces_activities_target Plasma::Activities)
    message(STATUS "PhosphorWorkspaces: PlasmaActivities found, activity support enabled")
elseif(TARGET KF6::Activities)
    set(_phosphorworkspaces_activities_target KF6::Activities)
    message(STATUS "PhosphorWorkspaces: KF6Activities found, activity support enabled")
else()
    message(STATUS "PhosphorWorkspaces: no activities library found, activity support disabled")
endif()
if(_phosphorworkspaces_activities_target)
    target_sources(PhosphorWorkspacesPlasma PRIVATE
        src/plasma/PlasmaActivityBackend.h
        src/plasma/PlasmaActivityBackend.cpp
    )
    target_compile_definitions(PhosphorWorkspacesPlasma PRIVATE HAVE_KACTIVITIES)
    target_link_libraries(PhosphorWorkspacesPlasma PRIVATE ${_phosphorworkspaces_activities_target})
endif()

# ═══════════════════════════════════════════════════════════════════════════════
# Install
# ═══════════════════════════════════════════════════════════════════════════════

if(NOT DEFINED KDE_INSTALL_LIBDIR)
    include(GNUInstallDirs)
    set(KDE_INSTALL_LIBDIR ${CMAKE_INSTALL_LIBDIR})
endif()

if(NOT DEFINED KDE_INSTALL_BINDIR)
    include(GNUInstallDirs)
    set(KDE_INSTALL_BINDIR ${CMAKE_INSTALL_BINDIR})
endif()

set_target_properties(PhosphorWorkspaces PROPERTIES
    VERSION ${PHOSPHORWORKSPACES_VERSION}
    SOVERSION 0
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

set_target_properties(PhosphorWorkspacesPlasma PROPERTIES
    VERSION ${PHOSPHORWORKSPACES_VERSION}
    SOVERSION 0
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    EXPORT_NAME Plasma
)

install(TARGETS PhosphorWorkspaces PhosphorWorkspacesPlasma
    EXPORT PhosphorWorkspacesTargets
    LIBRARY DESTINATION ${KDE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${KDE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${KDE_INSTALL_BINDIR}
)

install(DIRECTORY include/PhosphorWorkspaces
    DESTINATION ${KDE_INSTALL_INCLUDEDIR}
)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/phosphorworkspaces_export.h"
    DESTINATION ${KDE_INSTALL_INCLUDEDIR}/PhosphorWorkspaces
)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/PhosphorWorkspaces/Plasma/phosphorworkspacesplasma_export.h"
    DESTINATION ${KDE_INSTALL_INCLUDEDIR}/PhosphorWorkspaces/Plasma
)

install(EXPORT PhosphorWorkspacesTargets
    FILE PhosphorWorkspacesTargets.cmake
    NAMESPACE PhosphorWorkspaces::
    DESTINATION ${KDE_INSTALL_LIBDIR}/cmake/PhosphorWorkspaces
)

include(CMakePackageConfigHelpers)
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/PhosphorWorkspacesConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/PhosphorWorkspacesConfig.cmake"
    INSTALL_DESTINATION ${KDE_INSTALL_LIBDIR}/cmake/PhosphorWorkspaces
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/PhosphorWorkspacesConfigVersion.cmake"
    VERSION ${PHOSPHORWORKSPACES_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/PhosphorWorkspacesConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/PhosphorWorkspacesConfigVersion.cmake"
    DESTINATION ${KDE_INSTALL_LIBDIR}/cmake/PhosphorWorkspaces
)

# The workspace-map, reconciler, ledger and VirtualDesktopManager suites need
# nothing beyond Qt, so the tree is entered unconditionally. The ActivityManager
# suite inside it gates itself on the activities library.
if(CMAKE_PROJECT_NAME STREQUAL "PhosphorWorkspaces" OR BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()
