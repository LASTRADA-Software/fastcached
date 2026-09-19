# SPDX-License-Identifier: Apache-2.0
#
# Policies are pinned because a `cmake -P` script gets OLD defaults for every
# policy the project has not stated, which has already cost this tree once
# (CMP0057, where `if(... IN_LIST ...)` silently did nothing in a check).
cmake_minimum_required(VERSION 3.28)
#
# Test-name hygiene: fail if a Catch2 case is named something the CTest
# registration cannot pass back to the runner.
#
# `catch_discover_tests` registers each case as
# `add_test(NAME <case> COMMAND <exe> <case>)`, so the name is handed to Catch2
# as a command-line ARGUMENT. Catch2 parses its arguments before it parses test
# specs, so a name beginning with `-` is read as an option:
#
#   * `--help wins over whatever follows it` — Catch2 recognises `--help`,
#     prints its usage and exits 0. CTest reports a PASS for a case that never
#     ran. That is the shape this repository keeps a list about: nothing fails,
#     and the thing an operator was told is covered is not.
#   * `--cache-dir gives the node an on-disk tier` — Catch2 does not recognise
#     it, prints `Unrecognised token` and exits non-zero. CTest reports a
#     failure the case does not have, and it passes when run by hand, which is
#     the worst way to spend an afternoon.
#
# Both were live in this tree. The first had been silently not running since it
# was written.
#
# Runs as `cmake -P`, for the reasons check-repository-hygiene.cmake gives at
# length: this reads files, compares strings and reports, so a .sh + .ps1 pair
# would be two implementations of one rule differing only in syntax.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -P scripts/check-test-names.cmake
#
# Exit codes: 0 = every case name was READ and survives the round trip. 1 = one
# does not, OR one could not be read at all -- a case whose name this file cannot
# extract is a case it does not check, which is not a pass (#1261).

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()

# ---------------------------------------------------------------------------
# Every Catch2 macro that names a case -- a TABLE, and a refusal for the ones it
# does not name.
#
# This was four spellings in an alternation, under a comment saying "all four, not
# just the one this tree happens to use today". Catch2 declares SIXTEEN, so the
# sentence was true about the list and false about Catch2:
# `TEMPLATE_TEST_CASE_METHOD` and `SCENARIO_METHOD` were both absent, and neither
# is exotic. The list being short is not what made that a defect -- a list is
# exact about what it knows and SILENT about what it does not, and silence here
# reads identically to complete coverage (#1261).
#
# So the pattern captures the IDENTIFIER and membership is asked of this table,
# which buys two things an alternation cannot:
#
#   * no prefix ordering to get right. `(TEST_CASE|TEST_CASE_METHOD)` against
#     `TEST_CASE_METHOD(` depends on the regex engine backtracking out of the
#     first alternative, which is a property of CMake's engine rather than of this
#     rule. Capturing the whole identifier asks no such question.
#   * a macro NOT in this table whose name still carries `TEST_CASE` or
#     `SCENARIO` is refused BY NAME rather than skipped, so the next Catch2
#     spelling -- or the next local wrapper around one -- arrives as a refusal
#     that says what to do instead of as a case nobody checks.
#
# The name is the first string LITERAL of the invocation for every row: where the
# case name is not the first argument (`TEST_CASE_METHOD` takes a fixture type,
# `METHOD_AS_TEST_CASE` a method, `REGISTER_TEST_CASE` a function) the arguments
# ahead of it are never string literals, so there is nothing for the match to
# reach by mistake.
set(FastCachedCaseMacros
    TEST_CASE
    TEST_CASE_METHOD
    TEMPLATE_TEST_CASE
    TEMPLATE_TEST_CASE_SIG
    TEMPLATE_TEST_CASE_METHOD
    TEMPLATE_TEST_CASE_METHOD_SIG
    TEMPLATE_PRODUCT_TEST_CASE
    TEMPLATE_PRODUCT_TEST_CASE_SIG
    TEMPLATE_PRODUCT_TEST_CASE_METHOD
    TEMPLATE_PRODUCT_TEST_CASE_METHOD_SIG
    TEMPLATE_LIST_TEST_CASE
    TEMPLATE_LIST_TEST_CASE_METHOD
    METHOD_AS_TEST_CASE
    REGISTER_TEST_CASE
    SCENARIO
    SCENARIO_METHOD
)

