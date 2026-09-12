# SPDX-License-Identifier: Apache-2.0
#
# Nothing but `TextCell` itself may build a `CellKind::Text` cell.
#
# `fastcache-cli`'s `Cell` model carries a kind beside its text, and `CellKind::Text`
# means *these bytes are valid UTF-8* -- an invariant every renderer relies on, since
# `--format=json` interpolates `lexical` into a document that must be UTF-8 end to end.
# `TextCell` is where that is established: it asks `IsValidUtf8` and answers with a
# base64 `Binary` cell when the answer is no.
#
# ## Why a scan and not only the folding
#
# Because the folding is not reachable all the way down, and the tree has already paid
# for believing otherwise.
#
# The UTF-8 question used to be asked by a SECOND function, `TextOrBinaryCell`, whose
# doc comment read *"The one place the UTF-8 question is asked, so no command can
# forget to ask it."* That sentence was false as written and its falseness was the
# defect: nothing enforced it. Four sites called it and about twenty-nine called
# `TextCell`, and three of the twenty-nine carried bytes off the wire -- a memcached
# key from a GET reply, `ME`'s key, a meta flag value -- where no caller can assert
# anything about the encoding. A non-UTF-8 KEY therefore rendered as raw bytes and
# `--format=json` produced a document that was not UTF-8
# ([#1356](https://github.com/LASTRADA-Software/fastcached/issues/1356)).
#
# The fix folded the question into `TextCell`, which is the stronger half of this
# tree's rule: **a guard folded INTO the operation is self-enforcing; a guard called
# ALONGSIDE one needs a scan.** Moving three call sites to the other function would
# have left the next site free to skip the question again.
#
# What folding does NOT reach is `Cell` being an aggregate. `Cell { .kind =
# CellKind::Text, .lexical = bytes }` compiles anywhere in the binary and asks nothing,
# and no C++ this tree is willing to write forbids it -- the formatters read both
# members, and `AbsentCell` returns `Cell {}`. So that one shape is what the scan
# covers, and it covers only that.
#
# The rulebook's own sentence for the split: reach for the type system when the
# obligation is DO SOMETHING, reach for a scan when it is SAY WHY. This is the first
# kind and the type system does carry it, right up to the aggregate.
#
# ## Reads are not writes
#
# `case CellKind::Text:` in `CliFormat.cpp` and `cell.kind == CellKind::Text` in the
# tests are how the model is CONSUMED, and a check that refused those would refuse the
# renderers it exists to protect. So the violating shape is an assignment or an
# aggregate initialiser, not the token.
#
# The blind spots, stated rather than papered over, because a check whose edges are
# unnamed gets either trusted past them or widened on a guess:
#
#   * the enumerator routed through a variable -- `auto k = CellKind::Text; Cell { k,
#     s };` -- is invisible here. Nothing in this tree does that and the remedy if
#     something ever needs to is to call `TextCell`, not to widen this check.
#   * the token inside a STRING LITERAL reads as code, as in
#     `check-istreambuf-iterator.cmake`, and for the same reason: a literal-aware C++
#     lexer written in `cmake -P` would be a bigger liability than the case it covers.
#
# ## The factory file is DERIVED, never listed
#
# One file is allowed to write the enumerator: the one that defines `TextCell`. That
# file is found by searching for the definition rather than named in a table here.
#
# A hand-kept allowlist would be exact about the files it knows and silent about the
# ones it does not, and silence reads identically to complete coverage
# ([#492](https://github.com/LASTRADA-Software/fastcached/issues/492)). Worse for this
# check specifically: a listed path survives the file being MOVED, so the rename that
# relocated the factory would leave the check happily excusing a path that no longer
# exists while refusing the factory's new home -- and the fix a hurried reader reaches
# for is adding the new path to the list, leaving two.
#
# ## Fails closed, in two directions
#
# A scan that finds nothing must not be mistaken for a clean tree, so two things this
# repository definitely contains are required before any verdict is drawn: the factory
# must itself carry one of the write shapes (or the scan cannot recognise the shape at
# all), and the enumerator must be READ somewhere outside the factory (or it has been
# renamed and this scan is blind).
#
# Runs as `cmake -P`: it reads files, compares strings and reports. See
# `check-script-check-signals.cmake` for why such a check reports failure through its
# OUTPUT rather than an exit code.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -P scripts/check-cli-text-cell.cmake
#
# Exit codes: 0 always. The verdict is the presence of `CMake Error` in the output.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()

