# SPDX-FileCopyrightText: 2026 Phosphor Works Team
# SPDX-License-Identifier: LGPL-2.1-or-later

add_executable(phosphor-mcp-example wordcount.cpp)
target_link_libraries(phosphor-mcp-example PRIVATE PhosphorMcp::PhosphorMcp Qt6::Core)
# The version the server gives clients is the library's, so the two cannot
# drift. It comes from the target: the variable PHOSPHORMCP_VERSION is set in
# the directory of the library and is not visible here. The smoke test
# builds this source again as its fixture and checks the same version.
get_target_property(_phosphor_mcp_example_version PhosphorMcp VERSION)
if(NOT _phosphor_mcp_example_version)
    message(FATAL_ERROR "The target PhosphorMcp has no VERSION property for the example's version.")
endif()
target_compile_definitions(phosphor-mcp-example
    PRIVATE PHOSPHOR_MCP_EXAMPLE_VERSION="${_phosphor_mcp_example_version}")
