# SPDX-License-Identifier: Apache-2.0
#
# Policies are pinned for the reason the check itself states.
cmake_minimum_required(VERSION 3.28)
#
# `corrupt-store-diagnostics` must be SEEN to refuse, on each thing it claims and
# on nothing else.
#
# The check is a docs-subject check: it reads `docs/operations/corrupt-store.md`
# and the sources that compose what the page quotes, and it exists because those
# diagnostics were asserted by nothing (#633). A guard nobody has watched refuse
# is not a guard, so every rule it carries gets an arm here.
#
# ## The two directions that matter
#
# `wording` is the false-positive guard: rewording the prose AROUND a quote must
# be free, or the check is the change-detector #633 explicitly warns against.
# `shardPath` and `suffix` are the true positives -- the field-level assertions
# that were unreachable by unit test, because the daemon's composer sits in an
# anonymous namespace behind `add_executable(fastcached main.cpp)` with no test
# target, and the node's suffix is composed in a third file again.
#
# ## INCONCLUSIVE, and why this harness is born with it
#
# [#747](https://github.com/LASTRADA-Software/fastcached/issues/747) records that
# this tree's two older selftest harnesses cannot tell *the rule fired* from *the
# invocation never ran*: one reads any non-zero status as a refusal, the other
# ignores `RESULT_VARIABLE` entirely. Under load -- a box at 82 across 32 CPUs,
# with the OOM killer taking builds at exit 137 -- a killed spawn then reports as
# a rule regression, and a lane spent nine runs hunting a phantom because of it.
#
# Fixing those two is #747 and is deliberately not done here. Writing a THIRD
# instance of a defect filed the same day would be indefensible, so this harness
# distinguishes the state from its first commit: a spawn that could not run is
# reported INCONCLUSIVE, by name, and never folded into pass or fail.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -DFASTCACHED_SCRATCH_DIR=<dir> \
#         -P scripts/check-corrupt-store-diagnostics-selftest.cmake
#
# Exit codes: 0 always. The verdict is the presence of `CMake Error` in the output.

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()
if(NOT DEFINED FASTCACHED_SCRATCH_DIR)
    message(FATAL_ERROR "FASTCACHED_SCRATCH_DIR must be set")
endif()

set(check "${FASTCACHED_SOURCE_DIR}/scripts/check-corrupt-store-diagnostics.cmake")
if(NOT EXISTS "${check}")
    message(FATAL_ERROR "the check under test is missing: ${check}")
endif()

set(root "${FASTCACHED_SCRATCH_DIR}")
file(REMOVE_RECURSE "${root}")
set(failures "")
set(inconclusive "")
set(caseCount 0)

# ---------------------------------------------------------------------------
# The composing sources, as the real ones spell them. Each case overrides one.
set(realStorageError "inline std::string ToString() const\n{\n    return std::format(\"StorageError(code={} system={} context={})\", ToStringView(code), systemCode, context)\;\n}\n")
set(realCowTree "auto opened = FilePageStore::Open(options)\;\nif (!opened) return std::unexpected(TranslateError(store.error(), \"FilePageStore::Open\"))\;\n")
set(realDaemon "static std::string StorageOpenFailure(std::string_view what, std::string_view path)\n{\n    return std::format(\"failed to open {} '{}': {}{}\", what, path, error.ToString(), remedy)\;\n}\nreturn StorageOpenFailure(\"storage\", effective.storagePath)\;\nreturn StorageOpenFailure(\"shard\", path.string())\;\n")
set(realCacheTier "return std::unexpected { std::format(\"cannot open {}: {}\", options.path.string(), opened.error().ToString()) }\;\nreturn std::unexpected { std::format(\"--cache-dir {}\", storage.error()) }\;\n")
set(realNodeMain "logger.Logf(LogLevel::Error, \"{}\; refusing to start\", cacheTierOrRefusal.error())\;\n")

# The page, as the real one quotes it: two fenced diagnostics and one inline.
set(realPage "# Corrupt store\n\nSome prose about what to do.\n\n```\nfailed to open storage '/var/lib/fastcached/cache': StorageError(code=Corrupt system=0 context=FilePageStore::Open)\n```\n\n```\n--cache-dir cannot open /var/cache/fastcache-node/objects.cow: StorageError(code=Corrupt system=0 context=FilePageStore::Open)\; refusing to start\n```\n\nWith `--storage-shards` the message says `failed to open shard '<path>'` and\nnames the one file that is damaged.\n")

