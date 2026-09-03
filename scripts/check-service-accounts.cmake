# SPDX-License-Identifier: Apache-2.0
#
# Service accounts: one spelling, one creator, one remover.
#
# A service that names an account its platform cannot resolve does not fail
# loudly. On launchd the plist is accepted, `launchctl bootstrap` and `kickstart`
# both return 0, and only the spawn never happens -- which is why
# ServiceControl.cpp refuses at install time instead. That refusal is only as good
# as the agreement between three places that never see each other:
#
#   * the C++ that hands the name to the supervisor,
#   * the packaging that CREATES the account, and
#   * the uninstaller that REMOVES it again.
#
# Each of those has already been the odd one out. Issue #87 is the missing
# creator: `fastcache-compile-node` named `fastcache-node` and the macOS package
# created only `_fastcached`, so a system-scope worker install was refused on the
# one platform that ships the binary. A missing remover is quieter still -- a
# stale account makes the next install pick a DIFFERENT uid, silently orphaning
# the state directory the old one owned.
#
# Why a scan rather than generating the scripts from the C++ (or the other way
# round). Neither can generate the other: the C++ constant is compiled into a
# binary that must answer without the package present, and the postinstall runs on
# a machine where no build tree exists. What can be checked is that they agree,
# and that is what this does -- in milliseconds, on every platform, including the
# ones that cannot run a .pkg at all. That last part is the point: the failure it
# guards is macOS-only, and a check only macOS can run is a check that reports
# after the mistake has already been pushed.
#
# Runs as `cmake -P`, for the reasons check-repository-hygiene.cmake states at
# length: this compares strings and reports, so a .sh + .ps1 pair would be two
# implementations of one rule differing only in syntax, and cmake is the one tool
# guaranteed present.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -P scripts/check-service-accounts.cmake
#
# Exit codes: 0 = every account agrees. 1 = at least one of the three disagrees.

# ---------------------------------------------------------------------------
# One row per service account:
#
#   <cmake variable>|<C++ source that names it>|<why this account exists>
#
# The cmake variable is the single source of truth: it is what the top-level
# CMakeLists.txt declares and what packaging/macos/*.in substitute through
# @VAR@. The C++ source is where the same name is compiled into a ServiceSpec.
#
# Adding a service that runs unprivileged is adding a row here, and the check
# then insists the packaging caught up.
#
# No row may contain a ';' -- these are CMake lists, and a semicolon inside a row
# would split it into two.

cmake_minimum_required(VERSION 3.28)

set(FastCachedServiceAccounts
    "FASTCACHED_MACOS_SERVICE_ACCOUNT|src/FastCache/Platform/ServiceControl.cpp|The fastcached daemon. A system-wide launchd job with no UserName runs as root, and this one owns the cache storage every client's data lands in."
    "FASTCACHED_MACOS_NODE_ACCOUNT|src/apps/fastcache-compile-node/NodeConfig.cpp|The compile worker. Deliberately NOT the daemon's: a worker runs a compiler on input that arrived over the network, so sharing one account would let a compromised compile rewrite every cached object."
)

# The shell function packaging/macos/create-account.sh.inc defines. A postinstall
# is recognised as the creator of an account by calling it -- which also keeps the
# rule that there is ONE implementation of the dscl dance, rather than a copy per
# account differing only in the name it creates.
set(FastCachedCreateAccountHelper "fastcached_create_account")

# Where each side lives. Named rather than globbed at the point of use so a
# renamed file is reported as missing instead of quietly scanning nothing.
set(FastCachedAccountDeclaration "CMakeLists.txt")
set(FastCachedAccountCreatorGlob "packaging/macos/postinstall-*.sh.in")
set(FastCachedAccountRemover "packaging/macos/fastcached-uninstall.sh.in")
set(FastCachedAccountHelperFile "packaging/macos/create-account.sh.inc")

# ---------------------------------------------------------------------------

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR
        "FASTCACHED_SOURCE_DIR is not set. Invoke this script as: cmake "
        "-DFASTCACHED_SOURCE_DIR=<source root> -P ${CMAKE_CURRENT_LIST_FILE}")
endif()

if(NOT EXISTS "${FASTCACHED_SOURCE_DIR}/${FastCachedAccountDeclaration}")
    message(FATAL_ERROR
        "'${FASTCACHED_SOURCE_DIR}' has no ${FastCachedAccountDeclaration}. "
        "Is FASTCACHED_SOURCE_DIR the source root?")
endif()

