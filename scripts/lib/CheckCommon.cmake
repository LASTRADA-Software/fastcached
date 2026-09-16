# SPDX-License-Identifier: Apache-2.0
#
# What the `scripts/check-*.cmake` hygiene checks share.
#
# ## Why there was no such module until now
#
# There were twelve-plus of these checks and not one of them `include()`d another,
# so every common idiom existed three to five times (#495). This is the module that
# ends that, and #513 is the half of it about the `"value|reason"` ROW convention.
#
# ## What was measured before anything moved
#
# #513 recorded four copies under three names and reasoned that three names implies
# three implementations "that may differ". Half of that is wrong, and the half that
# is right matters more than the ticket expected. Measured by extracting every
# definition from `function(` to its matching `endfunction()` and hashing it:
#
#   fastcached_row_fields          6 copies, ONE digest -- byte-identical
#   fastcached_split_rows          2 copies, ONE digest -- byte-identical
#   fastcached_split_account_row   1
#   fastcached_registration_fields 1
#
# Nine general splitters under four names, not four under three: the tree grew after
# the ticket was filed, and three of the six `fastcached_row_fields` copies carry a
# comment telling the next author to keep them byte-identical and to count the file
# in when #495 lands. Those comments worked. Within a name there is no divergence at
# all.
#
# ## The divergence is BETWEEN the names, and it is about CMake lists
#
# `fastcached_row_fields` walks the row with `string(FIND)` and `string(SUBSTRING)`
# and builds no CMake list. The other three all hand text to CMake's list parser:
# `fastcached_split_account_row` and `fastcached_registration_fields` through
# `string(REPLACE "|" ";")` and `list(GET)`, and `fastcached_split_rows` by
# `list(APPEND)`ing into two parallel lists.
#
# The copies were then driven against each other on the awkward rows #513 names --
# an empty field, a value holding the separator, a trailing separator, a row with no
# separator at all -- plus a `;` and an unbalanced bracket in each position. What
# came back is not what the ticket expected, and not what this comment said before
# it was measured:
#
#   row            fastcached_row_fields    fastcached_split_account_row
#   a|b|c          a, b, c                  same
#   a|b|           a, b, ""                 same
#   a||c           a, "", c                 same
#   |b|c           "", b, c                 same
#   ab             REFUSED                  REFUSED
#   a|b            REFUSED                  REFUSED
#   a|b|c|d        a, b, "c|d"              REFUSED -- "got 4"
#   a|b|c;d        a, b, "c;d"              REFUSED -- "got 4"
#   a|[b|c         a, "[b", c               REFUSED -- "got 2"
#   [a|b|c         "[a", b, c               REFUSED -- "got 1"
#   a|b|[c         a, b, "[c"               same
#   a|b|c[         a, b, "c["               same
#   a|b|c]d        a, b, "c]d"              same
#
# **Every divergence is about which one REFUSES, and the list-building one always
# refuses LOUDLY.** Not one of them is a silent wrong answer, and an unbalanced
# bracket in the LAST field is inert in both. So consolidating on the list-free
# implementation is a change toward LAXITY, not a silent-defect fix -- two checks now
# accept rows they used to refuse: one with more separators than fields (the last
# absorbs the rest, which is what lets a reason be written in prose), and one whose
# earlier field carries a `;` or an unbalanced `[`.
#
# That is defensible and is the reason it was chosen: the six-copy majority already
# had this contract, refusing a prose reason for containing a `|` is the worse
# outcome of the two, and a row with four fields where three are read folds the
# fourth into text that is only ever printed. But it is a behaviour change and is
# recorded as one rather than described as a repair.
#
# `fastcached_split_rows` differs from `fastcached_row_fields` in the refusal WORDING
# alone ("Malformed row (no '|')" against "wanted 2 '|'-separated fields"), on every
# input measured.
#
# One hazard is upstream of all four and none of them can help with it: the row
# TABLES are CMake lists, so an unbalanced bracket or a `;` inside a row merges or
# splits it before any splitter is called. Measured on `check-service-accounts`, an
# unbalanced `[` in a reason takes it from 2 accounts to 1 -- identically before and
# after this consolidation, because the damage is done at `foreach(row IN LISTS ...)`.
# Keep `;` and unbalanced brackets out of row text.
#
# ## What is deliberately NOT reconciled
#
# `fastcached_split_rows` keeps its parallel-list OUTPUT. That shape is the one
# #495 calls out -- two lists that must be kept in step by hand -- and removing it
# means restructuring ten consumer sites across two checks that want a path list to
# `list(FIND)` in. That is its own change, and doing it inside a consolidation would
# put an unproven refactor of two live hygiene checks behind a rename. What it gets
# here is ONE definition of the convention: where the separator is, which field
# absorbs the rest, and what a malformed row does now live in one function that it
# calls, rather than in a second implementation that can drift from it.
#
# ## No `cmake_minimum_required` here, deliberately
#
# This file is INCLUDED, never run. The includer states the policies -- every
# registered check does, and `check-script-check-signals` pass 3 refuses one that
# does not -- and a declaration here would push a second policy stack onto a scope
# that did not ask for one.

