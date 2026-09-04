# SPDX-License-Identifier: Apache-2.0
#
# Policies are pinned because a `cmake -P` script gets OLD defaults for every
# policy the project has not stated -- CMP0057, where `if(... IN_LIST ...)`
# silently did nothing in a check.
cmake_minimum_required(VERSION 3.28)
#
# Every startup diagnostic `docs/operations/corrupt-store.md` quotes VERBATIM is
# still composable by the source that composes it.
#
# ## The failure this exists for
#
# That page's whole purpose is that an operator meeting `Corrupt` does NOT delete
# a healthy cache -- the rulebook is explicit that `Corrupt` is what makes
# somebody do that. Its usefulness rests entirely on the operator recognising
# their own console output in it. A refactor that changed `context=` or dropped
# the shard path would leave the page silently wrong, every test green, and the
# operator without the identification the page exists to give them (#633).
#
# Measured when #633 was filed: of the diagnostics the page quotes, exactly one
# was asserted anywhere. `CowTreeStorage_test.cpp` compares the `StorageError(...)`
# rendering exactly; `grep -rn "failed to open" --include=*_test.cpp src/` returned
# nothing at all.
#
# ## Why a docs-subject check rather than unit tests
#
# The sentences are composed in `src/apps/`, and the daemon's `StorageOpenFailure`
# sits in an ANONYMOUS NAMESPACE in `main.cpp` behind
# `add_executable(fastcached main.cpp)` with no test target anywhere -- so it is
# unreachable by unit test without restructuring the app. A check that READS BOTH
# SIDES needs no linkage at all, dodges the anonymous namespace, and covers the
# node's half too, which lives in a different binary again.
#
# The mechanism is the STORE lane's recommendation; the analysis in #633's thread
# is theirs.
#
# ## What is asserted, and what deliberately is not
#
# **Presence in the composing source, never equality with a formatted line.**
# #633 is explicit that a whole-line assertion is a change-detector, which is the
# thing it exists to avoid. So:
#
#   * for the `StorageError(...)` rendering the WHOLE format string is asserted,
#     and that is not a contradiction. The rendering IS the page's verbatim quote,
#     so a change-detector is exactly what it needs, and the literal moves with
#     the page.
#   * for the shard sentence the PATH'S PRESENCE is the field-level assertion --
#     the `'{}'` that carries the one damaged file. Asserting the whole shard
#     sentence would be the change-detector #633 warns about.
#
# Rewording the prose around a quote is free. Changing `context=`, dropping the
# `'{}'` that carries the path, or losing the `refusing to start` suffix is not.
#
# ## The node's sentence is composed in THREE places
#
# `--cache-dir {}` and `cannot open {}` in `CacheTier.cpp`, and `refusing to
# start` in the node's own `main.cpp`. A check that read only the tier would miss
# the suffix entirely, which is why the table below is per-FRAGMENT rather than
# per-sentence.
#
# ## The set is DERIVED from the page
#
# The diagnostics come from the page itself -- every line that carries one of the
# shapes it uses to show console output -- never from a list restated here. The
# table explains which source composes which FRAGMENT, and a diagnostic no row
# covers is REFUSED rather than passed over: an uncovered quote is exactly the
# silent gap this check exists to close.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -P scripts/check-corrupt-store-diagnostics.cmake
#
# Exit codes: 0 always. The verdict is the presence of `CMake Error` in the output.

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()

set(FastCachedCorruptStorePage "docs/operations/corrupt-store.md")

