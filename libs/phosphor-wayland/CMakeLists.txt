# SPDX-FileCopyrightText: 2026 Phosphor Works Team
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# PhosphorWayland — Wayland layer-shell integration (wlr-layer-shell-v1)
#
# A Qt6/C++20 library that wraps zwlr_layer_shell_v1 behind a Qt-friendly
# QPA shell-integration plugin (`phosphorwayland-qpa`) plus a thin
# `LayerSurface` helper for setting layer-surface properties on a
# QWindow. Shader-domain headers live in the sibling
# `phosphor-shaders` library.
#
# Can be built standalone or as a subdirectory of Phosphor.

cmake_minimum_required(VERSION 3.16)

# Library version — used for SOVERSION and as the project() version when
# built standalone.
set(PHOSPHORWAYLAND_VERSION "0.1.0")

# The Qt floor this library requires AND advertises in its generated
# PhosphorWaylandConfig.cmake. Taken from the top-level QT_MIN_VERSION when built
# in-tree so the two cannot drift, with its own value for a STANDALONE build, where
# that variable does not exist. The fallback is not optional: the config below
# substitutes this, and an undefined variable expands to nothing and ships a config
# whose find_dependency(Qt6 ...) carries no version floor at all.
if(DEFINED QT_MIN_VERSION)
    set(PHOSPHORWAYLAND_QT_MIN ${QT_MIN_VERSION})
else()
    set(PHOSPHORWAYLAND_QT_MIN "6.10.0")
endif()

# When built as a subdirectory, inherit the parent project's settings.
# When standalone, set our own.
if(NOT PROJECT_VERSION)
    project(PhosphorWayland VERSION ${PHOSPHORWAYLAND_VERSION} LANGUAGES C CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_AUTOMOC ON)
endif()

include(GenerateExportHeader)

# ═══════════════════════════════════════════════════════════════════════════════
# Dependencies
# ═══════════════════════════════════════════════════════════════════════════════

# These are likely already found by the parent project when built in-tree.
find_package(Qt6 ${PHOSPHORWAYLAND_QT_MIN} REQUIRED COMPONENTS Core Gui)

if(NOT TARGET Qt6::GuiPrivate)
    find_package(Qt6GuiPrivate CONFIG REQUIRED)
endif()
find_package(Qt6 ${PHOSPHORWAYLAND_QT_MIN} REQUIRED COMPONENTS WaylandClient)
if(NOT TARGET Qt6::WaylandClientPrivate)
    find_package(Qt6WaylandClientPrivate REQUIRED)
endif()
find_package(Wayland REQUIRED COMPONENTS Client)
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

# ═══════════════════════════════════════════════════════════════════════════════
# Wayland Protocol Code Generation
# ═══════════════════════════════════════════════════════════════════════════════

set(_ps_proto_dir ${CMAKE_CURRENT_SOURCE_DIR}/protocols)
set(_ps_gen_dir ${CMAKE_CURRENT_BINARY_DIR}/generated-protocols)
file(MAKE_DIRECTORY ${_ps_gen_dir})

add_custom_command(
    OUTPUT ${_ps_gen_dir}/wlr-layer-shell-unstable-v1-client-protocol.h
           ${_ps_gen_dir}/wlr-layer-shell-unstable-v1-protocol.c
    COMMAND ${WAYLAND_SCANNER} client-header
            ${_ps_proto_dir}/wlr-layer-shell-unstable-v1.xml
            ${_ps_gen_dir}/wlr-layer-shell-unstable-v1-client-protocol.h
    COMMAND ${WAYLAND_SCANNER} private-code
            ${_ps_proto_dir}/wlr-layer-shell-unstable-v1.xml
            ${_ps_gen_dir}/wlr-layer-shell-unstable-v1-protocol.c
    DEPENDS ${_ps_proto_dir}/wlr-layer-shell-unstable-v1.xml
    COMMENT "PhosphorWayland: generating wlr-layer-shell protocol code"
)

