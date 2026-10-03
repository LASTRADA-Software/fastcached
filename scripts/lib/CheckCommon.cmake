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
# A string literal's CONTENTS are code here, and that is the whole of why this is a scan rather
# than a pair of regexes. Both of the blind spots this header used to declare were the same
# false GREEN, which is the one direction a check exists to refuse:
#
#   * a `/*` inside a literal opened a block comment. `SocketExchange.cpp:410` carries
#     `"...Accept: */*\r\n..."`, no `*/` follows on that line, and the remaining 51 lines of the
#     file -- a `for (;;)` among them -- were blanked while the caller printed a clean count over
#     lines it had never read. Proof that this was the only such file, and that there was one:
#     a C++ file that COMPILES cannot end inside an unterminated block comment, and exactly one
#     of 915 did.
#   * a `//` inside a literal truncated the line there. 21 lines across 16 files, losing text
#     like `//") == 0)` -- the rest of a `REQUIRE`.
#
# The old header called this "blind to either introducer inside a STRING LITERAL, as every
# regex-shaped reader here is", which reads as a false-POSITIVE risk and is the reason it sat
# for as long as it did. Being blind to a MENTION costs nothing; being blind to a USE is what
# this function exists to prevent.
#
# Two limits remain, and both are stated because neither is a guess:
#
#   * A literal that does not END on its line -- a raw string's first line, 10 of them here --
#     is kept as code from the quote onward, and the scan resumes on the next line outside any
#     literal. So a `/*` inside a raw string's BODY still opens a comment. Keeping the text is
#     the fail-CLOSED direction: a finding somebody can see and exempt, rather than a clean
#     report over what was dropped.
#   * `'` is NOT an opener. 262 lines here are digit separators (`1'000'000`), and treating a
#     quote-shaped character as a char literal would blank the tail of every one of them -- a
#     fix with twelve times the reach of the defect. A `'\"'` therefore opens a literal that
#     usually does not close on its line, which lands on the keep-it-all path above.
#
# Where the string literal opening at @p quoteAt ends, or -1 when it does not end on this line.
#
# Needed because a comment introducer inside a string literal is not one, and the closing quote
# is the only thing that says where the literal stops.
#
# A regex cannot answer it. CMake's engine has no lazy quantifier, so `^"([^"\\]|\\.)*"` runs
# greedily to the LAST quote on the line and reports `"a" + "b"` as a single literal -- which
# would hand back a literal spanning the code between them.
#
# An ODD run of backslashes immediately before a quote escapes it and an even one does not, so
# `"a\\"` ends at its last character while `"a\""` does not. Counted backwards, and never past
# the opening quote, whose own predecessors are outside the literal.
#
# @param text The line.
# @param quoteAt Index of the opening quote.
# @param endOut Set to the index of the closing quote, or -1 when there is none on this line.
function(fastcached_literal_end text quoteAt endOut)
    string(LENGTH "${text}" length)
    math(EXPR from "${quoteAt} + 1")
    while(from LESS length)
        string(SUBSTRING "${text}" ${from} -1 tail)
        string(FIND "${tail}" "\"" hit)
        if(hit EQUAL -1)
            break()
        endif()
        math(EXPR candidate "${from} + ${hit}")
        set(slashes 0)
        math(EXPR back "${candidate} - 1")
        while(back GREATER ${quoteAt})
            string(SUBSTRING "${text}" ${back} 1 character)
            if(NOT character STREQUAL "\\")
                break()
            endif()
            math(EXPR slashes "${slashes} + 1")
            math(EXPR back "${back} - 1")
        endwhile()
        math(EXPR escaped "${slashes} % 2")
        if(escaped EQUAL 0)
            set(${endOut} ${candidate} PARENT_SCOPE)
            return()
        endif()
        math(EXPR from "${candidate} + 1")
    endwhile()
    set(${endOut} -1 PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# @param line The line, without its newline.
# @param inBlockComment Whether a block comment was open when the line began.
# @param strippedOut Set to what is left of the line: code only.
# @param skipOut Set to TRUE when the whole line sits inside a block comment.
# @param inBlockCommentOut Set to whether a block comment is still open when the line ends.
function(fastcached_strip_comment_line line inBlockComment strippedOut skipOut inBlockCommentOut)
    # The fast path, and the reason the scan below is affordable: a line carrying neither `//`
    # nor `/*`, outside a block comment, is already code. That is almost every line of the
    # ~180k a full-tree walk reads, and it costs one regex.
    if(NOT inBlockComment AND NOT line MATCHES "/[/*]")
        set(${strippedOut} "${line}" PARENT_SCOPE)
        set(${skipOut} FALSE PARENT_SCOPE)
        set(${inBlockCommentOut} FALSE PARENT_SCOPE)
        return()
    endif()

    set(code "")
    set(rest "${line}")
    set(open "${inBlockComment}")
    # A line that BEGAN inside a block comment and never leaves it is the whole-line-comment
    # case the callers skip. Cleared the moment a `*/` is consumed, which is what the old
    # `MATCHES "\\*/"` test answered.
    set(skipLine "${inBlockComment}")
    while(TRUE)
        string(LENGTH "${rest}" restLength)
        if(open)
            string(FIND "${rest}" "*/" closeAt)
            if(closeAt EQUAL -1)
                break()
            endif()
            # A space in place of the comment, so it cannot JOIN the tokens it sat between:
            # `int/*x*/y` must not read as one identifier. A joined token is a match this
            # reader invented, and the old greedy `REGEX REPLACE` substituted a space for
            # exactly this reason.
            string(APPEND code " ")
            set(open FALSE)
            set(skipLine FALSE)
            math(EXPR after "${closeAt} + 2")
            if(after GREATER_EQUAL restLength)
                break()
            endif()
            string(SUBSTRING "${rest}" ${after} -1 rest)
            continue()
        endif()

        # Outside a comment, whichever of a QUOTE, a `//` and a `/*` comes FIRST decides.
        #
        # The quote is in that race because a comment introducer inside a string literal is
        # not a comment introducer. Leaving it out is what let `"Accept: */*"` open a block
        # comment nothing closed, hiding the rest of that file from a clean-reporting scan.
        #
        # That the other two are decided POSITIONALLY rather than by stripping `//` first is
        # older and has its own history: the `/*` test used to run first, so a line comment
        # MENTIONING `/*` opened a comment no `*/` ever closed -- the same false green, two
        # steps in. Third copy of it: `check-cli-text-cell.cmake` and
        # `check-markup-entities.cmake` carried it too, in walks of their own.
        string(FIND "${rest}" "\"" quoteAt)
        string(FIND "${rest}" "//" lineAt)
        string(FIND "${rest}" "/*" blockAt)
        set(first -1)
        set(kind "none")
        if(NOT quoteAt EQUAL -1)
            set(first ${quoteAt})
            set(kind "quote")
        endif()
        if(NOT lineAt EQUAL -1 AND (first EQUAL -1 OR lineAt LESS first))
            set(first ${lineAt})
            set(kind "line")
        endif()
        if(NOT blockAt EQUAL -1 AND (first EQUAL -1 OR blockAt LESS first))
            set(first ${blockAt})
            set(kind "block")
        endif()

        if(kind STREQUAL "none")
            string(APPEND code "${rest}")
            break()
        endif()

        string(SUBSTRING "${rest}" 0 ${first} head)
        string(APPEND code "${head}")

        if(kind STREQUAL "line")
            break()
        endif()

        if(kind STREQUAL "block")
            set(open TRUE)
            math(EXPR after "${first} + 2")
            if(after GREATER_EQUAL restLength)
                break()
            endif()
            string(SUBSTRING "${rest}" ${after} -1 rest)
            continue()
        endif()

        # A string literal. Copy it through, closing quote included, and resume after it.
        fastcached_literal_end("${rest}" ${first} endAt)
        if(endAt EQUAL -1)
            # It does not end on this line: a raw string's first line, or a continuation. KEEP
            # the rest as code. A reader that keeps text can only produce a finding somebody
            # sees and exempts; one that drops it reports clean over what it never read, which
            # is the failure this whole function was rewritten for.
            string(SUBSTRING "${rest}" ${first} -1 tail)
            string(APPEND code "${tail}")
            break()
        endif()
        math(EXPR through "${endAt} + 1")
        math(EXPR span "${through} - ${first}")
        string(SUBSTRING "${rest}" ${first} ${span} literal)
        string(APPEND code "${literal}")
        if(through GREATER_EQUAL restLength)
            break()
        endif()
        string(SUBSTRING "${rest}" ${through} -1 rest)
    endwhile()
    set(${strippedOut} "${code}" PARENT_SCOPE)
    set(${skipOut} "${skipLine}" PARENT_SCOPE)
    set(${inBlockCommentOut} "${open}" PARENT_SCOPE)
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
# **A line with no comment introducer is copied with its neighbours, not on its own.** Outside
# a block comment, every whole line before the next one carrying `//` or `/*` is a line
# `fastcached_strip_comment_line` would hand back unchanged -- its own fast path -- so the run
# is appended in ONE step, with the per-line `\r` removal done as `\r\n` to `\n` over the run.
# Walking them one at a time cost a loop turn, a function call and two copies of the rest of
# the file per line, and that was most of what `test-loops` spent on the tree: measured, the
# walk over every scoped file took it to 92 s of its 180 s budget on a loaded host. Only lines
# carrying an introducer take the per-line path, which is unchanged.
#
# @param content The file content.
# @param outVar Set to the content with comments removed and newlines preserved.
function(fastcached_strip_comments content outVar)
    set(code "")
    set(rest "${content}")
    set(inBlockComment FALSE)
    while(TRUE)
        if(NOT inBlockComment)
            string(FIND "${rest}" "//" introducerAt)
            string(FIND "${rest}" "/*" blockIntroducerAt)
            if(introducerAt EQUAL -1 OR (NOT blockIntroducerAt EQUAL -1 AND blockIntroducerAt LESS introducerAt))
                set(introducerAt "${blockIntroducerAt}")
            endif()
            if(introducerAt EQUAL -1)
                # Nothing left that could open a comment: the rest is code, line ends and all.
                string(REPLACE "\r\n" "\n" rest "${rest}")
                string(REGEX REPLACE "\r$" "" rest "${rest}")
                string(APPEND code "${rest}")
                break()
            endif()
            string(SUBSTRING "${rest}" 0 ${introducerAt} runHead)
            string(FIND "${runHead}" "\n" runEnd REVERSE)
            if(NOT runEnd EQUAL -1)
                math(EXPR runEnd "${runEnd} + 1")
                string(SUBSTRING "${rest}" 0 ${runEnd} run)
                string(REPLACE "\r\n" "\n" run "${run}")
                string(APPEND code "${run}")
                string(SUBSTRING "${rest}" ${runEnd} -1 rest)
            endif()
            # A line that is ONLY a line comment -- the doc comment on nearly every declaration
            # -- strips to its indentation: the `//` is the first introducer and nothing stands
            # before it, which is what `fastcached_strip_comment_line` returns for it. Answered
            # here because the function CALL was the largest single cost of the walk.
            if(rest MATCHES "^([ \t]*)//")
                string(APPEND code "${CMAKE_MATCH_1}")
                string(FIND "${rest}" "\n" newline)
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
                continue()
            endif()
        endif()
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
## @param CONTAINING Literal strings; with CONTAINING_OUT, which files of FILES_OUT hold any of them.
## @param CONTAINING_BYTES Bytes, as two lowercase hex digits from `00` to `7f`; with
##        CONTAINING_OUT, which files of FILES_OUT hold any of them, judged on the true bytes.
## @param CONTAINING_OUT Name of the variable to receive that subset, sorted. FILES_OUT is unchanged.
## @param MISSING_OUT Name of the variable to receive the files of FILES_OUT this tree does not
##        have -- named by the index and absent, or a link to nothing -- sorted. Never a file
##        that is there and unreadable. Without it, a CONTAINING question over such a file is
##        REFUSED by name, since leaving it out of the answer would pass it.
#
# ## CONTAINING: which files a check has to READ
#
# A scan whose rule can only fire on a line carrying some literal reads every file to find the
# few that do. On a DrvFs checkout -- which is where every local gate tree takes its sources
# from -- a read is a 9P round trip, and reading `src/` cost a check seconds a single `git grep`
# spends in under half of one; with two or three lanes gating at once those seconds are what
# took the hygiene checks past their budgets. So the question is asked ONCE, of git, in git mode:
# `git grep -l -F` over the same pathspecs, intersected with FILES_OUT so the FILTER holds. The
# walk has no index to ask and reads each file, which is what every caller did before.
#
# FILES_OUT stays the whole set, so a count a check prints is the count it always printed; a
# file outside CONTAINING_OUT is one whose contents could not have fired the rule. A `git grep`
# that fails, or exits 0 having said ANYTHING on stderr, is not trusted and every file is read
# instead: git exits 0 over a file it could not open, so the status alone would pass that file.
# A file nobody can read is refused by name, whichever path found it.
#
# ## CONTAINING_BYTES: a question about BYTES, which a text read cannot answer
#
# `file(READ)` is not a byte read. Measured: it drops the CR of a CRLF on Windows AND on Linux
# (a 30-byte file with two CRLFs reads as 28), ends at a 0x1A on Windows (nine bytes read as
# one), and while the string it returns HOLDS a NUL, the regex engine stops at one, so nothing
# behind a NUL is ever matched. Every one of those losses reports clean. So a byte needle is
# never looked for in text:
#
#   * git mode asks ONE `git grep -l -a -P` for a class of `\xHH` escapes, under `LC_ALL=C` so
#     PCRE matches bytes rather than code points -- and `-P` because a NUL cannot be passed in
#     an argument, which rules out `-F`. `-a` makes a file git calls binary searched as text,
#     all of it. A git built without PCRE cannot answer; that is SAID, and the walk's reading
#     answers instead, so a missing feature costs time and never a verdict.
#   * the walk reads each file as HEX, which is exact, and matches a token only at a byte
#     boundary (see `fastcached_files_holding_bytes`).
#
# Restricted to `00`-`7f` because that is where a byte and a code point are the same number in
# every locale either side could be in; nothing here needs more.
function(fastcached_tracked_files sourceDir)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "FILTER;FILES_OUT;MODE_OUT;CONTAINING_OUT;MISSING_OUT"
        "PATHSPECS;GLOBS;CONTAINING;CONTAINING_BYTES")

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

    # Both or neither: needles with nowhere to put the answer, or a place for an answer nothing
    # asked, is a call that does not say what it means.
    set(hasContaining FALSE)
    if(NOT "${arg_CONTAINING}" STREQUAL "" OR NOT "${arg_CONTAINING_BYTES}" STREQUAL "")
        set(hasContaining TRUE)
    endif()
    if(hasContaining AND "${arg_CONTAINING_OUT}" STREQUAL "")
        message(FATAL_ERROR "fastcached_tracked_files: CONTAINING needs CONTAINING_OUT to answer into")
    endif()
    if(NOT hasContaining AND NOT "${arg_CONTAINING_OUT}" STREQUAL "")
        message(FATAL_ERROR "fastcached_tracked_files: CONTAINING_OUT names no CONTAINING to answer")
    endif()
    set(hasMissing FALSE)
    if(NOT "${arg_MISSING_OUT}" STREQUAL "")
        set(hasMissing TRUE)
    endif()
    foreach(token IN LISTS arg_CONTAINING_BYTES)
        if(NOT token MATCHES "^[0-7][0-9a-f]$")
            # An uppercase digit would never match the lowercase hex a read produces, and a
            # byte above 7f is a different number to PCRE in a UTF-8 locale: both would be a
            # needle that silently finds nothing.
            message(FATAL_ERROR
                "fastcached_tracked_files: CONTAINING_BYTES takes two lowercase hex digits from "
                "00 to 7f, and '${token}' is not one")
        endif()
    endforeach()

    set(found "")
    set(pathspecArguments "")
    set(mode "")

    fastcached_work_tree_state("${sourceDir}" workTreeState)
    if(workTreeState STREQUAL "work-tree")
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

        # A directory the walk cannot LIST is one whose files it never finds: `file(GLOB_RECURSE)`
        # skips it without a word, so everything under it is left out of FILES_OUT -- every file
        # PASSED, for every caller, whatever it asked. Measured on ext4 under chmod 000: the
        # directory itself is listed as an entry and `EXISTS` (`access(R_OK)`) calls it absent,
        # which is the signal. So each pattern's fixed directory prefix is walked for directories
        # and one this process cannot read is REFUSED by name. git mode needs none of this: the
        # index names every file whatever the directory's permissions.
        #
        # Blind spot, failing OPEN: on Windows neither `EXISTS` nor `IS_READABLE` sees an access
        # list, and a glob has no error channel, so a deny-list directory in a WALK there is still
        # skipped. The walk is the no-index case -- a source export -- and CI's Windows legs run
        # in git mode.
        set(unlistable "")
        foreach(pattern IN LISTS arg_GLOBS)
            # The pattern's directory part up to its first wildcard: `src/*` walks `src`, `*` the root.
            string(REGEX REPLACE "/?[^/]*[*?[].*$" "" prefix "${pattern}")
            set(prefixPath "${sourceDir}")
            if(NOT prefix STREQUAL "")
                set(prefixPath "${sourceDir}/${prefix}")
            endif()
            if(NOT IS_DIRECTORY "${prefixPath}")
                continue()
            endif()
            # The prefix itself first: unreadable, it lists as nothing at all.
            if(NOT EXISTS "${prefixPath}")
                if(prefix STREQUAL "")
                    set(prefix ".")
                endif()
                list(APPEND unlistable "${prefix}")
                continue()
            endif()
            file(GLOB_RECURSE entries LIST_DIRECTORIES true RELATIVE "${sourceDir}" "${prefixPath}/*")
            foreach(entry IN LISTS entries)
                if(NOT IS_DIRECTORY "${sourceDir}/${entry}" OR EXISTS "${sourceDir}/${entry}")
                    continue()
                endif()
                set(excluded FALSE)
                foreach(name IN LISTS excludeNames)
                    if("${entry}/" MATCHES "(^|/)${name}/")
                        set(excluded TRUE)
                        break()
                    endif()
                endforeach()
                if(NOT excluded)
                    list(APPEND unlistable "${entry}")
                endif()
            endforeach()
        endforeach()
        if(unlistable)
            list(REMOVE_DUPLICATES unlistable)
            list(SORT unlistable)
            list(JOIN unlistable ", " unlistableNames)
            message(FATAL_ERROR
                "fastcached_tracked_files: cannot list a directory -- ${unlistableNames} in "
                "${sourceDir}: the walk finds no file inside a directory it cannot read, so every "
                "file under it would be left out of the answer, which is a file passed. Make it "
                "readable and ask again.")
        endif()
    endif()

    list(REMOVE_DUPLICATES found)
    list(SORT found)

    # EVERY file a question is asked about is ACCOUNTED FOR: judged, reported missing, or refused
    # by name. A file silently left out of an answer is a file PASSED, and three states did that
    # here while every check read clean -- measured by planting `::htonl(` in a chmod-000
    # `Endian.hpp`: the per-file read before this seam refused, the seam exited 0.
    #
    #   * `git grep` exits 0 over a file it cannot open. It says so on stderr only
    #     (`error: failed to stat 'X': Permission denied`, on ext4 under chmod 000 and on Windows
    #     under a deny-read access list alike) and leaves the file out of `-l` and `-L` both.
    #   * It skips a tracked file deleted from the work tree without a word, stderr included.
    #   * It never searches a symlink, so a link to nothing was read, found absent, and skipped.
    #
    # So: any stderr from a search makes that search UNTRUSTED and the question is answered by
    # reading every file; a file the tree lacks (deleted, or a link to nothing) is
    # MISSING_OUT's answer when the caller asked that, and refused otherwise; and a file that is
    # there and cannot be read is refused whatever was asked -- never passed, and never MISSING,
    # which is a statement about the tree that would be false.
    #
    # An unreadable file is told from an absent one by the DIRECTORY, not the file: `EXISTS` is
    # `access(R_OK)` on POSIX and so calls an unreadable file absent, and `file(TIMESTAMP)` answers
    # empty for both. A literal `file(GLOB)` lists names out of the directory, which needs no
    # access to the file itself (see `fastcached_classify_unopened`).
    #
    # A MISSING-only question reads no contents, so there an unreadable file is simply PRESENT,
    # which is true; it is refused only where a verdict about its contents was asked for.
    set(links "")
    set(absent "")
    set(unreadable "")
    set(needsAccounting FALSE)
    if(hasContaining OR hasMissing)
        set(needsAccounting TRUE)
    endif()
    if(mode STREQUAL "git ls-files" AND needsAccounting)
        # git searches REGULAR files only: `git grep` skips a tracked symlink outright, neither
        # searching what it points at nor listing it with `-L`, where the per-file read every
        # caller did before followed the link. So the symlinks are set apart once, from the
        # index's modes, and every question below answers them by reading.
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${sourceDir}" ls-files -s ${pathspecArguments}
            OUTPUT_VARIABLE staged
            RESULT_VARIABLE stagedStatus)
        if(NOT stagedStatus EQUAL 0)
            message(FATAL_ERROR
                "fastcached_tracked_files: `git ls-files -s` could not read the index of "
                "${sourceDir} (status ${stagedStatus}), so which files are symlinks is unknown")
        endif()
        string(REGEX MATCHALL "(^|\n)120000 [^\t\n]*\t[^\n]*" linkRows "${staged}")
        foreach(linkRow IN LISTS linkRows)
            string(REGEX REPLACE "^\n?120000 [^\t]*\t" "" linkPath "${linkRow}")
            if(linkPath IN_LIST found)
                list(APPEND links "${linkPath}")
            endif()
        endforeach()
        list(REMOVE_DUPLICATES links)

        # Which files git can OPEN: `-L` with a pattern that never matches lists every regular
        # file it read, an empty one included, which no matching pattern would reach. A file of
        # the set outside that listing -- and not a link, which git never reads -- is the only
        # kind asked on its own, so the per-file cost is paid for the files in doubt and not
        # for the tree. Measured over DrvFs for `src/*` (903 files): 0.46 s for this, against
        # 1.60 s for `git ls-files -d`, whose `lstat`s are serial where the search is threaded.
        # The listing is untrusted like any search: stderr sends every file to be asked.
        fastcached_git_grep_pcre("${sourceDir}" -L "(?!)" "${pathspecArguments}" "${found}"
            openable openAnswered openNamed)
        list(APPEND unreadable ${openNamed})
        if(openAnswered)
            set(unlisted "${found}")
            foreach(listed IN LISTS openable links unreadable)
                list(REMOVE_ITEM unlisted "${listed}")
            endforeach()
            set(askEach ${links} ${unlisted})
        else()
            set(askEach "${found}")
        endif()
    elseif(needsAccounting)
        # A walk FOUND each file in a directory listing, so only one it cannot open is in doubt.
        set(askEach "${found}")
    else()
        set(askEach "")
    endif()
    foreach(candidate IN LISTS askEach)
        if(NOT EXISTS "${sourceDir}/${candidate}" AND NOT candidate IN_LIST unreadable)
            fastcached_classify_unopened("${sourceDir}" "${candidate}" why)
            list(APPEND ${why} "${candidate}")
        endif()
    endforeach()

    if(hasContaining)
        set(containing "")
        if(NOT "${arg_CONTAINING}" STREQUAL "")
            set(readText "${found}")
            if(mode STREQUAL "git ls-files")
                set(needleArguments "")
                foreach(needle IN LISTS arg_CONTAINING)
                    list(APPEND needleArguments -e "${needle}")
                endforeach()
                execute_process(
                    COMMAND "${GIT_EXECUTABLE}" -C "${sourceDir}" grep -l -F --no-color
                            ${needleArguments} ${pathspecArguments}
                    OUTPUT_VARIABLE grepped
                    ERROR_VARIABLE grepError
                    RESULT_VARIABLE grepStatus
                    OUTPUT_STRIP_TRAILING_WHITESPACE)
                fastcached_search_verdict("grep -l -F" "${sourceDir}" "${grepStatus}" "${grepError}"
                    "${found}" textAnswered textNamed)
                list(APPEND unreadable ${textNamed})
                if(textAnswered)
                    string(REPLACE "\n" ";" grepFiles "${grepped}")
                    foreach(candidate IN LISTS grepFiles)
                        if(candidate IN_LIST found)
                            list(APPEND containing "${candidate}")
                        endif()
                    endforeach()
                    set(readText "${links}")
                endif()
            endif()
            foreach(unjudgeable IN LISTS absent unreadable)
                list(REMOVE_ITEM readText "${unjudgeable}")
            endforeach()
            fastcached_files_holding_text("${sourceDir}" "${readText}" "${arg_CONTAINING}"
                holdingText unopenedText)
            list(APPEND containing ${holdingText})
            foreach(candidate IN LISTS unopenedText)
                fastcached_classify_unopened("${sourceDir}" "${candidate}" why)
                list(APPEND ${why} "${candidate}")
            endforeach()
        endif()

        if(NOT "${arg_CONTAINING_BYTES}" STREQUAL "")
            set(readBytes "${found}")
            if(mode STREQUAL "git ls-files")
                set(byteClass "")
                foreach(token IN LISTS arg_CONTAINING_BYTES)
                    string(APPEND byteClass "\\x${token}")
                endforeach()
                fastcached_git_grep_pcre("${sourceDir}" -l "[${byteClass}]" "${pathspecArguments}"
                    "${found}" grepFiles grepAnswered bytesNamed)
                list(APPEND unreadable ${bytesNamed})
                if(grepAnswered)
                    foreach(candidate IN LISTS grepFiles)
                        if(candidate IN_LIST found)
                            list(APPEND containing "${candidate}")
                        endif()
                    endforeach()
                    set(readBytes "${links}")
                endif()
            endif()
            foreach(unjudgeable IN LISTS absent unreadable)
                list(REMOVE_ITEM readBytes "${unjudgeable}")
            endforeach()
            fastcached_files_holding_bytes("${sourceDir}" "${readBytes}" "${arg_CONTAINING_BYTES}"
                holdingBytes unopenedBytes)
            list(APPEND containing ${holdingBytes})
            foreach(candidate IN LISTS unopenedBytes)
                fastcached_classify_unopened("${sourceDir}" "${candidate}" why)
                list(APPEND ${why} "${candidate}")
            endforeach()
        endif()

        list(REMOVE_DUPLICATES unreadable)
        list(SORT unreadable)
        if(unreadable)
            list(JOIN unreadable ", " unreadableNames)
            message(FATAL_ERROR
                "fastcached_tracked_files: cannot judge a file it cannot read -- ${unreadableNames} "
                "in ${sourceDir}: tracked, present, and not readable by this process (its "
                "permissions or an access list). A file left out of the answer is a file passed, so "
                "this is refused rather than reported clean; make it readable and ask again.")
        endif()
        list(REMOVE_DUPLICATES absent)
        list(SORT absent)
        if(absent AND NOT hasMissing)
            list(JOIN absent ", " absentNames)
            message(FATAL_ERROR
                "fastcached_tracked_files: cannot judge a file it cannot read -- ${absentNames} "
                "in ${sourceDir}: named by the index and not in the work tree (deleted, or a link "
                "to nothing). A file left out of the answer is a file passed, so this is refused "
                "rather than reported clean; restore it, or commit its removal.")
        endif()
        list(REMOVE_DUPLICATES containing)
        list(SORT containing)
        set(${arg_CONTAINING_OUT} "${containing}" PARENT_SCOPE)
    endif()

    if(hasMissing)
        # Which files of the set this tree does not have: the index names them and the checkout
        # lacks them, or a link names nothing. Never a file that is there and unreadable. A
        # per-file `EXISTS` is a round trip each over DrvFs -- measured, 1353 of them cost 15-86 s
        # on a loaded host -- so git mode asks only the files its `-L` listing did not name.
        list(REMOVE_DUPLICATES absent)
        list(SORT absent)
        set(${arg_MISSING_OUT} "${absent}" PARENT_SCOPE)
    endif()

    set(${arg_FILES_OUT} "${found}" PARENT_SCOPE)
    set(${arg_MODE_OUT} "${mode}" PARENT_SCOPE)
