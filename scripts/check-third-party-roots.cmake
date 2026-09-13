# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.28)

include("${CMAKE_CURRENT_LIST_DIR}/lib/CheckCommon.cmake")

# Third-party roots hygiene (#1370): every TREE-WIDE ENUMERATOR asks the one question
# "is this path first-party?" of `scripts/lib/third-party-roots.txt`, or carries an
# exemption row saying why the files it lists need no such answer.
#
# ## Why the enumerators are DERIVED
#
# Importing `vendor/` for #134 needed five enumerators told about it, each on its own:
# `tidy-sweep.sh`, the clang-tidy header filter and the registration derivation in
# `local-gate.sh`, `check-succeed-not-skip.cmake` and `check-target-file-guards.cmake`.
# Three of the five were found by REVIEW rather than by the import. Each answered "which
# files are this project's own?" separately, in its own grammar, with its own default --
# and an enumerator that had not learned about a root INCLUDED it, so third-party
# sources were linted, formatted, scanned and counted as first-party, silently and green.
#
# A list of enumerators would repeat that one level up: exact about the scripts it knows
# and silent about the next one. So what an enumerator IS is written down below as a set
# of SPELLINGS, the scan finds every site that spells one, and a site in a file that
# neither reads the roots file nor has an exemption row is refused. The sixth enumerator
# is refused on arrival, and nobody has to notice it.
#
# ## What counts as reading the roots, and what this does NOT cover
#
# A file reads the roots when it CALLS a reader outside a full-line comment. The readers
# are DERIVED from the two libraries that define them -- every public function of
# `scripts/lib/third-party-roots.sh`, and every `fastcached_*third_party*` function of
# `scripts/lib/CheckCommon.cmake` -- so a reader added there is recognised here without an
# edit. Naming the data file is not reading it, and neither is sourcing the library: the
# first version of this check accepted any mention of `third-party-roots`, which a comment
# satisfies.
#
# That is a FILE-level answer, not a per-site one -- a script whose one enumerator reads
# the roots and whose second does not passes. Stated rather than hidden, because a
# per-site rule would need to follow data flow through shell variables, which no line
# reader here can do honestly.
#
# Scanned: `scripts/` (`.sh`, `.cmake`, `.ps1`), `cmake/` (`.cmake`) and
# `.github/workflows/` (`.yml`). NOT scanned: `CMakeLists.txt` files, whose
# `GLOB_RECURSE` calls are relative to their own directories; C++ sources; and any
# spelling outside the set below. The spellings are a model narrower than bash, so a
# tree-wide walk spelled some other way -- a `for f in **` glob, a `ls -R` -- is not
# seen. Adding a spelling is adding a row.
#
# Runs as `cmake -P`. The verdict is `CMake Error` in the output, never the exit code
# (`check-script-check-signals.cmake`).
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -P scripts/check-third-party-roots.cmake

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()

fastcached_third_party_roots("${FASTCACHED_SOURCE_DIR}" thirdPartyRoots)
string(REPLACE ";" ", " rootsText "${thirdPartyRoots}")

# The readers, derived from where they are defined. Asked of THIS check's own libraries,
# not of the tree under test: they are the vocabulary the check recognises.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/lib/third-party-roots.sh" bashReaderLines
     REGEX "^[a-z][a-z0-9_]*\\(\\)[ \t]*\\{")
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/lib/CheckCommon.cmake" cmakeReaderLines
     REGEX "^function\\([ \t]*fastcached_[a-z0-9_]*third_party[a-z0-9_]*")
set(readerNames "")
foreach(readerLine IN LISTS bashReaderLines cmakeReaderLines)
    string(REGEX MATCH "([a-z][a-z0-9_]*)[ \t]*\\(\\)|fastcached_[a-z0-9_]*third_party[a-z0-9_]*" readerName "${readerLine}")
    if(CMAKE_MATCH_1)
        set(readerName "${CMAKE_MATCH_1}")
    endif()
    list(APPEND readerNames "${readerName}")
endforeach()
list(REMOVE_DUPLICATES readerNames)
if(NOT bashReaderLines OR NOT cmakeReaderLines)
    message(FATAL_ERROR
        "third-party roots: no reader was derived from scripts/lib/third-party-roots.sh or "
        "scripts/lib/CheckCommon.cmake, so no file could ever be seen reading the roots and "
        "every enumerator would be refused -- the CHECK is broken, not the tree")
endif()
string(REPLACE ";" "|" readerAlternation "${readerNames}")
string(REPLACE ";" ", " readersText "${readerNames}")

