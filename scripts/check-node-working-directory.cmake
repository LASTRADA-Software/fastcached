# SPDX-License-Identifier: Apache-2.0
#
# A unit that runs a COMPILER names a working directory, and names one it creates.
#
# systemd starts a service in `/` when the unit gives it no `WorkingDirectory=`.
# For a daemon that executes nothing that is merely untidy. For the compile worker
# it is a wrong object under a right key:
#
#   A dispatched compile carries the client's compilation directory and the
#   replacement to record for it. The worker spells that as
#   `-fdebug-prefix-map=<directory>=<replacement>` for the client's directory AND
#   for its own, because gcc records the client's and clang records the worker's.
#   A prefix-map rule appends the unmatched tail, so `<from>` = `/` matches every
#   absolute path in the object and keeps the rest: `/usr/include/stdio.h` becomes
#   `.usr/include/stdio.h`. Measured on gcc 14.2.0 against a node running in `/`,
#   `DW_AT_comp_dir` came back `.tmp/.../client` and every system header read
#   `.usr/include/...`.
#
# `WorkerPrefixMapRules` drops the worker's own rule when the worker's directory
# contains the client's, which is what stops a node installed from an older package
# producing that object -- at the cost of leaving clang's `DW_AT_comp_dir` naming
# this machine. The `WorkingDirectory=` is what buys that back. Issue #674.
#
# WHY A SCAN AND NOT A TEST THAT RUNS THE UNIT
#
# Nothing else can see this. Running the real unit needs root and a live systemd,
# so it is reachable only from the packaging job -- which runs twenty minutes in,
# is not a required context, and therefore reports to nobody (#684). And no
# end-to-end fixture can substitute: every one of them starts the node from the
# fixture's own working directory, which is exactly why #674 reached master with a
# dedicated dispatch e2e already green. A `cmake -P` scan of the shipped file is
# the only instrument that fails on the same commit that introduces the defect.
#
# WHAT IT REFUSES, AND WHY EACH ONE IS A REAL WAY TO GET THIS WRONG
#
#   no directive               the defect itself: systemd starts the service in `/`
#   only in a comment          a `#`-prefixed line documents an intention and
#                              configures nothing; a check that matched anywhere on
#                              the line would pass the file that caused #674 the
#                              moment somebody wrote a paragraph about it
#   `WorkingDirectory=`        an empty assignment RESETS the setting to its default
#                              in systemd, so a second, later, empty one silently
#                              undoes the first
#   `/`                        the value the defect produces, spelled out
#   `-/some/path`              the `-` prefix means "start anyway if the chdir
#                              fails", and starting anyway means starting in `/`
#   `~`                        the service account's home, which sysusers gives as
#                              `/` -- the bug by another spelling
#   a path nothing creates     the service then fails to start at every boot, which
#                              is a worse outcome than the one being fixed
#
# The last one is why this check reads `RuntimeDirectory=` and `StateDirectory=`
# rather than only looking for a directive: under the `ProtectSystem=strict` these
# units set, a directory the unit does not declare is read-only at best and absent
# at worst, and "add a WorkingDirectory=" is a one-line change that takes the
# service down if the path is wrong.
#
# Runs as `cmake -P`, for the reasons check-repository-hygiene.cmake states at
# length: it compares strings and reports, so a .sh + .ps1 pair would be two
# implementations of one rule differing only in syntax, and cmake is the one tool
# guaranteed present. See check-script-check-signals.cmake for why the verdict is
# read from this script's OUTPUT rather than from its exit code.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -P scripts/check-node-working-directory.cmake
#
# Exit codes: 0 always. The verdict is the presence of `CMake Error` in the output.

cmake_minimum_required(VERSION 3.28)