# Spelled once each: these are what is matched, what the matched-nothing guards count,
# and what the failure text names.
set(textEnumerator "CellKind::Text")

# The factory's DEFINITION, not its declaration. The leading newline is what separates
# them: a definition sits at namespace scope with no indentation, while the header's
# declaration is preceded by `[[nodiscard]] ` on the same line. Without it this matched
# `CliValue.hpp` as well and the check refused a healthy tree for having two factories
# -- which is the fail-closed direction, and is how this line came to be written.
set(factorySignature "\nCell TextCell(")
set(factoryShape "a line beginning `Cell TextCell(` at namespace scope")

# Walk a file line by line WITHOUT ever building a CMake list of lines.
#
# `;`, `\`, `[` and `]` are all CMake list structure and C++ is full of all four; the
# house `fastcached_read_lines` splitting idiom escapes them one at a time and still
# merges a line ending in a backslash with the next one, which hides the following
# line and drifts every line number below it -- a FALSE GREEN, measured and recorded
# on `fastcached_scan_lines` in `check-istreambuf-iterator.cmake`. This walks with
# `FIND`/`SUBSTRING` instead: immune to all four by construction rather than by
# escaping them.
#
# Consolidating the copies of that idiom is
# [#495](https://github.com/LASTRADA-Software/fastcached/issues/495) and is not this
# ticket -- absorbing it here would close one ticket by swallowing another.
#
# The walk is O(n^2) in the file's length, which is why the caller applies a whole-file
# test first.
function(fastcached_scan_text_cells content outWrites outReads)
    set(writes "")
    set(reads "")
    set(rest "${content}")
    set(lineNumber 0)
    set(inBlockComment FALSE)
    while(TRUE)
        string(FIND "${rest}" "\n" newline)
        if(newline EQUAL -1)
            set(line "${rest}")
        else()
            string(SUBSTRING "${rest}" 0 ${newline} line)
        endif()
        string(REGEX REPLACE "\r$" "" line "${line}")
        math(EXPR lineNumber "${lineNumber} + 1")

        # Strip comments, so the prose that documents this rule -- including the doc
        # comment on `TextCell` itself, which spells the aggregate shape to explain it
        # -- is not read as breaking it. That IS the exemption mechanism, derived
        # rather than listed.
        set(stripped "${line}")
        set(skipLine FALSE)
        if(inBlockComment)
            if(NOT stripped MATCHES "\\*/")
                set(skipLine TRUE)
            else()
                string(REGEX REPLACE "^.*\\*/" "" stripped "${stripped}")
                set(inBlockComment FALSE)
            endif()
        endif()
        if(NOT skipLine)
            # CMake's regex engine is greedy and has no lazy quantifier, so an inline
            # `/* ... */` strip takes everything between the FIRST `/*` and the LAST
            # `*/` on a line. A use between two block comments on one line is
            # invisible; nothing here does that.
            string(REGEX REPLACE "/\\*.*\\*/" " " stripped "${stripped}")

            # Which introducer comes FIRST decides. The order is not a detail: the
            # `/*` test used to run BEFORE `//` was stripped, so a line comment
            # mentioning `/*` opened a block comment no `*/` ever closed, and every
            # remaining line of that file was skipped while this still printed a
            # clean count over lines it never read -- a false green, in the one
            # direction the check exists to refuse.
            #
            # Positional rather than simply stripping `//` first, which MEASURED
            # identical on every input tried: below the inline `/* ... */` strip
            # above, removing `//...` removes any `/*` that followed it too, so the
            # two orderings agree. What position buys is not a different verdict but
            # independence -- it states the rule itself rather than being correct
            # only while the strip above it keeps running first. A reordering is
            # correct by PRECONDITION; this is correct by construction.
            #
            # Still blind to either introducer inside a STRING LITERAL, as every
            # regex-shaped reader here is. That is unchanged by this and is not what
            # was fixed.
            string(FIND "${stripped}" "/*" blockAt)
            string(FIND "${stripped}" "//" lineAt)
            if(NOT blockAt EQUAL -1 AND (lineAt EQUAL -1 OR blockAt LESS lineAt))
                string(SUBSTRING "${stripped}" 0 ${blockAt} stripped)
                set(inBlockComment TRUE)
            elseif(NOT lineAt EQUAL -1)
                string(SUBSTRING "${stripped}" 0 ${lineAt} stripped)
            endif()

            # Tabs to spaces before matching. CMake's regex has no `\t` escape -- it
            # would match a literal `t` -- so the alternative is a tab character
            # inside the pattern, which no reader can see.
            string(REPLACE "\t" " " stripped "${stripped}")

            # A WRITE: an assignment (designated initialiser included) or a positional
            # aggregate initialiser. `kind *= *` cannot match `kind == ` -- after the
            # single `=` the pattern requires `C` and finds the second `=` -- so a
            # comparison stays a read, which is the whole point.
            if(stripped MATCHES "kind *= *${textEnumerator}"
               OR stripped MATCHES "Cell *\\{ *${textEnumerator}")
                list(APPEND writes "${lineNumber}")
            elseif(stripped MATCHES "${textEnumerator}")
                list(APPEND reads "${lineNumber}")
            endif()
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
    set(${outWrites} "${writes}" PARENT_SCOPE)
    set(${outReads} "${reads}" PARENT_SCOPE)