# A root naming no directory describes nothing, and reads exactly like one being honoured.
foreach(root IN LISTS thirdPartyRoots)
    if(NOT IS_DIRECTORY "${FASTCACHED_SOURCE_DIR}/${root}")
        message(FATAL_ERROR
            "third-party roots: scripts/lib/third-party-roots.txt names ${root}, which is not a "
            "directory of this tree. A root that describes nothing reads exactly like one being "
            "honoured -- remove the row, or restore the directory it names")
    endif()
endforeach()

# ---------------------------------------------------------------------------
# What a tree-wide enumerator IS. One row per spelling: its name, then the rule, which is
# printed beside its count so a reader of the output can see what was counted.
#
# "Command position" is the start of a line, or after `;` `&` `|` `(` `{` `!` `$(` or a
# shell keyword -- so a spelling quoted inside a message is not a call.
set(FastCachedEnumeratorSpellings
    "git-ls-files|`git ls-files` at command position with no pathspec, or with a literal pathspec naming no directory before its first `*` -- `'*.hpp'` walks every directory, `'src/*.hpp'` walks one, and a variable names a path chosen elsewhere"
    "glob-recurse|`file(GLOB_RECURSE` over a pattern at the source root naming no literal directory -- `\${FASTCACHED_SOURCE_DIR}/*.cpp` and `\${FASTCACHED_SOURCE_DIR}/CMakeLists.txt` both walk every directory (a recursive glob needs no `*` to recurse), `\${FASTCACHED_SOURCE_DIR}/src/*` walks one, and a variable names a path chosen elsewhere"
    "find|`find` at command position whose first path is `.` or a variable NAMED as the repository root (root, ROOT, source_dir, SOURCE_DIR, source_root, repo_root, REPO_ROOT, FASTCACHED_SOURCE_DIR) -- a name is all a line reader has, so a root passed under another name is not seen"
    "grep-r|`grep` at command position with `-r` or `-R`, over `.`"
)

set(commandPosition "(^|[;&|({!]|[$][(]|[ \t](then|do|else|if|while|until|exec|time))[ \t]*")
set(sourceRootVariables "FASTCACHED_SOURCE_DIR|CMAKE_SOURCE_DIR|PROJECT_SOURCE_DIR")

# ---------------------------------------------------------------------------
# The exemptions, as data beside the roots: `<path>|<spelling>|<reason>`. The reason is
# REQUIRED -- an exemption is a decision, and a row without one reads exactly like a row
# somebody forgot. A row naming a file that no longer spells that enumerator is STALE and
# refused, or the table silently outlives what it excused.
set(exemptionsFile "${FASTCACHED_SOURCE_DIR}/scripts/lib/third-party-roots-exemptions.txt")
set(exemptRows "")
if(EXISTS "${exemptionsFile}")
    file(READ "${exemptionsFile}" exemptionsContent)
    fastcached_split_lines_verbatim("${exemptionsContent}" exemptionLines)
    foreach(line IN LISTS exemptionLines)
        if(line MATCHES "^[ \t]*#" OR line MATCHES "^[ \t]*$")
            continue()
        endif()
        fastcached_row_fields("${line}" exemptPath exemptSpelling exemptReason)
        string(STRIP "${exemptReason}" exemptReason)
        if(exemptReason STREQUAL "")
            message(FATAL_ERROR
                "third-party roots: the exemption for ${exemptPath} (${exemptSpelling}) gives no reason. "
                "An exemption is a decision; without its reason it reads exactly like a row somebody forgot.")
        endif()
        list(APPEND exemptRows "${exemptPath}|${exemptSpelling}")
        set("exemptionUsed_${exemptPath}|${exemptSpelling}" FALSE)
    endforeach()
endif()

# ---------------------------------------------------------------------------
# The files scanned. One traversal per directory, each ANCHORED -- this check is not
# itself a tree-wide enumerator.
set(scanned "")
foreach(directory IN ITEMS "scripts" "cmake" ".github/workflows")
    if(IS_DIRECTORY "${FASTCACHED_SOURCE_DIR}/${directory}")
        file(GLOB_RECURSE walked RELATIVE "${FASTCACHED_SOURCE_DIR}" LIST_DIRECTORIES false
             "${FASTCACHED_SOURCE_DIR}/${directory}/*")
        list(FILTER walked INCLUDE REGEX "\\.(sh|cmake|ps1|yml|yaml)$")
        list(APPEND scanned ${walked})
    endif()
