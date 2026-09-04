# SPDX-License-Identifier: Apache-2.0
#
# A binary's shipped reference configuration must name every setting a
# configuration file can carry, and no setting it cannot.
#
# The option table is the single source of truth for what a file accepts -- the
# `yamlKey` column drives the parse -- and the shipped YAML is what an operator
# actually reads. Nothing links the two. A flag added with a key and no line in the
# reference is a setting that works and that nobody can find; a line in the
# reference for a key the table dropped is worse, because an operator writes it,
# restarts, and the binary refuses to start naming a key it has never heard of.
#
# Neither direction shows up in any build or test: the flag parses, the file
# parses, and the two simply describe different products.
#
# ## Why this takes its files as PARAMETERS
#
# `check-node-config-reference.cmake` asks exactly this question about
# `fastcache-compile-node`, and its own header says why it must be asked -- every
# word of which is true of the daemon, which had no such check and is the binary
# most operators actually run
# ([#759](https://github.com/LASTRADA-Software/fastcached/issues/759)). Two checks
# differing only in two paths is the copy-paste this project treats as a defect, so
# this one is parameterised from the start.
#
# It does NOT repoint the node's registration at this script, and that restraint is
# deliberate rather than laziness: consolidating them is a change to a guard another
# lane's work depends on, and `check-glob-traversals.cmake` records the precedent --
# absorbing a consolidation into the ticket in front of you closes one ticket by
# swallowing another. The follow-up is small BECAUSE this is parameterised: point
# `node-config-reference` here and delete the older script. Nothing else.
#
# ## What this check does NOT see: the VALUE a key ships with
#
# It compares key NAMES. A line's value shape is invisible to it, and for one kind
# of setting that is a real gap rather than a boundary: a BOOLEAN shipped as a bare
# `#key:` is a trap, because "uncomment to enable" is the natural reading and
# uncommenting alone yields a type error and a binary that refuses to start.
# Booleans therefore ship as `#key: false`. The same bare shape is CORRECT for a
# value-taking setting (`#log_level:`): uncommenting alone fails loudly and
# self-explanatorily. So this is not a rule that could simply be extended to every
# key, which is why it is recorded here for the next person adding a setting rather
# than encoded.
#
# ## The two patterns, stated beside the two figures
#
# A census states its PATTERN, not only its number -- two audits of one file set
# differing by one, neither having miscounted, is what that rule is made of. So,
# exactly:
#
#   table keys      `\.yamlKey = "[a-z_]+"` over the table source, deduplicated.
#   reference keys  `^#([a-z_]+):` over the reference, one line each, NOT
#                   deduplicated (duplicates are their own refusal below).
#
# The reference pattern deliberately does not match a key mentioned in prose, and
# deliberately DOES match a commented key carrying an example value (`#log_source:
# false`) -- that line IS the commented setting, not a second mention of one. A
# looser `^#[a-z_]+:` counted without `sort -u` inflates the figure by re-counting
# nothing; that near-miss is why the pattern is written down here.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir>
#         -DFASTCACHED_TABLE=<path relative to source dir>
#         -DFASTCACHED_REFERENCE=<path relative to source dir>
#         -DFASTCACHED_LABEL=<name used in messages>
#         -P scripts/check-config-reference.cmake
#
# Exit codes: 0 = the two agree. 1 = at least one key is in one and not the other,
# or a scan matched nothing, or the reference documents a key twice.

# A `cmake -P` script has no project, so every policy starts unset and CMP0057
# (`if(... IN_LIST ...)`) is one of them -- unset, the operator is an unknown
# argument and the script errors out rather than answering.
cmake_minimum_required(VERSION 3.28)

foreach(_required FASTCACHED_SOURCE_DIR FASTCACHED_TABLE FASTCACHED_REFERENCE FASTCACHED_LABEL)
    if(NOT DEFINED ${_required})
        message(FATAL_ERROR "check-config-reference: ${_required} must be set")
    endif()
endforeach()

set(_table "${FASTCACHED_SOURCE_DIR}/${FASTCACHED_TABLE}")
set(_reference "${FASTCACHED_SOURCE_DIR}/${FASTCACHED_REFERENCE}")

foreach(_file "${_table}" "${_reference}")
    if(NOT EXISTS "${_file}")
        message(FATAL_ERROR "check-config-reference[${FASTCACHED_LABEL}]: ${_file} does not exist")
    endif()
endforeach()

# The table's keys, from the column itself rather than from a restatement of it.
file(READ "${_table}" _tableText)
string(REGEX MATCHALL "\\.yamlKey = \"[a-z_]+\"" _matches "${_tableText}")
set(_tableKeys "")
foreach(_match IN LISTS _matches)
    string(REGEX REPLACE "^\\.yamlKey = \"([a-z_]+)\"$" "\\1" _key "${_match}")
    list(APPEND _tableKeys "${_key}")
