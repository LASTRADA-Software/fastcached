# SPDX-License-Identifier: Apache-2.0
#
# `PEDANTIC_COMPILER_WERROR` decides whether warnings are FATAL. It may not decide
# which warnings EXIST.
#
# `cmake/portable/PedanticCompiler.cmake` adds `-pedantic` under
# `PEDANTIC_COMPILER`, and used to add the suppressions that flag makes necessary
# -- `-Wno-c2y-extensions`, `-Wno-c++20-extensions`, `-Wno-missing-declarations`,
# `-Wno-class-memaccess` -- under `PEDANTIC_COMPILER_WERROR`. Two conditions over
# one concern, so the four presets that inherit `PEDANTIC_COMPILER=ON` from `base`
# without turning `WERROR` on -- `clang-coverage`, `clang-asan-ubsan`, `clang-tsan`,
# `clang-tracy` -- got the warnings and none of the suppressions
# ([#611](https://github.com/LASTRADA-Software/fastcached/issues/611)).
#
# Measured on 0db96dc8 before the fix, each preset in its OWN fresh build directory
# with `/usr/bin/clang++` pinned by absolute path and no compiler-cache launcher:
# **7049 `-Wc2y-extensions` warnings across 195 translation units, identically in
# all four**, every one of them `'__COUNTER__' is a C2y extension` from Catch2's
# `TEST_CASE`. That is the diagnostic
# [#454](https://github.com/LASTRADA-Software/fastcached/issues/454) reported and
# could not explain.
#
# ## Why this is a check and not a comment
#
# The defect is invisible in every direction somebody would look. It breaks no
# build: those four presets have `WERROR` off, so 7049 warnings are 7049 warnings
# and the build exits 0. It does not reproduce in the preset people run, because
# `clang-debug` has `WERROR` on and therefore does get the suppressions. And it
# surfaces as a clang-tidy finding in a build directory somebody reused across
# presets, which reads as a stale cache rather than as a rule -- which is exactly
# how #454 came to be believed unreproducible.
#
# So the rule needs a reader, and the reader must not be a text scan. What went
# wrong was not a spelling; it was which `if()` a line sat inside. This check asks
# the file itself.
#
# ## How it asks
#
# It includes `PedanticCompiler.cmake` twice per compiler persona -- once with
# `PEDANTIC_COMPILER_WERROR` off, once on -- with `add_compile_options` replaced by
# a recorder and `check_cxx_compiler_flag` replaced by a stub that answers yes (see
# `scripts/pedantic-flag-probe/CheckCXXCompilerFlag.cmake`). The difference between
# the two recordings is, by construction, exactly the set of flags `WERROR` gates.
#
# That set may contain only flags that change whether a diagnostic is FATAL. A flag
# that changes WHICH diagnostics are produced is a violation whichever way it points
# -- appearing only when `WERROR` is on is #611; disappearing only when it is on
# would be the same defect wearing the other sign.
#
# Driving the file beats parsing it for a reason worth stating: a text scan would
# have to model `if`/`elseif`/`else`/`endif` nesting to answer "which condition is
# this line under", which is the one question that matters here, and a model of
# CMake that is subtly wrong fails in the confident direction. Including the file
# uses CMake's own answer.
#
# It is also already this project's idiom for asking a `cmake/portable/` module
# what it computes: `check-debug-prefix-map.cmake` and
# `check-fastcache-addr-opt-out.cmake` both `include()` `CompileCache.cmake` and
# drive it. The only thing new here is the stub, which those two do not need
# because the functions they call spawn nothing.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()

# The subject. A parameter so `check-pedantic-suppressions-selftest.cmake` can drive
# this same script against synthetic files -- a guard nobody has watched refuse is
# not a guard, and the only way to watch this one refuse is to hand it a file that
# violates the rule.
if(NOT DEFINED FASTCACHED_PEDANTIC_FILE)
    set(FASTCACHED_PEDANTIC_FILE "${FASTCACHED_SOURCE_DIR}/cmake/portable/PedanticCompiler.cmake")
endif()

