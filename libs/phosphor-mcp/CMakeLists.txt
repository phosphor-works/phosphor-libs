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
# Linux only: the transport writes a terminal through /proc/self/fd and
# sockets with MSG_NOSIGNAL, and takes back SIGPIPE with sigtimedwait.

# 3.21 floor, as phosphor-control's: the tree's floor. The valijson
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

# valijson, header-only JSON Schema validation (Draft 7), checks the arguments
# of each call against the tool's input schema. The archive
# cmake/PhosphorDependencies.cmake pins by default, as phosphor-fsloader takes
# it; system headers opt-in. Used only inside src/mcpschema.cpp, so it is a
# PRIVATE SYSTEM include: its warnings stay out of our build, and it never
# reaches the public ABI.
#
# phosphor-fsloader takes the same valijson, and a packager who switches it to
# the system's headers means the tree's valijson. So AUTO, the default,
# follows PHOSPHORFSLOADER_USE_SYSTEM_VALIJSON, and is resolved again at each
# configure: an option() would keep the first value in the cache, and a later
# switch of phosphor-fsloader alone would leave the two libraries on two
# different copies. ON and OFF choose for this library only. Both libraries
# look for the headers through one cache variable, VALIJSON_INCLUDE_DIR.
set(_phosphormcp_system_valijson_doc
    "Use system-provided valijson headers instead of the archive cmake/PhosphorDependencies.cmake pins: ON, OFF, or AUTO to follow PHOSPHORFSLOADER_USE_SYSTEM_VALIJSON")
# A build directory from before AUTO holds the BOOL that an option() wrote,
# known by its help text. The first option()'s default was OFF, and the
# second's was phosphor-fsloader's value at the directory's first configure.
# The cache records neither whether the value is that default or a choice,
# nor what phosphor-fsloader's value was then. So a value equal to the
# default as it reads now (OFF, or phosphor-fsloader's value at this
# configure) becomes AUTO, and the directory follows phosphor-fsloader from
# now on. Another value stays, with a message that says how to follow
# phosphor-fsloader instead. A -D...:BOOL given to this revision or a later
# one has another help text, and stays as given. In an old directory the
# option() gave every entry its own help text, so a choice there that equals
# the default becomes AUTO.
set(_phosphormcp_old_help
    "Use system-provided valijson headers instead of the archive cmake/PhosphorDependencies.cmake pins")
get_property(_phosphormcp_system_valijson_type CACHE PHOSPHORMCP_USE_SYSTEM_VALIJSON PROPERTY TYPE)
get_property(_phosphormcp_system_valijson_help CACHE PHOSPHORMCP_USE_SYSTEM_VALIJSON PROPERTY HELPSTRING)
set(_phosphormcp_old_default "")
if(_phosphormcp_system_valijson_type STREQUAL "BOOL")
    if(_phosphormcp_system_valijson_help STREQUAL _phosphormcp_old_help)
        set(_phosphormcp_old_default OFF)
    elseif(_phosphormcp_system_valijson_help STREQUAL
           "${_phosphormcp_old_help} (default: PHOSPHORFSLOADER_USE_SYSTEM_VALIJSON)")
        if(PHOSPHORFSLOADER_USE_SYSTEM_VALIJSON)
            set(_phosphormcp_old_default ON)
        else()
            set(_phosphormcp_old_default OFF)
        endif()
    endif()
endif()
if(NOT _phosphormcp_old_default STREQUAL "")
    if(PHOSPHORMCP_USE_SYSTEM_VALIJSON)
        set(_phosphormcp_old_value ON)
    else()
        set(_phosphormcp_old_value OFF)
    endif()
    if(_phosphormcp_old_value STREQUAL _phosphormcp_old_default)
        set(_phosphormcp_old_value AUTO)
    else()
        message(STATUS
            "PHOSPHORMCP_USE_SYSTEM_VALIJSON keeps ${_phosphormcp_old_value} from an older build directory. "
            "Set it to AUTO to follow PHOSPHORFSLOADER_USE_SYSTEM_VALIJSON.")
    endif()
    set(PHOSPHORMCP_USE_SYSTEM_VALIJSON ${_phosphormcp_old_value} CACHE STRING "${_phosphormcp_system_valijson_doc}" FORCE)
