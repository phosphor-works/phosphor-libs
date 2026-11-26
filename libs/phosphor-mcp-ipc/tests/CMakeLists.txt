# SPDX-FileCopyrightText: 2026 Phosphor Works Team
# SPDX-License-Identifier: LGPL-2.1-or-later

find_package(Qt6 REQUIRED COMPONENTS Test Network Qml)

# The repo-root isolation module, with stand-ins for a tree without it.
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

# The manifest test needs only the library. The other two need a router and
# an MCP client, and take both from the test helpers of the two sibling
# libraries. The ROUTER option adds those.
function(phosphormcpipc_add_test name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "ROUTER" "" "")
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name}
        PRIVATE
            PhosphorMcpIpc::PhosphorMcpIpc
            Qt6::Test
    )
    if(arg_ROUTER)
        # The header holds the Q_OBJECT class of the application's target,
        # so it is a source: AUTOMOC then reads it.
        target_sources(${name} PRIVATE mcpipctesthelpers.h)
        # The application side is a real IpcRouter with a C++ target and a
        # QML IpcTarget. The MCP side is the client end of a socket pair.
        target_include_directories(${name} PRIVATE
            ${PHOSPHORMCPIPC_MCP_TESTS_DIR}
            ${PHOSPHORMCPIPC_IPC_TESTS_DIR}
        )
        target_link_libraries(${name}
            PRIVATE
                PhosphorIpc::PhosphorIpc
                PhosphorMcp::PhosphorMcp
                Qt6::Network
                Qt6::Qml
        )
    endif()
    # As phosphor-mcp's tests: a unity build makes the test no longer GCC's
    # main file, and anonymous-namespace fixtures then trip
    # -Wsubobject-linkage.
    set_target_properties(${name} PROPERTIES UNITY_BUILD OFF)
    add_test(NAME ${name} COMMAND ${name} WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    phosphor_apply_test_isolation(${name})
    # QTEST_GUILESS_MAIN: a QCoreApplication, no display, also for the
    # QQmlEngine of the QML target.
    set_tests_properties(${name} PROPERTIES
        TIMEOUT 60
        LABELS "phosphormcpipc"
    )
endfunction()

phosphormcpipc_add_test(test_phosphor_mcp_ipc_manifest)

# A tree that holds this library alone has no sibling test helpers.
set(PHOSPHORMCPIPC_MCP_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../phosphor-mcp/tests")
set(PHOSPHORMCPIPC_IPC_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../phosphor-ipc/tests")
if(NOT EXISTS "${PHOSPHORMCPIPC_MCP_TESTS_DIR}/mcptesthelpers.h"
   OR NOT EXISTS "${PHOSPHORMCPIPC_IPC_TESTS_DIR}/ipctesthelpers.h")
    message(STATUS "phosphor-mcp-ipc: the test helpers of phosphor-mcp and phosphor-ipc are not in this tree, "
                   "so the bridge test and the program test are skipped")
    return()
endif()

phosphormcpipc_add_test(test_phosphor_mcp_ipc_bridge ROUTER)
# The program, run as an MCP client runs it.
phosphormcpipc_add_test(test_phosphor_mcp_ipc_program ROUTER)
target_compile_definitions(test_phosphor_mcp_ipc_program PRIVATE
    PHOSPHOR_MCP_IPC_PATH="$<TARGET_FILE:phosphor-mcp-ipc>"
    PHOSPHORMCPIPC_VERSION="${PHOSPHORMCPIPC_VERSION}"
)
add_dependencies(test_phosphor_mcp_ipc_program phosphor-mcp-ipc)
