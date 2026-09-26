# SPDX-License-Identifier: Apache-2.0
#
# No first-party translation unit is compiled with a GLOBAL instruction-set flag (#1442).
#
# `-msha`, `-msse4.1`, `-march=native` or `/arch:AVX2` on a target lets the compiler use those
# instructions anywhere in each unit it reaches -- including in the out-of-line copies of inline
# functions the linker then hands to code that runs on every CPU. A hardware path asks for its
# instructions per function and runs only once the CPU has been asked (#1420 carries the rule and
# the one such path).
#
# **No CI run can catch a violation**: the runners HAVE the instructions, so a target that gains
# `-msha` builds and passes everywhere it is tested and dies of SIGILL on the customer CPU that lacks
# them. So the question is asked of the flags, not of a run.
#
# ## Why the compile database
#
# It is the artefact every flag reaches after CMake has merged them -- a preset, a `-D` on a configure
# line, `CMAKE_<LANG>_FLAGS` and its `_INIT`, a toolchain file, target and per-source options,
# generator expressions. A scan of CMake files would see only the spellings somebody thought to look
# for. It is asked of the database of the build that SHIPS as well as of the ones that test:
# `scripts/check-instruction-set-flags.sh` says why, and runs it from the package jobs.
#
# ## First-party is an inclusion list: `src/`
#
# Not "the source tree minus dependencies": CI points `CPM_SOURCE_CACHE` inside the workspace, so every
# dependency's units sit under the source tree, and an exclusion rule bets on a layout this repository
# does not control. Units outside `src/` -- dependencies, `vendor/monocypher`, sources generated into the
# build tree -- are declined, counted and the first one named. A flag on a first-party TARGET reaches
# that target's `src/` units too, so declining its generated units loses nothing this rule is about.
#
# ## An unknown `-m` flag is refused
#
# `-m` reaches instruction sets (`-msha`, `-mavx2`) and much besides, and an allowlist that did not name
# a flag would read identically to one that had judged it. So a flag that enables nothing gets a row
# with its reason, and a row exists only for a spelling something produces -- the first real new one
# arrives as a refusal. `-arch` names an Apple slice, and the two BASELINE slices are rows of their own:
# CMake passes `-arch arm64` on every Apple build -- the macOS leg's database carries it on all 709
# first-party units, and this tree sets no `CMAKE_OSX_ARCHITECTURES`, so the flag comes from CMake's own
# Apple handling rather than from anything here. `arm64` and `x86_64` imply no instruction set beyond
# their architecture's own; every other value is refused, `x86_64h` because it implies AVX2 and an
# unknown slice because a slice is exactly the thing that can imply instructions. The spelling ALONE is
# refused too, since the value can arrive in a token of its own.
# A target triple (`--target=`, `-target`, `-Xclang -triple`)
# is refused for the same reason, and nothing here sets `CMAKE_<LANG>_COMPILER_TARGET`; so is clang's
# `-Xclang -target-cpu`, which is `-march=` one layer down. Every other spelling of a refused flag is read as
# the flag it spells: clang's `-Xclang=<flag>`, clang-cl's `-clang:` beside `/clang:`, GCC's `--machine-<ext>`.
#
# ## What it does NOT cover, stated so nobody reads it as more
#
#   - Per-function attributes (`__attribute__((target(...)))`, `[[gnu::target]]`): the legitimate spelling.
#   - Target pragmas: `scripts/check-target-pragmas.cmake`, which asks the tree rather than a build.
#   - Units outside `src/`, including vendored and dependency code.
#   - Flags that never reach a compile command: a compiler's BUILD-TIME environment (cl's `CL` and `_CL_`,
#     clang's `CCC_OVERRIDE_OPTIONS`), driver configuration files clang reads beside its binary, defaults a
#     compiler was built with (a GCC configured `--with-arch=`), and the link line, where LTO code
#     generation may take its own `-march`. Those are asked of the COMPILER instead:
#     `src/tests/InstructionSetBaseline.cpp`, which every executable compiles (#1447).
#   - A generator that writes no compile database (Visual Studio).
#   - A flag inside a quoted define (`-DX="-msha"`) is REFUSED, not understood, and a response file named
#     inside a response file is refused rather than followed: the reader errs toward refusing.
#
# ## Reading the database in batches
#
# `string(JSON ... GET)` parses the WHOLE document on every call, so reading every entry one GET at a time is
# quadratic in the database's size, which grows with every target: measured before batching, 5.8 s on CMake 4.3.1
# on Windows and 2.8 s on CMake 3.28 on WSL for one walk of a 1.48 MB, ~900-entry database. So the document is parsed
# whole ONCE, to validate it and count its entries, and is then read in batches of about `BatchBytes` of text, each
# cut after an entry and parsed on its own.
#
# The cut rests on a claim about CMake's WRITER, not about JSON: CMake puts each entry's closing brace alone at the
# start of a line, so `\n},` can end an entry and nothing else -- a JSON string cannot hold a raw line break, and a
# CMake entry holds no nested object. The claim is checked before it is used: the document must hold as many
# line-leading `}` as it has entries (one with no line break at all holds none), and every batch must parse on its
# own (a cut inside an entry leaves that entry's braces unbalanced, so it cannot). A database laid out any other way
# is read whole -- exactly as correctly, and quadratically -- and every run says which path it took,
# `parse: batched (N batch(es))` or
# `parse: whole-document (fallback: <reason>)`, so a fast path that stopped engaging is visible rather than slow.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<tree> -DFASTCACHED_COMPILE_DATABASE=<compile_commands.json>
#         [-DFASTCACHED_PLANT_UNIT=src/<unit>] -P scripts/check-instruction-set-flags.cmake
#
# With FASTCACHED_PLANT_UNIT, each of that unit's real commands also gets its grammar's PlantRows flag,
# and the run passes only when every planted flag is refused -- proof, on a real database, that the
# reading still sees a flag on a real first-party unit. The planted flag goes into the command text, so
# the extraction is exercised and not only the rows.
#
# The verdict is `CMake Error` in the output, never the exit code.