endforeach()
list(SORT scanned)
list(LENGTH scanned scannedCount)
if(scannedCount EQUAL 0)
    message(FATAL_ERROR
        "third-party roots: no script was found under scripts/, cmake/ or .github/workflows/; "
        "this check would pass vacuously")
endif()

# Whether @p spec, one `git ls-files` pathspec, walks every directory: it names no directory
# before its first `*` or variable.
function(fastcached_pathspec_is_tree_wide spec outVar)
    set(treeWide TRUE)
    if(spec MATCHES "^[^*$]*/" OR spec MATCHES "^[$]")
        set(treeWide FALSE)
    endif()
    set(${outVar} ${treeWide} PARENT_SCOPE)
endfunction()

# The `git ls-files` arguments on @p rest, as tokens: cut at the first shell or CMake
# terminator, redirections and flags dropped.
function(fastcached_ls_files_pathspecs rest outVar)
    string(REGEX REPLACE "[|;)&<>].*$" "" rest "${rest}")
    string(REGEX REPLACE "(OUTPUT_VARIABLE|RESULT_VARIABLE|ERROR_VARIABLE|WORKING_DIRECTORY|OUTPUT_STRIP_TRAILING_WHITESPACE).*$" "" rest "${rest}")
    string(REGEX REPLACE "[ \t][0-9]+$" "" rest "${rest}")
    string(REPLACE "\\;" " " rest "${rest}")
    separate_arguments(tokens UNIX_COMMAND "${rest}")
    set(specs "")
    foreach(token IN LISTS tokens)
        if(token MATCHES "^-" OR token STREQUAL "\\")
            continue()
        endif()
        list(APPEND specs "${token}")
    endforeach()
    set(${outVar} "${specs}" PARENT_SCOPE)
endfunction()

set(sites "")
set(violations "")
set(readingFiles "")
foreach(spellingRow IN LISTS FastCachedEnumeratorSpellings)
    fastcached_row_fields("${spellingRow}" spellingName spellingRule)
    set("siteCount_${spellingName}" 0)
endforeach()

