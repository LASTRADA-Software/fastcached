# SPDX-License-Identifier: Apache-2.0
#
# `tools-page-installed-set` must be SEEN to refuse each thing it claims to refuse,
# and seen to stay QUIET on the shapes that are merely awkward.
#
# The check exists because the Tools page is hand-written prose that no compiler
# reads. This fixture exists because the CHECK is hand-written CMake that nothing
# else reads either, and a check nobody has watched refuse is not a check -- it is
# decoration that reports green on the day it stops working.
#
# The mutations are applied to a SYNTHESISED tree, never to the tree under test, so
# there is no revert step and nothing can be left behind. Each case asserts its
# mutation actually LANDED before any verdict is drawn from it: a mutation that did
# not apply satisfies a `must not appear` needle perfectly and reports the guard as
# biting when nothing was staged.
#
# The BASELINE is the load-bearing case. Every refusal below is evidence only if the
# unmutated tree passes -- otherwise each case is merely observing the same
# pre-existing failure, and would go on passing if the rule it names were deleted.
#
# ## The three bracket arms
#
# `docs/tools/index.md` is markdown: `[` and `]` everywhere, in links and tables. A
# reader that splits lines into a CMake list merges elements at every unbalanced
# bracket, which is how `net-boundary` passed over a real cross-boundary include and
# `psk-signing-seam` counted 15 calls as 14 -- both SILENTLY, both green.
#
# A clean-tree bracket injection cannot see that. On a clean tree everything a merged
# element swallows is something the check had nothing to say about, so nothing changes
# and the check looks fine (#518 classified `net-boundary` as merely PARTIAL for exactly
# this reason). So there are three arms, and the third is not decoration:
#
#   * `documented not installed` -- the violation alone, proving the check finds it;
#   * `violation hidden behind a bracket` -- the same violation with an unbalanced `[`
#     on the line above, proving a bracket cannot swallow it;
#   * `bracket with no violation` -- the bracket alone, proving the check does not
#     REFUSE on a bracket. Without this last one, a check that rejected every bracket
#     would pass the middle arm for the wrong reason.
#
# A semicolon arm rides alongside for the same reason: `;` is CMake's list separator,
# and a code span containing one is ordinary markdown.
#
# Runs as `cmake -P`. See `check-script-check-signals.cmake` for why such a check
# reports failure through its OUTPUT rather than an exit code.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -DFASTCACHED_SCRATCH_DIR=<dir> \
#         -P scripts/check-tools-page-installed-set-selftest.cmake
#
# Exit codes: 0 always. The verdict is the presence of `CMake Error` in the output.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()
if(NOT DEFINED FASTCACHED_SCRATCH_DIR)
    message(FATAL_ERROR "FASTCACHED_SCRATCH_DIR must be set")
endif()

set(check "${FASTCACHED_SOURCE_DIR}/scripts/check-tools-page-installed-set.cmake")
if(NOT EXISTS "${check}")
    message(FATAL_ERROR "the check under test is missing: ${check}")
endif()

