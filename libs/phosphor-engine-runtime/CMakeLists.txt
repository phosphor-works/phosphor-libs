# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# PhosphorEngineRuntime — the placement engine runtime a host embeds
#
# The settings model (the schema and its host extension, the store-backed
# Settings and its interfaces, the defaults, keys and migrations) and the core
# the runtime is built on (its interfaces, types, resolvers, geometry and
# layout helpers, logging and translation catalog). The runtime's services,
# controllers and overlays follow it here area by area. Built standalone or as a subdirectory
# of Phosphor.

cmake_minimum_required(VERSION 3.16)

set(PHOSPHORENGINERUNTIME_VERSION "0.1.0")

# The Qt floor, from the top-level QT_MIN_VERSION in-tree so the two cannot
# drift, and its own value standalone, where that variable does not exist.
if(DEFINED QT_MIN_VERSION)
    set(PHOSPHORENGINERUNTIME_QT_MIN ${QT_MIN_VERSION})
else()
    set(PHOSPHORENGINERUNTIME_QT_MIN "6.10.0")
endif()

if(NOT PROJECT_VERSION)
    project(PhosphorEngineRuntime VERSION ${PHOSPHORENGINERUNTIME_VERSION} LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

include(GenerateExportHeader)

# ═══════════════════════════════════════════════════════════════════════════════
# Dependencies
# ═══════════════════════════════════════════════════════════════════════════════

find_package(Qt6 ${PHOSPHORENGINERUNTIME_QT_MIN} REQUIRED COMPONENTS Core Gui)
set(CMAKE_AUTOMOC ON)

if(NOT TARGET PhosphorAnimation::PhosphorAnimation)
    find_package(PhosphorAnimation CONFIG REQUIRED)
endif()
if(NOT TARGET PhosphorCompositor::PhosphorCompositor)
    find_package(PhosphorCompositor CONFIG REQUIRED)
endif()
if(NOT TARGET PhosphorConfig::PhosphorConfig)
    find_package(PhosphorConfig CONFIG REQUIRED)
endif()
if(NOT TARGET PhosphorEngine::PhosphorEngine)
    find_package(PhosphorEngine CONFIG REQUIRED)
endif()
if(NOT TARGET PhosphorIdentity::PhosphorIdentity)
    find_package(PhosphorIdentity CONFIG REQUIRED)
endif()
if(NOT TARGET PhosphorLayoutApi::PhosphorLayoutApi)
    find_package(PhosphorLayoutApi CONFIG REQUIRED)
endif()
if(NOT TARGET PhosphorProtocol::PhosphorProtocol)
    find_package(PhosphorProtocol CONFIG REQUIRED)
endif()
if(NOT TARGET PhosphorRegistry::PhosphorRegistry)
    find_package(PhosphorRegistry CONFIG REQUIRED)
endif()
if(NOT TARGET PhosphorRules::PhosphorRules)
    find_package(PhosphorRules CONFIG REQUIRED)
endif()
if(NOT TARGET PhosphorScreens::PhosphorScreens)
    find_package(PhosphorScreens CONFIG REQUIRED)
endif()
if(NOT TARGET PhosphorScrollEngine::PhosphorScrollEngine)
    find_package(PhosphorScrollEngine CONFIG REQUIRED)
endif()
if(NOT TARGET PhosphorShaders::PhosphorShaders)
    find_package(PhosphorShaders CONFIG REQUIRED)
endif()
if(NOT TARGET PhosphorSnapEngine::PhosphorSnapEngine)
    find_package(PhosphorSnapEngine CONFIG REQUIRED)
endif()
if(NOT TARGET PhosphorSurface::PhosphorSurface)
    find_package(PhosphorSurface CONFIG REQUIRED)
endif()
if(NOT TARGET PhosphorTileEngine::PhosphorTileEngine)
    find_package(PhosphorTileEngine CONFIG REQUIRED)
endif()
if(NOT TARGET PhosphorTiles::PhosphorTiles)
    find_package(PhosphorTiles CONFIG REQUIRED)
endif()
if(NOT TARGET PhosphorAudio::PhosphorAudio)
    find_package(PhosphorAudio CONFIG REQUIRED)
endif()
if(NOT TARGET PhosphorGeometry::PhosphorGeometry)
    find_package(PhosphorGeometry CONFIG REQUIRED)
endif()
if(NOT TARGET PhosphorPlacement::PhosphorPlacement)
    find_package(PhosphorPlacement CONFIG REQUIRED)
endif()
if(NOT TARGET PhosphorWorkspaces::PhosphorWorkspaces)
    find_package(PhosphorWorkspaces CONFIG REQUIRED)
endif()
if(NOT TARGET PhosphorZones::PhosphorZones)
    find_package(PhosphorZones CONFIG REQUIRED)
endif()

# ═══════════════════════════════════════════════════════════════════════════════
# Library
# ═══════════════════════════════════════════════════════════════════════════════

set(phosphorengineruntime_public_HDRS
    include/PhosphorEngineRuntime/AnimationBootstrap.h
    include/PhosphorEngineRuntime/AnimationShaderSupportedPaths.h
    include/PhosphorEngineRuntime/AppDirectoryName.h
    include/PhosphorEngineRuntime/CavaOptions.h
    include/PhosphorEngineRuntime/ConfigBackends.h
    include/PhosphorEngineRuntime/ConfigDefaults.h
    include/PhosphorEngineRuntime/ConfigDefaultsAppearance.h
    include/PhosphorEngineRuntime/ConfigDefaultsGaps.h
    include/PhosphorEngineRuntime/ConfigDefaultsLimits.h
    include/PhosphorEngineRuntime/ConfigDefaultsScreens.h
    include/PhosphorEngineRuntime/ConfigDefaultsScrolling.h
    include/PhosphorEngineRuntime/ConfigDefaultsScrollingBehavior.h
    include/PhosphorEngineRuntime/ConfigDefaultsScrollingShortcuts.h
    include/PhosphorEngineRuntime/ConfigDefaultsShaders.h
    include/PhosphorEngineRuntime/ConfigDefaultsWorkspaces.h
    include/PhosphorEngineRuntime/ConfigKeyMacro.h
    include/PhosphorEngineRuntime/ConfigKeyMacroUndef.h
    include/PhosphorEngineRuntime/ConfigKeys.h
    include/PhosphorEngineRuntime/ConfigKeysScrolling.h
    include/PhosphorEngineRuntime/ConfigKeysWorkspaces.h
    include/PhosphorEngineRuntime/ConfigLogging.h
    include/PhosphorEngineRuntime/ConfigMigration.h
    include/PhosphorEngineRuntime/ConfigMigrationUtil.h
    include/PhosphorEngineRuntime/Constants.h
    include/PhosphorEngineRuntime/CrossSurfaceResolver.h
    include/PhosphorEngineRuntime/DaemonGeometryResolver.h
    include/PhosphorEngineRuntime/DmabufThumbnail.h
    include/PhosphorEngineRuntime/Enums.h
    include/PhosphorEngineRuntime/GeometryUtils.h
    include/PhosphorEngineRuntime/IOverlayService.h
    include/PhosphorEngineRuntime/ISettings.h
    include/PhosphorEngineRuntime/Interfaces.h
    include/PhosphorEngineRuntime/LayoutBundleBuilder.h
    include/PhosphorEngineRuntime/LayoutPreviewSerialize.h
    include/PhosphorEngineRuntime/Logging.h
    include/PhosphorEngineRuntime/OverlayShaderTree.h
    include/PhosphorEngineRuntime/PerScreenResolver.h
    include/PhosphorEngineRuntime/RuntimeI18n.h
    include/PhosphorEngineRuntime/ScreenIdResolver.h
    include/PhosphorEngineRuntime/ScreenModeRouter.h
    include/PhosphorEngineRuntime/ScreenScale.h
    include/PhosphorEngineRuntime/Settings.h
    include/PhosphorEngineRuntime/SettingsConfigStore.h
    include/PhosphorEngineRuntime/SettingsInterfaces.h
    include/PhosphorEngineRuntime/SettingsSchema.h
    include/PhosphorEngineRuntime/SettingsSchemaChoices.h
    include/PhosphorEngineRuntime/ShaderRegistry.h
    include/PhosphorEngineRuntime/SpatialAdjacency.h
    include/PhosphorEngineRuntime/StripCardSerialize.h
    include/PhosphorEngineRuntime/StripSelectorHitTest.h
    include/PhosphorEngineRuntime/Types.h
    include/PhosphorEngineRuntime/UnifiedLayoutList.h
    include/PhosphorEngineRuntime/Utils.h
    include/PhosphorEngineRuntime/ZoneSelectorLayout.h
)

set(phosphorengineruntime_SRCS
    src/common/layoutbundlebuilder.cpp
    src/common/layoutpreviewserialize.cpp
    src/common/screenidresolver.cpp
    src/common/stripcardserialize.cpp
    src/config/configbackends.cpp
    src/config/configdefaults.cpp
    src/config/configkeys_disablelabels.h
    src/config/configlogging.cpp
    src/config/configmigration.cpp
    src/config/configmigration_v2.cpp
    src/config/configmigration_v3.cpp
    src/config/configmigration_v4.cpp
    src/config/configmigration_v4detail.h
    src/config/configmigration_v4finalize.cpp
    src/config/configmigration_v4rules.cpp
    src/config/configmigration_v5.cpp
    src/config/configmigration_v6.cpp
    src/config/configmigration_v7.cpp
    src/config/configmigration_v8.cpp
    src/config/configmigration_v8overlay.cpp
    src/config/perscreenresolver.cpp
    src/config/settings.cpp
    src/config/settings/animationprofile.cpp
    src/config/settings/disable.cpp
    src/config/settings/loadsave.cpp
    src/config/settings/perscreen.cpp
    src/config/settings/perscreen_detail.h
    src/config/settings/perscreen_selector.cpp
    src/config/settings/profiletrees.cpp
    src/config/settings/scrolling.cpp
    src/config/settings/setters.cpp
    src/config/settings/settings_detail.h
    src/config/settings/shortcuts.cpp
    src/config/settings/storescalars.cpp
    src/config/settings/systemcolors.cpp
    src/config/settings/triggers.cpp
    src/config/settings/uienums.cpp
    src/config/settings/workspaces.cpp
    src/config/settingsconfigstore.cpp
    src/config/settingsschema.cpp
    src/config/settingsschema_overlayshaders.cpp
    src/config/settingsschema_p.h
    src/config/settingsschema_scrolling.cpp
    src/config/settingsschema_shaderbounds.cpp
    src/config/settingsschema_shaderbounds.h
    src/config/settingsschema_shadertrees.cpp
    src/config/settingsschema_tiling.cpp
    src/config/settingsschema_workspaces.cpp
    src/core/interfaces/interfaces.cpp
    src/core/interfaces/isettings.cpp
    src/core/platform/appdirectoryname.cpp
    src/core/platform/logging.cpp
    src/core/resolve/animationbootstrap.cpp
    src/core/resolve/crosssurfaceresolver.cpp
    src/core/resolve/daemongeometryresolver.cpp
    src/core/resolve/screenmoderouter.cpp
    src/core/types/overlayshadertree.cpp
    src/core/utils/geometryutils.cpp
    src/core/utils/unifiedlayoutlist.cpp
    src/core/utils/utils.cpp
    src/runtime/common/runtimei18n.cpp
)

add_library(PhosphorEngineRuntime SHARED
    ${phosphorengineruntime_public_HDRS}
    ${phosphorengineruntime_SRCS}
)
add_library(PhosphorEngineRuntime::PhosphorEngineRuntime ALIAS PhosphorEngineRuntime)

generate_export_header(PhosphorEngineRuntime
    EXPORT_FILE_NAME phosphorengineruntime_export.h
    EXPORT_MACRO_NAME PHOSPHORENGINERUNTIME_EXPORT
)

if(NOT DEFINED KDE_INSTALL_INCLUDEDIR)
    include(GNUInstallDirs)
    set(KDE_INSTALL_INCLUDEDIR ${CMAKE_INSTALL_INCLUDEDIR})
endif()

target_include_directories(PhosphorEngineRuntime
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
        $<INSTALL_INTERFACE:${KDE_INSTALL_INCLUDEDIR}>
    PRIVATE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)

target_compile_features(PhosphorEngineRuntime PUBLIC cxx_std_20)

# PUBLIC: the public headers expose each of these libraries' types (the
# settings interfaces return their profiles, trees, rule stores and engine
# settings by value or reference).
target_link_libraries(PhosphorEngineRuntime
    PUBLIC
        Qt6::Core
        Qt6::Gui
        PhosphorAnimation::PhosphorAnimation
        PhosphorCompositor::PhosphorCompositor
        PhosphorConfig::PhosphorConfig
        PhosphorEngine::PhosphorEngine
        PhosphorIdentity::PhosphorIdentity
        PhosphorLayoutApi::PhosphorLayoutApi
        PhosphorProtocol::PhosphorProtocol
        PhosphorRegistry::PhosphorRegistry
        PhosphorRules::PhosphorRules
        PhosphorScreens::PhosphorScreens
        PhosphorScrollEngine::PhosphorScrollEngine
        PhosphorShaders::PhosphorShaders
        PhosphorSnapEngine::PhosphorSnapEngine
        PhosphorSurface::PhosphorSurface
        PhosphorTileEngine::PhosphorTileEngine
        PhosphorTiles::PhosphorTiles
        PhosphorAudio::PhosphorAudio
        PhosphorGeometry::PhosphorGeometry
        PhosphorPlacement::PhosphorPlacement
        PhosphorWorkspaces::PhosphorWorkspaces
        PhosphorZones::PhosphorZones
)

set_target_properties(PhosphorEngineRuntime PROPERTIES
    VERSION ${PHOSPHORENGINERUNTIME_VERSION}
    SOVERSION 0
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

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

install(TARGETS PhosphorEngineRuntime
    EXPORT PhosphorEngineRuntimeTargets
    LIBRARY DESTINATION ${KDE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${KDE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${KDE_INSTALL_BINDIR}
)

install(EXPORT PhosphorEngineRuntimeTargets
    FILE PhosphorEngineRuntimeTargets.cmake
    NAMESPACE PhosphorEngineRuntime::
    DESTINATION ${KDE_INSTALL_LIBDIR}/cmake/PhosphorEngineRuntime
)

include(CMakePackageConfigHelpers)
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/PhosphorEngineRuntimeConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/PhosphorEngineRuntimeConfig.cmake"
    INSTALL_DESTINATION ${KDE_INSTALL_LIBDIR}/cmake/PhosphorEngineRuntime
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/PhosphorEngineRuntimeConfigVersion.cmake"
    VERSION ${PHOSPHORENGINERUNTIME_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/PhosphorEngineRuntimeConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/PhosphorEngineRuntimeConfigVersion.cmake"
    DESTINATION ${KDE_INSTALL_LIBDIR}/cmake/PhosphorEngineRuntime
)

install(DIRECTORY include/PhosphorEngineRuntime
    DESTINATION ${KDE_INSTALL_INCLUDEDIR}
)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/phosphorengineruntime_export.h"
    DESTINATION ${KDE_INSTALL_INCLUDEDIR}/PhosphorEngineRuntime
)

# ═══════════════════════════════════════════════════════════════════════════════
# Tests
# ═══════════════════════════════════════════════════════════════════════════════

if(CMAKE_PROJECT_NAME STREQUAL "PhosphorEngineRuntime" OR BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()