foreach(relative IN LISTS scanned)
    file(READ "${FASTCACHED_SOURCE_DIR}/${relative}" content)
    # A call, outside a full-line comment: a comment is not a call site.
    set(readsRoots FALSE)
    string(REGEX REPLACE "(^|\n)[ \t]*#[^\n]*" "\\1" uncommented "${content}")
    if(uncommented MATCHES "(^|[^A-Za-z0-9_])(${readerAlternation})([^A-Za-z0-9_]|$)")
        set(readsRoots TRUE)
    endif()

    set(isCMake FALSE)
    if(relative MATCHES "\\.cmake$")
        set(isCMake TRUE)
    endif()

    # A CMake file may name the source root through an alias, `set(root "${FASTCACHED_SOURCE_DIR}")`.
    set(rootVariables "${sourceRootVariables}")
    if(isCMake)
        string(REGEX MATCHALL "set[ \t]*\\([ \t]*[A-Za-z0-9_]+[ \t]+\"[$][{](${sourceRootVariables})[}]\"[ \t]*\\)" aliasCalls "${content}")
        foreach(aliasCall IN LISTS aliasCalls)
            string(REGEX MATCH "\\([ \t]*([A-Za-z0-9_]+)" ignored "${aliasCall}")
            string(APPEND rootVariables "|${CMAKE_MATCH_1}")
        endforeach()
    endif()

    # A shell line ending in `\` would escape the list separator the splitter puts after
    # it and merge with the next line, shifting every later line number. A space after the
    # backslash keeps the lines apart and changes nothing matched here.
    string(REPLACE "\\\n" "\\ \n" content "${content}")
    fastcached_split_lines_verbatim("${content}" lines)
    list(LENGTH lines lineCount)
    set(lineNumber 0)
    foreach(line IN LISTS lines)
        math(EXPR lineNumber "${lineNumber} + 1")
        # Cheap before costly: most lines name none of the four words, and a CMake regex per
        # line over every script is what made the first version of this check slow.
        string(FIND "${line}" "ls-files" hasLsFiles)
        string(FIND "${line}" "GLOB_RECURSE" hasGlobRecurse)
        string(FIND "${line}" "find" hasFind)
        string(FIND "${line}" "grep" hasGrep)
        if(hasLsFiles EQUAL -1 AND hasGlobRecurse EQUAL -1 AND hasFind EQUAL -1 AND hasGrep EQUAL -1)
            continue()
        endif()
        if(line MATCHES "^[ \t]*#")
            continue()
        endif()
        string(REGEX REPLACE "[ \t]#.*$" "" code "${line}")

        # The next line, for a call whose arguments continue there. Fetched only when asked:
        # a `list(GET)` per line is quadratic in a six-thousand-line file.
        set(nextLine "")

        set(found "")

        # git-ls-files
        set(lsRest "")
        set(lsCall FALSE)
        if(isCMake)
            string(FIND "${code}" "ls-files" lsAt)
            if(NOT lsAt EQUAL -1)
                string(SUBSTRING "${code}" 0 ${lsAt} before)
                string(REGEX MATCHALL "\"" quotes "${before}")
                list(LENGTH quotes quoteCount)
                math(EXPR quoteParity "${quoteCount} % 2")
                if(quoteParity EQUAL 0 AND before MATCHES "(git|GIT_EXECUTABLE)")
                    set(lsCall TRUE)
                    math(EXPR restAt "${lsAt} + 8")
                    string(SUBSTRING "${code}" ${restAt} -1 lsRest)
                endif()
            endif()
        elseif(code MATCHES "${commandPosition}git[ \t]+(-C[ \t]+[^ \t]+[ \t]+)?ls-files(.*)$")
            set(lsCall TRUE)
            set(lsRest "${CMAKE_MATCH_4}")
        endif()
        if(lsCall)
            fastcached_ls_files_pathspecs("${lsRest}" specs)
            if(NOT specs AND (lsRest MATCHES "\\\\[ \t]*$" OR (isCMake AND NOT lsRest MATCHES "\\)")))
                if(lineNumber LESS lineCount)
                    list(GET lines ${lineNumber} nextLine)
                endif()
                fastcached_ls_files_pathspecs("${nextLine}" specs)
            endif()
            set(treeWide FALSE)
            list(LENGTH specs specCount)
            if(specCount EQUAL 0)
                set(treeWide TRUE)
            endif()
            foreach(spec IN LISTS specs)
                fastcached_pathspec_is_tree_wide("${spec}" specTreeWide)
                if(specTreeWide)
                    set(treeWide TRUE)
                endif()
            endforeach()
            if(treeWide)
                list(APPEND found "git-ls-files")
            endif()
        endif()

        # glob-recurse
        if(isCMake AND code MATCHES "^[ \t]*[fF][iI][lL][eE][ \t]*\\([ \t]*GLOB_RECURSE")
            set(statement "${code}")
            set(extra 0)
            string(REGEX MATCHALL "\\(" opens "${statement}")
            string(REGEX MATCHALL "\\)" closes "${statement}")
            list(LENGTH opens openCount)
            list(LENGTH closes closeCount)
            set(cursor ${lineNumber})
            while(openCount GREATER closeCount AND cursor LESS lineCount AND extra LESS 12)
                list(GET lines ${cursor} continuation)
                string(APPEND statement " ${continuation}")
                math(EXPR cursor "${cursor} + 1")
                math(EXPR extra "${extra} + 1")
                string(REGEX MATCHALL "\\(" opens "${statement}")
                string(REGEX MATCHALL "\\)" closes "${statement}")
                list(LENGTH opens openCount)
                list(LENGTH closes closeCount)
            endwhile()
            string(REGEX MATCHALL "\"[$][{](${rootVariables})[}]/[^\"]*\"" rootPatterns "${statement}")
            foreach(pattern IN LISTS rootPatterns)
                string(REGEX REPLACE "^\"[$][{][A-Za-z0-9_]+[}]/" "" subpath "${pattern}")
                # Tree-wide unless a literal directory comes first, or a variable names one.
                if(NOT subpath MATCHES "^[^/*]*/" AND NOT subpath MATCHES "^[$]")
                    list(APPEND found "glob-recurse")
                    break()
                endif()
            endforeach()
        endif()

        if(NOT isCMake)
            # A command continued with `\` is one command: its path argument may be lines away.
            set(joined "${code}")
            set(cursor ${lineNumber})
            set(extra 0)
            while(joined MATCHES "\\\\[ \t]*$" AND cursor LESS lineCount AND extra LESS 8)
                list(GET lines ${cursor} continuation)
                string(REGEX REPLACE "\\\\[ \t]*$" " " joined "${joined}")
                string(REGEX REPLACE "[ \t]#.*$" "" continuation "${continuation}")
                string(APPEND joined "${continuation}")
                math(EXPR cursor "${cursor} + 1")
                math(EXPR extra "${extra} + 1")
            endwhile()
            set(code "${joined}")
            # find
            if(code MATCHES "${commandPosition}find[ \t]+(\"?[$][{]?(root|ROOT|source_dir|SOURCE_DIR|source_root|repo_root|REPO_ROOT|FASTCACHED_SOURCE_DIR)[}]?\"?|\\.)([ \t]|$|\\))")
                list(APPEND found "find")
            endif()
            # grep-r
            if(code MATCHES "${commandPosition}grep[ \t]+(-[A-Za-z]*[rR][A-Za-z]*|--recursive)([ \t].*)?[ \t]\\.([ \t]|$|\\))")
                list(APPEND found "grep-r")
            endif()
        endif()

        list(REMOVE_DUPLICATES found)
        foreach(spelling IN LISTS found)
            math(EXPR "siteCount_${spelling}" "${siteCount_${spelling}} + 1")
            list(APPEND sites "${relative}:${lineNumber}")
            if(readsRoots)
                list(APPEND readingFiles "${relative}")
                continue()
            endif()
            set(rowKey "${relative}|${spelling}")
            if(rowKey IN_LIST exemptRows)
                set("exemptionUsed_${rowKey}" TRUE)
                continue()
            endif()
            list(APPEND violations
                 "${relative}:${lineNumber}: a tree-wide enumerator (${spelling}) in a file that does not read scripts/lib/third-party-roots.txt, so it takes every file under ${rootsText} as this project's own")
        endforeach()
    endforeach()
