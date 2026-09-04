#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# The gate .agent/rules/build-and-toolchain.md asks for, as a script rather than
# as a paragraph.
#
# "Build at least one release configuration and one non-clang compiler locally
# before pushing" is advice that has to be remembered, and this branch has twice
# paid a full CI cycle for forgetting it -- once for a GCC -O3 `-Wnull-dereference`
# through an inlined `memcpy`, which clang does not emit at any level, and once for
# a clang-tidy check the version on PATH had never heard of.
#
# Both are configurations a developer HAS and does not run. So this runs them.
#
# What it covers, and why each earns its minutes:
#
#   clang-format  at the version CI pins. Successive LLVM releases disagree about
#                 formatting, so a tree clean under whatever is on PATH can still be
#                 rejected -- for code nobody mis-wrote.
#   clang-debug   PEDANTIC + ASan + UBSan + clang-tidy, the ANALYSER pinned to the
#                 same version. The default agent preset, the only place sanitizers
#                 run at all, and the only preset here that tidies anything -- which
#                 is why the other one says so out loud rather than leaving a reader
#                 to infer that the gate's tidy coverage is both.
#   gcc-release   The second compiler, at -O3. A different warning set, and
#                 optimizer-dependent diagnostics that appear at no other level.
#
# And what it refuses to let anything else decide: both configurations are built
# with NO compiler-cache launcher. `-DUSE_COMPILER_CACHE=OFF`, checked afterwards
# against the generated build rather than assumed from the flag.
#
# That is not this script's own rule. `scripts/launcher-replay-e2e.sh` calls it
# "the standing -DUSE_COMPILER_CACHE=OFF rule" and records what it is for: in
# #319 a cache-backed build of a test binary segfaulted while the same commit
# built cache-off passed, and nothing in CI could have reported it.
# `CMakePresets.json` carries the same value on `clang-coverage` for an
# independent reason. So the project has taken this decision twice and written it
# down twice -- and until now the gate was the only reference build in the tree
# that dissented, silently, because `USE_COMPILER_CACHE` defaults to ON and this
# script never mentioned it. Measured before the fix: 148 `LAUNCHER = ` lines in
# `clang-debug` and 618 in `gcc-release`, every one of them pointing at whichever
# launcher happened to be installed, at whatever version, with no check and no
# mention (issue #471).
#
# The gate's verdict is a claim about a SOURCE TREE. A pinned analyser and a
# pinned formatter are pinned because their version changes that verdict and
# there is a canonical version to pin to; a compiler cache has neither, and is
# supposed to be verdict-NEUTRAL. When it is not, it substitutes an object the
# tree did not produce. So the same argument that pins the other two tools
# removes this one, rather than versioning it.
#
# What it deliberately does NOT cover: MSVC and clang-cl, which need Windows, and
# macOS/libc++, which needs a Mac. Those stay CI's job, and the point of this script
# is that everything reproducible locally is reproduced locally.
#
# Usage:  scripts/local-gate.sh [--no-format] [--self-test]
#
#   --no-format  skip the clang-format pass. It does NOT loosen the clang-tidy pin:
#                those are two tools and the flag names one of them.
#   --self-test  check the configure decision against synthetic CMake caches, and
#                the launcher refusal against synthetic `build.ninja` files, then
#                exit. Needs no compiler, no cmake and no clang-tidy, which is what
#                lets `ctest -R local-gate-selftest` run it everywhere.
#
# Exits non-zero on the first configuration that fails, having printed its errors.
# It never runs ctest against a build that did not complete -- a stale binary
# reporting a green suite is the failure mode this ordering exists to prevent, and
# it has happened here.
#
# Because it stops at the first red leg, EVERY run -- red or green -- ends with a
# per-leg verdict naming what passed, what failed, and what never started. Stopping
# early is right; saying nothing about the legs it skipped was not, and
#
#     GATE FAILED: gate-clang-debug tests
#
# read as "the rest passed" when the rest had not been asked. What that cost, and
# why the renderer below is a pure function, is in
# `.agent/rules/build-and-toolchain.md` (#501) -- one place, so a citation cannot
# drop the conditions the measurement came with.
#
# Reading a red run of THIS script: the leg block is the answer to "how much of the
# gate actually ran". For the failure itself, read the raw log the message names --
# ninja's diagnostic begins with a `FAILED:` line and puts the error text several
# lines BELOW it, so a wrapper filtering on `error|warning:` alone can surface an
# unrelated warning and none of the actual failure. Filter on `FAILED:` as well, or
# not at all.
#
# **bash 3.2**, because `--self-test` is in the default ctest set and macOS ships a
# 2007 `/bin/bash`. No `mapfile`, no `declare -A`, no `local -n` -- and no bare
# `"${arr[@]}"` on an array that can be EMPTY, which is an unbound-variable error
# under `set -u` before 4.4. Every array below is non-empty by construction.

set -uo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root" || exit 1

format=1
self_test=0
for arg in "$@"; do
    case "$arg" in
        --no-format) format=0 ;;
        --self-test) self_test=1 ;;
        *) echo "usage: $0 [--no-format] [--self-test]" >&2; exit 2 ;;
    esac
done

# The version CI pins, named rather than taken from PATH. A machine carrying both
# 20 and 22 resolves the bare name to whichever comes first, and the preset's own
# `CMAKE_CXX_CLANG_TIDY=clang-tidy` inherits that -- so a "clang-tidy clean" build
# can mean nothing, with the version it used printed nowhere.
#
# That paragraph described THIS SCRIPT until the pin below existed. It named the
# version for clang-format and then handed the analyser to PATH order: the argument
# was written down and not carried one call further, so the gate's own comment
# documented the defect it had. The pin now reaches the configure, and the run
# prints which binary it used.
tools_version="${CLANG_TOOLS_VERSION:-22}"

