# SPDX-License-Identifier: Apache-2.0
#
# No first-party translation unit is compiled with a GLOBAL instruction-set flag (#1442).
#
# `-msha`, `-msse4.1`, `-march=native` or `/arch:AVX2` on a target lets the compiler use those
# instructions anywhere in each unit it reaches -- including in the out-of-line copies of inline
# functions the linker then hands to code that runs on every CPU. A hardware path asks for its
# instructions per function (`__attribute__((target(...)))`) and runs only once `Core/CpuFeatures`
# has said the CPU has them; `.agent/rules/build-and-toolchain.md` carries the rule and its reasons.
#
# ## Why a check, and why over the compile database
#
# **No CI run can catch a violation.** The runners HAVE the instructions, so a target that gains
# `-msha` builds and passes everywhere it is tested and dies of SIGILL on the customer CPU that
# lacks them. So the question is asked of the flags, not of a run.
#
# The compile database is the artefact: every flag a unit is compiled with is in its `command`,
# wherever it came from -- a preset, a `-D` on a configure line, `target_compile_options`, the
# environment. A scan of CMake files would see only the spellings somebody thought to look for.
# And it is asked of the database of the build that SHIPS as well as the one that tests: the
# package jobs configure without tests, and macOS packages from a configure line no test leg
# uses, so a step there runs this same script (`scripts/check-instruction-set-flags.sh`).
#
# ## What decides, as data
#
#   DriverRows          which grammar a unit's compiler reads, from the driver's NAME
#   SpellingRows        per grammar, which tokens are refused or allowed, first match wins
#   ResponseFileRows    which `@file` a command may name without this check reading it
#   PlantRows           per grammar, the flag the plant mode puts into a real unit
#
# An unknown `-m` flag is REFUSED by the last Gnu row, not allowed by absence: `-m` reaches
# instruction sets (`-msha`, `-mavx2`) and much besides, and an allowlist that did not name a flag
# would read identically to one that had judged it. A flag that enables nothing gets a row with
# its reason. The rows exist only for spellings something actually produces -- CMake itself on
# Apple (`Modules/Platform/Apple-Clang.cmake`: `-mmacosx-version-min=`, and `-arch` from
# `CMAKE_OSX_ARCHITECTURES`) -- so the first real new one arrives as a refusal.
#
# `-arch` is judged per VALUE: `x86_64h` is the Haswell slice and implies AVX2.
#
# ## First-party is an inclusion list: `src/`
#
# Not "the source tree minus dependencies". CI points `CPM_SOURCE_CACHE` inside the workspace, so
# every dependency's units sit under the source tree, and an exclusion rule bets on a layout this
# repository does not control. What is outside `src/` -- dependencies, `vendor/endo`, sources
# generated into the build tree -- is declined, counted and the first one named. A flag on a
# first-party TARGET reaches that target's `src/` units too, so declining its generated units
# loses nothing this rule is about.
#
# ## The second arm: target pragmas
#
# `#pragma GCC target(...)` and `#pragma clang attribute ... target(...)` enable an instruction
# set for every function after them in the unit, which is the same hazard with no flag to see.
# So every C and C++ source under `src/` is scanned for one, outside comments.
#
# ## How it reads, and why
#
# The candidate tokens are matched in the RAW command string with `[`, `]` and `;` blanked first,
# and never split into a CMake list of every argument: a `-DX=[a]` beside a flag would otherwise
# merge list elements and hide the flag. No refused or allowed spelling contains those characters,
# so blanking them changes nothing a row can match.
#
# ## What it does NOT cover, stated so nobody reads it as more
#
#   - Per-function attributes (`__attribute__((target(...)))`, `[[gnu::target]]`): the legitimate
#     spelling, and deliberately not refused.
#   - Units outside `src/`, including vendored and dependency code.
#   - A generator that writes no compile database (Visual Studio): the ctest is not registered.
#   - A flag spelled inside a quoted define (`-DX="-msha"`) is REFUSED, not understood: the reader
#     is narrower than a compiler, and errs toward refusing.
#   - A response file named inside a response file is refused rather than followed.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<tree> -DFASTCACHED_COMPILE_DATABASE=<compile_commands.json>
#         [-DFASTCACHED_PLANT_UNIT=src/<unit>] -P scripts/check-instruction-set-flags.cmake
#
# With FASTCACHED_PLANT_UNIT, the named unit's real command is judged as if it also carried its
# grammar's PlantRows flag, and the run passes only when that flag is refused -- the proof, on a
# real database, that the check still sees a planted flag on a real first-party unit. The pragma
# arm does not run in that mode.
#
# The verdict is `CMake Error` in the output, never the exit code.