cmake_minimum_required(VERSION 3.28)

include("${CMAKE_CURRENT_LIST_DIR}/lib/CheckCommon.cmake")

foreach(required IN ITEMS FASTCACHED_SOURCE_DIR FASTCACHED_COMPILE_DATABASE)
    if("${${required}}" STREQUAL "")
        message(FATAL_ERROR "instruction-set-flags: ${required} must be set")
    endif()
endforeach()

# families|reason|driver-name pattern, on the lower-cased file name with `.exe` removed. First match wins.
# `scripts/tidy-sweep.sh` classifies cl-style drivers for a different question and keeps its own pattern.
set(DriverRows
    "Msvc,Gnu,ClangCl|clang-cl reads cl's /arch:, GCC's -m flags, and hands /clang:<flag> to the GCC grammar|^clang-cl(-[0-9.]+)?$"
    "Msvc|cl reads /arch: and -arch:|^cl$"
    "Gnu|GCC and clang read -m, -march= and -mcpu=, and Apple's clang reads -arch|^([a-z0-9_.]+-)*(clang|clang\\+\\+|gcc|g\\+\\+|cc|c\\+\\+)(-[0-9.]+)?$"
)

# family|verdict|kind|reason|spelling. The first row matching a token wins, and a token no row matches
# enables nothing in that grammar. Candidate tokens are EXTRACTED by a pattern derived from these rows, so
# a new row is read as soon as it is written.
#   verdict  allow, refuse, or unwrap (judge what follows the spelling by the Gnu rows)
#   kind     exact, prefix, iprefix (case-insensitive prefix), or flag-value (the spelling, then one value). A
#            flag-value row also judges its spelling ALONE, because the value can arrive as a token of its own --
#            clang-cl's `/clang:-arch /clang:x86_64h` hands the pair over one `/clang:` at a time -- so it must
#            refuse whatever the value is.
set(SpellingRows
    "Gnu|allow|prefix|it is the OS API floor CMakeLists.txt pins through CMAKE_OSX_DEPLOYMENT_TARGET, and enables no instruction|-mmacosx-version-min="
    "Gnu|allow|exact|it is the baseline Apple silicon slice, implies no instruction set beyond the architecture's own, and CMake passes it on every Apple build|-arch arm64"
    "Gnu|allow|exact|it is the baseline Intel slice, implying SSE2 and nothing above it -- x86_64h is the slice that implies more|-arch x86_64"
    "Gnu|refuse|flag-value|it names an Apple slice, and a slice can imply instructions (x86_64h implies AVX2)|-arch"
    "Gnu|refuse|prefix|it selects a CPU, and with it every instruction set that CPU has|-march="
    "Gnu|refuse|prefix|it selects a CPU, and with it every instruction set that CPU has|-mcpu="
    "Gnu|refuse|exact|it is clang's own switch for an instruction set, reached through -Xclang|-target-feature"
    "Gnu|refuse|exact|it is clang's own switch for a CPU, reached through -Xclang, and selects every instruction set that CPU has|-target-cpu"
    "Gnu|refuse|exact|it is clang's own switch for a target triple, reached through -Xclang, and a triple can imply instructions (x86_64h implies AVX2)|-triple"
    "Gnu|refuse|prefix|it names a target triple, and a triple can imply instructions (x86_64h implies AVX2)|--target="
    "Gnu|refuse|flag-value|it names a target triple, and a triple can imply instructions (x86_64h implies AVX2)|-target"
    "Gnu|unwrap|prefix|clang reads -Xclang=<flag> as -Xclang <flag>, handing the flag to its frontend|-Xclang="
    "Gnu|refuse|prefix|it is GCC's long spelling of -m (--machine-sha and --machine=sha are -msha)|--machine"
    "Gnu|refuse|prefix|it is an -m flag no row has judged, and -m reaches instruction sets such as -msha and -mavx2|-m"
    "Msvc|refuse|iprefix|it enables an instruction set for the whole translation unit|/arch:"
    "Msvc|refuse|iprefix|it is cl's other spelling of /arch:|-arch:"
    "ClangCl|unwrap|prefix|clang-cl hands what follows to the GCC grammar|/clang:"
    "ClangCl|unwrap|prefix|clang-cl reads every cl option with a dash as well, so this is /clang:|-clang:"
)