# Split one '|'-separated row into the variables named in ARGN, the last of which
# takes whatever remains -- so only the final field may contain a '|', which is what
# lets a reason be written in ordinary prose.
#
# Never builds a CMake list, so a field containing ';', '\', '[' or ']' is harmless.
# It carries the malformed-row refusal itself, so a caller needs no field-count
# guard of its own.
#
# The row LIST is a separate hazard and this cannot help with it: `set(rows "a;b")`
# splits at the list level before this is ever called. Keep ';' out of row text.
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

# Split a `"value|reason"` TABLE into two parallel lists, so a reason can be printed
# beside the rule it explains rather than being a comment nobody reads.
#
# The split itself is `fastcached_row_fields`, so where the separator is and what a
# malformed row does are stated once. The parallel-list output is this function's
# own shape and is discussed in this file's header.
#
# @param rows The NAME of the list variable holding the rows.
# @param valuesOut Set to the list of first fields, in row order.
# @param reasonsOut Set to the list of last fields, in row order.
function(fastcached_split_rows rows valuesOut reasonsOut)
    set(values "")
    set(reasons "")
    foreach(row IN LISTS ${rows})
        fastcached_row_fields("${row}" rowValue rowReason)
        list(APPEND values "${rowValue}")
        list(APPEND reasons "${rowReason}")
    endforeach()
    set(${valuesOut} "${values}" PARENT_SCOPE)
    set(${reasonsOut} "${reasons}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# Splitting file content into lines. TWO functions, not one, and they must not be
# merged (#495).
#
# The eight-plus copies of this looked like one idiom under three names. They are
# not: they are two idioms with OPPOSITE intent, and which one a site wants follows
# from what it does with the line next.
#
#   TOKENISED  the caller is about to parse the line as CMake ARGUMENTS, so `;`,
#              `[`, `]` and `\` are structure that has to be neutralised BEFORE
#              parsing. `check-glob-traversals` reads `file(GLOB_RECURSE ...)`
#              argument lists; blanking is chosen over escaping deliberately,
#              because an escape does not survive a line ending in a backslash and
#              every shell continuation in this repository is one.
#
#   VERBATIM   the caller PRINTS the line with an accurate `file:line`, so the text
#              has to survive. `;` is escaped rather than blanked; `[` and `]` are
#              still blanked, because nothing in this family matches on a bracket
#              and a space preserves every column and line number.
#
# A single shared function would have to pick one, and would break whichever family
# it did not choose -- SILENTLY, because the check would go on passing, having
# matched different text. That is why "split lines" was the wrong name: it is the
# name that hid the difference for eight copies.
#
# ## What was measured before they moved
#
# The count comparison that validates the rest of this module is structurally blind
# here: it sees a check examining a DIFFERENT NUMBER of subjects, and a dropped
# escape or a changed tab produces the same number of DIFFERENT ones. So the copies
# were driven against one another over content carrying a `;`, a balanced bracket
# pair, an unbalanced `[`, a trailing backslash and a tab, and the ELEMENTS were
# compared rather than the count:
#
#   line                A1 (lossy)         A2 (+tabs)         B1 (`\\;`)      B2 (`\;`)
#   "alpha; beta"       "alpha  beta"      same as A1         "alpha; beta"   same as B1
#   "[bracket] pair"    " bracket  pair"   same as A1         " bracket  pair" same as B1
#   "unbalanced [ here" "unbalanced   here" same as A1        "unbalanced   here" same
#   "back\slash tail"   "back slash tail"  same as A1         "back\slash tail" same as B1
#   "<TAB>tab indented" "<TAB>tab indented" " tab indented"   "<TAB>tab ..."  same as B1
#
# Two findings came out of that. The tab map is the ONLY within-family-A difference,
# and it is load-bearing rather than drift -- `check-byte-order-qualifier` spells
# optional whitespace as a plain space class, which CMake's regex engine will not
# match against a tab, so normalising it away would silently stop that check seeing
# tab-indented code. It is a named option here rather than a flattened default.
#
# And `check-target-file-guards` carried BOTH B spellings, in one file: `"\;"` at one
# reader and `"\\;"` at the other, with the bracket blanking on opposite sides of the
# escape. They are byte-identical on every element measured, so converging them is
# safe -- which is worth recording, because "one file spells it two ways" reads like
# a defect and is not one.
#
# ## What is NOT converted, and why
#
# `check-config-reference-reach` splits through an `@FC_SEMI@` PLACEHOLDER and blanks
# no brackets at all. That is a third strategy, and converting it would change what
# it matches: it keeps `;` inside the element rather than escaping it, and it is
# bracket-EXPOSED where both families here are not. Which of those matters is a
# question about that check's corpus -- exposure is a property of (reader, file,
# surviving lines) and never of the script -- so it is left where it is and named
# here rather than folded in quietly.

# Split content into lines for TOKENISING: `\`, `;`, `[` and `]` become spaces, so a
# line can be handed to a parser without its punctuation being read as structure.
# Lossy on exactly those four characters, and a space preserves every column.
#
# @param content The file content.
# @param linesOut Set to the content's lines, in order.
# @param ARGN `MAP_TABS` to turn tabs into spaces as well. Ask for it only when the
#        caller's pattern spells whitespace as a plain space class -- CMake's regex
#        engine does not read `\t` inside a bracket expression.
function(fastcached_split_lines_tokenised content linesOut)
    set(mapTabs FALSE)
    foreach(option IN LISTS ARGN)
        if(option STREQUAL "MAP_TABS")
            set(mapTabs TRUE)
        else()
            message(FATAL_ERROR "fastcached_split_lines_tokenised: unknown option `${option}`")
        endif()
    endforeach()
    string(REPLACE "\\" " " content "${content}")
    string(REPLACE ";" " " content "${content}")
    string(REPLACE "[" " " content "${content}")
    string(REPLACE "]" " " content "${content}")
    if(mapTabs)
        string(REPLACE "\t" " " content "${content}")
    endif()
    # ONE backslash each. CMake's argument parser turns `\r` and `\n` into the real
    # characters and its regex engine has no escapes of its own, so the doubled form
    # splits on the LETTER n instead.
    string(REGEX REPLACE "\r?\n" ";" lines "${content}")
    set(${linesOut} "${lines}" PARENT_SCOPE)
endfunction()

# Split content into lines that survive VERBATIM, for a caller that prints the line
# with an accurate `file:line`. `;` is escaped rather than blanked; `[` and `]` are
# blanked, because a bracket is grouping structure to CMake's list parser and nothing
# in this family matches on one.
#
# Only an UNBALANCED bracket groups -- `[[nodiscard]]` is inert -- so blanking every
# bracket is broader than the truth. It is safe here by WHAT THE CALLERS MATCH rather
# than by construction: a pattern that itself contained a bracket would stop matching,
# silently. `check-worker-refusals-counted` is the worked example and answers it by
# walking its lines without ever building a CMake list.
#
# @param content The file content.
# @param linesOut Set to the content's lines, in order.
function(fastcached_split_lines_verbatim content linesOut)
    string(REPLACE ";" "\\;" content "${content}")
    string(REPLACE "[" " " content "${content}")
    string(REPLACE "]" " " content "${content}")
    string(REPLACE "\r\n" "\n" content "${content}")
    string(REPLACE "\n" ";" lines "${content}")
    set(${linesOut} "${lines}" PARENT_SCOPE)
endfunction()

# The third-party roots (#1370): which directories of the repository hold source this
# project copies from elsewhere, read from `scripts/lib/third-party-roots.txt`, the ONE
# answer to "is this path first-party?". That file carries the format and the reasons; the
# bash reader is `third_party_roots` in `scripts/lib/third-party-roots.sh`.
#
# Refuses with FATAL_ERROR -- never an empty answer -- when the file is missing, names no
# root, or names one spelled outside the format: a leading or trailing `/`, or `..`. A check
# handed "nothing is third-party" would take every vendored file as this project's own.
#
# @param sourceDir The repository root.
# @param rootsOut Set to the roots, in file order, relative to @p sourceDir.
function(fastcached_third_party_roots sourceDir rootsOut)
    set(rootsFile "${sourceDir}/scripts/lib/third-party-roots.txt")
    if(NOT EXISTS "${rootsFile}")
        message(FATAL_ERROR
            "third-party roots: ${rootsFile} is missing, so no check can tell third-party files "
            "from this project's own")
    endif()
    file(READ "${rootsFile}" content)
    fastcached_split_lines_verbatim("${content}" lines)
    set(roots "")
    foreach(line IN LISTS lines)
        string(REGEX REPLACE "#.*$" "" line "${line}")
        string(STRIP "${line}" line)
        if(line STREQUAL "")
            continue()
        endif()
        if(line MATCHES "^/" OR line MATCHES "/$" OR line MATCHES "\\.\\." OR line MATCHES "\\\\")
            message(FATAL_ERROR
                "third-party roots: ${rootsFile} names '${line}', which is not a root relative "
                "to the repository (no leading or trailing '/', no '..', no backslash)")
        endif()
        list(APPEND roots "${line}")
    endforeach()
    list(LENGTH roots rootCount)
    if(rootCount EQUAL 0)
        message(FATAL_ERROR
            "third-party roots: ${rootsFile} names no root; refused rather than read as "
            "'nothing is third-party'")
    endif()
    set(${rootsOut} "${roots}" PARENT_SCOPE)
endfunction()

# Split a list of repository-relative paths into this project's own and the ones under a
# third-party root (#1370). Refuses exactly as `fastcached_third_party_roots`, and reads the
# roots even for an empty list.
#
# A PREFIX test on `<path>/` against `<root>/`, never a regex: a root is a path name, and
# a `.` or `+` in one is a character, not a pattern.
#
# @param sourceDir The repository root whose roots file is read.
# @param pathsVar The NAME of the caller's list; rewritten to the first-party paths.
# @param declinedOut Receives the paths under a third-party root, in their original order.
function(fastcached_decline_third_party sourceDir pathsVar declinedOut)
    fastcached_third_party_roots("${sourceDir}" roots)
    set(kept "")
    set(declined "")
    foreach(path IN LISTS ${pathsVar})
        set(underRoot FALSE)
        foreach(root IN LISTS roots)
            string(FIND "${path}/" "${root}/" position)
            if(position EQUAL 0)
                set(underRoot TRUE)
                break()
            endif()
        endforeach()
        if(underRoot)
            list(APPEND declined "${path}")
        else()
            list(APPEND kept "${path}")
        endif()
    endforeach()
    set(${pathsVar} "${kept}" PARENT_SCOPE)
    set(${declinedOut} "${declined}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# One line of C++ with its comments removed, carrying whether a block comment is still open.
#
# The ONE implementation of comment stripping for the line walks below. It was the body of
# `fastcached_scan_code_lines` and moved out when `check-test-loops.cmake` needed the same
# stripping over whole content rather than a per-line match: the comment-ordering defect
# described inside has already had three copies, and a fourth would be the next.
#
# The blind spot, stated rather than papered over: CMake's regex engine is greedy and has no
# lazy quantifier, so stripping inline `/* ... */` pairs takes everything between the FIRST `/*`
# and the LAST `*/` on a line. Code sitting between two block comments on one line is invisible
# here. Still blind to either introducer inside a STRING LITERAL, as every regex-shaped reader
# here is.
#
# @param line The line, without its newline.
# @param inBlockComment Whether a block comment was open when the line began.
# @param strippedOut Set to what is left of the line: code only.
# @param skipOut Set to TRUE when the whole line sits inside a block comment.
# @param inBlockCommentOut Set to whether a block comment is still open when the line ends.
function(fastcached_strip_comment_line line inBlockComment strippedOut skipOut inBlockCommentOut)
    set(stripped "${line}")
    set(skipLine FALSE)
    if(inBlockComment)
        if(NOT stripped MATCHES "\\*/")
            set(skipLine TRUE)
            set(stripped "")
        else()
            string(REGEX REPLACE "^.*\\*/" "" stripped "${stripped}")
            set(inBlockComment FALSE)
        endif()
    endif()
    if(NOT skipLine)
        string(REGEX REPLACE "/\\*.*\\*/" " " stripped "${stripped}")

        # Which introducer comes FIRST decides. The order is not a detail: the `/*` test used
        # to run BEFORE `//` was stripped, so a line comment mentioning `/*` opened a block
        # comment no `*/` ever closed, and every remaining line of that file was skipped while
        # the caller still printed a clean count over lines it never read -- a false green, in
        # the one direction a check exists to refuse.
        #
        # Positional rather than simply stripping `//` first, which MEASURED identical on every
        # input tried: below the inline `/* ... */` strip above, removing `//...` removes any
        # `/*` that followed it too, so the two orderings agree. What position buys is not a
        # different verdict but independence -- it states the rule itself rather than being
        # correct only while the strip above it keeps running first. A reordering is correct by
        # PRECONDITION; this is correct by construction.
        #
        # Third copy of this defect: `check-cli-text-cell.cmake` and
        # `check-markup-entities.cmake` carried it too, in walks of their own.
        string(FIND "${stripped}" "/*" blockAt)
        string(FIND "${stripped}" "//" lineAt)
        if(NOT blockAt EQUAL -1 AND (lineAt EQUAL -1 OR blockAt LESS lineAt))
            string(SUBSTRING "${stripped}" 0 ${blockAt} stripped)
            set(inBlockComment TRUE)
        elseif(NOT lineAt EQUAL -1)
            string(SUBSTRING "${stripped}" 0 ${lineAt} stripped)
        endif()
    endif()
    set(${strippedOut} "${stripped}" PARENT_SCOPE)
    set(${skipOut} "${skipLine}" PARENT_SCOPE)
    set(${inBlockCommentOut} "${inBlockComment}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# C++ content with every comment removed and every NEWLINE kept, for a scan whose subject
# can span lines -- a `for` header broken across two, say -- and so cannot be matched one
# line at a time.
#
# Line numbers survive because only comment TEXT is removed: a line inside a block comment
# becomes empty rather than disappearing. List-free, for the reasons given at
# `fastcached_scan_code_lines` below, and O(n^2) in the same way, so a caller filters on a
# whole-file `string(FIND)` first.
#
# @param content The file content.
# @param outVar Set to the content with comments removed and newlines preserved.
function(fastcached_strip_comments content outVar)
    set(code "")
    set(rest "${content}")
    set(inBlockComment FALSE)
    while(TRUE)
        string(FIND "${rest}" "\n" newline)
        if(newline EQUAL -1)
            set(line "${rest}")
        else()
            string(SUBSTRING "${rest}" 0 ${newline} line)
        endif()
        string(REGEX REPLACE "\r$" "" line "${line}")
        fastcached_strip_comment_line("${line}" "${inBlockComment}" stripped skipLine inBlockComment)
        string(APPEND code "${stripped}")
        if(newline EQUAL -1)
            break()
        endif()
        string(APPEND code "\n")
        math(EXPR skip "${newline} + 1")
        string(LENGTH "${rest}" restLength)
        if(skip GREATER_EQUAL restLength)
            break()
        endif()
        string(SUBSTRING "${rest}" ${skip} -1 rest)
    endwhile()
    set(${outVar} "${code}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# Scanning C++ for a USE of something, as opposed to a MENTION of it in a comment.
#
# Walk content line by line WITHOUT ever building a CMake list of lines, and report, per
# line, whether @p pattern appears at all (`mention:<line>`) and whether it survives
# comment stripping (`use:<line>`). A mention is counted BEFORE stripping, so a caller's
# matched-nothing guard cannot be satisfied by the stripping alone.
#
# It was `fastcached_scan_lines` in `check-istreambuf-iterator.cmake` and moved here when
# `check-ranges-seam.cmake` became its second caller: a second copy citing the first in a
# comment vouches for its bugs without inheriting its fixes, and this one has had two.
#
# ## Why list-free
#
# The list-splitting idiom this was written instead of (`fastcached_read_lines`, before #495
# split it into the two functions above) escaped `;` and `\`, blanked `[` and `]`, then split
# on newlines into a CMake list -- and a line ending in a backslash still merged with the next
# one, which its own comment claimed it prevented. Measured on CMake 3.28 with a four-line
# file: no trailing backslash gave 5 elements, a line ending in `\` gave 4 with lines 2 and 3
# merged into `two \;three`, and a line ending in `\` merged too.
#
# A merged line is not cosmetic for a use-scan: the merged element begins with whatever line 2
# began with, so a `//` comment swallows the real code on line 3, the use goes unreported, and
# every line number below it drifts. That is a false GREEN, the direction that does not get
# investigated. It was found by the `istreambuf-iterator-selftest` case that plants list
# structure ABOVE a violation and asserts the exact `file:line` -- an assertion on the filename
# alone passes under the bug.
#
# So this walks with `FIND`/`SUBSTRING` and puts no line into a list at all: immune to `;`,
# `\`, `[` and `]` by construction rather than by escaping them one at a time.
# `check-tsan-scope.cmake` is list-free for the same reason.
#
# The walk is O(n^2) in the content's length, so a caller applies a whole-file `string(FIND)`
# first and walks only the files that contain the token at all.
#
# @param content The file content.
# @param pattern A CMake regular expression for the thing whose USE is sought.
# @param outVar Set to a list of `mention:<line>` and `use:<line>` entries, in line order.
function(fastcached_scan_code_lines content pattern outVar)
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

        if(line MATCHES "${pattern}")
            # A raw mention, comment or code. Counted before stripping, so a caller's
            # matched-nothing guard cannot be satisfied by stripping alone.
            list(APPEND hits "mention:${lineNumber}")
        endif()

        # Strip comments, so prose explaining the rule is not read as breaking it.
        fastcached_strip_comment_line("${line}" "${inBlockComment}" stripped skipLine inBlockComment)
        if(NOT skipLine)
            if(stripped MATCHES "${pattern}")
                list(APPEND hits "use:${lineNumber}")
            endif()
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
    set(${outVar} "${hits}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# Whether a directory is inside a git work tree, and when it is not, WHY.
#
# The last thing every enumerating check had its own copy of, and the one place the copies
# already disagreed in SPELLING: they compared the exit status with `EQUAL 0` while
# `check-repository-hygiene.cmake` used `STREQUAL "0"`. Both are right, and they are two
# statements of one fact, which is the shape that drifts.
#
# THREE answers rather than a bool, because the two callers need different things from a
# negative one and neither can recover the distinction from `FALSE`: an enumeration walks the
# directory either way, while a check with nothing to fall back on SKIPS -- and it owes an
# operator a different sentence for "git cannot answer here" than for "this is not a
# checkout". A validator returns a reason.
#
# @param sourceDir The directory to ask about.
# @param outVar Set to `work-tree`, `no-git` (no git executable, or it could not be run) or
#        `not-a-work-tree` (git answered, and the answer is no).
function(fastcached_work_tree_state sourceDir outVar)
    if(NOT GIT_EXECUTABLE)
        find_package(Git QUIET)
    endif()
    if(NOT GIT_EXECUTABLE)
        set(${outVar} "no-git" PARENT_SCOPE)
        return()
    endif()

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${sourceDir}" rev-parse --is-inside-work-tree
        OUTPUT_VARIABLE insideWorkTree
        ERROR_QUIET
        RESULT_VARIABLE gitStatus
        OUTPUT_STRIP_TRAILING_WHITESPACE)

    # A git that could not be RUN is `no-git`, not `not-a-work-tree`: a missing binary and a
    # broken one are the same fact for every caller, and neither is a statement about the
    # directory. Only an answer counts as one.
    if(NOT gitStatus EQUAL 0)
        set(${outVar} "no-git" PARENT_SCOPE)
    elseif(insideWorkTree STREQUAL "true")
        set(${outVar} "work-tree" PARENT_SCOPE)
    else()
        set(${outVar} "not-a-work-tree" PARENT_SCOPE)
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Every tracked file a check's rule is ABOUT, and HOW they were found.
#
# Eight `check-*.cmake` readers carried a byte-identical copy of this machinery, and each asks
# a DIFFERENT question with it: `*CMakeLists.txt`, `src/*.cpp` `src/*.hpp`, one subdirectory,
# the test files by pathspec, every tracked C++ source. The census is on #1485. So the thing
# to share is NOT the file set -- that is per check and has to stay so; migrating a check onto
# somebody else's set would silently narrow or widen what its rule is enforced over, which is
# the defect this consolidation exists to remove. What is shared is the MODE SELECTION: the
# work-tree probe, the `ls-files` call, the walk fallback, the exclusion list and the ordering.
#
# `set(excludeNames "out" "build" "_deps" ".git" ".cache" ".claude")` was SEVEN byte-identical
# copies. An exclusion list bets on the world's layout, so seven bets that have to move
# together is the costliest part of the duplication rather than the most visible one.
#
# ## The FILTER applies in BOTH modes, and that is the point
#
# A check whose git path and walk path cover different sets is a guard that enforces one rule
# on a developer's machine and another in CI, and this tree has shipped one (#1476). So a
# filter is applied to every candidate whichever mode produced it, rather than living in the
# pathspec on one side and in the glob on the other where nothing compares them.
#
# ## THREE modes, not two
#
# `git ls-files` succeeding and naming NOTHING is not the same event as there being no index,
# and every copy collapsed them: it fell through to the walk and announced
# `directory walk (no git index)` in a tree that has one. That is a true-sounding statement
# about the environment which is false, and it is the string a self-test case asserts on --
# which is how a guard whose self-test exercised the walk while CI exercised git passed for as
# long as it did. A fresh `git init` with nothing staged produces it, and the self-tests drive
# that shape deliberately, so the fallback stays; what changes is that it says WHY it ran.
#
# ## The fallback is gated on the MODE, never on the list being empty
#
# The copies asked `if(NOT sourceFiles)`, which conflates "no mode answered" with "a mode
# answered and its filter kept nothing" -- so a real git answer whose filter emptied it would
# be overwritten by a walk, and the verdict would name the wrong mode. The mode names what
# ANSWERED, not what came back non-empty.

## Every tracked file matching a question, and how they were found.
## @param sourceDir The repository root.
## @param PATHSPECS git pathspecs for `ls-files --`. Omit for every tracked file.
## @param GLOBS The walk fallback's patterns, relative to sourceDir. Required.
## @param FILTER A regex every candidate must match, in BOTH modes. Omit for no filter.
## @param FILES_OUT Name of the variable to receive the paths, relative to the root and sorted.
## @param MODE_OUT Name of the variable to receive the mode's name, for the caller's verdict.
function(fastcached_tracked_files sourceDir)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "FILTER;FILES_OUT;MODE_OUT" "PATHSPECS;GLOBS")

    foreach(required FILES_OUT MODE_OUT GLOBS)
        if(NOT arg_${required})
            message(FATAL_ERROR
                "fastcached_tracked_files: ${required} is required. Without FILES_OUT or "
                "MODE_OUT the caller gets an answer it cannot read or cannot name; without "
                "GLOBS it reports CLEAN over a source export that has no git index.")
        endif()
    endforeach()
    if(arg_UNPARSED_ARGUMENTS)
        # A misspelled keyword would otherwise be dropped in silence, and dropping GLOBS or
        # FILTER WIDENS what a rule is enforced over -- the direction that reads as thorough.
        message(FATAL_ERROR
            "fastcached_tracked_files: unrecognised argument(s) '${arg_UNPARSED_ARGUMENTS}'")
    endif()

    # `if(arg_FILTER STREQUAL "")` does NOT fire when the keyword was omitted: CMake reads an
    # unset left operand as the literal string `arg_FILTER`, which is not empty, so the test
    # comes back the wrong way round. Quoting the variable is the fix, and omitted and empty
    # then mean the same thing here, which is what a caller intends either way.
    set(hasFilter FALSE)
    if(NOT "${arg_FILTER}" STREQUAL "")
        set(hasFilter TRUE)
    endif()

    set(found "")
    set(mode "")

    fastcached_work_tree_state("${sourceDir}" workTreeState)
    if(workTreeState STREQUAL "work-tree")
        set(pathspecArguments "")
        if(arg_PATHSPECS)
            set(pathspecArguments -- ${arg_PATHSPECS})
        endif()
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${sourceDir}" ls-files ${pathspecArguments}
            OUTPUT_VARIABLE tracked
            RESULT_VARIABLE lsStatus
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(lsStatus EQUAL 0 AND tracked STREQUAL "")
            set(mode "directory walk (git index names no matching file)")
        elseif(lsStatus EQUAL 0)
            string(REPLACE "\n" ";" trackedFiles "${tracked}")
            foreach(candidate IN LISTS trackedFiles)
                if(hasFilter AND NOT candidate MATCHES "${arg_FILTER}")
                    continue()
                endif()
                list(APPEND found "${candidate}")
            endforeach()
            set(mode "git ls-files")
        endif()
    endif()

    if(NOT mode STREQUAL "git ls-files")
        # Build trees and caches, which are not source and are frequently enormous. ONE copy
        # of this list, and the walk is sound in an export precisely because an export
        # contains neither by construction.
        set(excludeNames "out" "build" "_deps" ".git" ".cache" ".claude")
        set(globPatterns "")
        foreach(pattern IN LISTS arg_GLOBS)
            list(APPEND globPatterns "${sourceDir}/${pattern}")
        endforeach()
        file(GLOB_RECURSE walked RELATIVE "${sourceDir}" ${globPatterns})
        foreach(candidate IN LISTS walked)
            if(hasFilter AND NOT candidate MATCHES "${arg_FILTER}")
                continue()
            endif()
            set(excluded FALSE)
            foreach(name IN LISTS excludeNames)
                if(candidate MATCHES "(^|/)${name}/")
                    set(excluded TRUE)
                    break()
                endif()
            endforeach()
            if(NOT excluded)
                list(APPEND found "${candidate}")
            endif()
        endforeach()
        if(mode STREQUAL "")
            set(mode "directory walk (no git index)")
        endif()
    endif()

    list(REMOVE_DUPLICATES found)
    list(SORT found)

    set(${arg_FILES_OUT} "${found}" PARENT_SCOPE)
    set(${arg_MODE_OUT} "${mode}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# Every first-party C++ source in the repository, and HOW they were found.
#
# One of the questions `fastcached_tracked_files` can be asked, and the one two checks ask:
# `check-enumerator-walks.cmake` and `check-ranges-seam.cmake`. It is NOT the question the
# other six enumerating checks ask -- see the census on #1485 -- so it is a caller of the
# shared machinery rather than the thing every check calls.
#
# Third-party files are DECLINED rather than dropped, and named, because vendored code is not
# ours to edit and a silent exclusion reads the same as complete coverage.
#
# @param sourceDir The repository root.
# @param filesOut Set to the first-party C++ paths, relative to the root and sorted.
# @param declinedOut Set to the third-party paths that were excluded.
# @param modeOut Set to a human-readable name for how the files were found.
function(fastcached_first_party_cxx sourceDir filesOut declinedOut modeOut)
    # `\\.` and not `\.`: CMake unescapes a quoted argument BEFORE the regex engine sees it,
    # so `"\.(...)"` arrives as `.(...)` -- a dot matching ANY character. That is not a
    # near-miss: `check-apt-update.sh` then matches, because `.sh` is any-character followed
    # by the `h` alternative, and this helper's first measured answer was 79 shell scripts too
    # many. Measured against an independent `git ls-files | grep -cE`, which is the only
    # reason it was seen at all.
    #
    # `ipp` and `inl` are in the set although this tree tracks NONE of either (measured: 0,
    # against 464 `.hpp` as a positive control on the same pattern). They are here because
    # `check-ranges-seam.cmake` covered them before it migrated, and the day somebody adds the
    # first `.ipp` is the day a narrower set silently stops enforcing the ranges seam there --
    # no verdict changing, no self-test failing. A difference that is invisible today and
    # silent on the day it matters is worse than one that shows up as a count.
    set(cxxExtension "\\.(cpp|cc|cxx|hpp|hh|hxx|h|ipp|inl|ixx|cppm)$")

    fastcached_tracked_files("${sourceDir}"
        GLOBS "*"
        FILTER "${cxxExtension}"
        FILES_OUT found
        MODE_OUT mode)

    fastcached_decline_third_party("${sourceDir}" found declined)

    set(${filesOut} "${found}" PARENT_SCOPE)
    set(${declinedOut} "${declined}" PARENT_SCOPE)
    set(${modeOut} "${mode}" PARENT_SCOPE)
endfunction()
