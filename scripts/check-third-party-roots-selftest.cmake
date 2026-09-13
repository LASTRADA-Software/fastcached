# SPDX-License-Identifier: Apache-2.0
#
# `third-party-roots` must be SEEN to refuse, on each thing it claims, and seen to accept
# what it does not claim (#1370).
#
# The cases that matter most are the POSITIVE CONTROLS: one tree-wide enumerator of each
# spelling in the check's table, in a file that does not read the roots, each refused by
# `path:line` and spelling. A spelling nobody has watched refuse is a row of decoration.
# Beside them, the shapes the first versions of the check got wrong while it was written:
# a recursive glob over a literal name, a comment satisfying "reads the roots", a CMake
# `ls-files` whose pathspec is on the next line, and a shell line continued with `\`.
#
# And both READERS of `scripts/lib/third-party-roots.txt` -- bash 3.2 and `cmake -P` -- are
# driven over the same files and must agree: on the roots of a valid file, and on refusing
# a missing, empty or malformed one. Two readers of one format are two parsers, and they
# drift exactly where nobody compares them.
#
# A bracket argument drops the newline right after its opening `[=[`, so a fixture
# written that way starts on the NEXT line and its line numbers count from there.
#
# THIS FILE IS ITSELF SCANNED by the check it tests, so its fixtures cannot spell an
# enumerator or a reader call literally: its own text would then be a site, or a reader,
# by construction. Every such word goes through a placeholder expanded at write time
# (`FastCachedFixtureWords`).
#
# Runs as `cmake -P`. See `check-script-check-signals.cmake` for why such a check reports
# failure through its OUTPUT rather than an exit code.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -DFASTCACHED_SCRATCH_DIR=<dir> \
#         [-DFASTCACHED_BASH=<bash>] -P scripts/check-third-party-roots-selftest.cmake
#
# Exit codes: 0 always. The verdict is the presence of `CMake Error` in the output.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()
if(NOT DEFINED FASTCACHED_SCRATCH_DIR)
    message(FATAL_ERROR "FASTCACHED_SCRATCH_DIR must be set")
endif()

include("${FASTCACHED_SOURCE_DIR}/scripts/lib/CheckCommon.cmake")

set(check "${FASTCACHED_SOURCE_DIR}/scripts/check-third-party-roots.cmake")
if(NOT EXISTS "${check}")
    message(FATAL_ERROR "the check under test is missing: ${check}")
endif()

set(root "${FASTCACHED_SCRATCH_DIR}/third-party-roots-selftest")
file(REMOVE_RECURSE "${root}")
set(failures "")
# Counted where a case RUNS, so a run that stopped early cannot print a full count.
set(caseCount 0)

# The words a fixture spells and this file must not: `<placeholder>|<word>`.
set(FastCachedFixtureWords
    "@LS@|git ls-files"
    "@LSONLY@|ls-files"
    "@GLOB@|GLOB_RECURSE"
    "@FIND@|find"
    "@GREP@|grep"
    "@SPLIT@|first_party_paths"
    "@DECLINE@|fastcached_decline_third_party"
)

# Write one fixture file, expanding the placeholders.
# @param tree The synthetic tree. @param relative Where, inside it. @param body The text.
function(fastcached_fixture tree relative body)
    foreach(row IN LISTS FastCachedFixtureWords)
        fastcached_row_fields("${row}" placeholder word)
        string(REPLACE "${placeholder}" "${word}" body "${body}")
    endforeach()
    file(WRITE "${tree}/${relative}" "${body}")
endfunction()

# A tree the check ACCEPTS: one root that is a directory and is named in
# `.clang-format-ignore`, and one script whose tree-wide enumerator reads the roots. Every
# refusing case starts from this and breaks exactly one thing, so a refusal cannot come
# from something the case did not plant.
# @param name The case, and its directory. @param outVar Receives the tree.
function(fastcached_base_tree name outVar)
    set(tree "${root}/${name}")
    file(REMOVE_RECURSE "${tree}")
    file(WRITE "${tree}/scripts/lib/third-party-roots.txt" "# planted\nthirdparty/upstream\n")
    file(WRITE "${tree}/thirdparty/upstream/KEEP" "")
    file(WRITE "${tree}/.clang-format-ignore" "thirdparty/upstream/**\n")
    fastcached_fixture("${tree}" "scripts/reads.sh" [=[
files="$(@LS@ '*.sh')"
files="$(@SPLIT@ . "$files")"
]=])
    set(${outVar} "${tree}" PARENT_SCOPE)
