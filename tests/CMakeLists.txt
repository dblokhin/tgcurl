# Unit tests (no external framework; see test_util.h).

add_executable(test_json_out test_json_out.cpp)
target_link_libraries(test_json_out PRIVATE tgcurl_core)
add_test(NAME json_out COMMAND test_json_out)

# config is pure logic (no TDLib); links the core library.
add_executable(test_config test_config.cpp)
target_link_libraries(test_config PRIVATE tgcurl_core)
add_test(NAME config COMMAND test_config)

# tdclient touches TDLib, so this test links the TD-backed library.
add_executable(test_tdclient test_tdclient.cpp)
target_link_libraries(test_tdclient PRIVATE tgcurl_td)
add_test(NAME tdclient COMMAND test_tdclient)

# auth's head-less prompter contract (the rest of auth is network-dependent).
add_executable(test_auth test_auth.cpp)
target_link_libraries(test_auth PRIVATE tgcurl_td)
add_test(NAME auth COMMAND test_auth)

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

# login with stdin closed + empty config -> JSON error, no hang.
add_test(NAME cli_login_headless
  COMMAND ${CMAKE_COMMAND}
    -DTGCURL=$<TARGET_FILE:tgcurl>
    -DMODE=login_headless
    -DCMAKE_CURRENT_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cli_checks.cmake)

# logout with empty config -> {"ok":true}, exit 0.
add_test(NAME cli_logout_noconfig
  COMMAND ${CMAKE_COMMAND}
    -DTGCURL=$<TARGET_FILE:tgcurl>
    -DMODE=logout_noconfig
    -DCMAKE_CURRENT_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cli_checks.cmake)

# chats list with a bad --limit -> usage error (validated before any network).
add_test(NAME cli_chats_bad_limit
  COMMAND ${CMAKE_COMMAND}
    -DTGCURL=$<TARGET_FILE:tgcurl>
    -DMODE=chats_bad_limit
    -DCMAKE_CURRENT_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cli_checks.cmake)

# contacts with an unknown subcommand -> usage error.
add_test(NAME cli_contacts_bad_sub
  COMMAND ${CMAKE_COMMAND}
    -DTGCURL=$<TARGET_FILE:tgcurl>
    -DMODE=contacts_bad_sub
    -DCMAKE_CURRENT_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cli_checks.cmake)
