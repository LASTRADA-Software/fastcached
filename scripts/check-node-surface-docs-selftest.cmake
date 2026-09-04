# SPDX-License-Identifier: Apache-2.0
#
# `node-surface-docs` must be SEEN to refuse each thing it claims to refuse, and
# seen to stay QUIET on the shapes that are merely unusual.
#
# The check exists because the operator port documentation is hand-written prose
# that no compiler reads. This fixture exists because the CHECK is hand-written
# CMake that nothing else reads either, and four of its rules turned out not to
# implement what their own comments said:
#
#   * a numeric count claim (`6-surface table`) fell through the residue rule's
#     character class and was accepted in silence, while the comment beside the
#     number-word table said it was refused as unclassified;
#   * the residue rule took the LEFTMOST match, so an earlier `node surface`
#     masked any numeric claim later in the same sentence;
#   * a fence that merely INVOKES `--print-surfaces` without pasting output was
#     held to the whole table, reporting four missing surfaces for a page that
#     claimed nothing;
#   * only `flags[0]` was extracted, leaving the `discovery` row's second flag
#     outside the guard the flag rule exists to be.
#
# Every one of those was reachable only by mutating the tree. None was visible in
# a green run, and a green run is what the check produced on all four. That is
# the argument for a fixture rather than for more comments -- a rule nobody has
# watched refuse is not a rule, and three of the four defects above were written
# WITH a comment asserting the correct behaviour.
#
# The mutations are applied to a SYNTHESISED tree, never to the tree under test,
# so there is no revert step and nothing can be left behind. Each case asserts
# its mutation actually landed before any verdict is drawn from it: a mutation
# that did not apply satisfies a `must not appear` needle perfectly, and reports
# the guard as biting when nothing was staged.
#
# The BASELINE is the load-bearing case. Every refusal below is evidence only if
# the unmutated tree passes -- otherwise each case is merely observing the same
# pre-existing failure and would go on passing if the rule it names were deleted.
#
# What this does NOT do is judge the real documentation. That verdict is owned by
# `node-surface-docs` itself against the real tree; reproducing a verdict-clean
# corpus here would mean restating that script's pattern and exemption tables,
# and a second copy of a table is not a cross-check -- it is a second thing to be
# wrong, and it would redden this fixture whenever an unrelated row is added.
#
# Runs as `cmake -P`. See `check-script-check-signals.cmake` for why such a check
# reports failure through its OUTPUT rather than an exit code.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -DFASTCACHED_SCRATCH_DIR=<dir> \
#         -P scripts/check-node-surface-docs-selftest.cmake
#
# Exit codes: 0 always. The verdict is the presence of `CMake Error` in the output.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()
if(NOT DEFINED FASTCACHED_SCRATCH_DIR)
    message(FATAL_ERROR "FASTCACHED_SCRATCH_DIR must be set")
endif()

set(check "${FASTCACHED_SOURCE_DIR}/scripts/check-node-surface-docs.cmake")
if(NOT EXISTS "${check}")
    message(FATAL_ERROR "the check under test is missing: ${check}")
endif()

# ---------------------------------------------------------------------------
# The paths the check reads. Spelled once here and derived into every staged
# tree, because a path restated per case is a case that silently stops staging
# what the check reads and passes by never being reached.
set(FastCachedSelftestSourceRel "src/apps/fastcache-compile-node/NodeSurfaces.cpp")
set(FastCachedSelftestHeaderRel "src/apps/fastcache-compile-node/NodeSurfaces.hpp")
set(FastCachedSelftestDocRel "docs/operations/surfaces.md")