# ---------------------------------------------------------------------------
# Which source composes which fragment of a quoted diagnostic.
#
#   <fragment as it appears ON THE PAGE>|<source file>|<literal that must be there>|<why>
#
# No row may contain a `;` -- these are CMake lists, and a semicolon inside a row
# would split it into two.
set(FastCachedDiagnosticFragments
    "StorageError(code=|src/FastCache/Core/Errors/StorageError.hpp|StorageError(code={} system={} context={})|The rendering the page quotes character for character. The whole format string is pinned deliberately: this IS the verbatim quote, so a change-detector is what it needs."
    "context=FilePageStore::Open|src/FastCache/Cache/CowTreeStorage.cpp|\"FilePageStore::Open\"|The context token an operator matches against. Nothing else in this tree produces it, so a rename would leave the page naming a context that no longer exists."
    "failed to open storage '|src/apps/fastcached/main.cpp|failed to open {} '{}'|The daemon's one composer. The quoted `'{}'` is the field that matters -- it carries the path the operator needs to act on."
    "failed to open shard '|src/apps/fastcached/main.cpp|failed to open {} '{}'|The --storage-shards variant, which names the ONE damaged file. Same composer, different `what` argument."
    "--cache-dir |src/apps/fastcache-compile-node/CacheTier.cpp|--cache-dir {}|The node prefixes its tier's refusal with the flag the operator actually typed."
    "cannot open |src/apps/fastcache-compile-node/CacheTier.cpp|cannot open {}|The node's own open failure -- a different binary and a different sentence from the daemon's."
    "refusing to start|src/apps/fastcache-compile-node/main.cpp|refusing to start|The suffix the node's main adds. Composed in a THIRD place, so a check reading only the tier would miss it."
)

# The `what` arguments the daemon's single composer is called with. Separate from
# the table above because these are ARGUMENTS rather than format strings, and the
# page quotes both variants.
#
#   <fragment as it appears ON THE PAGE>|<literal argument>|<why>
set(FastCachedDaemonWhatLiterals
    "failed to open storage '|\"storage\"|The plain --storage path."
    "failed to open shard '|\"shard\"|The per-shard variant, which is the only one that names a single file."
)

# ---------------------------------------------------------------------------
# Read a file whole, and NEVER split it into a CMake list.
#
# Everything below asks only whether a literal is PRESENT, and `string(FIND)` over
# the whole content answers that -- which is immune BY CONSTRUCTION to the list
# grouping an unbalanced `[` or `]` causes. Markdown is full of brackets, every
# link being one, and #518 is a fresh record of what a bracket-vulnerable reader
# costs.
#
# Not neutralising the brackets either: the fragments matched here contain `'`,
# `{}` and brackets in ordinary use, and blanking brackets is exactly what broke
# `check-tsan-scope` when that remedy was applied where the brackets were data.
function(fastcached_read_whole relative outContent outFound)
    set(path "${FASTCACHED_SOURCE_DIR}/${relative}")
    if(NOT EXISTS "${path}")
        set(${outFound} FALSE PARENT_SCOPE)
        set(${outContent} "" PARENT_SCOPE)
        return()
    endif()
    file(READ "${path}" content)
    set(${outContent} "${content}" PARENT_SCOPE)
    set(${outFound} TRUE PARENT_SCOPE)
endfunction()

set(problems "")

fastcached_read_whole("${FastCachedCorruptStorePage}" pageContent pageFound)
if(NOT pageFound)
    message(FATAL_ERROR
        "check-corrupt-store-diagnostics: ${FastCachedCorruptStorePage} does not exist. This check "
        "reads the page and the sources that compose what it quotes. Without the page there is "
        "nothing to check, and reporting success would be the vacuous pass it exists to prevent.")
endif()

# ---------------------------------------------------------------------------
# Extract the diagnostics the page quotes, walking it WITHOUT building a list.
#
# Derived from the page rather than restated, so a diagnostic added later is
# picked up -- and, if no row covers it, refused.
set(quoted "")
set(quotedCount 0)
string(REPLACE "\r\n" "\n" pageWalk "${pageContent}")
while(NOT pageWalk STREQUAL "")
    string(FIND "${pageWalk}" "\n" newlineAt)
    if(newlineAt EQUAL -1)
        set(line "${pageWalk}")
        set(pageWalk "")
    else()
        string(SUBSTRING "${pageWalk}" 0 ${newlineAt} line)
        math(EXPR newlineAt "${newlineAt} + 1")
        string(SUBSTRING "${pageWalk}" ${newlineAt} -1 pageWalk)
    endif()

    set(isDiagnostic FALSE)
    string(FIND "${line}" "StorageError(code=" foundAt)
    if(NOT foundAt EQUAL -1)
        set(isDiagnostic TRUE)
    endif()
    string(FIND "${line}" "failed to open " foundAt)
    if(NOT foundAt EQUAL -1)
        set(isDiagnostic TRUE)
    endif()
    if(isDiagnostic)
        list(APPEND quoted "${line}")
        math(EXPR quotedCount "${quotedCount} + 1")
    endif()