# Render one line per configuration: what it did, including having done nothing.
#
# PURE -- everything it reports arrives as arguments. That split is the point rather
# than tidiness: the bug this fixes is a REPORTING bug, and a reporting bug whose
# report cannot be exercised without building two whole configurations is the same
# bug one level up. `--self-test` drives every verdict here in milliseconds.
#
# The three states are spelled so that no two can be mistaken for each other, which
# is the entire acceptance criterion: "failed" and "never ran" must not read alike,
# because a developer who confuses them pushes. An unrecognised state renders as
# unrecognised rather than as the nearest plausible neighbour -- a fourth state
# arriving here silently as `passed` would recreate the defect exactly.
#
# @param ... One `preset=state` pair per leg, in table order.
leg_summary() {
    local pair preset state label
    echo "== gate legs:"
    # `${1+"$@"}` rather than `"$@"`: before bash 4.4 an empty `$@` is an unbound
    # expansion under `set -u`, which is the hazard this file's header names. No
    # caller passes none today, but this function is documented as argument-driven,
    # so the next one would meet the reporter dying while reporting.
    for pair in ${1+"$@"}; do
        preset="${pair%%=*}"
        state="${pair#*=}"
        case "$state" in
            passed)  label="passed" ;;
            failed)  label="FAILED" ;;
            not-run) label="NOT RUN -- the gate stopped before this leg, so it has reported NOTHING" ;;
            *)       label="UNKNOWN STATE '$state' -- this is a bug in the gate, not a verdict" ;;
        esac
        # 18, not 14: the gate presets are `gate-clang-debug` and
        # `gate-gcc-release` (#487), which are 16 characters and ran into the
        # verdict column. Sized from the longest name this table can hold rather
        # than from the longest one it holds today.
        printf '==   %-18s %s\n' "$preset" "$label"
    done
}

# What each preset is here for, and whether the analyser pin has to reach it.
#
# A table rather than two calls with a flag threaded through them, because the
# second column is a fact about `CMakePresets.json` -- `clang-debug` sets
# `ENABLE_TIDY=ON` and `gcc-release` does not -- and a reader asking "does this gate
# tidy both configurations?" should find the answer written down rather than infer
# it from the absence of an argument. The answer is no, and a run says so.
#
#   preset|tidy      the preset runs clang-tidy, so the pin must reach it
#   preset|no-tidy   it does not, and the run prints that rather than staying silent
# The `gate-` presets, not the developer's. They inherit the real ones entry for
# entry and differ in exactly two things: their own `binaryDir` (derived from the
# preset name by `base`), and `USE_COMPILER_CACHE=OFF` as a cache variable rather
# than a `-D` this script passes. That is #487: a `-D` writes a cache entry,
# `option()` never overrides one, and the gate was configuring the very
# directories AGENT.md tells developers to build in -- so one gate run left every
# ordinary build in the tree uncached, permanently, in the repository whose
# product is a compile cache.
#
# NOT hidden, and they cannot be: `"hidden": true` makes `--preset` refuse a preset
# outright, and this script selects them by name. They are undocumented rather than
# hidden -- their displayName says whose directory they are and whose they are not,
# which is the most a visible preset can do.
gate_presets=(
    "gate-clang-debug|tidy"
    "gate-gcc-release|no-tidy"
)

# Every row must name a GATE-OWNED preset, checked rather than trusted. Reverting
# the two rows above fails the self-test, which compares literals -- but ADDING a
# third row naming an ordinary preset reopens #487 in full and passes every check
# in this file. That is the direction a literal comparison is blind to: exact about
# what it knows, silent about what arrives.
for _gate_row in "${gate_presets[@]}"; do
    case "${_gate_row%%|*}" in
        gate-*) ;;
        *)
            echo "GATE BUG: gate_presets row '${_gate_row}' does not name a gate-owned preset." >&2
            echo "  The gate must not configure a directory a developer builds in: a reference" >&2
            echo "  build turns the compiler cache off, and that setting is permanent for the" >&2
            echo "  directory. See issue #487." >&2
            exit 1
            ;;
    esac
done
unset _gate_row

# What each leg has done so far, by index into the table above. Declared empty and
# never pre-filled: `leg_pairs` reads an absent entry as `not-run`, which is the
# truth for every leg until the loop reaches it and is what a failure BEFORE the loop
# -- a missing analyser, a formatter that refuses -- has to report. Filling it in
# advance would only have created a second list to keep in step with the first.
leg_states=()

# Every leg's state, as `preset=state` pairs in table order, for `leg_summary`.
#
# Split from the renderer so the renderer stays pure. `:-not-run` is what makes the
# absence of an entry mean something rather than being an unbound-variable death
# under `set -u`. Unquoted at the call site on purpose: these are CMake preset
# names, which cannot contain whitespace.
leg_pairs() {
    local i=0 row
    for row in "${gate_presets[@]}"; do
        echo "${row%%|*}=${leg_states[$i]:-not-run}"
        i=$((i + 1))
    done
}

# Run every leg in table order, marking each one as it goes.
#
# `failed` BEFORE the leg runs and `passed` after it, so that any `fail` reached from
# inside the runner -- configure, launcher refusal, build, ctest -- renders this leg
# as failed and every later one as never started, without each of those call sites
# having to know it is being reported on.
#
# Takes the runner by NAME so `--self-test` can drive this loop with a stub. That is
# not indirection for its own sake: the bookkeeping here is the half of #501 that
# could plausibly be wrong, and until it took an argument the only way to exercise it
# was to build two whole configurations.
#
# @param 1 Name of the function to invoke per leg, as `runner <preset> <analyser>`.
run_all_legs() {
    local runner="$1" i=0 row status
    for row in "${gate_presets[@]}"; do
        leg_states[$i]="failed"
        # The status is CHECKED, not assumed. There is no `set -e` here, so an
        # unchecked call would mark the leg `passed` on any return at all -- and the
        # whole scheme would then rest on every failure path in the runner calling
        # `fail` and exiting, which is true today and is exactly the kind of thing a
        # later edit breaks by doing the idiomatic thing instead. A runner that
        # `return 1`s would otherwise be reported as a passed leg, the next leg would
        # run, and the gate would end `LOCAL GATE PASSED`: #501 reproduced one level
        # down, inside the fix for it.
        status=0
        "$runner" "${row%%|*}" "${row#*|}" || status=$?
        if [[ "$status" -ne 0 ]]; then
            fail "${row%%|*} returned $status rather than refusing"
        fi
        leg_states[$i]="passed"
        i=$((i + 1))
    done
}

