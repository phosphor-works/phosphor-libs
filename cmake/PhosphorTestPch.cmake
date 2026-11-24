# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# phosphor_share_test_pch()
#
# One precompiled header, built once, that every test target in the tree
# reuses (target_precompile_headers REUSE_FROM). The tree has around six
# hundred test executables of one or two sources each. A per-target PCH would
# cost more than it saves on a target that small, so without a shared one each
# test parsed QtCore and the standard library from scratch, which was most of
# its compile.
#
# Call it from a project's top CMakeLists after every tier has been added, and
# after the library trees that project adds: it walks the calling directory and
# everything below it for the targets to attach, so a test declared after the
# call, or outside that directory, is not covered. Each app calls it for its own
# tree, as the top-level project or inside a superbuild. A superbuild of several
# apps then has one PCH per app, each built with that app's compile flags
# (KDECompilerSettings' -fno-exceptions, for one), which a single PCH declared
# at the superbuild's root could not match.
#
# What the header may hold is set by how GCC validates a PCH. Every macro the
# PCH was built with must be defined, identically, when it is used, and a
# consumer's extra macros are fine as long as no header in the PCH tests them.
# Test targets differ in exactly two macro families:
#   * QT_TESTCASE_BUILDDIR / QT_TESTCASE_SOURCEDIR, which Qt6::Test defines per
#     directory, and
#   * the QT_<MODULE>_LIB set (QT_GUI_LIB, QT_QUICK_LIB, ...), which follows the
#     modules each test links.
# QtTest's own headers test both, so no QtTest header can be in the PCH, and
# the PCH target links Qt6::Core alone so that QT_CORE_LIB, which every test
# has, is its only module macro. It is declared in the calling directory, so
# its other definitions are the ones every target below it inherits. The
# header list is QtCore and standard headers the test sources include most.
#
# A target the PCH does not fit is not broken by it, because GCC then ignores
# the PCH and parses the header as text, with a -Winvalid-pch warning. GCC
# rejects a PCH built with other position-independence flags, so there is one
# host for each: the plain one, a -fPIE one for the test executables of the
# library directories that turn on CMAKE_POSITION_INDEPENDENT_CODE, and a
# -fPIC one for object libraries there. Arch's GCC compiles PIE by default, so
# there the plain and the -fPIE host agree anyway, but Fedora's does not. A
# host is made only when a target needs it. A target with a precompiled header
# of its own keeps it, and one that sets DISABLE_PRECOMPILE_HEADERS goes
# without.
#
# GCC only. Clang validates a PCH strictly and fails the compile on the
# mismatches GCC tolerates (a PIC level, a macro the PCH did not have), so a
# Clang build keeps compiling each test without one.
#
# A test can compile against a header it never included, because the PCH
# included it. The non-unity, ENABLE_PCH=OFF build is the gate for that, as it
# already is for the same hole unity builds open.

include_guard(GLOBAL)

