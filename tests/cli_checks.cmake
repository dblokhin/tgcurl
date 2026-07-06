# CLI-level assertions run by ctest. Invokes the built tgcurl binary and
# checks its behavior. Params: TGCURL (path to binary), MODE.
#
# Modes:
#   unknown          - an unknown subcommand -> JSON error, non-zero exit.
#   usage            - no subcommand -> JSON error, non-zero exit.
#   login_headless   - `login` with stdin closed and an empty config dir must
#                      fail with a JSON error and NOT hang (proves head-less
#                      runs never block waiting on stdin).
#   logout_noconfig  - `logout` with an empty config dir prints {"ok":true} and
#                      exits 0 (nothing to log out of; db cleared).
#   status_noconfig  - `status` with an empty config dir prints
#                      {"authorized":false,...} and exits 0 without prompting
#                      ("not logged in" is the answer, not an error).
#   chats_bad_limit  - `chats list --limit abc` -> usage error before any
#                      network (arg validation runs first).
#   contacts_bad_sub - `contacts frobnicate` -> usage error.
#   send_unresolvable- `send "John Smith" "x"` -> unresolvable error before any
#                      network (a free-text name can't be addressed).
#   chat_unresolvable- `chat "John Smith"` -> unresolvable error.
#   contacts_new_bad - `contacts new` (missing args) -> usage error pre-network.
#   contacts_block_unresolvable - `contacts block "John Smith"` -> unresolvable.
#   search_unresolvable - `search "q" --chat "John Smith"` -> unresolvable
#                      before any network.
#   sendfile_missing - `sendfile 42 /nonexistent` -> file_not_found before any
#                      network (file validated offline).
#   login_quiet      - `login` with a seeded (fake) config so TdClient is
#                      constructed and the flow reaches the phone prompt; asserts
#                      stderr carries NO TDLib logs (they are silenced by
#                      default, otherwise they'd bury the prompts).
#   mcp              - `-mcp` with an initialize/tools-list/tools-call script on
#                      stdin: the JSON-RPC handshake answers, tools/list carries
#                      the registry, a tools/call that fails pre-network returns
#                      an isError result, and EOF exits 0. Runs against an empty
#                      config dir — none of this needs a session.

# Auth modes run against a throwaway, empty config directory so they never
# touch a real session and start from a known "no config" state.
if(MODE MATCHES "^(login_headless|logout_noconfig|status_noconfig|chats_bad_limit|contacts_bad_sub|send_unresolvable|chat_unresolvable|contacts_new_bad|contacts_block_unresolvable|search_unresolvable|sendfile_missing|login_quiet|mcp)$")
  set(SCRATCH "${CMAKE_CURRENT_BINARY_DIR}/cli_scratch_${MODE}")
  file(REMOVE_RECURSE "${SCRATCH}")
  set(ENV{TGCURL_CONFIG_DIR} "${SCRATCH}")
endif()

# login_quiet needs a config so login gets past api_id/api_hash and actually
# spins up TDLib (which is where the log noise would come from). Seed a fake one.
if(MODE STREQUAL "login_quiet")
  file(MAKE_DIRECTORY "${SCRATCH}")
  file(WRITE "${SCRATCH}/config.json" "{\"api_id\": 12345678, \"api_hash\": \"abcdef0123456789abcdef0123456789\"}")
endif()

if(MODE STREQUAL "unknown")
  set(ARGS "definitely-not-a-command")
elseif(MODE STREQUAL "login_headless")
  set(ARGS "login")
elseif(MODE STREQUAL "logout_noconfig")
  set(ARGS "logout")
elseif(MODE STREQUAL "status_noconfig")
  set(ARGS "status")
elseif(MODE STREQUAL "chats_bad_limit")
  set(ARGS "chats;list;--limit;abc")
elseif(MODE STREQUAL "contacts_bad_sub")
  set(ARGS "contacts;frobnicate")
elseif(MODE STREQUAL "send_unresolvable")
  set(ARGS "send;John Smith;hello")
elseif(MODE STREQUAL "chat_unresolvable")
  set(ARGS "chat;John Smith")
elseif(MODE STREQUAL "contacts_new_bad")
  set(ARGS "contacts;new")
elseif(MODE STREQUAL "contacts_block_unresolvable")
  set(ARGS "contacts;block;John Smith")
elseif(MODE STREQUAL "search_unresolvable")
  set(ARGS "search;hello;--chat;John Smith")
elseif(MODE STREQUAL "sendfile_missing")
  set(ARGS "sendfile;42;/definitely/not/a/file.bin")
elseif(MODE STREQUAL "login_quiet")
  set(ARGS "login")
elseif(MODE STREQUAL "mcp")
  set(ARGS "-mcp")
else() # usage: no args
  set(ARGS "")
endif()

