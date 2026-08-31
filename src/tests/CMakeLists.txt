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

# Absolute path to checked-in test fixtures (e.g. the throwaway TLS cert/key),
# so tests can open them regardless of the working directory.
target_compile_definitions(FastCacheTest
    PRIVATE
        FASTCACHED_TESTDATA_DIR="${CMAKE_SOURCE_DIR}/testdata"
)

# Catch.cmake (catch_discover_tests) is placed on CMAKE_MODULE_PATH by the
# top-level CMakeLists, regardless of whether Catch2 came from CPM source or a
# system find_package.
include(Catch)
catch_discover_tests(FastCacheTest)

# ---------------------------------------------------------------------------
# The ThreadSanitizer canary: a program that MUST fail, so that a green sanitizer
# job means something. See src/tests/TsanCanary.cpp for why it contains a
# deliberate data race, and scripts/tsan-gate.sh for what refuses to run without
# it.
#
# Built here, by the same CMake that instruments everything else, rather than
# compiled by the gate script with flags of its own. That is the entire point: it
# inherits `-fsanitize=thread` from the same `add_compile_options` the real
# targets get, so it detects a Sanitizers.cmake that has stopped applying them --
# the defect that module's own comment records. A canary built with hand-written
# flags would keep passing through exactly that failure.
#
# Deliberately NOT linked against FastCache: it answers "is the toolchain
# instrumenting and reporting", which has to stay answerable even when the
# library does not build.
#
# Guarded, so no other configuration builds a knowingly-racy binary. The guard
# does not weaken the check -- when the option is ON but the flags never reach the
# compile line, this target still exists and still comes out uninstrumented,
# which is precisely the case the gate is looking for.
if(ENABLE_SANITIZER_THREAD)
    add_executable(tsan-canary TsanCanary.cpp)
    # No Catch2 and no catch_discover_tests. CTest could express "this must fail"
    # -- `WILL_FAIL`, or `PASS_REGULAR_EXPRESSION "data race"` -- so that is not
    # the reason; the reason is that the canary's verdict has to be read BEFORE
    # the suite is believed, and a sibling ctest case that merely also passes
    # proves nothing about the run that reported clean. scripts/tsan-gate.sh
    # sequences the two, and is the only thing that should run this.
    find_package(Threads REQUIRED)
    target_link_libraries(tsan-canary PRIVATE Threads::Threads)
endif()