if(NOT DEFINED FASTCACHED_PEDANTIC_PROBE_DIR)
    set(FASTCACHED_PEDANTIC_PROBE_DIR "${FASTCACHED_SOURCE_DIR}/scripts/pedantic-flag-probe")
endif()

# Which compilers to ask as. `PedanticCompiler.cmake` branches on
# `CMAKE_CXX_COMPILER_FRONTEND_VARIANT` first and on `CMAKE_CXX_COMPILER_ID` inside,
# so a persona is the PAIR -- and `clang-cl` is why: its ID is `Clang` and its
# frontend is `MSVC`, so it takes the MSVC arm while `clang++` takes the GNU one.
# Asking only as `Clang` would leave that whole branch unread.
#
# The fourth column is what the persona covers, because a row nobody can attribute
# to a real configuration is a row nobody can judge when it starts failing.
#
# The fifth is the flag that persona MUST add when `WERROR` is off -- its anchor.
# Without one this check cannot tell "the subject adds nothing for this persona"
# from "the subject is fine": `-fdiagnostics-color=always` is added at the top of
# the module, OUTSIDE `if(${PEDANTIC_COMPILER})`, so every persona records at least
# one flag no matter what the pedantic arms do. Measured: with all four
# `try_add_compile_options` calls deleted from the MSVC arm, the earlier
# "adds no flag at either setting" guard still reported success. The GNU personas
# were covered by a hardcoded `-pedantic` test and the MSVC ones by nothing, and
# the selftest's empty-subject case passed only because a synthetic subject omits
# that unconditional line -- the guard bit on the fixture and not on the subject,
# which is this project's own "the mode under test was not the mode in use".
#
# A column rather than a branch, so a fifth compiler brings its own anchor instead
# of inheriting whichever arm somebody last wrote an `if` for.
set(pedanticPersonaTable
    "Clang|GNU|clang++|-pedantic|clang-debug, clang-release, clang-coverage, clang-asan-ubsan, clang-tsan, clang-tracy"
    "GNU|GNU|g++|-pedantic|gcc-debug, gcc-release"
    "MSVC|MSVC|cl|/W4|cl-debug, cl-release"
    "Clang|MSVC|clang-cl|/W4|clangcl-debug, clangcl-release"
)

# The only flags `PEDANTIC_COMPILER_WERROR` may gate: the ones that decide FATALITY
# and select no diagnostic. Everything else selects a diagnostic and belongs under
# the same condition as the flag that makes it necessary.
#
# The taxonomy is the whole check, so it is worth being exact about where
# `-Wno-error=X` falls. It is a fatality flag: it says "keep X visible, do not fail
# on it", and a preset that then SEES X is getting what the flag asks for. `-Wno-X`
# says "never show me X", and a preset that sees 7049 of them is getting the
# opposite -- which is #611. One belongs to `WERROR`; the other does not.
#
# A regex column rather than literal flags, because `-Wno-error=` is a family and a
# row per diagnostic would be the copy-paste this rule is about. The reason column
# is not decoration: a row nobody can judge is a row that gets widened by the next
# person who meets a refusal.
# No ';' in any field. `foreach(... IN LISTS ...)` splits on it, so a semicolon in
# a reason makes one row into two -- the second holding no '|' at all. Found by the
# arity guard below on this very table, where "fatal on a GNU-style driver; selects
# no diagnostic" had silently become a second element. It cost nothing then, because
# the pattern half survived intact and still matched, so only the REASON was
# truncated -- which is the quiet half of every table defect in this tree.
set(pedanticFatalityFlags
    "^-Werror$|makes every existing warning fatal on a GNU-style driver, and selects no diagnostic"
    "^/WX$|the MSVC spelling of the same, currently commented out in the subject"
    "^-Wno-error=|exempts one diagnostic from fatality and leaves it VISIBLE, so a preset without -Werror that sees it is getting exactly what this asks for"
)

# Split one '|'-separated row into the variables named in ARGN, the last of which
# takes whatever remains -- so only the final field may contain a '|', which is
# what lets a reason be written in ordinary prose.
#
# Byte-for-byte with the copies in check-sccache-backend-caveat.cmake,
# check-byte-order-qualifier.cmake, check-node-surface-docs.cmake,
# check-version-fallback-warning.cmake and check-fastcache-addr-opt-out.cmake.
# Consolidating the copies is #495; keeping this one identical is what makes it
# visible to that sweep, and a sixth splitter spelled differently is precisely
# what such a sweep is silent about.
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

