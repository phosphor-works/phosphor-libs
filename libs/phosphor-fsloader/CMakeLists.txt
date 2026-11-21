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
# headers instead, found through JSONCONS_INCLUDE_DIR and checked here, the
# one place in the tree that does: phosphor-fsloader is jsoncons' only user.
# jsoncons is used only inside src/schemavalidator.cpp, so it is a PRIVATE
# SYSTEM include, which keeps its warnings out of the -Wall/-Wextra build, and
# the library's symbols are hidden but for its exports (below), so none of
# jsoncons' reaches the library's ABI. No add_subdirectory: the headers are
# wanted, not jsoncons' own install and export rules.
# ═══════════════════════════════════════════════════════════════════════════════

option(PHOSPHORFSLOADER_USE_SYSTEM_JSONCONS
    "Use system-provided jsoncons headers (1.7.0 or later) instead of the archive cmake/PhosphorDependencies.cmake pins" OFF)

# phosphor-fsloader used valijson once. A build directory from then keeps its
# two cache entries, which nothing reads any more. Each is removed from the
# cache, with a warning when it held something other than its default (the
# option turned on, a directory found or set), so that a packager does not
# believe it still chooses anything. phosphor-mcp removes its own old entries
# in its own CMakeLists.
foreach(_phosphorfsloader_old PHOSPHORFSLOADER_USE_SYSTEM_VALIJSON VALIJSON_INCLUDE_DIR)
    if(DEFINED CACHE{${_phosphorfsloader_old}})
        if(${_phosphorfsloader_old})
            message(WARNING
                "${_phosphorfsloader_old} is set in this build directory's cache, but nothing reads it, "
                "because phosphor-fsloader no longer uses valijson. Its schema validator is jsoncons, and "
                "PHOSPHORFSLOADER_USE_SYSTEM_JSONCONS and JSONCONS_INCLUDE_DIR choose its headers. It is "
                "removed from the cache.")
        endif()
        unset(${_phosphorfsloader_old} CACHE)
    endif()
    # The flag an earlier version kept to warn only once.
    unset(_PHOSPHORFSLOADER_WARNED_${_phosphorfsloader_old} CACHE)
endforeach()

