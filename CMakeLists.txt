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
find_package(Td 1.8.0 REQUIRED)

# ---- Core library (everything except main, so tests can link it) -----------
add_library(tgcurl_core
  src/tgcurl_version.cpp
)
target_include_directories(tgcurl_core PUBLIC src)
target_compile_definitions(tgcurl_core PUBLIC TGCURL_VERSION="${TGCURL_VERSION}")
target_link_libraries(tgcurl_core PUBLIC Td::TdStatic)

# ---- Executable ------------------------------------------------------------
add_executable(tgcurl src/main.cpp)
target_link_libraries(tgcurl PRIVATE tgcurl_core)

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