endfunction()

## Whether one `git grep` can be trusted, and which files of the set its stderr named.
## @param what The search, for the message: `grep -l -F`.
## @param sourceDir The repository root.
## @param status Its exit status.
## @param stderr Its standard error.
## @param found The file set, so only its own files are named.
## @param outAnswered Name of the variable set TRUE when the answer can be used as given.
## @param outNamed Name of the variable to receive the set's files the stderr quotes.
#
# Trusted means status 0 or 1 AND nothing on stderr. The status alone is not enough: git exits 0
# over a file it could not open, which is the defect this exists for. Any stderr at all is read
# as "some file may be missing from this answer", and the caller reads every file instead -- a
# benign line somebody's git prints costs time, never a verdict. A file the stderr QUOTES is one
# git could not open; naming it here is what lets a Windows access list, which `EXISTS` cannot
# see, be refused in this seam's words rather than by a failing `file(READ)`.
function(fastcached_search_verdict what sourceDir status stderr found outAnswered outNamed)
    set(named "")
    string(REGEX MATCHALL "'[^'\n]+'" quoted "${stderr}")
    foreach(quote IN LISTS quoted)
        string(REGEX REPLACE "^'(.*)'$" "\\1" quote "${quote}")
        if(quote IN_LIST found)
            list(APPEND named "${quote}")
        endif()
    endforeach()
    set(answered TRUE)
    if(NOT (status EQUAL 0 OR status EQUAL 1) OR NOT "${stderr}" STREQUAL "")
        string(STRIP "${stderr}" stderr)
        message(STATUS
            "fastcached_tracked_files: `git ${what}` did not answer cleanly for ${sourceDir} "
            "(status ${status}: ${stderr}), so this question is answered by reading every file "
            "instead -- exact, and slower")
        set(answered FALSE)
    endif()
    set(${outAnswered} "${answered}" PARENT_SCOPE)
    set(${outNamed} "${named}" PARENT_SCOPE)