add_custom_command(
    OUTPUT ${_ps_gen_dir}/single-pixel-buffer-v1-client-protocol.h
           ${_ps_gen_dir}/single-pixel-buffer-v1-protocol.c
    COMMAND ${WAYLAND_SCANNER} client-header
            ${_ps_proto_dir}/single-pixel-buffer-v1.xml
            ${_ps_gen_dir}/single-pixel-buffer-v1-client-protocol.h
    COMMAND ${WAYLAND_SCANNER} private-code
            ${_ps_proto_dir}/single-pixel-buffer-v1.xml
            ${_ps_gen_dir}/single-pixel-buffer-v1-protocol.c
    DEPENDS ${_ps_proto_dir}/single-pixel-buffer-v1.xml
    COMMENT "PhosphorWayland: generating single-pixel-buffer protocol code"
)

add_custom_command(
    OUTPUT ${_ps_gen_dir}/ext-idle-notify-v1-client-protocol.h
           ${_ps_gen_dir}/ext-idle-notify-v1-protocol.c
    COMMAND ${WAYLAND_SCANNER} client-header
            ${_ps_proto_dir}/ext-idle-notify-v1.xml
            ${_ps_gen_dir}/ext-idle-notify-v1-client-protocol.h
    COMMAND ${WAYLAND_SCANNER} private-code
            ${_ps_proto_dir}/ext-idle-notify-v1.xml
            ${_ps_gen_dir}/ext-idle-notify-v1-protocol.c
    DEPENDS ${_ps_proto_dir}/ext-idle-notify-v1.xml
    COMMENT "PhosphorWayland: generating ext-idle-notify protocol code"
)

add_custom_command(
    OUTPUT ${_ps_gen_dir}/xdg-toplevel-drag-v1-client-protocol.h
           ${_ps_gen_dir}/xdg-toplevel-drag-v1-protocol.c
    COMMAND ${WAYLAND_SCANNER} client-header
            ${_ps_proto_dir}/xdg-toplevel-drag-v1.xml
            ${_ps_gen_dir}/xdg-toplevel-drag-v1-client-protocol.h
    COMMAND ${WAYLAND_SCANNER} private-code
            ${_ps_proto_dir}/xdg-toplevel-drag-v1.xml
            ${_ps_gen_dir}/xdg-toplevel-drag-v1-protocol.c
    DEPENDS ${_ps_proto_dir}/xdg-toplevel-drag-v1.xml
    COMMENT "PhosphorWayland: generating xdg-toplevel-drag protocol code"
)

add_custom_command(
    OUTPUT ${_ps_gen_dir}/wlr-foreign-toplevel-management-unstable-v1-client-protocol.h
           ${_ps_gen_dir}/wlr-foreign-toplevel-management-unstable-v1-protocol.c
    COMMAND ${WAYLAND_SCANNER} client-header
            ${_ps_proto_dir}/wlr-foreign-toplevel-management-unstable-v1.xml
            ${_ps_gen_dir}/wlr-foreign-toplevel-management-unstable-v1-client-protocol.h
    COMMAND ${WAYLAND_SCANNER} private-code
            ${_ps_proto_dir}/wlr-foreign-toplevel-management-unstable-v1.xml
            ${_ps_gen_dir}/wlr-foreign-toplevel-management-unstable-v1-protocol.c
    DEPENDS ${_ps_proto_dir}/wlr-foreign-toplevel-management-unstable-v1.xml
    COMMENT "PhosphorWayland: generating wlr-foreign-toplevel-management protocol code"
)

add_custom_command(
    OUTPUT ${_ps_gen_dir}/zwp-idle-inhibit-v1-client-protocol.h
           ${_ps_gen_dir}/zwp-idle-inhibit-v1-protocol.c
    COMMAND ${WAYLAND_SCANNER} client-header
            ${_ps_proto_dir}/zwp-idle-inhibit-v1.xml
            ${_ps_gen_dir}/zwp-idle-inhibit-v1-client-protocol.h
    COMMAND ${WAYLAND_SCANNER} private-code
            ${_ps_proto_dir}/zwp-idle-inhibit-v1.xml
            ${_ps_gen_dir}/zwp-idle-inhibit-v1-protocol.c
    DEPENDS ${_ps_proto_dir}/zwp-idle-inhibit-v1.xml
    COMMENT "PhosphorWayland: generating zwp-idle-inhibit protocol code"
)