# $1 = case name, then five source overrides and the page. Empty means "as real".
function(fastcached_make_tree name pageText storageError cowTree daemon cacheTier nodeMain outVar)
    set(tree "${root}/${name}")
    file(REMOVE_RECURSE "${tree}")
    file(MAKE_DIRECTORY "${tree}/docs/operations")
    file(MAKE_DIRECTORY "${tree}/src/FastCache/Core/Errors")
    file(MAKE_DIRECTORY "${tree}/src/FastCache/Cache")
    file(MAKE_DIRECTORY "${tree}/src/apps/fastcached")
    file(MAKE_DIRECTORY "${tree}/src/apps/fastcache-compile-node")
    file(WRITE "${tree}/docs/operations/corrupt-store.md" "${pageText}")
    file(WRITE "${tree}/src/FastCache/Core/Errors/StorageError.hpp" "${storageError}")
    file(WRITE "${tree}/src/FastCache/Cache/CowTreeStorage.cpp" "${cowTree}")
    file(WRITE "${tree}/src/apps/fastcached/main.cpp" "${daemon}")
    file(WRITE "${tree}/src/apps/fastcache-compile-node/CacheTier.cpp" "${cacheTier}")
    file(WRITE "${tree}/src/apps/fastcache-compile-node/main.cpp" "${nodeMain}")
    set(${outVar} "${tree}" PARENT_SCOPE)
endfunction()

# Run the check and classify into THREE states, never two.
#
# A non-zero result with NO output is a spawn that did not run -- the OOM killer,
# a missing interpreter, a `noexec` mount. That is INCONCLUSIVE and is reported as
# itself: folding it into `objected` would make a killed process read as the rule
# firing, which is #747 exactly.
function(fastcached_run_check tree outVerdict outOutput)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DFASTCACHED_SOURCE_DIR=${tree}" -P "${check}"
        OUTPUT_VARIABLE captured ERROR_VARIABLE capturedErrors RESULT_VARIABLE result)
    set(combined "${captured}${capturedErrors}")
    set(${outOutput} "${combined}" PARENT_SCOPE)

    string(FIND "${combined}" "CMake Error" errorAt)
    if(NOT errorAt EQUAL -1)
        set(${outVerdict} "refused" PARENT_SCOPE)
        return()
    endif()
    if(combined STREQUAL "" AND NOT result EQUAL 0)
        set(${outVerdict} "inconclusive" PARENT_SCOPE)
        return()
    endif()
    set(${outVerdict} "passed" PARENT_SCOPE)
endfunction()

# $1 = description, $2 = want-pass|want-refuse, $3 = tree, $4 = substring the
# refusal must contain (optional).
macro(fastcached_case what want tree expect)
    math(EXPR caseCount "${caseCount} + 1")
    fastcached_run_check("${tree}" verdict output)
    if(verdict STREQUAL "inconclusive")
        list(APPEND inconclusive "  ${what}")
    elseif(want STREQUAL "want-pass" AND NOT verdict STREQUAL "passed")
        list(APPEND failures "  ${what}: expected a pass, got ${verdict}")
    elseif(want STREQUAL "want-refuse" AND NOT verdict STREQUAL "refused")
        list(APPEND failures "  ${what}: expected a refusal, got ${verdict}")
    elseif(NOT "${expect}" STREQUAL "")
        string(FIND "${output}" "${expect}" expectAt)
        if(expectAt EQUAL -1)
            list(APPEND failures "  ${what}: the verdict never mentioned `${expect}`, so it fired for some other reason")
        endif()
    endif()
endmacro()

# ---------------------------------------------------------------------------
# 1. The baseline. Every case below is evidence only if this passes.
fastcached_make_tree("baseline" "${realPage}" "${realStorageError}" "${realCowTree}"
                     "${realDaemon}" "${realCacheTier}" "${realNodeMain}" tree)
fastcached_case("the shipped shape passes" "want-pass" "${tree}" "")

# 2. Rewording the prose AROUND a quote is free. This is the false-positive
#    guard, and without it the check is the change-detector #633 warns against.
string(REPLACE "Some prose about what to do." "Entirely different prose, rewritten." rewordedPage "${realPage}")
fastcached_make_tree("wording" "${rewordedPage}" "${realStorageError}" "${realCowTree}"
                     "${realDaemon}" "${realCacheTier}" "${realNodeMain}" tree)
fastcached_case("rewording the prose around a quote is not a violation" "want-pass" "${tree}" "")

