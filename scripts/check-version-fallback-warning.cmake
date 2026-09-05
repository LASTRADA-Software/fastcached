# SPDX-License-Identifier: Apache-2.0
#
# When a version-mismatched `fastcache-cc` hands the build to sccache, the operator
# is told both facts in one place — checked as a pure computation, over the
# combinations a configure cannot be made to produce.
#
# `cmake/portable/CompileCache.cmake` prints a rejected launcher at `STATUS` and,
# eighty lines later, warns about the correctness hazard the replacement carries.
# Neither mentions the other, and one rejection reason — a WIRE VERSION mismatch —
# is exactly the one that predicts the hazard: a daemon IS running, the operator
# installed it, and it is out of step with the launcher
# ([#658](https://github.com/LASTRADA-Software/fastcached/issues/658)).
#
# ## Why a pure function
#
# Producing the situation for real needs a `fastcache-cc` and a `fastcached` whose
# WIRE VERSIONS disagree, both installed, with the daemon answering — and then an
# MSVC-family compiler for the caveat to exist at all, which this host does not
# have. That is not a fixture; it is a second machine and a downgrade. So the
# DECISION takes its four inputs as arguments and reads nothing, and the rows below
# state each combination explicitly.
#
# What this cannot cover is the wiring — that `_fc_cache_fastcache_cc_rejected_why`
# is set where the row is rejected and read where the fallback is chosen. Stated
# rather than implied: only a real configure exercises that, and the narrow guard
# is that the reason is remembered as a VARIABLE rather than parsed back out of the
# display sentence, so a rewording of the status line cannot silently disarm it.
#
# Verdict is read from the OUTPUT, not the exit code: `message(FATAL_ERROR)` exits 0
# on CMake 3.28, this project's declared minimum.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()

include("${FASTCACHED_SOURCE_DIR}/cmake/portable/CompileCache.cmake")

if(NOT COMMAND _fc_cache_predicted_hazard)
    message(FATAL_ERROR
        "_fc_cache_predicted_hazard is not defined. Either the define-only guard in "
        "cmake/portable/CompileCache.cmake returned before the function, or it was renamed and "
        "this check now proves nothing.")
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

set(_caveat "sccache replays a hit's /showIncludes stream")
set(_detail "Rebuild or reinstall whichever is older.")

set(_checked 0)
set(_failures 0)
set(_warned 0)
set(_quiet 0)

# reason | predicts | rejected | winner | caveat-present | expect | what this row is about
#
# The property is winner-AGNOSTIC: whoever won, if they carry a hazard and somebody
# was rejected for a reason their own row says predicts it, both are said in one
# place. An earlier version of this table asserted that a caveat-carrying `ccache`
# must NOT warn -- a state the candidate table makes impossible, and the wrong
# answer if it ever became possible. A test that enshrines a special case is one
# that has to be rewritten to generalise the code, which is the wrong way round.
#
# No ';' in any field: the row LIST splits on it before `fastcached_row_fields`
# ever runs. '|' in the LAST field is fine -- that is what the house splitter buys.
set(_rows
    "rejected (unsupported-version)|unsupported-version|fastcache-cc|sccache|yes|yes|the whole ticket: a reason that predicts, into a winner that carries the hazard"
    "rejected (unsupported-version)|unsupported-version|fastcache-cc|ccache|yes|yes|winner-agnostic: any winner carrying a caveat gets the same treatment"
    "rejected (unsupported-version)|unsupported-version|fastcache-cc|sccache|no|no|the winner carries no hazard, so there is nothing to predict"
    "no answer within 10s|unsupported-version|fastcache-cc|sccache|yes|no|no daemon answered -- predicts nothing about versions"
    "probe compile failed (1)|unsupported-version|fastcache-cc|sccache|yes|no|a broken probe is not a version mismatch"
    "not caching (uncacheable)|unsupported-version|fastcache-cc|sccache|yes|no|a deliberately uncacheable probe says nothing about the daemon"
    "rejected (unsupported-version)||fastcache-cc|sccache|yes|no|a row that declares no prediction stays silent whatever its reason"
    "|unsupported-version|fastcache-cc|sccache|yes|no|nothing was rejected at all"
    "rejected (unsupported-version)|unsupported-version|fastcache-cc||yes|no|no launcher won, so there is no fallback to warn about"
)