cmake_minimum_required(VERSION 3.28)

include("${CMAKE_CURRENT_LIST_DIR}/lib/CheckCommon.cmake")

foreach(required IN ITEMS FASTCACHED_SOURCE_DIR FASTCACHED_COMPILE_DATABASE)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "instruction-set-flags: ${required} must be set")
    endif()
endforeach()

# families|reason|driver-name pattern (on the lower-cased file name, `.exe` removed). First match wins.
set(DriverRows
    "Msvc,Gnu,ClangCl|clang-cl reads cl's /arch:, GCC's -m flags, and hands /clang:<flag> to the GCC grammar|^clang-cl(-[0-9.]+)?$"
    "Msvc|cl reads /arch: and -arch:|^cl$"
    "Gnu|GCC and clang read -m, -march= and -mcpu=, and Apple's clang reads -arch|^([a-z0-9_.]+-)*(clang|clang\\+\\+|gcc|g\\+\\+|cc|c\\+\\+)(-[0-9.]+)?$"
)

# family|verdict|kind|reason|spelling. First row that matches wins; a token no row matches enables nothing
# in that grammar. kind: exact, prefix, iprefix (case-insensitive), pair (flag and value), pair-any (flag,
# any value), unwrap (judge the rest of the token by the Gnu rows).
set(SpellingRows
    "Gnu|allow|prefix|the OS API floor CMake derives from CMAKE_OSX_DEPLOYMENT_TARGET, which enables no instruction|-mmacosx-version-min="
    "Gnu|allow|pair|the arm64 slice CMAKE_OSX_ARCHITECTURES names, whose instructions are the ARMv8 baseline|-arch arm64"
    "Gnu|allow|pair|the x86_64 slice CMAKE_OSX_ARCHITECTURES names, which is the x86-64 baseline|-arch x86_64"
    "Gnu|refuse|pair-any|an Apple slice no row has judged, and x86_64h for one implies AVX2|-arch"
    "Gnu|refuse|prefix|it selects a CPU, and with it every instruction set that CPU has|-march="
    "Gnu|refuse|prefix|it selects a CPU, and with it every instruction set that CPU has|-mcpu="
    "Gnu|refuse|exact|it is clang's own switch for an instruction set, reached through -Xclang|-target-feature"
    "Gnu|refuse|prefix|it is an -m flag no row has judged, and -m reaches instruction sets such as -msha and -mavx2|-m"
    "Msvc|refuse|iprefix|it enables an instruction set for the whole translation unit|/arch:"
    "Msvc|refuse|iprefix|it is cl's other spelling of /arch:|-arch:"
    "ClangCl|unwrap|prefix|clang-cl hands what follows to the GCC grammar|/clang:"
)

# verdict|reason|extension
set(ResponseFileRows
    "decline|CMake writes the module map at build time from the dependency scan, and it names module files only|.modmap"
)

# family|flag
set(PlantRows
    "Gnu|-msha"
    "Msvc|/arch:AVX2"
)

set(plantUnit "")
if(DEFINED FASTCACHED_PLANT_UNIT AND NOT FASTCACHED_PLANT_UNIT STREQUAL "")
    set(plantUnit "${FASTCACHED_PLANT_UNIT}")
endif()

