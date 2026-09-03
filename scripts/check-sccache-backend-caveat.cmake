# SPDX-License-Identifier: Apache-2.0
#
# Every page that recommends pointing sccache at fastcached must carry, right
# there, what that costs under MSVC and clang-cl.
#
# "Point sccache at fastcached" is by definition ONE cache shared by every
# checkout and every machine pointed at it, which is the maximal form of the
# hazard in .agent/rules/build-and-toolchain.md. PR #171 put a `message(WARNING)`
# in cmake/portable/CompileCache.cmake, which reaches the audience that
# configures that module. Issue #170 is the other audience: they have their own
# build system, set the variable themselves, and never go near it -- and
# README.md additionally claimed the OPPOSITE of the hazard, which read as
# reassurance.
#
# Prose drifts the way an include graph drifts: nothing fails, nothing warns, and
# the next page pitching this is written by somebody who never saw the caveat on
# the others. So the rule is enforced rather than remembered -- the same shape as
# check-net-boundary.cmake, and for the same reason. The message this prints on
# failure is the full statement of the rule; it is deliberately not restated here.
#
# Runs as `cmake -P`, for the reasons check-repository-hygiene.cmake states at
# length: this compares strings and reports, so a .sh + .ps1 pair would be two
# implementations of one rule differing only in syntax, and cmake is the one tool
# guaranteed present.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -P scripts/check-sccache-backend-caveat.cmake
#
# Optionally, and all three only affect what this reports about ITSELF -- never
# the verdict, which is a property of the source tree alone:
#
#   -DFASTCACHED_SCAN_BUDGET_SECONDS=<n>    the wall-clock budget to report
#                                           headroom against. Omitted, the cost
#                                           is reported with no headroom line.
#   -DFASTCACHED_SCAN_NARRATE_AFTER=<n>     start narrating progress once the
#                                           scan has been running this long.
#   -DFASTCACHED_SCAN_PROGRESS_EVERY=<n>    how often, in files, to consider
#                                           narrating.
#
# Exit codes: 0 = every recommendation carries its caveat. 1 = at least one does not.

# Script mode has no project, so every policy starts UNSET: a policy-gated
# construct then errors out or silently changes meaning, and which of the two
# depends on the CMake running it. See .agent/rules/build-and-toolchain.md.
#
# This file is why that matters here. Under CMP0007 OLD, `list()` DISCARDS empty
# elements -- which is every blank line in every file scanned below, so a line
# number is off by however many blank lines precede it. It reproduced exactly
# that way: identical on CMake 4.3 (where the policy defaults NEW) and wrong from
# the first blank line on CMake 3.28, which is the version this project supports.
#
# It used to be a hand-set `if(POLICY CMP0007) cmake_policy(SET ... NEW)` block,
# under a comment opening `A cmake -P script has no cmake_minimum_required`. That
# sentence was true when it was written and this line makes it false, so both are
# gone: one declaration sets CMP0007 along with every other policy of that
# vintage, and a per-policy opt-in only covers the trap somebody already met.
cmake_minimum_required(VERSION 3.28)

# ---------------------------------------------------------------------------
# What counts as recommending the configuration. These two environment variables
# are the only way to point sccache at a fastcached daemon, so naming one IS the
# recommendation -- there is no way to describe the setup without them, and no
# other reason to write them down.
#
#   <literal>|<why naming it is a recommendation>
#
# No row may contain a ';' -- these are CMake lists, and a semicolon inside a row
# would split it into two.
set(FastCachedSccacheBackendMarkers
    "SCCACHE_MEMCACHED|Points sccache at a fastcached daemon over the memcached wire format."
    "SCCACHE_REDIS|The same, over RESP2. sccache reaches fastcached through one of these two or not at all."
)

# ---------------------------------------------------------------------------
# The canonical caveat, and the include that splices it in. A page that includes
# THIS file needs nothing else: the include is the caveat, by identity rather
# than by keyword, which is the whole point of there being one wording.
#
# The path mirrors `base_path` under `pymdownx.snippets` in mkdocs.yml and has to
# move with it. A missing snippet is separately fatal to `mkdocs build --strict`
# (mkdocs.yml sets `check_paths: true` precisely so it cannot render as silence),
# but it is checked here too because here it would otherwise present as a page
# that simply forgot its caveat -- sending whoever reads that message off to
# write a second copy of one that already exists.
set(FastCachedSccacheSnippet "docs/snippets/sccache-backend-caveat.md")
set(FastCachedSccacheSnippetInclude "^[ \t]*--8<--[ ]*\"([^\"]+)\"")

# ---------------------------------------------------------------------------
# What counts as the caveat where it cannot be included -- README.md is rendered
# by GitHub, which splices nothing. Three elements, because a caveat missing any
# one of them is one a reader is right to skip:
#
#   <element>|<accepted spellings, ' / ' separated>|<why this element is required>
#
# The spelling field is the display form as well as the match list, so the two
# cannot disagree. Presence, not wording: each surface has its own length and
# voice, and pinning the prose would make every rewording a test failure while
# still not checking that the three facts survived it.
set(FastCachedSccacheCaveatElements
    "the exposed compilers|MSVC / clang-cl|A caveat that does not say WHEN it applies is one every reader learns to skip. GCC and Clang are genuinely unaffected -- their preprocessed output carries the paths, so two checkouts never share an entry to begin with -- and a warning that fired for them too would cost exactly the case that matters."
    "the mechanism|/showIncludes / /EP|Without it the reader cannot tell a performance note from a correctness one, nor recognise the symptom when it arrives hours later somewhere else as an object built against a header it never read."
    "the remedy|fastcache-cc|A hazard with no way out reads as a reason to stop reading. The launcher in this repository does not have this failure mode -- it rewrites a hit's paths into the consuming checkout and refuses a hit whose replayed dependency is absent -- which is the whole reason it exists."
)

# How far AFTER a recommendation its caveat may sit, in lines.
#
# Forward only, and that is the load-bearing half. A caveat belongs after the
# thing it qualifies -- which is also what CliParser_test.cpp asserts about the
# rendered `--help` -- and a window that also looked backwards was measurably
# satisfied by unrelated prose: with the whole caveat deleted from README.md, a
# symmetric window still found "MSVC" and "fastcache-cc" 10 and 34 lines ABOVE
# the marker, in a different section, and reported only the mechanism missing.
# Looking forward only, the same deletion reports all three.
set(FastCachedSccacheCaveatWindow 40)

