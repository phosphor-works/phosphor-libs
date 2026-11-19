# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# The third-party sources the libraries build from, each pinned to one archive
# by version, URL and SHA-256. This file is the only place these pins live. An
# app that builds this tree from source (PlasmaZones) repeats the URL and hash
# of each archive in its packaging recipes, and its dep-pins conventions rule
# reads them from here, at the commit the app pins.
#
# How an archive is obtained, in order:
#
#   1. PHOSPHOR_DEPS_ARCHIVES names a directory holding the archives under their
#      FILE names below. Each is checked against its SHA-256 and extracted into
#      the build tree. Nothing is downloaded and CPM is never loaded, so this is
#      the path every offline packaging build takes (makepkg, rpmbuild, OBS,
#      dpkg-buildpackage, Nix), with the recipe listing the archives as extra
#      sources.
#   2. Otherwise CPM.cmake (https://github.com/cpm-cmake/CPM.cmake, itself
#      pinned by hash) downloads the archive once into CPM_SOURCE_CACHE, which
#      is .cache/cpm at this repository's root unless the cache variable or
#      the environment variable says otherwise. An app that adds this tree
#      has set CPM_SOURCE_CACHE already, and its cache is used. Every build directory shares it, and
#      a configure works offline once it is filled. CPM's own overrides apply,
#      so -DCPM_luau_SOURCE=<dir> builds a local Luau checkout instead.
#
# A library that offers a system copy (PHOSPHOR_SYSTEM_LUAU,
# PHOSPHORFSLOADER_USE_SYSTEM_VALIJSON, PHOSPHORMCP_USE_SYSTEM_VALIJSON, whose
# default AUTO follows phosphor-fsloader's option) checks that option before calling in
# here, so it fetches no archive for a system copy. A standalone configure of
# such a library with its system option on never includes this file. The
# tree's root includes it on every configure, though, and CPM.cmake is loaded
# (and downloaded, the first time) when this file is included. So an offline
# configure of the whole tree sets PHOSPHOR_DEPS_ARCHIVES, or has a filled
# CPM_SOURCE_CACHE, even when every library uses a system copy. The directory
# then needs only the archives of the test suite (PHOSPHOR_TEST_DEPS), when
# the tests are built.
#
# Script mode, for packagers and CI:
#
#   cmake -DPHOSPHOR_DEPS_FETCH=<dir> [-DPHOSPHOR_DEPS_FETCH_TESTS=ON] -P cmake/PhosphorDependencies.cmake
#
# downloads every source archive into <dir> under its FILE name and checks it,
# so <dir> can be handed to PHOSPHOR_DEPS_ARCHIVES. With PHOSPHOR_DEPS_FETCH_TESTS
# it also downloads the archives only the test suite reads (PHOSPHOR_TEST_DEPS),
# for an offline build that runs the tests. An archive already there with the
# right hash is kept.
#
# Bumping a pin: change its VERSION, URL, FILE and SHA256 together, with the
# hash taken from a download you checked, never from whatever the next
# configure fetched. Then run check-conventions.py, which names every recipe
# still carrying the old pin.

include_guard(GLOBAL)

# ─── Pins ────────────────────────────────────────────────────────────────────
# Source archives: built into the tree. PHOSPHOR_DEPS lists them for the fetch
# script and for the dep-pins rule.
set(PHOSPHOR_DEPS luau valijson)

# Luau (MIT, Roblox), the tiling algorithms' runtime (phosphor-scripting).
set(PHOSPHOR_DEP_luau_VERSION "0.745")
set(PHOSPHOR_DEP_luau_URL "https://github.com/luau-lang/luau/archive/refs/tags/0.745.tar.gz")
set(PHOSPHOR_DEP_luau_FILE "luau-0.745.tar.gz")
set(PHOSPHOR_DEP_luau_SHA256 "53c22773bace6cabbabaead576fbc08d11a0317fce11a3dbf9d027dda6849248")

# valijson (BSD-2-Clause, Tristan Penman), header-only JSON Schema validation
# (phosphor-fsloader, phosphor-mcp).
set(PHOSPHOR_DEP_valijson_VERSION "1.1.3")
set(PHOSPHOR_DEP_valijson_URL "https://github.com/tristanpenman/valijson/archive/refs/tags/v1.1.3.tar.gz")
set(PHOSPHOR_DEP_valijson_FILE "valijson-1.1.3.tar.gz")
set(PHOSPHOR_DEP_valijson_SHA256 "887b53b1a924f6fe0b35fa3bbc9bbbe5ae8c72097b7f0f7c17b45f4cfa646029")

# Test data: read by the test suite and by nothing that is built or installed,
# so fetched only when BUILD_TESTING is on, and not part of PHOSPHOR_DEPS. A
# packaging build that skips the tests never needs these.
set(PHOSPHOR_TEST_DEPS phosphor-surface-packs)