# Any all-caps identifier opening a call. The row lookup decides what it is; this
# only has to find the candidate.
set(FastCachedMacroOpenPattern "^[ \t]*([A-Z][A-Z0-9_]*)[ \t]*\\(")

# A candidate this table does not name, but which is in the family: refused rather
# than skipped. Deliberately NOT anchored on the whole identifier -- a wrapper
# named `FASTCACHE_TEST_CASE` is exactly as invisible to the rows above as
# `TEMPLATE_TEST_CASE_METHOD` was.
set(FastCachedCaseMacroFamilyPattern "(TEST_CASE|SCENARIO)")

# How many lines past the one a macro OPENS on this will look for the case name.
# A wrapped invocation puts it on the next line; the bound is loose because the
# cost of a miss is a case excluded from every check in this file, and tight
# because running off into the body would start reading string literals that are
# not names. Exceeding it is a REFUSAL (see `unreadable` below), never a skip.
set(FastCachedCaseNameLookahead 8)

# ---------------------------------------------------------------------------
# Duplicated case names, and the ONE pair that is allowed (#729).
#
# `catch_discover_tests` registers `add_test(NAME <case> COMMAND <exe> <case>)`
# per case, so two cases sharing a name produce two ctest entries that each run
# the binary filtered by that name -- and Catch2 matches BOTH cases for either
# entry. Nothing is skipped, which is why this reads as harmless and is not:
#
#   * one defect surfaces as TWO red ctest entries, and neither names the case
#     that actually failed;
#   * `ctest -R "^<name>$"` cannot select one of the pair, so a bisect or a
#     targeted re-run silently runs both;
#   * per-case timings and the `--repeat` flake-hunting workflows are attributed
#     to a name that cannot say which case it was.
#
# **The exemptions are a stated decision, not a silent one**, which is the
# `RefuseWithoutCounter` shape this tree uses elsewhere: a row carries its
# REASON, so an allowed duplicate cannot be spelled the same way as a forgotten
# one. And a row that has STOPPED describing a duplicate is refused rather than
# ignored -- an exemption list that can rot silently is a second source of truth,
# and the one thing worse than no exemption is one nobody has to keep true.
#
# An exempted name may not contain `;`, `[`, `]` or `|`: these rows ARE a CMake
# list, which is the hazard this file's reader comment is about. Checked below
# rather than left as a comment nothing reads.
#
# name|reason
set(FastCachedDuplicateNameExemptions
    "A handler freed earlier in the same batch is not dispatched|EpollReactor_test.cpp is wholly inside `#if defined(__linux__)` and KqueueReactor_test.cpp wholly inside `#if defined(__APPLE__)`, so the two cases never co-exist in one build and never register as two ctest entries. The parallel naming is deliberate: it is what makes the same property greppable across the two platform reactors."
)

# ---------------------------------------------------------------------------
# Which files hold Catch2 cases. EVERY C++ source under `src/`, not the
# `*_test.cpp` suffix the convention uses.
#
# The suffix is a claim about where cases live, and it was wrong about eight files
# carrying seventeen cases: the five `src/apps/fastcache-bench/*Bench.cpp`, and
# `src/tests/`'s `CatchSkipCanary.cpp`, `ErrorPopupCanary.cpp` and
# `OffThreadAssertionCanary.cpp`. Some of those sit outside the convention on
# purpose -- a canary is not a test of the tree -- but a case in one is registered
# by `catch_discover_tests` exactly like any other, so it can collide with a name
# elsewhere and it can begin with a dash. Being deliberately outside the naming
# convention is not being outside the round trip (#1261).
#
# Headers too: nothing stops a case being declared in one, and "nothing stops it"
# is the whole argument -- the file set is not a place to bet on how this tree is
# arranged.
#
# The cost is paid back by the pre-filter below: the widened set reads about four
# times as many files and walks the newlines of only the ones that hold a
# candidate, so the run time is within noise of the narrow glob's.
#
# Through `fastcached_tracked_files`, which is this tree's one answer to HOW a check
# finds its file set, and not a raw `file(GLOB_RECURSE)`. The difference only became
# load-bearing when the set widened: `*_test.cpp` was a name a stray file is unlikely
# to carry, while every `.cpp` and `.hpp` under `src/` catches an untracked WIP file,
# a generated header or an editor's copy -- so a raw glob would enforce one rule on a
# developer's machine and another in CI. It also carries the MODE, which the vacuity
# refusal below names.
include("${CMAKE_CURRENT_LIST_DIR}/lib/CheckCommon.cmake")
fastcached_tracked_files("${FASTCACHED_SOURCE_DIR}"
    PATHSPECS "src/*.cpp" "src/*.hpp"
    GLOBS "src/*.cpp" "src/*.hpp"
    FILTER "\\.(cpp|hpp)$"
    FILES_OUT testSources MODE_OUT fileSetMode)

