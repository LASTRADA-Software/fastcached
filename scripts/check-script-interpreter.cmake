# SPDX-License-Identifier: Apache-2.0
#
# Every script-driven test names its interpreter, and names it the same way.
#
# ## The rule this gives an executor to
#
# `.agent/rules/build-and-toolchain.md` requires every script `ctest` runs to work
# on **bash 3.2** -- macOS ships a 2007 `/bin/bash` and a script in the default
# set runs on every platform CI builds. The rule was added after a `mapfile` took
# `merge-queue-contexts` red on `macOS-clang-release`.
#
# ## MEASURED, because #379's premise turned out to be false
#
# #379 said the macOS leg most likely proved the checks and not the constraint,
# on the ground that Homebrew's bash 5.x sits ahead of `/bin/bash` on GitHub's
# image. **It does not.** Both spellings were registered side by side and run on
# `macOS-clang-release` (PR #806, 2026-09-05):
#
#   COMMAND bash       -> /bin/bash  3.2.57(1)-release  mapfile UNSUPPORTED
#   COMMAND /bin/bash  -> /bin/bash  3.2.57(1)-release  mapfile UNSUPPORTED
#
# The same interpreter, and it is the 2007 one. The rule HAD an executor the whole
# time; nobody had checked. So this check is not repairing a hole -- it is pinning
# a property that was true by accident.
#
# That is still worth pinning, and the reason is measurable too: CMake does NOT
# resolve a bare command name at configure time. `CTestTestfile.cmake` keeps the
# literal `"bash"`, so the interpreter is chosen from `PATH` at RUN time and would
# move the day the image gains a newer bash -- which brew pulls in as a dependency
# of other formulae often enough. Naming `/bin/bash` removes a live dependency on
# PATH ordering rather than fixing a present defect.
#
# Condition on those figures: this repository's macOS job, its brew installs, on
# that date. They are a property of the image, not of the registration.
#
# `${FASTCACHED_BASH}` is `/bin/bash` everywhere but Windows, which has none and
# keeps the PATH lookup. Defined once in `src/tests/CMakeLists.txt`; this asserts
# every site uses it.
#
# ## Why the SET is derived and never listed
#
# #379's body named six call sites. The tree had **thirty**, three of the
# unlisted scripts predate the ticket, and one of the thirty is not a `COMMAND`
# at all -- `sccache-smoke.sh` is reached through a `set(_smoke_driver ...)`
# variable, which is the form a reader scanning for `COMMAND bash` walks past.
# Those three are the required `sccache smoke` contexts.
#
# A lane converting the six and reporting the constraint enforced would have left
# twenty-four sites unconverted with every test green. So the set is derived here,
# at check time, from what the file actually contains: a list in a ticket, a
# comment or this script would be a second thing to be wrong, and it would go
# stale in the direction that reports success (#492, #510).
#
# ## And a site it cannot classify is REFUSED, not skipped
#
# A registration form nobody anticipated is exactly how the twenty-fourth site
# hides. Skipping one is indistinguishable from there not being one, so an
# executable reference to a `.sh` whose interpreter token this scan cannot read is
# a violation naming the line -- the answer `check-gated-jobs.sh` rule A already
# gives a classifier read in a shape it does not recognise.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -P scripts/check-script-interpreter.cmake
#
# Exit codes: 0 always. The verdict is `CMake Error` in the output, which is why
# the registration carries FAIL_REGULAR_EXPRESSION.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()

set(testsFile "${FASTCACHED_SOURCE_DIR}/src/tests/CMakeLists.txt")
if(NOT EXISTS "${testsFile}")
    message(FATAL_ERROR "the test registration file is missing: ${testsFile}")
endif()

# The token every site must use, spelled once. `\${` so this is the literal text
# `${FASTCACHED_BASH}` and not an expansion of a variable this script never sets.
set(expectedToken "\${FASTCACHED_BASH}")

# Read and split by hand rather than with `file(STRINGS)`, for the reason
# `check-script-check-signals.cmake` records at length: a `;` makes one line two
# elements and an unbalanced `[` in a COMMENT merges every line after it, and
# going blind does not make a scan fail -- it leaves a smaller set that still
# agrees unanimously.
file(READ "${testsFile}" content)
string(REPLACE ";" "\\;" content "${content}")
string(REPLACE "[" " " content "${content}")
string(REPLACE "]" " " content "${content}")
string(REPLACE "\r\n" "\n" content "${content}")
string(REPLACE "\n" ";" lines "${content}")

# CMP0219 exists only on newer CMake than the declared minimum, so it is set
# behind `if(POLICY ...)`. Unset, a macro called with a backslash in an argument
# warns, and the registration's FAIL_REGULAR_EXPRESSION matches `CMake Warning`
# on purpose -- so the warning alone would take this check red with nothing wrong.
# Measured on `macOS-clang-release` while landing #680.
if(POLICY CMP0219)
    cmake_policy(SET CMP0219 NEW)
endif()

set(violations "")
set(siteCount 0)

