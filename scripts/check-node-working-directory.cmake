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
# AND THE STATE DIRECTORY A NODE'S IDENTITY LIVES IN
#
# A second question about the same units, asked here because it reads the same
# directives with the same parser. The compile node keeps its identity key in a state
# directory, and when no `--cluster-dir` names one it takes the platform's default --
# machine-wide for a PRIVILEGED process, the account's own otherwise. The packaged unit
# runs as `User=fastcache-node`, which is not privileged and whose home sysusers gives
# as `/`, so left to itself the machine's service would keep its identity under
# `/.local/state`, which `ProtectSystem=strict` leaves read-only. What makes it the
# machine's directory is `StateDirectory=<name>`: systemd creates it and hands the
# service `STATE_DIRECTORY`, which the node takes -- but only an entry named what
# `NodeStateDirectoryName` says, read out of the source rather than restated here, so a
# rename on either side is refused rather than silently handing over a directory the
# node ignores.
#
#   no StateDirectory=         the service keeps its identity in its account's home
#   a different name           systemd hands one over and the node declines it
#   a later empty assignment   RESETS the list, so nothing is handed over
#
# AND THE DIRECTORY A NODE'S DISK TIER LIVES IN
#
# A third question about the same unit, and the one an operator meets first. `cache_dir`
# has no default -- a disk tier is a resource an operator names -- so the unit cannot
# turn one on. What it owes is somewhere the operator CAN point it, and under
# `ProtectSystem=strict` that is a directory the unit declares. The state directory it
# already declares is not one: the node keeps its identity there and refuses to start
# beside an entry it has no row for ("this build keeps no entry by that name"). The unit
# shipped without a `CacheDirectory=` while the docs told operators to write
# `cache_dir: /var/cache/fastcache-node`, a path the service could not write -- so the
# packaged disk tier was reachable only through a drop-in, and the obvious workaround,
# a directory inside the state directory, stopped the node.
#
# So the unit's `CacheDirectory=` is read, and every `cache_dir` the docs and the shipped
# reference configuration CITE is judged against it: the path an operator is told to
# write must be one the service can write.
#
#   no CacheDirectory=         nothing persistent is writable but the state directory
#   a later empty assignment   RESETS the list, so nothing is created
#   a cited path it does not   the operator follows the docs and the node either cannot
#   create                     open the store or refuses to start
#   no citation in the         zero citations is no verdict, and the file an operator
#   reference configuration    edits is the one that must say where
#
# Blind spot, failing OPEN: only a cited path under /var/ is judged. A page citing
# `/srv/fc` for the packaged service passes, because the same text is right for a node
# started by hand and nothing in it says which one the page means. /var/ is where a
# system service keeps what it writes, so that is where a citation for one is read.
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

# `fastcached_tracked_files`: how the cited docs are found, one answer for every check.
include("${CMAKE_CURRENT_LIST_DIR}/lib/CheckCommon.cmake")

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

# One row per shipped unit whose program keeps a NODE IDENTITY and runs unprivileged:
#
#   <path under packaging/>|<why it needs the directory handed over>
#
# A unit named here that does not exist is a failure, for the table above's reason.
set(FastCachedUnitNodeStateDirectories
    "linux/fastcache-compile-node.service|The compile node, running as the unprivileged fastcache-node account: without StateDirectory= naming its directory it would keep the machine's identity in that account's home, / under sysusers and read-only under ProtectSystem=strict."
)

# Where the node's own name for its state directory is spelled, and the pattern that
# reads it. Read rather than restated: a second spelling here is how the unit and the
# binary would come to disagree with every check green.
set(FastCachedNodeDefaultsHeader "src/apps/fastcache-compile-node/NodeDefaults.hpp")
set(FastCachedNodeStateNamePattern "NodeStateDirectoryName = \"([A-Za-z0-9._-]+)\"")

# One row per shipped unit whose program takes a `cache_dir` and runs under
# ProtectSystem=strict:
#
#   <path under packaging/>|<why it needs a CacheDirectory=>
#
# A unit named here that does not exist is a failure, for the first table's reason.
set(FastCachedUnitNodeCacheDirectories
    "linux/fastcache-compile-node.service|The compile node under ProtectSystem=strict: cache_dir has no default, and the state directory it can write is refused as one, so without CacheDirectory= a disk tier has nowhere persistent to live."
)