# ---------------------------------------------------------------------------
# The synthesised ground truth: four surfaces, three TCP and one UDP, with the
# `discovery` row carrying TWO flags. That second flag is not decoration -- it is
# the shape that made `flags[0]` a defect, so a tree without it would let the
# fixed extraction and the broken one produce identical output.
set(baseSurfaceSource
"NodeSurfaceTable()
{
    return {
        NodeSurface {
            .name = \"node\",
            .flags = { \"--listen-node\", {} },
            .protocol = SurfaceProtocol::Tcp,
        },
        NodeSurface {
            .name = \"admin\",
            .flags = { \"--admin-listen\", {} },
            .protocol = SurfaceProtocol::Tcp,
        },
        NodeSurface {
            .name = \"raft\",
            .flags = { \"--listen-raft\", {} },
            .protocol = SurfaceProtocol::Tcp,
        },
        NodeSurface {
            .name = \"discovery\",
            .flags = { \"--discovery\", \"--discovery-reply-port\" },
            .protocol = SurfaceProtocol::Udp,
        },
    };
}
")

set(baseSurfaceHeader
"enum class NodeSurface : std::uint8_t
{
    Node,
    Admin,
    Raft,
    Discovery,
    Last,
};
")

# The document. It carries every literal the check's rules need: a transcript in
# table order, one port-set count claim, and all five flags. A page missing any
# of them would make the corresponding rule vacuous, which the check refuses by
# name -- so a thin document would fail the baseline rather than exercise it.
set(baseDoc
"# Surfaces

The four-surface table is what the node opens.

Ask the binary:

```console
$ fastcache-compile-node --print-surfaces --listen-node 6675 --listen-raft 6680 --discovery 255.255.255.255:6681
node              0.0.0.0:6675  TCP
admin             -             not served
raft              0.0.0.0:6680  TCP
discovery beacon  0.0.0.0:6681  UDP

notes:
  none
```

Set --admin-listen to serve the admin surface at all.

Three TCP rules are what a firewall needs.

And one UDP rules besides.

The beacon replies from --discovery-reply-port when one is set.

<!-- extra -->
")

# ---------------------------------------------------------------------------
# Run the check against a freshly staged tree carrying one mutation.
#
# The mutation is a plain string replacement so that a case states the DOCUMENT
# TEXT it changes rather than a line number, which drifts the moment the base
# document above gains a sentence. `~n~` stands for a newline: the case table is
# a `|`-separated list and an embedded newline in a row is a field this script
# would have to parse around for no benefit.
#
# @param target `doc` or `src` -- which staged file the replacement applies to.
# @param from The text to replace. Asserted present, or the case reports nothing.
# @param to The replacement.
# @param outOutput Set to the check's combined output, whitespace collapsed.
# @param outApplied Set to TRUE when the mutation was actually made.
function(fastcached_stage_and_run name target from to outOutput outApplied)
    set(tree "${FASTCACHED_SCRATCH_DIR}/${name}")
    file(REMOVE_RECURSE "${tree}")

    set(sourceText "${baseSurfaceSource}")
    set(docText "${baseDoc}")

    string(REPLACE "~n~" "\n" from "${from}")
    string(REPLACE "~n~" "\n" to "${to}")
    if(to STREQUAL "-")
        set(to "")
    endif()

    set(applied TRUE)
    if(target STREQUAL "doc")
        string(FIND "${docText}" "${from}" position)
        if(position EQUAL -1)
            set(applied FALSE)
        else()
            string(REPLACE "${from}" "${to}" docText "${docText}")
        endif()
    elseif(target STREQUAL "src")
        string(FIND "${sourceText}" "${from}" position)
        if(position EQUAL -1)
            set(applied FALSE)
        else()
            string(REPLACE "${from}" "${to}" sourceText "${sourceText}")
        endif()
    elseif(NOT target STREQUAL "none")
        message(FATAL_ERROR "unknown mutation target `${target}` in case `${name}`")
    endif()

    # Stage a page for every count exemption the check carries, at the path
    # that row names and containing the phrase it excuses. Without them the
    # check reports each row as having matched nothing -- correctly, since that
    # rule is what stops a stale exemption excusing whatever takes its wording
    # next -- and the baseline could never be green whatever the rules did.
    #
    # READ from the check, never restated. A copy of that table here would be a
    # second thing to be wrong, and it would redden this fixture every time an
    # unrelated exemption row is added, which is the maintenance cost that gets
    # a fixture deleted.
    foreach(exemptRow IN LISTS FastCachedSelftestExemptions)
        string(REPLACE "|" ";" exemptFields "${exemptRow}")
        list(LENGTH exemptFields exemptFieldCount)
        if(exemptFieldCount GREATER_EQUAL 2)
            list(GET exemptFields 0 exemptPath)
            list(GET exemptFields 1 exemptPhrase)
            if(exemptPath STREQUAL "" OR exemptPhrase STREQUAL "")
                message(FATAL_ERROR
                    "an exemption row read out of the check has an empty path or phrase, so this "
                    "fixture's parse of that table is wrong and no case below would mean anything: "
                    "[${exemptRow}]")
            endif()
            file(APPEND "${tree}/docs/${exemptPath}" "Exempted wording: ${exemptPhrase}\n")
        endif()
    endforeach()

    file(WRITE "${tree}/${FastCachedSelftestSourceRel}" "${sourceText}")
    file(WRITE "${tree}/${FastCachedSelftestHeaderRel}" "${baseSurfaceHeader}")
    file(WRITE "${tree}/${FastCachedSelftestDocRel}" "${docText}")

    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DFASTCACHED_SOURCE_DIR=${tree}" -P "${check}"
        OUTPUT_VARIABLE captured ERROR_VARIABLE capturedErrors)
    set(combined "${captured}${capturedErrors}")
    # `message(FATAL_ERROR)` word-wraps at a column that depends on how long the
    # binary directory's path is, so a multi-word needle can break across a line
    # on one machine and not another. Collapsing runs of whitespace is what makes
    # the needles below portable -- and it matters most for the NEGATIVE
    # assertions, where a wrapped line makes a `must not appear` needle pass for
    # free.
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
# contain a '|' or a ';': the first is the row separator and the second splits
# the CMake list silently, which is how this project's own row tables have lost
# half a sentence three times. The field count is asserted per row below.
set(FastCachedSurfaceSelftestCases
    # The baseline. Every refusal below is evidence only if this passes.
    "baseline|none|-|-|node surface docs: 4 surface(s)|CMake Error"

    # Direction one: a surface the table declares is missing from the transcript.
    "missing surface|doc|raft              0.0.0.0:6680  TCP~n~|-|does not list the `raft` surface|-"

    # Direction two: a port the binary never opens appears in one.
    "undeclared surface|doc|notes:|gossip            0.0.0.0:6699  TCP~n~~n~notes:|declares no such surface|-"

    # Same members, wrong order -- the edit somebody makes while tidying, which
    # changes nothing visible and is not output this binary produces.
    "reordered transcript|doc|admin             -             not served~n~raft              0.0.0.0:6680  TCP|raft              0.0.0.0:6680  TCP~n~admin             -             not served|the order and the membership both come from the table|-"

    # A recognised phrasing whose number has gone stale.
    "stale word count|doc|four-surface table|seven-surface table|claims 7 where|-"

    # DIGITS. The residue rule's character class excluded them, so this passed
    # green while the comment beside the number-word table said it was refused.
    "digit count claim|doc|four-surface table|6-surface table|counts something called a surface|-"

    # The leftmost-match hole: an earlier `<word> surface` collocation used to
    # mask a numeric claim later in the same sentence.
    "claim masked by an earlier collocation|doc|The four-surface table is what the node opens.|The node surface is one of 9 surfaces here.|counts something called a surface|-"

    # A fence that INVOKES the command and pastes no output is documentation of
    # the command, not a transcript of it. This is the false-positive direction:
    # the check used to report all four surfaces missing from a page that made no
    # claim at all, which is the failure mode that gets a check switched off.
    "invocation is not a transcript|doc|<!-- extra -->|```console~n~$ fastcache-compile-node --print-surfaces~n~```|node surface docs: 4 surface(s)|CMake Error"

    # The SECOND flag on the discovery row. Renaming it in the table while every
    # page keeps the old spelling is exactly what the flag rule exists to refuse,
    # and holding only `flags[0]` left it unguarded.
    "second flag renamed|src|--discovery-reply-port|--discovery-answer-port|no scanned document names that flag|-"

    # A partial parse must blame the SCAN, never the documents: reporting every
    # correct page as wrong is how somebody edits the prose to match a broken
    # scanner.
    "partial parse|src|            .name = \"raft\",|            .surfaceName = \"raft\",|-|-"
)

# ---------------------------------------------------------------------------
# The exemption rows the check carries, lifted out of it rather than copied.
#
# The rows are `path|phrase|reason`. Only the first two fields are used here --
# the reason is for a human reading the check -- and a row this parse cannot see
# would show up as the baseline failing rather than as a silent gap, because the
# check itself reports an exemption that matched nothing.
file(READ "${check}" checkText)
# Walked line by line, not lifted with one whole-file match. A `(.*)` spanning
# lines runs to the LAST thing resembling its terminator, which here swept up
# every quoted string after the table as well and staged a page under an empty
# path. That is the reason the check itself walks its own subject, recorded two
# files apart and learned twice.
#
# `[` and `]` are blanked along with `;`, and the omission was the point of #510:
# this split escaped semicolons and left brackets alone, so one UNBALANCED
# bracket anywhere in the check it reads would merge every line after it into a
# single element and the exemption table would silently stop parsing.
#
# It is worth naming what this file is: a SELFTEST, written after #510 was filed,
# reproducing the very defect #510 is about. A rule stated in the file that obeys
# it is never learned by the file that does not -- and here the file that did not
# is the one whose job is to watch a check refuse.
#
# Safe to blank here because the fields this parses are `path|phrase|reason` and
# none is bracketed. Where brackets ARE the data, blanking them is wrong and the
# remedy is a list-free offset walk instead -- see `check-tsan-scope`, whose rows
# are Catch2 tags like `[async]`.
string(REGEX REPLACE "\r\n" "\n" checkText "${checkText}")
string(REPLACE "\\" " " checkText "${checkText}")
string(REPLACE ";" " " checkText "${checkText}")
string(REPLACE "[" " " checkText "${checkText}")
string(REPLACE "]" " " checkText "${checkText}")
string(REPLACE "\n" ";" checkLines "${checkText}")
set(FastCachedSelftestExemptions "")
set(inExemptions FALSE)
foreach(line IN LISTS checkLines)
    if(line MATCHES "^set\\(FastCachedSurfaceCountExemptions")
        set(inExemptions TRUE)
        continue()
    endif()
    if(NOT inExemptions)
        continue()
    endif()
    if(line MATCHES "^\\)")
        break()
    endif()
    if(line MATCHES "\"([^\"]+)\"")
        list(APPEND FastCachedSelftestExemptions "${CMAKE_MATCH_1}")
    endif()
endforeach()
if(NOT inExemptions)
    message(FATAL_ERROR
        "could not find FastCachedSurfaceCountExemptions in ${check}. This fixture stages a page "
        "per exemption so the baseline can be green, and reading none would make every case below "
        "fail for a reason that has nothing to do with the rule it names.")
endif()
list(LENGTH FastCachedSelftestExemptions exemptionCount)
if(exemptionCount EQUAL 0)
    message(FATAL_ERROR
        "read zero exemption rows out of ${check}, so no page would be staged for them and the "
        "baseline would fail on rows that matched nothing.")
endif()

set(failures "")
set(casesRun 0)
# Derived from the table rather than counted by hand: a case that changes from a
# refusal to a pass must not leave a tally beside it saying otherwise.
set(refusalCases 0)

foreach(caseRow IN LISTS FastCachedSurfaceSelftestCases)
    string(REPLACE "|" ";" fields "${caseRow}")
    list(LENGTH fields fieldCount)
    if(NOT fieldCount EQUAL 6)
        list(APPEND failures
             "a case row has ${fieldCount} fields where 6 are required, so it was not run: ${caseRow}")
        continue()
    endif()
    list(GET fields 0 caseName)
    list(GET fields 1 caseTarget)
    list(GET fields 2 caseFrom)
    list(GET fields 3 caseTo)
    list(GET fields 4 caseMustAppear)
    list(GET fields 5 caseMustNotAppear)

    fastcached_stage_and_run("${caseName}" "${caseTarget}" "${caseFrom}" "${caseTo}" output applied)

    if(NOT applied)
        list(APPEND failures
             "[${caseName}] the mutation did not apply -- `${caseFrom}` is not in the base tree, so this case reported on an unmutated tree and its verdict means nothing")
        continue()
    endif()
    # A subprocess that failed to start satisfies every `must not appear` needle
    # perfectly, so producing output is asserted rather than assumed.
    if(output STREQUAL "" OR output STREQUAL " ")
        list(APPEND failures
             "[${caseName}] the check produced no output at all, so it did not run and nothing below was tested")
        continue()
    endif()
    math(EXPR casesRun "${casesRun} + 1")
    string(FIND "${caseMustNotAppear}" "CMake Error" refusalProbe)
    if(refusalProbe EQUAL -1)
        math(EXPR refusalCases "${refusalCases} + 1")
    endif()

    # A case naming no positive needle still has one: every refusal case must be
    # SEEN to refuse, or `-` would mean `anything goes`.
    if(caseMustAppear STREQUAL "-")
        set(caseMustAppear "CMake Error")
    endif()

    string(REPLACE " && " ";" mustAppear "${caseMustAppear}")
    foreach(needle IN LISTS mustAppear)
        string(FIND "${output}" "${needle}" position)
        if(position EQUAL -1)
            list(APPEND failures
                 "[${caseName}] expected the check to say `${needle}` and it did not. It said: ${output}")
        endif()
    endforeach()

    if(NOT caseMustNotAppear STREQUAL "-")
        string(REPLACE " && " ";" mustNotAppear "${caseMustNotAppear}")
        foreach(needle IN LISTS mustNotAppear)
            string(FIND "${output}" "${needle}" position)
            if(NOT position EQUAL -1)
                list(APPEND failures
                     "[${caseName}] the check said `${needle}` and must not have. It said: ${output}")
            endif()
        endforeach()
    endif()
endforeach()

# A table that ran no cases agrees with everything, which is the vacuous pass the
# check under test refuses in its own subject.
list(LENGTH FastCachedSurfaceSelftestCases caseCount)
if(casesRun EQUAL 0)
    list(APPEND failures "no case ran at all, so this fixture asserted nothing")
elseif(NOT casesRun EQUAL ${caseCount})
    list(APPEND failures
         "${casesRun} of ${caseCount} cases ran -- the rest were skipped above and their rules are unwatched")
endif()

if(failures)
    list(JOIN failures "\n  - " rendered)
    message(FATAL_ERROR
        "node-surface-docs does not refuse what it claims to refuse:\n  - ${rendered}\n")
endif()

message(STATUS
    "node surface docs selftest: ${casesRun} of ${caseCount} case(s) ran -- a baseline, a "
    "false-positive guard and ${refusalCases} refusals, each seen to follow a mutation that was "
    "asserted applied")
