# SPDX-License-Identifier: Apache-2.0
#
# The markup escaping convention has ONE home, and nothing else spells its entities.
#
# `&amp;` `&lt;` `&gt;` `&quot;` `&#39;` are what an XML-family document writes instead
# of the five bytes it cannot carry. They belong in `Core/Markup.hpp`'s table and
# nowhere else: a fleet page, a launcher report and a launchd plist all reach that one
# escaper (#1359).
#
# ## Why this check and not a walk over the shared function
#
# This is the part worth reading, because the obvious guard here is vacuous.
#
# #1334 shared only a TABLE, so each consumer kept its own walk and could drift from
# the rows — which is what a test walking the table catches. #1359 shared the
# FUNCTION, so that drift cannot happen: every consumer calls `EscapeMarkup`, and a
# test over `EscapeMarkup` would keep passing no matter how many new escapers appeared
# beside it, because the shared function is correct by construction.
#
# The failure mode that survives consolidation is somebody writing a **new** escaper
# and re-pointing call sites at it. Nothing about the shared function can see that. The
# instrument that can is the census that FOUND the three sites in the first place: look
# for the emitted ARTEFACT — the entity spellings — rather than for the implementation
# shape. A table-shaped census is blind to a `switch`-shaped duplicate by construction,
# and that asymmetry is exactly how the original sweep first mischaracterised this as a
# trivial difference of tables.
#
# So this is the acceptance clause of #1359 rather than decoration: *change one row at
# one site and something goes red naming that site.*
#
# ## `&apos;` is banned outright, and that is the sharp end
#
# The consolidation retired `&apos;` in favour of `&#39;`. Both are valid references
# for U+0027, nothing reads the spelling back, and one table with no per-consumer
# column was chosen over two tables. A reappearing `&apos;` is therefore not a style
# question: it is the drift this check exists to refuse, arriving in the exact shape it
# arrived in last time — a second escaper, in a target that did not want to reach the
# first one.
#
# ## What may spell them, all three answers derived rather than listed where possible
#
# 1. **The home**, found by searching for the table's definition rather than by
#    carrying a path — so the check cannot end up excusing a file that has moved.
# 2. **A comment**, stripped before matching. `Distributed/FleetView.cpp` mentions
#    `&amp;` in a sentence explaining the escaping and calls the shared helper 28
#    times; a check that refused the documentation of its own rule gets deleted rather
#    than obeyed.
# 3. **A `*_test.cpp`**, by suffix. A test of an escaper must spell what the escaper
#    emits, and there is no other way to assert it. THE LIMIT, stated rather than
#    discovered later: a test could hand-roll an escaper and this would not see it.
#    That is deliberate and it is sound for the hazard — a test escaper ships to
#    nobody, and the subject here is a second escaper reaching a DOCUMENT.
#
# Everything else needs a row in `ExemptionTable` with a reason, and a row that has
# stopped describing a real occurrence is refused as STALE — an exemption nobody has
# re-read is how a list stops describing the tree it governs.
#
# Runs as `cmake -P`: it reads files, compares strings and reports. See
# `check-script-check-signals.cmake` for why such a check reports failure through its
# OUTPUT rather than an exit code.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -P scripts/check-markup-entities.cmake
#
# Exit codes: 0 always. The verdict is the presence of `CMake Error` in the output.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()

# The artefact, spelled once. These are what is matched, what the matched-nothing
# guards count, and what the failure text names.
# The BODIES, not the entities: `&lt;` and `&gt;` each contain a `;`, which is
# CMake's list separator, so a list of the full spellings is silently ragged and
# `string(FIND "${line}" "")` then matches at position 0 on every line. Rebuilt as
# `&<body>;` at the point of use. Same shape as the bracket hazard the line walk below
# avoids -- one separator over.
set(entityBodies "amp" "lt" "gt" "quot" "#39")

# Retired by #1359 and refused by NAME, so its return is reported as the drift it is
# rather than as an ordinary duplicate.
set(retiredBody "apos")
set(retiredEntity "&${retiredBody};")

