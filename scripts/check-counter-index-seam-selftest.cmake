# SPDX-License-Identifier: Apache-2.0
#
# `counter-index-seam` must be SEEN to refuse, on each thing it claims, and to accept
# on nothing else (#1366).
#
# The cases a passing check cannot be told apart from are the vacuous ones:
#
#   `none`       -- nothing converts anywhere, as after the converter is renamed. Every
#                   file is then clean, and a check without a vacuity refusal passes
#                   forever.
#   `stale`      -- the allowed file holds fewer conversions than its row says. The row
#                   then vouches for a conversion that has moved, and waves through the
#                   next one written there.
#   `extra`      -- a SECOND conversion inside the one allowed file, which a per-file
#                   allow without a count would accept.
#
# And the opposite direction, which is how a guard gets deleted rather than fixed:
# `prose` (a comment spelling the cast) and `plural` (a cast of `counters.size()`, a
# different name) must both be accepted.
#
# Runs as `cmake -P`. See `check-script-check-signals.cmake` for why such a check
# reports failure through its OUTPUT rather than an exit code.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -DFASTCACHED_SCRATCH_DIR=<dir> \
#         -P scripts/check-counter-index-seam-selftest.cmake
#
# Exit codes: 0 always. The verdict is the presence of `CMake Error` in the output.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()
if(NOT DEFINED FASTCACHED_SCRATCH_DIR)
    message(FATAL_ERROR "FASTCACHED_SCRATCH_DIR must be set")
endif()

set(check "${FASTCACHED_SOURCE_DIR}/scripts/check-counter-index-seam.cmake")
if(NOT EXISTS "${check}")
    message(FATAL_ERROR "the check under test is missing: ${check}")
endif()

set(root "${FASTCACHED_SCRATCH_DIR}")
file(REMOVE_RECURSE "${root}")

set(failures)
set(cases 0)

# The converter file as the table allows it: exactly two conversions, and one mention in
# prose. Written as a stand-in rather than copied, so this selftest fails for reasons that
# belong to the check rather than to that header.
set(converterBody
"// CounterIndex is the one place: never static_cast<std::size_t>(counter) at a call site.\n\
inline constexpr std::size_t CounterCount = static_cast<std::size_t>(IMetricsSink::Counter::Last)\;\n\
constexpr std::optional<std::size_t> CounterIndex(Counter counter) { auto const index = static_cast<std::size_t>(counter)\; return index < CounterCount ? index : std::nullopt\; }\n")

# @param name Which synthetic tree.
# @param converter What Metrics/IMetricsSink.hpp contains.
# @param extraPath One further file, relative to src/; empty for none.
# @param extraBody What that file contains.
# @param outVar Set to the tree's root.
function(fastcached_make_tree name converter extraPath extraBody outVar)
    set(tree "${root}/${name}")
    file(REMOVE_RECURSE "${tree}")
    file(WRITE "${tree}/src/FastCache/Metrics/IMetricsSink.hpp" "${converter}")
    if(NOT extraPath STREQUAL "")
        file(WRITE "${tree}/src/${extraPath}" "${extraBody}\n")
    endif()
    set(${outVar} "${tree}" PARENT_SCOPE)
endfunction()

# @param tree Which synthetic tree to run the check against.
# @param outObjected Set TRUE when the check reported `CMake Error` or `CMake Warning`.
# @param outOutput Set to everything the check printed, whitespace runs collapsed -- `message()`
#        word-wraps, so a phrase can straddle a line break (measured in check-psk-signing-seam-selftest.cmake,
#        retired with the pre-shared key at #178).
function(fastcached_run_check tree outObjected outOutput)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DFASTCACHED_SOURCE_DIR=${tree}" -P "${check}"
        OUTPUT_VARIABLE captured ERROR_VARIABLE capturedErrors RESULT_VARIABLE ignored)
    set(combined "${captured}${capturedErrors}")
    string(REGEX REPLACE "[ \t\r\n]+" " " combined "${combined}")
    set(sawSignal FALSE)
    if(combined MATCHES "CMake Error|CMake Warning")
        set(sawSignal TRUE)
    endif()
    set(${outObjected} ${sawSignal} PARENT_SCOPE)
    set(${outOutput} "${combined}" PARENT_SCOPE)
endfunction()

# @param label The case, for the failure list.
# @param output What the check printed.
# @param needle A phrase the refusal must contain.
# @param why What it means when it does not.
function(fastcached_expect_phrase label output needle why)
    string(FIND "${output}" "${needle}" position)
    if(position EQUAL -1)
        set(failures ${failures} "${label}: ${why} (no `${needle}` in the output)" PARENT_SCOPE)
    endif()
endfunction()

# 1. Exactly the allowed conversions. Without this the check could refuse everything.
math(EXPR cases "${cases} + 1")
fastcached_make_tree("clean" "${converterBody}" "" "" tree)
fastcached_run_check("${tree}" objected output)
if(objected)
    list(APPEND failures "clean: a tree holding only the two allowed conversions was refused -- the check refuses everything")
else()
    fastcached_expect_phrase("clean" "${output}" "2 conversion(s) in the 1 file(s)"
        "it accepted without saying it saw the two conversions, so a pass cannot be told from a scan that read nothing")
endif()

# 2. A hand-rolled conversion elsewhere: the whole point.
math(EXPR cases "${cases} + 1")
fastcached_make_tree("handrolled" "${converterBody}" "FastCache/Metrics/StatsReading.cpp"
    "void Capture(Reading& reading) {\n    for (auto const& row: CounterTable)\n        reading.counters[static_cast<std::size_t>(row.counter)] = 1\;\n}" tree)