# ---------------------------------------------------------------------------
# Where to look. A file or a directory; directories are scanned recursively.
#
#   <path relative to the source root>|<why this root is scanned>
set(FastCachedSccacheScanRoots
    "README.md|The project's own pitch, and the surface issue #170 was filed about."
    "docs|The published documentation site."
    "src|Help text, an app README, and anything else that reaches a user."
    "scripts|Harnesses and checks."
    "cmake|The vendored compile-cache module and its README."
    ".agent|The rulebook."
    ".github|Workflows and issue templates. Nothing there names a backend variable today, and one that came to would most likely want an exemption row rather than a caveat -- but that is a decision to take in review, which is what scanning it forces."
)

# Which files are searched for a recommendation. Wider than the extensions this
# tree happens to use today, because a file this does not scan is a hole that
# reports green.
set(FastCachedSccacheScanGlobs
    "*.md" "*.txt" "*.rst"
    "*.cpp" "*.cc" "*.cxx" "*.hpp" "*.h" "*.hh" "*.inl" "*.ipp"
    "*.cmake" "*.sh" "*.ps1" "*.bat" "*.py"
    "*.yml" "*.yaml" "*.json" "*.in"
)

# Which of those this check is willing to JUDGE. Everything is searched; only
# prose is judged.
#
# The split is not tidiness, it is honesty about what a text scan can prove. In a
# C++ file the caveat is prose inside a string literal, and a scan of the SOURCE
# is satisfied just as happily by a comment near it -- so it would report success
# over help text that says nothing. It also has no section structure for a line
# window to approximate, and `--help`'s caveat sits 34 lines below the first
# marker with the window's whole margin consumed: three ordinary new examples
# above it turned the check red for a reason that had nothing to do with the
# caveat.
#
# So a non-prose file that names a marker is not judged and not waved through: it
# must appear in the exemption table, and its row must say what owns it instead.
# `--help` is owned by CliParser_test.cpp, which asserts against the RENDERED
# string -- strictly better evidence than anything available here.
set(FastCachedSccacheJudgedGlobs "*.md")

# ---------------------------------------------------------------------------
# Files that name a marker without recommending anything, or that something else
# is a better judge of. Every row is a decision somebody made in review, which is
# why each carries its reason -- and why a row naming a path that no longer exists
# fails this check rather than being ignored.
#
#   <path relative to the source root>|<why it is not judged here>
set(FastCachedSccacheExemptPaths
    "scripts/sccache-smoke.sh|A harness that drives a real sccache against a real daemon to prove the memcached text, memcached binary and RESP2 wire formats are all detected. It sets the variables to exercise the daemon, not to advise anyone to."
    "scripts/sccache-smoke.ps1|The Windows half of the same harness."
    "scripts/check-sccache-backend-caveat.cmake|This check. Its own marker table has to spell the variables out."
    "scripts/check-sccache-backend-caveat-selftest.cmake|This check's selftest. It synthesises a tree containing one recommendation carrying its caveat, so the marker is test DATA -- and it is deliberately written out in full rather than assembled from fragments, because a file that dodged this scan by spelling the variable in halves is exactly the hole the scan exists to close. It was caught by this check on its first ctest run, which is the mechanism working."
    "src/FastCache/Config/CliParser.cpp|The daemon's `--help`, whose sccache examples DO carry the caveat -- as a row of UsageFooters. It is owned by the `--help qualifies its sccache examples with the MSVC caveat` case in CliParser_test.cpp, which asserts the same three elements against the RENDERED usage string and additionally that the caveat reads after the examples. See the note on FastCachedSccacheJudgedGlobs above for why a source scan is the weaker of the two."
    "docs/snippets/sccache-backend-caveat.md|The caveat itself, which names the variables in the header telling an author what this check requires. Judging it would have it satisfy itself, which is a check that cannot fail."
    ".github/workflows/build.yml|Names the variable in a comment explaining why the Windows sccache assertion must be read BEFORE ctest -- the `sccache-smoke-*` tests restart the sccache server with it, zeroing the statistics -- and the smoke jobs themselves invoke the harness that sets it. That is the workflow driving the daemon, never advising anyone to build that way, which is the same ground as the sccache-smoke rows above. It is also YAML, where a caveat could only be a comment: this check cannot tell a comment stating the caveat from a comment merely mentioning the variable, so a green result here would prove nothing about what the file says. Owned by .agent/rules/build-and-toolchain.md, which states the caveat in full at the passage that comment refers to, and which this check does judge."
)

# ---------------------------------------------------------------------------
# What this check COSTS, which is a quantity under conditions and not a number.
#
# The scan is 800-odd individual file reads and essentially no compute. Measured
# on WSL2 9p (`/mnt/d`) on 2026-09-03: 4.3 s wall against 0.14 s user and 0.26 s
# sys, so 91% of the run is spent waiting on the filesystem bridge and the rest
# is rounding. That is the entire cost model -- per FILE, and set by the
# filesystem rather than by anything this script decides.
#
# Three figures for this one check were live in the tree at once (#479): the
# registration comment in src/tests/CMakeLists.txt said 0.2-0.5 s and "still far
# inside the timeout", a quiet bridged checkout measures ~4.5 s, and two lanes
# measured 53-57 s against a 60 s ceiling while several gates ran at once. Two
# orders of magnitude, and none of the three carried its conditions -- so each
# read as a fact about this check when all three are facts about a filesystem.
#
# The fix is therefore NOT a fourth number. This check measures itself and says
# so on every run, green included, so the margin is an observation somebody can
# act on rather than a claim written once and never re-taken. AGENT.md, under
# "Caching an expensive repeated answer": record a table of conditions, not a
# number; a spread states its own uncertainty and forces a citer to pick a row.
#
#   <highest ms/file this band covers, or `unbounded`>|<name>|<the conditions, and whether the per-file figure was measured or derived>
#
# Ceilings ascend and the last row is the catch-all. These are bands, not
# thresholds: nothing fails for landing in one. They exist so a reader of a slow
# run is told which of three known situations they are in instead of being left
# to re-time the check by hand, which is what #479 cost two lanes in one day.
#
# Each row says whether its per-file cost was MEASURED or DERIVED, because a
# handoff is a citation and only the sending end can make that distinction --
# AGENT.md again. Two of these three rows are wall times somebody else recorded,
# divided by a corpus count taken here; a reader who assumes all three were
# observed the same way will trust the widest one most, and it is the softest.
set(FastCachedSccacheScanCostBands
    "2|native|A filesystem reached directly -- ext4, or NTFS from a Windows cmake. DERIVED: 0.47 s reported in #479 for this class of work, over a corpus of this size. Not measured here."
    "20|bridged|WSL2 9p -- the WSL1 mechanism the older notes in this tree call DrvFs -- with the machine otherwise quiet. MEASURED: 4.0-5.1 s over this corpus across four checkouts on one machine, load average 0.6-0.9, 2026-09-03, of which 0.4 s was user+sys and the rest was wait."
    "unbounded|bridged and contended|The same bridge while several lanes build and run ctest at once. DERIVED: two lanes each timed 53-57 s twice on 2026-09-03 (#479). The per-file figure is that divided by a corpus count taken separately, on a machine state that could not afterwards be reproduced. It is the least directly observed row here and the one a slow run is most likely to land in."
)