# ---------------------------------------------------------------------------
# The MSVC checked-iterator canary: the Windows Debug counterpart of the one
# above, and deliberately the same shape rather than a second idiom.
#
# A Windows Debug leg is worth running for `_ITERATOR_DEBUG_LEVEL=2` -- it traps
# invalidated iterators, out-of-range indexing and mismatched container iterators
# at the point they happen, which is exactly what a Release build tolerates in
# silence. But nothing here STATES that level: it follows from `_DEBUG`, which
# follows from the runtime library, which follows from `CMAKE_BUILD_TYPE` and
# `CMAKE_MSVC_RUNTIME_LIBRARY`. Any of those moving takes the checks away with no
# warning, and the leg would still build, still run the whole suite, and still
# report green while checking nothing it was added to check.
#
# So it is asserted at run time by a program that must die (#315).
#
# Unlike `tsan-canary` this one IS registered as a ctest, and the difference is
# worth stating: the TSan canary has to be read BEFORE the suite is believed, so a
# shell gate sequences the two. Here the canary answers a question about the
# BUILD, not about a suite run, so it is just another check -- and registering it
# means it runs for anyone building `cl-debug` locally rather than only in CI,
# which is where this defect would otherwise be found.
#
# `scripts/iterator-debug-gate.ps1` and not `WILL_FAIL`, because a bare inversion
# cannot say WHY a green result is meaningless, and because a non-zero exit is not
# proof on its own -- the gate requires the runtime's own "subscript out of range"
# diagnostic, the same way the TSan gate requires "data race".
if(MSVC AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_executable(iterator-debug-canary IteratorDebugCanary.cpp)
    add_test(
        NAME "iterator-debug-canary"
        COMMAND pwsh -NoProfile -File "${CMAKE_SOURCE_DIR}/scripts/iterator-debug-gate.ps1"
                -Canary "$<TARGET_FILE:iterator-debug-canary>"
    )
    set_tests_properties("iterator-debug-canary" PROPERTIES
        LABELS "hygiene"
        TIMEOUT 120
    )
endif()

# ---------------------------------------------------------------------------
# Repository hygiene. Fails when a path that must never be committed is tracked
# by git. Today that is version.txt, whose committed presence outranked every git
# tag and pinned every build, every wire banner and every package to whatever it
# said. scripts/check-repository-hygiene.cmake carries the table of forbidden
# paths and the reason each one is forbidden, and prints the reason on failure.
#
# Deliberately NOT labelled "smoke": it starts no daemon, opens no socket and
# needs no compiler, so it belongs in the default `ctest` set and must survive
# `ctest -LE smoke`. It finishes in milliseconds.
#
# GIT_EXECUTABLE is located by cmake/Version.cmake and passed in rather than
# looked up again, so the check and the version resolution cannot disagree about
# which git they mean.
add_test(
    NAME "repository-hygiene"
    COMMAND ${CMAKE_COMMAND}
        "-DFASTCACHED_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
        "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
        -P "${CMAKE_SOURCE_DIR}/scripts/check-repository-hygiene.cmake"
)
# An exported source tarball has no git index to inspect, and the script reports
# that by printing `SKIP: ...` and exiting 0. SKIP_REGULAR_EXPRESSION and not the
# SKIP_RETURN_CODE 77 the drivers below use, because a `cmake -P` script cannot
# choose its own exit code before CMake 3.29 (cmake_language(EXIT)) while this
# project supports 3.28 — a SKIP_RETURN_CODE it could never return would be
# decoration. See the script header. TIMEOUT 60 rather than CTest's 1500s
# default: a wedged git should report in a minute, not stall the suite.
set_tests_properties("repository-hygiene" PROPERTIES
    LABELS "hygiene"
    SKIP_REGULAR_EXPRESSION "SKIP: "
    TIMEOUT 60
)

# ---------------------------------------------------------------------------
# The Net boundary. Fails when anything under Net/ or Async/ includes a FastCache
# header from outside the standalone unit those two form, or when one of the
# three dependency-free leaf headers they are allowed to reach stops being
# dependency-free. scripts/check-net-boundary.cmake carries the table and the
# reason each row is in it, and prints the whole table on failure.
#
# Net/ is meant to be lifted out of this tree and upstreamed, and that is a
# property of the include graph -- which drifts in silence. Nothing fails, nothing
# warns, and the edge is found by whoever attempts the lift: issue #100 counted
# ten that had accumulated exactly that way, none of which any test could see.
#
# Not labelled "smoke", for the reason repository-hygiene is not: it starts no
# daemon, opens no socket and needs no compiler, so it belongs in the default
# `ctest` set and must survive `ctest -LE smoke`. It finishes in milliseconds.
# Unlike repository-hygiene it has no skip path at all -- it reads the source
# tree, which is present in an exported tarball exactly as it is in a checkout.
add_test(
    NAME "net-boundary"
    COMMAND ${CMAKE_COMMAND}
        "-DFASTCACHED_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
        -P "${CMAKE_SOURCE_DIR}/scripts/check-net-boundary.cmake"
)
set_tests_properties("net-boundary" PROPERTIES
    LABELS "hygiene"
    TIMEOUT 60
)

# ---------------------------------------------------------------------------
# Test-name hygiene. Fails when a Catch2 case is named something the CTest
# registration cannot hand back to the runner.
#
# `catch_discover_tests` registers each case as `add_test(NAME <case> COMMAND
# <exe> <case>)`, so the name arrives at Catch2 as a command-line ARGUMENT and a
# leading `-` is read as an option rather than as a test spec. A recognised one
# makes CTest report a pass for a case that never ran; an unrecognised one makes
# it report a failure the case does not have, which then passes when run by hand.
# Both were live in this tree, and the first had been silently not running since
# the day it was written.
#
# Not labelled "smoke", for the reason the two above are not: it reads source
# files and reports, so it belongs in the default `ctest` set and must survive
# `ctest -LE smoke`. It finishes in milliseconds and, like net-boundary, has no
# skip path -- the source tree is present in an exported tarball exactly as it is
# in a checkout.
add_test(
    NAME "test-name-hygiene"
    COMMAND ${CMAKE_COMMAND}
        "-DFASTCACHED_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
        -P "${CMAKE_SOURCE_DIR}/scripts/check-test-names.cmake"
)
set_tests_properties("test-name-hygiene" PROPERTIES
    LABELS "hygiene"
    TIMEOUT 60
)

# ---------------------------------------------------------------------------
# Target-file guard hygiene. Fails when a `$<TARGET_FILE:>` names an executable
# the app table can gate off, without any enclosing condition asking whether
# that target exists.
#
# Such a reference is not a test that gets skipped -- it is a hard error at
# GENERATE time, so the whole configure fails and the message names CMake rather
# than the option the operator set. #390: `tls-smoke` was guarded on
# `FASTCACHED_ENABLE_TLS` and the sccache smokes on `SCCACHE`, so
# `-DFASTCACHED_BUILD_DAEMON=OFF` could not be configured at all.
#
# It ran green for everyone who does not have sccache installed, because the
# sccache rows are themselves gated on finding the binary. That is why this
# belongs in CI rather than in a reviewer's head: the tree alone does not say
# whether the bug is reachable, the developer's machine does.
#
# In the default set, for the reason test-name-hygiene gives above: it reads
# source files and reports.
add_test(
    NAME "target-file-guards"
    COMMAND ${CMAKE_COMMAND}
        "-DFASTCACHED_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
        -P "${CMAKE_SOURCE_DIR}/scripts/check-target-file-guards.cmake"
)
set_tests_properties("target-file-guards" PROPERTIES
    LABELS "hygiene"
    TIMEOUT 60
)

# ---------------------------------------------------------------------------
# The compile worker's shipped reference configuration against its option table.
# Fails when a setting the file can carry has no commented block for an operator
# to find, or when the file documents a key the table no longer has -- which is
# the worse direction, since uncommenting it makes the worker refuse to start.
#
# Nothing else connects the two: NodeOptions() drives the parse, the YAML is
# prose, and a build in which they describe different products is a build in
# which everything passes. See scripts/check-node-config-reference.cmake.
#
# Not labelled "smoke", for the reason the checks above are not: it reads two
# source files and compares strings, so it belongs in the default `ctest` set and
# must survive `ctest -LE smoke`. Both files ship in an exported tarball, so it
# has no skip path.
add_test(
    NAME "node-config-reference"
    COMMAND ${CMAKE_COMMAND}
        "-DFASTCACHED_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
        -P "${CMAKE_SOURCE_DIR}/scripts/check-node-config-reference.cmake"
)
set_tests_properties("node-config-reference" PROPERTIES
    LABELS "hygiene"
    TIMEOUT 60
)

# ---------------------------------------------------------------------------
# ThreadSanitizer scope hygiene. Fails when a test file in a concurrency-bearing
# directory carries no tag `scripts/tsan-gate.sh` selects on, so the gate's scope
# is enforced rather than merely asserted.
#
# It is registered here, in the DEFAULT set, and deliberately not under
# `if(ENABLE_SANITIZER_THREAD)`. The scope rots in ordinary builds -- somebody adds
# `Async/Foo_test.cpp` tagged `[queue][mpsc]`, which no row of the gate's table
# selects, and nothing says otherwise -- so a check that only runs in the
# sanitizer preset would report the drift to the one configuration nobody runs
# locally. This costs milliseconds and reads source files, so it survives
# `ctest -LE smoke` like the checks above.
#
# The failure it exists for has already happened once, before the gate merged.
# The reasoning, and why the check reads its tags out of the gate rather than
# keeping a second copy of them, is in the header of
# scripts/check-tsan-scope.cmake.
add_test(
    NAME "tsan-scope-hygiene"
    COMMAND ${CMAKE_COMMAND}
        "-DFASTCACHED_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
        -P "${CMAKE_SOURCE_DIR}/scripts/check-tsan-scope.cmake"
)
set_tests_properties("tsan-scope-hygiene" PROPERTIES
    LABELS "hygiene"
    TIMEOUT 60
)

# ---------------------------------------------------------------------------
# CI scope classification. `scripts/ci-scope.sh` decides whether a pull request
# compiles anything at all: the `changes` job in .github/workflows/build.yml reads
# its `code=` output, and every heavy job is gated on it.
#
# That makes a wrong answer in one direction very expensive and completely silent.
# `code=false` for a path that does reach a build is a change merged with every
# required check reporting green and no job having compiled it -- the failure this
# repository keeps a list about, arriving through the workflow rather than through
# the code. So the test asserts BOTH directions and, separately, that every way of
# not knowing (an unresolvable ref, a failed diff, an empty diff, a path matching
# no row) escalates to `code=true`.
#
# Not labelled "smoke": it starts no daemon, opens no socket and needs no
# compiler. It builds a throwaway git repository in a scratch directory, so it
# asserts nothing about the tree it runs in and works from an exported tarball --
# and, unlike `repository-hygiene`, needs no readable git worktree of its own.
#
# `if(NOT WIN32)` for the reason `cluster-e2e` carries the same guard: the script
# is POSIX and a Windows counterpart would be a translation rather than new
# coverage. The rule it protects is platform-independent and the Linux jobs run it.
if(NOT WIN32)
    add_test(
        NAME "ci-scope"
        COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/ci-scope-test.sh"
    )
    set_tests_properties("ci-scope" PROPERTIES
        LABELS "hygiene"
        TIMEOUT 120
    )
endif()

# ---------------------------------------------------------------------------
# Merge-queue reportability. Fails when a REQUIRED status check could not report
# a conclusion inside a `merge_group` event.
#
# The failure it exists for is silent and total: a workflow that does not listen
# for `merge_group` produces no check run for it, so the required context never
# reports and a queued pull request does not fail — it SITS THERE. Every workflow
# stays valid, every job stays correct, and nothing anywhere disagrees. It is the
# same never-arrives shape as a `paths-ignore` filter on a required check, which
# this repository has already paid for once, reached through a different door.
#
# It also cannot be demonstrated before the fact: a `merge_group` event only
# exists once a queue is enabled, and enabling a queue that stalls is the
# outcome. So the property is asserted from the workflow files instead.
#
# `if(NOT WIN32)` for the reason `ci-scope` carries the same guard: the script is
# POSIX and a Windows counterpart would be a translation rather than new coverage.
# What it checks is platform-independent and the Linux and macOS jobs run it.
if(NOT WIN32)
    add_test(
        NAME "merge-queue-contexts"
        COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/check-merge-queue-contexts.sh"
    )
    set_tests_properties("merge-queue-contexts" PROPERTIES
        LABELS "hygiene"
        TIMEOUT 60
    )
endif()

# ---------------------------------------------------------------------------
# The scope gate fails safe. Fails when a job gated on `scripts/ci-scope.sh`
# would be SKIPPED, rather than run, if the classifier itself failed.
#
# `ci-scope.sh` states at length that every way of not knowing escalates to
# `code=true`, because "we could not tell what changed" must never read as
# "nothing did". The workflow reading it inverted that one level up: sixteen jobs
# were gated `== 'true'`, so a `changes` job that FAILED published no output, the
# comparison was false, the jobs were skipped -- and a skipped required context is
# read by the ruleset as PASSING. A green, mergeable pull request that nothing had
# compiled.
#
# Sixteen hand-edits with nothing asserting them is how that comes back. Both
# rules are derived from the workflow rather than tabulated, so the SEVENTEENTH
# job cannot be added wrong; `release` needs `changes` too and is excluded by
# construction, because it never reads the classifier's output.
#
# `if(NOT WIN32)` for the reason `ci-scope` carries the same guard.
if(NOT WIN32)
    add_test(
        NAME "gated-jobs-fail-safe"
        COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/check-gated-jobs.sh"
    )
    set_tests_properties("gated-jobs-fail-safe" PROPERTIES
        LABELS "hygiene"
        TIMEOUT 60
    )
endif()

# ---------------------------------------------------------------------------
# The local gate pins its analyser. Fails when `scripts/local-gate.sh` would run
# clang-tidy at a version nobody chose, or would accept a build directory that
# already caches one.
#
# The gate itself cannot be a test -- it builds two whole configurations -- but the
# part that can be WRONG is a decision over a CMake cache file, and that part is
# pure. `--self-test` drives it against synthetic caches: a directory on the wrong
# analyser, one whose `find_program` never found any (`CLANG_TIDY_EXE-NOTFOUND`),
# one with no entry at all, and one already correct.
#
# The failure it exists for reads exactly like success. `find_program` never
# revisits a filled cache entry and the gate only configured when `CMakeCache.txt`
# was ABSENT, so a build directory kept whichever clang-tidy it first found forever
# and re-running the gate could not fix it -- re-running the gate is what skipped
# the configure. The same shape as the stale `FASTCACHE_CC-NOTFOUND` that kept
# build trees on sccache silently.
#
# Not labelled "smoke": it starts no daemon, opens no socket, and needs neither
# cmake nor clang-tidy. `if(NOT WIN32)` for the reason `ci-scope` carries the same
# guard -- the script is POSIX, and what it checks is platform-independent.
if(NOT WIN32)
    add_test(
        NAME "local-gate-selftest"
        COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/local-gate.sh" --self-test
    )
    set_tests_properties("local-gate-selftest" PROPERTIES
        LABELS "hygiene"
        TIMEOUT 60
    )
endif()

# ---------------------------------------------------------------------------
# The sccache-backend caveat. Fails when a file recommends pointing sccache at
# fastcached -- which is what naming either of the two `SCCACHE_*` backend
# variables is -- without saying, within 40 lines, what that costs under MSVC and
# clang-cl.
# scripts/check-sccache-backend-caveat.cmake carries the tables and prints them
# on failure.
#
# compile-cache-caveat, below, pins the warning the vendored CMake module emits
# when IT falls back to sccache. This is issue #170: the people README.md pitches
# the backend to never configure that module -- they have their own build system
# and set the variable themselves -- so that warning cannot reach them, and the
# configuration they are being sold is by definition ONE cache shared across
# every checkout and machine, which is the maximal form of the same hazard. Four
# surfaces said it with no caveat and README.md said the opposite of it.
#
# A rule about prose, enforced rather than remembered, for the reason net-boundary
# is: the next page pitching this arrives from an author who never saw the caveat
# on the others, and nothing about that fails, warns, or shows up in review.
#
# It searches every source root and JUDGES only markdown. A non-prose file that
# names a variable is neither judged nor waved through: it has to appear in the
# script's exemption table saying what owns it instead. `--help` is the live case
# -- its caveat is prose inside a C++ string literal, which a source scan finds
# just as happily in a comment beside it, so the `--help qualifies its sccache
# examples with the MSVC caveat` case in CliParser_test.cpp owns it and asserts
# against the RENDERED usage string.
#
# Not labelled "smoke", for the reason the three above are not: it reads source
# files and reports, so it belongs in the default `ctest` set and must survive
# `ctest -LE smoke`. It has no skip path -- the source tree is present in an
# exported tarball exactly as it is in a checkout. Slower than its siblings
# (~0.2-0.5 s against their ~0.1-0.2 s) because it reads every candidate file to
# find the few that name a variable, which is what makes it a discovery rather
# than a list of places to look; still far inside the timeout.
add_test(
    NAME "sccache-backend-caveat"
    COMMAND ${CMAKE_COMMAND}
        "-DFASTCACHED_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
        -P "${CMAKE_SOURCE_DIR}/scripts/check-sccache-backend-caveat.cmake"
)
set_tests_properties("sccache-backend-caveat" PROPERTIES
    LABELS "hygiene"
    TIMEOUT 60
)

# ---------------------------------------------------------------------------
# Service accounts. Fails when the account a binary compiles into its ServiceSpec
# is not the one the macOS package creates, or is created by no postinstall, or is
# never removed by the uninstaller.
# scripts/check-service-accounts.cmake carries the table and the reason each row is
# in it, and prints the whole table on failure.
#
# The failure it guards is silent AND macOS-only: launchd accepts a plist naming a
# UserName it cannot resolve, bootstraps the job, and simply never spawns it -- so
# the install-time refusal in ServiceControl.cpp is the only thing standing between
# an operator and a service that reports installed and never runs. Issue #87 was the
# other half of that refusal missing: `fastcache-compile-node` named `fastcache-node`
# and the package created only `_fastcached`.
#
# Registered here rather than left to the macOS packaging job precisely because it
# is macOS-only in effect: a check only one platform can run reports after the
# mistake has been pushed. This one reads the source tree, so every platform runs it.
#
# Not labelled "smoke", for the reason repository-hygiene is not: it starts no
# daemon, opens no socket and needs no compiler, so it belongs in the default
# `ctest` set and must survive `ctest -LE smoke`. It finishes in milliseconds, and
# like net-boundary it has no skip path -- the source tree it reads is present in an
# exported tarball exactly as it is in a checkout.
add_test(
    NAME "service-accounts"
    COMMAND ${CMAKE_COMMAND}
        "-DFASTCACHED_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
        -P "${CMAKE_SOURCE_DIR}/scripts/check-service-accounts.cmake"
)
set_tests_properties("service-accounts" PROPERTIES
    LABELS "hygiene"
    TIMEOUT 60
)

# ---------------------------------------------------------------------------
# A non-ASCII argument, from the command line into main(). Runs each executable
# that has an option table with an unrecognised flag spelled with U+00FC, and
# requires the diagnostic to echo those exact UTF-8 bytes back.
#
# It is the only proof there CAN be of the fix for issue #155, because the defect
# is in what the OS hands a process: Windows transcodes argv through the process's
# active code page, so nothing inside a process can observe what was lost. A Catch2
# case can assert the code page a test binary ended up with (and
# Platform/NarrowText_test.cpp does); only running a real executable says what an
# argument BECAME on the way in. Verified to distinguish the two states -- against a
# binary built without the manifest the same argument arrives as `67 72 FC 6E` and
# this fails.
#
# Registered here rather than beside one binary because it names four, per
# .agent/rules/testing.md. Each row is guarded on its target existing: the test
# client and the launcher are gated by their own options in src/apps.
#
# Not labelled "smoke": each run is one process that rejects an argument and exits
# before binding anything, needing no daemon, no socket and no compiler -- so it
# belongs in the default `ctest` set and must survive `ctest -LE smoke`.
foreach(_utf8ArgvTarget IN ITEMS fastcached fastcache-cc fastcache-compile-node compile-cache-testclient)
    if(TARGET ${_utf8ArgvTarget})
        add_test(
            NAME "utf8-argv-${_utf8ArgvTarget}"
            COMMAND ${CMAKE_COMMAND}
                "-DEXE=$<TARGET_FILE:${_utf8ArgvTarget}>"
                "-DNAME=${_utf8ArgvTarget}"
                -P "${CMAKE_SOURCE_DIR}/scripts/check-utf8-argv.cmake"
        )
        set_tests_properties("utf8-argv-${_utf8ArgvTarget}" PROPERTIES
            LABELS "hygiene"
            TIMEOUT 60
        )
    endif()
endforeach()

# ---------------------------------------------------------------------------
# Compiler-cache auto-install fallback. Configures the tiny fixture project in
# compile-cache-fixture/ once per way the fetch can be declined, and requires
# each one to end in a status message and a clean fall-through.
#
# What it defends is the property that makes cmake/portable/CompileCache.cmake safe to
# vendor: it is copied into repositories that never asked for fastcached, so a
# cache it cannot download has to cost them a slower build and nothing else. A
# configure that aborts because GitHub was unreachable would be a far worse
# failure than the missing cache it is reporting.
#
# Not labelled "smoke" for the same reason as repository-hygiene: it starts no
# daemon and opens no listening socket, so it belongs in the default `ctest`
# set. It does invoke the compiler (a project has to configure), which is why
# it is given more than that test's minute.
#
# The compiler, generator and make program are passed in rather than rediscovered
# so the fixture is configured the same way this build was, and the script's
# sandbox is the only thing that differs.
add_test(
    NAME "compile-cache-autoinstall"
    COMMAND ${CMAKE_COMMAND}
        "-DFASTCACHED_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
        "-DFASTCACHED_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/compile-cache-autoinstall"
        "-DFASTCACHED_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
        "-DFASTCACHED_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
        "-DFASTCACHED_GENERATOR=${CMAKE_GENERATOR}"
        -P "${CMAKE_SOURCE_DIR}/scripts/check-compile-cache-autoinstall.cmake"
)
# Windows and a host without uname both report by printing `SKIP: ` and exiting
# 0 — see the script header for why the sandbox it needs is POSIX-only, and
# repository-hygiene above for why a `cmake -P` script cannot return 77.
set_tests_properties("compile-cache-autoinstall" PROPERTIES
    LABELS "hygiene"
    SKIP_REGULAR_EXPRESSION "SKIP: "
    TIMEOUT 600
)

# ---------------------------------------------------------------------------
# Compiler-cache caveat reporting. Configures the same fixture once per selection
# outcome and requires the module to say so when it picks a launcher that can
# silently produce a wrong build -- and to stay quiet when it picks one that
# cannot.
#
# Issue #153: sccache replays a cache hit's dependency list with the paths of the
# build that STORED it, so a second checkout sharing the cache records
# dependencies pointing into the first and then rebuilds nothing when a header
# changes. Measured here as 1097 recorded dependency edges into the wrong
# worktree and `ninja: no work to do` after a real edit. The module cannot fix
# somebody else's replay; what it owes a developer is a word about it, and that
# word is what this pins.
#
# Both halves matter. A warning that fired for every launcher would be one nobody
# reads, so the silence is asserted too.
#
# Needs no sandbox -- it names the launchers through the find_program cache
# variables rather than hiding them -- so unlike compile-cache-autoinstall it
# runs on Windows as well and has no SKIP path. Labelled "hygiene" for the same
# reason: it starts no daemon and opens no socket, so it belongs in the default
# ctest set.
# Which half of the contract this build asserts: the hazard is MSVC's and
# clang-cl's, so the module carries the caveat on exactly that condition and the
# check has to expect the noise there and the silence everywhere else. Resolved
# here rather than re-derived in the script, so the two cannot drift.
set(_fc_caveat_msvc_like OFF)
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC"
   OR CMAKE_C_COMPILER_ID STREQUAL "MSVC" OR CMAKE_C_SIMULATE_ID STREQUAL "MSVC")
    set(_fc_caveat_msvc_like ON)