endfunction()

# Run the check against @p tree and judge the OUTPUT.
# @param name The case. @param tree The tree. @param expect `pass` or `refuse`.
# @param ARGN Phrases the whitespace-flattened output must carry. CMake wraps diagnostic
#        text, so a phrase is matched against the flattened output, never a raw line.
function(fastcached_expect name tree expect)
    math(EXPR next "${caseCount} + 1")
    set(caseCount ${next} PARENT_SCOPE)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DFASTCACHED_SOURCE_DIR=${tree}" -P "${check}"
        OUTPUT_VARIABLE captured
        ERROR_VARIABLE capturedErrors
        RESULT_VARIABLE ignored)
    set(combined "${captured}${capturedErrors}")
    string(REGEX REPLACE "[ \t\r\n]+" " " flat "${combined}")
    # A failure text must not carry a bracket or a `;`: either corrupts the list it joins.
    string(REPLACE "[" " " excerpt "${flat}")
    string(REPLACE "]" " " excerpt "${excerpt}")
    string(REPLACE ";" "," excerpt "${excerpt}")
    string(SUBSTRING "${excerpt}" 0 400 excerpt)

    set(objected FALSE)
    if(combined MATCHES "CMake Error|CMake Warning")
        set(objected TRUE)
    endif()
    if(expect STREQUAL "pass" AND objected)
        list(APPEND failures "${name}: refused a tree it must accept -- ${excerpt}")
    elseif(expect STREQUAL "refuse" AND NOT objected)
        list(APPEND failures "${name}: accepted a tree it must refuse -- ${excerpt}")
    endif()
    foreach(phrase IN LISTS ARGN)
        string(FIND "${flat}" "${phrase}" position)
        if(position EQUAL -1)
            list(APPEND failures "${name}: the output does not carry: ${phrase} -- got: ${excerpt}")
        endif()
    endforeach()
    set(failures "${failures}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# 1. The accepting direction. A check that refuses everything refuses this too.
fastcached_base_tree("clean" tree)
fastcached_expect("clean" "${tree}" pass
    "1 tree-wide enumerator site(s) in 1 scanned script(s), 1 file(s) reading the roots (thirdparty/upstream)")

# 2. THE POSITIVE CONTROLS: one enumerator of each spelling, none reading the roots. Each
#    must be refused by path, line and spelling -- a check that refused the tree for one of
#    them would still name only that one.
fastcached_base_tree("each-spelling" tree)
fastcached_fixture("${tree}" "scripts/ls.sh" [=[all="$(@LS@ '*.hpp')"
]=])
fastcached_fixture("${tree}" "scripts/glob.cmake" [=[file(@GLOB@ all "${FASTCACHED_SOURCE_DIR}/*.cpp")
]=])
fastcached_fixture("${tree}" "scripts/find.sh" [=[@FIND@ . -name '*.sh'
]=])
fastcached_fixture("${tree}" "scripts/grep.sh" [=[@GREP@ -rl needle .
]=])
fastcached_expect("each-spelling" "${tree}" refuse
    "scripts/ls.sh:1: a tree-wide enumerator (git-ls-files)"
    "scripts/glob.cmake:1: a tree-wide enumerator (glob-recurse)"
    "scripts/find.sh:1: a tree-wide enumerator (find)"
    "scripts/grep.sh:1: a tree-wide enumerator (grep-r)"
    "third-party roots: 4 violation(s)"
    "takes every file under thirdparty/upstream as this project's own")

# 3. A recursive glob needs no `*` to recurse: `${FASTCACHED_SOURCE_DIR}/CMakeLists.txt`
#    matches that name in every directory. The first version counted only a `*`, and so
#    missed a real site in `check-catch-skip-return-code.cmake`. Split over two lines too.
fastcached_base_tree("literal-name-glob" tree)
fastcached_fixture("${tree}" "scripts/named.cmake" [=[
file(@GLOB@ walked RELATIVE "${FASTCACHED_SOURCE_DIR}"
     "${FASTCACHED_SOURCE_DIR}/CMakeLists.txt")
]=])
fastcached_expect("literal-name-glob" "${tree}" refuse
    "scripts/named.cmake:1: a tree-wide enumerator (glob-recurse)")

# 4. What is NOT a tree-wide enumerator, all in files that do not read the roots: an
#    anchored pathspec, a variable pathspec, a find over a named scratch directory, prose
#    in a string, a comment, and globs under a literal or variable directory. Accepted, and
#    the one real site still counted -- or "counted nothing" would pass this too.
fastcached_base_tree("anchored" tree)
fastcached_fixture("${tree}" "scripts/anchored.sh" [=[
a="$(@LS@ 'src/*.hpp')"
b="$(@LS@ -- "$path")"
@FIND@ "$scratch" -name x
echo "@LS@ is how a census starts"
# @FIND@ . -name commented
]=])
fastcached_fixture("${tree}" "scripts/anchored.cmake" [=[
file(@GLOB@ x "${FASTCACHED_SOURCE_DIR}/src/*")
file(@GLOB@ y "${FASTCACHED_SOURCE_DIR}/${directory}/*")
message(STATUS "run @LS@ over the tree")
]=])
fastcached_expect("anchored" "${tree}" pass
    "1 tree-wide enumerator site(s) in 3 scanned script(s)")

# 5. A CMake alias of the source root is the source root.
fastcached_base_tree("cmake-alias" tree)
fastcached_fixture("${tree}" "scripts/alias.cmake" [=[
set(top "${FASTCACHED_SOURCE_DIR}")
file(@GLOB@ all "${top}/*.cmake")
]=])
fastcached_expect("cmake-alias" "${tree}" refuse
    "scripts/alias.cmake:2: a tree-wide enumerator (glob-recurse)")

# 6. A CMake `ls-files` whose pathspec is on the NEXT line -- the shape
#    `check-catch-skip-return-code.cmake` has. Read alone, the call has no pathspec at all.
fastcached_base_tree("cmake-git-next-line" tree)
fastcached_fixture("${tree}" "scripts/gitnext.cmake" [=[
execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${FASTCACHED_SOURCE_DIR}" @LSONLY@
            -- "src/*_test.cpp"
    OUTPUT_VARIABLE anchored)
execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${FASTCACHED_SOURCE_DIR}" @LSONLY@
            -- "*CMakeLists.txt"
    OUTPUT_VARIABLE tracked)
]=])
fastcached_expect("cmake-git-next-line" "${tree}" refuse
    "scripts/gitnext.cmake:6: a tree-wide enumerator (git-ls-files)"
    "third-party roots: 1 violation(s)")

