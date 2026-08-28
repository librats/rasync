cmake_minimum_required(VERSION 3.15)

# ── version, derived from git ────────────────────────────────────────────────
# MAJOR.MINOR.PATCH come from the newest reachable tag (`v1.2.3` or `1.2.3`);
# the fourth component is the commit count, so builds between two tags still
# order. Nothing is committed: version.h is generated from version.h.in into the
# build tree. A checkout without git or without tags still configures — it just
# lands on the fallback below, which is the only version number kept by hand.
set(RASYNC_FALLBACK_VERSION 0.1.0)
set(RASYNC_GIT_DESCRIBE  "unknown")
set(RASYNC_VERSION_BUILD 0)

find_package(Git QUIET)
if(GIT_FOUND AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/.git)
    execute_process(COMMAND ${GIT_EXECUTABLE} describe --tags --always --dirty
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE _git_describe OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(COMMAND ${GIT_EXECUTABLE} describe --tags --abbrev=0
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE _git_tag OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-list --count HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE _git_count OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)

    if(_git_describe)
        set(RASYNC_GIT_DESCRIBE ${_git_describe})
    endif()
    if(_git_count)
        set(RASYNC_VERSION_BUILD ${_git_count})
    endif()

    # The version is baked in at configure time, so a commit has to re-run
    # configure or the binary keeps reporting the tree it was first built from.
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/.git/HEAD)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            ${CMAKE_CURRENT_SOURCE_DIR}/.git/HEAD)
    endif()
endif()

if(_git_tag MATCHES "^v?([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    set(RASYNC_VERSION ${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3})
else()
    set(RASYNC_VERSION ${RASYNC_FALLBACK_VERSION})
endif()

project(rasync
    VERSION ${RASYNC_VERSION}
    DESCRIPTION "Real-time two-way directory synchronization over P2P"
    LANGUAGES C CXX)

set(RASYNC_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(RASYNC_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(RASYNC_VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(RASYNC_VERSION_STRING "${PROJECT_VERSION}.${RASYNC_VERSION_BUILD}")

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rasync/version.h.in
    ${PROJECT_BINARY_DIR}/generated/version.h
    @ONLY)

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
    src/rasync/core/file_stat.cpp
    src/rasync/core/manifest.cpp
    src/rasync/core/ignore.cpp
    src/rasync/core/scanner.cpp
    src/rasync/core/delta.cpp
    src/rasync/core/diff.cpp
    src/rasync/core/state_dir.cpp
)
# The generated dir carries version.h and nothing else; it is PUBLIC so every
# layer above — and the tests — reach it by the same `#include "version.h"`.
target_include_directories(rasync_core PUBLIC src/rasync ${PROJECT_BINARY_DIR}/generated)
# librats gives us SHA-256 + portable filesystem helpers; nothing more.
target_link_libraries(rasync_core PUBLIC rats)

# ── rasync_net: the sync protocol + session, built on librats' Node ──────────
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/rasync/net/sync_session.cpp)
add_library(rasync_net STATIC
    src/rasync/net/auth.cpp
    src/rasync/net/send_gate.cpp
    src/rasync/net/sync_session.cpp
    src/rasync/net/sync_service.cpp
    src/rasync/net/sync_router.cpp
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

message(STATUS "rasync ${RASYNC_VERSION_STRING} (git ${RASYNC_GIT_DESCRIBE}) — tests: ${RASYNC_BUILD_TESTS}")