endfunction()

## Why a file of the set could not be opened.
## @param sourceDir The repository root.
## @param candidate The path, relative to it.
## @param outVar Name of the variable to receive `absent` or `unreadable` -- the name of the list
##        the caller appends it to.
#
# `absent` is a file the directory does not list, or a link whose target it does not list;
# `unreadable` is one it lists and this process cannot open. The directory answers because the
# file cannot: `EXISTS` is `access(R_OK)` on POSIX, and `file(TIMESTAMP)` is empty for both. A
# literal `file(GLOB)` matches names read out of the directory, so it lists an unreadable file
# and a dangling link, and not a deleted file -- measured on ext4 under chmod 000. The glob's own
# metacharacters are bracketed so a path is matched as itself.
#
# A link is followed to the END of its chain, never one hop: a link to a link to nothing names a
# directory entry that IS listed -- the middle link -- so stopping there calls it `unreadable`.
# Bounded at 40 hops, Linux's own `MAXSYMLINKS`; a chain longer than that, or a cycle, is one the
# kernel refuses to open with `ELOOP`, which is a link to nothing reachable -- `absent`.
function(fastcached_classify_unopened sourceDir candidate outVar)
    set(path "${sourceDir}/${candidate}")
    foreach(hop RANGE 1 40)
        if(NOT IS_SYMLINK "${path}")
            break()
        endif()
        file(READ_SYMLINK "${path}" target)
        if(NOT IS_ABSOLUTE "${target}")
            get_filename_component(linkDirectory "${path}" DIRECTORY)
            set(target "${linkDirectory}/${target}")
        endif()
        set(path "${target}")
    endforeach()
    if(IS_SYMLINK "${path}")
        set(${outVar} "absent" PARENT_SCOPE)
        return()
    endif()
    string(REGEX REPLACE "([*?[])" "[\\1]" pattern "${path}")
    file(GLOB listed LIST_DIRECTORIES true "${pattern}")
    if(listed)
        set(${outVar} "unreadable" PARENT_SCOPE)
    else()
        set(${outVar} "absent" PARENT_SCOPE)
    endif()