endfunction()

# Which C++ this REPOSITORY owns, asked of git rather than inferred from directory
# names: a dependency cache is untracked by construction, whatever a package manager
# calls it or wherever it puts it. The same idiom, and the same fallback, as
# `check-istreambuf-iterator.cmake`.
if(NOT GIT_EXECUTABLE)
    find_program(GIT_EXECUTABLE NAMES git)
endif()

set(sourceFiles "")
set(scanSource "")

if(GIT_EXECUTABLE)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${FASTCACHED_SOURCE_DIR}" rev-parse --is-inside-work-tree
        OUTPUT_VARIABLE insideWorkTree
        ERROR_VARIABLE gitError
        RESULT_VARIABLE gitStatus
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(gitStatus EQUAL 0 AND insideWorkTree STREQUAL "true")
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${FASTCACHED_SOURCE_DIR}" ls-files
                    -- "src/*.cpp" "src/*.hpp"
            OUTPUT_VARIABLE tracked
            RESULT_VARIABLE lsStatus
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(lsStatus EQUAL 0 AND NOT tracked STREQUAL "")
            string(REPLACE "\n" ";" sourceFiles "${tracked}")
            set(scanSource "git ls-files")
        endif()
    endif()
endif()

# No git, or an export with no index. A source export contains no build tree and no
# dependency cache BY CONSTRUCTION, so the fallback is a plain recursive glob with the
# build-tree names still excluded -- and it says which mode produced the answer,
# because a fixture staging synthetic trees takes this path while CI takes git, and a
# check that cannot say which was used cannot be held to it.
if(NOT sourceFiles)
    set(excludeNames "out" "build" "_deps" ".git" ".cache" ".claude")
    file(GLOB_RECURSE walked RELATIVE "${FASTCACHED_SOURCE_DIR}"
         "${FASTCACHED_SOURCE_DIR}/src/*.cpp"
         "${FASTCACHED_SOURCE_DIR}/src/*.hpp")
    foreach(candidate IN LISTS walked)
        set(excluded FALSE)
        foreach(name IN LISTS excludeNames)
            if(candidate MATCHES "(^|/)${name}/")
                set(excluded TRUE)
                break()
            endif()
        endforeach()
        if(NOT excluded)
            list(APPEND sourceFiles "${candidate}")
        endif()
    endforeach()
    set(scanSource "directory walk (no git index)")