endif()

add_test(
    NAME "compile-cache-caveat"
    COMMAND ${CMAKE_COMMAND}
        "-DFASTCACHED_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
        "-DFASTCACHED_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/compile-cache-caveat"
        "-DFASTCACHED_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
        "-DFASTCACHED_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
        "-DFASTCACHED_GENERATOR=${CMAKE_GENERATOR}"
        "-DFASTCACHED_MSVC_LIKE=${_fc_caveat_msvc_like}"
        -P "${CMAKE_SOURCE_DIR}/scripts/check-compile-cache-caveat.cmake"
)
# A shell that is not a Developer shell has a cl.exe that exists and cannot
# compile, and this is the first of these checks that runs on Windows at all --
# so it reports that by printing `SKIP: ` rather than blaming the caveat logic
# for a toolchain nobody set up. repository-hygiene above says why a `cmake -P`
# script cannot return 77 instead.
set_tests_properties("compile-cache-caveat" PROPERTIES
    LABELS "hygiene"
    SKIP_REGULAR_EXPRESSION "SKIP: "
    TIMEOUT 600
)

# The other half: that it can actually install one. Serves a synthetic release
# over file:// through FASTCACHE_AUTO_INSTALL_DOWNLOAD_BASE — the same seam an
# organisation uses to install from an internal mirror — so the happy path is
# covered without the network, and without spending the GitHub API budget that
# a fleet of CI runners behind one address has to share.
add_test(
    NAME "compile-cache-install"
    COMMAND ${CMAKE_COMMAND}
        "-DFASTCACHED_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
        "-DFASTCACHED_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/compile-cache-install"
        "-DFASTCACHED_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
        "-DFASTCACHED_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
        "-DFASTCACHED_GENERATOR=${CMAKE_GENERATOR}"
        -P "${CMAKE_SOURCE_DIR}/scripts/check-compile-cache-install.cmake"
)
set_tests_properties("compile-cache-install" PROPERTIES
    LABELS "hygiene"
    SKIP_REGULAR_EXPRESSION "SKIP: "
    TIMEOUT 600
)

