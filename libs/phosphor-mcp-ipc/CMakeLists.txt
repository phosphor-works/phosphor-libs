# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# PhosphorMcpIpc, an MCP server that offers the calls of an application's
# phosphor-ipc socket as tools, as far as the application's manifest allows
# them. The library (McpIpcManifest, McpIpcBridge) and the program
# phosphor-mcp-ipc that an MCP client starts. Apart from phosphor-mcp, so
# that the MCP core does not depend on phosphor-ipc, and an application with
# an MCP server of its own does not link a socket client it does not use.

# 3.21 floor, as phosphor-mcp's.
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
if(NOT TARGET PhosphorIpc::PhosphorIpc)
    find_package(PhosphorIpc REQUIRED)
endif()
if(NOT TARGET PhosphorMcp::PhosphorMcp)
    find_package(PhosphorMcp REQUIRED)
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

# The program is exported too, so that an application that installs a
# manifest can find the path of the server it names in its MCP
# configuration.
install(TARGETS PhosphorMcpIpc phosphor-mcp-ipc
    EXPORT PhosphorMcpIpcTargets
    RUNTIME DESTINATION ${KDE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${KDE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${KDE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${KDE_INSTALL_INCLUDEDIR}
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