# ---------------------------------------------------------------------------
# One row per shipped systemd unit:
#
#   <path under packaging/>|<execs a compiler: yes|no>|<why that answer>
#
# The middle column is a CLAIM about the unit and is not derivable from it -- what
# `ExecStart=` names is a program this project builds, and whether that program
# spawns a compiler is a fact about the program. So it is tabulated, with the
# reason beside it, and a unit whose answer changes is a row to edit rather than a
# behaviour to rediscover.
#
# A `no` row is not unchecked. It is exempt from needing a directive, and still
# refused if it names one that would not work -- because the failure mode of a
# broken `WorkingDirectory=` (the service never starts) has nothing to do with
# compilers.
#
# Adding a unit is adding a row. A unit named here that does not exist is a
# failure, not a skip: this check's whole value is that it reads the file that
# ships, and a scan matching nothing is the one failure mode a consistency check
# must not have.
#
# No row may contain a ';' -- these are CMake lists, and a semicolon inside a row
# would split it into two.
set(FastCachedUnitWorkingDirectories
    "linux/fastcache-compile-node.service|yes|The compile worker. Its entire purpose is to spawn a compiler, and the prefix-map rules it builds are derived from this directory."
    "linux/fastcached.service|no|The cache daemon. It executes nothing at all, so no prefix-map rule is ever derived from its directory and a working directory of / costs it nothing but a busy mount point."
    "linux/fastcached-user.service|no|The per-user cache daemon. Executes nothing, exactly as the system daemon does not."
)

# systemd's own directory roots, so `RuntimeDirectory=x` is checked against the
# path it actually creates. Both are fixed for a SYSTEM unit, which every row
# above is; a user unit's runtime root is $XDG_RUNTIME_DIR, and a row for one
# would need this table to gain a scope column rather than these values to be
# loosened.
set(FastCachedRuntimeDirectoryRoot "/run")
set(FastCachedStateDirectoryRoot "/var/lib")

# `%t` is the runtime root, and is the one specifier a unit may reasonably spell
# here. Every other `%` is refused by name rather than passed through: a specifier
# this script cannot resolve is a value it cannot judge, and reporting on a value
# you did not resolve is how a check comes to approve of `%h` -- the account's
# home, which sysusers gives as `/`.
set(FastCachedResolvableSpecifier "%t")

# What an absolute path may be spelled with. Anything else is refused by name.
# Deliberately narrow: this is a path in a file this project ships, not a path an
# operator typed, so there is no case to accommodate. It is also what keeps a
# value carrying a CMake list separator from being judged as though it were a
# path -- see the substitution in fastcached_unit_directive below.
set(FastCachedPathCharacters "^/[A-Za-z0-9._/+-]*$")

# ---------------------------------------------------------------------------

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR
        "FASTCACHED_SOURCE_DIR is not set. Invoke this script as: cmake "
        "-DFASTCACHED_SOURCE_DIR=<source root> -P ${CMAKE_CURRENT_LIST_FILE}")
endif()

if(NOT EXISTS "${FASTCACHED_SOURCE_DIR}/packaging")
    message(FATAL_ERROR
        "'${FASTCACHED_SOURCE_DIR}' has no packaging/ directory. "
        "Is FASTCACHED_SOURCE_DIR the source root?")
endif()

# Split a "a|b|c" row into its three fields. Positional rather than named because
# a row is three values in a fixed order, and CMake has no record type to give
# them names with.
#
# @param row The raw row text.
# @param unitOut Set to field 1, the unit's path under packaging/.
# @param compilesOut Set to field 2, whether the unit's program spawns a compiler.
# @param reasonOut Set to field 3, why that answer (printed on failure).
function(fastcached_split_unit_row row unitOut compilesOut reasonOut)
    string(REPLACE "|" ";" fields "${row}")
    list(LENGTH fields fieldCount)
    if(NOT fieldCount EQUAL 3)
        message(FATAL_ERROR "Malformed row (expected 3 '|'-separated fields, got ${fieldCount}): ${row}")
    endif()
    list(GET fields 0 rowUnit)
    list(GET fields 1 rowCompiles)
    list(GET fields 2 rowReason)
    set(${unitOut} "${rowUnit}" PARENT_SCOPE)
    set(${compilesOut} "${rowCompiles}" PARENT_SCOPE)
    set(${reasonOut} "${rowReason}" PARENT_SCOPE)
endfunction()

