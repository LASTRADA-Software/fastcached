# SPDX-License-Identifier: Apache-2.0
# Coverage.cmake - Clang source-based code coverage.
#
# Options:
#   ENABLE_COVERAGE - Instrument every first-party target and offer the report
#                     targets below. OFF by default; the `clang-coverage` preset
#                     turns it on.
#
# Targets:
#   coverage        - Run the whole CTest suite under instrumentation and render
#                     the report (HTML, lcov, and a machine-readable summary).
#   coverage-clean  - Drop the report and every raw profile.
#
# Usage:
#   cmake --preset clang-coverage
#   cmake --build --preset clang-coverage
#   cmake --build --preset clang-coverage --target coverage
#
# Source-based coverage rather than gcov, and the reason is the shape of this
# suite rather than a preference. `catch_discover_tests(FastCacheTest)` gives
# every one of ~2000 TEST_CASEs its own process, the script-driven tests in
# src/tests spawn daemons and launchers besides, and CI runs the lot at
# CTEST_PARALLEL_LEVEL=4. gcov merges its counters into a shared .gcda per object
# file as each process exits, so concurrent writers race -- which is what the
# `--ignore-errors mismatch,inconsistent` in every lcov invocation on the
# internet is papering over, and a suppressed error there means under-counted
# coverage reported as a clean run. LLVM's runtime keys the raw profile on the
# binary's own module signature and merges into it under a lock, so the same
# suite needs no suppression and no serialization. It also means no lcov and no
# genhtml: `llvm-cov show` renders the HTML itself.

include(ProjectTargets)

option(ENABLE_COVERAGE "Enable code coverage instrumentation" OFF)

if(NOT ENABLE_COVERAGE)
    return()
endif()

# Everything below is a hard error rather than a warning that disables itself.
# A coverage build that quietly instruments nothing still compiles, still runs
# the suite, and still produces a report -- of zero files -- so every signal an
# author would check says the run was fine. That failure mode is what
# .agent/rules/build-and-toolchain.md means by "a tool that silently does
# nothing is worse than one that is visibly off"; the sanitizer preset had
# already been found in exactly that state once.

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR
        "[Coverage] ENABLE_COVERAGE requires Clang; this is a ${CMAKE_CXX_COMPILER_ID} build. "
        "Configure with the clang-coverage preset, or turn ENABLE_COVERAGE off.")
endif()