# ---------------------------------------------------------------------------
# FASTCACHE_AUTO_START staging (issue #90): the daemon binary comes from the
# SAME release archive the launcher does, so this proves one download serves
# both members rather than fetching (or failing to fetch) the archive twice.
# Same file:// mirror technique as compile-cache-install, for the same
# reasons — deterministic, offline, no GitHub rate-limit budget spent — and
# deliberately not what proves a spawned daemon actually answers a real
# connection: the mirror's stand-ins only answer --version, so that half is
# compile-cache-daemon-start's job, below.
add_test(
    NAME "compile-cache-daemon-staging"
    COMMAND ${CMAKE_COMMAND}
        "-DFASTCACHED_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
        "-DFASTCACHED_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/compile-cache-daemon-staging"
        "-DFASTCACHED_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
        "-DFASTCACHED_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
        "-DFASTCACHED_GENERATOR=${CMAKE_GENERATOR}"
        -P "${CMAKE_SOURCE_DIR}/scripts/check-compile-cache-daemon-staging.cmake"
)
set_tests_properties("compile-cache-daemon-staging" PROPERTIES
    LABELS "hygiene"
    SKIP_REGULAR_EXPRESSION "SKIP: "
    TIMEOUT 600
)

# ---------------------------------------------------------------------------
# End-to-end distributed-compilation test. Needs THREE real processes -- a
# scheduler, a worker and the launcher -- which is what makes it a script rather
# than a Catch2 case. What it asserts is that an object compiled in another
# process on another port is byte-identical to a locally compiled one, that a
# dispatched result still populates the cache, that a worker registered for a
# different toolchain is never chosen, and that every refusal path still ends in
# a successful build.
#
# Registered HERE and not beside the launcher, and that is load-bearing rather
# than tidiness: src/apps processes its table in order, so at the point
# src/apps/fastcache-cc is configured the fastcache-compile-node target does not
# exist yet and a $<TARGET_FILE:> guard on it silently skips the test. src/tests
# is added after src/apps (see the top-level CMakeLists), so every target this
# needs is defined by the time this runs -- which is also why
# .agent/rules/testing.md names this file as the home for script-driven tests.
#
# Both platforms run it, but they do NOT assert the same contract, and that is
# stated rather than left as an apparent omission. The Windows script covers the
# four cases that turn on the MSVC driver -- an equivalent object, still a cache,
# a C translation unit staying C, and fingerprint isolation -- because the driver
# is the only Windows-specific thing on this path: `/E` for the dispatch
# preprocess and `/TP`//`/TC` for the language are assertions about a driver
# nothing else exercises, and if either is wrong every dispatched TU fails and is
# retried locally, so the build is green and never once helped. Everything the
# POSIX fixture additionally covers -- failover, the role gate, concurrency, the
# driver's own language default, graceful stop -- is platform-neutral code already
# pinned by unit tests.
#
# The one exception, and the reason that last clause is the load-bearing half: the
# unreachable-cache case (issue #236) is asserted on BOTH platforms, because what
# it pins is control flow in `RunCached`, and `main.cpp` is in no test target at
# all. There is no unit test standing behind it, so a fixture that exists on one
# platform pins nothing for somebody editing that function on the other. A case is
# duplicated when the thing it guards has no other guard -- not when it is merely
# convenient to repeat.
#
# "Equivalent" and not "byte-identical" on Windows, and that difference is
# measured: both MSVC drivers stamp the clock into the COFF header, and `cl`
# additionally records the absolute path of the OBJECT inside the object, so a
# worker's output can never be byte-identical to a local build's. Every section
# carrying code or data is, and the fixture's own comparison is what says so --
# which is why that comparison has a self-test of its own below.
# ---------------------------------------------------------------------------
# The compile worker reads its configuration file (#291), driven through the
# real binary.
#
# `ApplyNodeConfiguration` is unit-tested on every platform, which is a different
# claim from "the process reads a file": `PurgeExpired` was correct and tested
# and had no production caller at all. This asserts the composition in `main`.
#
# It also asks the three questions only a process can answer -- absent,
# unreadable and malformed -- each of which must produce its OWN named outcome
# rather than the nearest neighbour, since the exit code and one line of stderr
# are everything an operator gets.
#
# NOT labelled "smoke": it opens no socket, starts no daemon and needs no
# compiler. `--print-surfaces` renders the resolved configuration and exits, so
# every case is a process that lives for milliseconds.
if(NOT TARGET fastcache-compile-node)
    message(STATUS "node-config-file-e2e: compile node not built -- skipping")
