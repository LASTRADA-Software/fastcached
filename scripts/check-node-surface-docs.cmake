# SPDX-License-Identifier: Apache-2.0
#
# The operator-facing port documentation must agree with `NodeSurfaceTable()`,
# which is the one place a node's surfaces are declared.
#
# A port table is the document somebody writes firewall rules from. Wrong here
# costs a closed port on a machine that needed it open, or an open one that did
# not -- and one of these documents is a hand-pasted `--print-surfaces`
# transcript, which looks like machine output and is a string in a markdown file.
# A reader has no way to tell it from the real thing (#462).
#
# ## Both directions, because one of them is the dangerous one
#
# Every surface the table declares must appear, AND nothing surface-shaped may
# appear that the table does not declare. A scan that only walks the table passes
# a document that has grown a port the binary never opens -- which for a firewall
# worksheet reads as authorisation to open something.
#
# ## Why there is a COUNT rule as well as a row rule
#
# This is the part nobody will reconstruct, so it is written down. The row rule
# cannot see prose. #290 merged three ports into one, #290 stage 3 then corrected
# every port NUMBER by hand, and the two things that survived that hand-edit were
# both sentences rather than table rows:
#
#     cluster-communication.md   "carries the full six-surface table"   (it has four)
#     fastcache-compile-node.md  "Five TCP rules and one wrong one"     (there are three)
#
# The second sits directly under a correct four-row table, and an operator counts
# firewall rules from it. Neither is reachable by comparing rows to rows.
#
# ## Why the count rule is anchored where it is
#
# `surface` means two different things in these documents, and that is not sloppy
# writing -- it is what #290 left behind. There are FOUR ports (this table) and
# THREE policy roles on the node port (cache verbs, compile verbs, scheduler
# verbs), and prose legitimately counts either. So a count rule anchored on the
# word `surface` alone checks two concepts with one regex and fires on correct
# prose, and a rule with a known false-positive class gets disabled by the third
# person it inconveniences.
#
# The anchor is therefore narrow and the residue is REFUSED rather than ignored:
# a phrasing that is a claim about the set of ports is matched by a row of
# `FastCachedSurfaceCountPatterns` and checked, and every other number-plus-
# `surface` collocation must appear in `FastCachedSurfaceCountExemptions` with a
# reason. A new wording is neither checked wrongly nor waved through -- it fails
# until somebody says which of the two senses it is in.
#
# ## What this deliberately cannot see, measured rather than assumed
#
# The count rule requires the number to be ADJACENT to `surface`. Measured on
# this corpus, that misses `distributed-compilation.md`'s "Membership gates
# **two** of a node's surfaces", where four words intervene. Widening the gap
# re-admits the false-positive class the section above exists to avoid, so the
# gap stays narrow and the limit is stated here instead.
#
# It also says nothing about prose naming a port that no longer exists -- "the
# compile port", "its own port". There is no anchor separating those from the
# ports that do exist without a hand-kept list of retired names, which would be a
# second copy of history and would go stale in the direction that reports green.
# Both are limits rather than open work: a check that covers less and is right is
# worth more than one that covers everything and cries wolf.
#
# Runs as `cmake -P`. See `check-script-check-signals.cmake` for why such a check
# reports failure through its OUTPUT rather than an exit code.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -P scripts/check-node-surface-docs.cmake
#
# Exit codes: 0 always. The verdict is the presence of `CMake Error` in the output.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()

# ---------------------------------------------------------------------------
# Where the truth is. Extracted, never restated: a second copy of the surface
# list is not a cross-check, it is a second thing to be wrong, and it would go
# stale in the direction that reports green.
set(FastCachedSurfaceSource "src/apps/fastcache-compile-node/NodeSurfaces.cpp")
set(FastCachedSurfaceHeader "src/apps/fastcache-compile-node/NodeSurfaces.hpp")

