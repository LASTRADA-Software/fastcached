# SPDX-License-Identifier: Apache-2.0
#
# `sccache-backend-caveat` must be SEEN to report what it cost, and seen to stay
# QUIET when there is nothing worth reporting.
#
# That check reads 800-odd files one at a time and does essentially no compute,
# so what it costs is set by the filesystem rather than by the check. A bare
# `ctest` timeout cannot say which of its known conditions a reader is looking
# at, so the check narrates its measured per-file cost once a run is already
# slow, and ctest captures the output of a test it kills. The figures and the
# conditions they were taken under live in `FastCachedSccacheScanCostBands` in
# the check itself -- pointed at, never copied here, which is the rule this
# whole change exists to enforce.
#
# That narration only ever runs on a filesystem slow enough to trigger it. On
# every machine where the check is comfortable it would have been merged
# untested and stayed untested -- and a fixture that has never completed has
# told you nothing. This drives it in milliseconds instead, against a synthesised
# tree, by overriding the two settings that decide when it narrates.
#
# The NEGATIVE cases are the load-bearing ones, and neither is obvious:
#
#   * A check that narrated UNCONDITIONALLY would pass a test that only proves
#     narration fires, while burying every green run in progress lines until
#     nobody reads the one that matters.
#   * A frozen clock reporting `0 ms/file` is a broken instrument reporting the
#     best possible filesystem. CMake returns `SOURCE_DATE_EPOCH` from every
#     `string(TIMESTAMP)` call, so that is one environment variable away at all
#     times, and a fast run and a dead clock are otherwise the same output.
#
# A negative assertion also passes for free against output that never arrived,
# so every case additionally has to have PRODUCED something. That is checked, not
# assumed: a subprocess that failed to start satisfies every `must not appear`
# needle in the table perfectly.
#
# What this does NOT do is judge the check's VERDICT. Reproducing a verdict-clean
# tree would mean restating that script's exemption and scan-root tables here,
# and a second copy of a table is not a cross-check -- it is a second thing to be
# wrong, and it would redden this fixture whenever an unrelated exemption row is
# added. The verdict is owned by `sccache-backend-caveat` itself against the real
# source tree; this owns the instrumentation, which nothing else watches. The
# cost line is deliberately printed BEFORE the verdict in that script, so it is
# present here whatever the synthesised tree makes it conclude.
#
# Runs as `cmake -P`. See `check-script-check-signals.cmake` for why such a check
# reports failure through its OUTPUT rather than an exit code.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -DFASTCACHED_SCRATCH_DIR=<dir> \
#         -P scripts/check-sccache-backend-caveat-selftest.cmake
#
# Exit codes: 0 always. The verdict is the presence of `CMake Error` in the output.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()
if(NOT DEFINED FASTCACHED_SCRATCH_DIR)
    message(FATAL_ERROR "FASTCACHED_SCRATCH_DIR must be set")
endif()

set(check "${FASTCACHED_SOURCE_DIR}/scripts/check-sccache-backend-caveat.cmake")
if(NOT EXISTS "${check}")
    message(FATAL_ERROR "the check under test is missing: ${check}")
endif()

# ---------------------------------------------------------------------------
# The knobs the cases drive the check with. Named once and interpolated into the
# case table below, because a literal spelled in six rows and again in a guard
# that claims to validate it is the "two numbers that agree today" shape this
# change argues against everywhere else.
set(FastCachedSelftestCorpusFiles 24)
set(FastCachedSelftestCheckpointEvery 5)
set(FastCachedSelftestBudgetSeconds 180)

# A budget the check must REFUSE: below twice its narration threshold, so a run
# killed at it could be killed having explained nothing. Positive, so it is the
# relationship being refused rather than the value.
set(FastCachedSelftestCrampedBudgetSeconds 15)

# ---------------------------------------------------------------------------
# The synthesised tree.
#
# Only what the fixture actually uses is created: `README.md`, which carries the
# one recommendation, and `docs/`, which carries the corpus. The check's other
# scan roots are deliberately absent rather than mirrored -- a mirror of that
# table would be a second copy that silently stops matching, and their absence
# reaches only the verdict, which nothing here asserts on.
set(tree "${FASTCACHED_SCRATCH_DIR}/tree")
file(REMOVE_RECURSE "${FASTCACHED_SCRATCH_DIR}")
file(MAKE_DIRECTORY "${tree}/docs")

# One recommendation carrying its caveat, so the check is not additionally
# reporting that it scanned nothing. The snippet include is the caveat by
# identity, which is the whole point of there being one wording.
file(WRITE "${tree}/docs/snippets/sccache-backend-caveat.md"
    "The canonical caveat: MSVC / clang-cl, /showIncludes, use fastcache-cc.\n")
file(WRITE "${tree}/README.md"
    "Point sccache at the daemon with SCCACHE_MEMCACHED.\n\n--8<-- \"sccache-backend-caveat.md\"\n")

