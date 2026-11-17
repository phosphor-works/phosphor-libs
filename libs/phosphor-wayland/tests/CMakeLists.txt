# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later

# PhosphorWayland unit tests, on the offscreen platform: no compositor, so no
# protocol traffic. They cover what the library decides without one.

find_package(Qt6 REQUIRED COMPONENTS Test)

# Shared test isolation. See phosphor-geometry/tests/CMakeLists.txt for why the
# include walks up from CMAKE_CURRENT_LIST_DIR and why the stand-ins exist.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../../cmake/PhosphorTestIsolation.cmake")
    include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/PhosphorTestIsolation.cmake)
else()
    function(phosphor_apply_test_isolation _target)
    endfunction()
    function(phosphor_append_test_environment _test_name)
        set_property(TEST ${_test_name} APPEND PROPERTY ENVIRONMENT ${ARGN})
    endfunction()
endif()

function(phosphorwayland_add_test _name)
    add_executable(${_name} ${_name}.cpp)
    set_target_properties(${_name} PROPERTIES AUTOMOC ON)
    target_compile_features(${_name} PRIVATE cxx_std_20)
    target_link_libraries(${_name}
        PRIVATE
            Qt6::Test
            Qt6::Gui
            PhosphorWayland::PhosphorWayland
    )
    add_test(NAME ${_name} COMMAND ${_name})
    phosphor_apply_test_isolation(${_name})
    set_tests_properties(${_name} PROPERTIES LABELS "phosphorwayland")
    # APPEND, never PROPERTIES ENVIRONMENT: a plain set would replace the XDG
    # sandbox phosphor_apply_test_isolation just installed.
    phosphor_append_test_environment(${_name} "QT_QPA_PLATFORM=offscreen")
endfunction()

phosphorwayland_add_test(test_backgroundeffect)
