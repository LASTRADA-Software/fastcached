# SPDX-License-Identifier: Apache-2.0
#
# The Tools page names exactly the binaries a user installs -- no more, no fewer.
#
# `docs/tools/index.md` is the overview a reader lands on to find out what this
# project gives them to run. What belongs there is the INSTALLED set: the targets
# carrying an `install()` rule, the ones a `.deb`, `.rpm`, `.pkg` or `.msi`
# actually puts on a machine. Everything else under `src/apps/` is contributor
# tooling that is built by CI and shipped to nobody.
#
# Nothing connected the two. `compile-cache-testclient` -- `FASTCACHED_BUILD_TESTCLIENT`
# OFF by default, no `install()` rule, in no package -- sat on that page as the third
# peer of `fastcache-cc` and `fastcache-compile-node` and was published to users at
# `/fastcached/tools/compile-cache-testclient/` for months
# ([#1030](https://github.com/LASTRADA-Software/fastcached/issues/1030)). It was moved
# to `Internals`, where its sibling `fastcache-bench` already lived. This is the guard
# that was missing, filed as
# [#1035](https://github.com/LASTRADA-Software/fastcached/issues/1035).
#
# ## Why a sentence was not enough
#
# The page now STATES the relation in its own opening line:
#
#   > The project ships three executables, and these are all of them: every target
#   > carrying an `install()` rule appears below.
#
# A sentence is not a check. A fifth internal tool documented there contradicts that
# sentence with nothing to say so -- which is exactly how the fourth one got in, and
# it is this repository's recurring shape: a rule written down in the files that obey
# it, reaching no file that does not.
#
# ## Both directions, because they are different mistakes
#
#   * **Documented but not installed** is the #1030 defect: a contributor harness
#     advertised beside the product. The reader is told to run something no package
#     contains.
#   * **Installed but not documented** is the mirror: a binary users get and cannot
#     find. It is the quieter of the two and nothing else in the tree would report it.
#
# They have different fixes, so they are reported separately rather than as one count.
#
# ## The installed set is DERIVED, twice over
#
# The app directories come from `FASTCACHED_APPS` in `src/apps/CMakeLists.txt` -- the
# table that gates what is built -- and whether each is installed comes from that app's
# own `CMakeLists.txt`. Neither is restated here. A second list is not a cross-check,
# it is a second thing to be wrong, and it drifts silently in the direction of agreeing
# with whatever it was copied from.
#
# That two-step matters: the app table alone cannot answer this question. It lists all
# five apps including the two that ship to nobody, and its `default` column is about
# whether a target BUILDS, not whether it INSTALLS. `compile-cache-testclient` is OFF
# by default AND uninstalled; a target could be ON by default and still uninstalled.
# Only the `install()` rule settles it.
#
# ## It refuses when it matches nothing
#
# Every scan here fails CLOSED. Two empty lists agree perfectly, so a scan whose file
# moved would otherwise report a clean tree in exactly the voice of a correct one --
# `node-config-reference` already refuses that way, for the same reason
# ([#492](https://github.com/LASTRADA-Software/fastcached/issues/492): a list is exact
# about what it knows and silent about what it does not, and silence reads identically
# to complete coverage).
#
# ## The subject is the page's `##` sections, NOT the nav
#
# This is the one a re-implementer gets wrong, so it is written down rather than left
# to be rediscovered.
#
# `fastcached` is installed and has NO page of its own under `Tools` -- the overview's
# section for it routes to the Quickstart. So the `Tools` nav in `mkdocs.yml` lists two
# pages plus an overview, while the installed set is three. A nav-derived check computes
# 2 != 3 and REFUSES A CORRECT TREE.
#
# The overview page's `##` sections are the thing that actually claims to enumerate the
# product. Those are the subject.
#
# ## Read without building a list of lines
#
# `docs/tools/index.md` is full of `[` and `]` -- markdown links and tables -- and a
# CMake list built by splitting its lines merges elements at every unbalanced bracket.
# This repository has measured that class of reader going SILENTLY wrong twice
# (`net-boundary` passed over a real cross-boundary include; `psk-signing-seam` counted
# 15 calls as 14 and still passed).
#
# And the usual remedy is wrong here. Blanking `[`/`]` before splitting is what broke
# `check-tsan-scope`, because a Catch2 tag IS `[async]` -- the brackets were the data.
# So this walks with `FIND`/`SUBSTRING` and never puts a line into a list at all. Only
# the extracted NAMES become list elements, and a target name has no separator in it.
#
# Runs as `cmake -P`: it reads files, compares strings and reports. See
# `check-script-check-signals.cmake` for why such a check reports failure through its
# OUTPUT rather than an exit code.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -P scripts/check-tools-page-installed-set.cmake
#
# Exit codes: 0 always. The verdict is the presence of `CMake Error` in the output.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()