# Where an operator is told what to write for `cache_dir`: the reference configuration
# they edit, and the pages. The reference is also the positive control -- it must cite
# one, or a scan finding nothing would read as every citation agreeing.
set(FastCachedCacheDirReference "packaging/config/fastcache-compile-node.yaml")
set(FastCachedCacheDirCitationSources "docs/*.md" "${FastCachedCacheDirReference}")

# A citation: `--cache-dir=<path>`, `--cache-dir <path>` or `cache_dir: <path>`, judged
# only under /var/ (the blind spot above). Group 2 is the path; a sentence's closing
# full stop is trimmed off it before it is judged.
set(FastCachedCacheDirCitationPattern "(--cache-dir[= ]|cache_dir:[ \t]*)(/var/[A-Za-z0-9._/+-]*)")

# systemd's own directory roots, so `RuntimeDirectory=x` is checked against the
# path it actually creates. All are fixed for a SYSTEM unit, which every row
# above is; a user unit's runtime root is $XDG_RUNTIME_DIR, and a row for one
# would need this table to gain a scope column rather than these values to be
# loosened.
set(FastCachedRuntimeDirectoryRoot "/run")
set(FastCachedStateDirectoryRoot "/var/lib")
set(FastCachedCacheDirectoryRoot "/var/cache")

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

# The names a list directive (`StateDirectory=`, `CacheDirectory=`) has in force:
# every assignment appends its space-separated names, and an empty one RESETS the
# list -- so reading "is one present" passes a unit whose last word undid the rest.
#
# @param content The unit file's text.
# @param directive The directive name, e.g. CacheDirectory.
# @param namesOut Set to the names in force, in file order.
# @param assignedOut Set to TRUE when the directive is assigned at all, even emptily.
function(fastcached_unit_list_in_force content directive namesOut assignedOut)
    fastcached_unit_directive("${content}" "${directive}" declarations)
    set(inForce "")
    set(assignedAny FALSE)
    foreach(declaration IN LISTS declarations)
        set(assignedAny TRUE)
        if(declaration STREQUAL "")
            set(inForce "")
        else()
            string(REGEX REPLACE "[ \t]+" ";" names "${declaration}")
            list(APPEND inForce ${names})
        endif()
    endforeach()
    set(${namesOut} "${inForce}" PARENT_SCOPE)
    set(${assignedOut} ${assignedAny} PARENT_SCOPE)
endfunction()

# Every directory a unit declares, as the absolute paths systemd creates for them.
# `RuntimeDirectory=`, `StateDirectory=` and `CacheDirectory=` each take a
# space-separated LIST of names, so one assignment can declare several.
#
# @param content The unit file's text.
# @param pathsOut Set to the list of absolute paths the unit creates.
function(fastcached_unit_created_directories content pathsOut)
    set(paths "")
    foreach(pair IN ITEMS "RuntimeDirectory|${FastCachedRuntimeDirectoryRoot}"
                          "StateDirectory|${FastCachedStateDirectoryRoot}"
                          "CacheDirectory|${FastCachedCacheDirectoryRoot}")
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
            set(createdText "none -- the unit declares no RuntimeDirectory=, StateDirectory= or CacheDirectory=")
        endif()
        list(APPEND violations
            "  packaging/${unit}\n      sets WorkingDirectory=${value}, which the unit does not create (it creates: ${createdText}). Under ProtectSystem=strict a directory the unit does not declare is read-only or absent, and a service whose chdir fails does not start")
    endif()
endforeach()

# ---------------------------------------------------------------------------
# The state directory a node's identity lives in.

set(stateViolations "")
set(nodeStateName "")
if(NOT EXISTS "${FASTCACHED_SOURCE_DIR}/${FastCachedNodeDefaultsHeader}")
    list(APPEND stateViolations
        "  ${FastCachedNodeDefaultsHeader}\n      does not exist, so the node's own name for its state directory was never read")
else()
    file(READ "${FASTCACHED_SOURCE_DIR}/${FastCachedNodeDefaultsHeader}" defaultsText)
    string(REGEX MATCH "${FastCachedNodeStateNamePattern}" found "${defaultsText}")
    if(found STREQUAL "")
        list(APPEND stateViolations
            "  ${FastCachedNodeDefaultsHeader}\n      spells no NodeStateDirectoryName this check can read, so no unit could be judged against it")
    else()
        set(nodeStateName "${CMAKE_MATCH_1}")
    endif()