endif()
# The first option() looked for the system's headers through a cache variable
# of its own. Its answer moves to the one that both libraries read, unless
# that one names a directory already, which then serves both, with a message.
# In the tree, phosphor-fsloader looks first: a configure that also switches
# it to the system's headers stops there before this runs, and one configure
# without that switch, or VALIJSON_INCLUDE_DIR set, gets past it.
if(DEFINED CACHE{PHOSPHORMCP_VALIJSON_INCLUDE_DIR})
    if(PHOSPHORMCP_VALIJSON_INCLUDE_DIR AND NOT VALIJSON_INCLUDE_DIR)
        set(VALIJSON_INCLUDE_DIR "${PHOSPHORMCP_VALIJSON_INCLUDE_DIR}"
            CACHE PATH "Directory containing valijson/validator.hpp" FORCE)
    elseif(PHOSPHORMCP_VALIJSON_INCLUDE_DIR
           AND NOT PHOSPHORMCP_VALIJSON_INCLUDE_DIR STREQUAL VALIJSON_INCLUDE_DIR)
        message(STATUS
            "PHOSPHORMCP_VALIJSON_INCLUDE_DIR (${PHOSPHORMCP_VALIJSON_INCLUDE_DIR}) is gone. phosphor-mcp now "
            "takes valijson's headers from VALIJSON_INCLUDE_DIR (${VALIJSON_INCLUDE_DIR}), as phosphor-fsloader does.")
    endif()
    unset(PHOSPHORMCP_VALIJSON_INCLUDE_DIR CACHE)
endif()
set(PHOSPHORMCP_USE_SYSTEM_VALIJSON AUTO CACHE STRING "${_phosphormcp_system_valijson_doc}")
set_property(CACHE PHOSPHORMCP_USE_SYSTEM_VALIJSON PROPERTY STRINGS AUTO ON OFF)
string(TOUPPER "${PHOSPHORMCP_USE_SYSTEM_VALIJSON}" _phosphormcp_system_valijson_mode)
if(_phosphormcp_system_valijson_mode STREQUAL "AUTO")
    if(PHOSPHORFSLOADER_USE_SYSTEM_VALIJSON)
        set(_phosphormcp_system_valijson ON)
    else()
        set(_phosphormcp_system_valijson OFF)
    endif()
# CMake's named constants mean ON and OFF as CMake reads them: in any case,
# but NOTFOUND and a name that ends in -NOTFOUND only in capitals. So do 0
# and positive integers written without leading zeros. Anything else is more
# likely a mistake, an empty value too, which CMake reads as false.
elseif(_phosphormcp_system_valijson_mode MATCHES "^(ON|YES|TRUE|Y|[1-9][0-9]*)$")
    set(_phosphormcp_system_valijson ON)
elseif(_phosphormcp_system_valijson_mode MATCHES "^(OFF|NO|FALSE|N|0|IGNORE)$"
       OR PHOSPHORMCP_USE_SYSTEM_VALIJSON MATCHES "^(.*-)?NOTFOUND$")
    set(_phosphormcp_system_valijson OFF)
else()
    message(FATAL_ERROR
        "PHOSPHORMCP_USE_SYSTEM_VALIJSON is \"${PHOSPHORMCP_USE_SYSTEM_VALIJSON}\". It takes AUTO, ON or OFF.")
endif()