# Every markdown page under `docs/` is scanned. GLOBBED, not listed, and that
# direction is the whole lesson of the two tickets that reached it before this
# one: `DocumentedCommandLines_test.cpp` records that a hand-written include list
# over these same documents silently omitted two pages carrying real node
# invocations, and `check-sccache-backend-caveat.cmake` globs its roots for the
# same reason. A list is exact about the pages it knows and silent about the ones
# it does not, and silence reads identically to complete coverage.
#
# Measured before choosing: globbing costs ZERO exemption rows today -- every
# number-plus-`surface` collocation under `docs/` already falls inside the pages
# a hand list would have named. What it buys is the pages such a list would have
# missed, among them `cluster-discovery.md`'s "What this means for a firewall"
# paragraph, which is the exact document class this check exists for.
#
#   <path substring>|<why this page is not judged>
#
# Deliberately empty, which is legal and is the strong position: a row here is a
# page somebody decided the table does not govern, and no such page has been
# found. A row matching nothing is refused below, so this cannot rot quietly.
set(FastCachedSurfaceDocExclusions
)

# Phrasings that are a claim about THE SET OF PORTS A NODE OPENS, and what the
# number in each must equal.
#
#   <regex, one capture group holding the number word>|<the literal it cannot match without>|<total|tcp|udp>|<why this phrasing is a port claim and not a policy-role one>
#
# Each names the port set explicitly -- a table, a process, a protocol's firewall
# rules -- which is what separates it from a sentence counting policy roles. No
# row may contain a ';'.
#
# The second field is what lets a document be skipped WITHOUT splitting it into
# lines: 73 of the 87 pages under `docs/` contain none of these literals. It is
# the same fix .agent/rules/metrics-and-observability.md records taking
# `worker-refusals-counted` from 2.9 s to 208 ms -- but the win here is far
# smaller and the honest figure is worth more than the precedent: measured on WSL
# 9p, 1.69 s before and 1.47 s after. The remainder is 87 `file(READ)`s and the
# 14 pages that DO mention a surface, and a filter can remove neither. It earns
# its place because the skipped share holds as `docs/` grows, not because it is
# the difference between fast and slow today.
#
# The needle is carried on the row rather than in a list beside the table, so a
# new pattern cannot arrive without one. It must be a literal the regex CANNOT
# match without: a needle broader than its pattern only costs time, one narrower
# silently skips a page the pattern would have caught.
set(FastCachedSurfaceCountPatterns
    "([a-z]+)-surface table|-surface table|total|Names the table itself, so the number is that table's arity. This is the wording that went stale as `six-surface` while the table had four rows."
    "up to ([a-z]+) surfaces in one process| surfaces in one process|total|Counts what one node process opens, which is the port set by definition."
    "([A-Za-z]+) TCP rules| TCP rules|tcp|Firewall rules for one protocol. A policy role has no protocol, so this cannot be the other sense."
    "([A-Za-z]+) UDP rules| UDP rules|udp|The same, for the one UDP surface."
)

# The residue rule's own literal. Its regex hard-codes `surface`, so a page
# without that word cannot reach it -- and `print-surfaces` contains it, so
# Rule A is covered by the same needle.
set(FastCachedSurfaceResidueNeedle "surface")

# Number words, in ascending order so a word's VALUE is its position. Digits are
# deliberately absent: a numeric form falls through to the residue rule and is
# refused as unclassified, which is the safe direction for a wording nobody has
# judged.
set(FastCachedSurfaceNumberWords
    "one" "two" "three" "four" "five" "six" "seven" "eight" "nine" "ten"
)

# Every number-plus-`surface` collocation that is NOT a claim about the port set.
# Each row is a decision somebody made in review, which is why each carries its
# reason -- and why a row whose phrase no longer appears fails this check rather
# than being ignored. These rows are also the only written record in this
# repository that the word names two different things.
#
#   <path substring>|<the phrase, verbatim and on one line>|<which sense it is in, and why>
set(FastCachedSurfaceCountExemptions
    "fastcache-compile-node.md|other two surfaces|Policy ROLES. The sentence divides who may reach the cache verbs from who may reach the compile and scheduler verbs -- all three of which live on the one node port."
    "fastcache-compile-node.md|two surfaces membership governs|Policy ROLES: the compile verbs and the scheduler verbs, which membership admits a caller to. The cache verbs are excluded by #287 and are not a port distinction."
    "fastcache-compile-node.md|node's three surfaces|Policy ROLES. `--fleet-member` admits a host to the cache, compile and scheduler verbs, and it is not a statement about ports."
    "fastcache-compile-node.md|Three surfaces read it, not one|Policy ROLES, and a different three again: discovery, the scheduler's signing and the worker's validation all read the PSK. Two of those are not ports at all."
    "fastcache-compile-node.md|There is one surface now|Counts the surfaces carrying the COMPILE VERBS, which is one -- the node port -- rather than the arity of the port set. It is the sentence explaining why the startup question has one answer again after #290 merged the ports that used to give it two, so a future merge is exactly what should make somebody re-read it."
    "node-credential-gap.md|All three surfaces refuse|Policy ROLES -- the three verb families that answer `AUTH` with `unknown-opcode`."
    "node-credential-gap.md|the three surfaces once answered it three different ways|Policy ROLES, and historical: it describes what those verb families did before #283 and #340."
)