fastcached_run_check("${tree}" objected output)
if(NOT objected)
    list(APPEND failures "handrolled: a counter indexed by a cast outside the converter was accepted -- the entire rule")
else()
    fastcached_expect_phrase("handrolled" "${output}" "FastCache/Metrics/StatsReading.cpp:3:"
        "the refusal did not name the site as file:line, so it cannot be acted on")
    fastcached_expect_phrase("handrolled" "${output}" "CounterIndex"
        "the refusal did not say what to do instead, so the likely response is to widen the table")
endif()

# 3. A second conversion inside the allowed file.
math(EXPR cases "${cases} + 1")
fastcached_make_tree("extra" "${converterBody}uint64_t Read(Counter counter) { return cells[static_cast<std::size_t>(counter)]\; }\n" "" "" tree)
fastcached_run_check("${tree}" objected output)
if(NOT objected)
    list(APPEND failures "extra: a third conversion in the file allowed two was accepted -- a per-file allow without a count")
else()
    fastcached_expect_phrase("extra" "${output}" "is allowed 2 conversion(s) and holds 3"
        "it refused, but not as the allowed file holding one too many, so the cause will be hunted elsewhere")
endif()

# 4. Prose is not a conversion.
math(EXPR cases "${cases} + 1")
fastcached_make_tree("prose" "${converterBody}" "FastCache/Metrics/MetricsCatalog.hpp"
    "/// Never `table[static_cast<std::size_t>(counter)]` -- see CounterIndex.\nint Answer()\; // not static_cast<int>(counter) either" tree)
fastcached_run_check("${tree}" objected output)
if(objected)
    list(APPEND failures "prose: a comment spelling the cast was refused -- the rule cannot then be explained in the files it governs")
endif()

# 5. A different name that merely contains the word.
math(EXPR cases "${cases} + 1")
fastcached_make_tree("plural" "${converterBody}" "FastCache/Metrics/StatsReadingCodec.cpp"
    "auto const bits = static_cast<std::uint32_t>(counters.size())\;" tree)
fastcached_run_check("${tree}" objected output)
if(objected)
    list(APPEND failures "plural: a cast of counters.size() was refused -- the word boundary is gone, and every such site becomes an exemption")
endif()

# 6. std::to_underlying is the same conversion.
math(EXPR cases "${cases} + 1")
fastcached_make_tree("underlying" "${converterBody}" "apps/fastcache-cli/DashboardLoop.hpp"
    "auto const index = std::to_underlying(counter)\;" tree)
fastcached_run_check("${tree}" objected output)
if(NOT objected)
    list(APPEND failures "underlying: std::to_underlying(counter) was accepted -- a second spelling of the same conversion")
else()
    fastcached_expect_phrase("underlying" "${output}" "apps/fastcache-cli/DashboardLoop.hpp:1:"
        "the refusal did not name the site as file:line")
endif()

# 7. A test source is in scope: the next production site is written by copying it.
math(EXPR cases "${cases} + 1")
fastcached_make_tree("testsource" "${converterBody}" "apps/fastcache-cli/StatsSource_test.cpp"
    "reading.counters[static_cast<std::size_t>(IMetricsSink::Counter::ConnectionsTotal)] = 3\;" tree)
fastcached_run_check("${tree}" objected output)
if(NOT objected)
    list(APPEND failures "testsource: a test indexing a counter table by a cast was accepted")
endif()

# 8. The allowed file holds fewer conversions than its row: a stale row.
math(EXPR cases "${cases} + 1")
fastcached_make_tree("stale"
    "inline constexpr std::size_t CounterCount = static_cast<std::size_t>(IMetricsSink::Counter::Last)\;\n" "" "" tree)
fastcached_run_check("${tree}" objected output)
if(NOT objected)
    list(APPEND failures "stale: the allowed file holds one conversion of two and the check passed -- a row allowing more than the file does")
else()
    fastcached_expect_phrase("stale" "${output}" "STALE converter row"
        "it refused, but not as a stale row, so the cause will be hunted in the wrong place")
endif()

# 9. Nothing converts anywhere: the vacuous pass.
math(EXPR cases "${cases} + 1")
fastcached_make_tree("none" "constexpr std::size_t Extent = EnumeratorCount<Metric>\;\n" "" "" tree)
fastcached_run_check("${tree}" objected output)
if(NOT objected)
    list(APPEND failures "none: nothing in the tree converts a counter and the check reported success -- it had stopped looking")
else()
    fastcached_expect_phrase("none" "${output}" "found NONE"
        "it refused, but not as a scan that matched nothing, so the reader is not told the check is broken")
endif()

# 10. A tree the glob table cannot read.
math(EXPR cases "${cases} + 1")
set(tree "${root}/nosources")
file(REMOVE_RECURSE "${tree}")
file(MAKE_DIRECTORY "${tree}/src/FastCache")
file(WRITE "${tree}/src/FastCache/notes.txt" "no source here\n")
fastcached_run_check("${tree}" objected output)
if(NOT objected)
    list(APPEND failures "nosources: a tree holding no source this check can read was accepted")
else()
    fastcached_expect_phrase("nosources" "${output}" "no source file"
        "it refused, but not as a walk that found no source")
endif()

# ---------------------------------------------------------------------------
if(failures)
    list(LENGTH failures failureCount)
    string(REPLACE ";" "\n  " rendered "${failures}")
    message(FATAL_ERROR
        "counter-index-seam selftest: ${failureCount} of ${cases} case(s) wrong\n  ${rendered}")
endif()

message(STATUS "counter index seam selftest: ${cases} synthetic tree(s), every verdict as expected")
