# SPDX-License-Identifier: Apache-2.0
#
# `cli-text-cell` must be SEEN to refuse a bypass, and seen to ACCEPT the reads.
#
# Both arms, and the accepting arm is not the decorative one. #1356's own acceptance
# clause says so in as many words -- *a guard nobody has watched accept is not known
# to work either* -- and this check has two ways to fail toward refusal that a red-only
# fixture cannot tell apart from working: `kind == CellKind::Text` differs from an
# assignment by one character, and `case CellKind::Text:` is how every renderer in the
# binary consumes the model. A check that refused either would be deleted rather than
# obeyed, which is the failure mode, not a cosmetic one.
#
# It also caught a real defect in the check on its first run, before this fixture
# existed: the factory signature was `Cell TextCell(` with no leading newline, which
# matched the header's `[[nodiscard]] Cell TextCell(...);` declaration as well and made
# the check refuse a healthy tree for carrying two factories. That is the fail-closed
# direction, so it presented as a red rather than as a silent pass -- which is why the
# "found 2" case below is kept: it is the behaviour that found the bug.
#
# ## The mode is part of the output, and asserted on both sides
#
# This fixture stages synthetic trees, and a synthetic tree is not a git repository. So
# every case would exercise the DIRECTORY WALK while CI exercises GIT -- which is
# exactly how `check-catch-skip-return-code` came to have six green self-test cases and
# a red CI run with no contradiction between them: the mode under test was not the mode
# in use. So the check names its mode, cases assert it, and the git path is staged for
# real with `git init` plus `git add` -- an index is all `git ls-files` needs, so no
# commit and no identity are required.
#
# ## Line numbers are asserted, not just filenames
#
# `;`, `\`, `[` and `]` are all CMake list structure and C++ is full of all four. Get
# the reading wrong and lines merge: the following line is hidden and every line number
# below it drifts. One case plants all four ABOVE a violation and asserts the exact
# `file:line`, because an assertion on the FILENAME alone passes under that bug.
#
# The mutations are applied to a SYNTHESISED tree, never to the tree under test, so
# there is no revert step and nothing can be left behind. Each case asserts its
# mutation actually LANDED before any verdict is drawn from it, and the BASELINE is
# load-bearing: every refusal below is evidence only if the unmutated tree passes.
#
# Runs as `cmake -P`. See `check-script-check-signals.cmake` for why such a check
# reports failure through its OUTPUT rather than an exit code.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -DFASTCACHED_SCRATCH_DIR=<dir> \
#         -P scripts/check-cli-text-cell-selftest.cmake
#
# Exit codes: 0 always. The verdict is the presence of `CMake Error` in the output.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()
if(NOT DEFINED FASTCACHED_SCRATCH_DIR)
    message(FATAL_ERROR "FASTCACHED_SCRATCH_DIR must be set")
endif()

set(check "${FASTCACHED_SOURCE_DIR}/scripts/check-cli-text-cell.cmake")
if(NOT EXISTS "${check}")
    message(FATAL_ERROR "the check under test is missing: ${check}")
endif()

if(NOT GIT_EXECUTABLE)
    find_program(GIT_EXECUTABLE NAMES git)
endif()

