# SPDX-FileCopyrightText: 2026 Phosphor Works Team
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# PhosphorMcpIpc, an MCP server that offers the calls of an application's
# phosphor-ipc socket as tools, as far as the application's manifest allows
# them. It holds the library (McpIpcManifest, McpIpcBridge) and the program
# phosphor-mcp-ipc that an MCP client starts. It is apart from phosphor-mcp,
# so that the MCP core does not depend on phosphor-ipc. An application with
# its own MCP server then links no socket client.

# 3.21, the floor of phosphor-mcp, which this library needs.
cmake_minimum_required(VERSION 3.21)

set(PHOSPHORMCPIPC_VERSION "0.1.0")

if(DEFINED QT_MIN_VERSION)
    set(PHOSPHORMCPIPC_QT_MIN ${QT_MIN_VERSION})
else()
    set(PHOSPHORMCPIPC_QT_MIN "6.10.0")
endif()

if(NOT PROJECT_VERSION)
    project(PhosphorMcpIpc VERSION ${PHOSPHORMCPIPC_VERSION} LANGUAGES CXX)
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

find_package(Qt6 ${PHOSPHORMCPIPC_QT_MIN} REQUIRED COMPONENTS Core)
# The siblings, as in-tree targets when this tree declared them first (the
# top-level CMakeLists adds this library after both), else installed copies.
# The versions are the first with what this library uses: IpcClient, and
# McpTool::isValidName with McpServer::toolProblem.
if(NOT TARGET PhosphorIpc::PhosphorIpc)
    find_package(PhosphorIpc 0.4 REQUIRED)
endif()
if(NOT TARGET PhosphorMcp::PhosphorMcp)
    find_package(PhosphorMcp 0.2 REQUIRED)
endif()

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

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

# ═══════════════════════════════════════════════════════════════════════════════
# Library
# ═══════════════════════════════════════════════════════════════════════════════

set(phosphormcpipc_public_HDRS
    include/PhosphorMcpIpc/McpIpcBridge.h
    include/PhosphorMcpIpc/McpIpcManifest.h
)

add_library(PhosphorMcpIpc SHARED
    ${phosphormcpipc_public_HDRS}
    src/mcpipcbridge.cpp
    src/mcpipcmanifest.cpp
)

add_library(PhosphorMcpIpc::PhosphorMcpIpc ALIAS PhosphorMcpIpc)

generate_export_header(PhosphorMcpIpc
    EXPORT_FILE_NAME ${CMAKE_CURRENT_BINARY_DIR}/PhosphorMcpIpc/phosphormcpipc_export.h
    EXPORT_MACRO_NAME PHOSPHORMCPIPC_EXPORT
)

target_include_directories(PhosphorMcpIpc
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
        $<INSTALL_INTERFACE:${KDE_INSTALL_INCLUDEDIR}>
)

target_compile_features(PhosphorMcpIpc PUBLIC cxx_std_20)

# PhosphorMcp is public: addTools() takes an McpServer. The socket client is
# private, since no public header names it.
target_link_libraries(PhosphorMcpIpc
    PUBLIC
        Qt6::Core
        PhosphorMcp::PhosphorMcp
    PRIVATE
        PhosphorIpc::PhosphorIpc
)

set_target_properties(PhosphorMcpIpc PROPERTIES
    VERSION ${PHOSPHORMCPIPC_VERSION}
    SOVERSION 0
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    # Each file keeps its own anonymous namespace.
    UNITY_BUILD OFF
)

# ═══════════════════════════════════════════════════════════════════════════════
# Program
# ═══════════════════════════════════════════════════════════════════════════════

add_executable(phosphor-mcp-ipc src/main.cpp)
add_executable(PhosphorMcpIpc::phosphor-mcp-ipc ALIAS phosphor-mcp-ipc)
target_link_libraries(phosphor-mcp-ipc PRIVATE PhosphorMcpIpc::PhosphorMcpIpc Qt6::Core)
# The version the server gives clients is the library's.
target_compile_definitions(phosphor-mcp-ipc PRIVATE PHOSPHORMCPIPC_VERSION="${PHOSPHORMCPIPC_VERSION}")

