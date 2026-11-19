// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <QLatin1StringView>

// The constants of the Model Context Protocol that applications and tests
// name. The library speaks exactly one revision. A server/discover,
// tools/list or tools/call request that names another one is refused with
// UnsupportedProtocolVersion. initialize, the handshake of older revisions,
// and every unknown method get MethodNotFound whatever revision they name.

namespace PhosphorMcp {

inline constexpr QLatin1StringView ProtocolVersion{"2026-07-28"};

// The keys of a request's params._meta that the protocol defines. The first
// two are required in every request. clientInfo and progressToken are
// optional (McpCall::requestMeta() has them).
inline constexpr QLatin1StringView MetaProtocolVersion{"io.modelcontextprotocol/protocolVersion"};
inline constexpr QLatin1StringView MetaClientCapabilities{"io.modelcontextprotocol/clientCapabilities"};
inline constexpr QLatin1StringView MetaClientInfo{"io.modelcontextprotocol/clientInfo"};
inline constexpr QLatin1StringView MetaProgressToken{"progressToken"};

// The key of a result's _meta that holds the server's name and version.
inline constexpr QLatin1StringView MetaServerInfo{"io.modelcontextprotocol/serverInfo"};

// The JSON-RPC error codes, and the codes that MCP adds which this server
// sends.
namespace ErrorCode {
inline constexpr int ParseError = -32700;
inline constexpr int InvalidRequest = -32600;
inline constexpr int MethodNotFound = -32601;
inline constexpr int InvalidParams = -32602;
inline constexpr int InternalError = -32603;
inline constexpr int MissingRequiredClientCapability = -32021;
inline constexpr int UnsupportedProtocolVersion = -32022;
} // namespace ErrorCode

} // namespace PhosphorMcp
