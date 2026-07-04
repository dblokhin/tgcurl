# CLI-level assertions run by ctest. Invokes the built tgcurl binary and
# checks exit code + that stderr carries a JSON error object.
# Params: TGCURL (path to binary), MODE (unknown|usage).

if(MODE STREQUAL "unknown")
  set(ARGS "definitely-not-a-command")
else() # usage: no args
  set(ARGS "")
endif()

execute_process(
  COMMAND ${TGCURL} ${ARGS}
  RESULT_VARIABLE code
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err)

if(code EQUAL 0)
  message(FATAL_ERROR "expected non-zero exit, got 0 (mode=${MODE})")
endif()

if(NOT err MATCHES "\"error\"")
  message(FATAL_ERROR "expected a JSON error on stderr (mode=${MODE}); got: ${err}")
endif()