# The surface decoration packs (LGPL-2.1-or-later with some GPL-3.0-or-later
# shaders, phosphor-works/phosphor-surface-packs), which phosphor-surface's
# tests load to check the library against the packs the apps ship. A commit,
# not a release, so VERSION is the commit hash.
set(PHOSPHOR_DEP_phosphor-surface-packs_VERSION "e89d9f7bb6320eab8b6a0b3325858d2ca5356ce3")
set(PHOSPHOR_DEP_phosphor-surface-packs_URL "https://github.com/phosphor-works/phosphor-surface-packs/archive/e89d9f7bb6320eab8b6a0b3325858d2ca5356ce3.tar.gz")
set(PHOSPHOR_DEP_phosphor-surface-packs_FILE "phosphor-surface-packs-e89d9f7bb6320eab8b6a0b3325858d2ca5356ce3.tar.gz")
set(PHOSPHOR_DEP_phosphor-surface-packs_SHA256 "fbbf0b9849d83396029bc4e310c3dcac49c08758e73d560538888e685f0a5e30")

# CPM.cmake itself. An app that adds this tree loads its own pinned CPM first,
# and that copy is used.
set(PHOSPHOR_CPM_VERSION "0.43.2")
set(PHOSPHOR_CPM_SHA256 "49a3bef91ceb65bb66d57255e12d1ffd22abc2f6408fa9fe4534c544a2f232aa")

