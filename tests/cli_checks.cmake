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
#   chats_bad_limit  - `chats list --limit abc` -> usage error before any
#                      network (arg validation runs first).
#   contacts_bad_sub - `contacts frobnicate` -> usage error.
#   send_unresolvable- `send "John Smith" "x"` -> unresolvable error before any
#                      network (a free-text name can't be addressed).
#   chat_unresolvable- `chat "John Smith"` -> unresolvable error.
#   contacts_new_bad - `contacts new` (missing args) -> usage error pre-network.
#   contacts_block_unresolvable - `contacts block "John Smith"` -> unresolvable.
#   login_quiet      - `login` with a seeded (fake) config so TdClient is
#                      constructed and the flow reaches the phone prompt; asserts
#                      stderr carries NO TDLib logs (they are silenced by
#                      default, otherwise they'd bury the prompts).

# Auth modes run against a throwaway, empty config directory so they never
# touch a real session and start from a known "no config" state.
if(MODE MATCHES "^(login_headless|logout_noconfig|chats_bad_limit|contacts_bad_sub|send_unresolvable|chat_unresolvable|contacts_new_bad|contacts_block_unresolvable|login_quiet)$")
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
elseif(MODE STREQUAL "login_quiet")
  set(ARGS "login")
else() # usage: no args
  set(ARGS "")
endif()

# Feed an empty stdin (via a written-empty file) so `login` sees immediate EOF
# rather than a live TTY — this is the "head-less" condition we assert on.
set(EMPTY_IN "${CMAKE_CURRENT_BINARY_DIR}/cli_empty_stdin")
file(WRITE "${EMPTY_IN}" "")

execute_process(
  COMMAND ${TGCURL} ${ARGS}
  INPUT_FILE "${EMPTY_IN}"
  RESULT_VARIABLE code
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
  TIMEOUT 20)

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