# reason|extension of a response file this check may leave unread.
set(DeclinedResponseFiles
    "CMake writes the module map at build time from the dependency scan, and it names module files only|.modmap"
)

# family|flag
set(PlantRows
    "Gnu|-msha"
    "Msvc|/arch:AVX2"
)

# What each row's kind MEANS is decided here, once: `SpellingMatcher<N>` is row N's anchored pattern over one
# normalised candidate token, and the extraction pattern is the same spelling followed by what may end the token.
# A spelling is inserted unescaped, so it may hold only characters a regular expression reads literally; brackets,
# semicolons and `|` never appear in one.
set(tokenEnd "[^ \t\r\n\"]")
set(CandidateAlternatives "@${tokenEnd}+")
set(rowIndex -1)
foreach(row IN LISTS SpellingRows)
    math(EXPR rowIndex "${rowIndex} + 1")
    fastcached_row_fields("${row}" family verdict kind reason spelling)
    if(NOT spelling MATCHES "^[-/:=_A-Za-z0-9]+( [-_A-Za-z0-9]+)?$")
        message(FATAL_ERROR "instruction-set-flags: SpellingRows spelling `${spelling}` holds a character the derived pattern would read as regex syntax")
    endif()
    if(NOT verdict MATCHES "^(allow|refuse|unwrap)$")
        message(FATAL_ERROR "instruction-set-flags: SpellingRows names an unknown verdict `${verdict}` for `${spelling}`")
    endif()
    if(kind STREQUAL "flag-value" AND NOT verdict STREQUAL "refuse")
        message(FATAL_ERROR "instruction-set-flags: SpellingRows row `${spelling}` is flag-value and does not refuse, but its value may arrive in a token of its own, where the row cannot read it")
    endif()
    set(alternative "")
    if(kind STREQUAL "iprefix")
        string(LENGTH "${spelling}" length)
        math(EXPR last "${length} - 1")
        foreach(at RANGE ${last})
            string(SUBSTRING "${spelling}" ${at} 1 character)
            string(TOUPPER "${character}" upper)
            string(TOLOWER "${character}" lower)
            if(upper STREQUAL lower)
                string(APPEND alternative "${character}")
            else()
                string(APPEND alternative "[${upper}${lower}]")
            endif()
        endforeach()
    else()
        set(alternative "${spelling}")
    endif()
    # A candidate reaches a matcher with its whitespace folded to one space and its quotes removed, so a flag-value row
    # matches its spelling followed by a value or standing alone. Either half of the pair may be quoted (`"-arch"
    # "x86_64h"`), which a single token never needs to be extracted.
    if(kind STREQUAL "flag-value")
        set(SpellingMatcher${rowIndex} "^${alternative}( |$)")
        string(APPEND alternative "\"?[ \t\r\n]+\"?${tokenEnd}+")
    elseif(kind STREQUAL "exact")
        set(SpellingMatcher${rowIndex} "^${alternative}$")
        string(APPEND alternative "${tokenEnd}*")
    elseif(kind MATCHES "^(prefix|iprefix)$")
        set(SpellingMatcher${rowIndex} "^${alternative}")
        string(APPEND alternative "${tokenEnd}*")
    else()
        message(FATAL_ERROR "instruction-set-flags: SpellingRows names an unknown kind `${kind}` for `${spelling}`")
    endif()
    string(APPEND CandidateAlternatives "|${alternative}")
