# SPDX-License-Identifier: Apache-2.0
#
# The compile worker's shipped reference configuration must name every setting a
# configuration file can carry, and no setting it cannot.
#
# `NodeOptions()` is the single source of truth for what the file accepts -- the
# `yamlKey` column drives the parse -- and `packaging/linux/fastcache-compile-node.yaml`
# is what an operator actually reads. Nothing links the two. A flag added with a
# key and no line in the reference is a setting that works and that nobody can
# find; a line in the reference for a key the table dropped is worse, because an
# operator writes it, restarts, and the worker refuses to start naming a key it
# has never heard of.
#
# Neither direction shows up in any build or test: the flag parses, the file
# parses, and the two simply describe different products. So the check is a
# string comparison over the two files, which is what makes `cmake -P` the right
# tool -- see check-repository-hygiene.cmake for the longer argument. It needs no
# compiler, no daemon and no socket, and belongs in the default `ctest` set.
#
# ## What this check does NOT see: the VALUE a key ships with
#
# It compares key NAMES. A line's value shape is invisible to it, and for one kind
# of setting that is a real gap rather than a boundary: a BOOLEAN shipped as a bare
# `#key:` is a trap, because "uncomment to enable" is the natural reading and
# uncommenting alone yields `TypeMismatch: setting names no value` and a worker that
# refuses to start. Booleans therefore ship as `#key: false`, like their six
# siblings in the reference file -- and #485 shipped one that did not, which this
# check passed.
#
# The same bare shape is CORRECT for a value-taking setting (`#scheduler:`,
# `#log_level:`): uncommenting alone fails loudly and self-explanatorily, and the
# operator was always going to supply the value the prose above it describes. So
# this is not a rule that could simply be extended to every key, which is why it is
# recorded here for the next person adding a setting rather than encoded.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -P scripts/check-node-config-reference.cmake
#
# Exit codes: 0 = the two agree. 1 = at least one key is in one and not the other.

# A `cmake -P` script has no project, so every policy starts unset and CMP0057
# (`if(... IN_LIST ...)`) is one of them -- unset, the operator is an unknown
# argument and the script errors out rather than answering. Stated as a minimum
# version so the whole set moves together with the project's own.
cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()

set(_table "${FASTCACHED_SOURCE_DIR}/src/apps/fastcache-compile-node/NodeConfig.cpp")
set(_reference "${FASTCACHED_SOURCE_DIR}/packaging/linux/fastcache-compile-node.yaml")

foreach(_file "${_table}" "${_reference}")
    if(NOT EXISTS "${_file}")
        message(FATAL_ERROR "check-node-config-reference: ${_file} does not exist")
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
list(SORT _tableKeys)

# A key the reference is claimed to document is a line that would BECOME a
# setting the moment its `#` is deleted -- `#key:` at the start of a line, which
# is the shape the whole file is written in. Prose mentioning a key in a sentence
# is not a documented setting and must not count as one, or the check passes on a
# file whose header happens to name everything.
file(STRINGS "${_reference}" _lines)
set(_referenceKeys "")
foreach(_line IN LISTS _lines)
    if(_line MATCHES "^#([a-z_]+):")
        list(APPEND _referenceKeys "${CMAKE_MATCH_1}")
    endif()
endforeach()
list(SORT _referenceKeys)

# Duplicates first: two commented blocks for one key means whichever an operator
# uncomments second is a duplicate YAML key, which yaml-cpp resolves silently.
set(_deduped ${_referenceKeys})
list(REMOVE_DUPLICATES _deduped)
if(NOT "${_deduped}" STREQUAL "${_referenceKeys}")
    message(FATAL_ERROR
        "check-node-config-reference: ${_reference} documents a key more than once. "
        "Uncommenting both would be a duplicate mapping key, which YAML resolves by "
        "silently keeping one.")
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
# from "no differences found". A regex that stopped matching -- the column
# renamed, the file reformatted -- makes both lists empty, and two empty lists
# agree perfectly. That is a check reporting success for work it did not do.
list(LENGTH _tableKeys _tableCount)
list(LENGTH _referenceKeys _referenceCount)
if(_tableCount EQUAL 0 OR _referenceCount EQUAL 0)
    message(FATAL_ERROR
        "check-node-config-reference: read ${_tableCount} keys from the table and "
        "${_referenceCount} from the reference; one of the two scans matched nothing, "
        "so this check proved nothing rather than passing.")
endif()

if(_missing OR _stale)
    set(_report "")
    if(_missing)
        string(APPEND _report
            "\n  in NodeOptions() and NOT documented in the reference: ${_missing}"
            "\n    -> add a commented block for each to ${_reference}")
    endif()
    if(_stale)
        string(APPEND _report
            "\n  documented in the reference and NOT in NodeOptions(): ${_stale}"
            "\n    -> an operator who uncomments one of these gets a worker that refuses to start")
    endif()
    message(FATAL_ERROR "check-node-config-reference: the table and the shipped reference disagree.${_report}")
endif()

message(STATUS "check-node-config-reference: ${_tableCount} settings, table and reference agree")