# ---------------------------------------------------------------------------
# The synthesised ground truth.
#
# Five apps, three of them installed -- the real shape, because a fixture whose
# installed set equals its app set could not tell the two-step derivation from a
# one-step one, and the whole point is that the app table alone does not answer
# this question.
set(baseAppTable
"# SPDX-License-Identifier: Apache-2.0
set(FASTCACHED_APPS
    #  directory                   option                          default  description
    \"fastcached|FASTCACHED_BUILD_DAEMON|ON|Build the fastcached daemon\"
    \"fastcache-cc|FASTCACHED_BUILD_LAUNCHER|ON|Build the launcher\"
    \"fastcache-compile-node|FASTCACHED_BUILD_NODE|ON|Build the compile worker\"
    \"compile-cache-testclient|FASTCACHED_BUILD_TESTCLIENT|OFF|Build the test client\"
    \"fastcache-bench|FASTCACHED_BUILD_BENCHMARKS|OFF|Build the micro-benchmarks\"
)
")

# An installed app's rules, and an uninstalled one's. The uninstalled text carries a
# `install(` that is NOT `install(TARGETS`, so a scan matching the looser spelling
# would call the whole tree installed and the fixture would notice.
set(baseInstalledRules
"add_executable(@NAME@ main.cpp)
install(TARGETS @NAME@
    RUNTIME DESTINATION bin
    COMPONENT @NAME@)
")
set(baseUninstalledRules
"add_executable(@NAME@ main.cpp)
# Never installed -- built by CI so it cannot quietly stop compiling.
# install(FILES notes.md DESTINATION share/doc)
")

# The page. Brackets and a table, because that is what the real one has.
set(basePage
"# Tools

The project ships three executables, and these are all of them: every target
carrying an `install()` rule appears below.

## `fastcached` — the cache daemon

The server. Start here: [Quickstart](../getting-started/quickstart.md).

## `fastcache-cc` — the compiler launcher

A drop-in replacement for [ccache](https://ccache.dev/) and
[sccache](https://github.com/mozilla/sccache).

Full reference: [fastcache-cc](fastcache-cc.md).

## `fastcache-compile-node` — the compile worker

Takes translation units that missed the cache and compiles them.

Full reference: [fastcache-compile-node](fastcache-compile-node.md).

## Which do I want?

| Goal | Use |
|------|-----|
| Speed up compiles | `fastcache-cc` + a `fastcached` daemon |
| A memcached- or Redis-compatible cache | `fastcached` alone |
<!-- extra -->
")

# ---------------------------------------------------------------------------
# Stage a tree, apply one mutation, run the check, return its collapsed output.
#
# `omit` is structural rather than textual: it names an app directory whose
# CMakeLists.txt is not written at all, which is the "the table names something the
# tree does not have" case and cannot be expressed as a string replacement.
function(fastcached_stage_and_run name target from to outOutput outApplied)
    set(tree "${FASTCACHED_SCRATCH_DIR}/${name}")
    file(REMOVE_RECURSE "${tree}")

    # Decode the placeholders. `~n~` is a newline; `~lb~`, `~rb~` and `~sc~` are `[`,
    # `]` and `;`.
    #
    # Those three cannot appear literally in a case row, and finding that out is the
    # fixture demonstrating its own subject: writing `[` into the row table below
    # merged the remaining SEVEN rows into one element, because a `set(...)` list
    # literal is exactly the bracket-vulnerable reader this check was written to avoid.
    # The field-count assertion caught it, which is what that assertion is for.
    #
    # Decoded HERE rather than in the table, so the staged tree gets a real bracket
    # while the row stays parseable.
    # Written out rather than driven from a table: a table of replacement PAIRS would
    # itself need a separator, and the separators are the thing being encoded.
    string(REPLACE "~n~" "\n" from "${from}")
    string(REPLACE "~lb~" "[" from "${from}")
    string(REPLACE "~rb~" "]" from "${from}")
    string(REPLACE "~sc~" ";" from "${from}")
    string(REPLACE "~n~" "\n" to "${to}")
    string(REPLACE "~lb~" "[" to "${to}")
    string(REPLACE "~rb~" "]" to "${to}")
    string(REPLACE "~sc~" ";" to "${to}")

    if(to STREQUAL "-")
        set(to "")
    endif()

    set(appTableText "${baseAppTable}")
    set(pageText "${basePage}")
    set(omitted "")
    set(mutateApp "")
    set(applied TRUE)

    if(target STREQUAL "none")
        # Nothing to apply, and nothing to assert about applying it.
    elseif(target STREQUAL "apptable")
        string(FIND "${appTableText}" "${from}" position)
        if(position EQUAL -1)
            set(applied FALSE)
        else()
            string(REPLACE "${from}" "${to}" appTableText "${appTableText}")
        endif()
    elseif(target STREQUAL "page")
        string(FIND "${pageText}" "${from}" position)
        if(position EQUAL -1)
            set(applied FALSE)
        else()
            string(REPLACE "${from}" "${to}" pageText "${pageText}")
        endif()
    elseif(target STREQUAL "omit")
        set(omitted "${from}")
    elseif(target STREQUAL "install")
        # `from` names the app directory whose install-ness is flipped; `to` is
        # `on` or `off`.
        set(mutateApp "${from}")
    else()
        message(FATAL_ERROR "unknown mutation target `${target}` in case `${name}`")
    endif()

    file(WRITE "${tree}/src/apps/CMakeLists.txt" "${appTableText}")
    file(WRITE "${tree}/docs/tools/index.md" "${pageText}")

    # Which apps install, before any mutation: the three real ones.
    set(installedApps "fastcached" "fastcache-cc" "fastcache-compile-node")
    set(allApps "fastcached" "fastcache-cc" "fastcache-compile-node"
                "compile-cache-testclient" "fastcache-bench")

    if(NOT mutateApp STREQUAL "")
        if(NOT mutateApp IN_LIST allApps)
            message(FATAL_ERROR
                "case `${name}` flips install-ness of `${mutateApp}`, which is not one of "
                "the staged apps -- this fixture would stage nothing and the case would "
                "pass by never being reached")
        endif()
        if(to STREQUAL "on")
            list(APPEND installedApps "${mutateApp}")
        elseif(to STREQUAL "off")
            list(REMOVE_ITEM installedApps "${mutateApp}")
        else()
            message(FATAL_ERROR "case `${name}`: install mutation must be `on` or `off`, got `${to}`")
        endif()
    endif()

    foreach(app IN LISTS allApps)
        if(app STREQUAL omitted)
            continue()
        endif()
        if(app IN_LIST installedApps)
            set(rules "${baseInstalledRules}")
        else()
            set(rules "${baseUninstalledRules}")
        endif()
        string(REPLACE "@NAME@" "${app}" rules "${rules}")
        file(WRITE "${tree}/src/apps/${app}/CMakeLists.txt" "${rules}")
    endforeach()

    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DFASTCACHED_SOURCE_DIR=${tree}" -P "${check}"
        OUTPUT_VARIABLE captured ERROR_VARIABLE capturedErrors)
    set(combined "${captured}${capturedErrors}")

    # `message(FATAL_ERROR)` word-wraps at a column that depends on how long the
    # scratch directory's path is, so a multi-word needle can break across a line on
    # one machine and not another. Collapsing runs of whitespace is what makes the
    # needles portable -- and it matters most for the NEGATIVE assertions, where a
    # wrapped line makes a `must not appear` needle pass for free.
    string(REGEX REPLACE "[ \t\r\n]+" " " combined "${combined}")

    set(${outOutput} "${combined}" PARENT_SCOPE)
    set(${outApplied} "${applied}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# The cases.
#
#   <name>|<target>|<from>|<to>|<must ALL appear>|<must NONE appear>
#
# The two needle fields are ' && '-separated, and `-` means none. No field may
# contain a '|' or a ';': the first is the row separator and the second splits the
# CMake list silently, which is how this project's own row tables have lost half a
# sentence three times. The field count is asserted per row below.
set(FastCachedToolsPageCases
    # The baseline. Every refusal below is evidence only if this passes.
    "baseline|none|-|-|3 installed target(s) of 5 app(s)|CMake Error"

    # Direction one, and the #1030 defect: a contributor tool advertised as product.
    "documented not installed|page|<!-- extra -->|## `compile-cache-testclient` — the probe|`compile-cache-testclient` has a section on the Tools page and no `install()` rule|-"

    # Direction two, the quieter mirror: a binary users get and cannot find.
    "installed not documented|install|fastcache-bench|on|`fastcache-bench` is installed and has no section on the Tools page|-"

    # BRACKET ARM 2 of 3: the same violation, with an unbalanced `[` on the line
    # above it. A list-splitting reader merges the heading into the bracket line and
    # reports a clean tree.
    "violation hidden behind a bracket|page|<!-- extra -->|See the guide ~lb~ and a stray bracket~n~~n~## `compile-cache-testclient` — the probe|`compile-cache-testclient` has a section on the Tools page and no `install()` rule|-"

    # BRACKET ARM 3 of 3: the bracket ALONE. Without this, a check that refused every
    # bracket would pass arm 2 for the wrong reason.
    "bracket with no violation|page|<!-- extra -->|A stray ~lb~ bracket and an unmatched ~rb~ one|3 installed target(s) of 5 app(s)|CMake Error"

    # The other list separator. A code span with a semicolon is ordinary markdown.
    "semicolon with no violation|page|<!-- extra -->|Run `a~sc~ b` and see.|3 installed target(s) of 5 app(s)|CMake Error"

    # --- fails CLOSED: four ways the scan can stop working, four distinct repairs ---

    # The app table restructured out from under the scan. Two empty lists agree
    # perfectly, so this must never read as a clean tree.
    "app table matched nothing|apptable|set(FASTCACHED_APPS|set(FASTCACHED_PROGRAMS_RENAMED|the app table matched no rows|-"

    # The table names an app the tree does not have. Unknown is not absent.
    "app named by the table is missing|omit|fastcache-cc|-|does not exist. Whether that app installs anything is therefore unknown|-"

    # Every install rule gone -- a renamed or restructured install idiom.
    "no install rule anywhere|install|fastcached|off|-|-"

    # The page still has headings, but not in the shape this scan reads.
    # Every backticked heading loses its backticks at once. Stripping only ONE leaves
    # the other two readable, which is a MISMATCH rather than a broken scan -- a
    # different verdict, correctly, and the reason this mutation is written to hit all
    # three.
    "heading style changed|page|## `|## |The heading style changed|-"

    # The page emptied or moved.
    "page has no headings|page|## |Section: |has no `##` headings at all|-"
)

# ---------------------------------------------------------------------------
# Run them.
set(failures "")
set(caseCount 0)

foreach(row IN LISTS FastCachedToolsPageCases)
    string(REPLACE "|" ";" fields "${row}")
    list(LENGTH fields fieldCount)
    if(NOT fieldCount EQUAL 6)
        message(FATAL_ERROR
            "case row has ${fieldCount} fields, expected 6 -- a row that does not parse "
            "would run as a different case than it reads as: [${row}]")
    endif()
    list(GET fields 0 caseName)
    list(GET fields 1 caseTarget)
    list(GET fields 2 caseFrom)
    list(GET fields 3 caseTo)
    list(GET fields 4 caseMustAppear)
    list(GET fields 5 caseMustNotAppear)

    math(EXPR caseCount "${caseCount} + 1")

    fastcached_stage_and_run("${caseCount}-${caseTarget}" "${caseTarget}"
                             "${caseFrom}" "${caseTo}" output applied)

    if(NOT applied)
        list(APPEND failures
             "${caseName}: the mutation did not apply -- `${caseFrom}` was not found in the staged tree, so this case asserted nothing")
        continue()
    endif()

    if(NOT caseMustAppear STREQUAL "-")
        string(REPLACE " && " ";" needles "${caseMustAppear}")
        foreach(needle IN LISTS needles)
            string(FIND "${output}" "${needle}" position)
            if(position EQUAL -1)
                list(APPEND failures "${caseName}: expected to see `${needle}` and did not")
            endif()
        endforeach()
    endif()

    if(NOT caseMustNotAppear STREQUAL "-")
        string(REPLACE " && " ";" needles "${caseMustNotAppear}")
        foreach(needle IN LISTS needles)
            string(FIND "${output}" "${needle}" position)
            if(NOT position EQUAL -1)
                list(APPEND failures "${caseName}: did not expect `${needle}` and saw it")
            endif()
        endforeach()
    endif()

    # Every case that names a refusal must actually have produced one, and every case
    # asserting silence must not have. Checked separately from the needles so that a
    # needle appearing in a DIFFERENT failure's text cannot stand in for the verdict.
    string(FIND "${output}" "CMake Error" errorPosition)
    if(caseMustNotAppear MATCHES "CMake Error")
        if(NOT errorPosition EQUAL -1)
            list(APPEND failures "${caseName}: expected the check to pass and it refused")
        endif()
    else()
        if(errorPosition EQUAL -1)
            list(APPEND failures "${caseName}: expected the check to refuse and it passed")
        endif()
    endif()
endforeach()

if(caseCount EQUAL 0)
    message(FATAL_ERROR
        "the case table is empty, so this fixture asserted nothing -- which is exactly "
        "what a green run with no cases looks like")
endif()

if(failures)
    list(LENGTH failures failureCount)
    message("")
    foreach(failure IN LISTS failures)
        message("  ${failure}")
    endforeach()
    message("")
    message("`tools-page-installed-set` did not behave as its own documentation says.")
    message("A check nobody has watched refuse is not a check.")
    message(FATAL_ERROR
        "tools-page-installed-set-selftest: ${failureCount} of ${caseCount} case(s) disagreed with the check")
endif()

message(STATUS "tools-page-installed-set-selftest: ${caseCount} case(s), every verdict as documented")