endforeach()
set(CandidatePattern "(^|[ \t\r\n\"])(${CandidateAlternatives})")

# @p path with forward slashes and no `..`, lower-cased on a host whose paths are case-insensitive. The key decides;
# the optional third argument receives the same path in its own case, for a name shown to a reader. Lower-casing
# ASCII keeps the length, so one offset cuts both.
function(fastcached_path_key path out)
    cmake_path(CONVERT "${path}" TO_CMAKE_PATH_LIST path NORMALIZE)
    if(ARGC GREATER 2)
        set(${ARGV2} "${path}" PARENT_SCOPE)
    endif()
    if(CMAKE_HOST_WIN32)
        string(TOLOWER "${path}" path)
    endif()
    set(${out} "${path}" PARENT_SCOPE)
endfunction()

# The families (a CMake list) the driver of @p command reads, or "" when no row names it.
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

# The candidate tokens in @p text. Brackets and semicolons are blanked first -- no spelling holds one -- so an
# unbalanced bracket in a neighbouring argument cannot fuse two candidates in the returned list.
function(fastcached_candidates text out)
    string(REGEX REPLACE "[][;]" " " text "${text}")
    string(REGEX MATCHALL "${CandidatePattern}" found "${text}")
    set(candidates "")
    foreach(token IN LISTS found)
        string(REGEX REPLACE "^[ \t\r\n\"]" "" token "${token}")
        string(REPLACE "\"" "" token "${token}")
        string(REGEX REPLACE "[ \t\r\n]+" " " token "${token}")
        list(APPEND candidates "${token}")
    endforeach()
    set(${out} "${candidates}" PARENT_SCOPE)
endfunction()

# The reason the first SpellingRow matching @p token under @p families refuses it, or "" when none does.
function(fastcached_refusal token families out)
    set(refusal "")
    set(rowIndex -1)
    foreach(row IN LISTS SpellingRows)
        math(EXPR rowIndex "${rowIndex} + 1")
        fastcached_row_fields("${row}" family verdict kind reason spelling)
        list(FIND families "${family}" applies)
        if(applies EQUAL -1 OR NOT token MATCHES "${SpellingMatcher${rowIndex}}")
            continue()
        endif()
        if(verdict STREQUAL "refuse")
            set(refusal "${reason}")
        elseif(verdict STREQUAL "unwrap")
            string(LENGTH "${spelling}" skip)
            string(SUBSTRING "${token}" ${skip} -1 inner)
            fastcached_refusal("${inner}" "Gnu" refusal)
        endif()
        break()
    endforeach()
    set(${out} "${refusal}" PARENT_SCOPE)
endfunction()