# ---------------------------------------------------------------------------
# Split one '|'-separated row into the variables named in ARGN, the last of which
# takes whatever remains -- so only the final field may contain a '|'.
#
# Copied verbatim from the sibling checks rather than varied, and that is the
# point: consolidating these into a shared module is #495, deliberately not
# pre-empted here, and #495's validation compares the copies as TEXT. A copy that
# rewrote an escape or a spelling would be equivalent and non-identical, which is
# exactly the divergence that consolidation cannot detect. Keep this byte-for-byte
# with `check-sccache-backend-caveat.cmake`, and count this file in when #495 lands.
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
#             direction, because a merged element does not shrink a window, it
#             WIDENS it to whatever it swallowed.
#
# All four are replaced by a space rather than escaped. Nothing is lost: no
# surface name, protocol token, number word or exemption phrase contains any of
# them, and a line's text is printed only as context in a diagnostic.
#
# Copied verbatim from the sibling checks rather than varied, and that is the
# point: consolidating these into a shared module is #495, deliberately not
# pre-empted here, and #495's validation compares the copies as TEXT. A copy that
# rewrote an escape or a spelling would be equivalent and non-identical, which is
# exactly the divergence that consolidation cannot detect. Keep this byte-for-byte
# with `check-sccache-backend-caveat.cmake`, and count this file in when #495 lands.
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

# ---------------------------------------------------------------------------
# Every '|'-table row parses, checked up front rather than where a row is used.
#
# A ';' anywhere in a row splits the CMake list in two, and the fragment is a row
# with too few fields. Whether that is ever NOTICED depends on where the fragment
# lands: a loop that breaks early never reaches it, and the only symptom is a
# reason sentence that stops mid-way. This file carried "No row may contain a
# ';'" in a comment above one of its own tables and then had one in another --
# the third time in one session that a rule written beside a table failed to
# carry to a table added later, so it is a check rather than a fourth comment.
#
#   <variable holding the table>|<how many fields each row has>
set(FastCachedSurfaceRowTables
    "FastCachedSurfaceDocExclusions|2"
    "FastCachedSurfaceCountPatterns|4"
    "FastCachedSurfaceCountExemptions|3"
)
foreach(tableRow IN LISTS FastCachedSurfaceRowTables)
    fastcached_row_fields("${tableRow}" tableName tableArity)
    set(rowIndex 0)
    list(LENGTH ${tableName} tableLength)
    foreach(row IN LISTS ${tableName})
        math(EXPR rowIndex "${rowIndex} + 1")
        string(REGEX MATCHALL "\\|" separators "${row}")
        list(LENGTH separators separatorCount)
        math(EXPR wantSeparators "${tableArity} - 1")
        if(separatorCount LESS ${wantSeparators})
            message(FATAL_ERROR
                "${tableName} row ${rowIndex} of ${tableLength} has ${separatorCount} '|' where "
                "${wantSeparators} are wanted, so it is a fragment rather than a row: '${row}'. A "
                "';' anywhere in a row splits the CMake list in two and truncates it silently.")
        endif()
    endforeach()
endforeach()

set(violations "")

