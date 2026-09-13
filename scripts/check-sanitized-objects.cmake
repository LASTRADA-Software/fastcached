# SPDX-License-Identifier: Apache-2.0
#
# Policies are pinned because a `cmake -P` script gets OLD defaults for every
# policy the project has not stated.
cmake_minimum_required(VERSION 3.28)
#
# Objects that must carry a sanitizer's instrumentation do, and a control object that must
# not, does not.
#
# ## Why the OBJECTS, and why there is a control
#
# The vendored TUI went unsanitized in every sanitizer configuration while the configure
# printed "Enabling" and the cache read ON (#134): the flags are directory-scoped and the
# `vendor/` targets are declared before them. Nothing a configure says could have shown it,
# and neither could a binary -- the link pulls the sanitizer runtime in whole, so an
# executable carries `__asan_init` DEFINED whether or not a single object was compiled for it
# (`scripts/tsan-gate.sh` records the same trap for `__tsan_init`, #472). An object cannot
# borrow a symbol from a runtime it is not linked to, so its UNDEFINED reference to the
# runtime's init is the one reading that means instrumented.
#
# A reader asked only about objects that should be instrumented can pass by never being able
# to say "absent" -- an `nm` that printed nothing and a match that fired on anything would both
# do it. So every run also reads a CONTROL: an object compiled in the same configuration with
# `-fno-sanitize=all`, which must come back WITHOUT the reference. The two lists together
# are what make a green run mean something.
#
# ## What this does NOT claim
#
# That the instrumented code was EXERCISED, or that the sanitizer would report a defect in it:
# only that the compiler instrumented it. And UndefinedBehaviorSanitizer has no per-object
# init to read, so a UBSan-only configuration is not what this answers for.
#
# Usage:
#   cmake -DFASTCACHED_NM=<nm> -DFASTCACHED_SYMBOLS=<sym>[|<sym>...]
#         -DFASTCACHED_INSTRUMENTED=<obj>[|<obj>...] -DFASTCACHED_UNINSTRUMENTED=<obj>[|<obj>...]
#         -P scripts/check-sanitized-objects.cmake
#
# `|`-separated rather than CMake lists, because a `;` in a `-D` argument splits the argument.
#
# Exit: the verdict is the OUTPUT, not the status -- read through `FAIL_REGULAR_EXPRESSION`.

foreach(required FASTCACHED_NM FASTCACHED_SYMBOLS FASTCACHED_INSTRUMENTED FASTCACHED_UNINSTRUMENTED)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR
            "check-sanitized-objects: ${required} is not set or is empty.\n"
            "An empty list is the absence of a verdict, not a clean one: with no object to read, "
            "every object read is instrumented and every control is not.")
    endif()
endforeach()

string(REPLACE "|" ";" symbols "${FASTCACHED_SYMBOLS}")
string(REPLACE "|" ";" instrumented "${FASTCACHED_INSTRUMENTED}")
string(REPLACE "|" ";" uninstrumented "${FASTCACHED_UNINSTRUMENTED}")

# @return in @p out: which of `symbols` @p object references UNDEFINED, as a list.
function(fastcached_sanitizer_references object out)
    if(NOT EXISTS "${object}")
        message(FATAL_ERROR
            "check-sanitized-objects: ${object} does not exist.\n"
            "Build the targets before running this check. A missing object is not an "
            "uninstrumented one, and must not be reported as either answer.")
    endif()
    execute_process(
        COMMAND "${FASTCACHED_NM}" -u "${object}"
        OUTPUT_VARIABLE nmOut
        ERROR_VARIABLE nmErr
        RESULT_VARIABLE nmStatus)
    if(NOT nmStatus EQUAL 0)
        message(FATAL_ERROR
            "check-sanitized-objects: `${FASTCACHED_NM} -u ${object}` did not answer "
            "(${nmStatus}): ${nmErr}\n"
            "An object nm cannot read is not an uninstrumented one, so this refuses rather than "
            "guessing which it was.")
    endif()
    set(found "")
    foreach(symbol IN LISTS symbols)
        # Mach-O prefixes C symbols with one more underscore, so `___asan_init` there.
        if(nmOut MATCHES "(^|[ \t\n])_?${symbol}(\n|$)")
            list(APPEND found "${symbol}")
        endif()
    endforeach()
    set("${out}" "${found}" PARENT_SCOPE)
endfunction()

set(problems "")
list(LENGTH symbols symbolCount)

foreach(object IN LISTS instrumented)
    fastcached_sanitizer_references("${object}" found)
    list(LENGTH found foundCount)
    if(NOT foundCount EQUAL symbolCount)
        string(REPLACE ";" ", " renderedFound "${found}")
        string(APPEND problems
            "  ${object} carries [${renderedFound}] of the required undefined references\n")
    endif()
endforeach()

foreach(object IN LISTS uninstrumented)
    fastcached_sanitizer_references("${object}" found)
    if(NOT found STREQUAL "")
        string(REPLACE ";" ", " renderedFound "${found}")
        string(APPEND problems
            "  the CONTROL ${object} carries [${renderedFound}], but it was compiled with "
            "-fno-sanitize=all. This reader cannot say `absent`, so its `present` above means "
            "nothing\n")
    endif()
endforeach()

string(REPLACE ";" ", " renderedSymbols "${symbols}")
if(NOT problems STREQUAL "")
    message(FATAL_ERROR
        "Sanitizer instrumentation is not what this configuration claims (looking for an "
        "undefined ${renderedSymbols}):\n${problems}\n"
        "An object without the reference was compiled with no sanitizer flag, whatever the "
        "configure printed. For the vendored TUI, the flags are applied target by target after "
        "`include(Sanitizers)` in the top-level CMakeLists.txt, because directory-scoped options "
        "cannot reach targets declared before them -- check that block still runs and still "
        "reaches every target `vendor/` declares.")
endif()

list(LENGTH instrumented instrumentedCount)
list(LENGTH uninstrumented uninstrumentedCount)
message("sanitized objects: ${instrumentedCount} object(s) carry an undefined ${renderedSymbols}; "
        "the ${uninstrumentedCount} control object(s) compiled with -fno-sanitize=all do not")
