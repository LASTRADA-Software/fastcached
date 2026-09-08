# SPDX-License-Identifier: Apache-2.0
#
# Every reference configuration must reach every platform that has a packaging
# format, not just the one whose maintainer noticed.
#
# `packaging/config/` holds the annotated reference files an operator reads and
# edits. They are platform-NEUTRAL by construction -- no absolute path is ever
# live in one, and a setting that must be live and per-platform is appended by the
# installer, which is the only thing that knows the prefix. That is what lets one
# file be the dpkg conffile, the macOS `.default` template and the MSI `.default`
# template all at once.
#
# Nothing enforced that they all get shipped. `fastcache-compile-node.yaml` arrived
# with #291 as a Linux conffile, and the macOS `.pkg` and the Windows MSI shipped
# NO worker configuration at all until #397 -- so an operator on either platform
# had no annotated reference to start from, and every setting documented only in
# that file was invisible there. Nothing was broken and nothing was logged: the
# file simply was not in two of the three payloads, which reads exactly like a file
# nobody needs.
#
# What this checks, per file under `packaging/config/`:
#
#   1. Linux   -- a `config/<name>|...` row in FASTCACHED_PLATFORM_ASSETS
#   2. macOS   -- a `config/<name>|${FASTCACHED_MACOS_PREFIX}/etc|...` row
#   3. Windows -- an `install(FILES .../config/<name>` under the `if(WIN32)` arm
#
# It reads `packaging/CMakeLists.txt` as TEXT rather than configuring the project,
# for `check-repository-hygiene.cmake`'s reason: it needs no compiler, no generator
# and no platform, so it runs in the default `ctest` set on every host and answers
# about all three platforms from any one of them. A check that could only run on
# the platform it is about is a check that never runs for the other two, which is
# the defect it exists to catch, one level up.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -P scripts/check-config-reference-reach.cmake
#
# Exit codes: 0 = every reference reaches every payload. 1 = at least one does not.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()

set(_dir "${FASTCACHED_SOURCE_DIR}/packaging/config")
set(_table "${FASTCACHED_SOURCE_DIR}/packaging/CMakeLists.txt")

if(NOT IS_DIRECTORY "${_dir}")
    message(FATAL_ERROR
        "check-config-reference-reach: ${_dir} is not a directory. The reference configurations "
        "live there rather than under packaging/linux/ precisely so their name does not say Linux; "
        "if they moved, this check moves with them.")
endif()
if(NOT EXISTS "${_table}")
    message(FATAL_ERROR "check-config-reference-reach: ${_table} does not exist")
endif()

file(GLOB _references RELATIVE "${_dir}" "${_dir}/*.yaml")

# Emptiness is its own failure and a separate assertion, never an inference from
# "no problems found". A glob that stopped matching -- the directory renamed, the
# extension changed -- reports a clean run over nothing at all, which is a check
# announcing success for work it did not do.
list(LENGTH _references _count)
if(_count EQUAL 0)
    message(FATAL_ERROR
        "check-config-reference-reach: no *.yaml under ${_dir}, so this check proved nothing "
        "rather than passing.")
endif()

file(READ "${_table}" _tableText)

# Whole-file substring tests, never a list split: `file(READ)` gives one string and
# there is nothing to neutralise, where `file(STRINGS)` returns a CMake LIST whose
# elements an unbalanced bracket on a kept line merges -- silently dropping
# everything after it. Four readers in this tree have had that bug (#495).
set(_missing "")
foreach(_name IN LISTS _references)
    set(_gaps "")

    if(NOT _tableText MATCHES "\"config/${_name}\\|\\$\\{FASTCACHED_SYSCONF_DIR\\}")
        list(APPEND _gaps "Linux (no FASTCACHED_PLATFORM_ASSETS row installing it to FASTCACHED_SYSCONF_DIR)")
    endif()
    if(NOT _tableText MATCHES "\"config/${_name}\\|\\$\\{FASTCACHED_MACOS_PREFIX\\}")
        list(APPEND _gaps "macOS (no .default row under FASTCACHED_MACOS_PREFIX/etc)")
    endif()
    if(NOT _tableText MATCHES "install\\(FILES \"\\$\\{CMAKE_CURRENT_SOURCE_DIR\\}/config/${_name}\"")
        list(APPEND _gaps "Windows (no install(FILES ...) under the if(WIN32) arm)")
    endif()

    if(_gaps)
        string(REPLACE ";" ", " _gapText "${_gaps}")
        list(APPEND _missing "${_name}: ${_gapText}")
    endif()