endforeach()
# Deduplicated because two rows may legitimately share one key -- the daemon's
# `--listen`/`--listen-tls` both answer to `listeners:` -- so a raw count of
# occurrences is not a count of settings. That is the other half of the census
# rule: the same file gives two different figures under two patterns, and only one
# of them is the population this check is about.
list(REMOVE_DUPLICATES _tableKeys)
list(SORT _tableKeys)

# A key the reference is claimed to document is a line that would BECOME a setting
# the moment its `#` is deleted -- `#key:` at the start of a line, which is the
# shape the whole file is written in. Prose mentioning a key in a sentence is not a
# documented setting and must not count as one, or the check passes on a file whose
# header happens to name everything.
#
# Read whole and split by hand, never `file(STRINGS)`: that call returns a CMake
# LIST, and an UNBALANCED `[` or `]` on a KEPT line merges that element with the
# ones after it, so everything past the bracket vanishes from the scan.
#
# Blanking the brackets is safe HERE and is not safe everywhere: this reader
# matches `^#([a-z_]+):`, a commented YAML key, which contains none. Where brackets
# are the data -- `check-tsan-scope`, whose rows are Catch2 tags like `[async]` --
# blanking them makes that check refuse on a good tree. Measured, not assumed.
#
# Another copy of this idiom; consolidating them is
# [#495](https://github.com/LASTRADA-Software/fastcached/issues/495) and is
# deliberately not done here.
file(READ "${_reference}" _referenceText)
string(REPLACE "\\" " " _referenceText "${_referenceText}")
string(REPLACE ";" " " _referenceText "${_referenceText}")
string(REPLACE "[" " " _referenceText "${_referenceText}")
string(REPLACE "]" " " _referenceText "${_referenceText}")
string(REGEX REPLACE "\r?\n" ";" _lines "${_referenceText}")
set(_referenceKeys "")
foreach(_line IN LISTS _lines)
    if(_line MATCHES "^#([a-z_]+):")
        list(APPEND _referenceKeys "${CMAKE_MATCH_1}")
    endif()
endforeach()
list(SORT _referenceKeys)

# Duplicates first: two commented blocks for one key means whichever an operator
# uncomments second is a duplicate YAML mapping key, which yaml-cpp resolves
# silently by keeping one. The operator gets half of what they wrote and no error
# -- which is the same silent-misconfiguration shape this whole check exists for,
# reached from inside the reference rather than between it and the table.
set(_deduped ${_referenceKeys})
list(REMOVE_DUPLICATES _deduped)
if(NOT "${_deduped}" STREQUAL "${_referenceKeys}")
    message(FATAL_ERROR
        "check-config-reference[${FASTCACHED_LABEL}]: ${_reference} documents a key more than once. "
        "Uncommenting both blocks would be a duplicate mapping key, which YAML resolves by "
        "silently keeping one -- so an operator following the file gets half of what they wrote "
        "and no error. Show the second case as more entries under the one key, not as a second "
        "block naming it again.")
endif()

set(_missing "")
foreach(_key IN LISTS _tableKeys)
    if(NOT _key IN_LIST _referenceKeys)
        list(APPEND _missing "${_key}")
    endif()
endforeach()

set(_stale "")
foreach(_key IN LISTS _referenceKeys)
    if(NOT _key IN_LIST _tableKeys)
        list(APPEND _stale "${_key}")
    endif()
endforeach()

# Emptiness is its own failure, and a separate assertion rather than an inference
# from "no differences found". A regex that stopped matching -- the column renamed,
# the file reformatted, a bracket merging every line into one element -- makes both
# lists empty, and two empty lists agree perfectly. That is a check reporting
# success for work it did not do.
list(LENGTH _tableKeys _tableCount)
list(LENGTH _referenceKeys _referenceCount)
if(_tableCount EQUAL 0 OR _referenceCount EQUAL 0)
    message(FATAL_ERROR
        "check-config-reference[${FASTCACHED_LABEL}]: read ${_tableCount} keys from the table and "
        "${_referenceCount} from the reference; one of the two scans matched nothing, so this "
        "check proved nothing rather than passing.")
endif()

if(_missing OR _stale)
    set(_report "")
    if(_missing)
        string(REPLACE ";" ", " _missingText "${_missing}")
        string(APPEND _report
            "\n  documented nowhere (an operator cannot find these): ${_missingText}")
    endif()
    if(_stale)
        string(REPLACE ";" ", " _staleText "${_stale}")
        string(APPEND _report
            "\n  documented but not accepted (uncommenting these refuses to start): ${_staleText}")
    endif()
    message(FATAL_ERROR
        "check-config-reference[${FASTCACHED_LABEL}]: ${FASTCACHED_TABLE} and ${FASTCACHED_REFERENCE} "
        "describe different products.${_report}")
endif()

message(STATUS
    "check-config-reference[${FASTCACHED_LABEL}]: ${_tableCount} settings, documented and accepted alike")