# When to start narrating progress, in seconds, and how often to consider it, in
# files.
#
# A fast run says nothing: a progress line per hundred files would be nine lines
# of noise on every green native run, and noise is how the useful line stops
# being read. A run that is ALREADY slow narrates, because that is the only way
# the reader of a `ctest` TIMEOUT learns which kind of failure they are looking
# at. ctest captures the output of a test it kills, so the last narration line
# survives the kill and says `still scanning -- 400 of 837 ... 65 ms/file`.
#
# The threshold sits above the bridged-and-quiet band's whole run so an ordinary
# WSL checkout stays silent, and far below any plausible ceiling so a contended
# one has narrated several times before anything kills it.
set(FastCachedSccacheScanNarrateAfterSeconds 10)
set(FastCachedSccacheScanProgressEvery 100)

# ---------------------------------------------------------------------------

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR
        "FASTCACHED_SOURCE_DIR is not set. Invoke this script as: cmake "
        "-DFASTCACHED_SOURCE_DIR=<source root> -P ${CMAKE_CURRENT_LIST_FILE}")
endif()

if(NOT IS_DIRECTORY "${FASTCACHED_SOURCE_DIR}")
    message(FATAL_ERROR "'${FASTCACHED_SOURCE_DIR}' is not a directory. Is it the source root?")
endif()

# The two narration settings are overridable so the selftest can drive the slow
# path in milliseconds instead of waiting for a filesystem to be slow. Nothing
# here can change the VERDICT -- it is a property of the source tree alone -- so
# an override can only make this check noisier or quieter about itself.
#
# There is deliberately no default for the budget, which is why it starts empty
# rather than appearing beside the two settings above. It is the number ctest
# enforces as this test's TIMEOUT, and a default here would be a second copy of
# it that drifts -- the shape of the defect this whole section exists to close.
# Run standalone with no budget, the cost is reported with no headroom line,
# which is honest: nothing was enforcing one.
set(scanBudgetSeconds "")

#   <-D name>|<the variable it sets>|<lowest accepted value>|<what it is, for the refusal>
#
# A table because all three are validated identically and getting one wrong is
# silent: **CMake's numeric comparisons are FALSE for a non-number**, so a value
# like `60s` passes every `LESS`/`LESS_EQUAL` guard written against it and
# reaches `math()` hundreds of lines later, where it surfaces as an arithmetic
# error naming a line number instead of naming the flag. Under this test's
# `FAIL_REGULAR_EXPRESSION` that reddens the check with a message about
# expression parsing. So the shape is asserted before the value is used, once,
# for every knob -- and a fourth knob is a row rather than a fourth spelling.
#
# No field may contain a '|' or a ';'.
set(FastCachedSccacheScanKnobs
    "FASTCACHED_SCAN_NARRATE_AFTER|FastCachedSccacheScanNarrateAfterSeconds|0|the number of seconds a run must already have taken before it starts narrating progress"
    "FASTCACHED_SCAN_PROGRESS_EVERY|FastCachedSccacheScanProgressEvery|1|the number of files between progress checkpoints. It is the modulus the checkpoint divides by, so 0 is a division by zero rather than a way to turn narration off -- raise FASTCACHED_SCAN_NARRATE_AFTER for that"
    "FASTCACHED_SCAN_BUDGET_SECONDS|scanBudgetSeconds|1|the wall-clock budget headroom is reported against. Omit it entirely to run with no budget and no headroom line, which is how it is switched off"
)
foreach(row IN LISTS FastCachedSccacheScanKnobs)
    string(REPLACE "|" ";" knobFields "${row}")
    list(LENGTH knobFields knobFieldCount)
    if(NOT knobFieldCount EQUAL 4)
        message(FATAL_ERROR
            "FastCachedSccacheScanKnobs row split into ${knobFieldCount} field(s) where 4 are "
            "wanted -- a '|' or a ';' has got into a field: ${row}")
    endif()
    list(GET knobFields 0 knobName)
    list(GET knobFields 1 knobVariable)
    list(GET knobFields 2 knobMinimum)
    list(GET knobFields 3 knobMeaning)

    if(NOT DEFINED ${knobName})
        continue()
    endif()
    set(knobValue "${${knobName}}")

    if(NOT knobValue MATCHES "^[0-9]+$")
        message(FATAL_ERROR
            "${knobName} is '${knobValue}', which is not a whole number. It is ${knobMeaning}.")
    endif()
    if(knobValue LESS ${knobMinimum})
        message(FATAL_ERROR
            "${knobName} is ${knobValue} and its lowest accepted value is ${knobMinimum}. It is "
            "${knobMeaning}.")
    endif()
    set(${knobVariable} "${knobValue}")
endforeach()

# `string(TIMESTAMP)` is the only clock a `cmake -P` script has. Script mode has
# no high-resolution timer, and spawning a subprocess to borrow one would cost
# more on the filesystem this exists to characterise than the thing it measures.
#
# Whole seconds is coarse against a 4 s run and ample against the question
# actually being asked, which is which BAND the filesystem is in: the native and
# contended bands are two orders of magnitude apart, and no rounding closes that.
#
# @param outVar Set to the current wall time in whole seconds since the epoch.
function(fastcached_wall_seconds outVar)
    string(TIMESTAMP nowSeconds "%s")
    set(${outVar} "${nowSeconds}" PARENT_SCOPE)
endfunction()

# CMake honours SOURCE_DATE_EPOCH by returning it from EVERY `string(TIMESTAMP)`
# call, so with that variable set in the environment every interval this script
# measures is exactly zero -- and a zero interval is indistinguishable from an
# instant run. An instrument that cannot report its own blindness is worse than
# no instrument, so the freeze is detected once, here, and carried as its own
# outcome. It is never allowed to present as speed. Four states, not two:
# .agent/rules/metrics-and-observability.md.
set(clockFrozen FALSE)
if(DEFINED ENV{SOURCE_DATE_EPOCH})
    set(clockFrozen TRUE)
