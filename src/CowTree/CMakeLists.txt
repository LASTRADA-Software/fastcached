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

    # <tests/WindowsErrorPopups.hpp>: this links CowTree rather than FastCache, and it
    # is FastCache that carries src/ as a PUBLIC include directory.
    target_include_directories(CowTreeTests PRIVATE "${CMAKE_SOURCE_DIR}/src")

    # Catch.cmake (catch_discover_tests) is placed on CMAKE_MODULE_PATH by the
    # top-level CMakeLists, regardless of whether Catch2 came from CPM source
    # or a system find_package.
    include(Catch)
    # `SKIP_RETURN_CODE 4` tells ctest a Catch2 skip is a skip and not a failure
    # (#499) -- and it is KNOWN BROKEN in the other direction: Catch2's exit code IS
    # its failed-assertion count, so a case failing exactly four assertions is scored
    # *skipped* (#1128). It stays because all three alternative channels were measured
    # and are worse; the argument is at the top of the top-level CMakeLists and the
    # fix is #1152. `catch-skip-return-code` enforces the property, and its green does
    # not mean the property is sound.
    catch_discover_tests(CowTreeTests PROPERTIES SKIP_RETURN_CODE 4)

    # -----------------------------------------------------------------------
    # A per-case TIMEOUT for the two cases whose guard, if removed, HANGS.
    #
    # Both pin a cycle guard, and a cycle is the one malformed shape a per-page CRC
    # cannot catch: every page around the loop is individually valid, so only a budget
    # stops the walk. Remove the guard and neither case FAILS -- the free-list one never
    # returns from `Open` while `_freeList` grows without bound, and the `ForEach` one
    # spins until the pending stack exhausts memory. **A hang in CI reads as an
    # infrastructure problem rather than as an assertion firing** (#638), and an OOM
    # reads as a runner that was too small. Both are the test's failure mode being
    # uninformative while the test itself is correct.
    #
    # Per case rather than on the `catch_discover_tests` call, which is the whole
    # difficulty and the reason #635 reported this instead of absorbing it: a blanket
    # property sets ONE number for 69 cases whose costs differ by orders of magnitude --
    # too low is a flake on a loaded runner, too high bounds nothing usefully -- and it
    # touches a registration `catch-skip-return-code` has teeth over.
    #
    # The mechanism is `TEST_INCLUDE_FILES`, which `catch_discover_tests` APPENDS its
    # generated file to. Appending a second file makes this run in the same ctest script
    # context, AFTER the discovered tests exist -- at configure time `set_tests_properties`
    # on a discovered name is a generate-time error, because discovery happens at BUILD
    # time.
    #
    # **The existence guard is HERE and cannot be there, which was measured rather than
    # assumed.** The first attempt put an `if(NOT TEST "<name>") message(FATAL_ERROR ...)`
    # into the generated file, and it refused a tree in which the case is plainly
    # present: `if(TEST ...)` is a BUILD-SYSTEM condition, and ctest's script interpreter
    # populates its own test list rather than CMake's, so the condition is false there
    # however many tests exist. And the failure it was written for is silent --
    # `set_tests_properties` naming a test ctest does not know is IGNORED, exit 0, no
    # output (measured on a two-line `CTestTestfile.cmake`). So a rename would leave the
    # bound unapplied with nothing to read, and the tell would be the next hang: exactly
    # the state this exists to remove, restored in silence.
    #
    # Asserting the NAME against the SOURCES at configure time answers it in the one
    # place that can. It is a weaker question than "ctest knows this test" and it is the
    # question that actually protects the bound, because a case only leaves the ctest
    # list by leaving the source.
    #
    # 120s is the number the hygiene scans carry. It is not a performance assertion:
    # both cases run in well under a second, and the bound exists to turn an unbounded
    # walk into a named failure rather than to police their cost.
    set(FASTCACHED_COWTREE_HANG_GUARDED_CASES
        "A free-list chain that loops refuses the store at Open"
        "ForEach refuses a tree whose child pointer loops back on itself"
    )

    set(_cowtreeHangBounds "${CMAKE_CURRENT_BINARY_DIR}/CowTreeTests_hang_bounds.cmake")
    file(WRITE "${_cowtreeHangBounds}"
"# Generated by src/CowTree/CMakeLists.txt (#638). Do not edit.\n"
"#\n"
"# Included through TEST_INCLUDE_FILES, after catch_discover_tests' own generated file.\n"
"# There is deliberately no existence guard in here: `if(TEST ...)` is false in ctest's\n"
"# script interpreter whatever exists, and `set_tests_properties` on a name ctest does\n"
"# not know is silently ignored. The guard is the configure-time assertion in\n"
"# src/CowTree/CMakeLists.txt, which is the one place that can answer.\n")

    foreach(hangGuardedCase IN LISTS FASTCACHED_COWTREE_HANG_GUARDED_CASES)
        set(_caseFound FALSE)
        foreach(testSource IN LISTS COWTREE_TEST_SOURCES)
            file(READ "${testSource}" _testText)
            string(FIND "${_testText}" "TEST_CASE(\"${hangGuardedCase}\"" _caseAt)
            if(NOT _caseAt EQUAL -1)
                set(_caseFound TRUE)
                break()
            endif()
        endforeach()
        if(NOT _caseFound)
            message(FATAL_ERROR
                "No CowTree test source declares TEST_CASE(\"${hangGuardedCase}\"). That case pins a "
                "cycle guard whose absence HANGS rather than fails, so it carries a per-case TIMEOUT "
                "applied BY NAME -- and a rename leaves the bound unapplied silently, because ctest "
                "ignores a property set on a test it does not know. Update "
                "FASTCACHED_COWTREE_HANG_GUARDED_CASES in ${CMAKE_CURRENT_LIST_FILE} (#638).")
        endif()
        file(APPEND "${_cowtreeHangBounds}"
"set_tests_properties(\"${hangGuardedCase}\" PROPERTIES TIMEOUT 120)\n")
    endforeach()

    set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${_cowtreeHangBounds}")
endif()