elseif(WIN32)
    # POSIX-only, the way the fixtures below record for themselves: a PowerShell
    # twin would be a translation of this script rather than new coverage, and the
    # one case that needs a platform -- an unreadable file -- is a POSIX mode.
    message(STATUS "node-config-file-e2e: POSIX-only fixture -- skipping on Windows")
else()
    add_test(
        NAME "node-config-file-e2e"
        COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/node-config-file-e2e.sh"
            "$<TARGET_FILE:fastcache-compile-node>"
    )
    set_tests_properties("node-config-file-e2e" PROPERTIES
        LABELS "hygiene"
        TIMEOUT 120
    )
endif()

# ---------------------------------------------------------------------------
# fleet-dashboard-e2e: the leader's dashboard, over a real socket.
#
# Registered HERE rather than beside the node for the reason `dist-compile-e2e`
# is: `src/apps` walks its app table in order, so a test registered beside one
# binary cannot name another, and a `$<TARGET_FILE:>` guard on a target
# configured later does not fail -- it silently skips the test forever.
#
# What it covers that the unit tests cannot: that the route rows reach the accept
# loop over a real bound port, that `/metrics` on that same port is byte-for-byte
# unaffected by the credential (the assertion protecting every existing scraper),
# and that a node registering with its own scheduler reaches the rendered page.
if(NOT TARGET fastcache-compile-node)
    message(STATUS "fleet-dashboard-e2e: compile node not built — skipping")
