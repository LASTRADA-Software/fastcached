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