endfunction()

## One `git grep -a -P` over a file set, in bytes.
## @param sourceDir The repository root.
## @param listing `-l` for the files that match, `-L` for the files that do not.
## @param pattern The PCRE pattern.
## @param pathspecArguments `--` and the pathspecs, or empty.
## @param found The file set, so only its own files are named.
## @param outFiles Name of the variable to receive the listed files.
## @param outAnswered Name of the variable set to TRUE when git's answer can be used as given.
## @param outNamed Name of the variable to receive the set's files git's stderr quotes.
#
# `LC_ALL=C` so PCRE matches bytes rather than code points. A git that cannot run the search --
# one built without PCRE refuses `-P` -- or that says anything at all on stderr is not trusted
# (`fastcached_search_verdict`), and the caller answers by reading instead: a missing feature
# costs time and never a verdict, where a search that failed read as one that found nothing
# would cost the verdict.
function(fastcached_git_grep_pcre sourceDir listing pattern pathspecArguments found outFiles outAnswered outNamed)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env LC_ALL=C
                "${GIT_EXECUTABLE}" -C "${sourceDir}" grep ${listing} -a -P --no-color
                -e "${pattern}" ${pathspecArguments}
        OUTPUT_VARIABLE grepped
        ERROR_VARIABLE grepError
        RESULT_VARIABLE grepStatus
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    fastcached_search_verdict("grep ${listing} -P" "${sourceDir}" "${grepStatus}" "${grepError}"
        "${found}" answered named)
    set(files "")
    if(answered AND grepStatus EQUAL 0)
        string(REPLACE "\n" ";" files "${grepped}")
    endif()
    set(${outFiles} "${files}" PARENT_SCOPE)
    set(${outAnswered} "${answered}" PARENT_SCOPE)
    set(${outNamed} "${named}" PARENT_SCOPE)
