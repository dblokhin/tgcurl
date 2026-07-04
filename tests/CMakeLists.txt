# Unit tests (no external framework; see test_util.h).

add_executable(test_json_out test_json_out.cpp)
target_link_libraries(test_json_out PRIVATE tgcurl_core)
add_test(NAME json_out COMMAND test_json_out)

# tdclient touches TDLib, so this test links the TD-backed library.
add_executable(test_tdclient test_tdclient.cpp)
target_link_libraries(test_tdclient PRIVATE tgcurl_td)
add_test(NAME tdclient COMMAND test_tdclient)

# --- CLI-level dispatch checks (exercise the real binary) -------------------
# Unknown subcommand -> non-zero exit and a JSON error on stderr.
add_test(NAME cli_unknown_command
  COMMAND ${CMAKE_COMMAND}
    -DTGCURL=$<TARGET_FILE:tgcurl>
    -DMODE=unknown
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cli_checks.cmake)

# No subcommand -> usage error, non-zero exit.
add_test(NAME cli_no_command
  COMMAND ${CMAKE_COMMAND}
    -DTGCURL=$<TARGET_FILE:tgcurl>
    -DMODE=usage
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cli_checks.cmake)