endif()

list(REMOVE_DUPLICATES sourceFiles)
list(SORT sourceFiles)

if(NOT sourceFiles)
    message("")
    message("  No C++ source was found under src/ in this repository at all.")
    message("")
    message("That is not a clean tree, it is a scan that stopped working -- a moved")
    message("source root, or a FASTCACHED_SOURCE_DIR pointing somewhere else.")
    message(FATAL_ERROR "cli-text-cell: the scan matched no C++ sources and cannot conclude")
endif()

# Taken here rather than beside the success message, because the REFUSAL path names it
# too and a value computed after the block that reads it interpolates as empty.
list(LENGTH sourceFiles fileCount)

# ---------------------------------------------------------------------------
# One pass, one read per file. Both questions -- *is this the factory* and *does this
# mention the enumerator* -- are answered from the same `file(READ)`, because reading
# every source twice to answer them separately doubled the check's cost for nothing.
# Measured on this tree over DrvFs, warm cache, 754 sources: 5.3s for two passes
# against 2.9s for the sibling scan's one. The numbers are pinned to that arrangement
# deliberately rather than pointing at a live figure, since they are a reading of one
# machine at one instant.
#
# The factory verdict is available in the same iteration that scans the file, so
# nothing has to be stashed and re-attributed afterwards.
set(factoryFiles "")
set(violations "")
set(factoryWrites 0)
set(foreignReads 0)

foreach(relative IN LISTS sourceFiles)
    # `git ls-files` lists the INDEX, so a file deleted from the worktree and not yet
    # staged is still named here. Reading it emits `CMake Error ... failed to open for
    # reading`, which the registration scores as THIS check failing for a reason with
    # nothing to do with cells.
    if(NOT EXISTS "${FASTCACHED_SOURCE_DIR}/${relative}")
        continue()
    endif()

    file(READ "${FASTCACHED_SOURCE_DIR}/${relative}" wholeFile)
    string(FIND "${wholeFile}" "${factorySignature}" factoryPosition)
    string(FIND "${wholeFile}" "${textEnumerator}" enumeratorPosition)

    if(NOT factoryPosition EQUAL -1)
        list(APPEND factoryFiles "${relative}")
    endif()

    # A whole-file test before the line walk. Almost nothing in this tree mentions the
    # enumerator, and splitting every source into lines to discover that costs seconds
    # on every platform for nothing -- the measurement that took a sibling scan from
    # 2.9s to 208ms.
    if(enumeratorPosition EQUAL -1)
        continue()
    endif()

    fastcached_scan_text_cells("${wholeFile}" writes reads)

    if(NOT factoryPosition EQUAL -1)
        list(LENGTH writes writeCount)
        math(EXPR factoryWrites "${factoryWrites} + ${writeCount}")
        continue()
    endif()

    list(LENGTH reads readCount)
    math(EXPR foreignReads "${foreignReads} + ${readCount}")
    foreach(lineNumber IN LISTS writes)
        list(APPEND violations "${relative}:${lineNumber}")
    endforeach()
endforeach()

# Asked BEFORE the violations are reported. With no factory found every legitimate
# write is a violation, and a list of them would send somebody correcting the factory
# to satisfy a check whose real complaint is that it can no longer find it.
list(LENGTH factoryFiles factoryCount)
if(NOT factoryCount EQUAL 1)
    message("")
    message("  Looked for the one definition of TextCell -- ${factoryShape} -- and found ${factoryCount}.")
    foreach(candidate IN LISTS factoryFiles)
        message("    ${candidate}")
    endforeach()
    message("")
    message("This check excuses exactly one file -- the one that DEFINES the factory --")
    message("and it finds that file by searching rather than by carrying a path. Zero")
    message("means the factory was renamed or removed, in which case the rule this")
    message("check enforces no longer has a subject and the check needs rewriting")
    message("rather than satisfying. More than one means there are two factories, and")
    message("two places asking the UTF-8 question is the shape #1356 was about.")
    message("")
    message("Enumerated ${fileCount} C++ source(s) via ${scanSource}.")
    message(FATAL_ERROR "cli-text-cell: expected exactly one factory definition, found ${factoryCount}")