# Take the next line of `sourceRest` into `line`, advancing `sourceRest` and `lineNo`.
#
# A `macro()` and not a `function()`, deliberately: it has to write three variables in
# the caller's scope, and CMake's function scoping would need three `PARENT_SCOPE`
# writes per call in the hottest loop this check has. It takes NO arguments, which is
# what keeps it clear of the textual-substitution hazard AGENT.md warns about -- there
# is nothing for CMake to re-parse.
#
# One copy because there were two: the walk below and the wrapped-name lookahead both
# split on a newline, and the second was the first with `set(line ...)` replaced by an
# append.
macro(TakeLine)
    math(EXPR lineNo "${lineNo} + 1")
    string(FIND "${sourceRest}" "\n" sourceNewline)
    if(sourceNewline EQUAL -1)
        set(line "${sourceRest}")
        set(sourceRest "")
    else()
        string(SUBSTRING "${sourceRest}" 0 ${sourceNewline} line)
        math(EXPR sourceNewline "${sourceNewline} + 1")
        string(SUBSTRING "${sourceRest}" ${sourceNewline} -1 sourceRest)
    endif()
endmacro()

set(violations "")
set(unknownMacros "")
set(unreadable "")
set(missingFiles "")
set(readCount 0)
set(scannedCount 0)
set(caseCount 0)

