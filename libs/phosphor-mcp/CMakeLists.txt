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
# Depends on Qt6::Core and the header-only jsoncons only. No Gui, no Qml, no
# Network: a server that runs inside a CLI command pulls in nothing more.
# Linux only: the transport writes a terminal through /proc/self/fd and
# sockets with MSG_NOSIGNAL, and takes back SIGPIPE with sigtimedwait.

# 3.21 floor, as phosphor-control's: the tree's floor. The jsoncons
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

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "PhosphorMcp needs Linux (pipe2, sigtimedwait, MSG_NOSIGNAL, /proc/self/fd)")
endif()

include(GenerateExportHeader)

# ═══════════════════════════════════════════════════════════════════════════════
# Dependencies
# ═══════════════════════════════════════════════════════════════════════════════

find_package(Qt6 ${PHOSPHORMCP_QT_MIN} REQUIRED COMPONENTS Core)

# jsoncons, header-only JSON with JSON Schema validation, checks the arguments
# of each call against the tool's input schema in JSON Schema 2020-12. The
# archive cmake/PhosphorDependencies.cmake pins by default, as
# phosphor-fsloader takes it; system headers opt-in. Used only inside
# src/mcpschema.cpp, so it is a PRIVATE SYSTEM include: its warnings stay out
# of our build, and it never reaches the public ABI.
#
# phosphor-fsloader takes the same jsoncons, and a packager who switches it to
# the system's headers means the tree's jsoncons. So AUTO, the default,
# follows PHOSPHORFSLOADER_USE_SYSTEM_JSONCONS, and is resolved again at each
# configure: an option() would keep the first value in the cache, and a later
# switch of phosphor-fsloader alone would leave the two libraries on two
# different copies. ON and OFF choose for this library only. Both libraries
# look for the headers through one cache variable, JSONCONS_INCLUDE_DIR.
set(PHOSPHORMCP_USE_SYSTEM_JSONCONS AUTO CACHE STRING
    "Use system-provided jsoncons headers instead of the archive cmake/PhosphorDependencies.cmake pins: ON, OFF, or AUTO to follow PHOSPHORFSLOADER_USE_SYSTEM_JSONCONS")
set_property(CACHE PHOSPHORMCP_USE_SYSTEM_JSONCONS PROPERTY STRINGS AUTO ON OFF)
string(TOUPPER "${PHOSPHORMCP_USE_SYSTEM_JSONCONS}" _phosphormcp_system_jsoncons_mode)
if(_phosphormcp_system_jsoncons_mode STREQUAL "AUTO")
    if(PHOSPHORFSLOADER_USE_SYSTEM_JSONCONS)
        set(_phosphormcp_system_jsoncons ON)
    else()
        set(_phosphormcp_system_jsoncons OFF)
    endif()
# CMake's named constants mean ON and OFF as CMake reads them: in any case,
# but NOTFOUND and a name that ends in -NOTFOUND only in capitals. So do 0
# and positive integers written without leading zeros. Anything else is more
# likely a mistake, an empty value too, which CMake reads as false.
elseif(_phosphormcp_system_jsoncons_mode MATCHES "^(ON|YES|TRUE|Y|[1-9][0-9]*)$")
    set(_phosphormcp_system_jsoncons ON)
elseif(_phosphormcp_system_jsoncons_mode MATCHES "^(OFF|NO|FALSE|N|0|IGNORE)$"
       OR PHOSPHORMCP_USE_SYSTEM_JSONCONS MATCHES "^(.*-)?NOTFOUND$")
    set(_phosphormcp_system_jsoncons OFF)
else()
    message(FATAL_ERROR
        "PHOSPHORMCP_USE_SYSTEM_JSONCONS is \"${PHOSPHORMCP_USE_SYSTEM_JSONCONS}\". It takes AUTO, ON or OFF.")
endif()