# ---------------------------------------------------------------------------
# The synthesised ground truth, mirroring the real shape: one factory that writes the
# enumerator, one renderer that switches on it, one test that compares against it.
set(baseFactorySource
"// SPDX-License-Identifier: Apache-2.0
#include \"CliValue.hpp\"

namespace FastCache::Cli
{

Cell TextCell(std::string text)
{
    if (IsValidUtf8(text))
        return Cell { .kind = CellKind::Text, .lexical = std::move(text) };
    return Cell { .kind = CellKind::Binary, .lexical = Base64Encode(text) };
}

} // namespace FastCache::Cli
")

set(baseRendererSource
"// SPDX-License-Identifier: Apache-2.0
#include \"CliFormat.hpp\"

namespace FastCache::Cli
{
void AppendJsonCell(Cell const& cell, std::string& out)
{
    switch (cell.kind)
    {
        case CellKind::Text:
        case CellKind::Binary:
            out += cell.lexical;
            return;
        default:
            out += \"null\";
    }
}
} // namespace FastCache::Cli
")

set(baseTestSource
"// SPDX-License-Identifier: Apache-2.0
#include \"CliValue.hpp\"

TEST_CASE(\"text is classified\")
{
    auto const cell = TextCell(\"plain\");
    REQUIRE(cell.kind == CellKind::Text);
}
")

# ---------------------------------------------------------------------------
# Stage a tree, apply one mutation, run the check, return its collapsed output.
function(fastcached_stage_and_run name target from to outOutput outApplied)
    set(tree "${FASTCACHED_SCRATCH_DIR}/${name}")
    file(REMOVE_RECURSE "${tree}")

    # `~n~`, `~lb~`, `~rb~`, `~sc~` and `~bar~` are a newline, `[`, `]`, `;` and `|`.
    # Those characters cannot appear literally in a case row: `|` is the field
    # separator and `;` splits the CMake list, while an unbalanced `[` merges every
    # following row into one element -- the same bracket-vulnerable reader the check's
    # own line splitter exists to avoid.
    foreach(varName from to)
        string(REPLACE "~n~" "\n" ${varName} "${${varName}}")
        string(REPLACE "~lb~" "[" ${varName} "${${varName}}")
        string(REPLACE "~rb~" "]" ${varName} "${${varName}}")
        string(REPLACE "~sc~" ";" ${varName} "${${varName}}")
        string(REPLACE "~bar~" "|" ${varName} "${${varName}}")
    endforeach()

    # A `-git` suffix stages a real git index as well, so the git enumeration path is
    # exercised rather than assumed.
    set(useGit FALSE)
    if(target MATCHES "^(.*)-git$")
        set(target "${CMAKE_MATCH_1}")
        set(useGit TRUE)
    endif()

    set(factoryText "${baseFactorySource}")
    set(rendererText "${baseRendererSource}")
    set(testText "${baseTestSource}")
    set(extraPath "")
    set(extraText "")
    set(writeSources TRUE)
    set(applied TRUE)

    if(target STREQUAL "none")
        # Nothing to apply.
    elseif(target STREQUAL "newfile")
        # `from` is the path, `to` is the content. Always applies -- there is nothing
        # to find -- so the landed-check below is satisfied by construction and the
        # row's needles carry the whole assertion.
        set(extraPath "${from}")
        set(extraText "${to}")
    elseif(target STREQUAL "factory")
        string(FIND "${factoryText}" "${from}" position)
        if(position EQUAL -1)
            set(applied FALSE)
        else()
            string(REPLACE "${from}" "${to}" factoryText "${factoryText}")
        endif()
    elseif(target STREQUAL "allreads")
        # Every read site at once. Mutating only ONE leaves the other standing and the
        # read count stays non-zero, so the guard correctly does not fire -- a
        # single-file mutation would test nothing and pass by never reaching the arm
        # it names.
        set(hitCount 0)
        foreach(textVar rendererText testText)
            string(FIND "${${textVar}}" "${from}" position)
            if(NOT position EQUAL -1)
                math(EXPR hitCount "${hitCount} + 1")
                string(REPLACE "${from}" "${to}" ${textVar} "${${textVar}}")
            endif()
        endforeach()
        if(hitCount LESS 2)
            set(applied FALSE)
        endif()
    elseif(target STREQUAL "nosources")
        set(writeSources FALSE)
    else()
        message(FATAL_ERROR "unknown mutation target `${target}` in case `${name}`")
    endif()

    if(writeSources)
        file(WRITE "${tree}/src/apps/fastcache-cli/CliValue.cpp" "${factoryText}")
        file(WRITE "${tree}/src/apps/fastcache-cli/CliFormat.cpp" "${rendererText}")
        file(WRITE "${tree}/src/apps/fastcache-cli/CliValue_test.cpp" "${testText}")
    else()
        # A tree with a src/ directory and no sources in it, which is what a moved
        # source root looks like from here.
        file(MAKE_DIRECTORY "${tree}/src")
        file(WRITE "${tree}/README.md" "not a source\n")
    endif()

    if(NOT extraPath STREQUAL "")
        file(WRITE "${tree}/${extraPath}" "${extraText}")
    endif()

    if(useGit)
        if(NOT GIT_EXECUTABLE)
            message(FATAL_ERROR
                "case `${name}` asks for the git enumeration path and no git was found. "
                "Skipping it silently would leave the mode CI actually takes untested, "
                "which is the defect this fixture exists to avoid")
        endif()
        execute_process(COMMAND "${GIT_EXECUTABLE}" init -q "${tree}"
                        OUTPUT_QUIET ERROR_QUIET RESULT_VARIABLE initStatus)
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${tree}" add -A
                        OUTPUT_QUIET ERROR_QUIET RESULT_VARIABLE addStatus)
        if(NOT initStatus EQUAL 0 OR NOT addStatus EQUAL 0)
            message(FATAL_ERROR
                "case `${name}` could not stage a git index (init=${initStatus} add=${addStatus}), "
                "so it would silently have tested the directory walk instead")
        endif()
    endif()

    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DFASTCACHED_SOURCE_DIR=${tree}" -P "${check}"
        OUTPUT_VARIABLE captured ERROR_VARIABLE capturedErrors)
    set(combined "${captured}${capturedErrors}")

    # `message(FATAL_ERROR)` word-wraps at a column that depends on how long the
    # scratch path is, so a multi-word needle can break across a line on one machine
    # and not another. Collapsing whitespace makes the needles portable -- and it
    # matters most for the NEGATIVE assertions, where a wrapped line makes a
    # `must not appear` needle pass for free.
    string(REGEX REPLACE "[ \t\r\n]+" " " combined "${combined}")

    set(${outOutput} "${combined}" PARENT_SCOPE)
    set(${outApplied} "${applied}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# The cases.
#
#   <name>|<target>|<from>|<to>|<must ALL appear>|<must NONE appear>
#
# Needle fields are ' && '-separated; `-` means none. The field count is asserted per
# row below, because a row that does not parse runs as a different case than it reads
# as.
set(FastCachedTextCellCases
    # THE ACCEPTING ARM, in both enumeration modes. Every refusal below is evidence
    # only if these pass -- and asserting the mode is what stops every case silently
    # testing the walk while CI takes git.
    "baseline via walk|none|-|-|no bypass across && directory walk (no git index)|CMake Error"
    "baseline via git|none-git|-|-|no bypass across && git ls-files|CMake Error"

    # THE RED ARM: a designated initialiser past the factory, named by file and line.
    "designated initialiser bypass|newfile|src/apps/fastcache-cli/CliVerbs.cpp|#include \"CliValue.hpp\"~n~Cell KeyCell(std::string key)~n~{~n~    return Cell { .kind = CellKind::Text, .lexical = std::move(key) }~sc~~n~}~n~|src/apps/fastcache-cli/CliVerbs.cpp:4: builds a|-"

    # ...and in git mode too, since that is the enumeration CI uses.
    "designated initialiser bypass via git|newfile-git|src/apps/fastcache-cli/CliVerbs.cpp|#include \"CliValue.hpp\"~n~Cell KeyCell(std::string key)~n~{~n~    return Cell { .kind = CellKind::Text, .lexical = std::move(key) }~sc~~n~}~n~|src/apps/fastcache-cli/CliVerbs.cpp:4: builds a && git ls-files|-"

    # The POSITIONAL aggregate, which a designated-initialiser-only pattern misses.
    # Two shapes reach the same hole and a check that saw one would report clean over
    # the other.
    "positional aggregate bypass|newfile|src/apps/fastcache-cli/CliVerbs.cpp|#include \"CliValue.hpp\"~n~Cell KeyCell(std::string key)~n~{~n~    return Cell { CellKind::Text, std::move(key) }~sc~~n~}~n~|src/apps/fastcache-cli/CliVerbs.cpp:4: builds a|-"

    # A plain assignment to an existing cell, which is the third spelling.
    "assignment bypass|newfile|src/apps/fastcache-cli/CliVerbs.cpp|#include \"CliValue.hpp\"~n~void Force(Cell& cell)~n~{~n~    cell.kind = CellKind::Text~sc~~n~}~n~|src/apps/fastcache-cli/CliVerbs.cpp:4: builds a|-"

    # THE ACCEPTING ARM PROPER, and the one the ticket names. These differ from a
    # violation by a single character or by the keyword in front, and a check that
    # refused them would refuse every renderer in the binary.
    "a switch arm is not a write|newfile|src/apps/fastcache-cli/Other.cpp|void Render(Cell const& c, std::string& o)~n~{~n~    switch (c.kind)~n~    {~n~        case CellKind::Text: o += c.lexical~sc~ return~sc~~n~        default: return~sc~~n~    }~n~}~n~|no bypass across|CMake Error"
    "an equality comparison is not a write|newfile|src/apps/fastcache-cli/Other.cpp|bool IsText(Cell const& c)~n~{~n~    return c.kind == CellKind::Text~sc~~n~}~n~|no bypass across|CMake Error"
    "an inequality comparison is not a write|newfile|src/apps/fastcache-cli/Other.cpp|bool NotText(Cell const& c)~n~{~n~    return c.kind != CellKind::Text~sc~~n~}~n~|no bypass across|CMake Error"

    # PROSE, including the shape this check's own documentation uses: the doc comment
    # on `TextCell` spells the aggregate out in order to explain it. A check that
    # refused that would refuse the documentation of its own rule.
    "a line comment is not a write|newfile|src/apps/fastcache-cli/Other.cpp|// Never Cell { .kind = CellKind::Text, ... } -- call TextCell instead.~n~int Note() { return 0~sc~ }~n~|no bypass across|CMake Error"
    "a block comment is not a write|newfile|src/apps/fastcache-cli/Other.cpp|/*~n~ A paragraph showing Cell { CellKind::Text, bytes } to argue against it.~n~*/~n~int Note() { return 0~sc~ }~n~|no bypass across|CMake Error"

    # COMMENT ORDERING, and the first of these is the false green it was: the `/*`
    # test ran BEFORE `//` was stripped, so a line comment NAMING a block opener
    # opened one that no `*/` ever closed -- every remaining line of that file
    # skipped, while the run still printed a clean count over lines it never read.
    # The violation one line below it was invisible.
    "a line comment naming a block opener does not open one|newfile|src/apps/fastcache-cli/Order.cpp|// Never Cell { .kind = ... } by hand, and not inside a /* block either.~n~Cell K(std::string k) { return Cell { .kind = CellKind::Text, .lexical = k }~sc~ }~n~|src/apps/fastcache-cli/Order.cpp:2: builds a|-"

    # ...and the CONTROL, which is what stops the repair from being *ignore block
    # comments*: a `//` inside an open block stays inside it, so line 2 is never
    # named, and the file RESUMES after `*/`, so line 4 is. Both halves are needed
    # -- a reader that dropped block tracking passes the case above.
    "a block comment survives a line marker inside it|newfile|src/apps/fastcache-cli/Order.cpp|/*~n~ Prose with a // in it, showing Cell { CellKind::Text, b } to argue against.~n~*/~n~Cell K(std::string k) { return Cell { .kind = CellKind::Text, .lexical = k }~sc~ }~n~|src/apps/fastcache-cli/Order.cpp:4: builds a|Order.cpp:2"

    # THE PARTIAL-FIX SHAPE: a file that documents the rule AND breaks it. Refused --
    # a prose mention must not excuse the file it sits in.
    "prose and a bypass in one file|newfile|src/apps/fastcache-cli/Both.cpp|// Call TextCell rather than Cell { .kind = CellKind::Text, ... }.~n~Cell KeyCell(std::string k)~n~{~n~    return Cell { .kind = CellKind::Text, .lexical = std::move(k) }~sc~~n~}~n~|src/apps/fastcache-cli/Both.cpp:4: builds a|-"

    # LINE NUMBERS, with the list structure that makes them drift planted above the
    # violation. A filename-only assertion passes under that bug.
    "line number survives brackets and semicolons|newfile|src/apps/fastcache-cli/Drift.cpp|int a~lb~4~rb~ = {1, 2, 3, 4}~sc~~n~auto b = a~lb~0~rb~~sc~ auto c = a~lb~1~rb~~sc~~n~// a trailing backslash \\~n~Cell K(std::string k) { return Cell { .kind = CellKind::Text, .lexical = k }~sc~ }~n~|src/apps/fastcache-cli/Drift.cpp:4: builds a|-"

    # --- fails CLOSED ---

    # The factory no longer writes the enumerator in a shape this scan recognises. A
    # clean verdict over the rest of the tree would then be vacuous, so it refuses.
    # This is the positive control ON the check, and it is what a red-only fixture
    # cannot supply.
    "factory write unrecognised|factory|Cell { .kind = CellKind::Text, .lexical = std::move(text) }|MakeTextCell(std::move(text))|the factory carries no recognisable write|-"

    # The enumerator renamed, so the scan is hunting a token that no longer exists --
    # which is indistinguishable from a clean tree unless it is reported.
    "enumerator read nowhere|allreads|CellKind::Text|CellKind::Utf8Text|the enumerator is never read|-"

    # The factory gone: the rule has no subject and the check needs rewriting rather
    # than satisfying.
    "no factory at all|factory|Cell TextCell(std::string text)|Cell MakeCell(std::string text)|expected exactly one factory definition, found 0|-"

    # No C++ under src/ at all -- a moved source root.
    "no sources at all|nosources|-|-|matched no C++ sources|-"
)

# ---------------------------------------------------------------------------
set(failures "")
set(caseCount 0)

foreach(row IN LISTS FastCachedTextCellCases)
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

    fastcached_stage_and_run("${caseCount}" "${caseTarget}" "${caseFrom}" "${caseTo}"
                             output applied)

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

    # The verdict itself, separate from the needles, so a needle appearing inside a
    # DIFFERENT failure's text cannot stand in for it.
    # `CMake Error|CMake Warning`, never `CMake Error` alone: a sub-run that merely
    # WARNS changes meaning silently and, read for the error word alone, is scored a
    # clean pass (#672). Enforced in `scripts/check-script-check-signals.cmake`.
    set(sawSignal FALSE)
    if(output MATCHES "CMake Error|CMake Warning")
        set(sawSignal TRUE)
    endif()
    # verdict-error-only: a read of the CASE TABLE, not of the sub-run. The table
    # spells `CMake Error` in the `must not appear` field to mean "this case expects
    # acceptance"; broadening it here would ask whether the TABLE mentions a warning,
    # which is a question about this file rather than about the check under test.
    if(caseMustNotAppear MATCHES "CMake Error")
        if(sawSignal)
            list(APPEND failures "${caseName}: expected the check to pass and it refused")
        endif()
    else()
        if(NOT sawSignal)
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
    message("`cli-text-cell` did not behave as its own documentation says.")
    message("A check nobody has watched refuse is not a check -- and one nobody has")
    message("watched ACCEPT a switch arm or an `==` is one that gets deleted, because")
    message("satisfying it would mean deleting a renderer.")
    # FINDINGS against CASES, and they are named separately because one case can
    # contribute several -- a missing needle, an unexpected needle and a wrong
    # verdict are three. Written as "N of M" this printed `22 of 15`, a number
    # nobody can explain, which is the tell for a count that overstates.
    message(FATAL_ERROR
        "cli-text-cell-selftest: ${failureCount} finding(s) across ${caseCount} case(s)")
endif()

message(STATUS "cli-text-cell-selftest: ${caseCount} case(s), every verdict as documented")