# Judge every candidate in @p text for @p unit, appending to the lists and counter the caller names: a problem's
# text to @p problemsVar, and each refused token as `<unit>|<token>` to @p refusedVar, which is what the plant is
# decided from -- never the wording of a problem.
# A function and not a macro: a macro substitutes its arguments textually and re-parses them, so a
# backslash in a Windows command would be eaten twice before a row ever saw it.
# @p depth is 0 for a command and 1 inside a response file, which may not name another.
function(fastcached_judge_text text families unit directory depth problemsVar refusedVar modmapsVar)
    set(found "${${problemsVar}}")
    set(refused "${${refusedVar}}")
    set(modmaps "${${modmapsVar}}")
    fastcached_candidates("${text}" candidates)
    foreach(token IN LISTS candidates)
        if(NOT token MATCHES "^@")
            fastcached_refusal("${token}" "${families}" refusal)
            if(NOT refusal STREQUAL "")
                list(APPEND found "${unit}: `${token}`, because ${refusal}")
                list(APPEND refused "${unit}|${token}")
            endif()
            continue()
        endif()
        string(SUBSTRING "${token}" 1 -1 response)
        cmake_path(GET response EXTENSION LAST_ONLY extension)
        set(responseDeclined FALSE)
        foreach(row IN LISTS DeclinedResponseFiles)
            fastcached_row_fields("${row}" rowReason rowExtension)
            if(extension STREQUAL rowExtension)
                set(responseDeclined TRUE)
                break()
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
                fastcached_judge_text("${responseText}" "${families}" "${unit}" "${directory}" 1 found refused modmaps)
            endif()
        endif()
    endforeach()
    set(${problemsVar} "${found}" PARENT_SCOPE)
    set(${refusedVar} "${refused}" PARENT_SCOPE)
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

# Batches, as "Reading the database in batches" above describes. `batch<N>` is a JSON array of whole entries and
# `batchEntries<N>` its length; the whole-document fallback is one batch holding the document itself.
set(BatchBytes 65536)
set(parseFallback "")
string(REGEX MATCHALL "\n}" entryEnds "${database}")
list(LENGTH entryEnds entryEndCount)
if(NOT entryEndCount EQUAL entryCount)
    set(parseFallback "${entryEndCount} line(s) start with `}` against ${entryCount} entries")
endif()
set(batchCount 0)
if(parseFallback STREQUAL "")
    string(FIND "${database}" "[" arrayOpen)
    string(FIND "${database}" "]" arrayClose REVERSE)
    math(EXPR bodyStart "${arrayOpen} + 1")
    math(EXPR bodyLength "${arrayClose} - ${bodyStart}")
    string(SUBSTRING "${database}" ${bodyStart} ${bodyLength} rest)
    string(LENGTH "${rest}" restLength)
    while(restLength GREATER 0)
        set(cut -1)
        if(restLength GREATER BatchBytes)
            string(SUBSTRING "${rest}" 0 ${BatchBytes} window)
            string(FIND "${window}" "\n}," cut REVERSE)
            if(cut EQUAL -1)
                string(FIND "${rest}" "\n}," cut)
            endif()
        endif()
        if(cut EQUAL -1)
            set(chunk "${rest}")
            set(rest "")
        else()
            math(EXPR chunkLength "${cut} + 2")
            math(EXPR restStart "${cut} + 3")
            string(SUBSTRING "${rest}" 0 ${chunkLength} chunk)
            string(SUBSTRING "${rest}" ${restStart} -1 rest)
        endif()
        string(LENGTH "${rest}" restLength)
        math(EXPR batchCount "${batchCount} + 1")
        set(batch${batchCount} "[${chunk}]")
        string(JSON batchEntries${batchCount} ERROR_VARIABLE batchError LENGTH "${batch${batchCount}}")
        if(NOT batchError STREQUAL "NOTFOUND")
            string(REGEX REPLACE "[ \t\r\n]+" " " batchError "${batchError}")
            set(parseFallback "batch ${batchCount} does not parse on its own (${batchError})")
            break()
        endif()
    endwhile()
endif()
if(parseFallback STREQUAL "")
    message(STATUS "instruction-set-flags: parse: batched (${batchCount} batch(es))")