# Every assignment of one directive, in file order, ignoring commented lines.
#
# The pattern requires a line break and then only horizontal whitespace before the
# name, which is what excludes a comment: a `#` between the two prevents the match.
# That is the rule #723 records -- a comment is not a call site -- and it matters
# here more than usual, because the fix for #674 is a directive surrounded by a
# long paragraph explaining it.
#
# A leading newline is prepended so a directive on the very first line is still
# reachable. `string(REGEX MATCHALL)` has no multiline anchor.
#
# A ';' is substituted out first. MATCHALL returns a LIST, so a semicolon anywhere
# inside a match would split that match in two and the caller would judge a
# fragment. The replacement leaves characters the path rule refuses, so such a
# value is reported rather than silently mangled.
#
# @param content The unit file's text.
# @param directive The directive name, e.g. WorkingDirectory.
# @param valuesOut Set to the list of assigned values, in file order, verbatim.
function(fastcached_unit_directive content directive valuesOut)
    string(REPLACE ";" "<semicolon>" safe "${content}")
    string(REGEX MATCHALL "[\r\n][ \t]*${directive}[ \t]*=[^\r\n]*" assignments "\n${safe}")
    set(values "")
    foreach(assignment IN LISTS assignments)
        string(REGEX REPLACE "^[\r\n][ \t]*${directive}[ \t]*=[ \t]*" "" value "${assignment}")
        string(REGEX REPLACE "[ \t]+$" "" value "${value}")
        list(APPEND values "${value}")
    endforeach()
    set(${valuesOut} "${values}" PARENT_SCOPE)
endfunction()

