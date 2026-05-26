# SPDX-License-Identifier: Apache-2.0
# Catch2 test binary. Picks up every *_test.cpp adjacent to source files in
# src/FastCache so adding a test file is a one-step operation: drop it next
# to the implementation and re-configure.

file(GLOB_RECURSE FASTCACHE_TEST_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/FastCache/*_test.cpp"
)

add_executable(FastCacheTest
    test_main.cpp
    ${FASTCACHE_TEST_SOURCES}
)

target_link_libraries(FastCacheTest
    PRIVATE
        FastCache
        Catch2::Catch2
)

# When Catch2 is fetched via CPM, its extras/ directory (containing Catch.cmake
# and catch_discover_tests) is not on CMAKE_MODULE_PATH by default. Add it.
if(Catch2_SOURCE_DIR)
    list(APPEND CMAKE_MODULE_PATH "${Catch2_SOURCE_DIR}/extras")
endif()
include(Catch)
catch_discover_tests(FastCacheTest)