# EVERY violation goes through here, and that is a seam rather than care.
#
# A raw `;` inside a `list(APPEND)` element is an ELEMENT BOUNDARY: one finding
# prints as two lines and is COUNTED as two, so the check miscounts its own
# output. #796 filed that after it happened in `check-script-check-signals.cmake`
# -- and it was then written AGAIN, here, within the hour, by the same author who
# had just filed it. Twice is not carelessness twice, it is a missing seam.
#
# The argument is prose and carries no backslash, so the macro is CMP0219-safe at
# every call site; the escaping happens inside, on the way into the list.
macro(fastcached_add_violation text)
    string(REPLACE ";" "\\;" _escapedViolation "${text}")
    list(APPEND violations "${_escapedViolation}")
endmacro()
set(lineNumber 0)
set(sawDefinition FALSE)

foreach(line IN LISTS lines)
    math(EXPR lineNumber "${lineNumber} + 1")
    if(line MATCHES "^[ \t]*#")
        continue()
    endif()

    # The definition itself. Without it every `COMMAND ${FASTCACHED_BASH} x.sh`
    # expands to `COMMAND x.sh`, which RUNS -- the script is executable and has a
    # shebang -- so the tests would pass while the interpreter this check exists
    # to pin was chosen by the kernel instead. A green suite proving nothing,
    # which is why the definition is asserted rather than assumed.
    if(line MATCHES "set\\(FASTCACHED_BASH[ \t]")
        set(sawDefinition TRUE)
    endif()

    # Only executable references. A `.ps1` sibling, or a `.sh` named in prose,
    # is not one -- comments are already gone above.
    if(NOT line MATCHES "\"[^\"]*scripts/[^\"]*\\.sh\"")
        continue()
    endif()

    math(EXPR siteCount "${siteCount} + 1")

    # The token immediately before the quoted path is the interpreter. Both forms
    # the tree actually uses are covered by that one rule: `COMMAND <tok> "x.sh"`
    # and `set(<var> <tok> "x.sh")`.
    if(NOT line MATCHES "([^ \t]+)[ \t]+\"[^\"]*scripts/[^\"]*\\.sh\"")
        string(STRIP "${line}" shownLine)
        fastcached_add_violation(
             "src/tests/CMakeLists.txt:${lineNumber} runs a shell script in a form this scan cannot classify, so it cannot say which interpreter it gets: ${shownLine}")
        continue()
    endif()

    set(token "${CMAKE_MATCH_1}")

    # `COMMAND "x.sh"` matches the pattern with `COMMAND` as the token, and
    # reporting that as the interpreter names the wrong half of a real defect --
    # #791's lesson. There is no interpreter there at all, and that is what the
    # message has to say, because a maintainer sent to look for one finds nothing.
    if(token MATCHES "^(COMMAND|set\\(.*)$")
        string(STRIP "${line}" shownLine)
        fastcached_add_violation(
             "src/tests/CMakeLists.txt:${lineNumber} runs a shell script with NO interpreter token before it, so the kernel picks one from the shebang and this check cannot pin it. Line: ${shownLine}")
    elseif(NOT token STREQUAL "${expectedToken}")
        string(STRIP "${line}" shownLine)
        fastcached_add_violation(
             "src/tests/CMakeLists.txt:${lineNumber} runs a shell script as `${token}` rather than `${expectedToken}`. A bare `bash` is resolved from PATH by ctest at RUN time -- CMake leaves it unresolved in CTestTestfile.cmake -- so which interpreter it gets is a property of the image rather than of this file. On the macOS image today it happens to be the same `/bin/bash` 3.2.57 the bash-3.2 rule is about, measured; naming it removes the dependency on that staying true. A literal `/bin/bash` is refused too: correct today, and broken on Windows the day a row moves. Line: ${shownLine}")
    endif()
endforeach()

# An empty scan is a REFUSAL. Two empty lists agree perfectly, and a registration
# form that stopped matching would otherwise take this whole check silently out of
# service -- which is the failure it exists to prevent, one level up.
if(siteCount EQUAL 0)
    fastcached_add_violation(
         "found no script-driven test registrations at all in src/tests/CMakeLists.txt -- either they are gone or they are spelled in a way this scan does not recognise, and in both cases this check is vouching for nothing")
endif()

if(NOT sawDefinition)
    fastcached_add_violation(
         "src/tests/CMakeLists.txt defines no `FASTCACHED_BASH`, so every site using it expands to no interpreter at all -- the script would still RUN, from its shebang, and every test would stay green while the interpreter was chosen by the kernel rather than pinned here")
endif()

if(violations)
    message("")
    foreach(violation IN LISTS violations)
        message("  ${violation}")
    endforeach()
    message("")
    message("Every script-driven test runs through `\${FASTCACHED_BASH}`, defined once in")
    message("src/tests/CMakeLists.txt. On macOS that is `/bin/bash`, the 2007 interpreter")
    message("the bash-3.2 rule is about; on Windows, which has none, it is a PATH lookup.")
    message("")
    list(LENGTH violations violationCount)
    message(FATAL_ERROR
        "script interpreter: ${violationCount} finding(s) across ${siteCount} script-driven registration(s)")
endif()

message(STATUS
    "script interpreter: all ${siteCount} script-driven registration(s) run through "
    "\${FASTCACHED_BASH}")