set(violations "")
set(vacuous "")

# `fastcached_row_fields` lets the LAST field hold a '|', which is right for a
# prose reason and not sufficient here: the FIRST field of a fatality row is a
# REGEX, and '|' is a regex metacharacter. `^-W(error|no-error=)|reason` -- the
# natural way somebody would merge the two rows below -- splits into a pattern of
# `^-W(error` and a reason of `no-error=)|reason`, matches nothing it was meant to,
# and refuses a legitimate flag while saying something that reads correct. So each
# row is required to hold exactly one '|', and the alternative is spelled out.
foreach(row IN LISTS pedanticFatalityFlags)
    string(REPLACE "|" ";" rowParts "${row}")
    list(LENGTH rowParts rowPartCount)
    if(NOT rowPartCount EQUAL 2)
        message(FATAL_ERROR
            "Malformed fatality row: ${row}\n"
            "A row is `<regex>|<reason>` and must hold exactly one '|', and no ';' anywhere.\n\n"
            "Two ways to arrive here. A ';' in any field: `foreach(... IN LISTS ...)` splits on it, "
            "so the row becomes two and the second holds no '|'. Or an alternation in the pattern "
            "column, which is a REGEX -- `^-W(error|no-error=)` collides with the field separator "
            "and splits into a pattern nobody wrote, refusing a legitimate flag while reading "
            "correct. Use a character class, or a second row.")
    endif()
endforeach()

# A missing input is refused where it is found, with its own sentence. Folding it
# into the report below would print the whole #611 trailer about moving
# suppressions and adding fatality rows at somebody whose file is simply not
# there -- advice that is true and answers a question they did not ask.
if(NOT EXISTS "${FASTCACHED_PEDANTIC_FILE}")
    message(FATAL_ERROR
        "${FASTCACHED_PEDANTIC_FILE}: the subject file is not there, so this check has nothing to "
        "read. It is a refusal rather than a pass because `absent` and `clean` are different "
        "states and only one of them means the rule holds.")
endif()

if(NOT EXISTS "${FASTCACHED_PEDANTIC_PROBE_DIR}/CheckCXXCompilerFlag.cmake")
    message(FATAL_ERROR
        "${FASTCACHED_PEDANTIC_PROBE_DIR}/CheckCXXCompilerFlag.cmake: the probe stub is not there, "
        "so every flag would go through a real try_compile -- unavailable in script mode -- and "
        "this check would report on nothing.")
endif()

# The recorder. `add_compile_options` is a directory command and is not available in
# script mode at all, so defining it is both the interception and the only way the
# subject can run here.
#
# A GLOBAL property and not a variable: the subject calls this from inside
# `try_add_compile_options`, so a `PARENT_SCOPE` write would land in that function's
# caller and be discarded when it returns. Measured -- the first draft recorded an
# empty list for every persona and would have passed a tree with the bug in it.
# @param ARGN The flags being added, each wrapped in the subject's CXX-only genex.
function(add_compile_options)
    foreach(flag IN LISTS ARGN)
        # Unwrap `$<$<COMPILE_LANGUAGE:CXX>:-Wfoo>`, which is how the subject adds
        # every flag. The rule is about which flag is added under which condition;
        # the genex is the same on all of them and would only obscure the diff.
        string(REGEX REPLACE "^\\$<\\$<COMPILE_LANGUAGE:CXX>:(.*)>$" "\\1" flag "${flag}")
        set_property(GLOBAL APPEND PROPERTY FASTCACHED_PEDANTIC_RECORDED "${flag}")
    endforeach()
endfunction()

