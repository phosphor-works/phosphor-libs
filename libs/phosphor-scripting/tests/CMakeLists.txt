# SPDX-FileCopyrightText: 2026 Phosphor Works Team
# SPDX-License-Identifier: LGPL-2.1-or-later

# PhosphorScripting unit tests. Headless and Core-only.

find_package(Qt6 REQUIRED COMPONENTS Test)
find_package(Threads REQUIRED)

# CMAKE_CURRENT_LIST_DIR, not CMAKE_SOURCE_DIR: the latter is this LIBRARY's
# directory in a standalone configure. The EXISTS guard covers an extracted
# subtree, where the repo-root module is not present.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../../cmake/PhosphorTestIsolation.cmake")
    include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/PhosphorTestIsolation.cmake)
else()
    function(phosphor_apply_test_isolation _target)
    endfunction()
endif()

function(phosphor_scripting_add_test _name)
    add_executable(${_name} ${ARGN})
    set_target_properties(${_name} PROPERTIES AUTOMOC ON)
    target_link_libraries(${_name}
        PRIVATE
            Qt6::Test
            Qt6::Core
            Threads::Threads
            PhosphorScripting::PhosphorScripting
    )
    add_test(NAME ${_name} COMMAND ${_name})
    phosphor_apply_test_isolation(${_name})
    set_tests_properties(${_name} PROPERTIES LABELS "phosphorscripting")
endfunction()

phosphor_scripting_add_test(phosphor_scripting_test_watchdog test_watchdog.cpp)