# 7. A shell command continued with `\` is one command, and a continued line earlier in the
#    file must not move where the site is reported: the line splitter once merged every line
#    ending in a backslash with the next.
fastcached_base_tree("shell-continued" tree)
fastcached_fixture("${tree}" "scripts/continued.sh" [=[echo one \
    two
@FIND@ \
    . -name '*.sh'
]=])
fastcached_expect("shell-continued" "${tree}" refuse
    "scripts/continued.sh:3: a tree-wide enumerator (find)")

# 8. A COMMENT is not a read, and neither is sourcing the library. The first version of the
#    check accepted any mention of the roots file, which this file satisfied.
fastcached_base_tree("mention-is-not-a-read" tree)
fastcached_fixture("${tree}" "scripts/mention.sh" [=[# this script calls @SPLIT@ on what it lists
. "$(dirname "$0")/lib/third-party-roots.sh"
@FIND@ . -name '*.sh'
]=])
fastcached_expect("mention-is-not-a-read" "${tree}" refuse
    "scripts/mention.sh:3: a tree-wide enumerator (find)")

# 9. The CMake reader counts as reading, derived from its definition.
fastcached_base_tree("cmake-reader" tree)
fastcached_fixture("${tree}" "scripts/decline.cmake" [=[
file(@GLOB@ all RELATIVE "${FASTCACHED_SOURCE_DIR}" "${FASTCACHED_SOURCE_DIR}/*.txt")
@DECLINE@("${FASTCACHED_SOURCE_DIR}" all declined)
]=])
fastcached_expect("cmake-reader" "${tree}" pass
    "2 tree-wide enumerator site(s) in 2 scanned script(s), 2 file(s) reading the roots")