# ---------------------------------------------------------------------------
# Ground truth: walk the surface rows, pairing each `.name` with the `.flags` and
# `.protocol` that follow it.
#
# Walked line by line rather than lifted with one whole-file `MATCHALL`, which is
# the idiom `check-node-config-reference.cmake` uses for a single field. The
# reason is `check-tsan-scope.cmake`'s: a match spanning several lines stops at
# the first thing resembling its terminator, so a future `.flags` value or
# `.note` holding one of CMake's list-reserved characters would silently drop
# rows. A walk over neutralised lines cannot lose a row without noticing -- a
# `.protocol` reached with no `.name` in hand is reported, because a verdict
# drawn from a scan that lost the table's shape is worth nothing.
set(surfaceSource "${FASTCACHED_SOURCE_DIR}/${FastCachedSurfaceSource}")
if(NOT EXISTS "${surfaceSource}")
    message(FATAL_ERROR
        "the surface table's source is missing: ${FastCachedSurfaceSource}. This check has no "
        "ground truth without it and must not report on the documents alone.")
endif()

file(READ "${surfaceSource}" surfaceContent)
fastcached_split_lines("${surfaceContent}" surfaceLines)

set(surfaceNames "")
set(surfaceProtocols "")
set(surfaceFlags "")
set(pendingName "")
set(pendingFlag "")
foreach(line IN LISTS surfaceLines)
    if(line MATCHES "^[ \t]*\\.name[ \t]*=[ \t]*\"([a-z]+)\"")
        set(pendingName "${CMAKE_MATCH_1}")
        set(pendingFlag "")
    elseif(line MATCHES "^[ \t]*\\.flags[ \t]*=[ \t]*\\{[ \t]*\"([^\"]+)\"")
        set(pendingFlag "${CMAKE_MATCH_1}")
    elseif(line MATCHES "^[ \t]*\\.protocol[ \t]*=[ \t]*SurfaceProtocol::([A-Za-z]+)")
        if(pendingName STREQUAL "")
            list(APPEND violations
                 "${FastCachedSurfaceSource}: a `.protocol` row with no `.name` above it; this scan has lost the table's shape, so nothing it says about the documents is worth reading")
        else()
            list(APPEND surfaceNames "${pendingName}")
            string(TOUPPER "${CMAKE_MATCH_1}" protocolToken)
            list(APPEND surfaceProtocols "${protocolToken}")
            list(APPEND surfaceFlags "${pendingFlag}")
            set(pendingName "")
            set(pendingFlag "")
        endif()
    endif()
endforeach()

list(LENGTH surfaceNames surfaceCount)
if(surfaceCount EQUAL 0)
    message(FATAL_ERROR
        "no surface rows were extracted from ${FastCachedSurfaceSource}. Either the table moved or "
        "its row syntax changed -- and a scan with no ground truth would agree with every document "
        "perfectly. Two empty lists agree, which is the failure this refusal exists to prevent.")
endif()

# A PARTIAL parse is the likelier failure and the nastier one: extract three rows
# of four and every correct document is reported wrong, so whoever fixes it edits
# the prose to match a broken scan. The arity is declared independently by the
# enum, so it is cross-checked rather than trusted.
set(surfaceHeader "${FASTCACHED_SOURCE_DIR}/${FastCachedSurfaceHeader}")
if(NOT EXISTS "${surfaceHeader}")
    message(FATAL_ERROR
        "the surface enum's header is missing: ${FastCachedSurfaceHeader}. Without it the row count "
        "cannot be cross-checked, and a partial parse would be reported as the documents being wrong.")
endif()
file(READ "${surfaceHeader}" headerContent)
fastcached_split_lines("${headerContent}" headerLines)

set(enumeratorCount 0)
set(inEnum FALSE)
foreach(line IN LISTS headerLines)
    if(line MATCHES "^enum class NodeSurface")
        set(inEnum TRUE)
        continue()
    endif()
    if(NOT inEnum)
        continue()
    endif()
    if(line MATCHES "^\\}")
        break()
    endif()
    # `Last` states the count and is never a surface, so it is not one here either.
    if(line MATCHES "^[ \t]+([A-Za-z][A-Za-z0-9]*)[ \t]*(=[^,]*)?,")
        if(NOT CMAKE_MATCH_1 STREQUAL "Last")
            math(EXPR enumeratorCount "${enumeratorCount} + 1")
        endif()
    endif()
endforeach()

