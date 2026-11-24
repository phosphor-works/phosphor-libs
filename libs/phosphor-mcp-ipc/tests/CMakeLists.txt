# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later

find_package(Qt6 REQUIRED COMPONENTS Test Network Qml)

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

function(phosphormcpipc_add_test name)
    add_executable(${name} ${name}.cpp mcpipctesthelpers.h)
    # The tests drive a real IpcRouter with targets of their own, C++ ones
    # and a QML IpcTarget, and the MCP side through the helpers of
    # phosphor-mcp's tests: the client end of a socket pair.
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../phosphor-mcp/tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../../phosphor-ipc/tests
    )
    target_link_libraries(${name}
        PRIVATE
            PhosphorMcpIpc::PhosphorMcpIpc
            PhosphorIpc::PhosphorIpc
            Qt6::Network
            Qt6::Qml
            Qt6::Test
    )
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
phosphormcpipc_add_test(test_phosphor_mcp_ipc_bridge)
# The program, run as an MCP client runs it.
phosphormcpipc_add_test(test_phosphor_mcp_ipc_program)
target_compile_definitions(test_phosphor_mcp_ipc_program
    PRIVATE PHOSPHOR_MCP_IPC_PATH="$<TARGET_FILE:phosphor-mcp-ipc>")
add_dependencies(test_phosphor_mcp_ipc_program phosphor-mcp-ipc)