# 10. An exemption with a reason excuses its site, and is counted.
fastcached_base_tree("exemption" tree)
fastcached_fixture("${tree}" "scripts/counts.sh" [=[@FIND@ . -type f | wc -l
]=])
file(WRITE "${tree}/scripts/lib/third-party-roots-exemptions.txt"
    "# planted\nscripts/counts.sh|find|counts every file, and which are first-party does not change a count of all of them\n")
fastcached_expect("exemption" "${tree}" pass "1 exemption(s)")

# 11. A STALE exemption is refused: one naming a file that does not spell that enumerator,
#     and one naming a file that does not exist.
fastcached_base_tree("exemption-stale" tree)
file(WRITE "${tree}/scripts/lib/third-party-roots-exemptions.txt"
    "scripts/reads.sh|find|reads.sh spells no find\nscripts/gone.sh|git-ls-files|the file was deleted\n")
fastcached_expect("exemption-stale" "${tree}" refuse
    "the row for scripts/reads.sh|find is STALE"
    "the row for scripts/gone.sh|git-ls-files is STALE")

# 12. An exemption without a reason is refused, whitespace being no reason.
fastcached_base_tree("exemption-reasonless" tree)
fastcached_fixture("${tree}" "scripts/counts.sh" [=[@FIND@ . -type f | wc -l
]=])
file(WRITE "${tree}/scripts/lib/third-party-roots-exemptions.txt" "scripts/counts.sh|find|   \n")
fastcached_expect("exemption-reasonless" "${tree}" refuse
    "the exemption for scripts/counts.sh (find) gives no reason")

# 13. The roots file: missing, naming no root, and each malformed spelling.
fastcached_base_tree("roots-missing" tree)
file(REMOVE "${tree}/scripts/lib/third-party-roots.txt")
fastcached_expect("roots-missing" "${tree}" refuse "third-party-roots.txt is missing")

fastcached_base_tree("roots-empty" tree)
file(WRITE "${tree}/scripts/lib/third-party-roots.txt" "# only a comment\n\n   \n")
fastcached_expect("roots-empty" "${tree}" refuse "names no root")

set(badIndex 0)
foreach(bad IN ITEMS "/thirdparty/upstream" "thirdparty/upstream/" "thirdparty/../upstream" "thirdparty\\upstream")
    math(EXPR badIndex "${badIndex} + 1")
    fastcached_base_tree("roots-bad-${badIndex}" tree)
    file(WRITE "${tree}/scripts/lib/third-party-roots.txt" "${bad}\n")
    fastcached_expect("roots-bad-${badIndex}" "${tree}" refuse "which is not a root relative to the repository")
endforeach()

# 14. A root naming no directory describes nothing.
fastcached_base_tree("root-without-directory" tree)
file(WRITE "${tree}/scripts/lib/third-party-roots.txt" "thirdparty/gone\n")
file(WRITE "${tree}/.clang-format-ignore" "thirdparty/gone/**\n")
fastcached_expect("root-without-directory" "${tree}" refuse
    "names thirdparty/gone, which is not a directory of this tree")

# 15. clang-format reads its own ignore file, so a root missing there is refused.
fastcached_base_tree("format-ignore-missing" tree)
file(WRITE "${tree}/.clang-format-ignore" "# nothing ignored\n")
fastcached_expect("format-ignore-missing" "${tree}" refuse
    ".clang-format-ignore: the third-party root thirdparty/upstream is not named there")

# 16. Two empty lists agree perfectly: no enumerator at all, and no script at all.
fastcached_base_tree("no-sites" tree)
file(WRITE "${tree}/scripts/reads.sh" "echo nothing enumerated here\n")
fastcached_expect("no-sites" "${tree}" refuse "no tree-wide enumerator was found in 1 script(s)")

fastcached_base_tree("no-scripts" tree)
file(REMOVE "${tree}/scripts/reads.sh")
fastcached_expect("no-scripts" "${tree}" refuse "no script was found")

# ---------------------------------------------------------------------------
# 17. The two READERS agree, over the same files. The bash one is run by the bash ctest
#     uses; with none, this is inconclusive and reported as a failure, never skipped.
if(NOT FASTCACHED_BASH)
    find_program(FASTCACHED_BASH NAMES bash)
