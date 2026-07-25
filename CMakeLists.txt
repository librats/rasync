cmake_minimum_required(VERSION 3.15)

project(rasync
    VERSION 0.1.0
    DESCRIPTION "Real-time two-way directory synchronization over P2P"
    LANGUAGES C CXX)

# ── standards / defaults ─────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)

option(RASYNC_BUILD_TESTS "Build the rasync test suite" ON)

# ── librats (P2P transport, discovery, crypto, fs) ───────────────────────────
# Pulled in as a subproject; we don't want its own tests/tools built for us.
set(RATS_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(RATS_BUILD_CLIENT   OFF CACHE BOOL "" FORCE)
set(RATS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(src/librats)

# ── rasync_core: pure, network-free sync logic (the unit-tested heart) ───────
add_library(rasync_core STATIC
    src/rasync/core/hash.cpp
    src/rasync/core/manifest.cpp
    src/rasync/core/ignore.cpp
    src/rasync/core/scanner.cpp
    src/rasync/core/delta.cpp
    src/rasync/core/diff.cpp
)
target_include_directories(rasync_core PUBLIC src/rasync)
# librats gives us SHA-256 + portable filesystem helpers; nothing more.
target_link_libraries(rasync_core PUBLIC rats)

# ── rasync_net: the sync protocol + session, built on librats' Node ──────────
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/rasync/net/sync_session.cpp)
add_library(rasync_net STATIC
    src/rasync/net/auth.cpp
    src/rasync/net/protocol.cpp
    src/rasync/net/sync_session.cpp
    src/rasync/net/sync_service.cpp
)
target_link_libraries(rasync_net PUBLIC rasync_core rats)
endif()

# ── rasync_app: CLI, terminal UI, daemon loop ────────────────────────────────
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/rasync/app/daemon.cpp)
add_library(rasync_app STATIC
    src/rasync/app/terminal.cpp
    src/rasync/app/cli.cpp
    src/rasync/app/daemon.cpp
)
target_link_libraries(rasync_app PUBLIC rasync_net)

# ── the executable ───────────────────────────────────────────────────────────
add_executable(rasync src/rasync/main.cpp)
target_link_libraries(rasync PRIVATE rasync_app)
endif()

if(RASYNC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

message(STATUS "rasync ${PROJECT_VERSION} — tests: ${RASYNC_BUILD_TESTS}")