# A refusal reports the legs as well as the reason. Before this, the reason WAS the
# whole report, and the reader supplied the rest from optimism.
#
# Below both tables rather than above them, because it READS them: `leg_pairs`
# expands `gate_presets`, which is unset until line ~190.
#
# Note what that does NOT buy, since the obvious reading is wrong: it is not a guard
# on the argument loop further up. A `fail` added there -- the natural home for
# "unknown flag" -- would run before this definition exists, so bash would report
# `fail: command not found`, return 127, and with no `set -e` the script would CARRY
# ON and exit 0. That fails open. Anything up there refuses with `echo >&2; exit`,
# as the usage arm already does.
fail() {
    echo "GATE FAILED: $*" >&2
    leg_summary $(leg_pairs) >&2
    exit 1
}

# The value CMake actually cached for one entry of a build directory, or empty
# when there is no such entry.
#
# Two entries are read through this, and both are read for the same reason: a
# cached value outlives every reason it was chosen, so what a directory HOLDS and
# what this run would ASK for are different questions.
#
#   CLANG_TIDY_EXE      and not `CMAKE_CXX_CLANG_TIDY`, because that is the entry
#                       `cmake/portable/ClangTidy.cmake`'s `find_program` fills --
#                       and `find_program` never revisits a filled cache entry.
#                       `CLANG_TIDY_EXE-NOTFOUND` is a value like any other here
#                       and compares unequal, which is the point: a directory
#                       configured on a machine that had no clang-tidy must not be
#                       accepted as one that has the right clang-tidy. Same shape
#                       as the stale `FASTCACHE_CC-NOTFOUND` which kept whole build
#                       trees on sccache without ever saying so.
#   USE_COMPILER_CACHE  whether a compiler-cache launcher fronts the compiler. Its
#                       default is ON, so a directory that has never been told
#                       otherwise holds ON -- and a directory configured before
#                       this check existed holds it too.
#
# A parameter rather than a second copy of the function: the two differ only in
# which name they look for, which is the definition of a value that belongs in an
# argument.
#
# One `awk` and no pipe. `sed ... | head -1` would be the obvious spelling and is
# the `producer | grep -q` trap in another costume: `head` exits at the first line,
# the producer dies of SIGPIPE, and `pipefail` reports the producer's status on the
# SUCCESS path.
# @param 1 Path to a CMakeCache.txt.
# @param 2 Cache entry name, without its `:TYPE` suffix.
cached_entry() {
    [[ -f "$1" ]] || return 0
    awk -v name="$2" 'index($0, name ":") == 1 { sub(/^[^=]*=/, ""); print; exit }' "$1"
}

# How many compile edges of a GENERATED build are fronted by a compiler-cache
# launcher: a count, or the word `unknown` when there is no build to read.
#
# A count and not a yes/no, because the number is what makes the refusal
# actionable, and not a `fronted <n>` string either -- that would be a wire format
# between two functions in one file, glued on by the producer and taken apart by
# the consumer, with only the producing end self-tested.
#
# This is the guard, and it is separate from the configure decision above because
# it answers a different question. Passing `-DUSE_COMPILER_CACHE=OFF` is not the
# same fact as no launcher being in effect: `cmake/portable/CompileCache.cmake`
# returns early when `CMAKE_CXX_COMPILER_LAUNCHER` was set externally -- by a
# preset, a toolchain file, or an older `-D` -- and leaves it untouched. A gate
# that only passed the flag would have ASKED; this one CHECKS.
#
# `LAUNCHER = ` in `build.ninja` and not a cache entry, because the cache cannot
# answer it: `CompileCache.cmake` sets `CMAKE_CXX_COMPILER_LAUNCHER` as an ordinary
# directory-scope variable and never as a cache entry. Reading it back out of
# `CMakeCache.txt` was this fix's first design and would have reported "no
# launcher" against both live gate directories on the machine this was written on,
# which carried 148 and 618 launcher-fronted edges at the time -- a guard that
# cannot fire, inside the fix for a ticket about guards that cannot fire. The
# generated build is the fact; the flag is only the intent. It is also already this
# project's idiom for this exact question: `scripts/launcher-replay-e2e.sh` checks
# the same string from the other side, to prove a launcher IS in use.
#
# `unknown` is its own answer and not folded into a count of zero, because a
# missing `build.ninja` is a state where the question cannot be answered rather
# than one where the answer is good, and a gate that cannot check must not report.
# Zero is a reading; `unknown` is the absence of one.
#
# The whole file is scanned rather than stopped at the first match, and that costs
# nothing worth saving: measured at 22.4ms against a 19.7ms bare `awk` spawn on a
# 1.3MB `build.ninja`, so ~2.7ms is the scan. Stopping early cannot help the only
# path a passing gate takes anyway -- answering zero means reading to EOF.
# `unreadable` is the fourth answer and is not optional. `awk` on a file it cannot
# open exits WITHOUT running its `END` block, so it prints nothing -- and an empty
# string falling through to the caller's default arm would render a failed READING
# as the worst positive one, refusing with "(launcher-fronted edges: )" and blaming
# an external launcher nobody set. Skipped, absent, unstarted and failed are four
# states; a `[[ -f ]]` that passes for a file whose permissions deny it is exactly
# where the fourth hides.
#
# The match is deliberately NOT anchored to ninja's two-space indent. Anchoring
# would fail OPEN if that spelling ever changed -- a count of zero reads as a clean
# build -- and this is a gate, so the loose match is the safe direction: it can
# only over-count. `CMAKE_<LANG>_LINKER_LAUNCHER` emits the same binding, which is
# why the number is reported as launcher-fronted EDGES rather than as compile
# edges; a linker cache in a reference build is a thing to refuse too.
# @param 1 Path to a generated build.ninja.
launcher_verdict() {
    if [[ ! -f "$1" ]]; then
        echo "unknown"
        return 0
    fi
    local n
    # stderr discarded because the failure is CONVERTED into a named state below:
    # `awk` writes "cannot open file ... Permission denied" and the classification
    # is what reports that, so the raw line would be noise in a ctest log rather
    # than information. Nothing is being hidden -- an unreadable file still fails
    # the gate, by name.
    n="$(awk 'index($0, "LAUNCHER = ") { n++ } END { print n+0 }' "$1" 2>/dev/null)"
    if [[ "$n" =~ ^[0-9]+$ ]]; then
        echo "$n"
    else
        echo "unreadable"
    fi
}