endfunction()

## Which files hold any of some literal strings, read one at a time.
## @param sourceDir The repository root.
## @param files Paths relative to it.
## @param needles The literals.
## @param outVar Name of the variable to receive the files that hold one, in the order given.
## @param outUnopened Name of the variable to receive the files that could not be opened, which
##        the caller accounts for -- never skipped here, since a skipped file is a passed one.
##        The seam classifies every file in doubt BEFORE reading, so this is reached only by a
##        file that changed in between; neutering it turns no self-test case red, and that is
##        why: it is the race's guard, not a second classifier.
function(fastcached_files_holding_text sourceDir files needles outVar outUnopened)
    set(holding "")
    set(unopened "")
    foreach(candidate IN LISTS files)
        if(IS_DIRECTORY "${sourceDir}/${candidate}")
            continue()
        endif()
        if(NOT EXISTS "${sourceDir}/${candidate}")
            list(APPEND unopened "${candidate}")
            continue()
        endif()
        file(READ "${sourceDir}/${candidate}" candidateContent)
        foreach(needle IN LISTS needles)
            string(FIND "${candidateContent}" "${needle}" needleAt)
            if(NOT needleAt EQUAL -1)
                list(APPEND holding "${candidate}")
                break()
            endif()
        endforeach()
    endforeach()
    set(${outVar} "${holding}" PARENT_SCOPE)
    set(${outUnopened} "${unopened}" PARENT_SCOPE)