endif()

fastcached_wall_seconds(runStartSeconds)

# Split one '|'-separated row into the variables named in ARGN, the last of which
# takes whatever remains -- so only the final field may contain a '|', which is
# what lets a reason be written in ordinary prose.
#
# Named `fastcached_row_fields` rather than the `fastcached_split_rows` that
# check-net-boundary.cmake defines: that one is a different contract (a whole
# table, two fixed outputs), and two functions answering to one name with
# different signatures is a trap for whoever consolidates them. Consolidating the
# five row-splitters across scripts/ is worth doing and is not this change.
#
# @param row The '|'-separated row.
# @param ARGN Output variable names, in field order.
function(fastcached_row_fields row)
    list(LENGTH ARGN fieldCount)
    math(EXPR lastField "${fieldCount} - 1")
    set(rest "${row}")
    foreach(field RANGE 0 ${lastField})
        list(GET ARGN ${field} outVar)
        if(field EQUAL lastField)
            set(value "${rest}")
        else()
            string(FIND "${rest}" "|" separator)
            if(separator EQUAL -1)
                message(FATAL_ERROR "Malformed row (wanted ${fieldCount} '|'-separated fields): ${row}")
            endif()
            string(SUBSTRING "${rest}" 0 ${separator} value)
            math(EXPR restStart "${separator} + 1")
            string(SUBSTRING "${rest}" ${restStart} -1 rest)
        endif()
        set(${outVar} "${value}" PARENT_SCOPE)
    endforeach()
endfunction()

# Split file content into a list of lines, one element per line.
#
# `file(STRINGS)` cannot be used: it returns a CMake list, so a line containing a
# ';' becomes several elements and every line number after it is wrong. Splitting
# by hand meets the same hazard from the other side, and ESCAPING does not survive
# it -- CMake's list syntax reserves four characters, and each was measured
# breaking this scan on a real file in this tree:
#
#   ';'       the separator itself.
#   '\'       its escape -- and CMake reads any ';' preceded by a backslash as
#             escaped without counting the backslashes first, so a line ending in
#             one (every shell continuation in this repository's READMEs) eats the
#             separator after it. README.md came out 365 lines where it has 368.
#   '[' ']'   grouping: a ';' between them is not a separator. One stray '`]`' in
#             a comment swallowed 451 lines into a single element -- the dangerous
#             direction, because a merged element does not shrink a caveat window,
#             it WIDENS it to whatever it swallowed.
#
# All four are replaced by a space rather than escaped. Nothing is lost: no marker
# and no caveat spelling contains any of them, and no line's text is ever printed
# -- only its number. Four calls rather than a table because a CMake list cannot
# hold a bare ';' or '\' to iterate over in the first place.
#
# @param content File content.
# @param linesOut Set to the content's lines, in order.
function(fastcached_split_lines content linesOut)
    string(REPLACE "\\" " " content "${content}")
    string(REPLACE ";" " " content "${content}")
    string(REPLACE "[" " " content "${content}")
    string(REPLACE "]" " " content "${content}")
    string(REGEX REPLACE "\r?\n" ";" lines "${content}")
    set(${linesOut} "${lines}" PARENT_SCOPE)
endfunction()

# Every cost band row parses, checked HERE rather than where a row is selected.
#
# The selection loop stops at the FIRST matching row, so a malformed row after it
# is never visited -- and the rows most likely to be malformed are the last ones,
# because that is where a new band goes. The contended row was written with a ';'
# in it and the defect was exactly that shape: CMake split the element in two,
# the loop matched and broke on the truncated half, and the only symptom was a
# conditions sentence that stopped mid-way. Nothing failed, and nothing could
# have. The rule was already written down for FastCachedSccacheBackendMarkers
# 200 lines above and did not carry itself to a new table, which is why this is a
# check rather than a second comment.
#
# The last row must also be the catch-all, or a per-file cost above every listed
# ceiling matches nothing and renders as `the '' band` -- an empty string in the
# shape of an answer.
#
# Every clause here is a way to get a WRONG BAND rather than no band, which is
# the worse direction: CMake's numeric comparisons are FALSE for anything
# non-numeric, so a ceiling typed `2O` instead of `20` never matches, and the run
# is silently reported under the NEXT row's name and conditions. A row inserted
# out of order does the same, because the loop takes the first match. Neither
# fails, and both hand the reader a specific, confident, wrong answer.
set(bandRowIndex 0)
set(previousCeiling "")
list(LENGTH FastCachedSccacheScanCostBands bandRowCount)
foreach(row IN LISTS FastCachedSccacheScanCostBands)
    math(EXPR bandRowIndex "${bandRowIndex} + 1")
    set(bandWhere "FastCachedSccacheScanCostBands row ${bandRowIndex} of ${bandRowCount}")

    string(FIND "${row}" "|" firstSeparator)
    if(firstSeparator EQUAL -1)
        message(FATAL_ERROR
            "${bandWhere} contains no '|' at all, so it is a fragment rather than a row: '${row}'. "
            "A ';' anywhere in a row splits the CMake list in two and truncates it silently. No "
            "row may contain one.")
    endif()
    fastcached_row_fields("${row}" bandCeiling bandRowName bandRowConditions)

    if(bandRowIndex EQUAL bandRowCount)
        if(NOT bandCeiling STREQUAL "unbounded")
            message(FATAL_ERROR
                "The last ${bandWhere} has ceiling '${bandCeiling}' where it must be the word "
                "`unbounded`. That row is the catch-all: without it a per-file cost above every "
                "listed ceiling matches no band and is reported as an empty name.")
        endif()
    else()
        if(NOT bandCeiling MATCHES "^[0-9]+$")
            message(FATAL_ERROR
                "${bandWhere} has ceiling '${bandCeiling}', which is not a whole number. CMake's "
                "numeric comparisons are FALSE for a non-number, so this row would never match and "
                "every run belonging to it would be reported under the next row's conditions -- a "
                "confident wrong answer rather than a missing one. Only the last row may be "
                "`unbounded`.")
        endif()
        # Nested rather than `... AND NOT bandCeiling GREATER ${previousCeiling}`:
        # on the first row `previousCeiling` is empty, and `if()` expands every
        # argument BEFORE it evaluates any of them, so the guard's own AND left
        # `GREATER` with nothing after it and CMake refused the whole condition.
        # There is no short-circuit to rely on at that level.
        if(NOT previousCeiling STREQUAL "")
            if(NOT bandCeiling GREATER ${previousCeiling})
                message(FATAL_ERROR
                    "${bandWhere} has ceiling ${bandCeiling} after ${previousCeiling}. Ceilings "
                    "must ascend strictly: selection takes the FIRST row whose ceiling is not "
                    "exceeded, so a row placed after a wider one is unreachable and its runs are "
                    "labelled with the wider row's name and conditions.")
            endif()
        endif()
        set(previousCeiling "${bandCeiling}")
    endif()
