# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later

add_executable(phosphor-mcp-example wordcount.cpp)
target_link_libraries(phosphor-mcp-example PRIVATE PhosphorMcp::PhosphorMcp Qt6::Core)
