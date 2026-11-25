# SPDX-FileCopyrightText: 2026 Phosphor Works Team
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# PhosphorPaths — where every Phosphor app's files live.
#
# Each XDG root holds one folder for the whole suite, `phosphor-works`. The
# trees the apps share sit directly beneath it (`phosphor-works/layouts`,
# `phosphor-works/shaders/<family>`, ...), and each app's own files sit in its
# folder there (`phosphor-works/<app>`): its config, state and cache. This
# library names those places, so no app or library spells the suite folder
# itself, and moves a tree an app kept elsewhere before into its new place.
#
# Depends on Qt6::Core only.

cmake_minimum_required(VERSION 3.16)

set(PHOSPHORPATHS_VERSION "0.1.0")

# The Qt floor this library requires AND advertises in its generated
# PhosphorPathsConfig.cmake. Taken from the top-level QT_MIN_VERSION when built
# in-tree so the two cannot drift, with its own value for a STANDALONE build.
if(DEFINED QT_MIN_VERSION)
    set(PHOSPHORPATHS_QT_MIN ${QT_MIN_VERSION})
else()
    set(PHOSPHORPATHS_QT_MIN "6.10.0")
endif()

if(NOT PROJECT_VERSION)
    project(PhosphorPaths VERSION ${PHOSPHORPATHS_VERSION} LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

include(GenerateExportHeader)

find_package(Qt6 ${PHOSPHORPATHS_QT_MIN} REQUIRED COMPONENTS Core)

# The suite folder's name, from the one place that defines it. The C++ API
# is compiled from it, and PhosphorPathsConfig.cmake exports it for an
# installed copy. A project builds its install destinations from it, never
# spelling the folder.
if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../cmake/PhosphorSuite.cmake")
    message(FATAL_ERROR "phosphor-paths needs the phosphor-libs tree's cmake/PhosphorSuite.cmake")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/PhosphorSuite.cmake")

set(phosphorpaths_public_HDRS
    include/PhosphorPaths/SuiteDirectories.h
    include/PhosphorPaths/TreeMove.h
)

set(phosphorpaths_SRCS
    src/suitedirectories.cpp
    src/treemove.cpp
)

add_library(PhosphorPaths SHARED
    ${phosphorpaths_public_HDRS}
    ${phosphorpaths_SRCS}
)

add_library(PhosphorPaths::PhosphorPaths ALIAS PhosphorPaths)

generate_export_header(PhosphorPaths
    EXPORT_FILE_NAME ${CMAKE_CURRENT_BINARY_DIR}/PhosphorPaths/phosphorpaths_export.h
    EXPORT_MACRO_NAME PHOSPHORPATHS_EXPORT
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
    # a standalone (non-ECM) configure expands it empty.
    include(GNUInstallDirs)
    set(KDE_INSTALL_BINDIR ${CMAKE_INSTALL_BINDIR})
endif()

target_include_directories(PhosphorPaths
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
        $<INSTALL_INTERFACE:${KDE_INSTALL_INCLUDEDIR}>
)

target_compile_features(PhosphorPaths PUBLIC cxx_std_20)

target_link_libraries(PhosphorPaths
    PUBLIC
        Qt6::Core
)

target_compile_definitions(PhosphorPaths
    PRIVATE
        PHOSPHORPATHS_SUITE_DIRECTORY_NAME="${PHOSPHOR_SUITE_DIRECTORY_NAME}"
)

set_target_properties(PhosphorPaths PROPERTIES
    VERSION ${PHOSPHORPATHS_VERSION}
    SOVERSION 0
)

if(CMAKE_PROJECT_NAME STREQUAL "PhosphorPaths" OR BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()

install(TARGETS PhosphorPaths
    EXPORT PhosphorPathsTargets
    RUNTIME DESTINATION ${KDE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${KDE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${KDE_INSTALL_LIBDIR}
)

install(FILES ${phosphorpaths_public_HDRS}
    DESTINATION ${KDE_INSTALL_INCLUDEDIR}/PhosphorPaths
)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/PhosphorPaths/phosphorpaths_export.h
    DESTINATION ${KDE_INSTALL_INCLUDEDIR}/PhosphorPaths
)

install(EXPORT PhosphorPathsTargets
    FILE PhosphorPathsTargets.cmake
    NAMESPACE PhosphorPaths::
    DESTINATION ${KDE_INSTALL_LIBDIR}/cmake/PhosphorPaths
)

include(CMakePackageConfigHelpers)
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/PhosphorPathsConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/PhosphorPathsConfig.cmake"
    INSTALL_DESTINATION ${KDE_INSTALL_LIBDIR}/cmake/PhosphorPaths
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/PhosphorPathsConfigVersion.cmake"
    VERSION ${PHOSPHORPATHS_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/PhosphorPathsConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/PhosphorPathsConfigVersion.cmake"
    DESTINATION ${KDE_INSTALL_LIBDIR}/cmake/PhosphorPaths
)
