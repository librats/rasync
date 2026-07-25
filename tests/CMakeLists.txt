# GoogleTest — fetched to match the version librats itself pins. Prefer a locally
# cached copy under .cache/ (populated on first fetch) so rebuilds work offline.
include(FetchContent)
set(_gtest_local "${CMAKE_SOURCE_DIR}/.cache/googletest-1.17.0.zip")
if(EXISTS "${_gtest_local}")
    set(_gtest_url "${_gtest_local}")
else()
    set(_gtest_url "https://github.com/google/googletest/archive/refs/tags/v1.17.0.zip")
endif()
FetchContent_Declare(
    googletest
    URL "${_gtest_url}"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

# ── core (pure logic) tests ──────────────────────────────────────────────────
add_executable(rasync_core_tests
    test_serialize.cpp
    test_hash.cpp
    test_manifest.cpp
    test_ignore.cpp
    test_path.cpp
    test_delta.cpp
    test_diff.cpp
    test_scanner.cpp
)
target_include_directories(rasync_core_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(rasync_core_tests PRIVATE rasync_core GTest::gtest_main)
gtest_discover_tests(rasync_core_tests DISCOVERY_TIMEOUT 60)

# ── protocol + end-to-end networking tests (need the net layer) ──────────────
if(TARGET rasync_net AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_sync_integration.cpp)
    add_executable(rasync_net_tests
        test_auth.cpp
        test_protocol.cpp
        test_sync_integration.cpp
    )
    target_include_directories(rasync_net_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(rasync_net_tests PRIVATE rasync_net GTest::gtest_main)
    gtest_discover_tests(rasync_net_tests DISCOVERY_TIMEOUT 60)
endif()