set(problems "")
set(firstParty 0)
set(declined 0)
set(firstDeclined "")
set(modmapsDeclined 0)
set(plantFound FALSE)
set(plantedUnitShown "")
set(plantFlags "")
set(plantRefused "")

# Lower-case on a host whose paths are case-insensitive, so a drive letter or a directory spelled
# differently by the compiler and by the configure line still compares equal.
function(fastcached_path_key path out)
    cmake_path(CONVERT "${path}" TO_CMAKE_PATH_LIST path NORMALIZE)
    if(CMAKE_HOST_WIN32)
        string(TOLOWER "${path}" path)
    endif()
    set(${out} "${path}" PARENT_SCOPE)
endfunction()

fastcached_path_key("${FASTCACHED_SOURCE_DIR}/src/" sourceRootKey)
fastcached_path_key("${plantUnit}" plantUnitKey)

# The families (a CMake list) a driver's name reads, or "" when no row names it.
function(fastcached_driver_families command familiesOut driverOut)
    string(REGEX MATCH "^[ \t]*(\"[^\"]*\"|[^ \t]+)" driver "${command}")
    string(STRIP "${driver}" driver)
    string(REPLACE "\"" "" driver "${driver}")
    cmake_path(GET driver FILENAME name)
    string(TOLOWER "${name}" name)
    string(REGEX REPLACE "\\.exe$" "" name "${name}")
    set(families "")
    foreach(row IN LISTS DriverRows)
        fastcached_row_fields("${row}" rowFamilies rowReason rowPattern)
        if(name MATCHES "${rowPattern}")
            string(REPLACE "," ";" families "${rowFamilies}")
            break()
        endif()
    endforeach()
    set(${familiesOut} "${families}" PARENT_SCOPE)
    set(${driverOut} "${name}" PARENT_SCOPE)
endfunction()

# The candidate tokens in @p text: anything a SpellingRow or a response file could be. Brackets and
# semicolons are blanked first, so the returned list is safe to walk.
function(fastcached_candidates text out)
    string(REGEX REPLACE "[][;]" " " text "${text}")
    string(REGEX MATCHALL
        "(^|[ \t\r\n\"])(-m[^ \t\r\n\"]*|-arch[ \t]+[^ \t\r\n\"]+|[-/][Aa][Rr][Cc][Hh]:[^ \t\r\n\"]*|-target-feature|/clang:[^ \t\r\n\"]*|@[^ \t\r\n\"]+)"
        found "${text}")
    set(candidates "")
    foreach(token IN LISTS found)
        string(REGEX REPLACE "^[ \t\r\n\"]" "" token "${token}")
        string(REGEX REPLACE "[ \t]+" " " token "${token}")
        list(APPEND candidates "${token}")
    endforeach()
    set(${out} "${candidates}" PARENT_SCOPE)
endfunction()

# The verdict of the first SpellingRow matching @p token under @p families: "refuse|<reason>", or "".
function(fastcached_judge_token token families out)
    set(verdict "")
    foreach(row IN LISTS SpellingRows)
        fastcached_row_fields("${row}" family rowVerdict kind reason spelling)
        list(FIND families "${family}" applies)
        if(applies EQUAL -1)
            continue()
        endif()
        set(matches FALSE)
        if(kind STREQUAL "exact" OR kind STREQUAL "pair")
            if(token STREQUAL spelling)
                set(matches TRUE)
            endif()
        elseif(kind STREQUAL "prefix" OR kind STREQUAL "unwrap")
            string(FIND "${token}" "${spelling}" at)
            if(at EQUAL 0)
                set(matches TRUE)
            endif()
        elseif(kind STREQUAL "iprefix")
            string(TOLOWER "${token}" lowerToken)
            string(TOLOWER "${spelling}" lowerSpelling)
            string(FIND "${lowerToken}" "${lowerSpelling}" at)
            if(at EQUAL 0)
                set(matches TRUE)
            endif()
        elseif(kind STREQUAL "pair-any")
            string(FIND "${token}" "${spelling} " at)
            if(at EQUAL 0)
                set(matches TRUE)
            endif()
        else()
            message(FATAL_ERROR "instruction-set-flags: SpellingRows names an unknown kind `${kind}` for `${spelling}`")
        endif()
        if(NOT matches)
            continue()
        endif()
        if(rowVerdict STREQUAL "unwrap")
            string(LENGTH "${spelling}" skip)
            string(SUBSTRING "${token}" ${skip} -1 inner)
            fastcached_judge_token("${inner}" "Gnu" innerVerdict)
            set(verdict "${innerVerdict}")
        elseif(rowVerdict STREQUAL "refuse")
            set(verdict "refuse|${reason}")
        endif()
        break()
    endforeach()
    set(${out} "${verdict}" PARENT_SCOPE)