set(appTable "${FASTCACHED_SOURCE_DIR}/src/apps/CMakeLists.txt")
set(toolsPage "${FASTCACHED_SOURCE_DIR}/docs/tools/index.md")

foreach(required "${appTable}" "${toolsPage}")
    if(NOT EXISTS "${required}")
        message("")
        message("  ${required}")
        message("")
        message("That file is what this check reads. Its absence is a moved source root or")
        message("a renamed file, not a clean tree.")
        message(FATAL_ERROR "tools-page-installed-set: a file this check reads does not exist")
    endif()
endforeach()

# Walk `content` line by line and capture group 1 of every line matching `pattern`.
#
# Never builds a CMake list of LINES -- that is the whole point. `[`, `]`, `;` and `\`
# in the text would each merge or split elements, and the two files read here are a
# markdown page and CMake source, which are full of all four. Only the captured names
# reach a list, and a target name contains no separator.
function(fc_capture_lines content pattern outVar)
    set(hits "")
    set(rest "${content}")
    while(TRUE)
        string(FIND "${rest}" "\n" newline)
        if(newline EQUAL -1)
            set(line "${rest}")
        else()
            string(SUBSTRING "${rest}" 0 ${newline} line)
        endif()
        string(REGEX REPLACE "\r$" "" line "${line}")

        if(line MATCHES "${pattern}")
            list(APPEND hits "${CMAKE_MATCH_1}")
        endif()

        if(newline EQUAL -1)
            break()
        endif()
        math(EXPR skip "${newline} + 1")
        string(LENGTH "${rest}" restLength)
        if(skip GREATER_EQUAL restLength)
            break()
        endif()
        string(SUBSTRING "${rest}" ${skip} -1 rest)
    endwhile()
    set(${outVar} "${hits}" PARENT_SCOPE)
endfunction()

