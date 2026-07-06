# Unit tests (no external framework; see test_util.h).

add_executable(test_json_out test_json_out.cpp)
target_link_libraries(test_json_out PRIVATE tgcurl_core)
add_test(NAME json_out COMMAND test_json_out)

# json_in is pure logic in the core library.
add_executable(test_json_in test_json_in.cpp)
target_link_libraries(test_json_in PRIVATE tgcurl_core)
add_test(NAME json_in COMMAND test_json_in)

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

# resolve's pure classify() (searchPublicChat is network-dependent).
add_executable(test_resolve test_resolve.cpp)
target_link_libraries(test_resolve PRIVATE tgcurl_td)
add_test(NAME resolve COMMAND test_resolve)

# send_confirm codifies the async send-acknowledgement rule (works with
# td_api objects but no network).
add_executable(test_send_confirm test_send_confirm.cpp)
target_link_libraries(test_send_confirm PRIVATE tgcurl_td)
add_test(NAME send_confirm COMMAND test_send_confirm)

# The command registry (single source for CLI dispatch and MCP tools).
add_executable(test_registry test_registry.cpp)
target_link_libraries(test_registry PRIVATE tgcurl_td)
add_test(NAME registry COMMAND test_registry)

# The shared message renderer (td_api objects, no network).
add_executable(test_message_render test_message_render.cpp)
target_link_libraries(test_message_render PRIVATE tgcurl_td)
add_test(NAME message_render COMMAND test_message_render)

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

# login with a seeded config reaches the phone prompt with TDLib logs silenced
# (they must not bury the prompt). Guards the log-verbosity fix.
add_test(NAME cli_login_quiet
  COMMAND ${CMAKE_COMMAND}
    -DTGCURL=$<TARGET_FILE:tgcurl>
    -DMODE=login_quiet
    -DCMAKE_CURRENT_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cli_checks.cmake)

# logout with empty config -> {"ok":true}, exit 0.
add_test(NAME cli_logout_noconfig
  COMMAND ${CMAKE_COMMAND}
    -DTGCURL=$<TARGET_FILE:tgcurl>
    -DMODE=logout_noconfig
    -DCMAKE_CURRENT_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cli_checks.cmake)

# status with empty config -> {"authorized":false}, exit 0, no prompt.
add_test(NAME cli_status_noconfig
  COMMAND ${CMAKE_COMMAND}
    -DTGCURL=$<TARGET_FILE:tgcurl>
    -DMODE=status_noconfig
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

# send with a free-text name -> unresolvable, before any network.
add_test(NAME cli_send_unresolvable
  COMMAND ${CMAKE_COMMAND}
    -DTGCURL=$<TARGET_FILE:tgcurl>
    -DMODE=send_unresolvable
    -DCMAKE_CURRENT_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cli_checks.cmake)

# chat with a free-text name -> unresolvable.
add_test(NAME cli_chat_unresolvable
  COMMAND ${CMAKE_COMMAND}
    -DTGCURL=$<TARGET_FILE:tgcurl>
    -DMODE=chat_unresolvable
    -DCMAKE_CURRENT_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cli_checks.cmake)

# contacts new with missing args -> usage error, before any network.
add_test(NAME cli_contacts_new_bad
  COMMAND ${CMAKE_COMMAND}
    -DTGCURL=$<TARGET_FILE:tgcurl>
    -DMODE=contacts_new_bad
    -DCMAKE_CURRENT_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cli_checks.cmake)

# contacts block with a free-text name -> unresolvable.
add_test(NAME cli_contacts_block_unresolvable
  COMMAND ${CMAKE_COMMAND}
    -DTGCURL=$<TARGET_FILE:tgcurl>
    -DMODE=contacts_block_unresolvable
    -DCMAKE_CURRENT_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cli_checks.cmake)

# search with a free-text --chat -> unresolvable, before any network.
add_test(NAME cli_search_unresolvable
  COMMAND ${CMAKE_COMMAND}
    -DTGCURL=$<TARGET_FILE:tgcurl>
    -DMODE=search_unresolvable
    -DCMAKE_CURRENT_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cli_checks.cmake)

# sendfile with a nonexistent path -> file_not_found, before any network.
add_test(NAME cli_sendfile_missing
  COMMAND ${CMAKE_COMMAND}
    -DTGCURL=$<TARGET_FILE:tgcurl>
    -DMODE=sendfile_missing
    -DCMAKE_CURRENT_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cli_checks.cmake)

# -mcp: JSON-RPC handshake, tools/list from the registry, a pre-network
# failing tools/call as an isError result, clean exit on EOF.
add_test(NAME cli_mcp
  COMMAND ${CMAKE_COMMAND}
    -DTGCURL=$<TARGET_FILE:tgcurl>
    -DMODE=mcp
    -DCMAKE_CURRENT_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cli_checks.cmake)