add_custom_command(
    OUTPUT ${_ps_gen_dir}/ext-data-control-v1-client-protocol.h
           ${_ps_gen_dir}/ext-data-control-v1-protocol.c
    COMMAND ${WAYLAND_SCANNER} client-header
            ${_ps_proto_dir}/ext-data-control-v1.xml
            ${_ps_gen_dir}/ext-data-control-v1-client-protocol.h
    COMMAND ${WAYLAND_SCANNER} private-code
            ${_ps_proto_dir}/ext-data-control-v1.xml
            ${_ps_gen_dir}/ext-data-control-v1-protocol.c
    DEPENDS ${_ps_proto_dir}/ext-data-control-v1.xml
    COMMENT "PhosphorWayland: generating ext-data-control protocol code"
)

add_custom_command(
    OUTPUT ${_ps_gen_dir}/wlr-data-control-unstable-v1-client-protocol.h
           ${_ps_gen_dir}/wlr-data-control-unstable-v1-protocol.c
    COMMAND ${WAYLAND_SCANNER} client-header
            ${_ps_proto_dir}/wlr-data-control-unstable-v1.xml
            ${_ps_gen_dir}/wlr-data-control-unstable-v1-client-protocol.h
    COMMAND ${WAYLAND_SCANNER} private-code
            ${_ps_proto_dir}/wlr-data-control-unstable-v1.xml
            ${_ps_gen_dir}/wlr-data-control-unstable-v1-protocol.c
    DEPENDS ${_ps_proto_dir}/wlr-data-control-unstable-v1.xml
    COMMENT "PhosphorWayland: generating wlr-data-control protocol code"
)

add_custom_command(
    OUTPUT ${_ps_gen_dir}/ext-session-lock-v1-client-protocol.h
           ${_ps_gen_dir}/ext-session-lock-v1-protocol.c
    COMMAND ${WAYLAND_SCANNER} client-header
            ${_ps_proto_dir}/ext-session-lock-v1.xml
            ${_ps_gen_dir}/ext-session-lock-v1-client-protocol.h
    COMMAND ${WAYLAND_SCANNER} private-code
            ${_ps_proto_dir}/ext-session-lock-v1.xml
            ${_ps_gen_dir}/ext-session-lock-v1-protocol.c
    DEPENDS ${_ps_proto_dir}/ext-session-lock-v1.xml
    COMMENT "PhosphorWayland: generating ext-session-lock protocol code"
)

add_custom_command(
    OUTPUT ${_ps_gen_dir}/ext-background-effect-v1-client-protocol.h
           ${_ps_gen_dir}/ext-background-effect-v1-protocol.c
    COMMAND ${WAYLAND_SCANNER} client-header
            ${_ps_proto_dir}/ext-background-effect-v1.xml
            ${_ps_gen_dir}/ext-background-effect-v1-client-protocol.h
    COMMAND ${WAYLAND_SCANNER} private-code
            ${_ps_proto_dir}/ext-background-effect-v1.xml
            ${_ps_gen_dir}/ext-background-effect-v1-protocol.c
    DEPENDS ${_ps_proto_dir}/ext-background-effect-v1.xml
    COMMENT "PhosphorWayland: generating ext-background-effect protocol code"
)

# Generated C code: skip PCH and unity build
set_source_files_properties(
    ${_ps_gen_dir}/wlr-layer-shell-unstable-v1-protocol.c
    ${_ps_gen_dir}/single-pixel-buffer-v1-protocol.c
    ${_ps_gen_dir}/ext-idle-notify-v1-protocol.c
    ${_ps_gen_dir}/xdg-toplevel-drag-v1-protocol.c
    ${_ps_gen_dir}/wlr-foreign-toplevel-management-unstable-v1-protocol.c
    ${_ps_gen_dir}/zwp-idle-inhibit-v1-protocol.c
    ${_ps_gen_dir}/wlr-data-control-unstable-v1-protocol.c
    ${_ps_gen_dir}/ext-data-control-v1-protocol.c
    ${_ps_gen_dir}/ext-session-lock-v1-protocol.c
    ${_ps_gen_dir}/ext-background-effect-v1-protocol.c
    src/qpa/xdg_popup_stub.c
    src/qpa/xdg_toplevel_stub.c
    PROPERTIES
        SKIP_PRECOMPILE_HEADERS ON
        SKIP_UNITY_BUILD_INCLUSION ON
)