foreach(source IN LISTS testSources)
    math(EXPR readCount "${readCount} + 1")
    # Read the bytes and walk the newlines. Never build a CMake list from file
    # content, and never neutralise anything in it.
    #
    # `file(STRINGS)` returns a LIST, and one UNBALANCED `[` or `]` makes CMake's
    # list parser swallow every following element. Measured on this check: one `]`
    # appended to a TEST_CASE line took it from 2992 cases to 2985, and it PASSED
    # both times -- the file count did not move, so nothing in the summary said so.
    #
    # The REGEX argument does not protect it: it filters BEFORE the list is built.
    #
    # Neutralising the brackets -- the fix the other converted readers use -- is
    # WRONG here and was tried first. Catch2 tags are literally `[cache]` and a case
    # name may contain brackets, so replacing them rewrites the very text this check
    # extracts and round-trips: it moved the count to 2990 and altered every tag.
    #
    # Walking newlines preserves the text exactly and costs nothing measurable --
    # 1s across all 192 sources against 1s for `file(STRINGS)`, same 2992 lines.
    # `fastcached_tracked_files` answers in paths RELATIVE to the source directory,
    # which is what a reported site wants anyway.
    set(relative "${source}")
    # A path the INDEX names and the tree does not have. Its own outcome, for the
    # reason `check-control-bytes.cmake` and `check-bash32-parse.sh` grew the same
    # arm: this file set comes from `git ls-files`, so a stale index, an interrupted
    # checkout or a dangling symlink lands here -- and without the guard `file(READ)`
    # aborts with a raw `CMake Error` naming CMake rather than this check, which sends
    # whoever meets it to the wrong place. Counted nowhere, so `readCount` stays a
    # count of files actually read.
    # Its OWN list, not `unreadable`, which means "a case whose NAME could not be
    # read". A file that is not there and a case whose name is unreachable are fixed
    # in different places, and folding them would be this batch's own subject landing
    # on this batch's own work.
    if(NOT EXISTS "${FASTCACHED_SOURCE_DIR}/${source}")
        list(APPEND missingFiles "  ${source}")
        continue()
    endif()
    file(READ "${FASTCACHED_SOURCE_DIR}/${source}" sourceRest)
    string(REPLACE "\r\n" "\n" sourceRest "${sourceRest}")
    # The pre-filter that pays for the widened file set. A strict SUPERSET of what
    # the walk below can find -- no line anchor, and it sees mentions in comments and
    # strings too -- so it fails toward doing the walk. `scannedCount` is what it
    # leaves: the files this check actually read line by line, reported beside
    # `readCount` so a pre-filter that stopped matching is visible as a number rather
    # than as a clean run.
    #
    # Two `string(FIND)` calls rather than one `MATCHES` over the family pattern, and
    # the reason is measured rather than stylistic: an alternation has no literal to
    # prescan, so CMake attempts a match at every offset across all 19 MB this reads,
    # while `FIND` is `std::string::find`. Ten paired runs: 5391 ms to 5093 ms, the
    # FIND variant winning 9 of 10, same `5121 case(s) across 318 of 1004` summary.
    # The predicate is identical because the pattern holds no metacharacter -- which
    # is why the family pattern itself stays, for the per-line test further down where
    # it is a real regex.
    string(FIND "${sourceRest}" "TEST_CASE" familyAt)
    if(familyAt EQUAL -1)
        string(FIND "${sourceRest}" "SCENARIO" familyAt)
    endif()
    if(familyAt EQUAL -1)
        continue()
    endif()
    math(EXPR scannedCount "${scannedCount} + 1")
    # Counted for every line, including the ones skipped below, so a reported
    # site is the line an editor jumps to rather than an index into the matches.
    set(lineNo 0)
    while(NOT sourceRest STREQUAL "")
        TakeLine()

        # What the REGEX argument used to do, now that the reader cannot.
        if(NOT line MATCHES "${FastCachedMacroOpenPattern}")
            continue()
        endif()
        set(macroName "${CMAKE_MATCH_1}")
        if(NOT macroName IN_LIST FastCachedCaseMacros)
            # In the family and not in the table: a case-declaring macro this
            # check has never heard of. Collected and refused below rather than
            # skipped here, because a skip is indistinguishable from a file with
            # no cases in it.
            if(macroName MATCHES "${FastCachedCaseMacroFamilyPattern}")
                list(APPEND unknownMacros "  ${relative}:${lineNo}: ${macroName}")
            endif()
            continue()
        endif()

        # The case name is the first string literal of the INVOCATION, which is
        # not always on the line the macro opens.
        #
        # It used to be the first literal on that LINE, and a line carrying none
        # `continue()`d -- so a `TEST_CASE(` whose name wraps to the next line was
        # not a violation, not a duplicate, not counted, and not reported: it left
        # the check entirely, in silence. One such case was live
        # (`src/apps/fastcache-cli/LiveEventSource_test.cpp`, whose name is long
        # enough that the formatter wraps it), and nothing in the summary could
        # have said so -- a case that is never seen cannot lower a count anybody
        # is comparing against (#1261).
        #
        # So the lines are accumulated until a COMPLETE literal is in hand. The
        # reported site stays the line the macro OPENS on, which is where an
        # editor should jump and what a reader would call the case's line.
        set(macroLineNo "${lineNo}")
        set(invocation "${line}")
        # The bound as a line NUMBER rather than a second counter: `lookahead` and
        # `lineNo` were incremented on adjacent lines and could never diverge.
        math(EXPR lookaheadLimit "${lineNo} + ${FastCachedCaseNameLookahead}")
        while(NOT invocation MATCHES "\"([^\"]*)\""
              AND NOT sourceRest STREQUAL ""
              AND lineNo LESS lookaheadLimit)
            TakeLine()
            string(APPEND invocation "\n${line}")
        endwhile()
        if(NOT invocation MATCHES "\"([^\"]*)\"")
            # A case whose name this check cannot read is a case excluded from
            # every check in this file. That is the defect above, so it is a
            # refusal rather than the `continue()` it replaced.
            list(APPEND unreadable "  ${relative}:${macroLineNo}: ${macroName}")
            continue()
        endif()
        # NOT written back to `lineNo`: the lines consumed above are gone from
        # `sourceRest`, so the counter has to stay where the walk is or every site
        # reported after this one is short by the lookahead. `macroLineNo` is what
        # a site is reported as; `lineNo` is where the file is.
        set(caseName "${CMAKE_MATCH_1}")
        math(EXPR caseCount "${caseCount} + 1")
        if(caseName MATCHES "^-")
            list(APPEND violations "  ${relative}: \"${caseName}\"")
        endif()

        # Keyed by DIGEST, never by the name itself. A case name may contain
        # `[`, `]` or `;` -- the tree's tags are literally `[cache]` -- and every
        # one of those corrupts a CMake list or a variable name. An MD5 is
        # list-safe, variable-name-safe, and needs no escaping of the text it
        # stands for. `seenKeys` is therefore a list of hex strings, which is the
        # one shape this file's reader comment says is safe to build.
        string(MD5 nameKey "${caseName}")
        if(DEFINED caseSites_${nameKey})
            set(caseSites_${nameKey} "${caseSites_${nameKey}}\n      ${relative}:${macroLineNo}")
            math(EXPR caseSeen_${nameKey} "${caseSeen_${nameKey}} + 1")
        else()
            set(caseName_${nameKey} "${caseName}")
            set(caseSites_${nameKey} "      ${relative}:${macroLineNo}")
            set(caseSeen_${nameKey} 1)
            list(APPEND seenKeys "${nameKey}")
        endif()
    endwhile()
