# SPDX-License-Identifier: Apache-2.0
#
# `cli-ambient-reads` must be SEEN to refuse a read, seen to ACCEPT prose, and seen to
# refuse an exemption row that no longer describes the tree.
#
# Every refusal here is a PLANTED read in a synthesised tree, never an edit to the tree
# under test, so nothing can be left behind -- and each case asserts its plant is in the
# output by `file:line` and token, because a check that refused for some other reason
# would otherwise pass the case. The baseline is load-bearing: every refusal below is
# evidence only if the unplanted tree passes, in BOTH enumeration modes.
#
# ## The mode is part of the output, and asserted on both sides
#
# A synthetic tree is not a git repository, so without a staged index every case would
# exercise the directory walk while CI exercises git -- how `check-catch-skip-return-code`
# came to have green self-tests and a red CI run. The `-git` cases stage a real index.
#
# ## The accepting arm is not decoration
#
# `DashboardLoop.hpp` names all four tokens in a comment to explain the rule. A check that
# refused prose would refuse the documentation of its own clause and be deleted rather
# than obeyed, so comment cases are asserted to PASS.
#
# Runs as `cmake -P`. See `check-script-check-signals.cmake` for why such a check reports
# failure through its OUTPUT rather than an exit code.
#
# A `directory walk` expectation below names the mode CLASS and not which of its two reasons
# ran. Since #1485 a walk says whether there was no index at all or whether the index named no
# matching file, and which one a staged tree gets depends on where the scratch directory sits --
# a tree with no `.git` of its own is still inside a work tree whenever `CMAKE_CURRENT_BINARY_DIR`
# is. The git-against-walk distinction these cases exist for is unaffected; the two reasons are
# pinned in `check-tracked-files-selftest.cmake`, which controls the git probe instead of
# inheriting it.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -DFASTCACHED_SCRATCH_DIR=<dir> \
#         [-DGIT_EXECUTABLE=<git>] -P scripts/check-cli-ambient-reads-selftest.cmake
#
# Exit codes: 0 always. The verdict is the presence of `CMake Error` in the output.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()
if(NOT DEFINED FASTCACHED_SCRATCH_DIR)
    message(FATAL_ERROR "FASTCACHED_SCRATCH_DIR must be set")
endif()

set(check "${FASTCACHED_SOURCE_DIR}/scripts/check-cli-ambient-reads.cmake")
if(NOT EXISTS "${check}")
    message(FATAL_ERROR "the check under test is missing: ${check}")
endif()

if(NOT GIT_EXECUTABLE)
    find_program(GIT_EXECUTABLE NAMES git)
endif()