endfunction()

# Judge every candidate in @p text for @p unit, appending to the lists and counter the caller names.
# A function and not a macro: a macro substitutes its arguments textually and re-parses them, so a
# backslash in a Windows command would be eaten twice before a row ever saw it.
# @p depth is 0 for a command and 1 inside a response file, which may not name another.
function(fastcached_judge_text text families unit directory depth problemsVar modmapsVar)
    set(found "${${problemsVar}}")
    set(modmaps "${${modmapsVar}}")
    fastcached_candidates("${text}" candidates)
    foreach(token IN LISTS candidates)
        string(SUBSTRING "${token}" 0 1 lead)
        if(lead STREQUAL "@")
            string(SUBSTRING "${token}" 1 -1 response)
            cmake_path(GET response EXTENSION LAST_ONLY extension)
            set(responseDeclined FALSE)
            foreach(row IN LISTS ResponseFileRows)
                fastcached_row_fields("${row}" rowVerdict rowReason rowExtension)
                if(extension STREQUAL rowExtension AND rowVerdict STREQUAL "decline")
                    set(responseDeclined TRUE)
                endif()
            endforeach()
            if(responseDeclined)
                math(EXPR modmaps "${modmaps} + 1")
            elseif(NOT depth EQUAL 0)
                list(APPEND found "${unit}: names `${token}` inside a response file, which this check does not follow")
            else()
                cmake_path(ABSOLUTE_PATH response BASE_DIRECTORY "${directory}" NORMALIZE OUTPUT_VARIABLE responsePath)
                if(NOT EXISTS "${responsePath}" OR IS_DIRECTORY "${responsePath}")
                    list(APPEND found "${unit}: cannot read response file `${token}` (${responsePath}), so not every flag it is compiled with can be seen")
                else()
                    file(READ "${responsePath}" responseText)
                    fastcached_judge_text("${responseText}" "${families}" "${unit}" "${directory}" 1 found modmaps)
                endif()
            endif()
            continue()
        endif()
        fastcached_judge_token("${token}" "${families}" verdict)
        if(NOT verdict STREQUAL "")
            fastcached_row_fields("${verdict}" word reason)
            list(APPEND found "${unit}: `${token}`, because ${reason}")
        endif()
    endforeach()
    set(${problemsVar} "${found}" PARENT_SCOPE)
    set(${modmapsVar} "${modmaps}" PARENT_SCOPE)
endfunction()

if(NOT EXISTS "${FASTCACHED_COMPILE_DATABASE}" OR IS_DIRECTORY "${FASTCACHED_COMPILE_DATABASE}")
    message(FATAL_ERROR "instruction-set-flags: the compile database `${FASTCACHED_COMPILE_DATABASE}` does not exist, so no flag has been judged")
endif()
file(READ "${FASTCACHED_COMPILE_DATABASE}" database)
string(JSON entryCount ERROR_VARIABLE jsonError LENGTH "${database}")
if(NOT jsonError STREQUAL "NOTFOUND")
    message(FATAL_ERROR "instruction-set-flags: `${FASTCACHED_COMPILE_DATABASE}` cannot be parsed as a compile database (${jsonError}), so no flag has been judged")
