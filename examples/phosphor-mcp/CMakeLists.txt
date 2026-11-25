# SPDX-FileCopyrightText: 2026 Phosphor Works Team
# SPDX-License-Identifier: LGPL-2.1-or-later

add_executable(phosphor-mcp-example wordcount.cpp)
target_link_libraries(phosphor-mcp-example PRIVATE PhosphorMcp::PhosphorMcp Qt6::Core)
# The version the server gives clients is the library's, so the two cannot
# drift. The smoke test checks it against the same variable.
target_compile_definitions(phosphor-mcp-example PRIVATE PHOSPHOR_MCP_EXAMPLE_VERSION="${PHOSPHORMCP_VERSION}")