foreach(_row IN LISTS _rows)
    fastcached_row_fields("${_row}"
        _reason _predicts _rejectedName _winner _hasCaveat _expect _about)

    if(_hasCaveat STREQUAL "yes")
        set(_thisCaveat "${_caveat}")
    else()
        set(_thisCaveat "")
    endif()

    _fc_cache_predicted_hazard(
        "${_reason}" "${_predicts}" "${_rejectedName}" "${_winner}" "${_thisCaveat}" "${_detail}" _got)
    math(EXPR _checked "${_checked} + 1")

    if(_expect STREQUAL "yes")
        math(EXPR _warned "${_warned} + 1")
        if(_got STREQUAL "")
            message(WARNING "no warning where one is required -- ${_about}")
            math(EXPR _failures "${_failures} + 1")
        elseif(NOT _got MATCHES "${_rejectedName}" OR NOT _got MATCHES "${_winner}")
            # It has to name BOTH launchers: joining the two facts is the entire
            # point, and a sentence naming one of them is the status quo.
            message(WARNING "the warning does not name both launchers -- ${_about}")
            math(EXPR _failures "${_failures} + 1")
        endif()
    else()
        math(EXPR _quiet "${_quiet} + 1")
        if(NOT _got STREQUAL "")
            message(WARNING "warned where it must not -- ${_about}")
            math(EXPR _failures "${_failures} + 1")
        endif()
    endif()
endforeach()

# Asserted, not printed: a row list that stopped being a list leaves the loop
# running zero times, and zero failures over zero rows is a pass reporting success
# for work it did not do. Both outcomes must also be exercised -- a table that lost
# every silent row would pass a check that only ever asserts a warning appears, and
# one that lost every warning row would pass more quietly still. Tallied in the
# loop rather than re-derived afterwards, which would restate two rows and assert
# less than they already do.
if(_checked EQUAL 0)
    message(FATAL_ERROR
        "check-version-fallback-warning: no rows were checked, so this check proved nothing rather "
        "than passing.")
endif()
if(_warned EQUAL 0 OR _quiet EQUAL 0)
    message(FATAL_ERROR
        "check-version-fallback-warning: ${_warned} warning row(s) and ${_quiet} silent row(s); one "
        "outcome is unexercised, so the other is passing without anything to disagree with it.")
endif()

# ---------------------------------------------------------------------------
# The COLUMN, and the wire name it has to agree with.
#
# The loop above drives the decision function directly, so it says nothing about
# what `cmake/portable/CompileCache.cmake` actually puts in the `predicts` column
# -- widening that column to `.` leaves every row above green. Generalising the
# decision into a table moved the fragility rather than removing it, and this is
# where it lands instead.
#
# Both literals are READ from the files that own them, never restated here. The
# module's pattern is a regex over a launcher stderr string whose only definition
# is the wire error table's `.name`, and nothing else connects the two: rename it
# there and this warning goes silent with every test still green. That is the
# coupling `check-tsan-scope` exists for, in a different file.
set(_module "${FASTCACHED_SOURCE_DIR}/cmake/portable/CompileCache.cmake")
set(_wire "${FASTCACHED_SOURCE_DIR}/src/FastCache/Protocol/CompileCacheWire.hpp")
foreach(_f "${_module}" "${_wire}")
    if(NOT EXISTS "${_f}")
        message(FATAL_ERROR "check-version-fallback-warning: ${_f} does not exist")
    endif()
endforeach()

file(READ "${_module}" _moduleText)
if(NOT _moduleText MATCHES "set\\(_fc_cache_fastcache_cc_predicts \"([^\"]*)\"\\)")
    message(FATAL_ERROR
        "check-version-fallback-warning: could not find fastcache-cc's `predicts` column in "
        "${_module}. Either it was renamed or the column is gone, and this check now proves "
        "nothing about what the module actually declares.")
endif()
set(_declared "${CMAKE_MATCH_1}")

file(READ "${_wire}" _wireText)
if(NOT _wireText MATCHES "ErrorCode::UnsupportedVersion, \\.name = \"([^\"]*)\"")
    message(FATAL_ERROR
        "check-version-fallback-warning: could not find UnsupportedVersion's wire name in "
        "${_wire}. The row moved or was reshaped, so the agreement below cannot be checked.")
endif()
set(_wireName "${CMAKE_MATCH_1}")

if(NOT _declared STREQUAL "${_wireName}")
    message(FATAL_ERROR
        "check-version-fallback-warning: the module predicts on '${_declared}' but the wire error "
        "table spells it '${_wireName}'. The launcher reports the wire name, so the warning would "
        "never fire -- silently, with every other test green.")
endif()

# And it must be NARROW. A column matching everything warns on "not installed" and
# "no answer", which is the alarm nobody reads -- and no row above can see it,
# because they supply their own pattern.
_fc_cache_predicted_hazard(
    "no answer within 10s" "${_declared}" "fastcache-cc" "sccache" "${_caveat}" "${_detail}" _wide)
if(NOT _wide STREQUAL "")
    message(FATAL_ERROR
        "check-version-fallback-warning: the declared pattern '${_declared}' also matches a "
        "rejection that predicts nothing, so this warns on every fall-through.")
endif()

if(NOT _failures EQUAL 0)
    message(FATAL_ERROR "check-version-fallback-warning: ${_failures} mismatch(es) over ${_checked} rows")
endif()

message(STATUS
    "check-version-fallback-warning: ${_checked} rows; a predicting rejection into a hazardous winner warns, nothing else does")