endforeach()

# And the property that makes ONE file able to serve three platforms at all: a
# reference ships with nothing in force. Every setting is a comment showing its
# default, so there is no live value -- and therefore no live absolute PATH, which
# is the only thing in these files that could be platform-specific. Measured at the
# commit that added this check: 0 live lines of 285 in fastcached.yaml and 0 of 296
# in fastcache-compile-node.yaml, so the rule holds today and an ordinary edit
# breaks it.
#
# Stated as `nothing is in force` rather than as a path-shaped regex, because a path
# rule underneath this one could never fire, and a check that can only pass vacuously
# reads as coverage. If a reference ever legitimately needs a live setting, a path
# rule is what has to REPLACE this one, and whoever relaxes it meets this paragraph.
#
# What a live per-platform value does instead: the INSTALLER appends it while
# seeding, because the installer is the only thing that knows the prefix. That is
# `storage_path` in packaging/macos/seed-config.sh.inc.
foreach(_name IN LISTS _references)
    file(READ "${_dir}/${_name}" _text)

    # Neutralise `;` BEFORE splitting into a CMake list. Splitting text on newlines
    # into a list makes every embedded semicolon a second separator, so one comment
    # line carrying one -- ordinary English punctuation, and this file's own header
    # has one -- is torn in two and its tail no longer starts with `#`. Measured on
    # the first run: 18 "lines in force" in a file with none, all of them fragments.
    # Same family as the `file(STRINGS)` bracket hazard (#495), one separator over.
    string(REPLACE ";" "@FC_SEMI@" _text "${_text}")
    # ONE backslash each, which is the house form and not a typo waiting to be
    # "fixed": CMake's argument parser turns `\r` and `\n` into the real characters,
    # and its regex engine has no `\r`/`\n` escapes of its own -- it reads `\\r` as a
    # literal `r`. Written the doubled way this splits on the LETTER n, which reports
    # 726 lines in force in a file that has none. Measured, on the first run.
    string(REGEX REPLACE "\r?\n" ";" _lines "${_text}")
    set(_live "")
    foreach(_line IN LISTS _lines)
        string(STRIP "${_line}" _stripped)
        if(NOT "${_stripped}" STREQUAL "" AND NOT _stripped MATCHES "^#")
            list(APPEND _live "${_stripped}")
        endif()
    endforeach()
    if(_live)
        list(GET _live 0 _first)
        string(REPLACE "@FC_SEMI@" ";" _first "${_first}")
        list(LENGTH _live _liveCount)
        message(FATAL_ERROR
            "check-config-reference-reach: ${_name} has ${_liveCount} line(s) in force, starting with: ${_first}. "
            "A shipped reference has NOTHING in force -- every setting is a comment showing its default. "
            "That is not tidiness: it is what lets one file be the Linux conffile, the macOS template and "
            "the MSI template at once, because a file with no live value has no live absolute path and so "
            "nothing that could differ per platform. A setting that must be live AND per-platform is "
            "appended by the installer, which is the only thing that knows the prefix -- see storage_path "
            "in packaging/macos/seed-config.sh.inc.")
    endif()
endforeach()

if(_missing)
    string(REPLACE ";" "\n  " _missingText "${_missing}")
    message(FATAL_ERROR
        "check-config-reference-reach: a reference configuration does not reach every payload.\n"
        "  ${_missingText}\n"
        "A file under packaging/config/ is the annotated reference an operator reads. One that "
        "ships on some platforms and not others is not a smaller feature -- it is a platform whose "
        "operators have no reference at all, and no signal anywhere that they are missing one. "
        "That was #397 for the compile worker, live for four releases. Add the row, or move the "
        "file out of packaging/config/ and say in its own header which platform it is for.")
endif()

message(STATUS
    "check-config-reference-reach: ${_count} reference configuration(s) reach the Linux, macOS and Windows payloads")