# Split a "a|b|c" row into its three fields. Positional rather than named because
# a row is three values in a fixed order, and CMake has no record type to give
# them names with.
#
# @param row The raw row text.
# @param variableOut Set to field 1, the cmake variable holding the account name.
# @param sourceOut Set to field 2, the C++ source that must name the same account.
# @param reasonOut Set to field 3, why the account exists (printed on failure).
function(fastcached_split_account_row row variableOut sourceOut reasonOut)
    string(REPLACE "|" ";" fields "${row}")
    list(LENGTH fields fieldCount)
    if(NOT fieldCount EQUAL 3)
        message(FATAL_ERROR "Malformed row (expected 3 '|'-separated fields, got ${fieldCount}): ${row}")
    endif()
    list(GET fields 0 rowVariable)
    list(GET fields 1 rowSource)
    list(GET fields 2 rowReason)
    set(${variableOut} "${rowVariable}" PARENT_SCOPE)
    set(${sourceOut} "${rowSource}" PARENT_SCOPE)
    set(${reasonOut} "${rowReason}" PARENT_SCOPE)
endfunction()

# Read a file whole. `file(READ)` rather than `file(STRINGS)` throughout this
# script, and that is not a preference: file(STRINGS) returns a LIST, so a line
# containing a semicolon -- which is every C++ statement -- splits into two
# elements and a regex anchored on the whole line stops matching. The bug that
# choice produces is a check that passes because it saw nothing.
#
# @param path Absolute path to read.
# @param contentOut Set to the file's contents, or "" when it does not exist.
# @param foundOut Set to TRUE when the file exists.
function(fastcached_read_file path contentOut foundOut)
    if(NOT EXISTS "${path}")
        set(${contentOut} "" PARENT_SCOPE)
        set(${foundOut} FALSE PARENT_SCOPE)
        return()
    endif()
    file(READ "${path}" content)
    set(${contentOut} "${content}" PARENT_SCOPE)
    set(${foundOut} TRUE PARENT_SCOPE)
endfunction()