elseif(WIN32)
    # POSIX-only for now, the way `cluster-e2e` records for itself: the fixture is
    # `/dev/tcp` and bash, and a PowerShell twin is a translation rather than new
    # coverage.
    message(STATUS "fleet-dashboard-e2e: POSIX-only fixture — skipping on Windows")
else()
    add_test(
        NAME "fleet-dashboard-e2e"
        COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/fleet-dashboard-e2e.sh"
            --node "$<TARGET_FILE:fastcache-compile-node>"
    )
    # Starts a real node with three listeners, so it must not race the other
    # server-starting smoke tests for ports.
    set_tests_properties("fleet-dashboard-e2e" PROPERTIES
        LABELS "smoke"
        SKIP_RETURN_CODE 77
        RUN_SERIAL TRUE
        TIMEOUT 600
    )

    # The same fixture over HTTPS, registered only where TLS is compiled in --
    # the shape `tls-smoke` already takes, and for the same reason: a test that
    # could never pass in the default build is dead configuration rather than
    # coverage. It additionally proves the handshake is REAL, by checking that a
    # plaintext request to the TLS port is not served: a port that merely answers
    # would otherwise pass every assertion above it.
    if(FASTCACHED_ENABLE_TLS)
        add_test(
            NAME "fleet-dashboard-tls-e2e"
            COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/fleet-dashboard-e2e.sh"
                --node "$<TARGET_FILE:fastcache-compile-node>"
                --tls-cert "${CMAKE_SOURCE_DIR}/testdata/tls/server.crt"
                --tls-key "${CMAKE_SOURCE_DIR}/testdata/tls/server.key"
        )
        set_tests_properties("fleet-dashboard-tls-e2e" PROPERTIES
            LABELS "smoke"
            SKIP_RETURN_CODE 77
            RUN_SERIAL TRUE
            TIMEOUT 600
        )

        # And the spelling that needs no certificate to exist first. Separate from
        # the case above rather than folded into it: the generated certificate has
        # its own properties to assert -- that it is valid for the names an
        # operator types, that it is NOT valid for others, and that the fingerprint
        # the node logged is the one on the wire -- and a fixture asserting two
        # different things reports which one broke by not saying.
        add_test(
            NAME "fleet-dashboard-self-signed-e2e"
            COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/fleet-dashboard-e2e.sh"
                --node "$<TARGET_FILE:fastcache-compile-node>"
                --tls-self-signed
        )
        set_tests_properties("fleet-dashboard-self-signed-e2e" PROPERTIES
            LABELS "smoke"
            SKIP_RETURN_CODE 77
            RUN_SERIAL TRUE
            TIMEOUT 600
        )
    endif()
endif()

# ---------------------------------------------------------------------------
# Two compile-node PROCESSES on one host must not share a scratch root (#279).
#
# Registered here rather than beside the node for the reason `dist-compile-e2e` is:
# it names three executables, and `src/apps` walks its app table in order, so a test
# registered beside one binary cannot name another.
#
# What it covers that `ScratchClaim_test.cpp` cannot: those cases prove the claim
# MECHANISM, this proves the WIRING -- that a node actually claims a root before it
# compiles. The defect was never a broken mechanism; there was none, and every unit
# test in the tree passed.
#
# Windows-only for now, the way `fleet-dashboard-e2e` records for itself. The claim
# path for both platforms is covered by the unit tests, which run everywhere; what is
# missing here is the POSIX half of the ORCHESTRATION, not of the guard.
if(NOT TARGET fastcached OR NOT TARGET fastcache-compile-node OR NOT TARGET fastcache-cc)
    message(STATUS "node-scratch-isolation-e2e: daemon, compile node or launcher not built — skipping")
elseif(NOT WIN32)
    message(STATUS "node-scratch-isolation-e2e: Windows-only fixture — skipping")
else()
    add_test(
        NAME "node-scratch-isolation-e2e"
        COMMAND pwsh -NoProfile -File "${CMAKE_SOURCE_DIR}/scripts/node-scratch-isolation-e2e.ps1"
            -Fastcached "$<TARGET_FILE:fastcached>"
            -Node "$<TARGET_FILE:fastcache-compile-node>"
            -Launcher "$<TARGET_FILE:fastcache-cc>"
    )
    # Starts three nodes and a daemon twice over and drives real compiles, so it must
    # not race the other server-starting smoke tests for ports or for CPU.
    set_tests_properties("node-scratch-isolation-e2e" PROPERTIES
        LABELS "smoke"
        SKIP_RETURN_CODE 77
        RUN_SERIAL TRUE
        TIMEOUT 900
    )
endif()

# That fixture's WAIT CLASSIFIER (`Get-WaitVerdict`), against synthesised readings.
#
# Registered outside the block above and gated on nothing but the platform,
# because it starts no daemon, opens no socket, runs no compiler and now spawns no
# process at all -- so it is NOT labelled "smoke" and belongs in the default set,
# the same reasoning `dist-compile-e2e-selftest` and `repository-hygiene` record
# for themselves.
#
# It exists because the classifier was wrong in production and nothing noticed.
# Asked why a worker had not come up in five minutes it answered "the process was
# still WORKING (it consumed CPU throughout)" about a process that had written
# nothing for the whole wait and spent 1.1% of one core, and that verdict carried
# an instruction to raise the budget that #354 explicitly refuses. A fixture's own
# arithmetic is the one thing nothing else in this tree tests.
#
# Not RUN_SERIAL and not slow, and both for the same reason. The first version
# drove real stand-in processes arranged to consume specific amounts of CPU: it
# needed a core to itself, took 53 seconds, and still failed on CI, because the
# band it had to land a figure inside is 0.35s wide while a PowerShell process's
# own startup costs 0.2-0.5s. The decision is now a pure function over a readings
# record and the cases are rows -- 17 of them, both sides of every bound, in about
# a third of a second. See `.agent/rules/testing.md`.
if(WIN32)
    add_test(
        NAME "node-scratch-isolation-e2e-selftest"
        COMMAND pwsh -NoProfile -File "${CMAKE_SOURCE_DIR}/scripts/node-scratch-isolation-e2e.ps1" -SelfTest
    )
    set_tests_properties("node-scratch-isolation-e2e-selftest" PROPERTIES
        LABELS "hygiene"
        TIMEOUT 120
    )
endif()