# "AppleClang" matches "Clang" above, so this has to be its own check. Apple
# numbers its toolchain independently of upstream LLVM -- an Xcode clang
# reporting 17 is not LLVM 17 -- so no llvm-profdata can ever satisfy the
# version match below, and the operator would be sent to install a package that
# cannot help. Homebrew's LLVM reports plain "Clang" with upstream numbering and
# works; naming it is the actionable half of this message.
if(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    message(FATAL_ERROR
        "[Coverage] ENABLE_COVERAGE does not support AppleClang: its version numbering is "
        "Apple's own, so it can never match an upstream llvm-profdata, whose raw profile "
        "format is versioned. Use Homebrew LLVM (brew install llvm, then configure with "
        "-DCMAKE_CXX_COMPILER=$(brew --prefix llvm)/bin/clang++), or measure coverage on "
        "Linux as the `coverage` job in .github/workflows/build.yml does.")
endif()

# clang-cl reports CMAKE_CXX_COMPILER_ID as "Clang", so the check above passes on
# Windows and nothing else here would have stopped it: the report pipeline is a
# bash script, and the tests it drives are the POSIX legs. The clang-coverage
# preset already refuses to configure on Windows; this is for anyone setting
# -DENABLE_COVERAGE=ON by hand.
if(WIN32)
    message(FATAL_ERROR
        "[Coverage] ENABLE_COVERAGE is not supported on Windows. Coverage is measured on "
        "Linux, by the `coverage` job in .github/workflows/build.yml.")
endif()

if(ENABLE_SANITIZER_ADDRESS OR ENABLE_SANITIZER_UNDEFINED OR ENABLE_SANITIZER_THREAD)
    message(FATAL_ERROR
        "[Coverage] ENABLE_COVERAGE cannot be combined with a sanitizer: the instrumentation "
        "each inserts distorts the other's counts. Use clang-coverage or clang-asan-ubsan, "
        "not both.")
endif()

if(NOT FASTCACHED_BUILD_TESTS)
    message(FATAL_ERROR
        "[Coverage] ENABLE_COVERAGE needs FASTCACHED_BUILD_TESTS=ON -- the coverage target "
        "measures what the test suite reaches, and there is no suite to run.")
endif()

# A compiler cache and coverage instrumentation are quietly incompatible, and
# this project ships the launcher that makes it worst. Coverage mapping data is
# embedded in the object file and names its sources by ABSOLUTE path, while
# fastcache-cc's whole purpose is that an object built under one checkout root
# serves a compile under another. A cache hit therefore replays a perfectly
# correct object carrying the PRODUCER's paths, and llvm-cov then reports files
# that do not exist on this machine -- or, where the roots happen to collide,
# attributes coverage to the wrong tree. Nothing fails; the report is simply
# about somebody else's checkout.
#
# The clang-coverage preset sets USE_COMPILER_CACHE=OFF so this never fires on
# the ordinary path. It stays here because the launcher can also be imposed
# directly with -DCMAKE_CXX_COMPILER_LAUNCHER=, which USE_COMPILER_CACHE does
# not gate (cmake/portable/CompileCache.cmake honours a launcher the caller set).
if(CMAKE_C_COMPILER_LAUNCHER OR CMAKE_CXX_COMPILER_LAUNCHER)
    message(FATAL_ERROR
        "[Coverage] ENABLE_COVERAGE cannot be combined with a compiler-cache launcher "
        "(C='${CMAKE_C_COMPILER_LAUNCHER}', CXX='${CMAKE_CXX_COMPILER_LAUNCHER}'): a cache hit "
        "replays an object whose embedded coverage mapping names the checkout it was built "
        "in, so the report would describe another tree. Configure with -DUSE_COMPILER_CACHE=OFF.")
endif()

# The raw profile format is versioned, and llvm-profdata refuses a file a
# different major version wrote. Prefer the suffixed binary matching the
# compiler, fall back to the unsuffixed one, then check what we actually found:
# a PATH whose plain `llvm-profdata` belongs to some older toolchain is the
# ordinary case on a developer machine with two LLVMs installed, and it fails at
# merge time with a message about the file rather than about the tool.
string(REGEX MATCH "^[0-9]+" COVERAGE_CLANG_MAJOR "${CMAKE_CXX_COMPILER_VERSION}")

foreach(tool IN ITEMS profdata cov)
    string(TOUPPER "${tool}" upper)

    find_program(LLVM_${upper}_PATH
        NAMES "llvm-${tool}-${COVERAGE_CLANG_MAJOR}" "llvm-${tool}"
        DOC "llvm-${tool}, version-matched to the compiler, for ENABLE_COVERAGE")

    if(NOT LLVM_${upper}_PATH)
        message(FATAL_ERROR
            "[Coverage] llvm-${tool} not found. It ships in the llvm-${COVERAGE_CLANG_MAJOR} "
            "package alongside clang-${COVERAGE_CLANG_MAJOR} (apt.llvm.org), or in the "
            "llvm formula on Homebrew.")
    endif()

    execute_process(
        COMMAND "${LLVM_${upper}_PATH}" --version
        OUTPUT_VARIABLE tool_version
        ERROR_VARIABLE tool_version
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    # Quoted, and the match tested for having happened at all: an unquoted
    # STREQUAL compares variable *names* when either side is not a defined
    # variable, and CMAKE_MATCH_1 keeps the previous iteration's capture when a
    # regex does not match -- either one turns this check into one that passes
    # whatever it is handed.
    string(REGEX MATCH "version ([0-9]+)" matched "${tool_version}")

    if(NOT matched)
        message(FATAL_ERROR
            "[Coverage] ${LLVM_${upper}_PATH} --version printed no recognizable version:\n"
            "${tool_version}")
    endif()

    if(NOT "${CMAKE_MATCH_1}" STREQUAL "${COVERAGE_CLANG_MAJOR}")
        # Drop the cache entry before failing, the same way CompileCache.cmake
        # unsets FASTCACHE_CC. find_program() caches what it found, and the cache
        # outlives a FATAL_ERROR -- so without this, someone who reads the
        # message, installs llvm-${COVERAGE_CLANG_MAJOR} and re-runs cmake gets
        # the identical failure from the stale entry, with nothing to suggest the
        # fix worked and only the cache is stale.
        set(found "${LLVM_${upper}_PATH}")
        unset(LLVM_${upper}_PATH CACHE)

        message(FATAL_ERROR
            "[Coverage] ${found} is LLVM ${CMAKE_MATCH_1}, but this is a Clang "
            "${COVERAGE_CLANG_MAJOR} build. The raw profile format is versioned, so the two "
            "must match; install llvm-${COVERAGE_CLANG_MAJOR} or point "
            "-DLLVM_${upper}_PATH at the matching binary.")
    endif()
endforeach()

# Gated here for the same reason as the two above rather than left to fail in
# the script: scripts/coverage.sh reads the percentage out of llvm-cov's JSON
# summary with it, at the very end, so a missing interpreter would otherwise
# surface only after the entire suite, the merge and both exports had run.
find_program(PYTHON3_PATH
    NAMES python3
    DOC "python3, used by scripts/coverage.sh to read llvm-cov's JSON summary")

if(NOT PYTHON3_PATH)
    message(FATAL_ERROR
        "[Coverage] python3 not found. scripts/coverage.sh needs it to extract the coverage "
        "percentage from llvm-cov's JSON summary.")
endif()

message(STATUS "[Coverage] Clang ${COVERAGE_CLANG_MAJOR} source-based instrumentation enabled")

# Directory-scoped, exactly as cmake/Sanitizers.cmake does it, and included from
# the same place in CMakeLists.txt for the same reason: `add_compile_options`
# reaches targets defined after it, and every CPM dependency has already been
# added by that point. yaml-cpp, Catch2 and Tracy are therefore never
# instrumented at all, which is a stronger exclusion than filtering them back
# out of the report afterwards.
add_compile_options(-fprofile-instr-generate -fcoverage-mapping)
add_link_options(-fprofile-instr-generate)

# Call once, after every add_subdirectory() -- fastcached_collect_executables()
# reads the build system as it stands, so a target added later is a target left
# out of the report. The walk itself lives in cmake/ProjectTargets.cmake, shared
# with cmake/Utf8CodePage.cmake, which needs the same list for the same reason:
# both are wrong in silence when it is incomplete.
function(fastcached_add_coverage_targets)
    fastcached_collect_executables("${CMAKE_SOURCE_DIR}" executables)

    if(NOT executables)
        message(FATAL_ERROR
            "[Coverage] no executables found under src/. fastcached_add_coverage_targets() "
            "must be called after the add_subdirectory() calls that define them.")
    endif()

    list(JOIN executables ", " measured)
    message(STATUS "[Coverage] Measuring: ${measured}")

    set(objects "")
    foreach(executable IN LISTS executables)
        list(APPEND objects "$<TARGET_FILE:${executable}>")
    endforeach()

    # Through `bash` rather than by execute bit, which is how every other
    # script-driven target and test in this repository spells it (see the
    # add_test() calls in src/tests/CMakeLists.txt): a mode bit is one more thing
    # a checkout can lose.
    add_custom_target(coverage
        COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/coverage.sh"
            --build-dir "${CMAKE_BINARY_DIR}"
            --source-dir "${CMAKE_SOURCE_DIR}"
            --llvm-profdata "${LLVM_PROFDATA_PATH}"
            --llvm-cov "${LLVM_COV_PATH}"
            --python3 "${PYTHON3_PATH}"
            -- ${objects}
        COMMENT "Running the test suite under instrumentation and rendering the coverage report"
        VERBATIM
        USES_TERMINAL
    )

    # add_dependencies() and not add_custom_target(DEPENDS ...), which takes
    # files rather than targets: passing target names there is accepted and
    # quietly builds nothing, so `--target coverage` on a fresh tree would run
    # ctest against binaries that do not exist yet.
    add_dependencies(coverage ${executables})

    # Two commands, because raw profiles land in two places. The pool this run
    # asked for is under coverage/raw, but an instrumented binary run with no
    # LLVM_PROFILE_FILE set writes default.profraw next to wherever it was
    # started -- which catch_discover_tests does on every link, when it runs the
    # test binary to enumerate cases. Removing only the first would leave those
    # behind for the next run to merge in.
    add_custom_target(coverage-clean
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${CMAKE_BINARY_DIR}/coverage"
        COMMAND find "${CMAKE_BINARY_DIR}" -name "*.profraw" -delete
        COMMENT "Dropping the coverage report and every raw profile"
        VERBATIM
    )
endfunction()