endfunction()

## Which files hold any of some bytes, read as HEX so every byte is seen.
## @param sourceDir The repository root.
## @param files Paths relative to it.
## @param tokens The bytes, as two lowercase hex digits each.
## @param outVar Name of the variable to receive the files that hold one, in the order given.
## @param outUnopened Name of the variable to receive the files that could not be opened, which
##        the caller accounts for, as `fastcached_files_holding_text` does.
#
# A token searched in raw hex also matches ACROSS a byte boundary -- `a0 7b` contains `07` --
# so the raw search only says where to look. A file with a raw hit is spaced, a blank after
# every pair, and the token is searched with its blank: a three-character needle whose third
# character is a blank can only start at a multiple of three, which is a whole byte.
function(fastcached_files_holding_bytes sourceDir files tokens outVar outUnopened)
    set(holding "")
    set(unopened "")
    foreach(candidate IN LISTS files)
        if(IS_DIRECTORY "${sourceDir}/${candidate}")
            continue()
        endif()
        if(NOT EXISTS "${sourceDir}/${candidate}")
            list(APPEND unopened "${candidate}")
            continue()
        endif()
        file(READ "${sourceDir}/${candidate}" hex HEX)
        set(spaced "")
        foreach(token IN LISTS tokens)
            string(FIND "${hex}" "${token}" rawAt)
            if(rawAt EQUAL -1)
                continue()
            endif()
            if(spaced STREQUAL "")
                string(REGEX REPLACE "(..)" "\\1 " spaced "${hex}")
            endif()
            string(FIND "${spaced}" "${token} " at)
            if(NOT at EQUAL -1)
                list(APPEND holding "${candidate}")
                break()
            endif()
        endforeach()
    endforeach()
    set(${outVar} "${holding}" PARENT_SCOPE)
    set(${outUnopened} "${unopened}" PARENT_SCOPE)
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