# ---------------------------------------------------------------------------
# End-to-end compile-cache test. Starts a real daemon, drives real compiles
# through the launcher, and asserts miss/hit, byte-identical object
# reproduction, cross-depth portability and -- since #423 -- that a wrong object
# planted under a correct key is named rather than linked. Those are the
# properties unit tests cannot cover, because they need a compiler and a server.
#
# Registered HERE and not beside the launcher, for the reason `dist-compile-e2e`
# below records and this test learned the hard way: src/apps processes its table
# in order, so at the point src/apps/fastcache-cc is configured the
# compile-cache-testclient target does not exist yet, and `if(TARGET
# compile-cache-testclient)` there is not a condition -- it is the constant
# FALSE. The fixture would have reported its wrong-object case SKIPPED on every
# machine, in every job, forever, in a sentence that reads exactly like a
# legitimate skip. `check-target-file-guards` cannot catch that: it asks whether
# the guard was WRITTEN, which it was.
#
# Labelled "smoke" so it can be selected (ctest -L smoke) or skipped
# (ctest -LE smoke); a missing prerequisite exits 77 and reports as skipped
# rather than failed.
#
# Both platforms assert the same contract except the planted-wrong-object case,
# which is POSIX-only because it needs compile-cache-testclient -- the only thing
# that can STORE a chosen value under a chosen key. Every other way of planting
# one goes through the launcher, which derives the key from the source and so
# cannot be made to disagree with itself. The mechanism it exercises
# (`HitVerification`) is platform-neutral and unit-tested on every platform; what
# the fixture adds is the end-to-end path, and one platform proving that is what
# the rule about duplicating a case asks for.
if(NOT TARGET fastcached OR NOT TARGET fastcache-cc)
    message(STATUS "compile-cache-e2e: daemon or launcher not built — skipping")
elseif(WIN32)
    add_test(
        NAME "compile-cache-e2e"
        COMMAND pwsh -NoProfile -File "${CMAKE_SOURCE_DIR}/src/apps/fastcache-cc/run-launcher-e2e.ps1"
            -Fastcached "$<TARGET_FILE:fastcached>"
            -Launcher "$<TARGET_FILE:fastcache-cc>"
    )
else()
    # Passed when it was built, and the fixture says out loud which of the two it
    # is running as -- FASTCACHED_BUILD_TESTCLIENT is off by default and on for
    # the linux and clang-tidy jobs, so a reader of a local run and a reader of a
    # CI log are looking at different tests and must be told so.
    set(_fc_e2e_testclient "")
    if(TARGET compile-cache-testclient)
        set(_fc_e2e_testclient --testclient "$<TARGET_FILE:compile-cache-testclient>")
    endif()
    add_test(
        NAME "compile-cache-e2e"
        COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/compile-cache-e2e.sh"
            --fastcached "$<TARGET_FILE:fastcached>"
            --launcher "$<TARGET_FILE:fastcache-cc>"
            --compiler "${CMAKE_CXX_COMPILER}"
            ${_fc_e2e_testclient}
    )
endif()
if(TARGET fastcached AND TARGET fastcache-cc)
    # Binds a port and spawns a daemon, so it must not run concurrently with the
    # other server-starting smoke tests.
    set_tests_properties("compile-cache-e2e" PROPERTIES
        LABELS "smoke"
        SKIP_RETURN_CODE 77
        RUN_SERIAL TRUE
    )
endif()

# ---------------------------------------------------------------------------
if(NOT TARGET fastcached OR NOT TARGET fastcache-compile-node OR NOT TARGET fastcache-cc)
    message(STATUS "dist-compile-e2e: daemon, compile node or launcher not built — skipping")
else()
    if(WIN32)
        add_test(
            NAME "dist-compile-e2e"
            COMMAND pwsh -NoProfile -File "${CMAKE_SOURCE_DIR}/scripts/dist-compile-e2e.ps1"
                -Fastcached "$<TARGET_FILE:fastcached>"
                -Node "$<TARGET_FILE:fastcache-compile-node>"
                -Launcher "$<TARGET_FILE:fastcache-cc>"
        )
    else()
        add_test(
            NAME "dist-compile-e2e"
            COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/dist-compile-e2e.sh"
                --fastcached "$<TARGET_FILE:fastcached>"
                --node "$<TARGET_FILE:fastcache-compile-node>"
                --launcher "$<TARGET_FILE:fastcache-cc>"
                --compiler "${CMAKE_CXX_COMPILER}"
        )
    endif()
    # Spawns three daemons and several workers and drives real compiles, so it
    # must not race the other server-starting smoke tests for ports or for CPU.
    set_tests_properties("dist-compile-e2e" PROPERTIES
        LABELS "smoke"
        SKIP_RETURN_CODE 77
        RUN_SERIAL TRUE
        TIMEOUT 900
    )

    # The Windows fixture's object comparison, against synthetic COFF input.
    #
    # NOT labelled "smoke", deliberately: it starts no daemon, opens no socket and
    # runs no compiler, so it belongs in the default ctest set -- the same
    # reasoning that puts repository-hygiene there. It exists because a fixture's
    # own logic is the one thing nothing else tests, and this one's cost five CI
    # round trips to find out, the last of them on a control that compared two
    # objects with different NAMES and so reported a reproducible driver as
    # non-reproducible.
    if(WIN32)
        add_test(
            NAME "dist-compile-e2e-selftest"
            COMMAND pwsh -NoProfile -File "${CMAKE_SOURCE_DIR}/scripts/dist-compile-e2e.ps1" -SelfTest
        )
        set_tests_properties("dist-compile-e2e-selftest" PROPERTIES TIMEOUT 120)
    endif()
endif()

# ---------------------------------------------------------------------------
# A three-node cluster, elected and administered for real.
#
# Every consensus rule this repository has is asserted against a simulated
# cluster in one process, which is the right place for them -- a scripted
# partition is not something three real processes can be made to reproduce. What
# a simulation cannot reach is the wire, the transport, the timers and the
# operator's own command line all having their first say at once, and this is
# where those meet: three processes elect a leader, a follower's refusal names an
# endpoint that a client then successfully dials, a setting replicates, and the
# cluster re-forms after its leader is killed.
#
# POSIX only for now. The properties are platform-independent and the fixture is
# not -- it wants a shell, `/dev/tcp` and process control -- so a Windows
# counterpart is a translation rather than new coverage, and the unit tests carry
# the rules on both.
#
# Registered here rather than beside the node for the reason dist-compile-e2e is:
# `src/apps` walks its app table in order, so a test registered next to one
# binary cannot name one that comes later -- and a `$<TARGET_FILE:>` guard on a
# target that does not exist yet does not fail, it silently skips the test
# forever.
if(NOT WIN32)
    if(NOT TARGET fastcache-compile-node)
        message(STATUS "cluster-e2e: compile node not built — skipping")
    else()
        add_test(
            NAME "cluster-e2e"
            COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/cluster-e2e.sh"
                --node "$<TARGET_FILE:fastcache-compile-node>"
        )
        # Starts three nodes that hold six ports between them and drives an
        # election, so it must not race the other server-starting smoke tests.
        set_tests_properties("cluster-e2e" PROPERTIES
            LABELS "smoke"
            SKIP_RETURN_CODE 77
            RUN_SERIAL TRUE
            TIMEOUT 300
        )
    endif()
endif()

