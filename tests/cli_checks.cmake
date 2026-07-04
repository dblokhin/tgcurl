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

# Auth modes run against a throwaway, empty config directory so they never
# touch a real session and start from a known "no config" state.
if(MODE MATCHES "^(login_headless|logout_noconfig)$")
  set(SCRATCH "${CMAKE_CURRENT_BINARY_DIR}/cli_scratch_${MODE}")
  file(REMOVE_RECURSE "${SCRATCH}")
  set(ENV{TGCURL_CONFIG_DIR} "${SCRATCH}")
endif()

if(MODE STREQUAL "unknown")
  set(ARGS "definitely-not-a-command")
elseif(MODE STREQUAL "login_headless")
  set(ARGS "login")
elseif(MODE STREQUAL "logout_noconfig")
  set(ARGS "logout")
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
