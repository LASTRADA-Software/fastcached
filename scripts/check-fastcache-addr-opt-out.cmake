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

# ---------------------------------------------------------------------------
# present | value | expected wanted | expected token
#
# The first two rows are the whole ticket: identical values, opposite answers,
# told apart by presence alone.
set(_rows
    "OFF||127.0.0.1:6674|0:"
    "ON|||1:"
    "ON|10.0.0.5:6674|10.0.0.5:6674|1:10.0.0.5:6674"
    "OFF|ignored-when-absent|127.0.0.1:6674|0:"
    "ON|127.0.0.1:6674|127.0.0.1:6674|1:127.0.0.1:6674"
)

set(_failures 0)
set(_checked 0)
foreach(_row IN LISTS _rows)
    string(REPLACE "|" ";" _fields "${_row}")
    list(LENGTH _fields _fieldCount)
    if(NOT _fieldCount EQUAL 4)
        message(FATAL_ERROR
            "check-fastcache-addr-opt-out: row '${_row}' has ${_fieldCount} fields, expected 4. "
            "A malformed row would otherwise read off the end and assert against an empty string.")
    endif()
    list(GET _fields 0 _present)
    list(GET _fields 1 _value)
    list(GET _fields 2 _expectWanted)
    list(GET _fields 3 _expectToken)

    _fc_resolve_addr("${_present}" "${_value}" _gotWanted _gotToken)
    math(EXPR _checked "${_checked} + 1")

    if(NOT _gotWanted STREQUAL "${_expectWanted}")
        message(WARNING
            "present=${_present} value='${_value}': wanted '${_gotWanted}', expected '${_expectWanted}'")
        math(EXPR _failures "${_failures} + 1")
    endif()
    if(NOT _gotToken STREQUAL "${_expectToken}")
        message(WARNING
            "present=${_present} value='${_value}': token '${_gotToken}', expected '${_expectToken}'")
        math(EXPR _failures "${_failures} + 1")
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

# And the two rows the ticket is ABOUT must both be present, by construction:
# a table that lost the presence pair would still run and still pass.
_fc_resolve_addr(ON "" _emptySet _emptySetToken)
_fc_resolve_addr(OFF "" _absent _absentToken)
if(_emptySet STREQUAL "${_absent}")
    message(FATAL_ERROR
        "check-fastcache-addr-opt-out: a set-but-empty FASTCACHE_ADDR resolves to '${_emptySet}', "
        "the same as an unset one. That is #372: the documented opt-out is folded into the default, "
        "so `export FASTCACHE_ADDR=` keeps caching.")
endif()
if(_emptySetToken STREQUAL "${_absentToken}")
    message(FATAL_ERROR
        "check-fastcache-addr-opt-out: set-but-empty and unset record the same token "
        "('${_emptySetToken}'), so a reconfigure cannot see an opt-out exported over a default.")
endif()

if(NOT _failures EQUAL 0)
    message(FATAL_ERROR "check-fastcache-addr-opt-out: ${_failures} mismatch(es) over ${_checked} rows")
endif()

message(STATUS "check-fastcache-addr-opt-out: ${_checked} rows; set-but-empty opts out, unset defaults")