endforeach()

# ---------------------------------------------------------------------------
# The scan found nothing, which is not the same as finding nothing wrong.
#
# Every check here reports by ACCUMULATING violations, so a scan that matched no
# file and a tree with no defect produce byte-identical output: two empty lists
# agree perfectly. Refused before any verdict is drawn from them.
if(readCount EQUAL 0 OR scannedCount EQUAL 0 OR caseCount EQUAL 0)
    message(FATAL_ERROR
        "test-name hygiene read ${readCount} file(s) under ${FASTCACHED_SOURCE_DIR}/src "
        "(file set from ${fileSetMode}), walked ${scannedCount} of them, and found "
        "${caseCount} case(s), so it is reporting on nothing. Three independent ways to "
        "get here and the numbers say which: the file set named no `.cpp` or `.hpp` "
        "under src/; the pre-filter matched none of the files it did name; or the macro "
        "table no longer names how this tree declares a case. All three make every "
        "check in this file pass vacuously.")
endif()

# ---------------------------------------------------------------------------
# A case-declaring macro this check does not know, and a case whose name it could
# not read. Two states, refused apart, and neither of them is "no defect found".
#
# Both are new refusals for what used to be a `continue()`, which is the whole of
# #1261: a case the reader stepped over was not a case it had cleared.
if(NOT unknownMacros STREQUAL "")
    list(JOIN unknownMacros "\n" report)
    message(FATAL_ERROR
        "macro(s) naming a Catch2 case that FastCachedCaseMacros does not list:\n${report}\n\n"
        "The identifier carries TEST_CASE or SCENARIO, so it declares cases that "
        "`catch_discover_tests` will register -- and this check has no row for it, so "
        "every one of those cases would go unchecked: not dash-checked, not counted, "
        "and invisible to duplicate detection.\n"
        "If it declares cases, add it to FastCachedCaseMacros in "
        "${CMAKE_CURRENT_LIST_FILE}; the case name has to be the first string literal "
        "of the invocation, as it is for every row there.\n"
        "If it does NOT declare cases, rename it so it does not read as one -- this "
        "check cannot tell the two apart from the call, and guessing in the other "
        "direction is how a case stops being checked.")
endif()