# ---------------------------------------------------------------------------
# FASTCACHE_AUTO_START spawn+probe (issue #90), the real-binary counterpart to
# compile-cache-daemon-staging above. That test proves the staging plumbing
# with fake --version-only stand-ins; a fake has no socket to accept on, so it
# cannot prove _fc_auto_start_fastcached actually starts something that
# answers a real connection. This drives the real, freshly built fastcached
# and fastcache-cc instead: a configure with no daemon reachable spawns one, a
# real translation unit compiled through the configured launcher reports a
# real MISS then a real HIT against it, and a second configure against the
# same address finds that daemon rather than starting another.
#
# Registered here rather than beside the launcher for the same reason
# dist-compile-e2e is: this needs the fastcached target, so it belongs after
# src/apps has defined it. POSIX only for now, matching
# check-compile-cache-install.cmake's and check-compile-cache-autoinstall.cmake's
# own scoping — the process bookkeeping here is pidfile-based and POSIX's
# --daemon has no Windows equivalent; Windows coverage of FASTCACHE_AUTO_START
# is left as follow-up work.
if(NOT WIN32 AND TARGET fastcached AND TARGET fastcache-cc)
    add_test(
        NAME "compile-cache-daemon-start"
        COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/check-compile-cache-daemon-start.sh"
            --fastcached "$<TARGET_FILE:fastcached>"
            --launcher "$<TARGET_FILE:fastcache-cc>"
            --compiler "${CMAKE_CXX_COMPILER}"
            --source-dir "${CMAKE_SOURCE_DIR}"
    )
    # Starts a real daemon on a real port, same as dist-compile-e2e, so it must
    # not race the other server-starting smoke tests.
    set_tests_properties("compile-cache-daemon-start" PROPERTIES
        LABELS "smoke"
        SKIP_RETURN_CODE 77
        RUN_SERIAL TRUE
        TIMEOUT 120
    )
endif()

# ---------------------------------------------------------------------------
# sccache smoke tests. Registered only when an sccache binary is present
# (SCCACHE is the find_program result from cmake/portable/CompileCache.cmake). Each test
# the freshly-built fastcached, points sccache at it over one wire protocol,
# and asserts a cache hit — the same compile-twice check the CI smoke jobs run,
# via the same scripts/sccache-smoke driver.
#
# They start a real server and invoke the real compiler, so they are slow and
# environment-dependent: the "smoke" label lets you select (ctest -L smoke) or
# skip (ctest -LE smoke) them, and a missing runtime prerequisite exits 77 to
# report as skipped rather than failed.

# ---------------------------------------------------------------------------
# TLS smoke test. Registered only in TLS-enabled builds. Starts the freshly
# built fastcached with the checked-in self-signed cert, connects a real TLS
# client, and asserts a RESP PING is answered +PONG over the encrypted channel
# — end-to-end coverage of the TlsSocket handshake/pump. A missing runtime
# prerequisite exits 77 (reported as skipped).
#
# The daemon target is part of the guard, not only the TLS option: a
# `$<TARGET_FILE:>` naming a target that does not exist is a hard generate
# error, so `-DFASTCACHED_BUILD_DAEMON=OFF` failed the configure outright
# instead of skipping this. Every other daemon-starting block here already
# tests `TARGET fastcached`; these two were the omissions.
if(NOT TARGET fastcached)
    message(STATUS "tls-smoke: daemon not built — skipping")
elseif(FASTCACHED_ENABLE_TLS)
    if(WIN32)
        add_test(
            NAME "tls-smoke"
            COMMAND pwsh -NoProfile -File "${CMAKE_SOURCE_DIR}/scripts/tls-smoke.ps1"
                -Fastcached "$<TARGET_FILE:fastcached>"
                -Cert "${CMAKE_SOURCE_DIR}/testdata/tls/server.crt"
                -Key "${CMAKE_SOURCE_DIR}/testdata/tls/server.key"
        )
    else()
        add_test(
            NAME "tls-smoke"
            COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/tls-smoke.sh"
                --fastcached "$<TARGET_FILE:fastcached>"
                --cert "${CMAKE_SOURCE_DIR}/testdata/tls/server.crt"
                --key "${CMAKE_SOURCE_DIR}/testdata/tls/server.key"
        )
    endif()
    # RUN_SERIAL for the reason every other daemon-starting test here carries it:
    # this one binds a port and spawns fastcached, so under `ctest --parallel` it
    # would race the sccache smokes and compile-cache-daemon-start for it.
    set_tests_properties("tls-smoke" PROPERTIES
        LABELS "smoke"
        SKIP_RETURN_CODE 77
        RUN_SERIAL TRUE
    )
endif()

if(NOT TARGET fastcached)
    message(STATUS "sccache smoke tests: daemon not built — skipping")
elseif(SCCACHE)
    set(FASTCACHED_SMOKE_PORT "11611" CACHE STRING "TCP port the sccache smoke tests bind fastcached to")

    if(WIN32)
        set(_smoke_driver pwsh -NoProfile -File "${CMAKE_SOURCE_DIR}/scripts/sccache-smoke.ps1")
    else()
        set(_smoke_driver bash "${CMAKE_SOURCE_DIR}/scripts/sccache-smoke.sh")
    endif()

    foreach(_proto IN ITEMS memcached redis)
        add_test(
            NAME "sccache-smoke-${_proto}"
            COMMAND ${_smoke_driver}
                --fastcached "$<TARGET_FILE:fastcached>"
                --protocol ${_proto}
                --port ${FASTCACHED_SMOKE_PORT}
                --compiler "${CMAKE_CXX_COMPILER}"
        )
        # Both tests bind the same port and drive one shared sccache server, so
        # they must not run concurrently with each other.
        set_tests_properties("sccache-smoke-${_proto}" PROPERTIES
            LABELS "smoke"
            SKIP_RETURN_CODE 77
            RUN_SERIAL TRUE
        )
    endforeach()
else()
    message(STATUS "sccache not found — skipping sccache smoke tests")
endif()

# ---------------------------------------------------------------------------
# migrate-storage-e2e: `fastcached --migrate-storage` over both storage shapes.
#
# What it covers that the unit tests cannot: `MigrateConfiguredStorage` and
# `StorageShardPaths` live in `main.cpp`, which is in no test target -- and the
# property that actually decides whether the feature works for an operator is
# that the flag enumerates the SAME files the daemon opens. A conversion walking
# a different set would exit zero and leave the daemon still refusing to start on
# the store it missed.
if(NOT TARGET fastcached)
    message(STATUS "migrate-storage-e2e: daemon not built — skipping")
elseif(WIN32)
    # POSIX-only fixture, the way `fleet-dashboard-e2e` records for itself: it is
    # bash and `mktemp`, and a PowerShell twin would be a translation rather than
    # new coverage. The paths under test are covered on Windows by the manual
    # check recorded in the pull request.
    message(STATUS "migrate-storage-e2e: POSIX-only fixture — skipping on Windows")
else()
    add_test(
        NAME "migrate-storage-e2e"
        COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/migrate-storage-e2e.sh"
            --fastcached "$<TARGET_FILE:fastcached>"
    )
    # Starts a real daemon to create the stores, so it must not race the other
    # server-starting smoke tests.
    set_tests_properties("migrate-storage-e2e" PROPERTIES
        LABELS "smoke"
        SKIP_RETURN_CODE 77
        RUN_SERIAL TRUE
        TIMEOUT 300
    )
endif()