endif()

if(NOT nodeStateName STREQUAL "")
    foreach(row IN LISTS FastCachedUnitNodeStateDirectories)
        string(REPLACE "|" ";" fields "${row}")
        list(GET fields 0 unit)
        set(path "${FASTCACHED_SOURCE_DIR}/packaging/${unit}")
        if(NOT EXISTS "${path}")
            list(APPEND stateViolations
                "  packaging/${unit}\n      is named by the state-directory table and does not exist, so nothing was scanned")
            continue()
        endif()
        file(READ "${path}" content)
        fastcached_unit_list_in_force("${content}" "StateDirectory" inForce assignedAny)

        if(NOT nodeStateName IN_LIST inForce)
            if(NOT assignedAny)
                list(APPEND stateViolations
                    "  packaging/${unit}\n      declares no StateDirectory=, so systemd hands the node no STATE_DIRECTORY and it keeps the machine's identity in its account's home")
            else()
                list(JOIN inForce " " inForceText)
                list(APPEND stateViolations
                    "  packaging/${unit}\n      declares StateDirectory= '${inForceText}' in force, not '${nodeStateName}' (NodeStateDirectoryName): systemd hands a directory over and the node declines it, so it keeps the machine's identity in its account's home")
            endif()
        endif()
    endforeach()
endif()

if(NOT stateViolations STREQUAL "")
    list(JOIN stateViolations "\n" stateReport)
    set(stateRulebook "")
    foreach(row IN LISTS FastCachedUnitNodeStateDirectories)
        string(REPLACE "|" ";" fields "${row}")
        list(GET fields 0 unit)
        list(GET fields 1 reason)
        string(APPEND stateRulebook "  packaging/${unit}\n      ${reason}\n")
    endforeach()
    # SEND_ERROR, not FATAL_ERROR: the working-directory verdict below still runs, so a unit
    # wrong both ways is reported both ways -- the verdict is the output's `CMake Error`.
    message(SEND_ERROR
        "Shipped systemd unit(s) would not hand the node its state directory:\n${stateReport}\n\n"
        "A unit whose program keeps a node identity and runs as an unprivileged account declares "
        "StateDirectory=<NodeStateDirectoryName>. The node takes the directory systemd hands over "
        "in STATE_DIRECTORY -- but only one named for it -- and without it falls back to the "
        "account's own state directory, which is a second identity at best and read-only at worst.\n\n"
        "${stateRulebook}"
        "The table lives in ${CMAKE_CURRENT_LIST_FILE}.")
endif()

# ---------------------------------------------------------------------------
# The directory a node's disk tier lives in, and the path an operator is told to write.

set(cacheViolations "")
set(createdCaches "")
foreach(row IN LISTS FastCachedUnitNodeCacheDirectories)
    string(REPLACE "|" ";" fields "${row}")
    list(GET fields 0 unit)
    set(path "${FASTCACHED_SOURCE_DIR}/packaging/${unit}")
    if(NOT EXISTS "${path}")
        list(APPEND cacheViolations
            "  packaging/${unit}\n      is named by the cache-directory table and does not exist, so nothing was scanned")
        continue()
    endif()
    file(READ "${path}" content)
    fastcached_unit_list_in_force("${content}" "CacheDirectory" inForce assignedAny)
    if(inForce STREQUAL "")
        if(assignedAny)
            list(APPEND cacheViolations
                "  packaging/${unit}\n      ends its CacheDirectory= assignments with an empty one, which RESETS the list, so nothing is created")
        else()
            list(APPEND cacheViolations
                "  packaging/${unit}\n      declares no CacheDirectory=, so under ProtectSystem=strict a cache_dir has nowhere persistent to live but the state directory, which the node refuses")
        endif()
    endif()
    foreach(name IN LISTS inForce)
        list(APPEND createdCaches "${FastCachedCacheDirectoryRoot}/${name}")
    endforeach()
endforeach()
list(JOIN createdCaches ", " createdCachesText)
if(createdCachesText STREQUAL "")
    set(createdCachesText "none")
endif()

fastcached_tracked_files("${FASTCACHED_SOURCE_DIR}"
    PATHSPECS ${FastCachedCacheDirCitationSources}
    GLOBS ${FastCachedCacheDirCitationSources}
    FILES_OUT citationSources
    MODE_OUT citationMode
    CONTAINING "cache-dir" "cache_dir"
    CONTAINING_OUT citingSources)

