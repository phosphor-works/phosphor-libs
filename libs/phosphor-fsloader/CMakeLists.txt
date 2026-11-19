# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# PhosphorFsLoader — filesystem-backed loader scaffolding with live reload.
#
# A tiny Qt6/C++20 library providing a reusable scaffold for "scan a set
# of directories, register entries with a consumer-supplied registry,
# watch for changes, and rescan on edit". The scaffolding (watcher,
# debounce, parent-watch promotion, file-watch re-arming, rescan-during-
# rescan race guard) lives in `WatchedDirectorySet`. Consumers plug in
# their schema-specific enumeration / parsing / commit logic via
# `IScanStrategy`.
#
# Two specialisations ship with the library out of the box:
#   • `DirectoryLoader` — top-level `*.json` scan with user-wins layering,
#                         dispatch to `IDirectoryLoaderSink`. Used by
#                         CurveLoader, ProfileLoader, etc.
#   • `MetadataPackScanStrategy<Payload>` — per-subdirectory `metadata.json`
#                         scan with SHA-1 change detection. Used by the three
#                         MetadataPackLoader-hosted pack registries.
#
# Loaders with different on-disk shapes (subdirectory layouts, non-JSON
# extensions, custom filename validation) implement `IScanStrategy`
# directly rather than extending `DirectoryLoader`.
#
# Depends on Qt6::Core only. No Gui / Qml / DBus coupling.

# 3.18 floor: the pinned-jsoncons acquisition below unpacks with
# file(ARCHIVE_EXTRACT), introduced in CMake 3.18.
cmake_minimum_required(VERSION 3.18)

set(PHOSPHORFSLOADER_VERSION "0.1.0")

# The Qt floor this library requires AND advertises in its generated
# PhosphorFsLoaderConfig.cmake. Taken from the top-level QT_MIN_VERSION when built in-tree so
# the two cannot drift, with its own value for a STANDALONE build, where that
# variable does not exist. The fallback is not optional: the Config template
# substitutes this, and an undefined variable expands to nothing and ships a config
# whose find_dependency(Qt6 ...) carries no version floor at all.
if(DEFINED QT_MIN_VERSION)
    set(PHOSPHORFSLOADER_QT_MIN ${QT_MIN_VERSION})
else()
    set(PHOSPHORFSLOADER_QT_MIN "6.10.0")
endif()

if(NOT PROJECT_VERSION)
    project(PhosphorFsLoader VERSION ${PHOSPHORFSLOADER_VERSION} LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_AUTOMOC ON)
endif()

include(GenerateExportHeader)

find_package(Qt6 ${PHOSPHORFSLOADER_QT_MIN} REQUIRED COMPONENTS Core)

# ═══════════════════════════════════════════════════════════════════════════════
# jsoncons: header-only JSON with JSON Schema validation (2020-12 by default).
#
# The archive cmake/PhosphorDependencies.cmake pins by default (from the
# PHOSPHOR_DEPS_ARCHIVES directory in an offline packaging build, downloaded
# with CPM otherwise). PHOSPHORFSLOADER_USE_SYSTEM_JSONCONS uses the system's
# headers instead, found through JSONCONS_INCLUDE_DIR, which phosphor-mcp
# reads too. jsoncons is used only inside src/schemavalidator.cpp, so it is a
# PRIVATE SYSTEM include: its warnings stay out of the -Wall/-Wextra build, and
# it never reaches the public ABI. No add_subdirectory: the headers are wanted,
# not jsoncons' own install and export rules.
# ═══════════════════════════════════════════════════════════════════════════════

option(PHOSPHORFSLOADER_USE_SYSTEM_JSONCONS
    "Use system-provided jsoncons headers instead of the archive cmake/PhosphorDependencies.cmake pins" OFF)