# ---------------------------------------------------------------------------
# The synthesised ground truth: a CLI file whose comment names every token, as
# `DashboardLoop.hpp` does, and a platform seam that really reads one.
set(baseCliSource
"// SPDX-License-Identifier: Apache-2.0
/// The behavioural half of the getenv/isatty/ioctl/GetConsoleMode scan.
namespace FastCache::Cli
{
int Clean() { return 0; }
} // namespace FastCache::Cli
")

set(basePlatformSource
"// SPDX-License-Identifier: Apache-2.0
#include <unistd.h>
bool StandardOutputIsTerminal() { return ::isatty(STDOUT_FILENO) != 0; }
")

set(tableHeader "# SPDX-License-Identifier: Apache-2.0\n# <path>|<token>|<reason>\n")

# ---------------------------------------------------------------------------
# Stage a tree, apply one case, run the check, return its collapsed output.
function(fastcached_stage_and_run name target path content exemptions outOutput)
    set(tree "${FASTCACHED_SCRATCH_DIR}/${name}")
    file(REMOVE_RECURSE "${tree}")

    # `~n~`, `~lb~`, `~rb~`, `~sc~`, `~bar~` and `~bs~` are a newline, `[`, `]`, `;`, `|`
    # and a backslash: none can appear literally in a case row, where `|` separates fields,
    # `;` splits the list and an unbalanced `[` merges every following row into one.
    foreach(varName path content exemptions)
        string(REPLACE "~n~" "\n" ${varName} "${${varName}}")
        string(REPLACE "~lb~" "[" ${varName} "${${varName}}")
        string(REPLACE "~rb~" "]" ${varName} "${${varName}}")
        string(REPLACE "~sc~" ";" ${varName} "${${varName}}")
        string(REPLACE "~bar~" "|" ${varName} "${${varName}}")
        string(REPLACE "~bs~" "\\" ${varName} "${${varName}}")
    endforeach()

    set(useGit FALSE)
    if(target MATCHES "^(.*)-git$")
        set(target "${CMAKE_MATCH_1}")
        set(useGit TRUE)
    endif()

    set(writeCli TRUE)
    set(writeTable TRUE)
    set(platformText "${basePlatformSource}")

    if(target STREQUAL "none")
        # Nothing to apply.
    elseif(target STREQUAL "newfile")
        file(WRITE "${tree}/${path}" "${content}")
    elseif(target STREQUAL "nocontrol")
        # The seam no longer reads anything this scanner recognises.
        set(platformText "bool StandardOutputIsTerminal() { return false; }\n")
    elseif(target STREQUAL "nosubject")
        set(writeCli FALSE)
    elseif(target STREQUAL "notable")
        set(writeTable FALSE)
    else()
        message(FATAL_ERROR "unknown target `${target}` in case `${name}`")
    endif()

    if(writeCli)
        file(WRITE "${tree}/src/apps/fastcache-cli/Clean.hpp" "${baseCliSource}")
    else()
        file(MAKE_DIRECTORY "${tree}/src/apps/fastcache-cli")
    endif()
    file(WRITE "${tree}/src/FastCache/Platform/Terminal.cpp" "${platformText}")
    if(writeTable)
        if(exemptions STREQUAL "-")
            file(WRITE "${tree}/scripts/cli-ambient-read-exemptions.txt" "${tableHeader}")
        else()
            file(WRITE "${tree}/scripts/cli-ambient-read-exemptions.txt" "${tableHeader}${exemptions}\n")
        endif()
    endif()

    if(useGit)
        if(NOT GIT_EXECUTABLE)
            message(FATAL_ERROR
                "case `${name}` asks for the git enumeration path and no git was found. "
                "Skipping it would leave the mode CI takes untested")
        endif()
        execute_process(COMMAND "${GIT_EXECUTABLE}" init -q "${tree}" OUTPUT_QUIET ERROR_QUIET RESULT_VARIABLE initStatus)
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${tree}" add -A OUTPUT_QUIET ERROR_QUIET RESULT_VARIABLE addStatus)
        if(NOT initStatus EQUAL 0 OR NOT addStatus EQUAL 0)
            message(FATAL_ERROR
                "case `${name}` could not stage a git index (init=${initStatus} add=${addStatus}), "
                "so it would silently have tested the directory walk instead")
        endif()
    endif()

    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DFASTCACHED_SOURCE_DIR=${tree}" "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}" -P "${check}"
        OUTPUT_VARIABLE captured ERROR_VARIABLE capturedErrors)
    set(combined "${captured}${capturedErrors}")

    # Diagnostics wrap at a column that depends on the scratch path's length, so a
    # multi-word needle can straddle a line on one machine and not another. Collapsed, a
    # needle is portable -- and a `must not appear` needle cannot pass for free.
    string(REGEX REPLACE "[ \t\r\n]+" " " combined "${combined}")
    set(${outOutput} "${combined}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
#   <name>|<target>|<path>|<content>|<exemption rows or ->|<must ALL appear>|<must NONE appear>
#
# Needle fields are ' && '-separated; `-` means none. The field count is asserted per row,
# because a row that does not parse runs as a different case than it reads as.
set(FastCachedAmbientReadCases
    # THE ACCEPTING ARM, in both modes, with the comment naming every token present.
    "baseline via walk|none|-|-|-|no getenv, isatty, ioctl, GetConsoleMode across && directory walk && 0 exemption row(s) && the control recognised 1 read(s)|CMake Error"
    "baseline via git|none-git|-|-|-|no getenv, isatty, ioctl, GetConsoleMode across && git ls-files|CMake Error"

    # THE RED ARM, one planted read per token, each named by file, line and token.
    "a getenv is refused|newfile|src/apps/fastcache-cli/Env.cpp|#include <cstdlib>~n~char const* Home() { return std::getenv(\"HOME\")~sc~ }~n~|-|src/apps/fastcache-cli/Env.cpp:2: getenv && 1 ambient read(s)|-"
    "an isatty is refused via git|newfile-git|src/apps/fastcache-cli/Tty.cpp|#include <unistd.h>~n~bool Tty() { return ::isatty(1) != 0~sc~ }~n~|-|src/apps/fastcache-cli/Tty.cpp:2: isatty && git ls-files|-"
    "an ioctl is refused|newfile|src/apps/fastcache-cli/Size.cpp|#include <sys/ioctl.h>~n~int Rows() { winsize w {}~sc~ ::ioctl(1, TIOCGWINSZ, &w)~sc~ return w.ws_row~sc~ }~n~|-|src/apps/fastcache-cli/Size.cpp:1: ioctl && src/apps/fastcache-cli/Size.cpp:2: ioctl|-"
    "a GetConsoleMode is refused|newfile|src/apps/fastcache-cli/Console.cpp|bool Vt(void* h) { unsigned long m = 0~sc~ return ::GetConsoleMode(h, &m) != 0~sc~ }~n~|-|src/apps/fastcache-cli/Console.cpp:1: GetConsoleMode|-"

    # The same question through another door: a word-boundary match would pass these.
    "a prefixed variant is the same read|newfile|src/apps/fastcache-cli/Win.cpp|int Tty() { return ::_isatty(1)~sc~ }~n~char* Home() { return ::secure_getenv(\"HOME\")~sc~ }~n~|-|src/apps/fastcache-cli/Win.cpp:1: isatty && src/apps/fastcache-cli/Win.cpp:2: getenv|-"

    # PROSE is not a read, on either comment form.
    "a line comment naming a token is not a read|newfile|src/apps/fastcache-cli/Note.cpp|// Never call isatty here: take TerminalCapabilities.~n~int Note() { return 0~sc~ }~n~|-|no getenv, isatty|CMake Error"
    "a block comment naming a token is not a read|newfile|src/apps/fastcache-cli/Note.cpp|/*~n~ getenv and ioctl belong in Platform.~n~*/~n~int Note() { return 0~sc~ }~n~|-|no getenv, isatty|CMake Error"

    # COMMENT ORDERING: a line comment NAMING a block opener must not open one, or the read
    # on the next line is skipped while a clean count is printed.
    "a line comment naming a block opener does not hide the next line|newfile|src/apps/fastcache-cli/Order.cpp|// no isatty in a /* block either~n~bool T() { return ::isatty(0) != 0~sc~ }~n~|-|src/apps/fastcache-cli/Order.cpp:2: isatty|Order.cpp:1"

    # LINE NUMBERS survive the characters that are CMake list structure.
    "line number survives brackets and semicolons|newfile|src/apps/fastcache-cli/Drift.cpp|int a~lb~4~rb~ = {1, 2, 3, 4}~sc~~n~auto b = a~lb~0~rb~~sc~ auto c = a~lb~1~rb~~sc~~n~// a trailing backslash ~bs~~n~char const* H() { return std::getenv(\"H\")~sc~ }~n~|-|src/apps/fastcache-cli/Drift.cpp:4: getenv|-"

    # --- the exemption table ---

    # A row with a reason covers exactly its (file, token), and is counted.
    "an exemption row covers its read|newfile|src/apps/fastcache-cli/Env.cpp|char const* Home() { return std::getenv(\"HOME\")~sc~ }~n~|src/apps/fastcache-cli/Env.cpp~bar~getenv~bar~reads a variable before any seam exists|1 exemption row(s) && no getenv|CMake Error"

    # A reason may itself contain a bar: fields split on the first two only.
    "a reason may contain a bar|newfile|src/apps/fastcache-cli/Env.cpp|char const* Home() { return std::getenv(\"HOME\")~sc~ }~n~|src/apps/fastcache-cli/Env.cpp~bar~getenv~bar~one ~bar~ two|1 exemption row(s)|CMake Error"

    # A row for ANOTHER token covers nothing, and is itself stale.
    "a row for another token is stale and covers nothing|newfile|src/apps/fastcache-cli/Env.cpp|char const* Home() { return std::getenv(\"HOME\")~sc~ }~n~|src/apps/fastcache-cli/Env.cpp~bar~isatty~bar~a reason|STALE -- src/apps/fastcache-cli/Env.cpp no longer names isatty|-"

    # THE STALE ROW: a permission for a read that is gone is refused, not tolerated.
    "a stale row is refused|none|-|-|src/apps/fastcache-cli/Clean.hpp~bar~getenv~bar~it used to read HOME|STALE -- src/apps/fastcache-cli/Clean.hpp no longer names getenv && problem(s) in scripts/cli-ambient-read-exemptions.txt|-"
    "a row naming a file outside the subject is stale|none|-|-|src/tools/Other.cpp~bar~getenv~bar~a reason|STALE -- src/tools/Other.cpp is not a source under src/apps/fastcache-cli/|-"
    "a row with no reason is refused|newfile|src/apps/fastcache-cli/Env.cpp|char const* Home() { return std::getenv(\"HOME\")~sc~ }~n~|src/apps/fastcache-cli/Env.cpp~bar~getenv~bar~|the row gives no reason|-"
    "a row naming an unknown token is refused|none|-|-|src/apps/fastcache-cli/Clean.hpp~bar~popen~bar~a reason|`popen` is not a token this scan matches|-"
    "a row that is not three fields is refused|none|-|-|src/apps/fastcache-cli/Clean.hpp getenv|not a <path>~bar~<token>~bar~<reason> row|-"

    # --- fails CLOSED ---
    "a missing table is not an empty one|notable|-|-|-|the exemption table scripts/cli-ambient-read-exemptions.txt is missing|-"
    "a scanner blind in the seams cannot conclude|nocontrol|-|-|-|the scanner recognised no read in the platform seams|-"
    "no subject sources at all|nosubject|-|-|-|matched no sources under src/apps/fastcache-cli|-"
)

# ---------------------------------------------------------------------------
set(failures "")
set(caseCount 0)

foreach(row IN LISTS FastCachedAmbientReadCases)
    string(REPLACE "|" ";" fields "${row}")
    list(LENGTH fields fieldCount)
    if(NOT fieldCount EQUAL 7)
        message(FATAL_ERROR
            "case row has ${fieldCount} fields, expected 7 -- a row that does not parse would "
            "run as a different case than it reads as: [${row}]")
    endif()
    list(GET fields 0 caseName)
    list(GET fields 1 caseTarget)
    list(GET fields 2 casePath)
    list(GET fields 3 caseContent)
    list(GET fields 4 caseExemptions)
    list(GET fields 5 caseMustAppear)
    list(GET fields 6 caseMustNotAppear)

    math(EXPR caseCount "${caseCount} + 1")
    fastcached_stage_and_run("${caseCount}" "${caseTarget}" "${casePath}" "${caseContent}" "${caseExemptions}" output)

    foreach(field must mustNot)
        if(field STREQUAL "must")
            set(needleText "${caseMustAppear}")
        else()
            set(needleText "${caseMustNotAppear}")
        endif()
        if(needleText STREQUAL "-")
            continue()
        endif()
        string(REPLACE "~bar~" "|" needleText "${needleText}")
        string(REPLACE " && " ";" needles "${needleText}")
        foreach(needle IN LISTS needles)
            string(FIND "${output}" "${needle}" position)
            if(field STREQUAL "must" AND position EQUAL -1)
                list(APPEND failures "${caseName}: expected to see `${needle}` and did not")
            elseif(field STREQUAL "mustNot" AND NOT position EQUAL -1)
                list(APPEND failures "${caseName}: did not expect `${needle}` and saw it")
            endif()
        endforeach()
    endforeach()

    # The verdict itself, apart from the needles, so a needle inside a DIFFERENT
    # failure's text cannot stand in for it. `CMake Error|CMake Warning`, never the error
    # word alone (#672).
    set(sawSignal FALSE)
    if(output MATCHES "CMake Error|CMake Warning")
        set(sawSignal TRUE)
    endif()
    # verdict-error-only: a read of the CASE TABLE, where `CMake Error` in the `must not
    # appear` field means "this case expects acceptance" -- not a read of the sub-run.
    if(caseMustNotAppear MATCHES "CMake Error")
        if(sawSignal)
            list(APPEND failures "${caseName}: expected the check to pass and it refused")
        endif()
    elseif(NOT sawSignal)
        list(APPEND failures "${caseName}: expected the check to refuse and it passed")
    endif()
endforeach()

if(caseCount EQUAL 0)
    message(FATAL_ERROR
        "the case table is empty, so this fixture asserted nothing -- which is exactly what a "
        "green run with no cases looks like")
endif()

if(failures)
    list(LENGTH failures failureCount)
    message("")
    foreach(failure IN LISTS failures)
        message("  ${failure}")
    endforeach()
    message("")
    message("`cli-ambient-reads` did not behave as its own documentation says. A check nobody")
    message("has watched refuse is not a check, and one nobody has watched ACCEPT a comment")
    message("gets deleted, because satisfying it would mean deleting the documentation.")
    message(FATAL_ERROR "cli-ambient-reads-selftest: ${failureCount} finding(s) across ${caseCount} case(s)")
endif()

message(STATUS "cli-ambient-reads-selftest: ${caseCount} case(s), every verdict as documented")
