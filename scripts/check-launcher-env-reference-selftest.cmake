# SPDX-License-Identifier: Apache-2.0
#
# `launcher-env-reference` must be SEEN to refuse, on each thing it claims, and SEEN
# to accept -- a guard nobody has watched accept is not known to work, and on a tree
# where the two files already agree (they do, at 17 names each) the refusing direction
# is the only one a green run ever exercises.
#
# Four of the cases are the check's own claims rather than decoration:
#
#   prose-only     a variable NAMED in the page's prose but given no table row must
#                  still be a violation. The page mentions these names dozens of
#                  times outside the table, so a reader matching mentions rather than
#                  rows would report a documented set that includes every variable
#                  anybody ever wrote a sentence about.
#   bracketed      the page is markdown and every other line carries a `[link](target)`.
#                  This case puts brackets on and around the rows, and the check must
#                  still agree -- the property `file(STRINGS)` would not have.
#   empty-header   } two empty lists agree perfectly, so a scan matching nothing must
#   empty-page     } REFUSE. This is the failure mode the check exists inside of.
#
# Runs as `cmake -P`. See `check-script-check-signals.cmake` for why such a check
# reports failure through its OUTPUT rather than an exit code.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -DFASTCACHED_SCRATCH_DIR=<dir> \
#         -P scripts/check-launcher-env-reference-selftest.cmake
#
# Exit codes: 0 always. The verdict is the presence of `CMake Error` in the output.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()
if(NOT DEFINED FASTCACHED_SCRATCH_DIR)
    message(FATAL_ERROR "FASTCACHED_SCRATCH_DIR must be set")
endif()

set(check "${FASTCACHED_SOURCE_DIR}/scripts/check-launcher-env-reference.cmake")
if(NOT EXISTS "${check}")
    message(FATAL_ERROR "the check under test is missing: ${check}")
endif()

set(root "${FASTCACHED_SCRATCH_DIR}/launcher-env-reference-selftest")
file(REMOVE_RECURSE "${root}")
set(failures "")
set(ran 0)

# A tree carrying only the two files the check reads, at the paths it reads them from.
function(fastcached_make_tree name header page outVar)
    set(tree "${root}/${name}")
    file(REMOVE_RECURSE "${tree}")
    file(MAKE_DIRECTORY "${tree}/src/apps/fastcache-cc")
    file(MAKE_DIRECTORY "${tree}/docs/tools")
    file(WRITE "${tree}/src/apps/fastcache-cc/LauncherCli.hpp" "${header}")
    file(WRITE "${tree}/docs/tools/fastcache-cc.md" "${page}")
    set(${outVar} "${tree}" PARENT_SCOPE)
endfunction()

# `want` is OBJECTED or ACCEPTED. The verdict is read from the OUTPUT, never from the
# exit status: `message(WARNING)` exits 0 while printing `CMake Warning`, and a check
# that shells out to another `cmake -P` exits 0 carrying its child's error on stdout.
function(fastcached_case name header page want)
    fastcached_make_tree("${name}" "${header}" "${page}" tree)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DFASTCACHED_SOURCE_DIR=${tree}" -P "${check}"
        OUTPUT_VARIABLE captured ERROR_VARIABLE capturedErrors RESULT_VARIABLE ignored)
    set(combined "${captured}${capturedErrors}")

    # CMake WRAPS its diagnostics, so a phrase can exist in the output and in no single
    # LINE of it. Flattened before matching, which is why `CMake Error` -- short, and
    # never straddling a wrap -- is what is matched rather than a sentence.
    string(REPLACE "\n" " " flattened "${combined}")
    # `CMake Error|CMake Warning`, never `CMake Error` alone: a sub-run that merely WARNS
    # changes meaning silently and, read for the error word alone, is scored a clean pass
    # (#672). Stated in full -- and enforced -- in `scripts/check-script-check-signals.cmake`.
    set(verdict "ACCEPTED")
    if(flattened MATCHES "CMake Error|CMake Warning")
        set(verdict "OBJECTED")
    endif()

    math(EXPR nextRan "${ran} + 1")
    set(ran "${nextRan}" PARENT_SCOPE)
    if(NOT verdict STREQUAL want)
        list(APPEND failures "  FAIL  ${name}: wanted ${want}, got ${verdict}")
        set(failures "${failures}" PARENT_SCOPE)
        message(STATUS "  FAIL  ${name}: wanted ${want}, got ${verdict}")
    else()
        message(STATUS "  ok    ${name}: ${verdict}")
    endif()