# The rest of the corpus: ordinary prose naming nothing, which is what nearly
# every file in the real tree is. They exist to be READ, because reading them is
# the cost this check reports on.
foreach(index RANGE 1 ${FastCachedSelftestCorpusFiles})
    file(WRITE "${tree}/docs/filler-${index}.md" "Ordinary documentation, naming no backend variable.\n")
endforeach()

# A generator that produced nothing must fail rather than let every case below
# pass over an empty tree. The corpus has to be wider than the checkpoint
# interval, or no checkpoint falls inside it and every narration case goes green
# by being unreachable -- the same vacuous pass the check under test refuses in
# its own subject.
file(GLOB_RECURSE builtFiles LIST_DIRECTORIES false "${tree}/*")
list(LENGTH builtFiles builtFileCount)
if(builtFileCount LESS_EQUAL ${FastCachedSelftestCheckpointEvery})
    message(FATAL_ERROR
        "the synthesised tree holds ${builtFileCount} file(s), which is not more than the "
        "${FastCachedSelftestCheckpointEvery}-file checkpoint interval the cases use -- no "
        "checkpoint could fall inside it, so every narration case below would pass by being "
        "unreachable rather than by working.")
endif()

set(failures "")

# Run the check under test against the synthesised tree and return everything it
# printed, with runs of whitespace collapsed to single spaces.
#
# The collapse is not tidiness. `message(FATAL_ERROR)` word-wraps its output at a
# column that depends on how long `CMAKE_CURRENT_BINARY_DIR` is, which differs
# per preset, per checkout and per CI runner -- so a multi-word needle can break
# across a line on one machine and not another. `check-psk-signing-seam-selftest`
# already paid for this once, on a tree it had been passing on. Every needle
# below is a multi-word phrase, and the failure lands hardest on the NEGATIVE
# assertions: a wrapped line makes a `must not appear` needle pass for free.
#
# @param epoch A SOURCE_DATE_EPOCH value to run under, or "-" for none.
# @param extraArgs Space-separated extra `-D` arguments, or "-" for none.
# @param outOutput Set to the check's combined stdout and stderr, whitespace collapsed.
function(fastcached_run_caveat_check epoch extraArgs outOutput)
    set(command "${CMAKE_COMMAND}")
    if(NOT epoch STREQUAL "-")
        # `execute_process` has no ENVIRONMENT option on this project's minimum
        # CMake, so the variable is placed by `cmake -E env` rather than exported
        # here -- which would leak into every later case in this script.
        set(command "${CMAKE_COMMAND}" -E env "SOURCE_DATE_EPOCH=${epoch}" "${CMAKE_COMMAND}")
    endif()

    set(extraArgsList "")
    if(NOT extraArgs STREQUAL "-")
        string(REPLACE " " ";" extraArgsList "${extraArgs}")
    endif()

    execute_process(
        COMMAND ${command} "-DFASTCACHED_SOURCE_DIR=${tree}" ${extraArgsList} -P "${check}"
        OUTPUT_VARIABLE captured ERROR_VARIABLE capturedErrors)
    set(combined "${captured}${capturedErrors}")
    string(REGEX REPLACE "[ \t\r\n]+" " " combined "${combined}")
    set(${outOutput} "${combined}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# The cases.
#
#   <name>|<SOURCE_DATE_EPOCH or ->|<extra -D args or ->|<substrings that must ALL appear>|<substrings that must NONE appear>
#
# Both substring fields are ' && '-separated. No field may contain a '|' or a
# ';': the first is the row separator and the second splits the CMake list,
# which is how the check's own cost-band table lost half a sentence silently.
# The field count is asserted per row below rather than trusted.
#
# `narrates` and `quiet at defaults` differ in exactly ONE argument -- the
# narration threshold -- so what separates them is the threshold and nothing
# else. Give the quiet case the DEFAULT checkpoint interval instead and it goes
# green for the wrong reason: a 26-file tree can never reach a 100-file
# checkpoint, so it would stay silent even had narration become unconditional.
set(FastCachedSccacheSelftestCases
    "narrates once a run is slow|-|-DFASTCACHED_SCAN_NARRATE_AFTER=0 -DFASTCACHED_SCAN_PROGRESS_EVERY=${FastCachedSelftestCheckpointEvery} -DFASTCACHED_SCAN_BUDGET_SECONDS=${FastCachedSelftestBudgetSeconds}|still scanning --  && file(s) after  &&  ms/file, so the whole scan needs |narrated nothing"
    "quiet at defaults, and reports headroom against the budget|-|-DFASTCACHED_SCAN_PROGRESS_EVERY=${FastCachedSelftestCheckpointEvery} -DFASTCACHED_SCAN_BUDGET_SECONDS=${FastCachedSelftestBudgetSeconds}|walked  &&  ms/file. That is the ' && % of the ${FastCachedSelftestBudgetSeconds}s budget|still scanning && No budget was given"
    "reports no headroom when given no budget|-|-DFASTCACHED_SCAN_PROGRESS_EVERY=${FastCachedSelftestCheckpointEvery}|walked  &&  ms/file. That is the ' && No budget was given|% of the"
    "a frozen clock is its own outcome, never speed|1700000000|-DFASTCACHED_SCAN_NARRATE_AFTER=0 -DFASTCACHED_SCAN_PROGRESS_EVERY=${FastCachedSelftestCheckpointEvery} -DFASTCACHED_SCAN_BUDGET_SECONDS=${FastCachedSelftestBudgetSeconds}|Cost NOT MEASURED && SOURCE_DATE_EPOCH is set| ms/file && still scanning"
    "a checkpoint wider than the corpus reports that it explained nothing|-|-DFASTCACHED_SCAN_NARRATE_AFTER=0 -DFASTCACHED_SCAN_PROGRESS_EVERY=100000 -DFASTCACHED_SCAN_BUDGET_SECONDS=${FastCachedSelftestBudgetSeconds}|narrated nothing|still scanning"
    "a budget too close to the narration threshold is refused before scanning|-|-DFASTCACHED_SCAN_BUDGET_SECONDS=${FastCachedSelftestCrampedBudgetSeconds}|at least twice the narration threshold|walked "
)

# Which sense each substring field is asserted in, so the two assertion loops are
# one loop over a table rather than two blocks differing in a `NOT`.
#
#   <field holding the ' && '-separated needles>|<TRUE if they must appear>|<what the failure line says>
set(FastCachedSccacheSelftestPolarities
    "caseWanted|TRUE|did not say"
    "caseUnwanted|FALSE|said, and must not have"
)

set(caseNames "")
foreach(row IN LISTS FastCachedSccacheSelftestCases)
    # No field here contains a '|', so a plain split is enough and the general
    # row splitter (whose contract is that only the LAST field may) would be a
    # third byte-identical copy of a function for a table this file authors.
    string(REPLACE "|" ";" caseFields "${row}")
    list(LENGTH caseFields caseFieldCount)
    if(NOT caseFieldCount EQUAL 5)
        message(FATAL_ERROR
            "case row split into ${caseFieldCount} field(s) where 5 are wanted -- a '|' or a ';' "
            "has got into a field: ${row}")
    endif()
    list(GET caseFields 0 caseName)
    list(GET caseFields 1 caseEpoch)
    list(GET caseFields 2 caseArgs)
    list(GET caseFields 3 caseWanted)
    list(GET caseFields 4 caseUnwanted)

    fastcached_run_caveat_check("${caseEpoch}" "${caseArgs}" output)
    list(APPEND caseNames "${caseName}")

    # A case that produced NOTHING satisfies every `must not appear` needle
    # perfectly, so silence is refused before anything is read out of it.
    if(output STREQUAL "" OR output STREQUAL " ")
        list(APPEND failures "  ${caseName}\n      printed nothing at all, so it asserted nothing")
        continue()
    endif()

    foreach(polarityRow IN LISTS FastCachedSccacheSelftestPolarities)
        string(REPLACE "|" ";" polarityFields "${polarityRow}")
        list(GET polarityFields 0 needleField)
        list(GET polarityFields 1 mustAppear)
        list(GET polarityFields 2 complaint)

        string(REPLACE " && " ";" needles "${${needleField}}")
        foreach(needle IN LISTS needles)
            string(FIND "${output}" "${needle}" position)
            set(present TRUE)
            if(position EQUAL -1)
                set(present FALSE)
            endif()
            if(NOT present STREQUAL "${mustAppear}")
                list(APPEND failures "  ${caseName}\n      ${complaint}: ${needle}")
            endif()
        endforeach()
    endforeach()
endforeach()

# A table nothing iterated passes every assertion it never made.
if(caseNames STREQUAL "")
    list(APPEND failures "  the case table\n      produced no runs at all")
endif()

if(NOT failures STREQUAL "")
    list(JOIN failures "\n" body)
    message(FATAL_ERROR
        "sccache-backend-caveat did not report on itself as expected:\n${body}\n\n"
        "That check's cost is a property of the FILESYSTEM rather than of the check, which is "
        "why it measures itself and narrates once a run is already slow: ctest captures the "
        "output of a test it kills, so that line is what a reader of a timeout gets instead of "
        "nothing. The negative cases matter as much as the positive one -- a check that narrated "
        "always would bury every green run and still pass a test that only proves narration "
        "fires, and a frozen clock reporting 0 ms/file would report the best possible filesystem "
        "while measuring nothing.\n\n"
        "The narration settings, the budget invariant and the cost bands with the conditions "
        "they were measured under all live in ${check}. This does not judge that check's "
        "verdict -- only what it says about its own cost.")
endif()

list(LENGTH caseNames casesRun)
list(JOIN caseNames ", " caseList)
message("sccache backend caveat selftest: ${casesRun} case(s) over a ${builtFileCount}-file tree, "
        "each asserting what the check says about its own cost -- ${caseList}")
