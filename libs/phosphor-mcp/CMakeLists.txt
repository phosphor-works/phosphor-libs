# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# PhosphorMcp, a Model Context Protocol server over stdio for Phosphor
# applications. The application declares its tools and answers their calls;
# the library speaks the protocol (revision 2026-07-28): JSON-RPC on stdin and
# stdout, the checks of every request, server/discover, tools/list, the
# dispatch of tools/call with argument checks against each tool's schema,
# cancellation, progress, and the end of the session.
#
# Depends on Qt6::Core only. No Gui, no Qml, no Network: a server that runs
# inside a CLI command pulls in nothing more. The JSON Schema checker of the
# arguments is the library's own (src/mcpschema*.cpp).
# Linux only: the transport writes a terminal through /proc/self/fd and
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

# The library used jsoncons once, with a switch to the system's headers. A
# build directory from then keeps the switch in its cache, where it would
# do nothing without a word.
if(DEFINED CACHE{PHOSPHORMCP_USE_SYSTEM_JSONCONS})
    if(NOT PHOSPHORMCP_USE_SYSTEM_JSONCONS STREQUAL "AUTO")
        message(WARNING "PHOSPHORMCP_USE_SYSTEM_JSONCONS is no longer used: phosphor-mcp checks schemas itself, "
            "without jsoncons. It is removed from the cache.")
    endif()
    unset(PHOSPHORMCP_USE_SYSTEM_JSONCONS CACHE)
endif()

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
    src/mcpschemacheck.cpp
    src/mcpschemacompile.cpp
    src/mcpschemareport.cpp
    src/mcpschemavalue.cpp
    src/mcpserver.cpp
    src/mcpsignals.cpp
    src/mcptool.cpp
    src/mcptransport.cpp
)

add_library(PhosphorMcp SHARED
    ${phosphormcp_public_HDRS}
    ${phosphormcp_SRCS}
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

if(NOT DEFINED KDE_INSTALL_DATADIR)
    include(GNUInstallDirs)
    set(KDE_INSTALL_DATADIR ${CMAKE_INSTALL_DATAROOTDIR})
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
# it. OFF inside an app that adds the tree or this library without tests:
# PROJECT_IS_TOP_LEVEL would be the app's there, when the app adds this
# directory itself. Never installed: it is source for authors of other
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