# Why this preset has to be configured, or empty when it does not.
#
# Split out because it is exactly what `--self-test` can check without a toolchain,
# and because getting it wrong is silent in the direction that matters: a gate that
# skips the re-configure keeps the wrong analyser and reports clean.
# @param 1 Path to the build directory.
# @param 2 `tidy` or `no-tidy`, from the table above.
# @param 3 Absolute path of the pinned analyser.
configure_reason() {
    local dir="$1" analyser="$2" wanted="$3"
    local reasons=""

    # `cmake --build --preset` on a directory that does not exist fails with
    # "<path> is not a directory", which names neither the preset nor the fix and is
    # what a FRESH CHECKOUT gets from the one script everybody is told to run.
    if [[ ! -f "$dir/CMakeCache.txt" ]]; then
        echo "no build directory yet"
        return 0
    fi

    # Otherwise only when something the gate pins is wrong: a re-configure costs
    # over a minute every run to do nothing. But "the cache file exists" was the
    # WHOLE test until recently, and that is precisely what let a build directory
    # keep the analyser it first found forever -- re-running the gate could not fix
    # it, because re-running the gate is what skipped the configure.
    if [[ "$analyser" == "tidy" ]]; then
        local have
        have="$(cached_entry "$dir/CMakeCache.txt" CLANG_TIDY_EXE)"
        if [[ "$have" != "$wanted" ]]; then
            reasons="cached clang-tidy is ${have:-absent}, not $wanted"
        fi
    fi

    # Asked of EVERY preset, unlike the analyser: a compiler cache fronts whichever
    # configuration it is configured into, and `gcc-release` was the more thoroughly
    # fronted of the two. An absent entry is a directory configured before this
    # check existed, and `USE_COMPILER_CACHE` defaults to ON, so absent is reported
    # rather than tolerated -- the same reading `CLANG_TIDY_EXE-NOTFOUND` gets above.
    #
    # `${reasons:+...}` rather than an if/else, so the sentence is written once: two
    # arms differing only by a separator are two places that have to stay identical,
    # and a third pinned entry would be a third pair.
    local caching
    caching="$(cached_entry "$dir/CMakeCache.txt" USE_COMPILER_CACHE)"
    if [[ "$caching" != "OFF" ]]; then
        reasons="${reasons:+$reasons; }compiler caching is ${caching:-absent}, not OFF"
    fi

    # Whatever the refusal will read is also a reason to configure, or the gate
    # cannot repair the state it refuses -- and CMake makes that state reachable:
    # `-D` values are entered into `CMakeCache.txt` and the file is written even
    # when the configure then FAILS, while the previously generated `build.ninja`
    # stays exactly as it was. So a first gate run can leave a directory whose cache
    # reads `OFF` with the right analyser, beside a launcher-fronted build. Judging
    # only the cache, every later run would find nothing to configure, refuse on the
    # stale build, and say "something set CMAKE_CXX_COMPILER_LAUNCHER externally" --
    # which would be false, and which re-running could never fix, because
    # re-running is what skips the configure. That is the precise shape the analyser
    # clause above exists to remove, and it must not be reopened one file over.
    #
    # Through `launcher_verdict`, so the decision and the refusal read the same
    # observable rather than two that can disagree. What survives a configure that
    # actually RAN is the only state the refusal's message is true about.
    #
    # `unreadable` is deliberately NOT a reason: a configure cannot repair a file
    # this process may not read, and the refusal names that state itself. Only the
    # two states a configure can actually fix are reasons to run one.
    local fronted
    fronted="$(launcher_verdict "$dir/build.ninja")"
    case "$fronted" in
        0|unreadable) ;;
        unknown) reasons="${reasons:+$reasons; }there is no build.ninja to check" ;;
        *)       reasons="${reasons:+$reasons; }the generated build is launcher-fronted (edges: $fronted)" ;;
    esac

    # Several clauses can hold at once, and a run that re-configures for more than
    # one reason should say all of them: reporting only the first would leave a
    # reader believing the others were already right.
    #
    # Unguarded: every caller takes this through `$( )`, which strips the trailing
    # newline, so an empty `echo` and printing nothing are the same value.
    echo "$reasons"
}