# ═══════════════════════════════════════════════════════════════════════════════
# PhosphorWayland Library
# ═══════════════════════════════════════════════════════════════════════════════

# --- Public headers with Q_OBJECT (must be listed for AUTOMOC) ---
set(phosphorwayland_public_HDRS
    include/PhosphorWayland/LayerSurface.h
    include/PhosphorWayland/SinglePixelBuffer.h
    include/PhosphorWayland/IdleNotifier.h
    include/PhosphorWayland/ToplevelDrag.h
    include/PhosphorWayland/ForeignToplevel.h
    include/PhosphorWayland/IdleInhibitor.h
    include/PhosphorWayland/CompositorLost.h
    include/PhosphorWayland/ClipboardDevice.h
    include/PhosphorWayland/SessionLock.h
    include/PhosphorWayland/LockSurface.h
    include/PhosphorWayland/BackgroundEffect.h
)

# --- Layer-shell sources ---
set(phosphorwayland_layershell_SRCS
    src/layersurface.cpp
    src/singlepixelbuffer.cpp
    src/idlenotifier.cpp
    src/topleveldrag.cpp
    src/foreigntoplevel.cpp
    src/idleinhibitor.cpp
    src/surfaceidentity.cpp
    src/clipboarddevice.cpp
    src/sessionlock.cpp
    src/locksurface.cpp
    src/compositorlost.cpp
    src/compositorlost_internal.h
    src/backgroundeffect.cpp
    src/backgroundeffect_internal.h
    src/qpa/layershellintegration.cpp
    src/qpa/layershellintegration.h
    src/qpa/layershellwindow.cpp
    src/qpa/layershellwindow.h
    src/qpa/sessionlockwindow.cpp
    src/qpa/sessionlockwindow.h
    src/qpa/xdg_popup_stub.c
    src/qpa/xdg_toplevel_stub.c
    ${_ps_gen_dir}/wlr-layer-shell-unstable-v1-protocol.c
    ${_ps_gen_dir}/single-pixel-buffer-v1-protocol.c
    ${_ps_gen_dir}/ext-idle-notify-v1-protocol.c
    ${_ps_gen_dir}/xdg-toplevel-drag-v1-protocol.c
    ${_ps_gen_dir}/wlr-foreign-toplevel-management-unstable-v1-protocol.c
    ${_ps_gen_dir}/zwp-idle-inhibit-v1-protocol.c
    ${_ps_gen_dir}/wlr-data-control-unstable-v1-protocol.c
    ${_ps_gen_dir}/ext-data-control-v1-protocol.c
    ${_ps_gen_dir}/ext-session-lock-v1-protocol.c
    ${_ps_gen_dir}/ext-background-effect-v1-protocol.c
)

add_library(PhosphorWayland SHARED
    ${phosphorwayland_public_HDRS}
    ${phosphorwayland_layershell_SRCS}
)

add_library(PhosphorWayland::PhosphorWayland ALIAS PhosphorWayland)

generate_export_header(PhosphorWayland
    EXPORT_FILE_NAME ${CMAKE_CURRENT_BINARY_DIR}/phosphorwayland_export.h
    EXPORT_MACRO_NAME PHOSPHORWAYLAND_EXPORT
)

# Resolve KDE_INSTALL_INCLUDEDIR here so INSTALL_INTERFACE can reference it.
# Without this, a consumer that overrides CMAKE_INSTALL_INCLUDEDIR (or the
# KDE-flavoured equivalent) gets an export that hard-codes bare `include/`
# while install(DIRECTORY) below still writes to the overridden location —
# find_package resolves but consumers fail to see the headers.
if(NOT DEFINED KDE_INSTALL_INCLUDEDIR)
    include(GNUInstallDirs)
    set(KDE_INSTALL_INCLUDEDIR ${CMAKE_INSTALL_INCLUDEDIR})