endwhile()

# Two empty lists agree perfectly. A page that yields no diagnostics has not been
# checked -- it has stopped being read.
if(quotedCount EQUAL 0)
    message(FATAL_ERROR
        "check-corrupt-store-diagnostics: found NO quoted diagnostic in "
        "${FastCachedCorruptStorePage}. Either the page stopped quoting console output -- in which "
        "case this check has nothing to enforce and should be removed deliberately -- or the "
        "extraction stopped matching. It is not treated as a clean pass.")
endif()

# ---------------------------------------------------------------------------
# Every quoted diagnostic must be covered by at least one fragment row, and every
# fragment it matches must still be composable by the source that owns it.
set(checkedFragments 0)
foreach(diagnostic IN LISTS quoted)
    set(covered FALSE)
    foreach(row IN LISTS FastCachedDiagnosticFragments)
        string(REPLACE "|" ";" fields "${row}")
        list(GET fields 0 fragment)
        list(GET fields 1 sourceFile)
        list(GET fields 2 literal)
        list(GET fields 3 reason)

        string(FIND "${diagnostic}" "${fragment}" fragmentAt)
        if(fragmentAt EQUAL -1)
            continue()
        endif()
        set(covered TRUE)
        math(EXPR checkedFragments "${checkedFragments} + 1")

        fastcached_read_whole("${sourceFile}" sourceContent sourceFound)
        if(NOT sourceFound)
            list(APPEND problems
                 "  ${sourceFile}\n      is named as composing `${fragment}` and does not exist")
            continue()
        endif()
        string(FIND "${sourceContent}" "${literal}" literalAt)
        if(literalAt EQUAL -1)
            list(APPEND problems
                 "  the page quotes `${fragment}`\n      but ${sourceFile} no longer contains `${literal}`\n      ${reason}")
        endif()
    endforeach()

    if(NOT covered)
        list(APPEND problems
             "  the page quotes a diagnostic no fragment row covers:\n      ${diagnostic}\n      Add a row naming the source that composes it, or this check vouches for a quote it never read.")
    endif()
endforeach()

# ---------------------------------------------------------------------------
# The `what` arguments, which are literals rather than format strings.
foreach(row IN LISTS FastCachedDaemonWhatLiterals)
    string(REPLACE "|" ";" fields "${row}")
    list(GET fields 0 fragment)
    list(GET fields 1 literal)
    list(GET fields 2 reason)

    set(pageHasIt FALSE)
    foreach(diagnostic IN LISTS quoted)
        string(FIND "${diagnostic}" "${fragment}" foundAt)
        if(NOT foundAt EQUAL -1)
            set(pageHasIt TRUE)
        endif()
    endforeach()
    if(NOT pageHasIt)
        continue()
    endif()

    fastcached_read_whole("src/apps/fastcached/main.cpp" daemonContent daemonFound)
    if(NOT daemonFound)
        list(APPEND problems "  src/apps/fastcached/main.cpp does not exist")
        continue()
    endif()
    string(FIND "${daemonContent}" "${literal}" literalAt)
    if(literalAt EQUAL -1)
        list(APPEND problems
             "  the page quotes `${fragment}`\n      but src/apps/fastcached/main.cpp no longer passes ${literal} as the `what`\n      ${reason}")
    endif()
    math(EXPR checkedFragments "${checkedFragments} + 1")
endforeach()

# ---------------------------------------------------------------------------
if(NOT problems STREQUAL "")
    list(JOIN problems "\n" report)
    message(FATAL_ERROR
        "The corrupt-store page quotes console output its source no longer composes:\n"
        "${report}\n\n"
        "An operator meeting `Corrupt` uses that page to decide NOT to delete a healthy cache, and "
        "that only works if they recognise their own console output in it. Fix the page to match "
        "what the code now prints, or restore what the page documents.\n"
        "The fragment table lives in ${CMAKE_CURRENT_LIST_FILE}.")
endif()

message(STATUS
        "corrupt-store diagnostics: ${quotedCount} quoted diagnostic(s) in "
        "${FastCachedCorruptStorePage}, ${checkedFragments} fragment(s) still composable by their source")