endif()
if(entryCount EQUAL 0)
    message(FATAL_ERROR "instruction-set-flags: `${FASTCACHED_COMPILE_DATABASE}` has no entries, so no flag has been judged")
endif()

math(EXPR lastEntry "${entryCount} - 1")
foreach(index RANGE ${lastEntry})
    string(JSON entry GET "${database}" ${index})
    string(JSON unitFile ERROR_VARIABLE fileError GET "${entry}" file)
    string(JSON directory ERROR_VARIABLE directoryError GET "${entry}" directory)
    string(JSON command ERROR_VARIABLE commandError GET "${entry}" command)
    if(NOT fileError STREQUAL "NOTFOUND" OR NOT directoryError STREQUAL "NOTFOUND")
        list(APPEND problems "entry ${index}: has no `file` or `directory`, so which unit it compiles is unknown")
        continue()
    endif()
    cmake_path(ABSOLUTE_PATH unitFile BASE_DIRECTORY "${directory}" NORMALIZE OUTPUT_VARIABLE unitPath)
    fastcached_path_key("${unitPath}" unitKey)
    string(FIND "${unitKey}" "${sourceRootKey}" underSource)
    if(NOT underSource EQUAL 0)
        math(EXPR declined "${declined} + 1")
        if(firstDeclined STREQUAL "")
            set(firstDeclined "${unitPath}")
        endif()
        continue()
    endif()
    # The key decides; the name shown is the path as written, so a Windows host does not print it lower-cased.
    # Lower-casing ASCII keeps the length, so the same offset cuts both.
    string(LENGTH "${sourceRootKey}" rootLength)
    cmake_path(CONVERT "${unitPath}" TO_CMAKE_PATH_LIST shownPath NORMALIZE)
    string(SUBSTRING "${shownPath}" ${rootLength} -1 relative)
    set(unit "src/${relative}")
    fastcached_path_key("${unit}" unitRelativeKey)
    math(EXPR firstParty "${firstParty} + 1")

    if(NOT commandError STREQUAL "NOTFOUND")
        list(APPEND problems "${unit}: its entry has no `command`, so the flags it is compiled with cannot be read")
        continue()
    endif()
    fastcached_driver_families("${command}" families driver)
    if(families STREQUAL "")
        list(APPEND problems "${unit}: compiled by `${driver}`, a driver DriverRows does not name, so which flags enable instructions is unknown -- add a row saying which grammar it reads")
        continue()
    endif()

    # The plant goes INTO the command text, so it is found by the same extraction and judged by the
    # same rows as a real flag; judging the flag alone would prove the rows and not the reading.
    if(NOT plantUnit STREQUAL "" AND unitRelativeKey STREQUAL plantUnitKey)
        set(plantFound TRUE)
        set(plantedUnitShown "${unit}")
        foreach(plantRow IN LISTS PlantRows)
            fastcached_row_fields("${plantRow}" plantFamily plantFlag)
            list(FIND families "${plantFamily}" plantApplies)
            if(NOT plantApplies EQUAL -1)
                string(APPEND command " ${plantFlag}")
                list(APPEND plantFlags "${plantFlag}")
            endif()
        endforeach()
    endif()

    fastcached_judge_text("${command}" "${families}" "${unit}" "${directory}" 0 problems modmapsDeclined)
endforeach()

if(firstParty EQUAL 0)
    list(APPEND problems "no entry compiles a unit under `${FASTCACHED_SOURCE_DIR}/src/` (${declined} declined), so no first-party flag has been judged")
endif()

set(declinedText "${declined} unit(s) outside src/ declined")
if(declined GREATER 0)
    string(APPEND declinedText " (first: ${firstDeclined})")
endif()

