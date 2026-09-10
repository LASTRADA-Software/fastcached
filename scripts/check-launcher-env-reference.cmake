# SPDX-License-Identifier: Apache-2.0
#
# The launcher's documented environment table must name every variable the launcher
# reads, and no variable it does not.
#
# `EnvName` in `src/apps/fastcache-cc/LauncherCli.hpp` holds the names, and
# `EnvironmentTable` in `LauncherCli.cpp` is what drives parsing and `--help` from
# them. `docs/tools/fastcache-cc.md` is what somebody reading the documentation site
# gets instead. Nothing linked the two, and the page said so in prose:
#
#   > `fastcache-cc --help` documents the same set, generated from the table in
#   > `LauncherCli.cpp` ... if the two ever disagree, `--help` is right.
#
# A sentence that names a hazard and does nothing about it. A variable added to the
# table and never written into the page is undocumented for every reader who is not
# at a terminal, with a fully green suite; a row on the page for a name the launcher
# no longer reads is worse, because it is advice that silently does nothing.
#
# The daemon has had exactly this check since `check-node-config-reference.cmake`,
# whose argument for `cmake -P` applies here unchanged: it is a string comparison
# over two files, needs no compiler, daemon or socket, and belongs in the default
# `ctest` set. This is #881, and #700 is why it exists -- adding one variable meant
# writing it into three unconnected places.
#
# ## Why this reader does not use `file(STRINGS)`
#
# The page is markdown and every other line carries a `[link](target)`. `file(STRINGS)`
# yields a CMake LIST, and a `]` in an element merges it with its neighbour -- the
# hazard `.agent/rules/build-and-toolchain.md` records per (reader, file, surviving
# lines), where two of six readers failed SILENTLY. Blanking the brackets is wrong
# here for the same reason it was wrong for `check-tsan-scope`: they are the data.
#
# So the whole file is read as one string and only the NAMES are matched out of it.
# Every captured substring is `FASTCACHE_[A-Z_]+`, which cannot contain a bracket, so
# there is no element for a stray `]` to merge with. Immune by construction rather
# than by the corpus happening to be safe today.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -P scripts/check-launcher-env-reference.cmake
#
# Exit codes: 0 = the two agree. 1 = a name is in one and not the other, or either
# scan matched nothing.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()

set(_header "${FASTCACHED_SOURCE_DIR}/src/apps/fastcache-cc/LauncherCli.hpp")
set(_page "${FASTCACHED_SOURCE_DIR}/docs/tools/fastcache-cc.md")

foreach(_file "${_header}" "${_page}")
    if(NOT EXISTS "${_file}")
        message(FATAL_ERROR "check-launcher-env-reference: ${_file} does not exist")
    endif()
endforeach()

# The names the launcher reads, taken from the declarations themselves. Anchored on
# the `constexpr std::string_view <Name> = "FASTCACHE_..."` shape so a mention in a
# comment -- and this header has several -- is not mistaken for a declaration.
file(READ "${_header}" _headerText)

# A COMMENT is not a declaration. This header carries several sentences naming these
# variables, and a commented-out declaration is exactly the shape of a real one -- so
# everything from `//` to end of line goes first. Two checks in this tree have matched
# their own documentation for want of this step, and the self-test's
# `comment-is-not-a-declaration` case found it here before review did.
string(REGEX REPLACE "//[^\n]*" "" _headerText "${_headerText}")

string(REGEX MATCHALL "constexpr std::string_view [A-Za-z]+ = \"FASTCACHE_[A-Z_]+\"" _decls "${_headerText}")
set(_declared "")
foreach(_decl IN LISTS _decls)
    string(REGEX MATCH "FASTCACHE_[A-Z_]+" _name "${_decl}")
    list(APPEND _declared "${_name}")
endforeach()
list(REMOVE_DUPLICATES _declared)
list(SORT _declared)

# The page's environment table, anchored on a ROW rather than on a mention: the page
# names these variables dozens of times in prose, and prose is not documentation of
# the set. A row begins a line, so the newline is part of the pattern -- CMake's regex
# has no multiline mode and `^` would anchor to the whole file.
file(READ "${_page}" _pageText)
string(REGEX MATCHALL "\n\\| `FASTCACHE_[A-Z_]+` \\|" _rows "${_pageText}")
set(_documented "")
foreach(_row IN LISTS _rows)
    string(REGEX MATCH "FASTCACHE_[A-Z_]+" _name "${_row}")
    list(APPEND _documented "${_name}")
endforeach()
list(REMOVE_DUPLICATES _documented)
list(SORT _documented)

# Two empty lists agree perfectly, so a scan that matched nothing is a REFUSAL and
# never a pass. This is the failure `check-node-config-reference` was given the same
# guard for, and it is the one a reader of a green run cannot see.
list(LENGTH _declared _declaredCount)
list(LENGTH _documented _documentedCount)
if(_declaredCount EQUAL 0)
    message(FATAL_ERROR
            "check-launcher-env-reference: found no FASTCACHE_* declaration in ${_header}. "
            "The scan matched nothing, which is not the same as the two agreeing.")
endif()
if(_documentedCount EQUAL 0)
    message(FATAL_ERROR
            "check-launcher-env-reference: found no FASTCACHE_* table row in ${_page}. "
            "The scan matched nothing, which is not the same as the two agreeing.")
endif()

set(_violations "")
foreach(_name IN LISTS _declared)
    if(NOT "${_name}" IN_LIST _documented)
        list(APPEND _violations
             "${_name} is read by the launcher and has no row in docs/tools/fastcache-cc.md")
    endif()
endforeach()
foreach(_name IN LISTS _documented)
    if(NOT "${_name}" IN_LIST _declared)
        list(APPEND _violations
             "${_name} has a row in docs/tools/fastcache-cc.md and is not read by the launcher")
    endif()
endforeach()

if(_violations)
    set(_text "")
    foreach(_violation IN LISTS _violations)
        string(APPEND _text "\n    ${_violation}")
    endforeach()
    message(FATAL_ERROR
            "check-launcher-env-reference: the launcher and its documented environment table disagree."
            "${_text}\n"
            "  Add or remove the row in docs/tools/fastcache-cc.md, or the declaration in "
            "src/apps/fastcache-cc/LauncherCli.hpp. The table in LauncherCli.cpp drives --help, "
            "so --help is right and the page is what drifts.")
endif()

message(STATUS
        "check-launcher-env-reference: ${_declaredCount} variable(s) read, "
        "${_documentedCount} documented, and they are the same set")