else()
    set(batchCount 1)
    set(batch1 "${database}")
    set(batchEntries1 ${entryCount})
    message(STATUS "instruction-set-flags: parse: whole-document (fallback: ${parseFallback})")
endif()

fastcached_path_key("${FASTCACHED_SOURCE_DIR}/src/" sourceRootKey)
string(LENGTH "${sourceRootKey}" sourceRootLength)
set(plantUnit "${FASTCACHED_PLANT_UNIT}")
set(plantUnitKey "")
if(NOT plantUnit STREQUAL "")
    fastcached_path_key("${FASTCACHED_SOURCE_DIR}/${plantUnit}" plantUnitKey)
endif()

set(problems "")
set(refusedTokens "")
set(firstParty 0)
set(declined 0)
set(firstDeclined "")
set(modmapsDeclined 0)
set(plantedUnit "")
set(plantedEntries 0)
set(plantUnreadable 0)
set(plantFlags "")

set(index -1)
foreach(batch RANGE 1 ${batchCount})
    math(EXPR lastInBatch "${batchEntries${batch}} - 1")
    foreach(inBatch RANGE ${lastInBatch})
        math(EXPR index "${index} + 1")
        string(JSON entry GET "${batch${batch}}" ${inBatch})
        string(JSON unitFile ERROR_VARIABLE fileError GET "${entry}" file)
        string(JSON directory ERROR_VARIABLE directoryError GET "${entry}" directory)
        string(JSON command ERROR_VARIABLE commandError GET "${entry}" command)
        if(NOT fileError STREQUAL "NOTFOUND" OR NOT directoryError STREQUAL "NOTFOUND")
            list(APPEND problems "entry ${index}: has no `file` or `directory`, so which unit it compiles is unknown")
            continue()
        endif()
        cmake_path(ABSOLUTE_PATH unitFile BASE_DIRECTORY "${directory}" NORMALIZE OUTPUT_VARIABLE unitPath)
        fastcached_path_key("${unitPath}" unitKey unitPath)
        string(FIND "${unitKey}" "${sourceRootKey}" underSource)
        if(NOT underSource EQUAL 0)
            math(EXPR declined "${declined} + 1")
            if(firstDeclined STREQUAL "")
                set(firstDeclined "${unitPath}")
            endif()
            continue()
        endif()
        string(SUBSTRING "${unitPath}" ${sourceRootLength} -1 relative)
        set(unit "src/${relative}")
        math(EXPR firstParty "${firstParty} + 1")

        # A plant-unit entry that cannot be read is counted apart: the plant run drops the problem that says why, so
        # without the count it would report the unit as absent from a database that compiles it.
        set(unreadable "")
        if(NOT commandError STREQUAL "NOTFOUND")
            set(unreadable "its entry has no `command`, so the flags it is compiled with cannot be read")
        else()
            fastcached_driver_families("${command}" families driver)
            if(families STREQUAL "")
                set(unreadable "compiled by `${driver}`, a driver DriverRows does not name, so which flags enable instructions is unknown -- add a row saying which grammar it reads")
            endif()
        endif()
        if(NOT unreadable STREQUAL "")
            list(APPEND problems "${unit}: ${unreadable}")
            if(unitKey STREQUAL plantUnitKey)
                math(EXPR plantUnreadable "${plantUnreadable} + 1")
            endif()
            continue()
        endif()

        if(unitKey STREQUAL plantUnitKey)
            set(plantedUnit "${unit}")
            math(EXPR plantedEntries "${plantedEntries} + 1")
            foreach(plantRow IN LISTS PlantRows)
                fastcached_row_fields("${plantRow}" plantFamily plantFlag)
                list(FIND families "${plantFamily}" plantApplies)
                if(NOT plantApplies EQUAL -1)
                    string(APPEND command " ${plantFlag}")
                    list(APPEND plantFlags "${plantFlag}")
                endif()
            endforeach()
        endif()

        fastcached_judge_text("${command}" "${families}" "${unit}" "${directory}" 0 problems refusedTokens modmapsDeclined)
    endforeach()
endforeach()