# The home's DEFINITION, not a path. The leading newline anchors it to a line start, so
# a mention inside a comment or a nested namespace does not claim to be the table.
# Leading whitespace allowed: the table sits inside `Detail`, so it is indented. A
# REGEX rather than a `FIND`, because anchoring at column 0 found nothing and
# reported it as "the table was renamed or removed" -- fail-closed, and loudly,
# which is how this was caught here rather than shipped.
set(homeSignature "\n[ \t]*inline constexpr std::array MarkupEscapes")
set(homeShape "a line declaring `inline constexpr std::array MarkupEscapes`")

# Files that may spell the entities without being the home and without being a test.
#
#   <path>|<reason>
#
# One row today. It is a REASON rather than a path list because the question a reader
# has is never "is this file listed" but "why is this allowed", and an opt-out with no
# answer to that is indistinguishable from somebody silencing the check.
set(ExemptionTable
    "src/apps/fastcache-compile-node/AdminEndpoint.cpp|hand-escaped literal prose inside string literals -- it writes an escaped Authorization header as DOCUMENTATION for an operator to read, so the angle brackets are content rather than an escaped value. It calls no escape helper at all, which is why #1359 settled that a consolidation cannot reach it and that it is not a fourth duplicate. (This reason spells no entity itself: they contain CMake's list separator.)"
)

# Walk a file line by line WITHOUT ever building a CMake list of lines.
#
# `;`, `\`, `[` and `]` are all CMake list structure and C++ is full of all four; the
# house splitting idiom merges a line ending in a backslash with the next one, which
# hides the following line and drifts every line number below it. `FIND`/`SUBSTRING` is
# immune to all four by construction. The measurement and the consequences are on
# `fastcached_scan_lines` in `check-istreambuf-iterator.cmake`; consolidating the copies
# is #495 and is not this ticket.
function(fastcached_scan_entities content outHits)
    set(hits "")
    set(rest "${content}")
    set(lineNumber 0)
    set(inBlockComment FALSE)
    while(TRUE)
        string(FIND "${rest}" "\n" newline)
        if(newline EQUAL -1)
            set(line "${rest}")
        else()
            string(SUBSTRING "${rest}" 0 ${newline} line)
        endif()
        string(REGEX REPLACE "\r$" "" line "${line}")
        math(EXPR lineNumber "${lineNumber} + 1")

        set(stripped "${line}")
        set(skipLine FALSE)
        if(inBlockComment)
            if(NOT stripped MATCHES "\\*/")
                set(skipLine TRUE)
            else()
                string(REGEX REPLACE "^.*\\*/" "" stripped "${stripped}")
                set(inBlockComment FALSE)
            endif()
        endif()
        if(NOT skipLine)
            # CMake's regex engine is greedy and has no lazy quantifier, so an inline
            # `/* ... */` strip takes everything between the FIRST `/*` and the LAST
            # `*/` on a line. A use between two block comments on one line is invisible.
            string(REGEX REPLACE "/\\*.*\\*/" " " stripped "${stripped}")

            # Which introducer comes FIRST decides. The order is not a detail: the
            # `/*` test used to run BEFORE `//` was stripped, so a line comment
            # mentioning `/*` opened a block comment no `*/` ever closed, and every
            # remaining line of that file was skipped while this still printed a
            # clean count over lines it never read -- a false green, in the one
            # direction the check exists to refuse.
            #
            # Positional rather than simply stripping `//` first, which MEASURED
            # identical on every input tried: below the inline `/* ... */` strip
            # above, removing `//...` removes any `/*` that followed it too, so the
            # two orderings agree. What position buys is not a different verdict but
            # independence -- it states the rule itself rather than being correct
            # only while the strip above it keeps running first. A reordering is
            # correct by PRECONDITION; this is correct by construction.
            #
            # Still blind to either introducer inside a STRING LITERAL, as every
            # regex-shaped reader here is. That is unchanged by this and is not what
            # was fixed.
            string(FIND "${stripped}" "/*" blockAt)
            string(FIND "${stripped}" "//" lineAt)
            if(NOT blockAt EQUAL -1 AND (lineAt EQUAL -1 OR blockAt LESS lineAt))
                string(SUBSTRING "${stripped}" 0 ${blockAt} stripped)
                set(inBlockComment TRUE)
            elseif(NOT lineAt EQUAL -1)
                string(SUBSTRING "${stripped}" 0 ${lineAt} stripped)
            endif()

            foreach(body IN LISTS entityBodies retiredBody)
                string(FIND "${stripped}" "&${body};" at)
                if(NOT at EQUAL -1)
                    # `=` rather than `;` between the two fields, for the reason the
                    # bodies are stored bare.
                    list(APPEND hits "${lineNumber}=&${body}")
                endif()
            endforeach()
        endif()

        if(newline EQUAL -1)
            break()
        endif()
        math(EXPR skip "${newline} + 1")
        string(LENGTH "${rest}" restLength)
        if(skip GREATER_EQUAL restLength)
            break()
        endif()
        string(SUBSTRING "${rest}" ${skip} -1 rest)
    endwhile()
    set(${outHits} "${hits}" PARENT_SCOPE)