set(citationCount 0)
set(referenceCitations 0)
foreach(source IN LISTS citingSources)
    file(READ "${FASTCACHED_SOURCE_DIR}/${source}" text)
    # MATCHALL returns a LIST, so a ';' inside a match would split it -- the reason
    # fastcached_unit_directive substitutes it out, and the path rule refuses the stand-in.
    string(REPLACE ";" "<semicolon>" text "${text}")
    string(REGEX MATCHALL "${FastCachedCacheDirCitationPattern}" citations "${text}")
    foreach(citation IN LISTS citations)
        string(REGEX MATCH "${FastCachedCacheDirCitationPattern}" ignored "${citation}")
        string(REGEX REPLACE "[.]+$" "" cited "${CMAKE_MATCH_2}")
        math(EXPR citationCount "${citationCount} + 1")
        if("${source}" STREQUAL "${FastCachedCacheDirReference}")
            math(EXPR referenceCitations "${referenceCitations} + 1")
        endif()
        # The directory itself or anything under it: a store path inside it is just as writable.
        set(writable FALSE)
        foreach(created IN LISTS createdCaches)
            string(FIND "${cited}/" "${created}/" at)
            if(at EQUAL 0)
                set(writable TRUE)
                break()
            endif()
        endforeach()
        if(NOT writable)
            list(APPEND cacheViolations
                "  ${source}\n      cites cache_dir ${cited}, which the unit does not create (its CacheDirectory= creates: ${createdCachesText}). An operator who writes it gets a store the service cannot open, or -- inside the state directory -- a node that refuses to start")
        endif()
    endforeach()
endforeach()

if(NOT EXISTS "${FASTCACHED_SOURCE_DIR}/${FastCachedCacheDirReference}")
    list(APPEND cacheViolations
        "  ${FastCachedCacheDirReference}\n      does not exist, so the file an operator edits was never read")
elseif(referenceCitations EQUAL 0)
    list(APPEND cacheViolations
        "  ${FastCachedCacheDirReference}\n      cites no cache_dir under /var/, so the file an operator edits never says where the packaged service keeps a disk tier -- and a scan judging nothing there would read as every citation agreeing")
endif()

if(NOT cacheViolations STREQUAL "")
    list(JOIN cacheViolations "\n" cacheReport)
    set(cacheRulebook "")
    foreach(row IN LISTS FastCachedUnitNodeCacheDirectories)
        string(REPLACE "|" ";" fields "${row}")
        list(GET fields 0 unit)
        list(GET fields 1 reason)
        string(APPEND cacheRulebook "  packaging/${unit}\n      ${reason}\n")
    endforeach()
    # SEND_ERROR for the state verdict's reason: the working-directory verdict below still runs.
    message(SEND_ERROR
        "Shipped systemd unit(s) and the cache_dir an operator is told to write disagree:\n${cacheReport}\n\n"
        "A unit whose program takes a cache_dir declares CacheDirectory=, and every cache_dir the "
        "docs and the reference configuration cite under /var/ is a directory it creates. Under "
        "ProtectSystem=strict nothing else persistent is writable but the state directory, and the "
        "node refuses to start beside an entry there it has no row for -- so a cited path the unit "
        "does not create is a store the service cannot open, or a node that will not start.\n\n"
        "${cacheRulebook}"
        "Cited paths were read from: ${FastCachedCacheDirCitationSources} (${citationMode}).\n"
        "The table lives in ${CMAKE_CURRENT_LIST_FILE}.")
endif()

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
        "unit itself creates through RuntimeDirectory=, StateDirectory= or CacheDirectory=. systemd starts a "
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

if(NOT stateViolations STREQUAL "" OR NOT cacheViolations STREQUAL "")
    return()
endif()
list(LENGTH FastCachedUnitWorkingDirectories unitCount)
list(LENGTH FastCachedUnitNodeStateDirectories stateUnitCount)
list(LENGTH FastCachedUnitNodeCacheDirectories cacheUnitCount)
list(LENGTH citationSources sourceCount)
message(STATUS "unit working directories: ${unitCount} shipped unit(s) start where they should, "
               "${stateUnitCount} hand the node its state directory, ${cacheUnitCount} create the "
               "directory ${citationCount} cache_dir citation(s) across ${sourceCount} file(s) name "
               "(${citationMode})")
