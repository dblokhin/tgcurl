cmake_minimum_required(VERSION 3.10)

# Version is injected by the Makefile (-DTGCURL_VERSION=...); default otherwise.
if(NOT DEFINED TGCURL_VERSION)
  set(TGCURL_VERSION "0.0.0-dev")
endif()

project(tgcurl VERSION 0.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(TGCURL_STATIC "Statically link TDLib and the C++ runtime for a portable binary" OFF)

# TDLib (Telegram's official library), found via its installed CMake package.
# 1.8.63 (copr stevenlin/tdlib-master): 1.8.0's MTProto layer is too old for
# Telegram to allow login (406 UPDATE_APP_TO_LOGIN). See DESIGN.md → Build.
find_package(Td 1.8.63 REQUIRED)

# ---- Pure-logic core (NO TDLib) --------------------------------------------
# Holds dependency-free logic (json, error, version, later config/resolve
# helpers) so unit tests can link it without dragging in TDLib. Do NOT link
# Td here — anything touching TDLib belongs in tgcurl_td below.
add_library(tgcurl_core
  src/tgcurl_version.cpp
  src/config.cpp
)
target_include_directories(tgcurl_core PUBLIC src)
target_compile_definitions(tgcurl_core PUBLIC TGCURL_VERSION="${TGCURL_VERSION}")

# ---- TDLib-backed layer ----------------------------------------------------
# The tdclient wrapper (issue #3), auth, and command bodies land here. This is
# the only target that *links* the TDLib archive, and it links it PRIVATE so
# the archive isn't forced onto other consumers. But tgcurl_td's public headers
# (tdclient.h) include <td/telegram/...>, so consumers must see TDLib's include
# dirs: we re-expose them PUBLIC via an INTERFACE, while the heavy archive link
# stays PRIVATE. On a vendored/non-default-path TDLib this is what keeps
# `#include "tdclient.h"` compiling for main.cpp and the command TUs.
add_library(tgcurl_td
  src/tdclient.cpp
  src/auth.cpp
  src/session.cpp
  src/resolve.cpp
  src/commands/auth_cmds.cpp
  src/commands/contacts.cpp
  src/commands/chats.cpp
  src/commands/chat.cpp
  src/commands/send.cpp
)
target_link_libraries(tgcurl_td PUBLIC tgcurl_core PRIVATE Td::TdStatic)
# Propagate only TDLib's include directories (not the link) to consumers.
get_target_property(_td_inc Td::TdStatic INTERFACE_INCLUDE_DIRECTORIES)
if(_td_inc)
  target_include_directories(tgcurl_td PUBLIC ${_td_inc})
endif()

# ---- Executable ------------------------------------------------------------
add_executable(tgcurl src/main.cpp)
target_link_libraries(tgcurl PRIVATE tgcurl_td)

if(TGCURL_STATIC)
  # Bundle the C++ runtime so the binary runs on hosts without matching
  # libstdc++/libgcc. TDLib is already linked statically via Td::TdStatic.
  # (glibc stays dynamic — fully-static glibc is unsupported.)
  target_link_options(tgcurl PRIVATE -static-libstdc++ -static-libgcc)
endif()

install(TARGETS tgcurl RUNTIME DESTINATION bin)

# ---- Tests -----------------------------------------------------------------
enable_testing()
add_subdirectory(tests)
