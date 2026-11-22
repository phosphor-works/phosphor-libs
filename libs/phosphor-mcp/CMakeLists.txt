# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# PhosphorMcp, a Model Context Protocol server over stdio for Phosphor
# applications. The application declares its tools and answers their calls.
# The library speaks the protocol (revision 2026-07-28), which is JSON-RPC on
# stdin and stdout, the checks of every request, server/discover, tools/list,
# the dispatch of tools/call with argument checks against each tool's schema,
# cancellation, progress, and the end of the session.
#
# Depends on Qt6::Core only. No Gui, no Qml, no Network, so a server that runs
# inside a CLI command pulls in nothing more. The JSON Schema checker of the
# arguments is the library's own (src/mcpschema*.cpp).
# Linux only, since the transport writes a terminal through /proc/self/fd and
# sockets with MSG_NOSIGNAL, and takes back SIGPIPE with sigtimedwait.

# 3.21 floor, as phosphor-control's.
cmake_minimum_required(VERSION 3.21)

set(PHOSPHORMCP_VERSION "0.1.0")

# The Qt floor this library requires AND advertises in its generated
# PhosphorMcpConfig.cmake. Taken from the top-level QT_MIN_VERSION when built
# in-tree so the two cannot drift, with its own value for a STANDALONE build,
# where that variable does not exist.
if(DEFINED QT_MIN_VERSION)
    set(PHOSPHORMCP_QT_MIN ${QT_MIN_VERSION})
else()
    set(PHOSPHORMCP_QT_MIN "6.10.0")
endif()

if(NOT PROJECT_VERSION)
    project(PhosphorMcp VERSION ${PHOSPHORMCP_VERSION} LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    # Stay strictly ISO C++20, no GNU extensions in LGPL public headers.
    set(CMAKE_CXX_EXTENSIONS OFF)
    set(CMAKE_AUTOMOC ON)
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "PhosphorMcp needs Linux (pipe2, sigtimedwait, MSG_NOSIGNAL, /proc/self/fd)")
endif()

include(GenerateExportHeader)

# ═══════════════════════════════════════════════════════════════════════════════
# Dependencies
# ═══════════════════════════════════════════════════════════════════════════════

find_package(Qt6 ${PHOSPHORMCP_QT_MIN} REQUIRED COMPONENTS Core)

# The library checked schemas with valijson once, and with jsoncons for a
# while after that. A build directory from then keeps their entries in its
# cache, where they would do nothing without a word. Each is removed, with a
# warning when it chose something (a switch turned on, a directory set), so
# that a packager does not believe it still does. The entries that only
# kept the result of a check of the headers, or that a development build
# of phosphor-fsloader set to warn once about these, go silently.
# VALIJSON_INCLUDE_DIR was phosphor-fsloader's too, which may have removed
# it first in the tree. Other entries stay: JSONCONS_INCLUDE_DIR, which
# phosphor-fsloader reads in the tree, and those that
# cmake/PhosphorDependencies.cmake set when it fetched valijson
# (PHOSPHOR_DEPS_ARCHIVES, and CPM's and FetchContent's entries,
# CPM_PACKAGE_valijson_* among them), since the tree's other libraries fetch
# through that file. In a build directory of the library alone nothing reads
# them.
foreach(_phosphormcp_old PHOSPHORMCP_USE_SYSTEM_VALIJSON PHOSPHORMCP_VALIJSON_INCLUDE_DIR VALIJSON_INCLUDE_DIR
        PHOSPHORMCP_USE_SYSTEM_JSONCONS)
    if(DEFINED CACHE{${_phosphormcp_old}})
        # AUTO, the default of both switches, is no choice.
        string(TOUPPER "${${_phosphormcp_old}}" _phosphormcp_old_value)
        if(${_phosphormcp_old} AND NOT _phosphormcp_old_value STREQUAL "AUTO")
            message(WARNING
                "${_phosphormcp_old} is set in this build directory's cache, but nothing reads it, because "
                "phosphor-mcp checks tool schemas itself, with no third-party validator. It is removed from the "
                "cache.")
        endif()
        unset(${_phosphormcp_old} CACHE)
    endif()
endforeach()
foreach(_phosphormcp_old _PHOSPHORMCP_VALIJSON_CHECKED_DIR PHOSPHORMCP_VALIJSON_HAS_JSON_POINTER
        _PHOSPHORMCP_JSONCONS_CHECKED_DIR PHOSPHORMCP_JSONCONS_HAS_WALK_STATE
        _PHOSPHORFSLOADER_WARNED_PHOSPHORMCP_USE_SYSTEM_VALIJSON
        _PHOSPHORFSLOADER_WARNED_PHOSPHORMCP_VALIJSON_INCLUDE_DIR)
    unset(${_phosphormcp_old} CACHE)
endforeach()
unset(_phosphormcp_old)
unset(_phosphormcp_old_value)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ═══════════════════════════════════════════════════════════════════════════════
# Library
# ═══════════════════════════════════════════════════════════════════════════════

set(phosphormcp_public_HDRS
    include/PhosphorMcp/PhosphorMcp.h
    include/PhosphorMcp/McpCall.h
    include/PhosphorMcp/McpProtocol.h
    include/PhosphorMcp/McpResult.h
    include/PhosphorMcp/McpServer.h
    include/PhosphorMcp/McpTool.h
    include/PhosphorMcp/McpTransport.h
)

set(phosphormcp_SRCS
    src/mcpcall.cpp
    src/mcplogging.cpp
    src/mcpresult.cpp
    src/mcpschema.cpp
    src/mcpserver.cpp
    src/mcpsignals.cpp
    src/mcptool.cpp
    src/mcptransport.cpp
)

