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

if(NOT COMMAND _fc_cache_version_fallback_warning)
    message(FATAL_ERROR
        "_fc_cache_version_fallback_warning is not defined. Either the define-only guard in "
        "cmake/portable/CompileCache.cmake returned before the function, or it was renamed and "
        "this check now proves nothing.")
endif()

set(_caveat "sccache replays a hit's /showIncludes stream")
set(_addr "127.0.0.1:6674")

set(_checked 0)
set(_failures 0)

# reason | chosen | caveat-present | expect-warning | what this row is about
#
# No `;` in any field: it IS a CMake list separator, so a row containing one splits
# into more elements than it has and the last `list(GET)` reads off the end. The
# field-count assertion below caught exactly that while this table was being
# written, which is why it is an assertion rather than a comment.
set(_rows
    "rejected (unsupported-version)|sccache|yes|yes|the whole ticket: the one reason that predicts the hazard"
    "rejected (unsupported-version)|ccache|yes|no|ccache is unaffected, so there is nothing to join"
    "rejected (unsupported-version)|sccache|no|no|a non-MSVC driver: the caveat does not exist, so neither does the hazard"
    "no answer within 10s|sccache|yes|no|no daemon answered -- predicts nothing about versions"
    "probe compile failed (1)|sccache|yes|no|a broken probe is not a version mismatch"
    "not caching (uncacheable)|sccache|yes|no|a deliberately uncacheable probe says nothing about the daemon"
    "|sccache|yes|no|nothing was rejected at all"
    "rejected (unsupported-version)||yes|no|no launcher won, so there is no fallback to warn about"
)

foreach(_row IN LISTS _rows)
    string(REPLACE "|" ";" _f "${_row}")
    list(LENGTH _f _n)
    if(NOT _n EQUAL 5)
        message(FATAL_ERROR
            "check-version-fallback-warning: row '${_row}' has ${_n} fields, expected 5. A malformed "
            "row would otherwise read off the end and assert against an empty string.")
    endif()
    list(GET _f 0 _reason)
    list(GET _f 1 _chosen)
    list(GET _f 2 _hasCaveat)
    list(GET _f 3 _expect)
    list(GET _f 4 _about)

    if(_hasCaveat STREQUAL "yes")
        set(_thisCaveat "${_caveat}")
    else()
        set(_thisCaveat "")
    endif()

    _fc_cache_version_fallback_warning("${_reason}" "${_chosen}" "${_thisCaveat}" "${_addr}" _got)
    math(EXPR _checked "${_checked} + 1")

    if(_expect STREQUAL "yes")
        if(_got STREQUAL "")
            message(WARNING "no warning where one is required -- ${_about}")
            math(EXPR _failures "${_failures} + 1")
        elseif(NOT _got MATCHES "${_addr}")
            # It has to NAME the daemon, or an operator with more than one cannot
            # act on it. That is the difference between a diagnosis and a notice.
            message(WARNING "the warning does not name the endpoint -- ${_about}")
            math(EXPR _failures "${_failures} + 1")
        endif()
    else()
        if(NOT _got STREQUAL "")
            message(WARNING "warned where it must not -- ${_about}")
            math(EXPR _failures "${_failures} + 1")
        endif()
    endif()
endforeach()

# Asserted, not printed: a row list that stopped being a list leaves the loop
# running zero times, and zero failures over zero rows is a pass reporting success
# for work it did not do.
if(_checked EQUAL 0)
    message(FATAL_ERROR
        "check-version-fallback-warning: no rows were checked, so this check proved nothing rather "
        "than passing.")
endif()

# And both directions must actually be exercised. A table that lost every negative
# row would pass a check that only ever asserts a warning appears -- and one that
# lost the single positive row would pass even more quietly.
_fc_cache_version_fallback_warning("rejected (unsupported-version)" "sccache" "${_caveat}" "${_addr}" _positive)
_fc_cache_version_fallback_warning("no answer within 10s" "sccache" "${_caveat}" "${_addr}" _negative)
if(_positive STREQUAL "")
    message(FATAL_ERROR
        "check-version-fallback-warning: the situation this exists for produces no warning, so the "
        "negative rows below are passing vacuously.")
endif()
if(NOT _negative STREQUAL "")
    message(FATAL_ERROR
        "check-version-fallback-warning: a rejection that is NOT a version mismatch also warns, so "
        "this warns on every fall-through and is the alarm nobody reads.")
endif()

if(NOT _failures EQUAL 0)
    message(FATAL_ERROR "check-version-fallback-warning: ${_failures} mismatch(es) over ${_checked} rows")
endif()

message(STATUS
    "check-version-fallback-warning: ${_checked} rows; a version mismatch into sccache warns, nothing else does")
