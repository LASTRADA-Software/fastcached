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

# The two line-splitting idioms, defined once (#495). They are TWO -- tokenised and
# verbatim -- with opposite intent, and the module says which one a site wants and
# why merging them would break whichever family it did not choose, silently.
include("${CMAKE_CURRENT_LIST_DIR}/lib/CheckCommon.cmake")

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

# Scripts NOT bound by the bash-3.2 rule, and the ONE other token each may use.
#
# One row per exemption, `script|token|reason`, matched per row. A bare allowlist of
# script names would be a hole: it would let `tidy-sweep.sh` be run as a literal
# `/bin/bash`, or as a bare `bash` resolved from PATH at run time, which is the whole
# defect this check exists to refuse. The TOKEN is part of the row, so an exemption
# permits exactly one substitution and not "anything goes for this file".
#
# An exemption outlives the shape it was granted for, so it is deleted with the shape
# rather than reworded.
set(interpreterExemptions
    "tidy-sweep.sh|\${FASTCACHED_BASH44}|declares a bash-4.4 floor and exits 77 below one, so the 3.2 pin asserts a rule it is not bound by and reports SKIPPED on macOS in perpetuity (#598). FASTCACHED_BASH44 is version-CHECKED rather than merely found, because a stock macOS find_program returns /bin/bash 3.2 quite happily."
)

# Read and split by hand rather than with `file(STRINGS)`, for the reason
# `check-script-check-signals.cmake` records at length: a `;` makes one line two
# elements and an unbalanced `[` in a COMMENT merges every line after it, and
# going blind does not make a scan fail -- it leaves a smaller set that still
# agrees unanimously.
file(READ "${testsFile}" content)
fastcached_split_lines_verbatim("${content}" lines)

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
# Exempt sites are counted and NAMED, so the summary cannot claim a pin it does not have.
set(exemptCount 0)
set(exemptSites "")

# EVERY violation goes through here, and that is a seam rather than care.
#
# A raw `;` inside a `list(APPEND)` element is an ELEMENT BOUNDARY: one finding
# prints as two lines and is COUNTED as two, so the check miscounts its own
# output. #796 filed that after it happened in `check-script-check-signals.cmake`
# -- and it was then written AGAIN, here, within the hour, by the same author who
# had just filed it. Twice is not carelessness twice, it is a missing seam.
#
# The argument is prose and carries no backslash, so this is CMP0219-safe at every call
# site; the escaping happens inside, on the way into the list.
#
# A FUNCTION and not a macro, and that is a fix rather than a preference. A macro
# substitutes its arguments TEXTUALLY, so `${text}` re-expands the argument's own
# content -- and every message here interpolates `${expectedToken}`, whose value is the
# literal string `${FASTCACHED_BASH}`. This script deliberately never sets that variable
# (see the `\${` note above), so the second expansion resolved it to nothing and the
# violation read:
#
#     runs a shell script as `bash` rather than ``
#
# The one message whose whole job is to name the token a maintainer must use could not
# name it. Measured on this tree before the change. A function binds `text` as a real
# variable, so `${text}` dereferences once and the token survives.
# `"${violations}"` QUOTED on the way in. Unquoted, the accumulated list expands into
# separate arguments and the `\` escapes stored by earlier calls are consumed, so
# re-joining re-splits every element that contains a `;` -- the #796 miscount this seam
# exists to prevent, reintroduced by the very conversion that fixed the message. Measured
# against a tree with two bad registrations, one of whose lines carries a `;`: 3 findings
# unquoted becomes 4. The escape and the expansion have to agree, and only one of them was
# being thought about.
function(fastcached_add_violation text)
    string(REPLACE ";" "\\;" _escapedViolation "${text}")
    set(_grown "${violations}")
    list(APPEND _grown "${_escapedViolation}")
    set(violations "${_grown}" PARENT_SCOPE)
