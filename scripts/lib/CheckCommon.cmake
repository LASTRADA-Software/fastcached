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