endforeach()

# A bound this check can reach without ever having narrated is the bare timeout
# #479 is about. The narration is what makes a kill say WHICH kind of failure it
# was, so a budget must leave room for narration to have happened first -- twice
# the threshold, so at least one whole narration window fits inside it. Stated in
# two comments before this; a relationship the design rests on is worth more than
# prose in the file that depends on it.
if(NOT scanBudgetSeconds STREQUAL "")
    math(EXPR narrationRoom "${FastCachedSccacheScanNarrateAfterSeconds} * 2")
    if(scanBudgetSeconds LESS ${narrationRoom})
        message(FATAL_ERROR
            "FASTCACHED_SCAN_BUDGET_SECONDS is ${scanBudgetSeconds}s and narration does not start "
            "until ${FastCachedSccacheScanNarrateAfterSeconds}s, so a run killed at that budget "
            "could be killed having explained nothing -- which is the bare timeout this check's "
            "narration exists to replace. The budget must be at least twice the narration "
            "threshold. Lower FastCachedSccacheScanNarrateAfterSeconds or raise the budget in "
            "src/tests/CMakeLists.txt, and keep them moving together.")
    endif()
endif()

set(markerLiterals "")
foreach(row IN LISTS FastCachedSccacheBackendMarkers)
    fastcached_row_fields("${row}" markerLiteral markerReason)
    list(APPEND markerLiterals "${markerLiteral}")
endforeach()

set(exemptPaths "")
foreach(row IN LISTS FastCachedSccacheExemptPaths)
    fastcached_row_fields("${row}" exemptPath exemptReason)
    list(APPEND exemptPaths "${exemptPath}")
endforeach()

set(snippetPath "${FASTCACHED_SOURCE_DIR}/${FastCachedSccacheSnippet}")
get_filename_component(snippetName "${FastCachedSccacheSnippet}" NAME)

# ---------------------------------------------------------------------------
# An exemption for a path that no longer exists is one that has stopped being
# checked, and it would go on excusing whatever takes that name next. Its reason
# is printed with it: whether to move the row or delete it depends on why it was
# written, and this is the last moment anyone will look.
set(staleExemptions "")
foreach(row IN LISTS FastCachedSccacheExemptPaths)
    fastcached_row_fields("${row}" exemptPath exemptReason)
    if(NOT EXISTS "${FASTCACHED_SOURCE_DIR}/${exemptPath}")
        list(APPEND staleExemptions "  ${exemptPath}\n      is exempted but does not exist. It was exempted because: ${exemptReason}")
    endif()
endforeach()

if(NOT EXISTS "${snippetPath}")
    set(missingSnippet
        "  ${FastCachedSccacheSnippet}\n      is the canonical caveat every documentation page includes, and it is not there")
else()
    set(missingSnippet "")
endif()

# ---------------------------------------------------------------------------
# Collect the files to search, and remember which of them may be judged.
set(scanFiles "")
# Turn a list of shell globs into one anchored regex, so a directory can be walked
# ONCE and the results partitioned in memory.
#
# `file(GLOB_RECURSE var a b c)` traverses the tree once PER PATTERN, and this check
# passed it ~19 of them per root, twice. Measured on DrvFs: one traversal of `src/`
# costs 2.09 s, and 38 of them is the 78.7 s this check took -- arithmetic that
# matches the observation almost exactly.
# @param globs The shell globs, each like `*.md`.
# @param outVar Set to an anchored alternation regex.
function(fastcached_globs_to_regex globs outVar)
    set(parts "")
    foreach(glob IN LISTS globs)
        string(REPLACE "." "PLACEHOLDERDOT" one "${glob}")
        string(REPLACE "*" ".*" one "${one}")
        string(REPLACE "PLACEHOLDERDOT" "\\." one "${one}")
        list(APPEND parts "${one}")
    endforeach()
    string(REPLACE ";" "|" joined "${parts}")
    set(${outVar} "(${joined})$" PARENT_SCOPE)
endfunction()

fastcached_globs_to_regex("${FastCachedSccacheScanGlobs}" scanRegex)
fastcached_globs_to_regex("${FastCachedSccacheJudgedGlobs}" judgedRegex)

set(judgedFiles "")
set(missingRoots "")
# The walk is timed separately because the split between walking and reading is
# what says whether a slow run is traversal cost or read cost -- it was 43% of
# the run when this was measured. It is DECORATIVE: no decision reads it, the
# band and the headroom are both taken from the whole-run figure, and at
# whole-second resolution a ~2 s walk prints as 1 s or 2 s. Reported as evidence,
# never quoted as a quantity.
fastcached_wall_seconds(walkStartSeconds)
foreach(row IN LISTS FastCachedSccacheScanRoots)
    fastcached_row_fields("${row}" scanRoot scanRootReason)
    set(rootPath "${FASTCACHED_SOURCE_DIR}/${scanRoot}")
    if(IS_DIRECTORY "${rootPath}")
        # ONE traversal per root, partitioned afterwards. See
        # `fastcached_globs_to_regex` for the arithmetic this replaces.
        file(GLOB_RECURSE rootAll LIST_DIRECTORIES false "${rootPath}/*")

        set(rootFiles ${rootAll})
        list(FILTER rootFiles INCLUDE REGEX "${scanRegex}")
        list(APPEND scanFiles ${rootFiles})

        set(rootJudged ${rootAll})
        list(FILTER rootJudged INCLUDE REGEX "${judgedRegex}")
        list(APPEND judgedFiles ${rootJudged})
    elseif(EXISTS "${rootPath}")
        list(APPEND scanFiles "${rootPath}")
        foreach(glob IN LISTS FastCachedSccacheJudgedGlobs)
            string(REPLACE "*" ".*" globPattern "${glob}")
            if(rootPath MATCHES "${globPattern}$")
                list(APPEND judgedFiles "${rootPath}")
                break()
            endif()
        endforeach()
    else()
        # A renamed or mistyped root would otherwise take a whole surface out of
        # this check's view while it went on reporting success.
        list(APPEND missingRoots
            "  ${scanRoot}\n      is named in the scan table but is neither a file nor a directory. It is scanned because: ${scanRootReason}")
    endif()

    # The walk narrates per ROOT, which is as fine-grained as it can be: a
    # `file(GLOB_RECURSE)` is one uninterruptible call, so the blind window is
    # one root's traversal and cannot be made smaller without reintroducing the
    # per-pattern traversals #502 removed. Without this the whole walk -- 43% of
    # the run when measured -- was a window in which a kill carried nothing,
    # which is the bare timeout this narration exists to replace, surviving
    # inside the fix for it.
    if(NOT clockFrozen)
        fastcached_wall_seconds(rootWalkSeconds)
        math(EXPR walkElapsed "${rootWalkSeconds} - ${runStartSeconds}")
        if(walkElapsed GREATER_EQUAL ${FastCachedSccacheScanNarrateAfterSeconds})
            list(LENGTH scanFiles walkedSoFar)
            message(
                "sccache backend caveat: still walking -- ${walkedSoFar} candidate(s) after "
                "${walkElapsed}s, having just finished '${scanRoot}'. Nothing has been READ yet, so "
                "this is directory traversal cost. This check cannot wedge -- it walks, reads and "
                "exits -- so a run this slow is the filesystem.")
        endif()
    endif()