endfunction()

# Which C++ this REPOSITORY owns, asked of git rather than inferred from directory
# names. Same idiom and same fallback as `check-cli-text-cell.cmake`, and it SAYS which
# mode produced the answer: a fixture staging synthetic trees takes the walk while CI
# takes git, and a check that cannot name the mode it used cannot be held to it.
if(NOT GIT_EXECUTABLE)
    find_program(GIT_EXECUTABLE NAMES git)
endif()

set(sourceFiles "")
set(scanSource "")

if(GIT_EXECUTABLE)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${FASTCACHED_SOURCE_DIR}" rev-parse --is-inside-work-tree
        OUTPUT_VARIABLE insideWorkTree RESULT_VARIABLE gitStatus ERROR_VARIABLE gitError
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(gitStatus EQUAL 0 AND insideWorkTree STREQUAL "true")
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${FASTCACHED_SOURCE_DIR}" ls-files
                    -- "src/*.cpp" "src/*.hpp"
            OUTPUT_VARIABLE tracked RESULT_VARIABLE lsStatus OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(lsStatus EQUAL 0 AND NOT tracked STREQUAL "")
            string(REPLACE "\n" ";" sourceFiles "${tracked}")
            set(scanSource "git ls-files")
        endif()
    endif()
endif()

if(NOT sourceFiles)
    set(excludeNames "out" "build" "_deps" ".git" ".cache" ".claude")
    file(GLOB_RECURSE walked RELATIVE "${FASTCACHED_SOURCE_DIR}"
         "${FASTCACHED_SOURCE_DIR}/src/*.cpp" "${FASTCACHED_SOURCE_DIR}/src/*.hpp")
    foreach(candidate IN LISTS walked)
        set(excluded FALSE)
        foreach(name IN LISTS excludeNames)
            if(candidate MATCHES "(^|/)${name}/")
                set(excluded TRUE)
                break()
            endif()
        endforeach()
        if(NOT excluded)
            list(APPEND sourceFiles "${candidate}")
        endif()
    endforeach()
    set(scanSource "directory walk (no git index)")
endif()

list(REMOVE_DUPLICATES sourceFiles)
list(SORT sourceFiles)

if(NOT sourceFiles)
    message("")
    message("  No C++ source was found under src/ in this repository at all.")
    message("")
    message("That is not a clean tree, it is a scan that stopped working -- a moved")
    message("source root, or a FASTCACHED_SOURCE_DIR pointing somewhere else.")
    message(FATAL_ERROR "markup-entities: the scan matched no C++ sources and cannot conclude")
endif()

list(LENGTH sourceFiles fileCount)

# Exemption paths, separated from their reasons once so both are available below.
set(exemptPaths "")
foreach(row IN LISTS ExemptionTable)
    string(FIND "${row}" "|" bar)
    if(bar EQUAL -1)
        message(FATAL_ERROR "markup-entities: exemption row has no reason: [${row}]")
    endif()
    string(SUBSTRING "${row}" 0 ${bar} rowPath)
    list(APPEND exemptPaths "${rowPath}")
endforeach()

# ---------------------------------------------------------------------------
# One pass, one read per file: the home, the hits and the exemption liveness all come
# from the same `file(READ)`.
set(homeFiles "")
set(violations "")
set(retiredHits "")
set(homeEntityCount 0)
set(testEntityCount 0)
set(usedExemptions "")