if(NOT missingFiles STREQUAL "")
    list(JOIN missingFiles "
" report)
    message(FATAL_ERROR
        "file(s) the ${fileSetMode} file set names and this tree does not have:
${report}

"
        "They were NOT read, so nothing here is a verdict about the cases in them. This "
        "is not a finding about any case name -- do not go looking for one.
"
        "A stale index, an interrupted checkout, or a dangling symlink produces it. "
        "`git status` will say which; a `git checkout -- <path>` or a re-clone fixes it.")
endif()

if(NOT unreadable STREQUAL "")
    list(JOIN unreadable "\n" report)
    message(FATAL_ERROR
        "Catch2 case(s) whose name this check could not read:\n${report}\n\n"
        "The macro opens and no complete string literal follows within "
        "${FastCachedCaseNameLookahead} line(s), so there is no name to round-trip. A "
        "case this check cannot read is a case it does not check at all -- which is "
        "not a pass, and used to be silent.\n"
        "If the name is genuinely further down, raise FastCachedCaseNameLookahead in "
        "${CMAKE_CURRENT_LIST_FILE}. If the name is not a plain string literal (a "
        "macro, a concatenation), make it one: `catch_discover_tests` has to pass the "
        "expanded name back to the runner as an argument, so a name this file cannot "
        "read is one nothing else can check either.")
endif()

# ---------------------------------------------------------------------------
# Duplicates, and the exemption rows checked in BOTH directions.
set(duplicateReport "")
set(usedExemptions "")
foreach(nameKey IN LISTS seenKeys)
    if(caseSeen_${nameKey} LESS 2)
        continue()
    endif()
    set(exemptReason "")
    foreach(row IN LISTS FastCachedDuplicateNameExemptions)
        string(FIND "${row}" "|" bar)
        if(bar EQUAL -1)
            message(FATAL_ERROR
                "exemption row carries no '|' separator, so it states no reason:\n  ${row}")
        endif()
        string(SUBSTRING "${row}" 0 ${bar} exemptName)
        if(exemptName STREQUAL "${caseName_${nameKey}}")
            math(EXPR afterBar "${bar} + 1")
            string(SUBSTRING "${row}" ${afterBar} -1 exemptReason)
            list(APPEND usedExemptions "${exemptName}")
        endif()
    endforeach()
    if(NOT exemptReason STREQUAL "")
        continue()
    endif()
    string(APPEND duplicateReport
           "\n  \"${caseName_${nameKey}}\" (${caseSeen_${nameKey}} cases)\n${caseSites_${nameKey}}")
endforeach()

if(NOT duplicateReport STREQUAL "")
    message(FATAL_ERROR
        "Catch2 case name(s) declared more than once:${duplicateReport}\n\n"
        "catch_discover_tests registers one ctest entry per NAME, and Catch2 matches "
        "every case carrying that name -- so each entry runs both cases. Nothing is "
        "skipped, which is why this looks harmless: what breaks is attribution. One "
        "failure appears as two red entries, neither naming the case that failed; "
        "`ctest -R \"^<name>$\"` cannot select one of the pair; and per-case timings "
        "and --repeat flake hunts are filed under a name that cannot say which case "
        "it was.\n"
        "Rename one of them so each states its own subject -- the tags already "
        "differ, and the name should too.\n"
        "If the two genuinely cannot co-exist in one build (mutually exclusive "
        "platform guards, say), add a row to FastCachedDuplicateNameExemptions in "
        "${CMAKE_CURRENT_LIST_FILE} carrying the REASON. An exemption is a stated "
        "decision; leaving the duplicate is not.")
endif()

# A row that has stopped describing a duplicate. Refused rather than ignored: an
# exemption nobody has to keep true is a licence that outlives its argument, and
# the next real duplicate of that name would then be waved through silently.
set(staleExemptions "")
foreach(row IN LISTS FastCachedDuplicateNameExemptions)
    string(FIND "${row}" "|" bar)
    string(SUBSTRING "${row}" 0 ${bar} exemptName)
    if(exemptName MATCHES "[][;|]")
        message(FATAL_ERROR
            "exemption name contains a character that cannot survive a CMake list "
            "(one of ; [ ] |):\n  \"${exemptName}\"\n"
            "Rename the case instead; this table cannot represent it.")
    endif()
    if(NOT exemptName IN_LIST usedExemptions)
        string(APPEND staleExemptions "\n  \"${exemptName}\"")
    endif()
endforeach()

if(NOT staleExemptions STREQUAL "")
    message(FATAL_ERROR
        "FastCachedDuplicateNameExemptions has row(s) that no longer describe a "
        "duplicated case name:${staleExemptions}\n\n"
        "The duplicate was resolved, or the case was renamed or deleted. Remove the "
        "row -- a standing exemption for a name that is now unique would silently "
        "permit the NEXT duplicate of it.\n"
        "The rule lives in ${CMAKE_CURRENT_LIST_FILE}.")
endif()

list(LENGTH FastCachedDuplicateNameExemptions exemptionCount)

if(NOT violations STREQUAL "")
    list(JOIN violations "\n" report)
    message(FATAL_ERROR
        "Catch2 case name(s) begin with '-':\n${report}\n\n"
        "catch_discover_tests passes the name to the runner as an argument, and "
        "Catch2 reads a leading dash as an option rather than as a test spec. A "
        "recognised one (--help) makes CTest report a pass for a case that never "
        "ran; an unrecognised one makes it report a failure the case does not "
        "have.\n"
        "Rename the case so the flag is not the first thing in it -- "
        "\"Naming --cache-dir gives the node an on-disk tier\" rather than "
        "\"--cache-dir gives the node an on-disk tier\".\n"
        "The rule lives in ${CMAKE_CURRENT_LIST_FILE}.")
endif()

list(LENGTH FastCachedCaseMacros caseMacroCount)
message("test-name hygiene: ${caseCount} case(s) across ${scannedCount} of ${readCount} file(s) "
        "survive the CTest round trip (file set from ${fileSetMode}), all names distinct "
        "(${caseMacroCount} case macro(s) known, ${exemptionCount} stated exemption(s))")