endforeach()
list(REMOVE_DUPLICATES scanFiles)
list(SORT scanFiles)
fastcached_wall_seconds(walkEndSeconds)
math(EXPR walkSeconds "${walkEndSeconds} - ${walkStartSeconds}")
list(LENGTH scanFiles scanCount)

# ---------------------------------------------------------------------------
# Every marker occurrence in a judged file must be followed, inside the window, by
# the canonical include or by every caveat element.
set(violations "")
set(unjudgeable "")
set(recommendingFiles "")
set(markerHits 0)
set(filesVisited 0)
set(filesRead 0)
set(bytesRead 0)
set(narrated FALSE)

foreach(scanFile IN LISTS scanFiles)
    math(EXPR filesVisited "${filesVisited} + 1")

    # Narrate, but only once the run is ALREADY slow. See
    # FastCachedSccacheScanNarrateAfterSeconds for why a fast run stays silent
    # and why this line is the whole answer to a `ctest` TIMEOUT on this check.
    #
    # `%` rather than a countdown so the checkpoint is a property of the file
    # index rather than of a second counter that could fall out of step with it.
    math(EXPR sinceCheckpoint "${filesVisited} % ${FastCachedSccacheScanProgressEvery}")
    if(sinceCheckpoint EQUAL 0 AND NOT clockFrozen)
        fastcached_wall_seconds(checkpointSeconds)
        math(EXPR elapsedSeconds "${checkpointSeconds} - ${runStartSeconds}")
        if(elapsedSeconds GREATER_EQUAL ${FastCachedSccacheScanNarrateAfterSeconds})
            math(EXPR checkpointMsPerFile "${elapsedSeconds} * 1000 / ${filesVisited}")
            math(EXPR projectedSeconds "${elapsedSeconds} * ${scanCount} / ${filesVisited}")
            set(narrated TRUE)
            # A plain message(), never message(WARNING): src/tests/CMakeLists.txt
            # sets this test's FAIL_REGULAR_EXPRESSION to "CMake Error|CMake
            # Warning", so the obvious way to say "this filesystem is slow" is
            # the one output that turns a passing check red. Whoever next wants
            # to make this louder will reach for WARNING first; this is why not.
            message(
                "sccache backend caveat: still scanning -- ${filesVisited} of ${scanCount} file(s) "
                "after ${elapsedSeconds}s, about ${checkpointMsPerFile} ms/file, so the whole scan "
                "needs roughly ${projectedSeconds}s at this rate. This check does no compute worth "
                "measuring and cannot wedge -- it reads N files and exits -- so a run this slow is "
                "the filesystem, not a hang. See FastCachedSccacheScanCostBands in "
                "${CMAKE_CURRENT_LIST_FILE} for which band that per-file cost lands in.")
        endif()
    endif()

    file(RELATIVE_PATH relativeFile "${FASTCACHED_SOURCE_DIR}" "${scanFile}")

    list(FIND exemptPaths "${relativeFile}" exemptPosition)
    if(NOT exemptPosition EQUAL -1)
        continue()
    endif()

    # Cheap whole-file test first: splitting a file into lines is the expensive
    # part, and almost none of them name a marker at all. Measured on a native
    # filesystem, this is what keeps that part at a fifth of a second rather than
    # over a second -- a figure about CMake's list handling, not about the I/O,
    # which is what dominates the run everywhere else and is reported separately
    # at the end of this file.
    file(READ "${scanFile}" content)
    math(EXPR filesRead "${filesRead} + 1")

    # `string(LENGTH)` over a string ALREADY IN MEMORY, and never `file(SIZE)`:
    # that is a stat across the very bridge this check exists to characterise,
    # so it would trade a few microseconds of counting for another millisecond
    # of filesystem wait per file -- roughly 300x the wrong way, in the one loop
    # where that matters. Measured A/B against the pre-instrumentation script on
    # WSL2 9p, three alternating runs each: 3.79 s mean before, 3.88 s after,
    # inside a 0.57 s run-to-run spread. The whole instrument costs less than one
    # quantization step of the ms/file figure it prints.
    string(LENGTH "${content}" contentBytes)
    math(EXPR bytesRead "${bytesRead} + ${contentBytes}")
    set(namesAMarker FALSE)
    foreach(markerLiteral IN LISTS markerLiterals)
        string(FIND "${content}" "${markerLiteral}" markerPosition)
        if(NOT markerPosition EQUAL -1)
            set(namesAMarker TRUE)
            break()
        endif()
    endforeach()
    if(NOT namesAMarker)
        continue()
    endif()

    list(APPEND recommendingFiles "${relativeFile}")

    list(FIND judgedFiles "${scanFile}" judgedPosition)
    if(judgedPosition EQUAL -1)
        list(APPEND unjudgeable
            "  ${relativeFile}\n      names a backend variable and is not prose this check can judge, and no exemption row says what owns it instead")
        continue()
    endif()

    fastcached_split_lines("${content}" fileLines)
    list(LENGTH fileLines lineCount)

    # A split that lost a line does not report a missing line -- it reports a
    # WIDER window, because the lines it swallowed are still in the element it
    # merged them into. That direction passes, silently, on a caveat sitting
    # hundreds of lines from the recommendation it is supposed to qualify. So the
    # count is checked against newlines counted before the split, the one number
    # the split cannot influence.
    string(REGEX MATCHALL "\n" newlines "${content}")
    list(LENGTH newlines newlineCount)
    math(EXPR expectedLines "${newlineCount} + 1")
    if(NOT lineCount EQUAL expectedLines)
        message(FATAL_ERROR
            "Splitting ${relativeFile} into lines produced ${lineCount} where the file has "
            "${newlineCount} newline(s), so this check cannot trust its own line numbers -- and "
            "a merged element would silently widen the caveat window to whatever it swallowed. "
            "A character CMake's list syntax reserves has reached the split unneutralised; "
            "fastcached_split_lines in ${CMAKE_CURRENT_LIST_FILE} says which four those are.")
    endif()

    # `foreach(IN LISTS)` with a running counter rather than an indexed
    # `list(GET)`: CMake stores a list as a joined string and re-parses it on
    # every GET, which measured 14x slower over the same files.
    math(EXPR lastLine "${lineCount} - 1")
    set(lineIndex -1)
    foreach(line IN LISTS fileLines)
        math(EXPR lineIndex "${lineIndex} + 1")

        set(lineMarker "")
        foreach(markerLiteral IN LISTS markerLiterals)
            string(FIND "${line}" "${markerLiteral}" markerPosition)
            if(NOT markerPosition EQUAL -1)
                set(lineMarker "${markerLiteral}")
                break()
            endif()
        endforeach()
        if(lineMarker STREQUAL "")
            continue()
        endif()

        math(EXPR markerHits "${markerHits} + 1")

        # The window: this line and the ones after it, clamped to the file.
        math(EXPR windowLast "${lineIndex} + ${FastCachedSccacheCaveatWindow}")
        if(windowLast GREATER ${lastLine})
            set(windowLast ${lastLine})
        endif()
        math(EXPR windowCount "${windowLast} - ${lineIndex} + 1")
        list(SUBLIST fileLines ${lineIndex} ${windowCount} windowLines)

        # Route one: the page includes the canonical caveat. Nothing else is
        # asked of it -- that is what "one wording" means.
        set(satisfied FALSE)
        foreach(windowLine IN LISTS windowLines)
            if(windowLine MATCHES "${FastCachedSccacheSnippetInclude}"
               AND CMAKE_MATCH_1 STREQUAL "${snippetName}"
               AND missingSnippet STREQUAL "")
                set(satisfied TRUE)
                break()
            endif()
        endforeach()

        # Route two: the page states it, because it cannot include anything.
        set(missingElements "")
        if(NOT satisfied)
            list(JOIN windowLines "\n" window)
            foreach(row IN LISTS FastCachedSccacheCaveatElements)
                fastcached_row_fields("${row}" elementName elementSpellings elementReason)
                string(REPLACE " / " ";" spellings "${elementSpellings}")

                set(present FALSE)
                foreach(spelling IN LISTS spellings)
                    string(FIND "${window}" "${spelling}" spellingPosition)
                    if(NOT spellingPosition EQUAL -1)
                        set(present TRUE)
                        break()
                    endif()
                endforeach()
                if(NOT present)
                    list(APPEND missingElements "${elementName} (${elementSpellings})")
                endif()
            endforeach()
            if(missingElements STREQUAL "")
                set(satisfied TRUE)
            endif()
        endif()

        if(NOT satisfied)
            math(EXPR humanLine "${lineIndex} + 1")
            list(JOIN missingElements ", " missingText)
            list(APPEND violations
                "  ${relativeFile}:${humanLine}\n      names ${lineMarker}, and the ${FastCachedSccacheCaveatWindow} lines after it neither include ${snippetName} nor carry: ${missingText}")
        endif()
    endforeach()