if(PHOSPHORFSLOADER_USE_SYSTEM_JSONCONS)
    find_path(JSONCONS_INCLUDE_DIR
        NAMES jsoncons_ext/jsonschema/jsonschema.hpp
        DOC "Directory containing jsoncons_ext/jsonschema/jsonschema.hpp")
    if(NOT JSONCONS_INCLUDE_DIR)
        message(FATAL_ERROR
            "PHOSPHORFSLOADER_USE_SYSTEM_JSONCONS=ON but jsoncons' headers "
            "(jsoncons_ext/jsonschema/jsonschema.hpp) were not found. Install jsoncons' "
            "development files, point JSONCONS_INCLUDE_DIR at them, or unset the option "
            "to use the pinned archive.")
    endif()
    # The validator needs make_json_schema with evaluation_options and
    # validate() with a reporter that returns walk_state. Checked by compiling
    # against the headers found, so an older jsoncons fails here and not in
    # the middle of the build.
    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_INCLUDES "${JSONCONS_INCLUDE_DIR}")
    set(CMAKE_REQUIRED_FLAGS "-fexceptions")
    check_cxx_source_compiles([=[
        #include <jsoncons/json.hpp>
        #include <jsoncons_ext/jsonschema/jsonschema.hpp>
        namespace js = jsoncons::jsonschema;
        int main()
        {
            const auto options = js::evaluation_options{}
                .default_version(js::schema_version::draft202012())
                .require_format_validation(true);
            auto schema = js::make_json_schema(jsoncons::ojson::parse("{}"), options);
            schema.validate(jsoncons::ojson(), [](const js::validation_message& m) {
                return m.instance_location().to_string().empty() ? js::walk_state::advance : js::walk_state::abort;
            });
            return 0;
        }
    ]=] PHOSPHORFSLOADER_SYSTEM_JSONCONS_WORKS)
    unset(CMAKE_REQUIRED_INCLUDES)
    unset(CMAKE_REQUIRED_FLAGS)
    if(NOT PHOSPHORFSLOADER_SYSTEM_JSONCONS_WORKS)
        message(FATAL_ERROR
            "The jsoncons headers in ${JSONCONS_INCLUDE_DIR} lack the JSON Schema API that "
            "phosphor-fsloader uses (make_json_schema with evaluation_options, validate() with a "
            "walk_state reporter), so they are older than jsoncons 1.7.0. Install 1.7.0 or later, "
            "or unset PHOSPHORFSLOADER_USE_SYSTEM_JSONCONS to use the pinned archive.")
    endif()
    set(_phosphorfsloader_jsoncons_include "${JSONCONS_INCLUDE_DIR}")
else()
    # In-tree the root CMakeLists has already included the module at the top
    # scope, which is where it has to be (it explains why). A standalone
    # configure of this library reaches it by path.
    if(NOT COMMAND phosphor_fetch_dependency)
        include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/PhosphorDependencies.cmake")
    endif()
    phosphor_fetch_dependency(jsoncons)
    set(_phosphorfsloader_jsoncons_include "${jsoncons_SOURCE_DIR}/include")
endif()

set(phosphorfsloader_SRCS
    src/containedfilereader.cpp
    src/directoryloader.cpp
    src/identitychain.cpp
    src/metadatapackscanstrategy.cpp
    src/schemavalidator.cpp
    src/watcheddirectoryset.cpp
    src/watchedfileset.cpp
)

set(phosphorfsloader_public_HDRS
    include/PhosphorFsLoader/ContainedFileReader.h
    include/PhosphorFsLoader/DirectoryLoader.h
    include/PhosphorFsLoader/FileLimits.h
    include/PhosphorFsLoader/IDirectoryLoaderSink.h
    include/PhosphorFsLoader/IScanStrategy.h
    include/PhosphorFsLoader/JsonEnvelopeValidator.h
    include/PhosphorFsLoader/PackPathGuard.h
    include/PhosphorFsLoader/MetadataPackScanStrategy.h
    include/PhosphorFsLoader/ParsedEntry.h
    include/PhosphorFsLoader/SchemaValidator.h
    include/PhosphorFsLoader/WatchedDirectorySet.h
    include/PhosphorFsLoader/WatchedFileSet.h
)

add_library(PhosphorFsLoader SHARED
    ${phosphorfsloader_public_HDRS}
    ${phosphorfsloader_SRCS}
)

add_library(PhosphorFsLoader::PhosphorFsLoader ALIAS PhosphorFsLoader)