if(NOT plantUnit STREQUAL "")
    if(NOT plantFound)
        list(APPEND problems "plant: `${plantUnit}` is not a first-party unit of this database, so the plant was never judged")
    elseif(plantFlags STREQUAL "")
        list(APPEND problems "plant: no PlantRows flag applies to `${plantUnit}`'s driver, so the plant was never judged")
    endif()
    # Each planted flag must have produced exactly the refusal a real one would; that refusal is the
    # expected outcome, so it is taken out of the problems, and a plant that produced none is one.
    # A unit compiled by several targets has several entries, and each was planted and must be refused.
    foreach(flag IN LISTS plantFlags)
        set(expectedPrefix "${plantedUnitShown}: `${flag}`, because ")
        set(kept "")
        set(seen FALSE)
        foreach(problem IN LISTS problems)
            string(FIND "${problem}" "${expectedPrefix}" at)
            if(at EQUAL 0 AND NOT seen)
                set(seen TRUE)
            else()
                list(APPEND kept "${problem}")
            endif()
        endforeach()
        set(problems "${kept}")
        if(seen)
            list(APPEND plantRefused "${flag}")
        else()
            list(APPEND problems "plant: `${flag}` planted into `${plantUnit}` was ACCEPTED -- the check cannot see the flag it exists to refuse")
        endif()
    endforeach()
    if(problems STREQUAL "")
        list(LENGTH plantRefused plantedCount)
        list(REMOVE_DUPLICATES plantRefused)
        list(JOIN plantRefused "`, `" refusedText)
        message(STATUS "instruction-set-flags plant: `${refusedText}` planted into ${plantedUnitShown} (${plantedCount} entr(y/ies)) refused as it must; ${firstParty} first-party unit(s) judged, ${declinedText}")
    endif()
else()
    # The pragma arm: one traversal of src/, then only the files that mention a pragma at all.
    file(GLOB_RECURSE sourceFiles LIST_DIRECTORIES false "${FASTCACHED_SOURCE_DIR}/src/*")
    set(scanned 0)
    foreach(path IN LISTS sourceFiles)
        if(NOT path MATCHES "\\.(c|cc|cpp|cxx|h|hh|hpp|hxx|inl|ipp)$")
            continue()
        endif()
        math(EXPR scanned "${scanned} + 1")
        file(READ "${path}" content)
        string(FIND "${content}" "pragma" mentionsPragma)
        if(mentionsPragma EQUAL -1)
            continue()
        endif()
        fastcached_scan_code_lines("${content}"
            "#[ \t]*pragma[ \t]+(GCC[ \t]+target|clang[ \t]+attribute.*target)" hits)
        cmake_path(RELATIVE_PATH path BASE_DIRECTORY "${FASTCACHED_SOURCE_DIR}" OUTPUT_VARIABLE shown)
        foreach(hit IN LISTS hits)
            if(hit MATCHES "^use:([0-9]+)$")
                list(APPEND problems "${shown}:${CMAKE_MATCH_1}: a target pragma enables an instruction set for every function after it in the unit")
            endif()
        endforeach()
    endforeach()
    if(scanned EQUAL 0)
        list(APPEND problems "no C or C++ source under `${FASTCACHED_SOURCE_DIR}/src/`, so the target-pragma arm judged nothing")
    endif()
    if(problems STREQUAL "")
        message(STATUS "instruction-set-flags: ${firstParty} first-party unit(s) judged, none carries a global instruction-set flag; ${declinedText}; ${modmapsDeclined} module-map response file(s) declined; ${scanned} source(s) under src/ carry no target pragma")
    endif()
endif()

if(NOT problems STREQUAL "")
    list(LENGTH problems problemCount)
    list(JOIN problems "\n  " problemText)
    message(FATAL_ERROR
        "instruction-set-flags: ${problemCount} problem(s) in `${FASTCACHED_COMPILE_DATABASE}`:\n  ${problemText}\n"
        "An instruction set is asked for per function -- __attribute__((target(...))) -- and that function runs only "
        "once Core/CpuFeatures says the CPU has it (.agent/rules/build-and-toolchain.md, instruction-set extensions). "
        "A flag that enables no instruction set gets a SpellingRows row with its reason; a new compiler gets a DriverRows row. "
        "Not covered: per-function attributes, and units outside src/.")
endif()