# As above, but only while between a line matching `beginPattern` and the next line
# matching `endPattern`.
#
# The app rows must be read from the `FASTCACHED_APPS` table specifically, not from
# anything in that file shaped like `"a|b"`. Ungated, this check matched the row shape
# anywhere in the file -- so renaming the table out from under it changed nothing, and
# its own "the app table matched no rows" guard could never fire. That was found by the
# selftest case written to watch that guard refuse, which is the argument for having
# one.
function(fc_capture_block_lines content beginPattern endPattern pattern outVar)
    set(hits "")
    set(rest "${content}")
    set(inBlock FALSE)
    while(TRUE)
        string(FIND "${rest}" "\n" newline)
        if(newline EQUAL -1)
            set(line "${rest}")
        else()
            string(SUBSTRING "${rest}" 0 ${newline} line)
        endif()
        string(REGEX REPLACE "\r$" "" line "${line}")

        if(inBlock)
            if(line MATCHES "${endPattern}")
                set(inBlock FALSE)
            elseif(line MATCHES "${pattern}")
                list(APPEND hits "${CMAKE_MATCH_1}")
            endif()
        elseif(line MATCHES "${beginPattern}")
            set(inBlock TRUE)
        endif()

        if(newline EQUAL -1)
            break()
        endif()
        math(EXPR skip "${newline} + 1")
        string(LENGTH "${rest}" restLength)
        if(skip GREATER_EQUAL restLength)
            break()
        endif()
        string(SUBSTRING "${rest}" ${skip} -1 rest)
    endwhile()
    set(${outVar} "${hits}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# Side A: the installed set.
#
# Step one -- which app directories exist, from the table that gates them, read as
# THAT table rather than as any line in the file shaped like a row.
file(READ "${appTable}" appTableText)
fc_capture_block_lines("${appTableText}"
                       "^[ \t]*set\\([ \t]*FASTCACHED_APPS([ \t]|$)"
                       "^[ \t]*\\)[ \t]*$"
                       "^[ \t]*\"([A-Za-z0-9_.+-]+)\\|" appDirectories)

if(NOT appDirectories)
    message("")
    message("  No app rows were found in ${appTable}.")
    message("")
    message("The `FASTCACHED_APPS` table is where every executable in this tree is")
    message("declared, one row per app. Zero rows means the table was renamed or")
    message("restructured and this scan is no longer reading it -- which is not the same")
    message("as a tree with no apps, and must not be reported as a pass.")
    message(FATAL_ERROR "tools-page-installed-set: the app table matched no rows and this check cannot conclude")
endif()
list(REMOVE_DUPLICATES appDirectories)
list(SORT appDirectories)

# Step two -- which of those actually install something, from each app's own rules.
set(installed "")
foreach(appDir IN LISTS appDirectories)
    set(appCMake "${FASTCACHED_SOURCE_DIR}/src/apps/${appDir}/CMakeLists.txt")
    if(NOT EXISTS "${appCMake}")
        message("")
        message("  The app table names `${appDir}`, but ${appCMake}")
        message("  does not exist.")
        message("")
        message("Whether that app installs anything is therefore unknown, and an unknown")
        message("is not an absence. Reported as a broken scan rather than folded into the")
        message("uninstalled set, which would silently make this whole check weaker.")
        message(FATAL_ERROR "tools-page-installed-set: an app named by the table has no CMakeLists.txt")
    endif()

    file(READ "${appCMake}" appCMakeText)
    fc_capture_lines("${appCMakeText}"
                     "^[ \t]*install\\(TARGETS[ \t]+([A-Za-z0-9_.+-]+)" appInstalls)
    foreach(target IN LISTS appInstalls)
        list(APPEND installed "${target}")
    endforeach()
endforeach()

if(NOT installed)
    message("")
    message("  No `install(TARGETS ...)` rule was found under any app directory.")
    message("")
    message("This project installs three binaries. Zero means the rules moved, were")
    message("renamed, or are now written in a shape this scan cannot read. It is not")
    message("evidence that the Tools page is correct.")
    message(FATAL_ERROR "tools-page-installed-set: no install rule was found and this check cannot conclude")
endif()
list(REMOVE_DUPLICATES installed)
list(SORT installed)

# ---------------------------------------------------------------------------
# Side B: the documented set.
file(READ "${toolsPage}" toolsPageText)
fc_capture_lines("${toolsPageText}" "^##[ \t]+`([A-Za-z0-9_.+-]+)`" documented)

if(NOT documented)
    # Separate the two ways this happens, because they are different repairs: a page
    # that lost its tool sections, and a page that still has them in a shape this scan
    # no longer recognises.
    fc_capture_lines("${toolsPageText}" "^##[ \t]+(.*)$" anyHeadings)
    message("")
    if(anyHeadings)
        list(LENGTH anyHeadings headingCount)
        message("  ${toolsPage} has ${headingCount} `##` heading(s), and none of them names")
        message("  a tool as `## \\`name\\``.")
        message("")
        message("The heading style changed. This scan reads the backtick-quoted form, so it")
        message("is now blind to the sections it exists to check -- report that, rather than")
        message("a clean tree.")
    else()
        message("  ${toolsPage} has no `##` headings at all.")
        message("")
        message("That is a moved or emptied page, not a page that documents nothing.")
    endif()
    message(FATAL_ERROR "tools-page-installed-set: the Tools page matched no tool sections and this check cannot conclude")
endif()
list(REMOVE_DUPLICATES documented)
list(SORT documented)

# ---------------------------------------------------------------------------
# The comparison, in both directions.
set(documentedNotInstalled "")
foreach(name IN LISTS documented)
    if(NOT name IN_LIST installed)
        list(APPEND documentedNotInstalled "${name}")
    endif()
endforeach()

set(installedNotDocumented "")
foreach(name IN LISTS installed)
    if(NOT name IN_LIST documented)
        list(APPEND installedNotDocumented "${name}")
    endif()
endforeach()

if(documentedNotInstalled OR installedNotDocumented)
    message("")
    foreach(name IN LISTS documentedNotInstalled)
        message("  `${name}` has a section on the Tools page and no `install()` rule.")
    endforeach()
    foreach(name IN LISTS installedNotDocumented)
        message("  `${name}` is installed and has no section on the Tools page.")
    endforeach()
    message("")
    message("The Tools page is the overview a user lands on to find what they can run,")
    message("and its opening sentence promises it lists every target carrying an")
    message("`install()` rule.")
    message("")
    message("A tool documented but NOT installed is the #1030 defect: contributor")
    message("tooling advertised beside the product, published to readers who cannot")
    message("install it. Move its page under `Internals`, where `fastcache-bench` and")
    message("`compile-cache-testclient` already live, and drop its section here.")
    message("")
    message("A tool installed but NOT documented is the mirror: a binary users get and")
    message("cannot find. Give it a section.")
    message("")
    message("Installed:  ${installed}")
    message("Documented: ${documented}")
    message(FATAL_ERROR "tools-page-installed-set: the Tools page and the installed set disagree")
endif()

list(LENGTH installed installedCount)
list(LENGTH appDirectories appCount)
message(STATUS
    "tools-page-installed-set: ${installedCount} installed target(s) of ${appCount} app(s) "
    "named exactly by the Tools page -- ${installed}")
