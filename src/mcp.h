// MCP (Model Context Protocol) server mode: `tgcurl -mcp`.
//
// Exposes the command registry as MCP *tools* so an AI agent can drive a
// long-lived tgcurl instead of shelling out per command. The transport is
// MCP's stdio one: JSON-RPC 2.0 messages, one per line, requests on stdin and
// responses on stdout (which is why stdout stays JSON-only everywhere else in
// tgcurl too). Each tools/call runs the exact same handler as the CLI
// subcommand, opening its own one-shot TDLib session — semantics identical to
// running the binary, minus the process spawn. See DESIGN.md → MCP mode.
#ifndef TGCURL_MCP_H
#define TGCURL_MCP_H

#include <istream>
#include <ostream>

namespace tgcurl::mcp {

// Serve MCP over the given streams until EOF on `in`. Returns the process
// exit code (0 on a clean shutdown).
int serve(std::istream& in, std::ostream& out);

} // namespace tgcurl::mcp

#endif // TGCURL_MCP_H
