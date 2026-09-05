# SPDX-License-Identifier: Apache-2.0
#
# Whether a set-but-EMPTY `FASTCACHE_ADDR` opts out — checked as a pure
# computation, over the one distinction a developer cannot easily stage.
#
# `docs/tools/fastcache-cc.md` promises that a set but empty `FASTCACHE_ADDR`
# means no caching. The launcher honours that when it reads its own environment;
# `cmake/portable/CompileCache.cmake` decides whether the launcher is used AT ALL,
# and decided it from `if("$ENV{FASTCACHE_ADDR}" STREQUAL "")` — true for an unset
# name and for an empty one alike, so `export FASTCACHE_ADDR=` was folded straight
# back into the default and kept caching
# ([#372](https://github.com/LASTRADA-Software/fastcached/issues/372)).
#
# ## Why a pure function rather than a configure
#
# The two inputs that matter differ ONLY in whether the environment defines the
# name — the value is the empty string either way. A test that exported an empty
# variable and configured would be measuring the SHELL as much as the module, and
# on PowerShell it could not express the case at all: `$env:X = ""` keeps the name
# in PowerShell's own provider view while the child process never receives it, so
# the module would correctly see "unset" and the fixture would be asserting the
# wrong thing while looking right. That trap is documented in
# `docs/tools/fastcache-cc.md` and is the reason this check drives the DECISION
# rather than the acquisition.
#
# So `_fc_resolve_addr` takes presence as a parameter and reads nothing, and the
# rows below state presence explicitly. The module's own single line of
# acquisition — `if(DEFINED ENV{FASTCACHE_ADDR})` — is what this cannot cover, and
# that is stated rather than implied.
#
# Verdict is read from the OUTPUT, not the exit code: `message(FATAL_ERROR)` exits
# 0 on CMake 3.28, this project's declared minimum. See src/tests/CMakeLists.txt,
# which registers this with FAIL_REGULAR_EXPRESSION.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()

# Under `cmake -P` the module returns after its pure computations, so this brings
# in the function without the launcher probing, the download and the daemon start.
include("${FASTCACHED_SOURCE_DIR}/cmake/portable/CompileCache.cmake")

if(NOT COMMAND _fc_resolve_addr)
    message(FATAL_ERROR
        "_fc_resolve_addr is not defined. Either the define-only guard in "
        "cmake/portable/CompileCache.cmake returned before the function, or the function was "
        "renamed and this check now proves nothing.")
endif()

# Split a '|'-separated row into named fields.
#
# The house splitter, copied byte-for-byte rather than re-derived. It never builds
# a CMake list, so a field containing ';', '\', '[' or ']' is harmless -- the
# bracket-vulnerable-reader hazard the rulebook records -- and it carries the
# malformed-row refusal itself, so a caller needs no field-count guard of its own.
# The LAST field may contain '|', which is what lets a row end in prose.
#
# The row LIST is a separate hazard and this cannot help with it: `set(rows "a;b")`
# splits at the list level before this is ever called. Keep ';' out of row text.
#
# Consolidating the copies is
# [#495](https://github.com/LASTRADA-Software/fastcached/issues/495), deliberately
# not pre-empted here. Keep this byte-for-byte with its siblings and count this
# file in when #495 lands.
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

# ---------------------------------------------------------------------------
# present | value | expected address | what this row is about
#
# The first two rows are the whole ticket: identical values, opposite answers,
# told apart by presence alone. `_positives` and `_negatives` are tallied below
# so a table that lost either of them cannot pass by having nothing to disagree.
set(_rows
    "OFF||127.0.0.1:6674|absent: the compiled-in default"
    "ON|||set but EMPTY: the documented opt-out, and the whole ticket"
    "ON|10.0.0.5:6674|10.0.0.5:6674|set to an address: taken verbatim"
    "OFF|ignored-when-absent|127.0.0.1:6674|a value is meaningless when the name is absent"
    "ON|127.0.0.1:6674|127.0.0.1:6674|set to the DEFAULT: a pin, and indistinguishable by value alone"
)

set(_failures 0)
set(_checked 0)
set(_optOuts 0)
set(_defaults 0)
foreach(_row IN LISTS _rows)
    fastcached_row_fields("${_row}" _present _value _expectWanted _about)

    _fc_resolve_addr("${_present}" "${_value}" _gotWanted)
    math(EXPR _checked "${_checked} + 1")

    if(NOT _gotWanted STREQUAL "${_expectWanted}")
        message(WARNING
            "present=${_present} value='${_value}': resolved '${_gotWanted}', expected '${_expectWanted}' -- ${_about}")
        math(EXPR _failures "${_failures} + 1")
    endif()

    # Tallied here rather than re-derived after the loop: a post-loop pair that
    # calls the function again with the same arguments restates two rows and
    # asserts less than they do.
    if(_expectWanted STREQUAL "")
        math(EXPR _optOuts "${_optOuts} + 1")
    else()
        math(EXPR _defaults "${_defaults} + 1")
    endif()
endforeach()

# The count is asserted, not merely printed. A row list that stopped being a list
# — one stray separator, one edit — leaves the loop running zero times, and zero
# failures out of zero rows is a pass reporting success for work it did not do.
if(_checked EQUAL 0)
    message(FATAL_ERROR
        "check-fastcache-addr-opt-out: no rows were checked, so this check proved nothing "
        "rather than passing.")
endif()

# Both outcomes must be exercised. A table that lost every opt-out row would pass
# a check that only ever sees the default, and one that lost every default row
# would pass even more quietly -- neither is visible in a failure count.
if(_optOuts EQUAL 0 OR _defaults EQUAL 0)
    message(FATAL_ERROR
        "check-fastcache-addr-opt-out: ${_optOuts} opt-out row(s) and ${_defaults} default row(s); "
        "one outcome is unexercised, so the other is passing without anything to disagree with it.")
endif()

if(NOT _failures EQUAL 0)
    message(FATAL_ERROR "check-fastcache-addr-opt-out: ${_failures} mismatch(es) over ${_checked} rows")
endif()

message(STATUS "check-fastcache-addr-opt-out: ${_checked} rows; set-but-empty opts out, unset defaults")