if [[ "$self_test" -eq 1 ]]; then
    scratch="$(mktemp -d)"
    trap 'rm -rf "$scratch"' EXIT
    self_test_failures=0
    # Skipped is its own outcome, and it has to be VISIBLE or it reads as tested.
    # `ctest` shows a passing test's output to nobody, so a skip announced only on
    # stderr is a case that silently did not run -- which is the exact collapse this
    # file spends its length refusing. It rides the final PASSED line instead,
    # beside the interpreter, for the reason the interpreter is named there.
    self_test_skipped=""

    # @param 1 What is being checked. @param 2 Expected. @param 3 Actual.
    expect() {
        if [[ "$2" != "$3" ]]; then
            echo "SELF-TEST FAILED: $1: expected '$2', got '$3'" >&2
            self_test_failures=$((self_test_failures + 1))
        fi
    }

    # A fixture is a build DIRECTORY, not a lone cache file, because the decision
    # under test reads two files out of one directory and the cheaper stand-in
    # could not express "the cache is fine and the generated build is missing".
    #
    # @param 1 Directory name under $scratch.
    # @param 2 CMakeCache.txt content.
    # @param 3 `ninja` for a clean generated build, `fronted` for a
    #          launcher-fronted one, `none` for a cache with no build beside it.
    fixture() {
        mkdir -p "$scratch/$1"
        printf '%b' "$2" > "$scratch/$1/CMakeCache.txt"
        case "$3" in
            ninja)
                printf 'build x.o: CXX_COMPILER__foo x.cpp\n  DEP_FILE = x.o.d\n' \
                    > "$scratch/$1/build.ninja" ;;
            fronted)
                printf 'build x.o: CXX_COMPILER__foo x.cpp\n  LAUNCHER = /usr/bin/cmake -E env /usr/bin/fastcache-cc \n' \
                    > "$scratch/$1/build.ninja" ;;
            none) ;;
            # A mistyped or newly-invented mode would otherwise produce a directory
            # with no build.ninja -- silently the `none` fixture rather than the one
            # the caller named, and every expectation written against it would be
            # testing something else. A generator that produced nothing fails.
            *) echo "SELF-TEST BROKEN: fixture '$1' asked for unknown mode '$3'" >&2
               exit 1 ;;
        esac
    }

    # Every fixture states BOTH pinned entries, because a cache is only a useful
    # stand-in for a build directory if it is complete: one missing
    # `USE_COMPILER_CACHE` would make the caching clause fire in the cases written
    # to isolate the analyser one, and the two would stop being separable.
    fixture right     'CLANG_TIDY_EXE:FILEPATH=/usr/bin/clang-tidy-22\nUSE_COMPILER_CACHE:BOOL=OFF\n'      ninja
    fixture wrong     'CLANG_TIDY_EXE:FILEPATH=/usr/bin/clang-tidy-20\nUSE_COMPILER_CACHE:BOOL=OFF\n'      ninja
    fixture notfound  'CLANG_TIDY_EXE:FILEPATH=CLANG_TIDY_EXE-NOTFOUND\nUSE_COMPILER_CACHE:BOOL=OFF\n'     ninja
    fixture absent    'CMAKE_BUILD_TYPE:STRING=Debug\nUSE_COMPILER_CACHE:BOOL=OFF\n'                       ninja

    # The compiler-cache side. `caching-on` is what every gate directory on a
    # developer's machine looks like today, since the option defaults to ON;
    # `caching-unset` is one configured before the gate asked at all.
    fixture caching-on     'CLANG_TIDY_EXE:FILEPATH=/usr/bin/clang-tidy-22\nUSE_COMPILER_CACHE:BOOL=ON\n'  ninja
    fixture caching-unset  'CLANG_TIDY_EXE:FILEPATH=/usr/bin/clang-tidy-22\n'                              ninja
    fixture both-wrong     'CLANG_TIDY_EXE:FILEPATH=/usr/bin/clang-tidy-20\nUSE_COMPILER_CACHE:BOOL=ON\n'  ninja

    # Everything the gate pins is right, and the generated build is either missing
    # or stale and launcher-fronted. Both are what a FAILED configure leaves behind:
    # CMake writes the cache with the `-D` values and then does not regenerate.
    fixture no-ninja      'CLANG_TIDY_EXE:FILEPATH=/usr/bin/clang-tidy-22\nUSE_COMPILER_CACHE:BOOL=OFF\n'  none
    fixture stale-fronted 'CLANG_TIDY_EXE:FILEPATH=/usr/bin/clang-tidy-22\nUSE_COMPILER_CACHE:BOOL=OFF\n'  fronted

    expect "reads the cached analyser" \
        "/usr/bin/clang-tidy-20" "$(cached_entry "$scratch/wrong/CMakeCache.txt" CLANG_TIDY_EXE)"
    expect "reads nothing when the entry is absent" \
        "" "$(cached_entry "$scratch/absent/CMakeCache.txt" CLANG_TIDY_EXE)"
    expect "reads the NOTFOUND sentinel as a value rather than as absence" \
        "CLANG_TIDY_EXE-NOTFOUND" "$(cached_entry "$scratch/notfound/CMakeCache.txt" CLANG_TIDY_EXE)"
    expect "reads the compiler-cache entry through the same reader" \
        "ON" "$(cached_entry "$scratch/caching-on/CMakeCache.txt" USE_COMPILER_CACHE)"
    expect "reads nothing when the compiler-cache entry is absent" \
        "" "$(cached_entry "$scratch/caching-unset/CMakeCache.txt" USE_COMPILER_CACHE)"
    # An entry name is matched at the start of the line and up to its colon, so a
    # CMake `-ADVANCED` sibling is a different entry rather than a prefix match.
    printf 'USE_COMPILER_CACHE-ADVANCED:INTERNAL=1\nUSE_COMPILER_CACHE:BOOL=OFF\n' > "$scratch/advanced"
    expect "an -ADVANCED sibling is not mistaken for the entry" \
        "OFF" "$(cached_entry "$scratch/advanced" USE_COMPILER_CACHE)"

    expect "a missing build directory is configured" \
        "no build directory yet" \
        "$(configure_reason "$scratch/nope" tidy /usr/bin/clang-tidy-22)"
    expect "a directory already on the pinned analyser is left alone" \
        "" "$(configure_reason "$scratch/right" tidy /usr/bin/clang-tidy-22)"

    # The three that the old `if [[ ! -f CMakeCache.txt ]]` answered "leave it
    # alone", each of which is a gate reporting on an analyser nobody asked for.
    expect "a directory on the WRONG analyser is re-configured" \
        "cached clang-tidy is /usr/bin/clang-tidy-20, not /usr/bin/clang-tidy-22" \
        "$(configure_reason "$scratch/wrong" tidy /usr/bin/clang-tidy-22)"
    expect "a directory that never found one is re-configured" \
        "cached clang-tidy is CLANG_TIDY_EXE-NOTFOUND, not /usr/bin/clang-tidy-22" \
        "$(configure_reason "$scratch/notfound" tidy /usr/bin/clang-tidy-22)"
    expect "a directory with no analyser entry at all is re-configured" \
        "cached clang-tidy is absent, not /usr/bin/clang-tidy-22" \
        "$(configure_reason "$scratch/absent" tidy /usr/bin/clang-tidy-22)"

    # And the other direction, which costs minutes rather than correctness: a preset
    # that runs no analyser must never be re-configured over one, or `gcc-release`
    # rebuilds from scratch on every run of the gate chasing a tool it does not use.
    expect "a no-tidy preset ignores the analyser entirely" \
        "" "$(configure_reason "$scratch/wrong" no-tidy /usr/bin/clang-tidy-22)"

    # The compiler-cache clause. Unlike the analyser it is asked of every preset,
    # so the no-tidy row must still be re-configured over it -- `gcc-release` was
    # the more heavily fronted of the gate's two configurations (618 launcher edges
    # against 148) and a clause that skipped it would have left the worse half.
    expect "a directory with compiler caching ON is re-configured" \
        "compiler caching is ON, not OFF" \
        "$(configure_reason "$scratch/caching-on" tidy /usr/bin/clang-tidy-22)"
    expect "a directory that predates the check is re-configured" \
        "compiler caching is absent, not OFF" \
        "$(configure_reason "$scratch/caching-unset" tidy /usr/bin/clang-tidy-22)"
    expect "a no-tidy preset is re-configured over compiler caching too" \
        "compiler caching is ON, not OFF" \
        "$(configure_reason "$scratch/caching-on" no-tidy /usr/bin/clang-tidy-22)"
    expect "both pins wrong reports both, not the first" \
        "cached clang-tidy is /usr/bin/clang-tidy-20, not /usr/bin/clang-tidy-22; compiler caching is ON, not OFF" \
        "$(configure_reason "$scratch/both-wrong" tidy /usr/bin/clang-tidy-22)"

    # A directory the refusal cannot READ must be re-configured rather than left to
    # be refused forever. Everything this fixture pins is already correct, so
    # without the clause it would get no configure, then fail `unknown` on a
    # `build.ninja` nothing was ever going to generate -- and re-running the gate
    # could not repair it, which is the exact shape the analyser clause removed.
    expect "a cache with no generated build beside it is re-configured" \
        "there is no build.ninja to check" \
        "$(configure_reason "$scratch/no-ninja" tidy /usr/bin/clang-tidy-22)"
    expect "a correct cache beside a launcher-fronted build is re-configured" \
        "the generated build is launcher-fronted (edges: 1)" \
        "$(configure_reason "$scratch/stale-fronted" tidy /usr/bin/clang-tidy-22)"
    expect "a no-tidy preset is re-configured over a stale fronted build too" \
        "the generated build is launcher-fronted (edges: 1)" \
        "$(configure_reason "$scratch/stale-fronted" no-tidy /usr/bin/clang-tidy-22)"

    # The refusal. `-DUSE_COMPILER_CACHE=OFF` does not settle this on its own --
    # `CompileCache.cmake` returns early over an externally-set launcher -- so what
    # the gate refuses on is the generated build, and these are the three answers it
    # can get. The fronted fixture carries a real `LAUNCHER = ` line, verbatim from a
    # gate directory, rather than the bare word: what is being tested is that the
    # gate recognises what CMake actually emits, including the leading indent.
    printf 'build x.o: CXX_COMPILER__foo x.cpp\n  LAUNCHER = /usr/bin/cmake -E env FASTCACHE_ADDR=127.0.0.1:6674 /usr/bin/fastcache-cc \n  DEP_FILE = x.o.d\nbuild y.o: CXX_COMPILER__foo y.cpp\n  LAUNCHER = /usr/bin/cmake -E env /usr/bin/fastcache-cc \n' \
        > "$scratch/ninja-fronted"

    # Two edges rather than one, because a count is what the refusal reports and a
    # verdict that answered 1 for every fronted build would pass a single-edge test
    # while telling an operator nothing.
    expect "a launcher-fronted build is counted, not merely noticed" \
        "2" "$(launcher_verdict "$scratch/ninja-fronted")"
    expect "a build with no launcher edge reads zero" \
        "0" "$(launcher_verdict "$scratch/right/build.ninja")"
    expect "a build.ninja that is not there is unknown, never zero" \
        "unknown" "$(launcher_verdict "$scratch/no-ninja/build.ninja")"

    # The fourth state. `awk` on a file it cannot open exits without running `END`,
    # so it prints NOTHING -- and an empty answer reaching the caller's default arm
    # would refuse a build nobody could read while blaming a launcher nobody set.
    #
    # Only meaningful where the permission actually bites: root reads everything, so
    # on such a host this case is reported SKIPPED rather than passing vacuously. A
    # check that cannot fail is not a check, and saying so is the difference between
    # a state that was tested and one that merely did not complain.
    fixture denied 'CLANG_TIDY_EXE:FILEPATH=/usr/bin/clang-tidy-22\nUSE_COMPILER_CACHE:BOOL=OFF\n' fronted
    chmod 000 "$scratch/denied/build.ninja" 2>/dev/null
    if [[ -r "$scratch/denied/build.ninja" ]]; then
        self_test_skipped="${self_test_skipped:+$self_test_skipped, }unreadable build.ninja (this user reads a 0000 file)"
    else
        expect "a build.ninja that cannot be READ is its own answer, not a count" \
            "unreadable" "$(launcher_verdict "$scratch/denied/build.ninja")"
        expect "and it is not a reason to configure, because no configure fixes it" \
            "" "$(configure_reason "$scratch/denied" no-tidy /usr/bin/clang-tidy-22)"
    fi
    chmod 644 "$scratch/denied/build.ninja" 2>/dev/null

    # The per-leg verdict (#501). The renderer is pure, so every state is reachable
    # here without a compiler -- which is the whole reason it takes its input as
    # arguments instead of reading the globals.
    #
    # The defect being guarded is a READING failure, not a crash: the old gate
    # printed one true sentence and let the reader infer a false one. So what these
    # pin is that the states are DISTINGUISHABLE, not merely that each prints
    # something.
    expect "a leg that ran and passed says so" \
        "== gate legs:
==   gate-clang-debug   passed" \
        "$(leg_summary gate-clang-debug=passed)"
    expect "a leg that ran and failed says FAILED" \
        "== gate legs:
==   gate-clang-debug   FAILED" \
        "$(leg_summary gate-clang-debug=failed)"

    # The acceptance criterion, asserted as the inequality it actually is rather than
    # by eyeballing two literals: these two renderings must never coincide.
    expect "FAILED and NOT RUN do not render the same for the same preset" \
        "differ" \
        "$([[ "$(leg_summary gate-gcc-release=failed)" != "$(leg_summary gate-gcc-release=not-run)" ]] && echo differ || echo SAME)"

    # The scenario from #493, end to end: first leg red, second never asked. The
    # second line is the one that was missing for five consecutive gate runs.
    expect "a first-leg failure names the leg that never started" \
        "== gate legs:
==   gate-clang-debug   FAILED
==   gate-gcc-release   NOT RUN -- the gate stopped before this leg, so it has reported NOTHING" \
        "$(leg_summary gate-clang-debug=failed gate-gcc-release=not-run)"

    # The fourth state, and the reason the renderer has a default arm at all: a state
    # it does not know must not be shown as the nearest plausible verdict. `passed`
    # there would recreate #501 exactly, one level down.
    expect "an unrecognised state is reported as unrecognised, never as a verdict" \
        "== gate legs:
==   gate-clang-debug   UNKNOWN STATE 'wat' -- this is a bug in the gate, not a verdict" \
        "$(leg_summary gate-clang-debug=wat)"

    # An entry nothing has written yet reads as never run, which is what makes an
    # empty `leg_states` honest rather than a hole -- and the second check is the
    # index arithmetic itself, which pairs row N with state N.
    expect "leg_pairs reads an unwritten entry as never run" \
        "gate-clang-debug=not-run
gate-gcc-release=not-run" \
        "$(leg_pairs)"
    expect "leg_pairs pairs each table row with the state written for it" \
        "gate-clang-debug=passed
gate-gcc-release=failed" \
        "$(leg_states=(passed failed); leg_pairs)"

    # THE WIRING, which none of the above can see. Deleting the summary from `fail`,
    # or dropping the index advance in `run_all_legs`, leaves every check above green
    # while reverting the whole of #501 -- a report nothing calls is the bug it was
    # written to fix. Both mutations were confirmed to survive the checks above
    # before these were written, which is the only reason to trust that they add
    # anything.
    #
    # Driven through the real `run_all_legs` against stub runners. `fail` exits, and
    # inside `$( ... )` that ends the substitution's subshell rather than this script,
    # so the refusal can be captured and read rather than killing the self-test.
    #
    # `; echo CONTINUED` is the sentinel for a THIRD mutation the other two miss:
    # because the summary is printed INSIDE `fail`, a `fail` that stopped exiting
    # would produce byte-identical output while letting the gate run the next leg and
    # finish `LOCAL GATE PASSED` after having printed `GATE FAILED`. The expected text
    # ends at the leg block, so the sentinel appearing is a mismatch.
    stub_ok() { :; }
    stub_fail_first() {
        if [[ "$1" == "${gate_presets[0]%%|*}" ]]; then fail "$1 tests"; fi
    }
    stub_returns_one() { return 1; }

    expect "a run where every leg passes marks every leg passed" \
        "gate-clang-debug=passed
gate-gcc-release=passed" \
        "$(run_all_legs stub_ok; leg_pairs)"

    expect "a first-leg failure refuses through fail(), naming the leg never reached" \
        "GATE FAILED: gate-clang-debug tests
== gate legs:
==   gate-clang-debug   FAILED
==   gate-gcc-release   NOT RUN -- the gate stopped before this leg, so it has reported NOTHING" \
        "$( (run_all_legs stub_fail_first; echo CONTINUED) 2>&1 )"

    # A runner that RETURNS non-zero rather than calling `fail`. There is no `set -e`,
    # so an unchecked call would mark this leg `passed`, run the next one, and end the
    # gate green -- #501 one level down. The natural future edit (a non-fatal leg that
    # returns instead of exiting) is exactly what would do it.
    expect "a runner that returns non-zero is a failed leg, never a passed one" \
        "GATE FAILED: gate-clang-debug returned 1 rather than refusing
== gate legs:
==   gate-clang-debug   FAILED
==   gate-gcc-release   NOT RUN -- the gate stopped before this leg, so it has reported NOTHING" \
        "$( (run_all_legs stub_returns_one; echo CONTINUED) 2>&1 )"

    # The table drives the launcher and analyser decisions above, the `leg_pairs`
    # checks, and the whole gate below, so a row that stopped parsing would make
    # those vacuous while every one of them passed. The `leg_summary` checks are
    # deliberately NOT among them -- they pass their pairs explicitly, which is what
    # lets them state what the renderer does independently of what the gate runs.
    expect "the preset table still has two rows" "2" "${#gate_presets[@]}"
    for row in "${gate_presets[@]}"; do
        case "${row#*|}" in
            tidy|no-tidy) ;;
            *) echo "SELF-TEST FAILED: unknown analyser column in '$row'" >&2
               self_test_failures=$((self_test_failures + 1)) ;;
        esac
    done

    [[ "$self_test_failures" -eq 0 ]] || exit 1

    # The interpreter is named for the same reason the gate names its analyser: this
    # script is written to bash 3.2 because macOS ships one, and "it passed on some
    # bash" is the shape of claim this whole file exists to stop making. A runner
    # with a newer bash first on PATH proves the checks and not the constraint, and
    # the log is the only place that difference is visible.
    echo "LOCAL GATE SELF-TEST PASSED (bash ${BASH_VERSION})${self_test_skipped:+ -- SKIPPED: $self_test_skipped}"
    exit 0
fi

# Resolved once, to an absolute path, and checked before anything is built -- the
# treatment `clang-format` already had, for the same reason: a gate whose tool is
# missing must refuse BY NAME rather than fall back to whatever PATH offers, because
# the fallback is a clean report about a different analyser.
#
# Independent of `--no-format`, which names the formatter and not this.
#
# Only demanded when some preset in the table actually tidies. A gate that refused
# to start over a tool none of its configurations use would be a gate people stop
# running.
tidy=""
tidy_path=""
for row in "${gate_presets[@]}"; do
    if [[ "${row#*|}" == "tidy" ]]; then
        tidy="clang-tidy-${tools_version}"
        tidy_path="$(command -v "$tidy" 2>/dev/null || true)"
        [[ -n "$tidy_path" ]] || fail "$tidy not found, and this gate will not fall back to whatever clang-tidy is on PATH; install it (pip download clang-tidy==${tools_version}.1.0) or set CLANG_TOOLS_VERSION"
        break
    fi
done

if [[ "$format" -eq 1 ]]; then
    formatter="clang-format-${tools_version}"
    command -v "$formatter" >/dev/null 2>&1 \
        || fail "$formatter not found; install it or pass --no-format"
    git ls-files '*.h' '*.hpp' '*.cpp' | xargs "$formatter" -i --style=file \
        || fail "clang-format"
    echo "== formatted with $formatter"
fi