foreach(relative IN LISTS sourceFiles)
    # `git ls-files` lists the INDEX, so a file deleted from the worktree and not yet
    # staged is still named. Reading it emits a `CMake Error` the registration would
    # score as THIS check failing, for a reason with nothing to do with markup.
    if(NOT EXISTS "${FASTCACHED_SOURCE_DIR}/${relative}")
        continue()
    endif()

    file(READ "${FASTCACHED_SOURCE_DIR}/${relative}" wholeFile)

    set(homeAt -1)
    if(wholeFile MATCHES "${homeSignature}")
        set(homeAt 1)
        list(APPEND homeFiles "${relative}")
    endif()

    # A whole-file test before the line walk: almost nothing in this tree spells an
    # entity, and splitting every source into lines to discover that costs seconds on
    # every platform for nothing.
    set(mentions FALSE)
    foreach(body IN LISTS entityBodies retiredBody)
        string(FIND "${wholeFile}" "&${body};" at)
        if(NOT at EQUAL -1)
            set(mentions TRUE)
            break()
        endif()
    endforeach()
    if(NOT mentions)
        continue()
    endif()

    fastcached_scan_entities("${wholeFile}" hits)
    if(NOT hits)
        continue()
    endif()

    # `&apos;` is refused wherever it is, the home included -- the home spells `&#39;`
    # now, and a table carrying both is the divergence rather than the fix. Tests are
    # the one place it may appear, since a case may assert that it is GONE.
    if(NOT relative MATCHES "_test\\.cpp$")
        foreach(hit IN LISTS hits)
            if(hit MATCHES "^([0-9]+)=(.+)$" AND CMAKE_MATCH_2 STREQUAL "&${retiredBody}")
                list(APPEND retiredHits "${relative}:${CMAKE_MATCH_1}")
            endif()
        endforeach()
    endif()

    if(NOT homeAt EQUAL -1)
        list(LENGTH hits n)
        math(EXPR homeEntityCount "${homeEntityCount} + ${n}")
        continue()
    endif()

    if(relative MATCHES "_test\\.cpp$")
        list(LENGTH hits n)
        math(EXPR testEntityCount "${testEntityCount} + ${n}")
        continue()
    endif()

    list(FIND exemptPaths "${relative}" exemptAt)
    if(NOT exemptAt EQUAL -1)
        list(APPEND usedExemptions "${relative}")
        continue()
    endif()

    foreach(hit IN LISTS hits)
        if(hit MATCHES "^([0-9]+)=(.+)$")
            list(APPEND violations "${relative}:${CMAKE_MATCH_1} spells ${CMAKE_MATCH_2}\;")
        endif()
    endforeach()
endforeach()

# ---------------------------------------------------------------------------
# FAIL CLOSED. The home must exist, exactly once, and must carry the entities -- or the
# scan is looking for something that is no longer there and every clean verdict below
# is vacuous.
list(LENGTH homeFiles homeCount)
if(NOT homeCount EQUAL 1)
    message("")
    message("  Looked for the one markup table -- ${homeShape} -- and found ${homeCount}.")
    foreach(candidate IN LISTS homeFiles)
        message("    ${candidate}")
    endforeach()
    message("")
    message("This check excuses exactly one file, the one that DEFINES the table, and it")
    message("finds it by searching rather than by carrying a path. Zero means the table")
    message("was renamed or removed, so the rule has no subject and this check needs")
    message("rewriting rather than satisfying. More than one means there are two tables,")
    message("which is the state #1359 existed to end.")
    message("")
    message("Enumerated ${fileCount} C++ source(s) via ${scanSource}.")
    message(FATAL_ERROR "markup-entities: expected exactly one markup table, found ${homeCount}")
endif()

list(GET homeFiles 0 homeFile)

if(homeEntityCount EQUAL 0)
    message("")
    message("  ${homeFile} defines the markup table and spells no entity this scan can see.")
    message("")
    message("The home is this scan's positive control: if the spellings are not")
    message("recognised THERE, they will not be recognised anywhere, and a clean result")
    message("over the rest of the tree means nothing. Either the table now writes them")
    message("some other way -- in which case teach this scan that spelling -- or the")
    message("comment stripping above has begun eating code.")
    message("")
    message("Enumerated ${fileCount} C++ source(s) via ${scanSource}.")
    message(FATAL_ERROR "markup-entities: the home spells no entity, so the scan cannot conclude")
endif()