endforeach()

# ---------------------------------------------------------------------------
# What this run cost, printed on EVERY run -- green, red, fast or slow -- and
# printed HERE, before the verdict, so a failing run carries it too.
#
# This is what replaces the assertion this check used to be described by in its
# own registration: `~0.2-0.5 s ... still far inside the timeout`. That sentence
# was true where it was written, wrong by an order of magnitude on a bridged
# checkout, and structurally incapable of ever falsifying itself. A margin
# nobody re-measures stops being a margin silently; an observation printed on
# every run cannot. It states the finding and stops -- no remedy is suggested,
# because an instrument cannot know whether the remedy is the thing in dispute.
fastcached_wall_seconds(runEndSeconds)
math(EXPR runSeconds "${runEndSeconds} - ${runStartSeconds}")
math(EXPR bytesReadKiB "${bytesRead} / 1024")

if(clockFrozen)
    # Not a fast run. Its own outcome, never folded into the timings: with
    # SOURCE_DATE_EPOCH set every interval this script can take reads as exactly
    # zero, which would otherwise present as the best possible filesystem.
    message(
        "sccache backend caveat: walked ${scanCount} candidate(s) and read ${filesRead} "
        "(${bytesReadKiB} KiB). Cost NOT MEASURED: SOURCE_DATE_EPOCH is set in the environment "
        "and CMake returns it from every string(TIMESTAMP) call, so every interval this check "
        "can time reads as zero. Unset it to measure. Progress narration is off for the same "
        "reason -- it has no clock to decide on.")
elseif(scanCount EQUAL 0)
    # Nothing to divide by. The vacuity refusal below is what actually acts on
    # this; saying it here keeps the cost line from reporting a per-file figure
    # over no files, which is a number with no subject.
    message("sccache backend caveat: no candidate files under the scanned roots, so there is no per-file cost to report.")