# Every directory a unit declares, as the absolute paths systemd creates for them.
# `RuntimeDirectory=` and `StateDirectory=` each take a space-separated LIST of
# names, so one assignment can declare several.
#
# @param content The unit file's text.
# @param pathsOut Set to the list of absolute paths the unit creates.
function(fastcached_unit_created_directories content pathsOut)
    set(paths "")
    foreach(pair IN ITEMS "RuntimeDirectory|${FastCachedRuntimeDirectoryRoot}"
                          "StateDirectory|${FastCachedStateDirectoryRoot}")
        string(REPLACE "|" ";" halves "${pair}")
        list(GET halves 0 directive)
        list(GET halves 1 root)
        fastcached_unit_directive("${content}" "${directive}" declarations)
        foreach(declaration IN LISTS declarations)
            # An empty assignment resets the setting, so it declares nothing and
            # must not contribute an entry of "<root>/".
            if(NOT declaration STREQUAL "")
                string(REGEX REPLACE "[ \t]+" ";" names "${declaration}")
                foreach(name IN LISTS names)
                    if(NOT name STREQUAL "")
                        list(APPEND paths "${root}/${name}")
                    endif()
                endforeach()
            endif()
        endforeach()
    endforeach()
    set(${pathsOut} "${paths}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------

set(violations "")

foreach(row IN LISTS FastCachedUnitWorkingDirectories)
    fastcached_split_unit_row("${row}" unit compiles reason)
    set(path "${FASTCACHED_SOURCE_DIR}/packaging/${unit}")

    if(NOT EXISTS "${path}")
        list(APPEND violations
            "  packaging/${unit}\n      is named by this check's table and does not exist, so nothing was scanned")
        continue()
    endif()

    file(READ "${path}" content)
    fastcached_unit_directive("${content}" "WorkingDirectory" assigned)
    fastcached_unit_created_directories("${content}" created)

    # The LAST assignment is the one in force -- systemd overrides rather than
    # accumulates for this directive -- and an empty one resets to the default,
    # which is `/`. Reading the first, or reading "is one present", both pass a
    # file whose second assignment silently undid the first.
    set(value "")
    list(LENGTH assigned assignedCount)
    if(assignedCount GREATER 0)
        math(EXPR lastIndex "${assignedCount} - 1")
        list(GET assigned ${lastIndex} value)
    endif()

    if(value STREQUAL "")
        if(compiles STREQUAL "yes")
            if(assignedCount GREATER 0)
                list(APPEND violations
                    "  packaging/${unit}\n      assigns WorkingDirectory= with no value, which RESETS it: systemd starts the service in / and every absolute path in a dispatched object is rewritten")
            else()
                list(APPEND violations
                    "  packaging/${unit}\n      names no WorkingDirectory=, so systemd starts it in / -- and a prefix-map rule built from / rewrites every absolute path in a dispatched object")
            endif()
        endif()
        # A unit that spawns no compiler and asks for no directory is complete.
        continue()
    endif()

    # The `-` prefix tells systemd to start the service even when the chdir fails,
    # and where it fails the service starts in `/`. That is the defect with a
    # directive present to hide it, which is worse than the defect.
    if(value MATCHES "^-")
        list(APPEND violations
            "  packaging/${unit}\n      spells WorkingDirectory=${value}: the '-' prefix starts the service anyway when the chdir fails, and 'anyway' means in /")
        continue()
    endif()

    # Resolved before anything is judged, so no rule below reports on a string
    # whose meaning it does not know.
    string(REPLACE "${FastCachedResolvableSpecifier}" "${FastCachedRuntimeDirectoryRoot}" resolved "${value}")
    if(resolved MATCHES "%")
        list(APPEND violations
            "  packaging/${unit}\n      spells WorkingDirectory=${value}, which carries a specifier this check cannot resolve -- and a value nobody resolved is one nobody judged")
        continue()
    endif()

    if(resolved STREQUAL "/")
        list(APPEND violations
            "  packaging/${unit}\n      sets WorkingDirectory=${value}, which is the defect spelled out: a prefix-map rule of /=<replacement> matches every absolute path in the object and keeps its tail")
        continue()
    endif()

    if(NOT resolved MATCHES "${FastCachedPathCharacters}")
        list(APPEND violations
            "  packaging/${unit}\n      sets WorkingDirectory=${value}, which is not an absolute path spelled in the characters this check can judge")
        continue()
    endif()

    if(NOT resolved IN_LIST created)
        list(JOIN created ", " createdText)
        if(createdText STREQUAL "")
            set(createdText "none -- the unit declares no RuntimeDirectory= or StateDirectory=")
        endif()
        list(APPEND violations
            "  packaging/${unit}\n      sets WorkingDirectory=${value}, which the unit does not create (it creates: ${createdText}). Under ProtectSystem=strict a directory the unit does not declare is read-only or absent, and a service whose chdir fails does not start")
    endif()
endforeach()

# ---------------------------------------------------------------------------
if(NOT violations STREQUAL "")
    list(JOIN violations "\n" report)

    set(rulebook "")
    foreach(row IN LISTS FastCachedUnitWorkingDirectories)
        fastcached_split_unit_row("${row}" unit compiles reason)
        string(APPEND rulebook "  packaging/${unit}\n      spawns a compiler: ${compiles}\n      ${reason}\n")
    endforeach()

    message(FATAL_ERROR
        "Shipped systemd unit(s) would run in the wrong directory:\n${report}\n\n"
        "A unit whose program SPAWNS A COMPILER names a WorkingDirectory=, and names one the "
        "unit itself creates through RuntimeDirectory= or StateDirectory=. systemd starts a "
        "service in / when it is given none, and the worker builds -fdebug-prefix-map rules "
        "from that directory: a rule appends the unmatched tail, so /=<replacement> rewrites "
        "EVERY absolute path in a dispatched object -- /usr/include/stdio.h becomes "
        ".usr/include/stdio.h. The cache key is correct, so that object is stored and shared. "
        "Issue #674.\n\nThe units and why each answer is what it is:\n\n"
        "${rulebook}\n"
        "Nothing else observes this: running the real unit needs root and a live systemd, and "
        "every end-to-end fixture starts the node from the fixture's own directory -- which is "
        "how #674 reached master with a dispatch e2e already green.\n"
        "The table lives in ${CMAKE_CURRENT_LIST_FILE}.")
endif()

list(LENGTH FastCachedUnitWorkingDirectories unitCount)
message(STATUS "unit working directories: ${unitCount} shipped unit(s) start where they should")