endfunction()

set(twoDecls
    "namespace EnvName\n{\n"
    "    constexpr std::string_view Addr = \"FASTCACHE_ADDR\"\;\n"
    "    constexpr std::string_view Token = \"FASTCACHE_TOKEN\"\;\n}\n")
string(JOIN "" twoDecls ${twoDecls})

set(twoRows
    "| Variable | Meaning | Default |\n|---|---|---|\n"
    "| `FASTCACHE_ADDR` | where the cache is | none |\n"
    "| `FASTCACHE_TOKEN` | the credential | unset |\n")
string(JOIN "" twoRows ${twoRows})

fastcached_case("agree" "${twoDecls}" "${twoRows}" "ACCEPTED")

# Declared and undocumented: the direction #881 is about.
set(threeDecls "${twoDecls}")
string(REPLACE "}\n" "    constexpr std::string_view Extra = \"FASTCACHE_EXTRA\"\;\n}\n" threeDecls "${threeDecls}")
fastcached_case("declared-undocumented" "${threeDecls}" "${twoRows}" "OBJECTED")

# Documented and never read: advice that silently does nothing.
set(threeRows "${twoRows}| `FASTCACHE_GONE` | a name nothing reads | unset |\n")
fastcached_case("documented-unread" "${twoDecls}" "${threeRows}" "OBJECTED")

# A mention in PROSE is not a row. Without this case the check could match anywhere in
# the page and would call every variable documented that anybody wrote a sentence about.
set(proseOnly "${twoRows}\nSet `FASTCACHE_EXTRA` to change behaviour; see above.\n")
fastcached_case("prose-only" "${threeDecls}" "${proseOnly}" "OBJECTED")

# The bracket claim in the check's header, asserted rather than assumed. A
# `file(STRINGS)` reader merges elements across a stray `]`, which is how two of this
# repository's readers passed over a real violation in silence.
set(bracketed
    "| Variable | Meaning | Default |\n|---|---|---|\n"
    "| `FASTCACHE_ADDR` | see [the protocol page](../protocols/compile-cache.md#error-codes) | none |\n"
    "| `FASTCACHE_TOKEN` | a credential [1] and a stray ] bracket | unset |\n")
string(JOIN "" bracketed ${bracketed})
fastcached_case("bracketed" "${twoDecls}" "${bracketed}" "ACCEPTED")

# And bracketed AND wrong, so the case above cannot pass by the check having been
# blinded: brackets must not swallow a violation either.
set(bracketedWrong "${bracketed}| `FASTCACHE_GONE` | [nothing](x) reads this | unset |\n")
fastcached_case("bracketed-violation" "${twoDecls}" "${bracketedWrong}" "OBJECTED")

# Two empty lists agree perfectly. Both halves, because one guard is not the other.
fastcached_case("empty-header" "namespace EnvName\n{\n}\n" "${twoRows}" "OBJECTED")
fastcached_case("empty-page" "${twoDecls}" "no table here at all\n" "OBJECTED")

# A declaration-shaped line in a COMMENT is not a declaration. The real header carries
# several sentences naming these variables.
set(commentedDecl "${twoDecls}// constexpr std::string_view Ghost = \"FASTCACHE_GHOST\"\;\n")
fastcached_case("comment-is-not-a-declaration" "${commentedDecl}" "${twoRows}" "ACCEPTED")

if(failures)
    set(text "")
    foreach(failure IN LISTS failures)
        string(APPEND text "\n${failure}")
    endforeach()
    message(FATAL_ERROR "launcher-env-reference selftest: ${ran} case(s) ran, and:${text}")
endif()

message(STATUS "launcher-env-reference selftest: ${ran} case(s) ran, all as expected")