if(PHOSPHORFSLOADER_USE_SYSTEM_JSONCONS)
    find_path(JSONCONS_INCLUDE_DIR
        NAMES jsoncons_ext/jsonschema/jsonschema.hpp
        DOC "Directory containing jsoncons_ext/jsonschema/jsonschema.hpp")
    if(NOT JSONCONS_INCLUDE_DIR)
        message(FATAL_ERROR
            "PHOSPHORFSLOADER_USE_SYSTEM_JSONCONS=ON but jsoncons' headers "
            "(jsoncons_ext/jsonschema/jsonschema.hpp) were not found. Install jsoncons' "
            "development files (1.7.0 or later), point JSONCONS_INCLUDE_DIR at them, or unset "
            "the option to use the pinned archive.")
    endif()
    # find_path keeps a directory set by hand without looking in it.
    if(NOT EXISTS "${JSONCONS_INCLUDE_DIR}/jsoncons_ext/jsonschema/jsonschema.hpp")
        message(FATAL_ERROR
            "JSONCONS_INCLUDE_DIR is ${JSONCONS_INCLUDE_DIR}, which has no "
            "jsoncons_ext/jsonschema/jsonschema.hpp. Point it at the directory that holds "
            "jsoncons/ and jsoncons_ext/.")
    endif()
    # The floor is jsoncons 1.7.0, the first release whose JSON Schema API the
    # validator compiles against: checked by compiling a use of each part of
    # it against the headers found, so an older jsoncons fails here and not
    # in the middle of the build. The result is cached for the directory it
    # was found for: a new JSONCONS_INCLUDE_DIR is checked again, and so is
    # the same one after a failure, whose result is not kept.
    if(NOT "${JSONCONS_INCLUDE_DIR}" STREQUAL "${_PHOSPHORFSLOADER_JSONCONS_CHECKED_DIR}")
        unset(PHOSPHORFSLOADER_SYSTEM_JSONCONS_WORKS CACHE)
    endif()
    include(CheckCXXSourceCompiles)
    include(CMakePushCheckState)
    cmake_push_check_state(RESET)
    set(CMAKE_REQUIRED_INCLUDES "${JSONCONS_INCLUDE_DIR}")
    # schemavalidator.cpp is built with exceptions; so is the check, whatever
    # KDECompilerSettings put in CMAKE_CXX_FLAGS.
    if(MSVC)
        set(CMAKE_REQUIRED_FLAGS "/EHsc")
    else()
        set(CMAKE_REQUIRED_FLAGS "-fexceptions")
    endif()
    check_cxx_source_compiles([=[
        #include <jsoncons/json.hpp>
        #include <jsoncons_ext/jsonschema/jsonschema.hpp>
        #include <string>
        #include <utility>
        namespace js = jsoncons::jsonschema;
        #if JSONCONS_VERSION_MAJOR < 1 || (JSONCONS_VERSION_MAJOR == 1 && JSONCONS_VERSION_MINOR < 7)
        #error "jsoncons older than 1.7.0"
        #endif
        int main()
        {
            const auto options = js::evaluation_options{}
                .default_version(js::schema_version::draft202012())
                .require_format_validation(true);
            const auto meta = js::make_json_schema(js::draft202012::schema_draft202012<jsoncons::json>::get_schema(), options);
            jsoncons::json object(jsoncons::json_object_arg);
            object.reserve(1);
            object.insert_or_assign(std::string("a"), jsoncons::json(jsoncons::json_array_arg));
            auto schema = js::make_json_schema(std::move(object), options);
            meta.validate(jsoncons::json(), [](const js::validation_message& m) {
                return m.details().empty() && m.keyword().empty() && m.eval_path().to_string().empty()
                    ? js::walk_state::advance : js::walk_state::abort;
            });
            schema.validate(jsoncons::json(), [](const js::validation_message& m) {
                return m.instance_location().to_string().empty() ? js::walk_state::advance : js::walk_state::abort;
            });
            return 0;
        }
    ]=] PHOSPHORFSLOADER_SYSTEM_JSONCONS_WORKS)
    cmake_pop_check_state()
    if(NOT PHOSPHORFSLOADER_SYSTEM_JSONCONS_WORKS)
        unset(PHOSPHORFSLOADER_SYSTEM_JSONCONS_WORKS CACHE)
        message(FATAL_ERROR
            "The jsoncons headers in ${JSONCONS_INCLUDE_DIR} do not compile against the JSON Schema "
            "API that phosphor-fsloader uses, which needs jsoncons 1.7.0 or later (CMakeConfigureLog.yaml "
            "has the compiler's output). Install 1.7.0 or later, or unset "
            "PHOSPHORFSLOADER_USE_SYSTEM_JSONCONS to use the pinned archive.")
    endif()
    set(_PHOSPHORFSLOADER_JSONCONS_CHECKED_DIR "${JSONCONS_INCLUDE_DIR}" CACHE INTERNAL "")
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

# Only what PHOSPHORFSLOADER_EXPORT marks leaves the library. In-tree
# KDECompilerSettings sets this for every target; set here, it holds for a
# standalone build too, where the jsoncons templates compiled into
# schemavalidator.cpp would otherwise be exported by the thousand.
set_target_properties(PhosphorFsLoader PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

# jsoncons throws on a schema that does not compile, and schemavalidator.cpp
# catches it to fail closed. KDECompilerSettings disables exceptions for every
# compiler it knows; its kde_source_files_enable_exceptions() turns them back
# on for this one translation unit, with each compiler's flag. Without
# KDECompilerSettings (a standalone build) nothing disabled them. The file is
# kept out of unity builds, so its flag and its jsoncons headers stay its own.
#
# No other file may include a jsoncons header. It would still compile without
# exceptions, as jsoncons then calls std::terminate() where it would throw, so
# a schema error would end the process: a rule kept by review, which the build
# cannot enforce. (This file cannot be built that way: its try/catch would not
# compile.)
if(COMMAND kde_source_files_enable_exceptions)
    kde_source_files_enable_exceptions(src/schemavalidator.cpp)
endif()
set_source_files_properties(src/schemavalidator.cpp PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)

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
