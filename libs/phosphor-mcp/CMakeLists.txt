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
# Depends on Qt6::Core and the header-only valijson only. No Gui, no Qml, no
# Network: a server that runs inside a CLI command pulls in nothing more.

# 3.21 floor: PROJECT_IS_TOP_LEVEL gates the example below. The valijson
# acquisition needs 3.18 (file(ARCHIVE_EXTRACT)).
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

include(GenerateExportHeader)

# ═══════════════════════════════════════════════════════════════════════════════
# Dependencies
# ═══════════════════════════════════════════════════════════════════════════════

find_package(Qt6 ${PHOSPHORMCP_QT_MIN} REQUIRED COMPONENTS Core)

# valijson, header-only JSON Schema validation (Draft 7), checks the arguments
# of each call against the tool's input schema. The archive
# cmake/PhosphorDependencies.cmake pins by default, as phosphor-fsloader takes
# it; system headers opt-in. Used only inside src/mcpschema.cpp, so it is a
# PRIVATE SYSTEM include: its warnings stay out of our build, and it never
# reaches the public ABI.
option(PHOSPHORMCP_USE_SYSTEM_VALIJSON
    "Use system-provided valijson headers instead of the archive cmake/PhosphorDependencies.cmake pins" OFF)

if(PHOSPHORMCP_USE_SYSTEM_VALIJSON)
    find_path(PHOSPHORMCP_VALIJSON_INCLUDE_DIR
        NAMES valijson/validator.hpp
        DOC "Directory containing valijson/validator.hpp")
    if(NOT PHOSPHORMCP_VALIJSON_INCLUDE_DIR)
        message(FATAL_ERROR
            "PHOSPHORMCP_USE_SYSTEM_VALIJSON=ON but valijson headers "
            "(valijson/validator.hpp) were not found. Install valijson dev files "
            "or unset the option to use the pinned archive.")
    endif()
else()
    # In-tree the root CMakeLists has already included the module at the top
    # scope, which is where it has to be (it explains why). A standalone
    # configure of this library reaches it by path.
    if(NOT COMMAND phosphor_fetch_dependency)
        include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/PhosphorDependencies.cmake")
    endif()
    phosphor_fetch_dependency(valijson)
    set(_phosphormcp_valijson_root "${valijson_SOURCE_DIR}")
    set(PHOSPHORMCP_VALIJSON_INCLUDE_DIR "${_phosphormcp_valijson_root}/include")
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

target_include_directories(PhosphorMcp SYSTEM PRIVATE "${PHOSPHORMCP_VALIJSON_INCLUDE_DIR}")

target_compile_features(PhosphorMcp PUBLIC cxx_std_20)

# valijson's schema parser throws on a schema it cannot compile, and
# std::regex throws while it validates. KDECompilerSettings turns exceptions
# off, so they come back on for the one translation unit that includes
# valijson, which catches everything before it returns. VALIJSON_USE_EXCEPTIONS
# travels with the flag, as in phosphor-fsloader.
set_source_files_properties(src/mcpschema.cpp PROPERTIES
    COMPILE_OPTIONS "$<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:-fexceptions>"
    COMPILE_DEFINITIONS "VALIJSON_USE_EXCEPTIONS=1")

target_link_libraries(PhosphorMcp
    PUBLIC
        Qt6::Core
)

set_target_properties(PhosphorMcp PROPERTIES
    VERSION ${PHOSPHORMCP_VERSION}
    SOVERSION 0
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    # mcpschema.cpp alone is built with exceptions: a unity batch would
    # carry the flag to its neighbours, or drop it.
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

# valijson is BSD-2-Clause, and with the pinned archive its headers are
# compiled into this library, so its notice ships with it. The same file, at
# the same path, as phosphor-fsloader installs.
if(NOT PHOSPHORMCP_USE_SYSTEM_VALIJSON)
    set(_phosphormcp_valijson_license "${_phosphormcp_valijson_root}/LICENSE")
    if(EXISTS "${_phosphormcp_valijson_license}")
        install(FILES ${_phosphormcp_valijson_license}
            DESTINATION ${KDE_INSTALL_DATADIR}/licenses/phosphor-libs
            RENAME LICENSE.valijson)
    endif()
endif()

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

# ON when this tree is the outermost project (phosphor-libs on its own, or
# this library standalone), OFF inside an app that adds the tree. Never
# installed: it is source for authors of other servers, and the subject of the
# smoke test.
option(BUILD_PHOSPHOR_MCP_EXAMPLES "Build the phosphor-mcp example server" ${PROJECT_IS_TOP_LEVEL})
if(BUILD_PHOSPHOR_MCP_EXAMPLES)
    add_subdirectory(examples)
endif()

if(CMAKE_PROJECT_NAME STREQUAL "PhosphorMcp" OR BUILD_TESTING)
    enable_testing()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
        add_subdirectory(tests)
    endif()
endif()
