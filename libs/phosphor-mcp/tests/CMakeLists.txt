# SPDX-FileCopyrightText: 2026 Phosphor Works Team
# SPDX-License-Identifier: LGPL-2.1-or-later

find_package(Qt6 REQUIRED COMPONENTS Test)

# The repo-root isolation module, with stand-ins for an extracted subtree.
# phosphor-ipc's tests/CMakeLists.txt explains each case.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../../cmake/PhosphorTestIsolation.cmake")
    include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/PhosphorTestIsolation.cmake)
else()
    function(phosphor_apply_test_isolation _target)
    endfunction()
    function(phosphor_append_test_environment _test_name)
        set_property(TEST ${_test_name} APPEND PROPERTY ENVIRONMENT ${ARGN})
    endfunction()
endif()

function(phosphormcp_add_test name)
    add_executable(${name} ${name}.cpp mcptesthelpers.h)
    target_link_libraries(${name}
        PRIVATE
            PhosphorMcp::PhosphorMcp
            Qt6::Test
    )
    # A unity build wraps even a single source in a generated file that
    # includes it. The test is then not GCC's main file, and its
    # anonymous-namespace fixtures captured in lambdas trip
    # -Wsubobject-linkage, as phosphor-control's tests avoid the same way.
    set_target_properties(${name} PROPERTIES UNITY_BUILD OFF)
    add_test(NAME ${name} COMMAND ${name} WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    phosphor_apply_test_isolation(${name})
    # QTEST_GUILESS_MAIN everywhere: a QCoreApplication, no display. The
    # library's own tests give the server socket pairs and pipes inside the
    # test process. test_phosphor_mcp_example runs the example as a child.
    set_tests_properties(${name} PROPERTIES
        TIMEOUT 60
        LABELS "phosphormcp"
    )
endfunction()

phosphormcp_add_test(test_phosphor_mcp_transport)
phosphormcp_add_test(test_phosphor_mcp_protocol)
phosphormcp_add_test(test_phosphor_mcp_tools)
phosphormcp_add_test(test_phosphor_mcp_calls)
phosphormcp_add_test(test_phosphor_mcp_signals)
# The bound of the argument check: each row that passes it spends the whole
# bound, half a second of a release build and several seconds of a debug
# build with sanitizers.
phosphormcp_add_test(test_phosphor_mcp_bounds)
set_tests_properties(test_phosphor_mcp_bounds PROPERTIES TIMEOUT 900)

# The work and the memory that the checker charges for each step, counted
# exactly. The checker is private to the library, whose symbols are hidden,
# so its objects are linked into the test.
add_executable(test_phosphor_mcp_checkwork test_phosphor_mcp_checkwork.cpp)
target_link_libraries(test_phosphor_mcp_checkwork PRIVATE PhosphorMcpSchemaChecker Qt6::Test)
set_target_properties(test_phosphor_mcp_checkwork PROPERTIES UNITY_BUILD OFF)
add_test(NAME test_phosphor_mcp_checkwork COMMAND test_phosphor_mcp_checkwork
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
phosphor_apply_test_isolation(test_phosphor_mcp_checkwork)
# Two of its rows walk a value to the bound, some 40 million units each:
# a few seconds of a release build, and some twenty seconds of a debug build
# with sanitizers on a busy machine.
set_tests_properties(test_phosphor_mcp_checkwork PROPERTIES
    TIMEOUT 300
    LABELS "phosphormcp"
)

# The example server (examples/phosphor-mcp/wordcount.cpp), built here as a
# test fixture and driven as a client would drive it, so the example cannot
# fall behind the library. The examples themselves are built only with
# PHOSPHOR_LIBS_BUILD_EXAMPLES. A library configured on its own, outside the
# repository, has no examples/ and skips this test.
set(_phosphormcp_example_source "${CMAKE_CURRENT_LIST_DIR}/../../../examples/phosphor-mcp/wordcount.cpp")
if(EXISTS "${_phosphormcp_example_source}")
    add_executable(phosphor-mcp-example-fixture "${_phosphormcp_example_source}")
    target_link_libraries(phosphor-mcp-example-fixture PRIVATE PhosphorMcp::PhosphorMcp Qt6::Core)
    target_compile_definitions(phosphor-mcp-example-fixture PRIVATE PHOSPHOR_MCP_EXAMPLE_VERSION="${PHOSPHORMCP_VERSION}")
    phosphormcp_add_test(test_phosphor_mcp_example)
    target_compile_definitions(test_phosphor_mcp_example
        PRIVATE
            PHOSPHOR_MCP_EXAMPLE_PATH="$<TARGET_FILE:phosphor-mcp-example-fixture>"
            PHOSPHORMCP_VERSION="${PHOSPHORMCP_VERSION}")
    add_dependencies(test_phosphor_mcp_example phosphor-mcp-example-fixture)
else()
    message(STATUS "phosphor-mcp: test_phosphor_mcp_example is not built: no examples/phosphor-mcp in this tree")
endif()