endif()

target_include_directories(PhosphorWayland
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
        $<INSTALL_INTERFACE:${KDE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${_ps_gen_dir}
)

# C++20 as a PUBLIC target feature: CMAKE_CXX_STANDARD is only set by the
# `if(NOT PROJECT_VERSION)` block above — i.e. only when PhosphorWayland is
# the top-level project. When built as a subdirectory of Phosphor (or
# any other parent) consumers inherit whatever standard the parent picked.
# Declare the requirement on the target so find_package(PhosphorWayland)
# consumers get the right compile-feature propagation regardless of build
# configuration.
target_compile_features(PhosphorWayland PUBLIC cxx_std_20)

target_link_libraries(PhosphorWayland
    PUBLIC
        Qt6::Core
        Qt6::Gui
    PRIVATE
        Qt6::GuiPrivate
        Qt6::WaylandClientPrivate
        Wayland::Client
)

set_target_properties(PhosphorWayland PROPERTIES
    VERSION ${PHOSPHORWAYLAND_VERSION}
    SOVERSION 0
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    # Unity builds merge multiple source files into one TU, which surfaces
    # spurious "defined but not used" warnings on anonymous-namespace
    # helpers that GCC can't fully track across the merge. The translation
    # units are small enough that unity buys us nothing here.
    UNITY_BUILD OFF
)

# ═══════════════════════════════════════════════════════════════════════════════
# Tests
# ═══════════════════════════════════════════════════════════════════════════════

if(CMAKE_PROJECT_NAME STREQUAL "PhosphorWayland" OR BUILD_TESTING)
    enable_testing()

    # test_plugin_layout's stand-in for a newer build of this library: the
    # same sources and soname, with LayerShellIntegration grown by a few
    # hundred bytes, in a directory of its own. The test links it, so it is
    # the libPhosphorWayland.so.0 the plugin finds already loaded, as a
    # system plugin finds an application's newer copy. Defined here, not in
    # tests/, because the generated protocol sources belong to this directory.
    add_library(PhosphorWaylandGrownLayout SHARED EXCLUDE_FROM_ALL
        ${phosphorwayland_public_HDRS}
        ${phosphorwayland_layershell_SRCS}
    )
    target_include_directories(PhosphorWaylandGrownLayout
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_BINARY_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${_ps_gen_dir}
    )
    target_compile_features(PhosphorWaylandGrownLayout PUBLIC cxx_std_20)
    target_compile_definitions(PhosphorWaylandGrownLayout PUBLIC PHOSPHORWAYLAND_TEST_LAYOUT_GROWTH=512)
    target_link_libraries(PhosphorWaylandGrownLayout
        PUBLIC
            Qt6::Core
            Qt6::Gui
            Qt6::GuiPrivate
            Qt6::WaylandClientPrivate
            Wayland::Client
    )
    set_target_properties(PhosphorWaylandGrownLayout PROPERTIES
        OUTPUT_NAME PhosphorWayland
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/grown-layout
        DEFINE_SYMBOL PhosphorWayland_EXPORTS
        VERSION ${PHOSPHORWAYLAND_VERSION}
        SOVERSION 0
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
        UNITY_BUILD OFF
    )

    add_subdirectory(tests)
endif()

# ═══════════════════════════════════════════════════════════════════════════════
# QPA Shell Integration Plugin (loaded at runtime by Qt)
# ═══════════════════════════════════════════════════════════════════════════════

add_library(phosphorwayland-qpa MODULE src/qpa/phosphorwaylandplugin.cpp)
target_link_libraries(phosphorwayland-qpa PRIVATE
    PhosphorWayland
    Qt6::WaylandClientPrivate
    Wayland::Client
)
target_include_directories(phosphorwayland-qpa PRIVATE ${_ps_gen_dir})
set_target_properties(phosphorwayland-qpa PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins/wayland-shell-integration
)

# ═══════════════════════════════════════════════════════════════════════════════
# Install
# ═══════════════════════════════════════════════════════════════════════════════

# Use KDE install dirs when available (in-tree build), fall back to GNUInstallDirs
if(NOT DEFINED KDE_INSTALL_LIBDIR)
    include(GNUInstallDirs)
    set(KDE_INSTALL_LIBDIR ${CMAKE_INSTALL_LIBDIR})
endif()

if(NOT DEFINED KDE_INSTALL_DATADIR)
    include(GNUInstallDirs)
    set(KDE_INSTALL_DATADIR ${CMAKE_INSTALL_DATADIR})
endif()

if(NOT DEFINED KDE_INSTALL_BINDIR)
    include(GNUInstallDirs)
    set(KDE_INSTALL_BINDIR ${CMAKE_INSTALL_BINDIR})
endif()

# Library
install(TARGETS PhosphorWayland
    EXPORT PhosphorWaylandTargets
    LIBRARY DESTINATION ${KDE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${KDE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${KDE_INSTALL_BINDIR}
)

# Export the targets file so downstream find_package(PhosphorWayland) works AND
# so dependent libraries (PhosphorRendering) can install their own export sets
# that reference PhosphorWayland::PhosphorWayland. Without this, install(EXPORT) on
# any consumer fails: "target requires target PhosphorWayland that is not in any
# export set."
install(EXPORT PhosphorWaylandTargets
    FILE PhosphorWaylandTargets.cmake
    NAMESPACE PhosphorWayland::
    DESTINATION ${KDE_INSTALL_LIBDIR}/cmake/PhosphorWayland
)

# Generate and install a minimal find_package config so consumers can
# `find_dependency(PhosphorWayland)`.
include(CMakePackageConfigHelpers)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/PhosphorWaylandConfig.cmake.in"
"@PACKAGE_INIT@\n"
"include(CMakeFindDependencyMacro)\n"
"find_dependency(Qt6 ${PHOSPHORWAYLAND_QT_MIN} COMPONENTS Core Gui)\n"
"include(\"\${CMAKE_CURRENT_LIST_DIR}/PhosphorWaylandTargets.cmake\")\n"
"check_required_components(PhosphorWayland)\n"
)
configure_package_config_file(
    "${CMAKE_CURRENT_BINARY_DIR}/PhosphorWaylandConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/PhosphorWaylandConfig.cmake"
    INSTALL_DESTINATION ${KDE_INSTALL_LIBDIR}/cmake/PhosphorWayland
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/PhosphorWaylandConfigVersion.cmake"
    VERSION ${PHOSPHORWAYLAND_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/PhosphorWaylandConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/PhosphorWaylandConfigVersion.cmake"
    DESTINATION ${KDE_INSTALL_LIBDIR}/cmake/PhosphorWayland
)

# Public headers.
#
# The extensionless Qt-style forwarding headers are named explicitly because
# FILES_MATCHING PATTERN "*.h" does not match them. Without this a consumer
# writing #include <PhosphorWayland/LayerSurface> builds in-tree, where
# BUILD_INTERFACE points at the source include directory and the file is
# present, and then fails against the installed package.
install(DIRECTORY include/PhosphorWayland/
    DESTINATION ${KDE_INSTALL_INCLUDEDIR}/PhosphorWayland
    FILES_MATCHING
        PATTERN "*.h"
        PATTERN "IdleNotifier"
        PATTERN "LayerSurface"
        PATTERN "SinglePixelBuffer"
        PATTERN "ToplevelDrag"
)

# Export header
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/phosphorwayland_export.h
    DESTINATION ${KDE_INSTALL_INCLUDEDIR}/PhosphorWayland
)

# QPA plugin
if(NOT DEFINED QT6_INSTALL_PLUGINS)
    get_target_property(QT6_INSTALL_PLUGINS Qt6::Core QT_INSTALL_PLUGINS)
    if(NOT QT6_INSTALL_PLUGINS)
        set(QT6_INSTALL_PLUGINS "${KDE_INSTALL_LIBDIR}/qt6/plugins")
    endif()
endif()
install(TARGETS phosphorwayland-qpa
    DESTINATION ${QT6_INSTALL_PLUGINS}/wayland-shell-integration
)

# The licence notices of the vendored protocols. wayland-scanner's private-code
# compiles each protocol's interface tables into libPhosphorWayland and the QPA
# plugin, and every protocol's <copyright> block asks for its notice to go with
# copies of the software: MIT for most, HPND-sell-variant for the wlroots ones
# and the data-control protocols. LICENSE.wayland-protocols holds each notice
# under the protocol's name and SPDX identifier, in this tree's own licence
# directory beside the Luau and jsoncons notices. Read from the XML at configure
# time, so the file follows the vendored protocols. A protocol whose notice is
# missing, or whose licence is neither of the two, stops the configure rather
# than shipping without its notice (phosphor-works/PlasmaZones#1193).
file(GLOB _ps_proto_files LIST_DIRECTORIES false CONFIGURE_DEPENDS "${_ps_proto_dir}/*.xml")
list(SORT _ps_proto_files)
string(CONCAT _ps_license_text
    "The licence notices of the Wayland protocols that phosphor-wayland compiles into libPhosphorWayland\n"
    "and the phosphorwayland-qpa plugin, from the <copyright> block of each protocol XML in\n"
    "libs/phosphor-wayland/protocols.\n")
foreach(_ps_proto_file IN LISTS _ps_proto_files)
    get_filename_component(_ps_proto_name "${_ps_proto_file}" NAME)
    file(READ "${_ps_proto_file}" _ps_proto_xml)
    string(REGEX MATCH "<copyright>(.*)</copyright>" _ps_proto_match "${_ps_proto_xml}")
    if(NOT _ps_proto_match)
        message(FATAL_ERROR
            "The protocol ${_ps_proto_name} has no <copyright> block, and its licence notice must ship in "
            "LICENSE.wayland-protocols. Add the notice of its upstream to the XML.")
    endif()
    set(_ps_proto_notice "${CMAKE_MATCH_1}")
    # Indented in the XML: drop each line's leading whitespace and the blank
    # lines at either end.
    string(REGEX REPLACE "\n[ \t]+" "\n" _ps_proto_notice "${_ps_proto_notice}")
    string(STRIP "${_ps_proto_notice}" _ps_proto_notice)
    string(REGEX REPLACE "[ \t\n]+" " " _ps_proto_flat "${_ps_proto_notice}")
    if(_ps_proto_flat MATCHES "Permission is hereby granted, free of charge, to any person obtaining a copy")
        set(_ps_proto_spdx "MIT")
    elseif(_ps_proto_flat MATCHES "Permission to use, copy, modify, distribute, and sell this software and its documentation for any purpose is hereby granted without fee")
        set(_ps_proto_spdx "HPND-sell-variant")
    else()
        message(FATAL_ERROR
            "The licence of the protocol ${_ps_proto_name} is neither MIT nor HPND-sell-variant. Name it in "
            "libs/phosphor-wayland/CMakeLists.txt and in the licence lists of the packaging recipes.")
    endif()
    string(APPEND _ps_license_text "\n----\n\n${_ps_proto_name} (${_ps_proto_spdx}):\n\n${_ps_proto_notice}\n")
endforeach()
set(_ps_license_file "${CMAKE_CURRENT_BINARY_DIR}/LICENSE.wayland-protocols")
file(WRITE "${_ps_license_file}" "${_ps_license_text}")
# The XMLs are inputs of the file above: a protocol changed re-runs the
# configure, and the CONFIGURE_DEPENDS glob catches one added or removed.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_ps_proto_files})
install(FILES "${_ps_license_file}" DESTINATION ${KDE_INSTALL_DATADIR}/licenses/phosphor-libs)
# For a project that builds phosphor-wayland in-tree and installs it on its own
# terms instead of through these install rules: the notices it has to ship
# with the library and the plugin.
set(PHOSPHORWAYLAND_LICENSE_FILES "${_ps_license_file}" CACHE INTERNAL
    "Licence notices that must ship with libPhosphorWayland and the phosphorwayland-qpa plugin")

# Protocol XML (for downstream development)
install(DIRECTORY protocols/
    DESTINATION ${KDE_INSTALL_DATADIR}/phosphorwayland/protocols
    FILES_MATCHING PATTERN "*.xml"
)