endif()

list(GET factoryFiles 0 factoryFile)

# THE FIRST FAIL-CLOSED GUARD. The factory must carry a write shape, or this scan
# cannot recognise the shape it exists to find and every clean verdict below is
# vacuous. A guard nobody has watched RECOGNISE anything is not known to work.
if(factoryWrites EQUAL 0)
    message("")
    message("  ${factoryFile} defines the factory and writes `${textEnumerator}` nowhere")
    message("  this scan can see.")
    message("")
    message("The factory is the one site that legitimately builds a Text cell, so it is")
    message("also the scan's positive control: if the write shape is not recognised")
    message("THERE, it will not be recognised anywhere, and a clean result over the rest")
    message("of the tree means nothing. Either the factory now spells the construction")
    message("some other way -- in which case teach this scan that spelling -- or the")
    message("comment stripping above has begun eating code.")
    message("")
    message("Enumerated ${fileCount} C++ source(s) via ${scanSource}.")
    message(FATAL_ERROR "cli-text-cell: the factory carries no recognisable write, so the scan cannot conclude")
endif()

# THE SECOND. The enumerator must be READ outside the factory. `CliFormat.cpp`
# switches on it and the tests compare against it, so zero means it was renamed and
# this scan is looking for a token that no longer exists -- which reads exactly like a
# clean tree. Two empty results agree perfectly.
if(foreignReads EQUAL 0)
    message("")
    message("  `${textEnumerator}` is read nowhere outside ${factoryFile}.")
    message("")
    message("The renderers switch on it and the tests compare against it, so this is not")
    message("a tree where nobody bypasses the factory -- it is a scan that is no longer")
    message("looking at what it thinks it is. A renamed enumerator produces exactly this")
    message("result, and it is indistinguishable from success unless it is reported.")
    message("")
    message("Enumerated ${fileCount} C++ source(s) via ${scanSource}.")
    message(FATAL_ERROR "cli-text-cell: the enumerator is never read, so the scan cannot conclude")
endif()

if(violations)
    list(LENGTH violations violationCount)
    message("")
    foreach(violation IN LISTS violations)
        message("  ${violation}: builds a `${textEnumerator}` cell without the factory")
    endforeach()
    message("")
    message("`${textEnumerator}` means *these bytes are valid UTF-8*, and that is")
    message("established in exactly one place: `TextCell`, which asks `IsValidUtf8` and")
    message("answers with a base64 `Binary` cell when the answer is no. A cell built")
    message("past it carries the kind without the property, and `--format=json` then")
    message("interpolates bytes into a document that has to be UTF-8 end to end (#1356).")
    message("")
    message("Call the factory:")
    message("")
    message("  Cell cell = TextCell(std::move(bytes));")
    message("")
    message("It is total -- it takes anything and answers Text or Binary -- so there is")
    message("no input for which hand-building the cell is the only option.")
    message("")
    message("WHAT THIS DOES NOT SAY: nothing here objects to READING the enumerator. A")
    message("`case ${textEnumerator}:` and a `== ${textEnumerator}` are how the model is")
    message("consumed and are deliberately untouched, so do not delete a renderer's")
    message("switch arm to satisfy this.")
    message("")
    # Named on the REFUSAL path too. Which enumeration produced a verdict is part of
    # the verdict: a fixture staging synthetic trees takes the walk while CI takes git.
    message("Enumerated ${fileCount} C++ source(s) via ${scanSource}.")
    message(FATAL_ERROR "cli-text-cell: ${violationCount} site(s) build a Text cell without the factory")
endif()

message(STATUS
    "cli-text-cell: ${factoryWrites} write(s) in ${factoryFile}, ${foreignReads} read(s) elsewhere, "
    "no bypass across ${fileCount} C++ source(s) via ${scanSource}")