# Every account name a C++ source spells as a literal, in either shape this tree
# uses: the daemon's `constexpr ... DaemonServiceAccount = "_fastcached"` and the
# worker's designated initialiser `.serviceAccount = "fastcache-node"`. Both end
# in `erviceAccount = "<name>"`, which is what this matches -- either case of the
# leading S, because one shape is a type-scoped constant and the other a member.
#
# A comparison is not an assignment: `spec.serviceAccount == "x"` fails the
# pattern, because after the first `=` comes a second rather than a quote. That
# matters for the daemon's source, which tests emptiness in several places.
#
# MATCHALL returns whole matches rather than captures, so the pattern is kept
# free of semicolons -- it stops at the closing quote -- and the name is peeled
# off afterwards. A list element containing a ';' would split, which is the same
# trap as file(STRINGS) above.
#
# @param content The source text.
# @param namesOut Set to the list of account names found, in order.
function(fastcached_account_literals content namesOut)
    string(REGEX MATCHALL "[sS]erviceAccount[ \t]*=[ \t]*\"[^\"]*\"" assignments "${content}")
    set(names "")
    foreach(assignment IN LISTS assignments)
        string(REGEX REPLACE "^.*\"([^\"]*)\"$" "\\1" name "${assignment}")
        list(APPEND names "${name}")
    endforeach()
    set(${namesOut} "${names}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# The declarations, read once: `set(FASTCACHED_MACOS_..._ACCOUNT "name")`.
fastcached_read_file("${FASTCACHED_SOURCE_DIR}/${FastCachedAccountDeclaration}" declarationText declarationFound)

# The macOS scripts, read once each rather than per row.
file(GLOB creatorPaths LIST_DIRECTORIES false "${FASTCACHED_SOURCE_DIR}/${FastCachedAccountCreatorGlob}")
fastcached_read_file("${FASTCACHED_SOURCE_DIR}/${FastCachedAccountRemover}" removerText removerFound)
fastcached_read_file("${FASTCACHED_SOURCE_DIR}/${FastCachedAccountHelperFile}" helperText helperFound)

set(violations "")

# A scan that finds no scripts would pass every row below by checking nothing --
# the one failure mode a consistency test must not have. Reported first, because
# every other diagnosis would be a consequence of it.
if(creatorPaths STREQUAL "")
    list(APPEND violations
        "  ${FastCachedAccountCreatorGlob}\n      matches no file, so nothing was scanned for account creation")
endif()
if(NOT removerFound)
    list(APPEND violations
        "  ${FastCachedAccountRemover}\n      does not exist, so nothing removes any account")
endif()
if(NOT helperFound)
    list(APPEND violations
        "  ${FastCachedAccountHelperFile}\n      does not exist, so there is no one implementation of account creation to share")
elseif(NOT helperText MATCHES "${FastCachedCreateAccountHelper}[ \t]*\\(")
    list(APPEND violations
        "  ${FastCachedAccountHelperFile}\n      defines no ${FastCachedCreateAccountHelper}() function")
endif()

foreach(row IN LISTS FastCachedServiceAccounts)
    fastcached_split_account_row("${row}" variable source reason)

    # --- the declaration ---------------------------------------------------
    set(account "")
    if(declarationFound AND declarationText MATCHES "set\\([ \t]*${variable}[ \t]+\"([^\"]+)\"")
        set(account "${CMAKE_MATCH_1}")
    endif()
    if(account STREQUAL "")
        list(APPEND violations
            "  ${variable}\n      is not declared as set(${variable} \"<account>\") in ${FastCachedAccountDeclaration}")
        continue()
    endif()

    # --- the C++ that hands the name to the supervisor ----------------------
    fastcached_read_file("${FASTCACHED_SOURCE_DIR}/${source}" sourceText sourceFound)
    if(NOT sourceFound)
        list(APPEND violations "  ${source}\n      is named by ${variable} but does not exist")
    else()
        fastcached_account_literals("${sourceText}" literals)
        list(LENGTH literals literalCount)
        if(literalCount EQUAL 0)
            list(APPEND violations
                "  ${source}\n      spells no service account, but ${variable} says it should spell '${account}'")
        elseif(NOT literalCount EQUAL 1)
            # Two accounts in one file is not something this check can attribute
            # to one row, and guessing would be worse than saying so: add a row.
            list(JOIN literals ", " literalText)
            list(APPEND violations
                "  ${source}\n      spells ${literalCount} service accounts (${literalText}); each one needs a row of its own")
        else()
            list(GET literals 0 literal)
            if(NOT literal STREQUAL account)
                list(APPEND violations
                    "  ${source}\n      compiles in '${literal}' while ${variable} creates '${account}'")
            endif()
        endif()
    endif()

    # --- exactly one creator ------------------------------------------------
    # Exactly one, not at least one: two postinstalls creating the same account
    # would each run the free-id scan, and the second would find the first's
    # record and skip -- but only if it lost the race. macOS does not order
    # component postinstalls, so the outcome would be decided by which ran first.
    set(creators "")
    foreach(creatorPath IN LISTS creatorPaths)
        fastcached_read_file("${creatorPath}" creatorText creatorFound)
        file(RELATIVE_PATH creatorName "${FASTCACHED_SOURCE_DIR}" "${creatorPath}")
        if(creatorText MATCHES "@${variable}@" AND creatorText MATCHES "${FastCachedCreateAccountHelper}[ \t]+")
            list(APPEND creators "${creatorName}")
        endif()
    endforeach()
    list(LENGTH creators creatorCount)
    if(creatorCount EQUAL 0)
        list(APPEND violations
            "  ${account}\n      is named by ${source} and created by no ${FastCachedAccountCreatorGlob}: a system-scope install of it is refused on macOS, or worse, runs as root")
    elseif(NOT creatorCount EQUAL 1)
        list(JOIN creators ", " creatorText)
        list(APPEND violations
            "  ${account}\n      is created by ${creatorCount} postinstalls (${creatorText}); they would race the free-id scan")
    endif()

    # --- and a remover ------------------------------------------------------
    if(removerFound AND NOT removerText MATCHES "@${variable}@")
        list(APPEND violations
            "  ${account}\n      is created but never removed by ${FastCachedAccountRemover}: the next install picks a different uid and orphans what the old one owned")
    endif()
endforeach()

# ---------------------------------------------------------------------------
if(NOT violations STREQUAL "")
    list(JOIN violations "\n" report)

    set(rulebook "")
    foreach(row IN LISTS FastCachedServiceAccounts)
        fastcached_split_account_row("${row}" variable source reason)
        string(APPEND rulebook "  ${variable}\n      named in ${source}\n      ${reason}\n")
    endforeach()

    message(FATAL_ERROR
        "Service account(s) do not agree across the three places that must:\n${report}\n\n"
        "Every unprivileged service account is declared once, in ${FastCachedAccountDeclaration}, "
        "compiled into a ServiceSpec by its own binary, created by exactly one macOS postinstall "
        "through ${FastCachedCreateAccountHelper}(), and deleted again by "
        "${FastCachedAccountRemover}. The accounts and why each exists:\n\n"
        "${rulebook}\n"
        "None of these disagreements fails loudly at runtime: launchd accepts a plist naming a "
        "UserName it cannot resolve, registers the job, and simply never spawns it.\n"
        "The table lives in ${CMAKE_CURRENT_LIST_FILE}.")
endif()

list(LENGTH FastCachedServiceAccounts accountCount)
message(STATUS "service accounts: ${accountCount} account(s) agree across the C++, the postinstalls and the uninstaller")