# The JSON Schema checker, built once for the library and for
# test_phosphor_mcp_checkwork, which counts the work of each step and so
# reads the checker's own functions, hidden in the library.
add_library(PhosphorMcpSchemaChecker OBJECT
    src/mcpschemacheck.cpp
    src/mcpschemacompile.cpp
    src/mcpschemareport.cpp
    src/mcpschemavalue.cpp
)
target_compile_features(PhosphorMcpSchemaChecker PUBLIC cxx_std_20)
target_include_directories(PhosphorMcpSchemaChecker PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>)
target_link_libraries(PhosphorMcpSchemaChecker PUBLIC Qt6::Core)
set_target_properties(PhosphorMcpSchemaChecker PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    # The anonymous namespaces of the checker's files must stay apart.
    UNITY_BUILD OFF
    # No QObject, form or resource in the checker, so no autogen step, and
    # no empty mocs_compilation.cpp in the library and in
    # test_phosphor_mcp_checkwork, also in the tree, which turns all three
    # on.
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

add_library(PhosphorMcp SHARED
    ${phosphormcp_public_HDRS}
    ${phosphormcp_SRCS}
    $<TARGET_OBJECTS:PhosphorMcpSchemaChecker>
)

add_library(PhosphorMcp::PhosphorMcp ALIAS PhosphorMcp)

generate_export_header(PhosphorMcp
    EXPORT_FILE_NAME ${CMAKE_CURRENT_BINARY_DIR}/PhosphorMcp/phosphormcp_export.h
    EXPORT_MACRO_NAME PHOSPHORMCP_EXPORT
)

if(NOT DEFINED KDE_INSTALL_INCLUDEDIR)
    include(GNUInstallDirs)
    set(KDE_INSTALL_INCLUDEDIR ${CMAKE_INSTALL_INCLUDEDIR})
endif()

if(NOT DEFINED KDE_INSTALL_LIBDIR)
    include(GNUInstallDirs)
    set(KDE_INSTALL_LIBDIR ${CMAKE_INSTALL_LIBDIR})
endif()

if(NOT DEFINED KDE_INSTALL_BINDIR)
    include(GNUInstallDirs)
    set(KDE_INSTALL_BINDIR ${CMAKE_INSTALL_BINDIR})
endif()

target_include_directories(PhosphorMcp
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
        $<INSTALL_INTERFACE:${KDE_INSTALL_INCLUDEDIR}>
)

target_compile_features(PhosphorMcp PUBLIC cxx_std_20)

target_link_libraries(PhosphorMcp
    PUBLIC
        Qt6::Core
)

set_target_properties(PhosphorMcp PROPERTIES
    VERSION ${PHOSPHORMCP_VERSION}
    SOVERSION 0
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    # A unity batch makes GCC warn (-Wsubobject-linkage) that
    # McpServer::Private has fields whose types come from an anonymous
    # namespace, which it accepts only in the main file, and would merge the
    # anonymous namespaces of the checker's files.
    UNITY_BUILD OFF
)

# ═══════════════════════════════════════════════════════════════════════════════
# Install
# ═══════════════════════════════════════════════════════════════════════════════

install(TARGETS PhosphorMcp
    EXPORT PhosphorMcpTargets
    RUNTIME DESTINATION ${KDE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${KDE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${KDE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${KDE_INSTALL_INCLUDEDIR}
)

install(FILES ${phosphormcp_public_HDRS}
    DESTINATION ${KDE_INSTALL_INCLUDEDIR}/PhosphorMcp
)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/PhosphorMcp/phosphormcp_export.h
    DESTINATION ${KDE_INSTALL_INCLUDEDIR}/PhosphorMcp
)

install(EXPORT PhosphorMcpTargets
    FILE PhosphorMcpTargets.cmake
    NAMESPACE PhosphorMcp::
    DESTINATION ${KDE_INSTALL_LIBDIR}/cmake/PhosphorMcp
)

include(CMakePackageConfigHelpers)
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/PhosphorMcpConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/PhosphorMcpConfig.cmake"
    INSTALL_DESTINATION ${KDE_INSTALL_LIBDIR}/cmake/PhosphorMcp
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/PhosphorMcpConfigVersion.cmake"
    VERSION ${PHOSPHORMCP_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/PhosphorMcpConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/PhosphorMcpConfigVersion.cmake"
    DESTINATION ${KDE_INSTALL_LIBDIR}/cmake/PhosphorMcp
)

# ═══════════════════════════════════════════════════════════════════════════════
# Example and tests
# ═══════════════════════════════════════════════════════════════════════════════

# ON when phosphor-libs is the outermost project, when this library is
# configured on its own, and when the tests are built, whose smoke test runs
# it. OFF inside an app that adds the tree or this library without tests,
# where PROJECT_IS_TOP_LEVEL would be the app's when the app adds this
# directory itself. Never installed, since it is source for authors of other
# servers.
if(PHOSPHOR_LIBS_TOP_LEVEL OR CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR OR BUILD_TESTING)
    set(_phosphormcp_examples_default ON)
else()
    set(_phosphormcp_examples_default OFF)
endif()
option(BUILD_PHOSPHOR_MCP_EXAMPLES "Build the phosphor-mcp example server" ${_phosphormcp_examples_default})
if(BUILD_PHOSPHOR_MCP_EXAMPLES)
    add_subdirectory(examples)
endif()

if(CMAKE_PROJECT_NAME STREQUAL "PhosphorMcp" OR BUILD_TESTING)
    enable_testing()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
        add_subdirectory(tests)
    endif()
endif()