endfunction()
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

    # Is the script on this line exempt, and if so which single token may it use?
    #
    # Matched on the basename as it appears in the quoted path, so a row cannot be
    # satisfied by a different file whose name merely contains it.
    set(_exemptToken "")
    set(_exemptScript "")
    set(_exemptReason "")
    foreach(exemption IN LISTS interpreterExemptions)
        string(FIND "${exemption}" "|" _bar1)
        string(SUBSTRING "${exemption}" 0 ${_bar1} _rowScript)
        math(EXPR _afterBar1 "${_bar1} + 1")
        string(SUBSTRING "${exemption}" ${_afterBar1} -1 _rowRest)
        string(FIND "${_rowRest}" "|" _bar2)
        string(SUBSTRING "${_rowRest}" 0 ${_bar2} _rowToken)
        math(EXPR _afterBar2 "${_bar2} + 1")
        string(SUBSTRING "${_rowRest}" ${_afterBar2} -1 _rowReason)
        # The script name is a literal, not a pattern. Unescaped, `tidy-sweep.sh`'s dots
        # are wildcards -- harmless against today's tree and the same mistake as a `pkill
        # -f` pattern read as a literal, in a third instrument.
        string(REPLACE "." "\\." _rowScriptRegex "${_rowScript}")
        if(line MATCHES "\"[^\"]*scripts/${_rowScriptRegex}\"")
            set(_exemptToken "${_rowToken}")
            set(_exemptScript "${_rowScript}")
            set(_exemptReason "${_rowReason}")
            break()
        endif()
    endforeach()

    # `COMMAND "x.sh"` matches the pattern with `COMMAND` as the token, and
    # reporting that as the interpreter names the wrong half of a real defect --
    # #791's lesson. There is no interpreter there at all, and that is what the
    # message has to say, because a maintainer sent to look for one finds nothing.
    if(token MATCHES "^(COMMAND|set\\(.*)$")
        string(STRIP "${line}" shownLine)
        fastcached_add_violation(
             "src/tests/CMakeLists.txt:${lineNumber} runs a shell script with NO interpreter token before it, so the kernel picks one from the shebang and this check cannot pin it. Line: ${shownLine}")
    elseif(NOT token STREQUAL "${expectedToken}" AND NOT _exemptToken STREQUAL "")
        # An exempt script, run through the one token its row permits. Counted as a site
        # so the census below still sees it -- an exemption removes the OBJECTION, never
        # the file, or the "found no registrations at all" floor could be walked past by
        # exempting everything.
        math(EXPR exemptCount "${exemptCount} + 1")
        list(APPEND exemptSites "${_exemptScript} -> ${_exemptToken}")
        if(NOT token STREQUAL "${_exemptToken}")
            string(STRIP "${line}" shownLine)
            fastcached_add_violation(
                 "src/tests/CMakeLists.txt:${lineNumber} runs `${_exemptScript}` as `${token}`. That script is exempt from the `${expectedToken}` pin, but the exemption names ONE permitted token, `${_exemptToken}`, and this is not it. An exemption that admitted any token would readmit exactly what this check refuses: a literal path that breaks on another platform, or a bare `bash` resolved from PATH by ctest at run time. The row's stated reason is: ${_exemptReason} Line: ${shownLine}")
        endif()
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

# The exempt sites are named, not folded into the total. "all N run through
# ${FASTCACHED_BASH}" stopped being true the moment an exemption existed, and a summary
# that overstates its own coverage is what this check is about one level up -- a reader
# would take the pin as universal and it is not.
math(EXPR pinnedCount "${siteCount} - ${exemptCount}")
if(exemptCount GREATER 0)
    message(STATUS
        "script interpreter: ${pinnedCount} of ${siteCount} script-driven registration(s) run "
        "through \${FASTCACHED_BASH}; ${exemptCount} exempt by a named row: ${exemptSites}")
else()
    message(STATUS
        "script interpreter: all ${siteCount} script-driven registration(s) run through "
        "\${FASTCACHED_BASH}")
endif()