if(NOT surfaceCount EQUAL ${enumeratorCount})
    message(FATAL_ERROR
        "${surfaceCount} surface row(s) were extracted from ${FastCachedSurfaceSource} while "
        "${FastCachedSurfaceHeader} declares ${enumeratorCount} surface enumerator(s). This scan has "
        "lost part of the table, so every document it disagrees with is one it cannot judge -- fix "
        "the extraction rather than the prose.")
endif()

set(tcpCount "${surfaceProtocols}")
list(FILTER tcpCount INCLUDE REGEX "^TCP$")
list(LENGTH tcpCount tcpCount)
set(udpCount "${surfaceProtocols}")
list(FILTER udpCount INCLUDE REGEX "^UDP$")
list(LENGTH udpCount udpCount)
set(totalCount "${surfaceCount}")

# ---------------------------------------------------------------------------
set(docNeedles "${FastCachedSurfaceResidueNeedle}")
foreach(patternRow IN LISTS FastCachedSurfaceCountPatterns)
    fastcached_row_fields("${patternRow}" countPattern countNeedle countKind countReason)
    list(APPEND docNeedles "${countNeedle}")
endforeach()

file(GLOB_RECURSE docFiles LIST_DIRECTORIES false "${FASTCACHED_SOURCE_DIR}/docs/*.md")
list(SORT docFiles)

set(exclusionsUsed "")
set(exemptionsUsed "")
set(flagsSeen "")
set(transcriptsChecked 0)
set(countClaimsChecked 0)
set(docsScanned 0)

