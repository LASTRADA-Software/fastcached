# SPDX-License-Identifier: Apache-2.0
# CowTree — standalone copy-on-write B+tree library.
#
# Sibling of src/FastCache/ with no dependency on FastCache. Other
# projects can pick this up by add_subdirectory()'ing it and linking
# the `CowTree` target.

add_library(CowTree STATIC
    CowTree/Crc32c.cpp
    CowTree/Meta.cpp
    CowTree/InMemoryPageStore.cpp
    CowTree/FilePageStore.cpp
    CowTree/PageLayout.cpp
    CowTree/CowTree.cpp
)

target_include_directories(CowTree PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_compile_features(CowTree PUBLIC cxx_std_23)

if(WIN32)
    target_compile_definitions(CowTree PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WIN32_WINNT=0x0A00
    )
endif()

# Header-only files surfaced for IDE indexing.
target_sources(CowTree
    PRIVATE
        FILE_SET cowtree_public_headers
        TYPE HEADERS
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES
            CowTree/Bytes.hpp
            CowTree/Crc32c.hpp
            CowTree/Errors.hpp
            CowTree/PageId.hpp
            CowTree/Meta.hpp
            CowTree/IPageStore.hpp
            CowTree/InMemoryPageStore.hpp
            CowTree/FilePageStore.hpp
            CowTree/PageLayout.hpp
            CowTree/CowTree.hpp
)

# ---------------------------------------------------------------------------
# Tests
if(FASTCACHED_BUILD_TESTS)
    file(GLOB COWTREE_TEST_SOURCES
        CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp"
    )

    add_executable(CowTreeTests
        test_main.cpp
        ${COWTREE_TEST_SOURCES}
    )

    target_link_libraries(CowTreeTests
        PRIVATE
            CowTree
            Catch2::Catch2
    )

    if(Catch2_SOURCE_DIR)
        list(APPEND CMAKE_MODULE_PATH "${Catch2_SOURCE_DIR}/extras")
    endif()
    include(Catch)
    catch_discover_tests(CowTreeTests)
endif()