# The repository root: this file sits in its cmake/ directory. Captured here,
# at include time, because CMAKE_CURRENT_LIST_DIR names the caller inside a
# function.
get_filename_component(_PHOSPHOR_DEPS_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

# ─── Script mode: download the source archives ───────────────────────────────
if(CMAKE_SCRIPT_MODE_FILE)
    if(NOT PHOSPHOR_DEPS_FETCH)
        message(FATAL_ERROR "usage: cmake -DPHOSPHOR_DEPS_FETCH=<dir> -P ${CMAKE_CURRENT_LIST_FILE}")
    endif()
    get_filename_component(_dir "${PHOSPHOR_DEPS_FETCH}" ABSOLUTE)
    file(MAKE_DIRECTORY "${_dir}")
    set(_names ${PHOSPHOR_DEPS})
    if(PHOSPHOR_DEPS_FETCH_TESTS)
        list(APPEND _names ${PHOSPHOR_TEST_DEPS})
    endif()
    foreach(_name IN LISTS _names)
        set(_file "${_dir}/${PHOSPHOR_DEP_${_name}_FILE}")
        set(_have "")
        if(EXISTS "${_file}")
            file(SHA256 "${_file}" _have)
        endif()
        if(NOT _have STREQUAL PHOSPHOR_DEP_${_name}_SHA256)
            message(STATUS "Downloading ${PHOSPHOR_DEP_${_name}_URL}")
            # To a .part file, renamed only once its hash checks, so an
            # interrupted download never sits under the real name.
            file(DOWNLOAD "${PHOSPHOR_DEP_${_name}_URL}" "${_file}.part"
                 EXPECTED_HASH SHA256=${PHOSPHOR_DEP_${_name}_SHA256} STATUS _status)
            list(GET _status 0 _code)
            if(NOT _code EQUAL 0)
                file(REMOVE "${_file}.part")
                list(GET _status 1 _why)
                message(FATAL_ERROR "Could not download ${PHOSPHOR_DEP_${_name}_URL}: ${_why}")
            endif()
            file(RENAME "${_file}.part" "${_file}")
        endif()
        message(STATUS "${PHOSPHOR_DEP_${_name}_FILE}: ok")
    endforeach()
    return()
endif()

# ─── Configure mode ──────────────────────────────────────────────────────────
set(PHOSPHOR_DEPS_ARCHIVES "" CACHE PATH
    "Directory holding the pinned dependency archives (cmake/PhosphorDependencies.cmake), for an offline build. Empty: download them with CPM")

# CPM.cmake, loaded here at the including directory's scope rather than from
# inside phosphor_fetch_dependency: CPM keeps part of its state in ordinary
# variables of the scope that includes it, which a function scope would drop.
# Not loaded at all for an offline build (PHOSPHOR_DEPS_ARCHIVES), which must
# not reach the network even for CPM itself. The cached copy is checked against
# its hash on every configure, not only after a download, so a broken-off
# download left in the cache is replaced rather than included.
if(NOT PHOSPHOR_DEPS_ARCHIVES AND NOT COMMAND CPMAddPackage)
    if(NOT DEFINED CPM_SOURCE_CACHE AND NOT DEFINED ENV{CPM_SOURCE_CACHE})
        set(CPM_SOURCE_CACHE "${_PHOSPHOR_DEPS_ROOT}/.cache/cpm" CACHE PATH
            "Where CPM.cmake keeps downloaded packages, shared by every build directory")
    endif()
    if(DEFINED CPM_SOURCE_CACHE)
        set(_phosphor_cpm_file "${CPM_SOURCE_CACHE}/cpm/CPM_${PHOSPHOR_CPM_VERSION}.cmake")
    else()
        set(_phosphor_cpm_file "$ENV{CPM_SOURCE_CACHE}/cpm/CPM_${PHOSPHOR_CPM_VERSION}.cmake")
    endif()
    set(_phosphor_cpm_have "")
    if(EXISTS "${_phosphor_cpm_file}")
        file(SHA256 "${_phosphor_cpm_file}" _phosphor_cpm_have)
    endif()
    if(NOT _phosphor_cpm_have STREQUAL PHOSPHOR_CPM_SHA256)
        file(DOWNLOAD
            "https://github.com/cpm-cmake/CPM.cmake/releases/download/v${PHOSPHOR_CPM_VERSION}/CPM.cmake"
            "${_phosphor_cpm_file}.part" EXPECTED_HASH SHA256=${PHOSPHOR_CPM_SHA256} STATUS _phosphor_cpm_status)
        list(GET _phosphor_cpm_status 0 _phosphor_cpm_code)
        if(NOT _phosphor_cpm_code EQUAL 0)
            file(REMOVE "${_phosphor_cpm_file}.part")
            message(FATAL_ERROR
                "Could not download CPM.cmake (${_phosphor_cpm_status}). Configure with network access once, "
                "or pass -DPHOSPHOR_DEPS_ARCHIVES=<dir> with the archives cmake/PhosphorDependencies.cmake pins.")
        endif()
        file(RENAME "${_phosphor_cpm_file}.part" "${_phosphor_cpm_file}")
    endif()
    include("${_phosphor_cpm_file}")
endif()

# phosphor_fetch_dependency(<name>)
#
# Makes the pinned archive <name> (one of PHOSPHOR_DEPS or PHOSPHOR_TEST_DEPS)
# available and sets <name>_SOURCE_DIR in the caller's scope to its unpacked
# tree: the archive's contents, with its single top-level directory removed.
# Only fetches the source. What to build from it is the caller's business.
function(phosphor_fetch_dependency name)
    if(NOT name IN_LIST PHOSPHOR_DEPS AND NOT name IN_LIST PHOSPHOR_TEST_DEPS)
        message(FATAL_ERROR "phosphor_fetch_dependency: '${name}' is not pinned in ${CMAKE_CURRENT_FUNCTION_LIST_FILE}")
    endif()
    set(_version "${PHOSPHOR_DEP_${name}_VERSION}")
    set(_sha "${PHOSPHOR_DEP_${name}_SHA256}")

    if(PHOSPHOR_DEPS_ARCHIVES)
        set(_archive "${PHOSPHOR_DEPS_ARCHIVES}/${PHOSPHOR_DEP_${name}_FILE}")
        if(NOT EXISTS "${_archive}")
            message(FATAL_ERROR
                "PHOSPHOR_DEPS_ARCHIVES is ${PHOSPHOR_DEPS_ARCHIVES}, which has no ${PHOSPHOR_DEP_${name}_FILE}. "
                "Fetch it from ${PHOSPHOR_DEP_${name}_URL}, or fill the directory with "
                "cmake -DPHOSPHOR_DEPS_FETCH=<dir> -P cmake/PhosphorDependencies.cmake")
        endif()
        file(SHA256 "${_archive}" _have)
        if(NOT _have STREQUAL _sha)
            message(FATAL_ERROR
                "${_archive} does not match its pin:\n  expected ${_sha}\n  got      ${_have}")
        endif()
        # Extracted once per pinned version. The stamp is written after the
        # extraction finishes, so one cut short (disk full, ^C) is redone on the
        # next configure instead of passing as complete. The hash is part of
        # the stamp, so a re-pinned archive under the same version is
        # extracted again.
        set(_root "${CMAKE_BINARY_DIR}/_phosphor_deps")
        set(_dest "${_root}/${name}-${_version}")
        set(_stamp "${_root}/.${name}-${_version}-${_sha}")
        if(NOT EXISTS "${_stamp}")
            file(REMOVE_RECURSE "${_dest}" "${_dest}.tmp")
            file(ARCHIVE_EXTRACT INPUT "${_archive}" DESTINATION "${_dest}.tmp")
            file(GLOB _top LIST_DIRECTORIES true "${_dest}.tmp/*")
            list(LENGTH _top _count)
            if(_count EQUAL 1 AND IS_DIRECTORY "${_top}")
                file(RENAME "${_top}" "${_dest}")
                file(REMOVE_RECURSE "${_dest}.tmp")
            else()
                file(RENAME "${_dest}.tmp" "${_dest}")
            endif()
            file(TOUCH "${_stamp}")
        endif()
        set(${name}_SOURCE_DIR "${_dest}" PARENT_SCOPE)
        return()
    endif()

    CPMAddPackage(NAME ${name} VERSION ${_version}
                  URL "${PHOSPHOR_DEP_${name}_URL}"
                  URL_HASH SHA256=${_sha}
                  DOWNLOAD_ONLY YES)
    set(${name}_SOURCE_DIR "${${name}_SOURCE_DIR}" PARENT_SCOPE)
endfunction()