# Feed an empty stdin (via a written-empty file) so `login` sees immediate EOF
# rather than a live TTY — this is the "head-less" condition we assert on.
# The mcp mode instead feeds a scripted JSON-RPC session.
set(EMPTY_IN "${CMAKE_CURRENT_BINARY_DIR}/cli_empty_stdin")
file(WRITE "${EMPTY_IN}" "")
if(MODE STREQUAL "mcp")
  set(EMPTY_IN "${CMAKE_CURRENT_BINARY_DIR}/cli_mcp_stdin")
  file(WRITE "${EMPTY_IN}" "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{},\"clientInfo\":{\"name\":\"ctest\",\"version\":\"0\"}}}
{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}
{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}
{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"send_message\",\"arguments\":{\"id\":\"John Smith\",\"text\":\"hello\"}}}
")
endif()

# login_quiet spins up a real TDLib client that talks to the network before the
# prompt appears; under a loaded machine that alone can take >20s, so it gets a
# larger budget (a timeout kills the process silently and fails the test).
set(RUN_TIMEOUT 20)
if(MODE STREQUAL "login_quiet")
  set(RUN_TIMEOUT 60)
endif()

execute_process(
  COMMAND ${TGCURL} ${ARGS}
  INPUT_FILE "${EMPTY_IN}"
  RESULT_VARIABLE code
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
  TIMEOUT ${RUN_TIMEOUT})

if(MODE STREQUAL "mcp")
  # Clean shutdown on EOF.
  if(NOT code EQUAL 0)
    message(FATAL_ERROR "mcp: expected exit 0, got ${code}; err=${err}")
  endif()
  # initialize answered with our server info.
  if(NOT out MATCHES "\"serverInfo\":{\"name\":\"tgcurl\"")
    message(FATAL_ERROR "mcp: initialize response missing serverInfo; got: ${out}")
  endif()
  # tools/list exposes the registry (spot-check two tools and a schema).
  if(NOT out MATCHES "\"name\":\"send_message\"" OR NOT out MATCHES "\"name\":\"contacts_list\"")
    message(FATAL_ERROR "mcp: tools/list missing expected tools; got: ${out}")
  endif()
  if(NOT out MATCHES "\"inputSchema\":{\"type\":\"object\"")
    message(FATAL_ERROR "mcp: tools/list missing inputSchema; got: ${out}")
  endif()
  # The session-lifecycle commands must NOT be exposed as tools (CLI-only).
  foreach(cli_only login logout status)
    if(out MATCHES "\"name\":\"${cli_only}\"")
      message(FATAL_ERROR "mcp: ${cli_only} must not be an MCP tool; got: ${out}")
    endif()
  endforeach()
  # The failing tools/call surfaces as an isError result, not a dead session.
  if(NOT out MATCHES "\"isError\":true" OR NOT out MATCHES "unresolvable")
    message(FATAL_ERROR "mcp: expected an isError unresolvable tool result; got: ${out}")
  endif()
  # Protocol traffic stays off stderr; the human-facing startup notice is ON
  # stderr (stdout would corrupt the protocol stream).
  if(err MATCHES "jsonrpc")
    message(FATAL_ERROR "mcp: JSON-RPC leaked onto stderr; got: ${err}")
  endif()
  if(NOT err MATCHES "MCP server ready")
    message(FATAL_ERROR "mcp: expected the startup notice on stderr; got: ${err}")
  endif()
  if(out MATCHES "MCP server ready")
    message(FATAL_ERROR "mcp: startup notice leaked onto stdout (protocol stream); got: ${out}")
  endif()
  return()
endif()

if(MODE STREQUAL "logout_noconfig")
  # Success path: exit 0 and {"ok":true} on stdout.
  if(NOT code EQUAL 0)
    message(FATAL_ERROR "logout_noconfig: expected exit 0, got ${code}; err=${err}")
  endif()
  if(NOT out MATCHES "\"ok\":true")
    message(FATAL_ERROR "logout_noconfig: expected {\"ok\":true} on stdout; got: ${out}")
  endif()
  return()
endif()

if(MODE STREQUAL "status_noconfig")
  # Success path: "not logged in" is the answer, not an error.
  if(NOT code EQUAL 0)
    message(FATAL_ERROR "status_noconfig: expected exit 0, got ${code}; err=${err}")
  endif()
  if(NOT out MATCHES "\"authorized\":false")
    message(FATAL_ERROR "status_noconfig: expected {\"authorized\":false} on stdout; got: ${out}")
  endif()
  return()
endif()

# All other modes are error paths: non-zero exit + a JSON error on stderr.
if(code EQUAL 0)
  message(FATAL_ERROR "expected non-zero exit, got 0 (mode=${MODE})")
endif()

if(NOT err MATCHES "\"error\"")
  message(FATAL_ERROR "expected a JSON error on stderr (mode=${MODE}); got: ${err}")
endif()

# The unresolvable modes must fail specifically with that error code, proving
# arg validation runs before (and independently of) any session.
if(MODE MATCHES "unresolvable")
  if(NOT err MATCHES "unresolvable")
    message(FATAL_ERROR "expected an 'unresolvable' error (mode=${MODE}); got: ${err}")
  endif()
endif()

# sendfile validates the file offline, before any session.
if(MODE STREQUAL "sendfile_missing")
  if(NOT err MATCHES "file_not_found")
    message(FATAL_ERROR "expected a 'file_not_found' error; got: ${err}")
  endif()
endif()

# login_quiet: TDLib is constructed but its logs must be silenced by default,
# so the interactive prompts (also on stderr) stay visible. Assert no TDLib log
# markers leaked onto stderr.
if(MODE STREQUAL "login_quiet")
  if(err MATCHES "Td\\.cpp" OR err MATCHES "Client\\.cpp" OR err MATCHES "td_requests")
    message(FATAL_ERROR "TDLib logs leaked onto stderr (should be silenced); got: ${err}")
  endif()
  # And the phone prompt should be present (proves we reached the auth flow).
  if(NOT err MATCHES "Phone number")
    message(FATAL_ERROR "expected the phone-number prompt on stderr; got: ${err}")
  endif()
endif()