if(_phosphormcp_system_jsoncons)
    find_path(JSONCONS_INCLUDE_DIR
        NAMES jsoncons_ext/jsonschema/jsonschema.hpp
        DOC "Directory containing jsoncons_ext/jsonschema/jsonschema.hpp")
    # A directory given by hand is not searched, so it is checked too.
    if(NOT JSONCONS_INCLUDE_DIR OR NOT EXISTS "${JSONCONS_INCLUDE_DIR}/jsoncons_ext/jsonschema/jsonschema.hpp")
        message(FATAL_ERROR
            "PHOSPHORMCP_USE_SYSTEM_JSONCONS is ${PHOSPHORMCP_USE_SYSTEM_JSONCONS} (AUTO follows PHOSPHORFSLOADER_USE_SYSTEM_JSONCONS), but "
            "jsoncons_ext/jsonschema/jsonschema.hpp was not found (JSONCONS_INCLUDE_DIR is \"${JSONCONS_INCLUDE_DIR}\"). "
            "Install jsoncons' headers and point JSONCONS_INCLUDE_DIR at them, or set PHOSPHORMCP_USE_SYSTEM_JSONCONS and "
            "PHOSPHORFSLOADER_USE_SYSTEM_JSONCONS to OFF to use the pinned archive.")
    endif()
    # jsoncons 1.7.0 is the first with jsonschema::walk_state, which
    # src/mcpschema.cpp returns from its reporter, beside the validation
    # messages' keyword, evaluation path and details that it reads. A
    # header-only install has no version file worth trusting, so the check
    # compiles against the headers. Its result is kept for the directory it
    # was made with, and a failure is not kept.
    if(NOT "${JSONCONS_INCLUDE_DIR}" STREQUAL "${_PHOSPHORMCP_JSONCONS_CHECKED_DIR}")
        unset(PHOSPHORMCP_JSONCONS_HAS_WALK_STATE CACHE)
        set(_PHOSPHORMCP_JSONCONS_CHECKED_DIR "${JSONCONS_INCLUDE_DIR}" CACHE INTERNAL
            "The jsoncons headers that PHOSPHORMCP_JSONCONS_HAS_WALK_STATE was checked with")
    endif()
    include(CheckCXXSourceCompiles)
    include(CMakePushCheckState)
    cmake_push_check_state(RESET)
    set(CMAKE_REQUIRED_INCLUDES "${JSONCONS_INCLUDE_DIR}")
    check_cxx_source_compiles([=[
#include <jsoncons/json.hpp>
#include <jsoncons_ext/jsonschema/jsonschema.hpp>
#include <cstddef>
int main()
{
    namespace jss = jsoncons::jsonschema;
    const auto schema = jss::make_json_schema(jsoncons::json::parse("{}"),
        jss::evaluation_options{}.default_version(jss::schema_version::draft202012()));
    std::size_t seen = 0;
    schema.validate(jsoncons::json(1), [&](const jss::validation_message& message) -> jss::walk_state {
        seen += message.keyword().size() + message.details().size() + message.eval_path().string().size()
            + message.instance_location().string().size();
        return jss::walk_state::abort;
    });
    return seen == 0 ? 0 : 1;
}
]=] PHOSPHORMCP_JSONCONS_HAS_WALK_STATE)
    cmake_pop_check_state()
    if(NOT PHOSPHORMCP_JSONCONS_HAS_WALK_STATE)
        unset(PHOSPHORMCP_JSONCONS_HAS_WALK_STATE CACHE)
        message(FATAL_ERROR
            "The jsoncons headers in ${JSONCONS_INCLUDE_DIR} are older than 1.7.0, which phosphor-mcp needs "
            "(jsonschema::walk_state). Install jsoncons 1.7.0 or later, or set "
            "PHOSPHORMCP_USE_SYSTEM_JSONCONS to OFF to use the pinned archive.")
    endif()
    set(_phosphormcp_jsoncons_include "${JSONCONS_INCLUDE_DIR}")
else()
    # In-tree the root CMakeLists has already included the module at the top
    # scope, which is where it has to be (it explains why). A standalone
    # configure of this library reaches it by path.
    if(NOT COMMAND phosphor_fetch_dependency)
        include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/PhosphorDependencies.cmake")
    endif()
    phosphor_fetch_dependency(jsoncons)
    set(_phosphormcp_jsoncons_root "${jsoncons_SOURCE_DIR}")
    set(_phosphormcp_jsoncons_include "${_phosphormcp_jsoncons_root}/include")
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

target_include_directories(PhosphorMcp SYSTEM PRIVATE "${_phosphormcp_jsoncons_include}")

target_compile_features(PhosphorMcp PUBLIC cxx_std_20)

# jsoncons throws on a schema it cannot compile, and can throw on a value it
# cannot check. KDECompilerSettings turns exceptions off, so they come back
# on for the one translation unit that includes jsoncons, which catches
# everything before it returns. jsoncons reads __cpp_exceptions to choose
# between throwing and std::terminate(), so the flag needs no define beside
# it. No other translation unit includes a jsoncons header.
set_source_files_properties(src/mcpschema.cpp PROPERTIES
    COMPILE_OPTIONS "$<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:-fexceptions>")

target_link_libraries(PhosphorMcp
    PUBLIC
        Qt6::Core
)

set_target_properties(PhosphorMcp PROPERTIES
    VERSION ${PHOSPHORMCP_VERSION}
    SOVERSION 0
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    # CMake keeps mcpschema.cpp, which has compile options of its own, out of
    # any unity batch already. A batch of the other sources makes GCC warn
    # (-Wsubobject-linkage) that McpServer::Private has fields whose types
    # come from an anonymous namespace, which it accepts only in the main
    # file.
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

# jsoncons is BSL-1.0, and with the pinned archive its headers are compiled
# into this library, so its notice ships with it. The same file, at the same
# path, as phosphor-fsloader installs.
if(NOT _phosphormcp_system_jsoncons)
    set(_phosphormcp_jsoncons_license "${_phosphormcp_jsoncons_root}/LICENSE")
    if(EXISTS "${_phosphormcp_jsoncons_license}")
        install(FILES ${_phosphormcp_jsoncons_license}
            DESTINATION ${KDE_INSTALL_DATADIR}/licenses/phosphor-libs
            RENAME LICENSE.jsoncons)
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