endforeach()

# ---------------------------------------------------------------------------
# A scan that found no enumerator at all is a statement about this check, not about the
# tree: two empty lists agree perfectly.
list(LENGTH sites siteCount)
if(siteCount EQUAL 0)
    message(FATAL_ERROR
        "third-party roots: no tree-wide enumerator was found in ${scannedCount} script(s); "
        "this check would pass vacuously")
endif()

foreach(rowKey IN LISTS exemptRows)
    if(NOT exemptionUsed_${rowKey})
        list(APPEND violations
             "scripts/lib/third-party-roots-exemptions.txt: the row for ${rowKey} is STALE -- that file no longer spells that enumerator, so the exemption excuses nothing and must go")
    endif()
endforeach()

# clang-format reads `.clang-format-ignore` for itself, so it is a second place a root must
# be named, and the one enumerator no script here can be made to ask.
set(formatIgnore "${FASTCACHED_SOURCE_DIR}/.clang-format-ignore")
set(formatIgnored "")
if(EXISTS "${formatIgnore}")
    file(STRINGS "${formatIgnore}" formatIgnored)
endif()
foreach(root IN LISTS thirdPartyRoots)
    if(NOT "${root}/**" IN_LIST formatIgnored AND NOT "${root}/" IN_LIST formatIgnored AND NOT "${root}" IN_LIST formatIgnored)
        list(APPEND violations
             ".clang-format-ignore: the third-party root ${root} is not named there, so clang-format rewrites it -- add `${root}/**`")
    endif()
endforeach()

list(REMOVE_DUPLICATES readingFiles)
list(LENGTH readingFiles readingCount)
set(countsText "")
foreach(spellingRow IN LISTS FastCachedEnumeratorSpellings)
    fastcached_row_fields("${spellingRow}" spellingName spellingRule)
    string(APPEND countsText "\n  ${spellingName}: ${siteCount_${spellingName}} site(s) -- ${spellingRule}")
endforeach()

if(violations)
    message("")
    foreach(violation IN LISTS violations)
        message("  ${violation}")
    endforeach()
    message("")
    message("A tree-wide enumerator lists this repository's files without naming a directory of")
    message("its own, so it takes third-party source as first-party unless it asks. Ask: split")
    message("the list with `first_party_paths` / `third_party_paths` from")
    message("scripts/lib/third-party-roots.sh, or `fastcached_decline_third_party` from")
    message("scripts/lib/CheckCommon.cmake, and name what was declined. Where the files it lists")
    message("truly need no such answer (it counts, or walks something that is not the")
    message("repository), add a row with the reason to scripts/lib/third-party-roots-exemptions.txt.")
    message("")
    message("This does NOT ask you to name ${rootsText} in the script: a root restated there is")
    message("the defect #1370 ends. The readers recognised: ${readersText}.")
    list(LENGTH violations violationCount)
    message(FATAL_ERROR "third-party roots: ${violationCount} violation(s)${countsText}")
endif()

list(LENGTH exemptRows exemptCount)
message(STATUS
    "third-party roots: ${siteCount} tree-wide enumerator site(s) in ${scannedCount} scanned script(s), "
    "${readingCount} file(s) reading the roots (${rootsText}) through one of (${readersText}), ${exemptCount} exemption(s)${countsText}")