foreach(docFile IN LISTS docFiles)
    file(RELATIVE_PATH docPath "${FASTCACHED_SOURCE_DIR}" "${docFile}")

    set(excluded FALSE)
    foreach(row IN LISTS FastCachedSurfaceDocExclusions)
        fastcached_row_fields("${row}" exclusionPath exclusionReason)
        string(FIND "${docPath}" "${exclusionPath}" position)
        if(NOT position EQUAL -1)
            set(excluded TRUE)
            list(APPEND exclusionsUsed "${row}")
            break()
        endif()
    endforeach()
    if(excluded)
        continue()
    endif()
    math(EXPR docsScanned "${docsScanned} + 1")

    file(READ "${docFile}" docContent)

    # Flags are looked for across the whole corpus rather than per document: no
    # single page is obliged to name every surface.
    foreach(flag IN LISTS surfaceFlags)
        if(NOT flag STREQUAL "")
            string(FIND "${docContent}" "${flag}" position)
            if(NOT position EQUAL -1)
                list(APPEND flagsSeen "${flag}")
            endif()
        endif()
    endforeach()

    # Whole-file first: neither rule can fire on a page holding none of the
    # literals its patterns need, and splitting one into lines is the expensive
    # part. Exact rather than approximate -- every needle comes off the pattern
    # that needs it, so this cannot skip a page a rule would have matched.
    set(worthSplitting FALSE)
    foreach(needle IN LISTS docNeedles)
        string(FIND "${docContent}" "${needle}" position)
        if(NOT position EQUAL -1)
            set(worthSplitting TRUE)
            break()
        endif()
    endforeach()
    if(NOT worthSplitting)
        continue()
    endif()
    fastcached_split_lines("${docContent}" docLines)

    # -----------------------------------------------------------------------
    # ONE pass over the lines, serving both rules.
    #
    # Rule A: a `--print-surfaces` transcript is machine output or it is a lie.
    # Checked HARDER than prose, deliberately. A fenced block is the one place in
    # these documents that claims to be output, so it is the one place where a
    # checkable contract exists -- and de-transcripting it would cost the "ask
    # the binary" advice its illustration. The label is the row's name plus, when
    # an endpoint has one, its role (`discovery beacon`), so it is matched by its
    # FIRST word.
    #
    # Rule B: a count of the port set must equal the table's arity, and any other
    # number-plus-`surface` collocation must have been classified by a human. The
    # LINE is the context, which is why this rides the same walk: taking a context
    # out of the raw content instead needed a `[^\n]*`-wrapped pattern that is
    # quadratic in line length and hands back UNSANITISED text.
    set(fenceState "")
    set(seenInFence "")
    foreach(line IN LISTS docLines)
        if(line MATCHES "^[ \t]*```")
            if(fenceState STREQUAL "transcript")
                math(EXPR transcriptsChecked "${transcriptsChecked} + 1")
                if(NOT seenInFence STREQUAL "${surfaceNames}")
                    set(missing "${surfaceNames}")
                    if(seenInFence)
                        list(REMOVE_ITEM missing ${seenInFence})
                    endif()
                    if(missing)
                        foreach(absent IN LISTS missing)
                            list(APPEND violations
                                 "${docPath}: a `--print-surfaces` transcript does not list the `${absent}` surface, which ${FastCachedSurfaceSource} declares -- an operator copying it opens one port too few")
                        endforeach()
                    else()
                        list(JOIN seenInFence ", " sawText)
                        list(JOIN surfaceNames ", " wantText)
                        list(APPEND violations
                             "${docPath}: a `--print-surfaces` transcript lists [${sawText}] where the table declares [${wantText}] -- the same surfaces in a different order, and the order and the membership both come from the table, so this is not output this binary produces")
                    endif()
                endif()
                set(fenceState "")
            elseif(fenceState STREQUAL "")
                set(fenceState "fence")
            else()
                set(fenceState "")
            endif()
            continue()
        endif()

        if(NOT fenceState STREQUAL "")
            if(line MATCHES "print-surfaces")
                set(fenceState "transcript")
                set(seenInFence "")
                continue()
            endif()
            if(fenceState STREQUAL "transcript")
                # An output row: a bare word in column one, then an address or a
                # `-`. The `$` line, the `notes:` block and the wrapped
                # continuation of the command all fail the two-column shape.
                if(line MATCHES "^([a-z]+)([ \t]+[a-z]*)?[ \t]+([^ \t]+)[ \t]")
                    set(label "${CMAKE_MATCH_1}")
                    list(FIND surfaceNames "${label}" position)
                    if(position EQUAL -1)
                        list(APPEND violations
                             "${docPath}: a `--print-surfaces` transcript has a `${label}` line, and ${FastCachedSurfaceSource} declares no such surface -- a reader cannot tell this from real output, and on a firewall worksheet an extra row reads as authorisation to open a port")
                    else()
                        list(APPEND seenInFence "${label}")
                    endif()
                endif()
                continue()
            endif()
        endif()

        # Rule B. A port-set phrasing is checked; anything else counting
        # something called a surface must have been classified.
        set(handled FALSE)
        foreach(patternRow IN LISTS FastCachedSurfaceCountPatterns)
            fastcached_row_fields("${patternRow}" countPattern countNeedle countKind countReason)
            if(line MATCHES "${countPattern}")
                string(TOLOWER "${CMAKE_MATCH_1}" numberWord)
                list(FIND FastCachedSurfaceNumberWords "${numberWord}" wordIndex)
                if(NOT wordIndex EQUAL -1)
                    math(EXPR claimed "${wordIndex} + 1")
                    math(EXPR countClaimsChecked "${countClaimsChecked} + 1")
                    set(handled TRUE)
                    if(NOT claimed EQUAL ${${countKind}Count})
                        list(APPEND violations
                             "${docPath}: `${line}` claims ${claimed} where ${FastCachedSurfaceSource} declares ${${countKind}Count}. This phrasing is a claim about the port set because: ${countReason}")
                    endif()
                    break()
                endif()
            endif()
        endforeach()
        if(handled)
            continue()
        endif()

        if(NOT line MATCHES "([A-Za-z]+)[- ]surfaces?")
            continue()
        endif()
        string(TOLOWER "${CMAKE_MATCH_1}" leadingWord)
        list(FIND FastCachedSurfaceNumberWords "${leadingWord}" wordIndex)
        if(wordIndex EQUAL -1)
            continue()
        endif()

        foreach(exemptRow IN LISTS FastCachedSurfaceCountExemptions)
            fastcached_row_fields("${exemptRow}" exemptPath exemptPhrase exemptReason)
            string(FIND "${docPath}" "${exemptPath}" pathPosition)
            if(pathPosition EQUAL -1)
                continue()
            endif()
            string(FIND "${line}" "${exemptPhrase}" phrasePosition)
            if(NOT phrasePosition EQUAL -1)
                set(handled TRUE)
                list(APPEND exemptionsUsed "${exemptRow}")
                break()
            endif()
        endforeach()

        if(NOT handled)
            list(APPEND violations
                 "${docPath}: `${line}` counts something called a surface and nothing says which sense it is in. `surface` means BOTH the ${surfaceCount} ports this node opens AND the policy roles that share the node port, so this cannot be judged automatically -- either write it in a form FastCachedSurfaceCountPatterns recognises, or add it to FastCachedSurfaceCountExemptions saying which sense it is in")
        endif()
    endforeach()