# Appends every non-imported target declared in <dir> and its subdirectories to
# the list named <out>.
function(_phosphor_targets_below dir out)
    get_property(_targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    get_property(_subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(_sub IN LISTS _subdirs)
        _phosphor_targets_below("${_sub}" _below)
        list(APPEND _targets ${_below})
    endforeach()
    set(${out} ${_targets} PARENT_SCOPE)
endfunction()

# Sets <out> to the PCH host for one position-independence variant: "" (none),
# "PIE" or "PIC". The host is made on first use, in the directory of the
# phosphor_share_test_pch() call, so it inherits that directory's definitions.
function(_phosphor_test_pch_host variant out)
    # Named for the calling project, so each app in a superbuild has its own.
    set(_pch "${PROJECT_NAME}_test_pch")
    if(variant)
        string(TOLOWER "${variant}" _suffix)
        string(APPEND _pch "_${_suffix}")
    endif()
    if(NOT TARGET ${_pch})
        # The PCH target needs one source to compile. Written with
        # file(CONFIGURE) so a reconfigure leaves it untouched and does not
        # rebuild the PCH.
        set(_src "${CMAKE_CURRENT_BINARY_DIR}/${_pch}.cpp")
        file(CONFIGURE OUTPUT "${_src}" CONTENT "// Host for the shared test PCH (cmake/PhosphorTestPch.cmake).\n")
        add_library(${_pch} OBJECT "${_src}")
        target_link_libraries(${_pch} PRIVATE Qt6::Core)
        # POSITION_INDEPENDENT_CODE would give an object library the PIC flag
        # for the PIE variant too, so the flag comes from the compiler's own
        # option list instead.
        set_target_properties(${_pch} PROPERTIES
            AUTOMOC OFF
            AUTOUIC OFF
            AUTORCC OFF
            UNITY_BUILD OFF
            POSITION_INDEPENDENT_CODE OFF)
        if(variant)
            target_compile_options(${_pch} PRIVATE ${CMAKE_CXX_COMPILE_OPTIONS_${variant}})
        endif()
        target_precompile_headers(${_pch} PRIVATE
            <QCoreApplication>
            <QDebug>
            <QDir>
            <QFile>
            <QFileInfo>
            <QHash>
            <QJsonArray>
            <QJsonDocument>
            <QJsonObject>
            <QJsonValue>
            <QList>
            <QMap>
            <QObject>
            <QPoint>
            <QPointer>
            <QRect>
            <QRectF>
            <QRegularExpression>
            <QScopeGuard>
            <QSet>
            <QSize>
            <QStandardPaths>
            <QString>
            <QStringList>
            <QTemporaryDir>
            <QTimer>
            <QUrl>
            <QUuid>
            <QVariant>
            <QVariantMap>
            <algorithm>
            <functional>
            <limits>
            <memory>
            <optional>
            <vector>
        )
    endif()
    set(${out} ${_pch} PARENT_SCOPE)
endfunction()

function(phosphor_share_test_pch)
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        return()
    endif()
    _phosphor_targets_below("${CMAKE_CURRENT_SOURCE_DIR}" _all)
    set(_attached 0)
    foreach(_target IN LISTS _all)
        if(_target MATCHES "^${PROJECT_NAME}_test_pch(_pie|_pic)?$")
            continue()
        endif()
        get_target_property(_type ${_target} TYPE)
        if(NOT _type STREQUAL "EXECUTABLE" AND NOT _type STREQUAL "OBJECT_LIBRARY")
            continue()
        endif()
        # A test target is one declared in a tests/ directory at any depth.
        get_target_property(_dir ${_target} SOURCE_DIR)
        if(NOT _dir MATCHES "/tests(/|$)")
            continue()
        endif()
        get_target_property(_own ${_target} PRECOMPILE_HEADERS)
        get_target_property(_reuse ${_target} PRECOMPILE_HEADERS_REUSE_FROM)
        # A target that opted out builds without one, and must not wait for it.
        get_target_property(_off ${_target} DISABLE_PRECOMPILE_HEADERS)
        if(_own OR _reuse OR _off)
            continue()
        endif()
        # A target that does not compile C++ has nothing to precompile for.
        get_target_property(_sources ${_target} SOURCES)
        if(NOT _sources MATCHES "\\.(cpp|cxx|cc)(;|$)")
            continue()
        endif()
        # The flags the target compiles with: CMake gives an executable the PIE
        # flag and a library the PIC flag when POSITION_INDEPENDENT_CODE is on.
        get_target_property(_pic ${_target} POSITION_INDEPENDENT_CODE)
        set(_variant "")
        if(_pic AND _type STREQUAL "EXECUTABLE")
            set(_variant PIE)
        elseif(_pic)
            set(_variant PIC)
        endif()
        _phosphor_test_pch_host("${_variant}" _pch)
        target_precompile_headers(${_target} REUSE_FROM ${_pch})
        math(EXPR _attached "${_attached} + 1")
    endforeach()
    message(STATUS "Shared test PCH ${PROJECT_NAME}_test_pch: attached to ${_attached} test targets")
endfunction()