else()
    math(EXPR msPerFile "${runSeconds} * 1000 / ${scanCount}")

    # Which band that lands in. The ceilings ascend and the last row is the
    # catch-all, so this loop always selects -- and if a later edit breaks that
    # invariant it is refused below rather than rendering `the '' band`, which is
    # an instrument quietly reporting nothing in the shape of an answer.
    set(bandName "")
    set(bandConditions "")
    foreach(row IN LISTS FastCachedSccacheScanCostBands)
        fastcached_row_fields("${row}" bandCeiling rowBandName rowBandConditions)
        # Two `if`s rather than one `OR`: the catch-all row's ceiling is a WORD,
        # and handing a word to LESS_EQUAL is a hard error the moment anything
        # evaluates it. Not worth resting on short-circuit order.
        set(inBand FALSE)
        if(bandCeiling STREQUAL "unbounded")
            set(inBand TRUE)
        elseif(msPerFile LESS_EQUAL ${bandCeiling})
            set(inBand TRUE)
        endif()
        if(inBand)
            set(bandName "${rowBandName}")
            set(bandConditions "${rowBandConditions}")
            break()
        endif()
    endforeach()

    if(bandName STREQUAL "")
        message(FATAL_ERROR
            "${msPerFile} ms/file matched no row of FastCachedSccacheScanCostBands, so this check "
            "cannot say which of its known conditions it is running under. The last row's ceiling "
            "must be the word `unbounded` -- it is the catch-all, and without it a cost above every "
            "listed ceiling renders as an empty band name, which reads like an answer and is not "
            "one. The table is in ${CMAKE_CURRENT_LIST_FILE}.")
    endif()

    # Headroom, and only against the number ctest actually enforces. Run
    # standalone with no budget there is nothing to report headroom against, and
    # inventing a default here would be the second copy of the bound that this
    # whole section exists to prevent.
    if(scanBudgetSeconds STREQUAL "")
        # One argument, deliberately. `set(var "a" "b")` builds a LIST, and
        # expanding it splices a bare `;` into the sentence -- which is what this
        # line did until it was read back. `message()` concatenates its arguments
        # and `set()` does not, and the two look identical at the call site.
        set(headroom "No budget was given, so no headroom is reported -- nothing was enforcing one. src/tests/CMakeLists.txt passes the number ctest enforces.")
    else()
        math(EXPR budgetUsedPercent "${runSeconds} * 100 / ${scanBudgetSeconds}")
        set(headroom "That is ${budgetUsedPercent}% of the ${scanBudgetSeconds}s budget ctest enforces on this check.")
    endif()

    # A run slow enough to have deserved narration that produced none is a run a
    # kill would have left unexplained -- which is the whole failure #479 is
    # about, reappearing one level up. It happens when the checkpoint interval is
    # wider than the corpus: no ordinary run meets that, a trimmed scan table
    # would, and nothing else would ever say so.
    if(runSeconds GREATER_EQUAL ${FastCachedSccacheScanNarrateAfterSeconds} AND NOT narrated)
        string(APPEND headroom
            " This run passed the ${FastCachedSccacheScanNarrateAfterSeconds}s narration threshold and "
            "narrated nothing, so a timeout here would have carried no explanation: "
            "${FastCachedSccacheScanProgressEvery} file(s) between checkpoints against "
            "${scanCount} candidate(s).")
    endif()

    message(
        "sccache backend caveat: walked ${scanCount} candidate(s) in ${walkSeconds}s and read "
        "${filesRead} of them (${bytesReadKiB} KiB); ${runSeconds}s wall in total, about "
        "${msPerFile} ms/file. That is the '${bandName}' band: ${bandConditions} ${headroom}")
endif()

# ---------------------------------------------------------------------------
# A check that scanned nothing passes, which is the one failure mode a rule like
# this must not be allowed to have. sccache renaming its variables, or the scan
# roots being restructured, would both land here.
set(vacuous "")
if(markerHits EQUAL 0)
    set(markerTable "")
    foreach(row IN LISTS FastCachedSccacheBackendMarkers)
        fastcached_row_fields("${row}" markerLiteral markerReason)
        string(APPEND markerTable "  ${markerLiteral}\n      ${markerReason}\n")
    endforeach()
    string(APPEND vacuous
        "  Not one of these appears anywhere under the scanned roots:\n\n${markerTable}\n"
        "  Either the recommendation is gone -- in which case delete this check -- or the\n"
        "  marker table has gone stale and this check has been passing by scanning nothing.")
endif()

# ---------------------------------------------------------------------------
# One row per class of failure, in the order they are worth reading: the ones
# that say this check has stopped working first, the findings themselves last.
#
#   <variable holding the report lines>|<heading>
set(sccacheCaveatReportSections
    "vacuous|This check has stopped checking anything"
    "missingRoots|The scan table names somewhere that is not there"
    "staleExemptions|Exemption(s) name a path that no longer exists"
    "missingSnippet|The canonical caveat is missing"
    "unjudgeable|Recommendation(s) this check is not the right judge of"
    "violations|Recommendation(s) with no caveat after them"
)

set(report "")
foreach(row IN LISTS sccacheCaveatReportSections)
    fastcached_row_fields("${row}" sectionVariable sectionHeading)
    if(NOT "${${sectionVariable}}" STREQUAL "")
        list(JOIN ${sectionVariable} "\n" sectionBody)
        string(APPEND report "${sectionHeading}:\n${sectionBody}\n")
    endif()
endforeach()

if(NOT report STREQUAL "")
    set(rulebook "")
    foreach(row IN LISTS FastCachedSccacheCaveatElements)
        fastcached_row_fields("${row}" elementName elementSpellings elementReason)
        string(APPEND rulebook "  ${elementName} -- any of: ${elementSpellings}\n      ${elementReason}\n")
    endforeach()

    message(FATAL_ERROR
        "${report}"
        "\nPointing sccache at fastcached is ONE cache shared by every checkout and every "
        "machine pointed at it, which under MSVC and clang-cl is the maximal form of the "
        "hazard in .agent/rules/build-and-toolchain.md: sccache hashes `/EP` output, which "
        "carries no paths, and replays a hit's `/showIncludes` with the absolute paths of "
        "the checkout that stored it. Anywhere this project recommends that configuration "
        "it says so in the same breath, within ${FastCachedSccacheCaveatWindow} lines AFTER "
        "the variable, one of two ways.\n\n"
        "  1. Include the canonical caveat -- `--8<-- \"${snippetName}\"`. Preferred, and "
        "all a MkDocs page ever needs.\n"
        "  2. State it, for a surface that can include nothing. README.md is rendered by "
        "GitHub, so it restates it, and must then carry:\n\n"
        "${rulebook}\n"
        "Wording is free; those three facts are not. ${FastCachedSccacheSnippet} is the one "
        "wording -- reword it there and every page that includes it follows. Exempt a file "
        "only when it names the variable without recommending anything, or when something "
        "else is a better judge of it than a text scan, and say which in its row.\n"
        "The tables live in ${CMAKE_CURRENT_LIST_FILE}.")
endif()

list(LENGTH recommendingFiles recommendingCount)
message("sccache backend caveat: ${markerHits} recommendation(s) across ${recommendingCount} file(s) "
        "each carry the MSVC/clang-cl caveat within ${FastCachedSccacheCaveatWindow} lines")