endforeach()

# ---------------------------------------------------------------------------
# Every surface's own flag must reach the documentation somewhere. It is the
# third mechanical column of the table an operator maps a firewall rule to a flag
# with, and without it `--listen-raft` could be renamed while every page kept the
# old spelling and this check stayed green.
set(surfaceIndex 0)
foreach(flag IN LISTS surfaceFlags)
    list(GET surfaceNames ${surfaceIndex} name)
    math(EXPR surfaceIndex "${surfaceIndex} + 1")
    if(flag STREQUAL "")
        continue()
    endif()
    list(FIND flagsSeen "${flag}" seen)
    if(seen EQUAL -1)
        list(APPEND violations
             "the `${name}` surface is configured by `${flag}` and no scanned document names that flag -- either it was renamed and the documentation still spells the old one, or a surface an operator has to open is undocumented")
    endif()
endforeach()

# An exclusion or exemption whose subject has gone is one that has stopped being
# checked, and it would go on excusing whatever takes that wording next.
foreach(row IN LISTS FastCachedSurfaceDocExclusions)
    list(FIND exclusionsUsed "${row}" position)
    if(position EQUAL -1)
        fastcached_row_fields("${row}" exclusionPath exclusionReason)
        list(APPEND violations
             "the exclusion for `${exclusionPath}` matched no document. It was excluded because: ${exclusionReason}")
    endif()
endforeach()
foreach(row IN LISTS FastCachedSurfaceCountExemptions)
    list(FIND exemptionsUsed "${row}" position)
    if(position EQUAL -1)
        fastcached_row_fields("${row}" exemptPath exemptPhrase exemptReason)
        list(APPEND violations
             "${exemptPath}: the exemption for `${exemptPhrase}` matched nothing. It was exempted because: ${exemptReason}")
    endif()
endforeach()

# A scan that examined nothing agrees with everything. Each half is asserted
# separately: a document set that yielded no transcripts and a table that yielded
# no rows are different failures with different fixes.
if(docsScanned EQUAL 0)
    list(APPEND violations
         "no markdown document was found under docs/. The glob has stopped matching, and a scan over no documents agrees with all of them")
endif()
if(transcriptsChecked EQUAL 0)
    list(APPEND violations
         "no `--print-surfaces` transcript was found in any scanned document. Either the fenced blocks moved or their shape changed -- and a scan that checks no transcript passes a stale one forever")
endif()
if(countClaimsChecked EQUAL 0)
    list(APPEND violations
         "no port-set count claim was found in any scanned document. FastCachedSurfaceCountPatterns exists because two such claims went stale through a hand-edit that corrected every port NUMBER, so matching none of them means this rule has stopped looking rather than that the prose is clean")
endif()

if(violations)
    message("")
    foreach(violation IN LISTS violations)
        message("  ${violation}")
    endforeach()
    message("")
    message("`NodeSurfaceTable()` is where a node's surfaces are declared, and these documents")
    message("are a second copy of that data with no link back to it. A port table is what an")
    message("operator writes firewall rules from, so a wrong one closes a port that was needed")
    message("or opens one that was not -- and the transcript form actively invites belief.")
    message("")
    message("The tables live in ${CMAKE_CURRENT_LIST_FILE}.")
    list(LENGTH violations violationCount)
    message(FATAL_ERROR "node surface docs: ${violationCount} finding(s)")
endif()

message(STATUS
    "node surface docs: ${surfaceCount} surface(s) (${tcpCount} TCP, ${udpCount} UDP) against "
    "${docsScanned} document(s) -- ${transcriptsChecked} transcript(s), ${countClaimsChecked} "
    "port-set count claim(s) and ${surfaceCount} flag(s) checked")