generate_export_header(PhosphorFsLoader
    EXPORT_FILE_NAME ${CMAKE_CURRENT_BINARY_DIR}/PhosphorFsLoader/phosphorfsloader_export.h
    EXPORT_MACRO_NAME PHOSPHORFSLOADER_EXPORT
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
    # The install(TARGETS) below names a RUNTIME destination too; without this
    # a standalone (non-ECM) configure expands it empty. Matches the sibling
    # fallbacks in phosphor-registry and phosphor-tiles.
    include(GNUInstallDirs)
    set(KDE_INSTALL_BINDIR ${CMAKE_INSTALL_BINDIR})
endif()

target_include_directories(PhosphorFsLoader
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
        $<INSTALL_INTERFACE:${KDE_INSTALL_INCLUDEDIR}>
)

target_compile_features(PhosphorFsLoader PUBLIC cxx_std_20)

# jsoncons' headers are a PRIVATE implementation detail of schemavalidator.cpp.
# SYSTEM keeps the project's -Wall/-Wextra/-Wpedantic from flagging the
# third-party headers.
target_include_directories(PhosphorFsLoader SYSTEM PRIVATE "${_phosphorfsloader_jsoncons_include}")

# jsoncons throws on a schema that does not compile, and schemavalidator.cpp
# catches it to fail closed. KDECompilerSettings disables exceptions by
# default (see the -fexceptions override on vendored Luau), so they are
# enabled on this one translation unit. No other file may include a jsoncons
# header: under -fno-exceptions its throwing code would not build.
set_source_files_properties(src/schemavalidator.cpp PROPERTIES
    COMPILE_OPTIONS "$<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:-fexceptions>")

target_link_libraries(PhosphorFsLoader
    PUBLIC
        Qt6::Core
)

set_target_properties(PhosphorFsLoader PROPERTIES
    VERSION ${PHOSPHORFSLOADER_VERSION}
    SOVERSION 0
)

if(CMAKE_PROJECT_NAME STREQUAL "PhosphorFsLoader" OR BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()

if(NOT DEFINED KDE_INSTALL_DATADIR)
    include(GNUInstallDirs)
    set(KDE_INSTALL_DATADIR ${CMAKE_INSTALL_DATAROOTDIR})
endif()

# jsoncons is BSL-1.0 (Daniel Parker) and, in the default pinned build, its
# headers are compiled into this library, so its licence goes with the
# distribution: in this tree's own licence directory, next to the vendored
# Luau notice phosphor-scripting installs. The system-jsoncons build uses
# separately installed headers that carry their own licence, so it is skipped
# there. Guarded on the file existing for the same reason as the Luau rule: a
# scope that skips the extraction must not leave an install rule pointing at a
# path that was never created.
if(NOT PHOSPHORFSLOADER_USE_SYSTEM_JSONCONS)
    set(_phosphorfsloader_jsoncons_license "${jsoncons_SOURCE_DIR}/LICENSE")
    if(EXISTS "${_phosphorfsloader_jsoncons_license}")
        install(FILES ${_phosphorfsloader_jsoncons_license}
            DESTINATION ${KDE_INSTALL_DATADIR}/licenses/phosphor-libs
            RENAME LICENSE.jsoncons)
    endif()
endif()

install(TARGETS PhosphorFsLoader
    EXPORT PhosphorFsLoaderTargets
    RUNTIME DESTINATION ${KDE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${KDE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${KDE_INSTALL_LIBDIR}
)

install(FILES ${phosphorfsloader_public_HDRS}
    DESTINATION ${KDE_INSTALL_INCLUDEDIR}/PhosphorFsLoader
)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/PhosphorFsLoader/phosphorfsloader_export.h
    DESTINATION ${KDE_INSTALL_INCLUDEDIR}/PhosphorFsLoader
)

install(EXPORT PhosphorFsLoaderTargets
    FILE PhosphorFsLoaderTargets.cmake
    NAMESPACE PhosphorFsLoader::
    DESTINATION ${KDE_INSTALL_LIBDIR}/cmake/PhosphorFsLoader
)

include(CMakePackageConfigHelpers)
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/PhosphorFsLoaderConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/PhosphorFsLoaderConfig.cmake"
    INSTALL_DESTINATION ${KDE_INSTALL_LIBDIR}/cmake/PhosphorFsLoader
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/PhosphorFsLoaderConfigVersion.cmake"
    VERSION ${PHOSPHORFSLOADER_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/PhosphorFsLoaderConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/PhosphorFsLoaderConfigVersion.cmake"
    DESTINATION ${KDE_INSTALL_LIBDIR}/cmake/PhosphorFsLoader
)