# ═══════════════════════════════════════════════════════════════════════════════
# Install
# ═══════════════════════════════════════════════════════════════════════════════

# The program loads libPhosphorMcpIpc, which loads libPhosphorIpc and
# libPhosphorMcp. They can install outside the directories that the
# dynamic linker searches by itself. The program and the library then each
# need a run path, because a run path serves only the file that holds it.
# KDECMakeSettings sets one for a whole tree. Where no install run path is
# in force, this block gives the two targets a run path to their own
# library directory. That is the case for this library configured alone,
# and under an application that sets none. A system prefix gets no run
# path.
if(UNIX AND NOT APPLE AND NOT CMAKE_INSTALL_RPATH AND NOT CMAKE_SKIP_RPATH AND NOT CMAKE_SKIP_INSTALL_RPATH)
    if(IS_ABSOLUTE "${KDE_INSTALL_LIBDIR}")
        set(_phosphormcpipc_libdir "${KDE_INSTALL_LIBDIR}")
    else()
        set(_phosphormcpipc_libdir "${CMAKE_INSTALL_PREFIX}/${KDE_INSTALL_LIBDIR}")
    endif()
    list(FIND CMAKE_PLATFORM_IMPLICIT_LINK_DIRECTORIES "${_phosphormcpipc_libdir}" _phosphormcpipc_platform)
    list(FIND CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES "${_phosphormcpipc_libdir}" _phosphormcpipc_cxx)
    if(_phosphormcpipc_platform EQUAL -1 AND _phosphormcpipc_cxx EQUAL -1)
        set_target_properties(PhosphorMcpIpc phosphor-mcp-ipc PROPERTIES
            INSTALL_RPATH "${_phosphormcpipc_libdir}"
        )
    endif()
endif()

install(TARGETS PhosphorMcpIpc
    EXPORT PhosphorMcpIpcTargets
    RUNTIME DESTINATION ${KDE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${KDE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${KDE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${KDE_INSTALL_INCLUDEDIR}
)
# The program is in the export set too. A project that finds the installed
# package gets PhosphorMcpIpc::phosphor-mcp-ipc with the installed path, for
# the MCP configuration that it writes. A project that builds this tree as
# a subproject gets the path in the build tree from that target. It takes
# the installed path from the install directories. A packager can put the
# program in another package than the CMake files. Both must then be
# installed together, or find_package(PhosphorMcpIpc) fails on the missing
# file, as for phosphor-control-metadata. The rule has no INCLUDES, because
# a program has no headers for its users.
install(TARGETS phosphor-mcp-ipc
    EXPORT PhosphorMcpIpcTargets
    RUNTIME DESTINATION ${KDE_INSTALL_BINDIR}
)

install(FILES ${phosphormcpipc_public_HDRS}
    DESTINATION ${KDE_INSTALL_INCLUDEDIR}/PhosphorMcpIpc
)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/PhosphorMcpIpc/phosphormcpipc_export.h
    DESTINATION ${KDE_INSTALL_INCLUDEDIR}/PhosphorMcpIpc
)

install(EXPORT PhosphorMcpIpcTargets
    FILE PhosphorMcpIpcTargets.cmake
    NAMESPACE PhosphorMcpIpc::
    DESTINATION ${KDE_INSTALL_LIBDIR}/cmake/PhosphorMcpIpc
)

include(CMakePackageConfigHelpers)
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/PhosphorMcpIpcConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/PhosphorMcpIpcConfig.cmake"
    INSTALL_DESTINATION ${KDE_INSTALL_LIBDIR}/cmake/PhosphorMcpIpc
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/PhosphorMcpIpcConfigVersion.cmake"
    VERSION ${PHOSPHORMCPIPC_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/PhosphorMcpIpcConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/PhosphorMcpIpcConfigVersion.cmake"
    DESTINATION ${KDE_INSTALL_LIBDIR}/cmake/PhosphorMcpIpc
)

# ═══════════════════════════════════════════════════════════════════════════════
# Tests
# ═══════════════════════════════════════════════════════════════════════════════

if(CMAKE_PROJECT_NAME STREQUAL "PhosphorMcpIpc" OR BUILD_TESTING)
    enable_testing()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
        add_subdirectory(tests)
    endif()
endif()