# @param 1 The preset to build and test.
# @param 2 `tidy` or `no-tidy`, from the table.
run_preset() {
    local preset="$1"
    local analyser="$2"
    local log
    log="$(mktemp)"

    # The build directory path is spelled rather than asked for, and it is coupled
    # to CMakePresets.json's single `binaryDir` of
    # `${sourceDir}/out/build/${presetName}`. A preset that moved its build
    # directory would configure once too often, which is the harmless direction.
    #
    # Once, and the two files derived from it: a second literal spelling of the same
    # directory could name a file the gate never opened, and one of the two places
    # it appeared was a failure message telling a developer where to look.
    local build_dir="out/build/${preset}"
    local ninja="${build_dir}/build.ninja"

    # The analyser is passed as a cache entry rather than through the preset,
    # because `find_program` short-circuits on a cache entry that is already set --
    # verified, and the reason `cmake/portable/ClangTidy.cmake` needs no change and
    # stays generic. `-D` on the command line sets that entry even on a directory
    # that already cached a different one.
    #
    # A non-empty array by construction: bash 3.2 under `set -u` treats
    # `"${arr[@]}"` on an empty array as an unbound variable, and this gate runs on
    # macOS.
    local -a configure
    # No `-DUSE_COMPILER_CACHE=OFF` here: the gate presets carry it as a cache
    # variable (#487). The refusal below still reads `build.ninja` rather than the
    # cache, for #471's reason -- passing the flag is not the fact.
    configure=(cmake --preset "$preset")
    if [[ "$analyser" == "tidy" ]]; then
        configure+=("-DCLANG_TIDY_EXE=${tidy_path}")
        echo "== $preset: clang-tidy pinned to $tidy ($tidy_path)"
    else
        echo "== $preset: no clang-tidy (ENABLE_TIDY is off in this preset)"
    fi

    local reason
    reason="$(configure_reason "$build_dir" "$analyser" "$tidy_path")"
    if [[ -n "$reason" ]]; then
        echo "== $preset: configure ($reason)"

        # The gate owns this directory (#487). It used to configure
        # `out/build/clang-debug` and `out/build/gcc-release` -- the two AGENT.md
        # tells developers and agents to build in -- and turning the compiler cache
        # off there turned it off for every ORDINARY build in that tree from then
        # on, because a `-D` writes a cache entry and `option()` never overrides
        # one. A standing ~2.4x on a full rebuild, in the repository whose product
        # is a compile cache.
        #
        # Now the setting lives in a `gate-` preset with its own `binaryDir`, so
        # the decision that is right for a reference build is imposed on nothing
        # else. The NOTE that used to be printed here was the stated-cost half of
        # accepting that hazard; it is gone with the hazard rather than kept as
        # reassurance about something that no longer happens.

        # Turning the launcher off rewrites every compile command, so ninja rebuilds
        # the whole configuration once. Said HERE, at the moment it is decided,
        # because a developer watching both presets rebuild from scratch with no
        # explanation will reasonably file it as breakage. An explained cost is a
        # cost; an unexplained one is a bug report.
        #
        # Decided from what the CURRENT build actually HAS, not from the reason
        # text. The whole point of this check is that the flag is the intent and the
        # generated build is the fact, and the two disagree here in a case that is
        # ordinary rather than exotic: on a machine with no launcher installed at
        # all, `USE_COMPILER_CACHE` reads ON while `build.ninja` carries no launcher
        # edge, and the configure rewrites no compile command. Warning about a
        # from-scratch rebuild there would be a warning about nothing.
        case "$(launcher_verdict "$ninja")" in
            unknown|0) ;;
            *)
                echo "== $preset: dropping the compiler-cache launcher changes every compile"
                echo "==   command, so this configuration rebuilds from scratch ONCE. Expected."
                ;;
        esac
        if ! "${configure[@]}" > "$log" 2>&1; then
            tail -40 "$log"
            fail "$preset configure (full log: $log)"
        fi
    fi

    # After the configure and BEFORE the build, because what is being refused is a
    # build that has not happened yet. The flag above states the intent; this reads
    # the fact out of the generated build, and they are not the same -- see
    # launcher_verdict.
    local verdict
    verdict="$(launcher_verdict "$ninja")"
    case "$verdict" in
        0)
            echo "== $preset: no compiler-cache launcher in the generated build"
            ;;
        unknown)
            fail "$preset: $ninja is not there after configuring, so whether a compiler cache fronts this build cannot be answered; a gate that cannot check must not report"
            ;;
        unreadable)
            fail "$preset: $ninja cannot be read, so whether a compiler cache fronts this build cannot be answered; a gate that cannot check must not report. This is a permission or filesystem problem, not a launcher one -- no configure will repair it"
            ;;
        *)
            fail "$preset: the generated build is fronted by a compiler-cache launcher despite the gate preset's USE_COMPILER_CACHE=OFF (launcher-fronted edges: $verdict), so its objects need not match this tree (#319, #368); something set CMAKE_CXX_COMPILER_LAUNCHER externally -- a preset, a toolchain file, or an older -D -- and cmake/portable/CompileCache.cmake leaves such a value untouched. Reconfigure with --fresh, or unset it"
            ;;
    esac

    echo "== $preset: build"
    if ! cmake --build --preset "$preset" > "$log" 2>&1; then
        grep -E 'error:|FAILED' "$log" | head -40
        fail "$preset build (full log: $log)"
    fi

    # Parallel, and that is the point rather than the speed. Every TEST_CASE is
    # its own process under catch_discover_tests, so a fixture that names a
    # scratch directory from a per-process counter hands two concurrent cases the
    # same path and the second wipes the first. That bug has been written five
    # times in this repository and nothing has ever run the tests in the shape
    # that shows it -- CI does not, and neither did this gate. The tests that
    # genuinely cannot share (a daemon, a fixed port) carry RUN_SERIAL.
    #
    # getconf rather than nproc: this gate runs on macOS too.
    local jobs="${FASTCACHE_GATE_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"

    echo "== $preset: test (--parallel $jobs)"
    if ! ctest --preset "$preset" --parallel "$jobs" > "$log" 2>&1; then
        grep -E '\*\*\*Failed|\*\*\*Timeout|tests passed' "$log" | head -30
        fail "$preset tests (full log: $log)"
    fi
    grep -E 'tests passed' "$log" | head -1
    rm -f "$log"
}

run_all_legs run_preset

# The same block on the way out green. A summary that appears only on failure is one
# nobody has read when it matters, and the presets are read from the table rather
# than named in the sentence -- the old line said "(clang-debug + gcc-release)" as a
# literal, so a third row would have been silently absent from the gate's own
# statement of what it had just done.
echo
leg_summary $(leg_pairs)
echo "LOCAL GATE PASSED"