endif()
set(readerDriver "${root}/read-roots.cmake")
# Paths reach the driver as -D values, never as text written into it: a Windows path in a
# quoted CMake string is a string of escapes.
file(WRITE "${readerDriver}"
    "include(\"\${COMMON}\")\n"
    "fastcached_third_party_roots(\"\${TREE}\" roots)\n"
    "message(STATUS \"ROOTS=\${roots}\")\n")
# `<name>|<file content>|<roots both must read, or REFUSED>`. Content escapes are CMake's.
set(readerCases
    "valid|# comment\n  thirdparty/upstream  \n\nother/lib # trailing comment\nlast/one|thirdparty/upstream,other/lib,last/one"
    "no-final-newline|only/root|only/root"
    "comments-only|# nothing\n\n|REFUSED"
    "absolute|/abs/root\n|REFUSED"
    "missing||REFUSED"
)
foreach(readerCase IN LISTS readerCases)
    fastcached_row_fields("${readerCase}" readerName readerContent readerWant)
    set(tree "${root}/readers-${readerName}")
    file(REMOVE_RECURSE "${tree}")
    file(MAKE_DIRECTORY "${tree}/scripts/lib")
    if(NOT readerName STREQUAL "missing")
        file(WRITE "${tree}/scripts/lib/third-party-roots.txt" "${readerContent}")
    endif()
    math(EXPR caseCount "${caseCount} + 1")

    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DTREE=${tree}"
                "-DCOMMON=${FASTCACHED_SOURCE_DIR}/scripts/lib/CheckCommon.cmake" -P "${readerDriver}"
        OUTPUT_VARIABLE cmakeOut ERROR_VARIABLE cmakeErr RESULT_VARIABLE ignored)
    set(cmakeText "${cmakeOut}${cmakeErr}")
    if(cmakeText MATCHES "CMake Error|CMake Warning")
        set(cmakeRead "REFUSED")
        # verdict-error-only: the reader refuses through message(FATAL_ERROR), so this asks
        # which of the two fired -- a warning is its own outcome, and never equals a wanted read.
        if(NOT cmakeText MATCHES "CMake Error")
            set(cmakeRead "WARNED")
        endif()
    elseif(cmakeOut MATCHES "ROOTS=([^\n]*)")
        string(REPLACE ";" "," cmakeRead "${CMAKE_MATCH_1}")
    else()
        set(cmakeRead "NO ANSWER")
    endif()

    if(NOT FASTCACHED_BASH)
        list(APPEND failures "readers-${readerName}: no bash was found, so the bash reader was not compared -- inconclusive, not a pass")
        continue()
    endif()
    execute_process(
        COMMAND "${FASTCACHED_BASH}" -c ". \"$1\" && third_party_roots \"$2\""
                reader "${FASTCACHED_SOURCE_DIR}/scripts/lib/third-party-roots.sh" "${tree}"
        OUTPUT_VARIABLE bashOut ERROR_VARIABLE bashErr RESULT_VARIABLE bashStatus)
    if(bashStatus EQUAL 2)
        set(bashRead "REFUSED")
    elseif(bashStatus EQUAL 0)
        string(STRIP "${bashOut}" bashRead)
        string(REPLACE "\r" "" bashRead "${bashRead}")
        string(REPLACE "\n" "," bashRead "${bashRead}")
    else()
        set(bashRead "EXIT ${bashStatus}")
    endif()

    if(NOT cmakeRead STREQUAL readerWant)
        list(APPEND failures "readers-${readerName}: the CMake reader read ${cmakeRead}, wanted ${readerWant}")
    endif()
    if(NOT bashRead STREQUAL readerWant)
        list(APPEND failures "readers-${readerName}: the bash reader read ${bashRead}, wanted ${readerWant}")
    endif()
endforeach()

if(failures)
    list(LENGTH failures failureCount)
    message("")
    foreach(failure IN LISTS failures)
        message("  ${failure}")
    endforeach()
    message("")
    message("`third-party-roots` is what refuses a tree-wide enumerator that has not asked which")
    message("files are third-party. Each case drives it against a synthetic tree and asserts ONE")
    message("verdict and what it named, so a failure says which direction broke.")
    message(FATAL_ERROR "third-party roots selftest: ${failureCount} of ${caseCount} case(s) wrong")
endif()
message(STATUS "third-party roots selftest: ${caseCount} case(s) ran, every verdict as expected")