# 3. The shard path dropped from the daemon's composer. #633's claim (2), and the
#    field-level assertion: the `'{}'` is what carries the damaged file's name.
string(REPLACE "failed to open {} '{}'" "failed to open {}" strippedDaemon "${realDaemon}")
fastcached_make_tree("shardPath" "${realPage}" "${realStorageError}" "${realCowTree}"
                     "${strippedDaemon}" "${realCacheTier}" "${realNodeMain}" tree)
fastcached_case("dropping the quoted path from the daemon's composer is refused" "want-refuse" "${tree}" "failed to open")

# 4. The `shard` argument gone. Same composer, the variant that names one file.
string(REPLACE "\"shard\"" "\"chunk\"" renamedWhat "${realDaemon}")
fastcached_make_tree("shardWhat" "${realPage}" "${realStorageError}" "${realCowTree}"
                     "${renamedWhat}" "${realCacheTier}" "${realNodeMain}" tree)
fastcached_case("renaming the `shard` what-argument is refused" "want-refuse" "${tree}" "shard")

# 5. The node's suffix, composed in a THIRD file. A check reading only the tier
#    would miss this entirely.
string(REPLACE "refusing to start" "giving up" strippedSuffix "${realNodeMain}")
fastcached_make_tree("suffix" "${realPage}" "${realStorageError}" "${realCowTree}"
                     "${realDaemon}" "${realCacheTier}" "${strippedSuffix}" tree)
fastcached_case("losing the node's `refusing to start` suffix is refused" "want-refuse" "${tree}" "refusing to start")

# 6. The context token renamed. The one an operator matches against.
string(REPLACE "\"FilePageStore::Open\"" "\"PageStore::Open\"" renamedContext "${realCowTree}")
fastcached_make_tree("context" "${realPage}" "${realStorageError}" "${renamedContext}"
                     "${realDaemon}" "${realCacheTier}" "${realNodeMain}" tree)
fastcached_case("renaming the context token is refused" "want-refuse" "${tree}" "FilePageStore::Open")

# 7. The rendering itself changed. Here the WHOLE format string is the assertion,
#    deliberately -- the rendering IS the page's verbatim quote.
string(REPLACE "StorageError(code={} system={} context={})" "StorageError(code={} ctx={})" renamedRendering "${realStorageError}")
fastcached_make_tree("rendering" "${realPage}" "${renamedRendering}" "${realCowTree}"
                     "${realDaemon}" "${realCacheTier}" "${realNodeMain}" tree)
fastcached_case("changing the StorageError rendering is refused" "want-refuse" "${tree}" "StorageError(code=")

# 8. A diagnostic the fragment table does not cover. An uncovered quote is the
#    silent gap this check exists to close, so it is refused rather than skipped.
string(REPLACE "Some prose about what to do."
       "```\nfailed to open the widget registry: WidgetError(code=Missing)\n```" extraPage "${realPage}")
fastcached_make_tree("uncovered" "${extraPage}" "${realStorageError}" "${realCowTree}"
                     "${realDaemon}" "${realCacheTier}" "${realNodeMain}" tree)
fastcached_case("a quoted diagnostic no row covers is refused" "want-refuse" "${tree}" "no fragment row covers")

# 9. A page quoting nothing. Two empty lists agree perfectly.
fastcached_make_tree("vacuous" "# Corrupt store\n\nJust prose now.\n" "${realStorageError}" "${realCowTree}"
                     "${realDaemon}" "${realCacheTier}" "${realNodeMain}" tree)
fastcached_case("a page that quotes no diagnostic at all is refused" "want-refuse" "${tree}" "NO quoted diagnostic")

# ---------------------------------------------------------------------------
# INCONCLUSIVE is reported as itself and never as a pass. A run that could not
# execute an arm has not tested that arm, and saying so is the whole point of
# #747.
if(NOT inconclusive STREQUAL "")
    list(JOIN inconclusive "\n" inconclusiveReport)
    message(FATAL_ERROR
        "check-corrupt-store-diagnostics-selftest: ${caseCount} case(s) ran and the following could "
        "not be EXECUTED at all:\n${inconclusiveReport}\n\n"
        "That is not a rule regression. A spawn produced no output and a non-zero status, which is "
        "what an OOM kill, a missing interpreter or a `noexec` mount looks like. Re-run at a lower "
        "-j before looking at the check.")
endif()

if(NOT failures STREQUAL "")
    list(JOIN failures "\n" report)
    message(FATAL_ERROR
        "check-corrupt-store-diagnostics-selftest: ${caseCount} case(s) ran and these did not behave "
        "as claimed:\n${report}\n")
endif()

message(STATUS "check-corrupt-store-diagnostics-selftest: ${caseCount} case(s), each seen to behave as claimed")