# Every flag the subject adds for one persona at one `WERROR` setting.
# @param 1 CMAKE_CXX_COMPILER_ID to answer as.
# @param 2 CMAKE_CXX_COMPILER_FRONTEND_VARIANT to answer as.
# @param 3 The PEDANTIC_COMPILER_WERROR setting, ON or OFF.
# @param 4 Name of the variable the recorded flag list is reported through.
function(pedantic_flags_for compilerId frontendVariant werror outVariable)
    set(CMAKE_MODULE_PATH "${FASTCACHED_PEDANTIC_PROBE_DIR}")
    set(CMAKE_CXX_COMPILER_ID "${compilerId}")
    set(CMAKE_CXX_COMPILER_FRONTEND_VARIANT "${frontendVariant}")

    # Normal variables, so the subject's `option()` calls are no-ops under CMP0077
    # and leave these standing. That is the whole reason both settings are reachable
    # in one process.
    set(PEDANTIC_COMPILER ON)
    set(PEDANTIC_COMPILER_WERROR "${werror}")

    set_property(GLOBAL PROPERTY FASTCACHED_PEDANTIC_RECORDED "")
    include("${FASTCACHED_PEDANTIC_FILE}")
    get_property(recorded GLOBAL PROPERTY FASTCACHED_PEDANTIC_RECORDED)
    set(${outVariable} "${recorded}" PARENT_SCOPE)
endfunction()

# How many times a flag appears in a recorded list.
#
# A COUNT and not `IN_LIST`, and that difference is the whole of what this check
# can say about the repair its own refusal warns against. Adding a suppression
# outside the `WERROR` block while leaving it inside leaves the flag present at
# BOTH settings, so as SETS the difference is empty and the check passes -- which
# would make it green on exactly the shape the trailer sends people at: two
# conditions naming one diagnostic, with a guard reading as having approved it.
# As multisets the counts differ, and the copy is visible.
# @param 1 Name of the variable holding the recorded flag list.
# @param 2 The flag to count.
# @param 3 Name of the variable the count is reported through.
function(pedantic_count_flag listVariable flag outVariable)
    set(seen 0)
    foreach(candidate IN LISTS ${listVariable})
        if(candidate STREQUAL flag)
            math(EXPR seen "${seen} + 1")
        endif()
    endforeach()
    set(${outVariable} "${seen}" PARENT_SCOPE)
endfunction()

