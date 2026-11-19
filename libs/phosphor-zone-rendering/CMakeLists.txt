# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# PhosphorZoneRendering — zone overlays drawn with shader packs
#
# ZoneShaderItem, the Qt Quick item that renders a zone-overlay shader pack
# over a layout's zones through PhosphorRendering, with the pack entry
# scaffold, the include-path and vertex-stage resolution shared with the warm
# bake, and the zone-label texture. The engine runtime's overlays draw with it,
# and so do a settings app's previews and the pack tooling.

cmake_minimum_required(VERSION 3.16)

set(PHOSPHORZONERENDERING_VERSION "0.1.0")

if(DEFINED QT_MIN_VERSION)
    set(PHOSPHORZONERENDERING_QT_MIN ${QT_MIN_VERSION})
else()
    set(PHOSPHORZONERENDERING_QT_MIN "6.10.0")
endif()

if(NOT PROJECT_VERSION)
    project(PhosphorZoneRendering VERSION ${PHOSPHORZONERENDERING_VERSION} LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

include(GenerateExportHeader)

find_package(Qt6 ${PHOSPHORZONERENDERING_QT_MIN} REQUIRED COMPONENTS Core Gui Quick)
set(CMAKE_AUTOMOC ON)

if(NOT TARGET PhosphorEngineRuntime::PhosphorEngineRuntime)
    find_package(PhosphorEngineRuntime CONFIG REQUIRED)
endif()
if(NOT TARGET PhosphorRendering::PhosphorRendering)
    find_package(PhosphorRendering CONFIG REQUIRED)
endif()
if(NOT TARGET PhosphorShaders::PhosphorShaders)
    find_package(PhosphorShaders CONFIG REQUIRED)
endif()
if(NOT TARGET PhosphorZones::PhosphorZones)
    find_package(PhosphorZones CONFIG REQUIRED)
endif()

set(phosphorzonerendering_public_HDRS
    include/PhosphorZoneRendering/ZoneEntryScaffold.h
    include/PhosphorZoneRendering/ZoneLabelTextureBuilder.h
    include/PhosphorZoneRendering/ZoneShaderItem.h
    include/PhosphorZoneRendering/ZoneShaderNodeRhi.h
)

set(phosphorzonerendering_SRCS
    src/zoneentryscaffold.cpp
    src/zonelabeltexturebuilder.cpp
    src/zonerenderinglogging.cpp
    src/zonerenderinglogging.h
    src/zoneshaderitem.cpp
    src/zoneshaderitem_setters.cpp
    src/zoneshadernoderhi.cpp
)

add_library(PhosphorZoneRendering SHARED
    ${phosphorzonerendering_public_HDRS}
    ${phosphorzonerendering_SRCS}
)
add_library(PhosphorZoneRendering::PhosphorZoneRendering ALIAS PhosphorZoneRendering)

generate_export_header(PhosphorZoneRendering
    EXPORT_FILE_NAME phosphorzonerendering_export.h
    EXPORT_MACRO_NAME PHOSPHORZONERENDERING_EXPORT
)

if(NOT DEFINED KDE_INSTALL_INCLUDEDIR)
    include(GNUInstallDirs)
    set(KDE_INSTALL_INCLUDEDIR ${CMAKE_INSTALL_INCLUDEDIR})
endif()

target_include_directories(PhosphorZoneRendering
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
        $<INSTALL_INTERFACE:${KDE_INSTALL_INCLUDEDIR}>
    PRIVATE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)

target_compile_features(PhosphorZoneRendering PUBLIC cxx_std_20)

# PUBLIC: ZoneShaderItem is a PhosphorRendering::ShaderEffect, its headers
# return PhosphorShaders entry candidates and bake results, and the include
# paths come from the engine runtime's ConfigDefaults.
target_link_libraries(PhosphorZoneRendering
    PUBLIC
        Qt6::Core
        Qt6::Gui
        Qt6::Quick
        PhosphorEngineRuntime::PhosphorEngineRuntime
        PhosphorRendering::PhosphorRendering
        PhosphorShaders::PhosphorShaders
    PRIVATE
        PhosphorZones::PhosphorZones
)

set_target_properties(PhosphorZoneRendering PROPERTIES
    VERSION ${PHOSPHORZONERENDERING_VERSION}
    SOVERSION 0
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

if(NOT DEFINED KDE_INSTALL_LIBDIR)
    include(GNUInstallDirs)
    set(KDE_INSTALL_LIBDIR ${CMAKE_INSTALL_LIBDIR})
endif()
if(NOT DEFINED KDE_INSTALL_BINDIR)
    include(GNUInstallDirs)
    set(KDE_INSTALL_BINDIR ${CMAKE_INSTALL_BINDIR})
endif()

install(TARGETS PhosphorZoneRendering
    EXPORT PhosphorZoneRenderingTargets
    LIBRARY DESTINATION ${KDE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${KDE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${KDE_INSTALL_BINDIR}
)
install(EXPORT PhosphorZoneRenderingTargets
    FILE PhosphorZoneRenderingTargets.cmake
    NAMESPACE PhosphorZoneRendering::
    DESTINATION ${KDE_INSTALL_LIBDIR}/cmake/PhosphorZoneRendering
)
include(CMakePackageConfigHelpers)
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/PhosphorZoneRenderingConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/PhosphorZoneRenderingConfig.cmake"
    INSTALL_DESTINATION ${KDE_INSTALL_LIBDIR}/cmake/PhosphorZoneRendering
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/PhosphorZoneRenderingConfigVersion.cmake"
    VERSION ${PHOSPHORZONERENDERING_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/PhosphorZoneRenderingConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/PhosphorZoneRenderingConfigVersion.cmake"
    DESTINATION ${KDE_INSTALL_LIBDIR}/cmake/PhosphorZoneRendering
)
install(DIRECTORY include/PhosphorZoneRendering
    DESTINATION ${KDE_INSTALL_INCLUDEDIR}
)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/phosphorzonerendering_export.h"
    DESTINATION ${KDE_INSTALL_INCLUDEDIR}/PhosphorZoneRendering
)

if(CMAKE_PROJECT_NAME STREQUAL "PhosphorZoneRendering" OR BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()
