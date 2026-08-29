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
# Exit codes: 0 = every recommendation carries its caveat. 1 = at least one does not.

# A `cmake -P` script has no `cmake_minimum_required`, so every policy is unset
# and takes its OLD behaviour. Under CMP0007 OLD, `list()` DISCARDS empty
# elements -- which is every blank line in every file scanned here, so a line
# number is off by however many blank lines precede it. It reproduced exactly
# that way: identical on CMake 4.3 (where the policy defaults NEW) and wrong from
# the first blank line on CMake 3.28, which is the version this project supports.
# Guarded by `if(POLICY ...)` so a CMake that has retired the policy still runs
# this rather than failing on the line that asks for it.
if(POLICY CMP0007)
    cmake_policy(SET CMP0007 NEW)
endif()

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
    "src/FastCache/Config/CliParser.cpp|The daemon's `--help`, whose sccache examples DO carry the caveat -- as a row of UsageFooters. It is owned by the `--help qualifies its sccache examples with the MSVC caveat` case in CliParser_test.cpp, which asserts the same three elements against the RENDERED usage string and additionally that the caveat reads after the examples. See the note on FastCachedSccacheJudgedGlobs above for why a source scan is the weaker of the two."
    "docs/snippets/sccache-backend-caveat.md|The caveat itself, which names the variables in the header telling an author what this check requires. Judging it would have it satisfy itself, which is a check that cannot fail."
    ".github/workflows/build.yml|Names the variable in a comment explaining why the Windows sccache assertion must be read BEFORE ctest -- the `sccache-smoke-*` tests restart the sccache server with it, zeroing the statistics -- and the smoke jobs themselves invoke the harness that sets it. That is the workflow driving the daemon, never advising anyone to build that way, which is the same ground as the sccache-smoke rows above. It is also YAML, where a caveat could only be a comment: this check cannot tell a comment stating the caveat from a comment merely mentioning the variable, so a green result here would prove nothing about what the file says. Owned by .agent/rules/build-and-toolchain.md, which states the caveat in full at the passage that comment refers to, and which this check does judge."
)

# ---------------------------------------------------------------------------

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR
        "FASTCACHED_SOURCE_DIR is not set. Invoke this script as: cmake "
        "-DFASTCACHED_SOURCE_DIR=<source root> -P ${CMAKE_CURRENT_LIST_FILE}")
endif()

if(NOT IS_DIRECTORY "${FASTCACHED_SOURCE_DIR}")
    message(FATAL_ERROR "'${FASTCACHED_SOURCE_DIR}' is not a directory. Is it the source root?")
endif()

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
set(judgedFiles "")
set(missingRoots "")
foreach(row IN LISTS FastCachedSccacheScanRoots)
    fastcached_row_fields("${row}" scanRoot scanRootReason)
    set(rootPath "${FASTCACHED_SOURCE_DIR}/${scanRoot}")
    if(IS_DIRECTORY "${rootPath}")
        set(rootGlobs "")
        foreach(glob IN LISTS FastCachedSccacheScanGlobs)
            list(APPEND rootGlobs "${rootPath}/${glob}")
        endforeach()
        file(GLOB_RECURSE rootFiles LIST_DIRECTORIES false ${rootGlobs})
        list(APPEND scanFiles ${rootFiles})

        set(judgedGlobs "")
        foreach(glob IN LISTS FastCachedSccacheJudgedGlobs)
            list(APPEND judgedGlobs "${rootPath}/${glob}")
        endforeach()
        file(GLOB_RECURSE rootJudged LIST_DIRECTORIES false ${judgedGlobs})
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
endforeach()
list(REMOVE_DUPLICATES scanFiles)
list(SORT scanFiles)

# ---------------------------------------------------------------------------
# Every marker occurrence in a judged file must be followed, inside the window, by
# the canonical include or by every caveat element.
set(violations "")
set(unjudgeable "")
set(recommendingFiles "")
set(markerHits 0)

foreach(scanFile IN LISTS scanFiles)
    file(RELATIVE_PATH relativeFile "${FASTCACHED_SOURCE_DIR}" "${scanFile}")

    list(FIND exemptPaths "${relativeFile}" exemptPosition)
    if(NOT exemptPosition EQUAL -1)
        continue()
    endif()

    # Cheap whole-file test first: splitting a file into lines is the expensive
    # part, and almost none of them name a marker at all. Measured, this is what
    # keeps the check at a fifth of a second rather than over a second.
    file(READ "${scanFile}" content)
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