if(_phosphormcp_system_valijson)
    find_path(VALIJSON_INCLUDE_DIR
        NAMES valijson/validator.hpp
        DOC "Directory containing valijson/validator.hpp")
    if(NOT VALIJSON_INCLUDE_DIR)
        message(FATAL_ERROR
            "PHOSPHORMCP_USE_SYSTEM_VALIJSON is ${PHOSPHORMCP_USE_SYSTEM_VALIJSON} (AUTO follows PHOSPHORFSLOADER_USE_SYSTEM_VALIJSON), but "
            "valijson/validator.hpp was not found. Install valijson's headers and point VALIJSON_INCLUDE_DIR at "
            "them, or set PHOSPHORMCP_USE_SYSTEM_VALIJSON and PHOSPHORFSLOADER_USE_SYSTEM_VALIJSON to OFF to "
            "use the pinned archive.")
    endif()
    # valijson 1.1.0 is the first whose errors carry jsonPointer, which
    # src/mcpschema.cpp reads. A header-only install has no version file
    # worth trusting, so the check compiles against the headers. Its result
    # is kept for the directory it was made with, and a failure is not kept.
    if(NOT "${VALIJSON_INCLUDE_DIR}" STREQUAL "${_PHOSPHORMCP_VALIJSON_CHECKED_DIR}")
        unset(PHOSPHORMCP_VALIJSON_HAS_JSON_POINTER CACHE)
        set(_PHOSPHORMCP_VALIJSON_CHECKED_DIR "${VALIJSON_INCLUDE_DIR}" CACHE INTERNAL
            "The valijson headers that PHOSPHORMCP_VALIJSON_HAS_JSON_POINTER was checked with")
    endif()
    include(CheckCXXSourceCompiles)
    include(CMakePushCheckState)
    cmake_push_check_state(RESET)
    set(CMAKE_REQUIRED_INCLUDES "${VALIJSON_INCLUDE_DIR}")
    check_cxx_source_compiles([=[
#include <valijson/validation_results.hpp>
int main()
{
    valijson::ValidationResults::Error error;
    error.jsonPointer = "/text";
    return error.jsonPointer.empty() ? 1 : 0;
}
]=] PHOSPHORMCP_VALIJSON_HAS_JSON_POINTER)
    cmake_pop_check_state()
    if(NOT PHOSPHORMCP_VALIJSON_HAS_JSON_POINTER)
        unset(PHOSPHORMCP_VALIJSON_HAS_JSON_POINTER CACHE)
        message(FATAL_ERROR
            "The valijson headers in ${VALIJSON_INCLUDE_DIR} are older than 1.1.0, which phosphor-mcp needs "
            "(ValidationResults::Error::jsonPointer). Install valijson 1.1.0 or later, or set "
            "PHOSPHORMCP_USE_SYSTEM_VALIJSON to OFF to use the pinned archive.")
    endif()
    set(_phosphormcp_valijson_include "${VALIJSON_INCLUDE_DIR}")
else()
    # In-tree the root CMakeLists has already included the module at the top
    # scope, which is where it has to be (it explains why). A standalone
    # configure of this library reaches it by path.
    if(NOT COMMAND phosphor_fetch_dependency)
        include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/PhosphorDependencies.cmake")
    endif()
    phosphor_fetch_dependency(valijson)
    set(_phosphormcp_valijson_root "${valijson_SOURCE_DIR}")
    set(_phosphormcp_valijson_include "${_phosphormcp_valijson_root}/include")
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

target_include_directories(PhosphorMcp SYSTEM PRIVATE "${_phosphormcp_valijson_include}")

target_compile_features(PhosphorMcp PUBLIC cxx_std_20)

# valijson's schema parser throws on a schema it cannot compile, and its
# validator can throw on a value it cannot check. KDECompilerSettings turns
# exceptions off, so they come back on for the one translation unit that
# includes valijson, which catches everything before it returns. VALIJSON_USE_EXCEPTIONS
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

# valijson is BSD-2-Clause, and with the pinned archive its headers are
# compiled into this library, so its notice ships with it. The same file, at
# the same path, as phosphor-fsloader installs.
if(NOT _phosphormcp_system_valijson)
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