# And something OUTSIDE the home must spell them too, or the comment stripping and the
# test-suffix rule between them have swallowed the whole corpus -- which reads exactly
# like a clean tree. The tests that assert what the escaper emits are that anchor.
if(testEntityCount EQUAL 0)
    message("")
    message("  No `*_test.cpp` spells a markup entity anywhere in this repository.")
    message("")
    message("A test of an escaper must spell what the escaper emits, and several here do.")
    message("Zero means this scan is no longer reading what it thinks it is -- comment")
    message("stripping that has begun eating code, or a suffix rule that now matches")
    message("every file. It is not evidence that nobody hand-rolls an escaper.")
    message("")
    message("Enumerated ${fileCount} C++ source(s) via ${scanSource}.")
    message(FATAL_ERROR "markup-entities: no test spells an entity, so the scan cannot conclude")
endif()

# A STALE exemption is refused. A row nobody has re-read is how a list stops describing
# the tree it governs, and it is the half of an opt-out that rots silently.
set(staleExemptions "")
foreach(path IN LISTS exemptPaths)
    list(FIND usedExemptions "${path}" at)
    if(at EQUAL -1)
        list(APPEND staleExemptions "${path}")
    endif()
endforeach()
if(staleExemptions)
    message("")
    foreach(path IN LISTS staleExemptions)
        message("  ${path}: exempted, and spells no markup entity")
    endforeach()
    message("")
    message("An exemption that no longer describes anything is worse than none: it reads")
    message("as a considered decision while covering nothing, and it will still be here")
    message("excusing whatever moves into that path next. Delete the row.")
    message("")
    message("Enumerated ${fileCount} C++ source(s) via ${scanSource}.")
    message(FATAL_ERROR "markup-entities: ${staleExemptions} is exempted and needs no exemption")
endif()

if(retiredHits)
    list(LENGTH retiredHits retiredCount)
    message("")
    foreach(hit IN LISTS retiredHits)
        message("  ${hit}\;: spells the retired entity")
    endforeach()
    message("")
    message("`${retiredEntity}` was RETIRED by #1359. Three sites implemented the markup")
    message("convention and two already spelled `&#39;`; converging them onto one table")
    message("with no per-consumer column retired the third spelling. Both are valid")
    message("character references for U+0027, so this is not a correctness complaint --")
    message("it is that a second spelling reappearing is the drift this check exists to")
    message("refuse, in the exact shape it took last time.")
    message("")
    message("Call `EscapeMarkup` (Core/Markup.hpp) rather than writing the entity.")
    message("")
    message("Enumerated ${fileCount} C++ source(s) via ${scanSource}.")
    message(FATAL_ERROR "markup-entities: ${retiredCount} site(s) spell the retired `${retiredEntity}`")
endif()

if(violations)
    list(LENGTH violations violationCount)
    message("")
    foreach(violation IN LISTS violations)
        message("  ${violation}")
    endforeach()
    message("")
    message("The markup escaping convention has one home, ${homeFile}, and every consumer")
    message("reaches it: the fleet page and its charts, the launcher's report, and the")
    message("launchd plist writer. Three targets used to spell it separately and the")
    message("third of them disagreed about one row (#1359).")
    message("")
    message("Escape through the shared function:")
    message("")
    message("  out += EscapeMarkup(text);   // FastCache/Core/Markup.hpp")
    message("")
    message("It is header-only and std-only, so even `fastcache-cc` -- which links no")
    message("library at all -- reaches it for no `_fc_cc_core` row and no link.")
    message("")
    message("WHAT THIS DOES NOT SAY: a comment may discuss the entities, a `*_test.cpp`")
    message("may assert them, and text that is DOCUMENTATION rather than an escaped value")
    message("can take a row in this check's ExemptionTable with a reason. Do not delete a")
    message("comment or an assertion to satisfy this.")
    message("")
    message("Enumerated ${fileCount} C++ source(s) via ${scanSource}.")
    message(FATAL_ERROR "markup-entities: ${violationCount} site(s) spell a markup entity outside the one home")
endif()

list(LENGTH usedExemptions exemptionCount)
message(STATUS
    "markup-entities: ${homeEntityCount} entity spelling(s) in ${homeFile}, ${testEntityCount} in tests, "
    "${exemptionCount} exempted site(s), no duplicate escaper across ${fileCount} C++ source(s) via ${scanSource}")