if(firstParty EQUAL 0)
    list(APPEND problems "no entry compiles a unit under `${FASTCACHED_SOURCE_DIR}/src/` (${declined} declined), so no first-party flag has been judged")
endif()

set(summary "${firstParty} first-party unit(s) judged, none carries a global instruction-set flag; ${declined} unit(s) outside src/ declined")
if(declined GREATER 0)
    string(APPEND summary " (first: ${firstDeclined})")
endif()
string(APPEND summary "; ${modmapsDeclined} module-map response file(s) declined")

if(NOT plantUnit STREQUAL "")
    # The plant run answers ONE question -- is a planted flag still refused -- so only the plant decides it. A real
    # violation elsewhere in the database is the unplanted run's to report; failing here too would show one defect
    # as two reds, the second one blaming the plant.
    set(plantProblems "")
    if(plantedUnit STREQUAL "" AND plantUnreadable GREATER 0)
        list(APPEND plantProblems "plant: `${plantUnit}` is in this database, but none of its ${plantUnreadable} entr(y/ies) could be read (no `command`, or a driver DriverRows does not name), so the plant was never judged -- the unplanted run names why")
    elseif(plantedUnit STREQUAL "")
        list(APPEND plantProblems "plant: `${plantUnit}` is not a first-party unit of this database, so the plant was never judged")
    elseif(plantFlags STREQUAL "")
        list(APPEND plantProblems "plant: no PlantRows flag applies to `${plantUnit}`'s driver, so the plant was never judged")
    endif()
    # Each planted flag must have produced the refusal a real one would. That refusal is the expected outcome, so
    # one refusal of that token on that unit is spent per planted flag, and does not count as an unplanted problem;
    # a plant that produced none is a problem. A unit compiled by several targets has several entries, each planted.
    list(LENGTH problems otherProblems)
    foreach(flag IN LISTS plantFlags)
        list(FIND refusedTokens "${plantedUnit}|${flag}" at)
        if(at EQUAL -1)
            list(APPEND plantProblems "plant: `${flag}` planted into `${plantedUnit}` was ACCEPTED -- the check cannot see the flag it exists to refuse")
        else()
            list(REMOVE_AT refusedTokens ${at})
            math(EXPR otherProblems "${otherProblems} - 1")
        endif()
    endforeach()
    set(distinctFlags "${plantFlags}")
    list(REMOVE_DUPLICATES distinctFlags)
    list(JOIN distinctFlags "`, `" flagsText)
    set(summary "plant: `${flagsText}` planted into ${plantedUnit} (${plantedEntries} entr(y/ies)) refused as it must; ${firstParty} first-party unit(s) judged; ${otherProblems} unplanted problem(s) left to the unplanted run")
    set(problems "${plantProblems}")
endif()

if(problems STREQUAL "")
    message(STATUS "instruction-set-flags: ${summary}")
else()
    list(LENGTH problems problemCount)
    list(JOIN problems "\n  " problemText)
    # Only plant problems reach a plant run's refusal, and the tree cannot cause one, so it must not be sent there.
    if(plantUnit STREQUAL "")
        string(CONCAT remedy
            "An instruction set is asked for per function -- __attribute__((target(...))) -- and that function runs only "
            "once the CPU has been asked (#1420: Core/CpuFeatures, and the instruction-set extension entry in "
            ".agent/rules/build-and-toolchain.md). A flag that enables no instruction set gets a SpellingRows row with its "
            "reason; a new compiler gets a DriverRows row. What this check does not cover is listed in its header.")
    else()
        string(CONCAT remedy
            "The plant is a flag this check places itself and must refuse, so nothing in the tree needs changing: a plant "
            "ACCEPTED is a defect in this check's reading (the extraction pattern derived from SpellingRows, or a row), and "
            "a plant never judged means FASTCACHED_PLANT_UNIT names no first-party unit this database compiles, its entries "
            "cannot be read, or no PlantRows flag fits its driver. A real flag in the tree is the unplanted run's to report.")
    endif()
    message(FATAL_ERROR
        "instruction-set-flags: ${problemCount} problem(s) in `${FASTCACHED_COMPILE_DATABASE}`:\n  ${problemText}\n"
        "${remedy}")
endif()