# Why a flag is allowed to be gated on `WERROR`, or empty when it is not.
# @param 1 The flag.
# @param 2 Name of the variable the reason is reported through.
function(pedantic_fatality_reason flag outVariable)
    set(${outVariable} "" PARENT_SCOPE)
    foreach(row IN LISTS pedanticFatalityFlags)
        fastcached_row_fields("${row}" rowPattern rowReason)
        if(flag MATCHES "${rowPattern}")
            set(${outVariable} "${rowReason}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
endfunction()

list(LENGTH pedanticPersonaTable personaCount)
if(personaCount EQUAL 0)
    list(APPEND vacuous "  the persona table is empty; this check asked nothing")
endif()

# Every flag `WERROR` gates, with the reason its row gives for being allowed there.
# Reported on the success line rather than discarded: the reason column is what a
# future author reads before widening the table, and a column nothing ever prints
# is one nobody can check the wording of.
set(gatedFlags "")

foreach(persona IN LISTS pedanticPersonaTable)
    fastcached_row_fields("${persona}"
        personaId personaFrontend personaDriver personaAnchor personaPresets)

    pedantic_flags_for("${personaId}" "${personaFrontend}" OFF withoutWerror)
    pedantic_flags_for("${personaId}" "${personaFrontend}" ON withWerror)

    # The anchor is what says this check still has a subject for this persona. A
    # count cannot: the module adds `-fdiagnostics-color=always` unconditionally,
    # so "recorded something" is true of every persona however empty the pedantic
    # arms are.
    if(NOT "${personaAnchor}" IN_LIST withoutWerror)
        list(APPEND vacuous
             "  ${personaDriver} (${personaId}/${personaFrontend}): ${personaAnchor} is not added when PEDANTIC_COMPILER_WERROR is OFF, so the asymmetry this check exists for cannot arise for this compiler and nothing about it is being checked")
        continue()
    endif()

    # Whichever way it points, a flag the two settings disagree about is a flag
    # `WERROR` decides. Only fatality may sit there.
    #
    # Compared by COUNT over the union, not by membership: a suppression added
    # outside the block AND left inside it is present at both settings, so a
    # set difference is empty and the copy is invisible -- see
    # `pedantic_count_flag`.
    set(everyFlag ${withoutWerror} ${withWerror})
    list(REMOVE_DUPLICATES everyFlag)
    foreach(flag IN LISTS everyFlag)
        pedantic_count_flag(withoutWerror "${flag}" countOff)
        pedantic_count_flag(withWerror "${flag}" countOn)

        if(countOn GREATER countOff AND countOff GREATER 0)
            list(APPEND violations
                 "  ${personaDriver} (${personaPresets}): ${flag} is added unconditionally AND again inside the PEDANTIC_COMPILER_WERROR block, so one diagnostic is named under two conditions -- move the flag rather than copying it, or the two places drift apart")
        elseif(countOn GREATER countOff)
            pedantic_fatality_reason("${flag}" reason)
            if("${reason}" STREQUAL "")
                list(APPEND violations
                     "  ${personaDriver} (${personaPresets}): ${flag} is added only when PEDANTIC_COMPILER_WERROR is ON, and it selects a diagnostic rather than deciding fatality")
            else()
                list(APPEND gatedFlags "  ${flag} -- ${reason}")
            endif()
        elseif(countOff GREATER countOn)
            list(APPEND violations
                 "  ${personaDriver} (${personaPresets}): ${flag} is DROPPED when PEDANTIC_COMPILER_WERROR is ON, so making warnings fatal also changes which warnings exist")
        endif()
    endforeach()
endforeach()

# No `missing` row: the two missing-input states are refused inline above, where
# they are found, so nothing ever appends to such a bucket. A heading nobody can
# reach reads as live coverage for a state that is handled somewhere else.
set(pedanticReportSections
    "vacuous|This check has stopped checking anything"
    "violations|PEDANTIC_COMPILER_WERROR decides which warnings exist"
)

set(report "")
foreach(row IN LISTS pedanticReportSections)
    fastcached_row_fields("${row}" sectionVariable sectionHeading)
    if(NOT "${${sectionVariable}}" STREQUAL "")
        list(JOIN ${sectionVariable} "\n" sectionBody)
        string(APPEND report "${sectionHeading}:\n${sectionBody}\n")
    endif()
endforeach()

if(NOT "${report}" STREQUAL "")
    message(FATAL_ERROR
        "${report}"
        "\n`PEDANTIC_COMPILER_WERROR` decides whether warnings are FATAL. It may not decide which "
        "warnings EXIST -- so `-pedantic` and every suppression it makes necessary are governed by "
        "ONE condition, `PEDANTIC_COMPILER`.\n\n"
        "Four presets inherit `PEDANTIC_COMPILER=ON` from `base` and leave `WERROR` off: "
        "clang-coverage, clang-asan-ubsan, clang-tsan, clang-tracy. While the suppressions sat "
        "inside the `WERROR` block they got `-pedantic` and none of them -- measured at 7049 "
        "`-Wc2y-extensions` warnings each, from Catch2's `__COUNTER__`, and reported as an "
        "unreproducible clang-tidy finding in #454 because it only appears in a build directory "
        "configured that way.\n\n"
        "Adding the suppressions outside the block as WELL as inside it does not close this: two "
        "places naming one diagnostic is the defect, not the spelling of it. Move the flag, do not "
        "copy it -- and this check counts occurrences rather than testing membership, so the copy "
        "is refused by name rather than passing quietly.\n\n"
        "If a flag genuinely belongs to fatality and selects no diagnostic, add it -- with its "
        "reason -- to the table in ${CMAKE_CURRENT_LIST_FILE}.")
endif()

# By flag, not by persona-flag pair: the reason is a property of the flag, and the
# same four rows repeated once per compiler is noise that would stop being read.
list(REMOVE_DUPLICATES gatedFlags)
list(SORT gatedFlags)
list(LENGTH gatedFlags gatedFlagCount)
list(JOIN gatedFlags "\n" gatedFlagBody)
message("pedantic suppressions: ${personaCount} compiler persona(s) asked, "
        "${gatedFlagCount} flag(s) gated on PEDANTIC_COMPILER_WERROR, all of them fatality-only\n"
        "${gatedFlagBody}")
