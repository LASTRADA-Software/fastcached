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
# Usage:  scripts/local-gate.sh [--no-format] [--self-test] [--classify=<log>]
#
#   --no-format  skip the clang-format pass. It does NOT loosen the clang-tidy pin:
#                those are two tools and the flag names one of them.
#   --self-test  check the configure decision against synthetic CMake caches, and
#                the launcher refusal against synthetic `build.ninja` files, then
#                exit. Needs no compiler, no cmake and no clang-tidy, which is what
#                lets `ctest -R local-gate-selftest` run it everywhere.
#   --classify=  read a captured gate log and say what happened to the run that
#                wrote it -- `passed`, `failed`, `did-not-conclude` or
#                `no-gate-run` -- with a distinct exit status for each. `-` reads
#                stdin. See "A run that never concluded" below.
#
# ## A run that never concluded (#584)
#
# The verdict came from `fail()` and from the green path, and NEITHER runs when the
# process is killed. So a run terminated by a signal ended with no `GATE FAILED`,
# no `LOCAL GATE PASSED` and -- after #501 -- no leg block either, and the only
# thing distinguishing it from a completed run was the ABSENCE of both lines. That
# is a real signal and a fragile one: absence is also what a scrolled-past line, a
# truncated log and a lost terminal look like.
#
# It is not hypothetical. Every lane on a shared machine runs these same script
# names out of different worktrees, so a name-matched `pkill -f local-gate.sh` in
# one lane hits every other lane's run -- the worktree path appears nowhere in the
# pattern.
#
# **A trap does not fix this here, and that is measured rather than assumed.** Bash
# defers a TRAPPED signal until the running foreground command returns, and this
# gate's foreground commands are `cmake`, `ninja` and `ctest`. Measured on bash
# 5.3: SIGTERM sent at t=2s into a 6s foreground command ran the handler at t=6s.
# The standard workaround -- background the command and `wait` for it, which bash
# interrupts -- does fire immediately (t=2s, measured), and it is WORSE here: the
# child is orphaned and reparented, so the gate would report `did-not-conclude`
# while leaving `ninja` still writing into the build directory. A developer's
# re-run then races an orphaned build in the same `binaryDir`, which is the
# two-gates-in-one-build-directory defect this rulebook already names. A late trap
# is close to no trap; an early one that orphans the build is a worse bug than the
# reporting gap it closes.
#
# So the route is a START MARKER plus a rule, and the rule is:
#
#     A log with a start marker and no terminal line DID NOT CONCLUDE. Re-run it.
#     Never interpret it, and never read it as a red gate.
#
# `did-not-conclude` is a distinct outcome from `failed` because they are fixed in
# different places -- one is somebody else's `pkill`, the other is your code. It is
# the fifth state beside skipped / absent / unstarted / failed.
#
# `--classify` is that rule as a function rather than as prose, so the tooling
# around a run can tell. It is pure -- log text in, one word out -- which is why
# `--self-test` drives every outcome in milliseconds. Capture BOTH streams into the
# log (`> gate.log 2>&1`): the failure line goes to stderr and the pass line to
# stdout, and a log holding only one of them can answer only half the question.
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

# The status a usage error exits with. Defined here because the argument loop below
# is the first thing that can use it, and READ by the outcome table further down
# rather than restated there: `--classify=$LOG` with an unset `LOG` lands on this
# arm, so no classification outcome may share the number, and a second literal `2`
# would go on agreeing with itself after one of them moved.
gate_usage_status=2

format=1
self_test=0
classify_log=""
# `${1+"$@"}` rather than `"$@"`: before bash 4.4 -- macOS ships a 2007
# /bin/bash -- expanding an empty `$@` under `set -u` is an UNBOUND variable
# and the script dies before it runs a step. This fires on exactly the
# zero-argument invocation AGENT.md documents, and ctest always passes
# --self-test, so `local-gate-selftest` cannot reach it (#793). The same
# spelling and the same reasoning are already 40 lines below, in `Leg` --
# the argument was carried one call short.
for arg in ${1+"$@"}; do
    case "$arg" in
        --no-format) format=0 ;;
        --self-test) self_test=1 ;;
        # `=<value>` rather than a second token, so this stays the one-pass `for`
        # loop it has always been. An empty value is refused here rather than read
        # as stdin: `--classify=` is a typo, and `-` is how stdin is asked for.
        --classify=?*) classify_log="${arg#*=}" ;;
        *) echo "usage: $0 [--no-format] [--self-test] [--classify=<log>|-]" >&2; exit "$gate_usage_status" ;;
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

# ---------------------------------------------------------------------------
# What a run says about itself, and what a LOG of one says afterwards (#584).
#
# The three lines a run emits about its own fate, defined once and both WRITTEN
# and READ through these names. A classifier holding its own copy of the strings
# would be a second claim rather than a cross-check: it would go on agreeing with
# itself after the gate stopped printing what it looks for, which is the shape of
# defect this whole file is about.
#
# Matched anchored at the start of a line, never as a substring anywhere -- this
# script's own text quotes all three, and a log carrying a `set -x` trace or a
# quoted message must not be read as a verdict. (`LOCAL GATE SELF-TEST PASSED` is
# safe for the same reason: it does not START with the pass marker.)
gate_start_marker="== LOCAL GATE STARTED"
gate_passed_marker="LOCAL GATE PASSED"
gate_failed_marker="GATE FAILED:"

# Outcome, exit status, and what a reader should do about it.
#
# A table, so the statuses are DERIVED from one place and the self-test can assert
# what the ticket actually requires -- that no two outcomes share a status. A
# killed run rendering as a red gate is the failure being fixed; two rows that
# drifted onto one number would rebuild it silently.
#
# `0` and `1` are the gate's OWN statuses for those two outcomes, so classifying a
# log answers with what the run itself answered. `2` is skipped deliberately and is
# the interesting one: it is this script's usage status, and `--classify=$LOG` with
# an unset `LOG` is a usage error -- a caller reading only the status would take
# "you typed that wrong" for "the run was killed". The two are not commensurable, so
# no outcome may sit on it, and the self-test asserts that rather than trusting it.
#
# EVERY status `--classify` can exit with, including the last two, which are not
# verdicts about a log at all -- one says this gate has no row for an outcome, the
# other that the log could not be read. They belong in the table for the reason
# the table exists: a status spelled as a literal somewhere else is invisible to
# the distinctness assertion below, so it can drift onto another one silently,
# which is the whole failure being prevented.
#
#   outcome|status|sentence
gate_outcomes=(
    "passed|0|the gate ran to the end and passed."
    "failed|1|the gate ran and FAILED. Read the log; this is a verdict about your tree."
    "did-not-conclude|3|the run STARTED and printed no verdict. It was killed -- a signal, a lost terminal, a reboot. It did NOT fail. Re-run it; do not interpret it."
    "no-gate-run|4|this log holds no gate run to classify. It was truncated above the start marker, or it is not a gate log."
    "unrecognised|5|UNRECOGNISED OUTCOME -- this gate has no row for that, which is a bug in the gate and not a verdict."
    "unreadable-log|6|the log named could not be read, so there is nothing here to classify. That is a different fact from anything the log might have said."
)

# What a gate LOG says about the run that produced it. PURE: log text on stdin,
# one outcome word on stdout, nothing else read and nothing written.
#
# The LAST start marker wins, and a terminal line counts only when a marker came
# before it. Both halves are load-bearing:
#
#   - A log file is reused. A `/tmp` wipe once left a PREVIOUS run's durable copy
#     sitting where the current one goes, and it reported a failure already fixed;
#     only an accident of timestamps told them apart. So a green run followed by a
#     killed one is `did-not-conclude`, which a `grep -c PASSED` gets backwards.
#   - A terminal line with no marker before it is not attributable to a run this
#     log can show, so it is `no-gate-run` rather than the verdict it looks like.
#     Escalating every way of not knowing to "re-run it" is the fail-closed
#     direction, and the only one that cannot silently vouch for a tree.
gate_outcome() {
    local line outcome="no-gate-run"
    # `|| [[ -n "$line" ]]` so a final line with no trailing newline is still read.
    while IFS= read -r line || [[ -n "$line" ]]; do
        case "$line" in
            "$gate_start_marker"*)
                outcome="did-not-conclude"
                ;;
            "$gate_passed_marker"*)
                [[ "$outcome" == "did-not-conclude" ]] && outcome="passed"
                ;;
            "$gate_failed_marker"*)
                [[ "$outcome" == "did-not-conclude" ]] && outcome="failed"
                ;;
        esac
    done
    echo "$outcome"
}

# The table row for an outcome, or the `unrecognised` row for one that has no
# row. An outcome this table cannot name renders as unrecognised rather than as
# the nearest plausible neighbour -- a seventh outcome arriving here as `passed`
# would be #501's defect rebuilt inside #584's fix. The fallback comes from the
# TABLE like every other row, so the distinctness assertion covers it too.
# @param 1 outcome word. Echoes `status|sentence`.
gate_outcome_row() {
    local row fallback=""
    for row in "${gate_outcomes[@]}"; do
        case "${row%%|*}" in
            "$1")         echo "${row#*|}"; return 0 ;;
            unrecognised) fallback="${row#*|}" ;;
        esac
    done
    echo "${fallback} (asked for '$1')"
}

# The exit-status column of an outcome's row. An accessor rather than a
# `${row%%|*}` at each call site: five temporaries existed only to hold the row
# on its way to this one field.
# @param 1 outcome word.
gate_outcome_status() {
    local row
    row="$(gate_outcome_row "$1")"
    echo "${row%%|*}"
}

# The tests a leg did not run, BY NAME, read out of that leg's own ctest output.
#
# `100% tests passed, 0 tests failed out of 3543` was the only thing a gate log
# said about tests, and under #1128 it is precisely the line that cannot be
# trusted: a Catch2 case failing exactly four assertions exits 4, every
# registration reads exit 4 as a skip, so ctest scores it `Skipped` and still
# prints that. The correct reading is therefore *check the skipped list* -- and
# the gate carried no skip data at all, so that instruction could not be carried
# out by anybody, and a lane believing it had checked had not (#1130).
#
# **A COUNT is refused as the fix, and that was measured rather than argued.** A
# branch adding a skip guarded on `geteuid() == 0` leaves the tally unchanged on
# an unprivileged gate: the skip never fires, the case runs normally, and a
# reader checking the number sees the familiar figure and concludes correctly FOR
# THE WRONG REASON. One more skip is not my skip, and an unchanged count is not
# an unchanged set. So this reports names.
#
# ## Why it reads ctest's OUTPUT and not `Testing/Temporary/LastTestsDisabled.log`
#
# That file holds the names, is written at no extra cost, and is the obvious
# source. It is also a stale verdict waiting to happen: ctest writes it only when
# something did not run, and does NOT remove or empty it when nothing did.
# Measured, one build directory, consecutive runs:
#
#   a run skipping one test    LastTestsDisabled.log written        17:51:15
#   a run skipping nothing     untouched -- still 17:51:15, still naming that test
#
# So a reader trusting the file after a clean run is handed the PREVIOUS run's
# skips, with nothing in the file to say so. The leg's own ctest output cannot go
# stale that way, because it is produced by the run it describes. It is also
# richer: the file merges every reason under a name that says `Disabled`, while
# ctest labels each entry `(Skipped)`, `(Disabled)` or `(Not Run)`.
#
# ## Three outcomes, not two
#
# "nothing skipped" is a conclusion drawn from the ABSENCE of a block, so
# something that must be PRESENT is asserted first: a ctest run that finished
# always prints its totals line. Text carrying none is reported UNKNOWN rather
# than as nothing-skipped. That is the state this project keeps collapsing -- the
# file was not there and nothing was skipped are not the same fact, and only one
# of them is about the tree.
#
# Entries are matched by SHAPE rather than by reading the block to end of input,
# because ctest prints `The following tests FAILED:` AFTER this block and a
# range-to-EOF would report those failures as skips -- which would be #1128's own
# defect rebuilt inside its remedy.
#
# The pointer is SUPPLEMENTARY and the names are the record: an inlined list is
# read months later, on another machine, after the build tree is gone, which is
# most of when a gate log is read at all. It cannot go stale either, because it
# is written by the run it describes -- the staleness hazard belongs to the
# POINTER here, which is why the one offered below names `LastTest.log`, refreshed
# every run, and never `LastTestsDisabled.log`, which is not. A pointer alone
# would have cost the whole ticket: the log would still carry no skip data, and
# the reader would still need a tree that may not exist.
#
# @param 1 Label to report against, normally the preset.
# @param 2 Optional build directory, named in the pointer line when given.
# Reads a ctest run's output on stdin.
skip_report() {
    local label="$1" build_dir="${2:-}" text entries count line
    text="$(cat)"

    # Herestrings rather than pipes throughout: `producer | grep -q` is a false
    # negative under `pipefail`, which this script sets.
    if ! grep -q 'tests passed' <<< "$text"; then
        echo "== ${label}: SKIPS UNKNOWN -- this output carries no ctest totals line, so"
        echo "==   it cannot support 'nothing was skipped'. Whether the run finished is"
        echo "==   the question to answer first."
        return 0
    fi

    entries="$(awk '
        /^The following tests did not run:/ { inblock = 1; next }
        inblock && /^[ \t]*[0-9]+ - / { sub(/^[ \t]*/, ""); print; next }
        inblock { inblock = 0 }
    ' <<< "$text")"

    if [[ -z "$entries" ]]; then
        echo "== ${label}: no tests skipped"
        return 0
    fi

    count="$(grep -c . <<< "$entries")"
    echo "== ${label}: ${count} test(s) did not run, by name --"
    while IFS= read -r line || [[ -n "$line" ]]; do
        [[ -n "$line" ]] && echo "==   ${line}"
    done <<< "$entries"
    if [[ -n "$build_dir" ]]; then
        echo "==   each one's own output is in ${build_dir}/Testing/Temporary/LastTest.log"
    fi
}

if [[ -n "$classify_log" ]]; then
    if [[ "$classify_log" == "-" ]]; then
        _classify_outcome="$(gate_outcome)"
    elif [[ -r "$classify_log" ]]; then
        _classify_outcome="$(gate_outcome < "$classify_log")"
    else
        # Not an outcome: the classifier could not READ its subject, which is a
        # different fact from anything the subject might have said. Reporting it as
        # `no-gate-run` would answer a question nobody could ask.
        echo "local-gate --classify: cannot read '$classify_log'" >&2
        exit "$(gate_outcome_status unreadable-log)"
    fi
    _classify_row="$(gate_outcome_row "$_classify_outcome")"
    echo "local-gate: ${_classify_outcome} -- ${_classify_row#*|}"
    exit "${_classify_row%%|*}"
fi

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
# only over-count. `CMAKE_<LANG>_LINKER_LAUNCHER` emits the same binding, and a
# linker cache in a reference build is a thing to refuse too.
#
# The number is reported as LAUNCHER BINDINGS, which is what it counts, and NOT as
# edges -- it was called edges and that was a unit error. Ninja emits one `LAUNCHER
# = ` line per RULE where the value is uniform, so a Linux measurement of this tree
# reads **5, covering 501 compile edges**, while #626's Windows reproduction
# recorded **669**. Those two numbers are not comparable and nothing said so. The
# verdict itself is unaffected -- any count above zero refuses -- but a refusal
# message is read by somebody comparing it with another platform, which is the
# whole reason this one exists.
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

# Whether the compiler the generated build will ACTUALLY execute is itself a
# compiler cache, and which one.
#
# `launcher_verdict` above answers "is a launcher configured", which is a
# DIFFERENT question and was the only one being asked. A distribution can put a
# cache AHEAD of the real compiler on `PATH`: Fedora ships `/usr/lib64/ccache/`
# populated with symlinks to `ccache` and puts it on `PATH` from a profile snippet
# nobody opted into. Then there is no launcher, `build.ninja` holds zero LAUNCHER
# bindings, the gate prints "no compiler-cache launcher in the generated build" --
# and every compile still goes through ccache, because THE COMPILER IS THE CACHE.
# Both statements are true and together they are misleading (#716, #804, #887).
#
# Measured on the host this was found on:
#     g++     -> /usr/lib64/ccache/g++     -> /usr/bin/ccache
#     clang++ -> /usr/lib64/ccache/clang++ -> /usr/bin/ccache
#
# Read out of `CMakeCXXCompiler.cmake` rather than `CMakeCache.txt`, because the
# cache holds what was TYPED and that differs per preset -- `clang-debug` records
# a bare `clang++`, `gcc-release` an absolute path -- while this file holds what
# CMake RESOLVED, which is what ninja runs. One observable, both presets.
#
# States: a cache NAME, `none`, or `unknown` when the file is missing or
# unreadable. `unknown` is not `none`: a gate that cannot check must not report.
# @param 1 Path to the build directory.
compiler_shim_verdict() {
    local file compiler resolved base
    file="$(ls "$1"/CMakeFiles/*/CMakeCXXCompiler.cmake 2>/dev/null | head -1)"
    if [[ -z "$file" || ! -r "$file" ]]; then
        echo "unknown"
        return 0
    fi
    compiler="$(sed -n 's/^set(CMAKE_CXX_COMPILER "\(.*\)")$/\1/p' "$file" | head -1)"
    if [[ -z "$compiler" ]]; then
        echo "unknown"
        return 0
    fi

    # `readlink -f` is GNU and macOS only grew it in 12.3, and this runs in the
    # default ctest set. Fall back to python3, then to the raw path -- an
    # unresolved path still gets the component check below, so the worst case is
    # a symlinked shim going unnoticed rather than a wrong verdict.
    resolved="$(readlink -f "$compiler" 2>/dev/null || true)"
    if [[ -z "$resolved" ]]; then
        resolved="$(python3 -c 'import os,sys; print(os.path.realpath(sys.argv[1]))' "$compiler" 2>/dev/null || true)"
    fi
    [[ -n "$resolved" ]] || resolved="$compiler"

    base="$(basename "$resolved")"
    case "$base" in
        ccache | sccache | distcc | icecc | fastcache-cc)
            echo "$base"
            return 0
            ;;
    esac

    # A shim directory whose entries are wrapper SCRIPTS rather than symlinks
    # resolves to itself, so the path component is the only thing left to read.
    case "/$compiler/" in
        */ccache/*)
            echo "ccache"
            return 0
            ;;
        */sccache/*)
            echo "sccache"
            return 0
            ;;
    esac
    echo "none"
}

# How many of this tree's headers clang-tidy's header filter would REPORT on
# (#1040), as `<matched>/<total>` -- or one word naming why it could not be asked.
#
# clang-tidy discards a finding in an unmatched header silently, with only a
# `Suppressed NNN warnings (NNN in non-user code)` line to show for it, so a
# filter matching NOTHING and a tree with no header findings render identically.
# Three patterns anchored on the checkout's NAME have now got this wrong, each
# fixed against the layouts its author happened to have; #1040 is the third, and
# it hid because CI's own checkout is the one shape that still matched.
#
# So the question asked here is not "does the pattern look right" but "how many
# of the headers in THIS working tree does it actually cover", which is a
# question a layout nobody has thought of yet still answers.
#
# `grep -E` is POSIX ERE and clang-tidy is `llvm::Regex`, which is a MODEL of the
# tool rather than the tool -- stated because a model more permissive than what
# it stands for produces confident wrong agreement. It is sound for the patterns
# this file has carried (character classes, alternation, `.*`) and it is checked
# against the real analyser by `scripts/check-header-filter.sh`, which plants a
# violation and asks clang-tidy itself.
#
# @param 1 Path to the `.clang-tidy` to read.
# @param 2 Repository root, since the filter is matched against absolute paths.
header_filter_coverage() {
    local config="$1" root="$2"
    [[ -r "$config" ]] || { echo "no-config"; return 0; }

    local include exclude
    include="$(sed -n "s/^HeaderFilterRegex:[[:space:]]*'\(.*\)'[[:space:]]*$/\1/p" "$config" | head -1)"
    [[ -n "$include" ]] || { echo "no-regex"; return 0; }
    exclude="$(sed -n "s/^ExcludeHeaderFilterRegex:[[:space:]]*'\(.*\)'[[:space:]]*$/\1/p" "$config" | head -1)"

    # The tracked set, not a directory walk: a header the repository does not
    # carry is not one this gate owes an opinion about, and a build directory is
    # full of headers that are nobody's first-party code.
    local headers
    headers="$(git -C "$root" ls-files '*.hpp' '*.h' 2>/dev/null)"
    [[ -n "$headers" ]] || { echo "no-headers"; return 0; }

    # A dependency must stay OUT, and coverage alone cannot see that: dependency
    # trees are not tracked, so `git ls-files` never lists them, and a filter that
    # takes catch2 leaves every coverage count perfect while the sweep drowns --
    # measured at 228 reported lines from one test translation unit.
    #
    # EVERY layout this project unpacks dependencies into is asked about, not only
    # the one on this machine. The first fix for #1040 got this wrong: it
    # excluded `_deps`, where FetchContent unpacks locally; `build.yml` sets
    # `CPM_SOURCE_CACHE` to `.cache/CPM`, so CI's catch2 sits at
    # `.cache/CPM/catch2/<hash>/src/catch2/...`, which contains `/src/` and is not
    # under `_deps`. It was taken, the sweep failed inside catch2, and no developer
    # machine could see it. A local guard that models one layout reproduces the
    # defect it exists to catch.
    #
    # Asked as PATHS rather than as files that must exist: a build directory may
    # legitimately not be there yet, and a check that only bites after a build does
    # not bite when it is first needed.
    local dep
    for dep in "${root}/out/build/gate-clang-debug/_deps/catch2-src/src/catch2/catch_test_macros.hpp" \
               "${root}/.cache/CPM/catch2/0123456789abcdef/src/catch2/catch_test_macros.hpp"; do
        if grep -qE "$include" <<< "$dep" \
            && { [[ -z "$exclude" ]] || ! grep -qE "$exclude" <<< "$dep"; }; then
            echo "deps-leak"
            return 0
        fi
    done

    header_filter_match "$include" "$exclude" "$root" <<< "$headers"
}

# The matching itself, over repo-relative header paths on stdin, as
# `<matched>/<total>` plus the first unmatched path when there is one.
#
# Split from the enumeration so `--self-test` can drive it at a SYNTHETIC root:
# the outcome under test is a property of where a checkout lives, and a case that
# used the real root would assert the opposite thing depending on which machine
# ran it -- passing on CI's `.../fastcached/` for the very layout that fails in a
# lane worktree, which is #1040's own blind spot rebuilt inside its guard.
#
# @param 1 The include regex. @param 2 The exclude regex, possibly empty.
# @param 3 The root the filter is matched against, since clang-tidy sees absolute paths.
header_filter_match() {
    local include="$1" exclude="$2" root="$3"
    local total=0 matched=0 first_missed="" path
    while IFS= read -r path; do
        [[ -n "$path" ]] || continue
        total=$((total + 1))
        if grep -qE "$include" <<< "$root/$path" \
            && { [[ -z "$exclude" ]] || ! grep -qE "$exclude" <<< "$root/$path"; }; then
            matched=$((matched + 1))
        elif [[ -z "$first_missed" ]]; then
            first_missed="$path"
        fi
    done
    echo "${matched}/${total}${first_missed:+ $first_missed}"
}

# What the gate says about that coverage.
#
# `all` is the only outcome that carries on. ZERO matched is the #1040 shape and
# gets its own sentence, because "the filter matched nothing" and "the tree has
# no header findings" are the two states this whole guard exists to separate --
# and a PARTIAL match is its own outcome again, since a pattern covering 200 of
# 285 headers is a gate that is silently blind to 85 files while looking healthy.
#
# @param 1 The `.clang-tidy` path, for the sentence. @param 2 The coverage verdict.
header_filter_report() {
    local config="$1" verdict="$2"
    local counts="${verdict%% *}" missed=""
    [[ "$verdict" == *" "* ]] && missed="${verdict#* }"
    local matched="${counts%%/*}" total="${counts##*/}"

    case "$verdict" in
        '')
            echo "the header-filter coverage check produced NO verdict for $config. That is a bug in the GATE, not a verdict about this tree: header_filter_coverage answers a count or one naming word on every path, so an empty answer means the substitution that read it died. There is a line on stderr above this one naming the variable"
            return 1
            ;;
        no-config)
            echo "$config is not readable, so which headers clang-tidy would report on cannot be answered; a gate that cannot check must not report"
            return 1
            ;;
        no-regex)
            echo "$config names no HeaderFilterRegex, so clang-tidy would report findings in NO header at all and say so only as 'Suppressed NNN warnings'. That is the #1040 failure with the pattern removed rather than mis-written"
            return 1
            ;;
        no-headers)
            echo "no tracked headers were found under this tree, so header-filter coverage could not be measured. That is the CHECK failing, not a clean tree -- git ls-files answered nothing, which in a checkout of this repository cannot be true"
            return 1
            ;;
        deps-leak)
            echo "clang-tidy's header filter in $config would report findings inside a DEPENDENCY tree, which carries no .clang-tidy of its own and would bury every first-party finding under someone else's code -- measured at 228 reported catch2 lines from a single test translation unit. Coverage cannot see this, because dependency trees are untracked and so are absent from the set the count is taken over: a filter that takes catch2 still reports perfect coverage. Both layouts are checked, because they disagree -- FetchContent unpacks into out/build/*/_deps/ locally while build.yml sets CPM_SOURCE_CACHE to .cache/CPM, and a pattern excluding only the first passed every developer machine and failed CI inside catch2. Fix it by narrowing what the filter INCLUDES to this repository's own roots under src/, not by adding another dependency location to exclude -- an exclusion bets on where the world puts things, and that bet has now lost once"
            return 1
            ;;
        0/*)
            echo "clang-tidy's HeaderFilterRegex in $config matches NONE of this tree's $total tracked headers, so every header finding would be discarded as non-user code and the analyser would report clean by analysing nothing (#1040). This is a property of where this checkout LIVES: three patterns anchored on the directory name have now missed a real layout, most recently every lane worktree at .../fastcached-worktrees/<lane>/src/. Fix the pattern, do not delete this check"
            return 1
            ;;
    esac

    if [[ "$matched" == "$total" ]]; then
        echo "== clang-tidy header filter covers all $total tracked headers"
        return 0
    fi
    echo "clang-tidy's HeaderFilterRegex in $config matches only $matched of this tree's $total tracked headers, so findings in the other $((total - matched)) would be discarded silently -- for example $missed. A partial match is worse than none, because the analyser still reports findings and so looks like it is working (#1040)"
    return 1
}

# What the gate SAYS about a verdict, split out from reading one.
#
# Both checks below read their verdict out of a command substitution and then
# decide what to say about it, and the two halves fail in different ways: a reader
# can be wrong about the tree, while a REPORTER can be wrong about the reader. The
# decision is a pure function of the verdict here for the reason `leg_summary` is
# one -- every state is then reachable from `--self-test` with no compiler, no
# build directory and no cache installed, which is the only way the states can be
# shown to be DISTINGUISHABLE rather than merely each printing something.
#
# Each prints its sentence and returns 0 to carry on or 1 to refuse. The refusal
# itself stays at `run_preset`'s statement level, through `report_or_refuse`,
# because `fail` inside a `$( )` ends the substitution's subshell and not the gate
# -- a reporter cannot refuse on its own behalf, and one that tried would print
# `GATE FAILED` and let the build run.
#
# THE EMPTY ARM IS THE POINT (#1031). Every path of both readers echoes a word, so
# an empty verdict does not mean "the tree is like this" -- it means the
# substitution that read it DIED. Under `set -u` an unbound variable inside `$( )`
# kills that subshell, writes one line to stderr and hands the caller "", which the
# `*)` arm then reported as `the resolved C++ compiler IS  (a compiler cache)`: a
# cache named nothing, refusing correct trees, with a blank as the only tell. So
# the empty verdict is its own named outcome. What is left on `*)` is exact rather
# than a default -- `compiler_shim_verdict`'s remaining answers are cache NAMES and
# `launcher_verdict`'s are counts -- which is the point of naming it, since the
# repair for one collapse is the prime site for the next.
#
# @param 1 The preset.
# @param 2 Path to the generated build, named in the sentences that mention it.
# @param 3 The verdict from `launcher_verdict`.
launcher_report() {
    local preset="$1" ninja="$2" verdict="$3"
    case "$verdict" in
        '')
            echo "$preset: the launcher check produced NO verdict for $ninja. That is a bug in the GATE, not a verdict about this tree: launcher_verdict answers a count, \`unknown\` or \`unreadable\` on every path, so an empty answer means the substitution that read it died -- an unbound variable under set -u is how that happened to the shim check one clause down (#1031), and this arm names the same state here rather than waiting for its turn. There is a line on stderr above this one naming the variable. Do not read this as a launcher"
            return 1
            ;;
        0)
            echo "== $preset: no compiler-cache launcher in the generated build"
            return 0
            ;;
        unknown)
            echo "$preset: $ninja is not there after configuring, so whether a compiler cache fronts this build cannot be answered; a gate that cannot check must not report"
            return 1
            ;;
        unreadable)
            echo "$preset: $ninja cannot be read, so whether a compiler cache fronts this build cannot be answered; a gate that cannot check must not report. This is a permission or filesystem problem, not a launcher one -- no configure will repair it"
            return 1
            ;;
        *)
            echo "$preset: the generated build is fronted by a compiler-cache launcher despite the gate preset's USE_COMPILER_CACHE=OFF (LAUNCHER bindings: $verdict), so its objects need not match this tree (#319, #368). This REFUSES rather than warns because it is wrong in BOTH directions: the observed case (#626) was five metrics tests failing in files the branch never touched, which costs somebody an investigation -- and the same substituted object can equally HIDE a real failure, with nothing to say so. A verdict about a tree that was not built cannot be read in either direction, so there is no safe way to continue past it. Something set CMAKE_CXX_COMPILER_LAUNCHER externally -- a preset, a toolchain file, or an older -D -- and cmake/portable/CompileCache.cmake leaves such a value untouched. Reconfigure with --fresh, or unset it"
            return 1
            ;;
    esac
}

# The same, for the resolved compiler.
# @param 1 The preset.
# @param 2 The build directory, named in the sentence that says where to look.
# @param 3 The verdict from `compiler_shim_verdict`.
shim_report() {
    local preset="$1" build_dir="$2" shim="$3"
    case "$shim" in
        '')
            echo "$preset: the compiler-shim check produced NO verdict for $build_dir. That is a bug in the GATE, not a verdict about this tree: compiler_shim_verdict answers \`none\`, \`unknown\` or a cache NAME on every path, so an empty answer means the substitution that read it died -- which is #1031 exactly, \$dir where \$build_dir was meant, reported by the default arm as a cache with no name. There is a line on stderr above this one naming the variable. Do not read this as a cache"
            return 1
            ;;
        none)
            echo "== $preset: the resolved compiler is not a compiler cache"
            return 0
            ;;
        unknown)
            echo "$preset: the resolved compiler cannot be read from $build_dir/CMakeFiles/*/CMakeCXXCompiler.cmake, so whether a cache fronts this build cannot be answered; a gate that cannot check must not report"
            return 1
            ;;
        *)
            echo "$preset: the resolved C++ compiler IS $shim (a compiler cache), so this is not a reference build and its objects need not match this tree (#319, #368, #716, #804, #887). USE_COMPILER_CACHE=OFF and zero LAUNCHER bindings are both TRUE here and neither can see this: a distribution can put a cache ahead of the real compiler on PATH -- Fedora ships /usr/lib64/ccache/ with symlinks to ccache and puts it on PATH from a profile snippet nobody opted into -- and then there is no launcher to count. This REFUSES rather than warns for the same reason the launcher clause does: a verdict about a tree that was not built cannot be read in EITHER direction, so a substituted object can equally invent a failure or hide one. Point CMAKE_CXX_COMPILER at the real compiler (readlink -f will tell you where the shim goes), or take the shim directory off PATH for this run"
            return 1
            ;;
    esac
}

# Say what a reporter said, or refuse the gate with it.
#
# The status is what decides, never the text: a reporter whose sentence is right
# and whose `return` is wrong would print a refusal and let the build run, which is
# the same shape of quiet-wrong-verdict the reporters exist to stop. Written once
# because both call sites differ only by which reporter they name.
#
# The two kinds of argument fail differently, and both are covered rather than one
# standing in for the other. A PLAIN one -- the preset, the directory -- is expanded
# in the gate's own shell, so an unbound name there kills the gate loudly and names
# itself. The VERDICT argument is a substitution, so an unbound name inside it dies
# in that subshell and arrives as "" instead: quiet, and exactly #1031. That one is
# caught by the reporters' empty arm, not here, which is why the arm exists at all.
#
# @param 1.. The reporter and its arguments.
report_or_refuse() {
    local line status
    line="$("$@")"
    status=$?
    # And the same collapse one level up, in the repair for it: a reporter that
    # said NOTHING would reach `fail ""`, which prints `GATE FAILED:` and stops
    # having named no reason at all -- a refusal with a blank where the cause goes,
    # which is #1031's shape exactly. Every arm of every reporter echoes, so an
    # empty line means the reporter did not RUN: a name typed wrong is status 127
    # with `command not found` on stderr and nothing here.
    [[ -n "$line" ]] || fail "the gate's own reporter '$1' produced no line at all (exit $status), so there is no verdict to read. That is a bug in the GATE, not a verdict about this tree -- every arm of every reporter echoes, so this means the reporter did not run. There is a line on stderr above this one saying why"
    [[ "$status" -eq 0 ]] || fail "$line"
    echo "$line"
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
        *)       reasons="${reasons:+$reasons; }the generated build is launcher-fronted (LAUNCHER bindings: $fronted)" ;;
    esac

    # Several clauses can hold at once, and a run that re-configures for more than
    # one reason should say all of them: reporting only the first would leave a
    # reader believing the others were already right.
    #
    # Unguarded: every caller takes this through `$( )`, which strips the trailing
    # newline, so an empty `echo` and printing nothing are the same value.
    echo "$reasons"
}

# Whether the formatter should be run at all, and over how many files.
#
# A pure function of the list so `--self-test` drives the same code the run uses;
# the alternative is a guard tested nowhere, which is the shape this file exists to
# refuse.
#
# It exists because an EMPTY list is not a formatted tree. `git ls-files | xargs
# clang-format -i` hands the formatter no operands, GNU xargs runs it anyway, and
# clang-format reads stdin and answers `error: cannot use -i when reading from
# stdin` -- so the gate reported `GATE FAILED: clang-format` for a tree whose
# formatting it had never examined. Measured on GNU findutils 4.9.0: empty input
# with no `-r` RUNS the command with no arguments, and the real pipeline exits
# **123**, which is xargs's "the utility exited non-zero" and neither 0 nor 1.
#
# The observed cause was a linked worktree whose `.git` file held an ABSOLUTE
# `gitdir:`: the other git could not resolve it and listed nothing, where the same
# tree listed 395 files once the pointer was relative (#1064).
#
# NOT `xargs -r`. That fixes the symptom in the wrong direction -- an empty list
# would become a silent PASS, the gate announcing it had formatted a tree it never
# read. The guard is on the LIST, so both xargs dialects refuse identically and the
# verdict does not depend on which platform the developer is standing on.
#
# @param 1 The newline-separated list, as `git ls-files` prints it.
# @return `refuse <n>` when there is nothing to format, `format <n>` otherwise.
format_plan() {
    local list="$1"
    local count=0
    if [[ -n "$list" ]]; then
        count="$(printf '%s\n' "$list" | wc -l | tr -d ' ')"
    fi
    if [[ "$count" -eq 0 ]]; then
        echo "refuse ${count}"
    else
        echo "format ${count}"
    fi
}

if [[ "$self_test" -eq 1 ]]; then
    scratch="$(mktemp -d)"
    trap 'rm -rf "$scratch"' EXIT
    self_test_failures=0
    # How many checks RAN, printed on both paths. `expect` is silent when it
    # passes, so without this a run that died half way through -- an unbound
    # variable, a syntax error in a case, a stray `exit` -- is indistinguishable
    # from one where every check passed, and "no failures printed" reads as "the
    # guard did not fire". That is #584's own confusion one level down, and it is
    # what makes breaking a rule and WATCHING this refuse a check rather than a
    # hope.
    self_test_ran=0
    # Skipped is its own outcome, and it has to be VISIBLE or it reads as tested.
    # `ctest` shows a passing test's output to nobody, so a skip announced only on
    # stderr is a case that silently did not run -- which is the exact collapse this
    # file spends its length refusing. It rides the final PASSED line instead,
    # beside the interpreter, for the reason the interpreter is named there.
    self_test_skipped=""

    # @param 1 What is being checked. @param 2 Expected. @param 3 Actual.
    expect() {
        self_test_ran=$((self_test_ran + 1))
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
        "the generated build is launcher-fronted (LAUNCHER bindings: 1)" \
        "$(configure_reason "$scratch/stale-fronted" tidy /usr/bin/clang-tidy-22)"
    expect "a no-tidy preset is re-configured over a stale fronted build too" \
        "the generated build is launcher-fronted (LAUNCHER bindings: 1)" \
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

    # `compiler_shim_verdict` -- the OTHER way a cache fronts a reference build,
    # which no count of LAUNCHER bindings can see (#716, #804, #887).
    #
    # Both directions, and both SHAPES of shim: a symlink into `ccache` (what
    # Fedora ships) and a wrapper the resolver cannot follow, where the directory
    # NAME is the only evidence left. Without the second, a distro that ships
    # scripts rather than symlinks passes a guard written against symlinks alone.
    mk_compiler_fixture() {  # $1 = fixture name, $2 = compiler path to record
        mkdir -p "$scratch/$1/CMakeFiles/3.28.0"
        printf 'set(CMAKE_CXX_COMPILER "%s")\n' "$2" > "$scratch/$1/CMakeFiles/3.28.0/CMakeCXXCompiler.cmake"
    }

    mkdir -p "$scratch/shimdir/ccache" "$scratch/realbin"
    printf '#!/bin/sh\nexit 0\n' > "$scratch/realbin/ccache"; chmod +x "$scratch/realbin/ccache"
    printf '#!/bin/sh\nexit 0\n' > "$scratch/realbin/g++";    chmod +x "$scratch/realbin/g++"
    ln -sf "$scratch/realbin/ccache" "$scratch/shimdir/ccache/clang++"

    mk_compiler_fixture shim-symlink "$scratch/shimdir/ccache/clang++"
    expect "a compiler that RESOLVES to ccache is named, not counted" \
        "ccache" "$(compiler_shim_verdict "$scratch/shim-symlink")"

    # A wrapper script in a `ccache` directory: it resolves to itself, so only the
    # path component says what it is.
    printf '#!/bin/sh\nexit 0\n' > "$scratch/shimdir/ccache/g++-wrapper"; chmod +x "$scratch/shimdir/ccache/g++-wrapper"
    mk_compiler_fixture shim-wrapper "$scratch/shimdir/ccache/g++-wrapper"
    expect "a wrapper inside a ccache directory is caught by the path component" \
        "ccache" "$(compiler_shim_verdict "$scratch/shim-wrapper")"

    mk_compiler_fixture real-compiler "$scratch/realbin/g++"
    expect "a real compiler is none, so the guard does not refuse every host" \
        "none" "$(compiler_shim_verdict "$scratch/real-compiler")"

    # `unknown` is not `none`: a gate that cannot read the file must not report.
    mkdir -p "$scratch/no-compiler-file"
    expect "a build directory with no CMakeCXXCompiler.cmake is unknown, never none" \
        "unknown" "$(compiler_shim_verdict "$scratch/no-compiler-file")"

    mkdir -p "$scratch/empty-compiler-file/CMakeFiles/3.28.0"
    : > "$scratch/empty-compiler-file/CMakeFiles/3.28.0/CMakeCXXCompiler.cmake"
    expect "a file naming no compiler is unknown, never none" \
        "unknown" "$(compiler_shim_verdict "$scratch/empty-compiler-file")"

    # -----------------------------------------------------------------------
    # What the gate SAYS about those verdicts (#1031), which is a different
    # question from what it reads and had no check of its own. The reader above
    # was correct throughout; the two clauses that ACT on it were where a correct
    # tree got refused.
    #
    # What a reporter answered, reduced to the two things a caller acts on: the
    # words that tell one outcome from another, and the STATUS, which is what
    # actually decides. Pinning the paragraphs whole would be a test of the prose;
    # pinning only the words would miss a reporter that says the right thing and
    # returns 0 -- which prints a refusal and then lets the gate build the tree it
    # just refused, and no text-only assertion can see it.
    #
    # @param 1 A substring that must appear. @param 2.. The reporter and its arguments.
    # stderr is merged in, or the one case whose subject WRITES there -- `fail`,
    # which is the whole of `report_or_refuse`'s refusal path -- would be read as a
    # reporter that said nothing. The reporters themselves write to stdout only, so
    # nothing else is affected.
    report_says() {
        local want="$1"; shift
        local text status
        text="$("$@" 2>&1)"
        status=$?
        case "$text" in
            *"$want"*) printf 'said|%s' "$status" ;;
            *)         printf 'DID NOT SAY [%s], said [%s]|%s' "$want" "$text" "$status" ;;
        esac
    }

    # The three verdicts a healthy gate can reach, each asserted on its own,
    # because the defect was two of the four outcomes arriving at one arm. A case
    # that only checked "the gate said OK" is what let it ship.
    expect "a real compiler is reported as no cache, and the gate carries on" \
        "== gate-clang-debug: the resolved compiler is not a compiler cache|0" \
        "$(text="$(shim_report gate-clang-debug "$scratch/real-compiler" none)"; printf '%s|%s' "$text" "$?")"
    expect "a cache is NAMED, and refuses" \
        "said|1" "$(report_says 'IS ccache (a compiler cache)' shim_report gate-clang-debug /b ccache)"
    expect "a compiler that cannot be read refuses, and says where it looked" \
        "said|1" "$(report_says '/b/CMakeFiles/*/CMakeCXXCompiler.cmake' shim_report gate-clang-debug /b unknown)"

    # And the fourth, which is the ticket. Driven rather than described: `dir` is
    # `local` to `configure_reason` and therefore unset here exactly as it was
    # unset in `run_preset`, so this is the expression that clause used to hold.
    # Under `set -u` bash kills the substitution's subshell and hands the caller
    # "", putting the diagnosis on stderr -- quiet at the point that ACTS on it,
    # which is the whole reason a wrong variable name presented as a verdict about
    # the tree rather than as a crash. stderr is dropped so the self-test's own
    # output stays readable; the point being made is about the VALUE.
    shim_verdict_of_unbound_dir() { compiler_shim_verdict "$dir"; }
    expect "an unbound build directory yields an empty verdict, not a diagnosis" \
        "" "$(shim_verdict_of_unbound_dir 2>/dev/null)"
    expect "and an empty verdict is refused BY NAME, never as a nameless cache" \
        "said|1" \
        "$(report_says 'produced NO verdict' \
            shim_report gate-clang-debug "$scratch/real-compiler" "$(shim_verdict_of_unbound_dir 2>/dev/null)")"

    # The assertion that actually separates fixed from broken, and it is NOT the
    # inequality it looks like it should be: under the bug the empty verdict
    # rendered as `IS  (a compiler cache)` and a named one as `IS ccache (a
    # compiler cache)`, so those two DIFFER and an inequality check passes while
    # the defect is live. A signal that cannot be false in the failing case is not
    # evidence. What must hold is the positive claim: an outcome meaning "this
    # check did not run" must make no claim about a cache at all.
    expect "an empty verdict makes no claim about a compiler cache" \
        "no" "$([[ "$(shim_report gate-clang-debug /b '')" == *"a compiler cache"* ]] && echo yes || echo no)"

    # The launcher clause is the same shape three lines up, reading a different
    # file, and it had the same open default arm. Its four healthy states plus the
    # empty one, so the pair is covered rather than the half that happened to fire.
    expect "a build with no launcher edge is reported clean, and the gate carries on" \
        "== gate-clang-debug: no compiler-cache launcher in the generated build|0" \
        "$(text="$(launcher_report gate-clang-debug /b/build.ninja 0)"; printf '%s|%s' "$text" "$?")"
    expect "a launcher-fronted build refuses, quoting the count" \
        "said|1" "$(report_says 'LAUNCHER bindings: 2' launcher_report gate-clang-debug /b/build.ninja 2)"
    expect "a build.ninja that is not there refuses, naming the file" \
        "said|1" "$(report_says '/b/build.ninja is not there' launcher_report gate-clang-debug /b/build.ninja unknown)"
    expect "a build.ninja that cannot be read refuses as a permission problem" \
        "said|1" "$(report_says 'not a launcher one' launcher_report gate-clang-debug /b/build.ninja unreadable)"
    expect "an empty launcher verdict is refused BY NAME, never as a launcher" \
        "said|1" "$(report_says 'produced NO verdict' launcher_report gate-clang-debug /b/build.ninja '')"
    expect "an empty launcher verdict claims no launcher bindings" \
        "no" "$([[ "$(launcher_report gate-clang-debug /b/build.ninja '')" == *"LAUNCHER bindings:"* ]] && echo yes || echo no)"

    # THE PLUMBING, which none of the above can see: a reporter's STATUS has to
    # reach `fail`. Dropping the status check leaves every check above green while
    # letting the gate build a tree it has just refused -- the reporters print, so
    # the refusal is even in the log. Both stubs are reporters in shape only, which
    # is the point: what is under test is the caller.
    #
    # `fail` exits, and inside `$( ... )` that ends the substitution's subshell
    # rather than this script, so the refusal can be read rather than killing the
    # self-test. `; echo CONTINUED` is the sentinel for the mutation the text alone
    # cannot see. `leg_states` is reset inside the subshell so the leg block is the
    # same whatever has run before this point.
    stub_report_ok()     { echo "== a check says something"; return 0; }
    stub_report_refuse() { echo "a check refuses for a reason"; return 1; }
    stub_report_silent() { return 1; }

    expect "a reporter that returns 0 is printed, and the gate carries on" \
        "== a check says something
CONTINUED" \
        "$( (report_or_refuse stub_report_ok; echo CONTINUED) 2>&1 )"
    expect "a reporter that returns non-zero refuses through fail(), and stops" \
        "GATE FAILED: a check refuses for a reason
== gate legs:
==   gate-clang-debug   NOT RUN -- the gate stopped before this leg, so it has reported NOTHING
==   gate-gcc-release   NOT RUN -- the gate stopped before this leg, so it has reported NOTHING" \
        "$( (leg_states=(); report_or_refuse stub_report_refuse; echo CONTINUED) 2>&1 )"

    # A reporter that produced no line is the same collapse inside the repair for
    # it: `fail "$line"` with an empty line prints `GATE FAILED:` and a blank where
    # the cause goes. The reporter is named instead. A typo'd reporter name is the
    # way this is actually reached -- status 127, `command not found` on stderr and
    # nothing on stdout -- and a stub that returns without echoing is that state
    # without depending on the shell's wording for a missing command.
    expect "a reporter that produced no line is refused by NAME, not by a blank" \
        "said|1" \
        "$(report_says "reporter 'stub_report_silent' produced no line at all" \
            report_or_refuse stub_report_silent)"

    # -----------------------------------------------------------------------
    # The clang-tidy header filter (#1040). Driven at SYNTHETIC roots, because
    # the whole defect is that the answer depends on where a checkout lives: a
    # case using this run's own root would assert one thing in a lane worktree
    # and the opposite on CI, which is the blind spot being closed.
    _hdrs="src/FastCache/Core/Base64.hpp
src/apps/fastcached/Main.hpp"
    _old='.*/(fastcached[^/]*|worktrees/[^/]+)/src/.*'
    _new='.*/src/(CowTree|FastCache|apps|tests)/.*'
    _exc=''

    # The three layouts the old pattern covered, which must not regress...
    expect "the old pattern covered the primary checkout" \
        "2/2" "$(header_filter_match "$_old" "" /w/fastcached <<< "$_hdrs")"
    expect "the old pattern covered a .claude worktree" \
        "2/2" "$(header_filter_match "$_old" "" /w/fastcached/.claude/worktrees/w1 <<< "$_hdrs")"
    expect "the old pattern covered a wt-NNN checkout" \
        "2/2" "$(header_filter_match "$_old" "" /w/fastcached-wt-139 <<< "$_hdrs")"

    # ... and the one it did NOT, which is the ticket. Zero of two, and the
    # verdict names the first header that would have gone unanalysed.
    expect "the old pattern covered NO header in a lane worktree" \
        "0/2 src/FastCache/Core/Base64.hpp" \
        "$(header_filter_match "$_old" "" /w/fastcached-worktrees/lane-a <<< "$_hdrs")"

    # The replacement covers all four, which is the point of not naming a layout.
    for _root in /w/fastcached /w/fastcached/.claude/worktrees/w1 \
                 /w/fastcached-wt-139 /w/fastcached-worktrees/lane-a; do
        expect "the new pattern covers $_root" \
            "2/2" "$(header_filter_match "$_new" "$_exc" "$_root" <<< "$_hdrs")"
    done

    # And a dependency stays out -- in EVERY layout this project unpacks into, not
    # just the one on the machine running this. `_deps` is FetchContent's default
    # locally; `build.yml` sets `CPM_SOURCE_CACHE` to `.cache/CPM`, so CI's catch2
    # is at `.cache/CPM/<name>/<hash>/src/catch2/...`. Both contain `/src/`.
    _dep_deps="out/build/x/_deps/catch2-src/src/catch2/catch_test_macros.hpp"
    _dep_cpm=".cache/CPM/catch2/0123456789abcdef/src/catch2/catch_test_macros.hpp"
    for _dep in "$_dep_deps" "$_dep_cpm"; do
        expect "a dependency header is not matched: $_dep" \
            "0/1 $_dep" \
            "$(header_filter_match "$_new" "" /w/fastcached-worktrees/lane-a <<< "$_dep")"
    done

    # Pinned as the inequality it is. The first #1040 fix -- broad
    # `.*/src/.*` with `_deps` excluded -- is correct for one layout and takes the
    # other, which is why CI failed inside catch2 on a tree every local run called
    # clean. A case driven only against `_deps` passes under that bug.
    expect "the old broad+exclude pattern kept _deps out" \
        "0/1 $_dep_deps" \
        "$(header_filter_match '.*/src/.*' '.*/_deps/.*' /w/fastcached-worktrees/lane-a <<< "$_dep_deps")"
    expect "... and TOOK the CPM layout, which is the defect CI caught" \
        "1/1" \
        "$(header_filter_match '.*/src/.*' '.*/_deps/.*' /w/fastcached-worktrees/lane-a <<< "$_dep_cpm")"

    # A partial match is its own outcome: an analyser blind to 1 of 2 headers
    # still reports findings, so it looks like it is working.
    expect "a partial match names how many and which" \
        "1/2 src/apps/fastcached/Main.hpp" \
        "$(header_filter_match '.*/src/FastCache/.*' "" /w/fastcached <<< "$_hdrs")"

    # The reporter. `all` is the only arm that carries on -- asserted as the
    # STATUS beside the text, since a reporter that says the right words and
    # returns 0 lets the gate analyse nothing and call it clean.
    expect "full coverage is reported and the gate carries on" \
        "== clang-tidy header filter covers all 285 tracked headers|0" \
        "$(text="$(header_filter_report /w/.clang-tidy 285/285)"; printf '%s|%s' "$text" "$?")"
    expect "zero coverage refuses, naming #1040 and the lane layout" \
        "said|1" "$(report_says 'matches NONE of this tree' header_filter_report /w/.clang-tidy 0/285)"
    expect "partial coverage refuses, naming the count and an example" \
        "said|1" "$(report_says 'matches only 200 of this tree' header_filter_report /w/.clang-tidy '200/285 src/apps/x.hpp')"
    expect "a config with no HeaderFilterRegex refuses" \
        "said|1" "$(report_says 'names no HeaderFilterRegex' header_filter_report /w/.clang-tidy no-regex)"
    expect "a config that cannot be read refuses" \
        "said|1" "$(report_says 'is not readable' header_filter_report /w/.clang-tidy no-config)"
    expect "an empty header set refuses as the CHECK failing, not a clean tree" \
        "said|1" "$(report_says 'That is the CHECK failing' header_filter_report /w/.clang-tidy no-headers)"
    expect "an empty coverage verdict is refused BY NAME, never as coverage" \
        "said|1" "$(report_says 'produced NO verdict' header_filter_report /w/.clang-tidy '')"
    expect "a filter that would take _deps refuses, and says why coverage cannot see it" \
        "said|1" "$(report_says 'dependency trees are untracked' header_filter_report /w/.clang-tidy deps-leak)"

    # THE WIRING, which none of the above can see: the real `.clang-tidy` in this
    # very tree must cover every header this repository tracks. This is the case
    # that would have been RED on every lane worktree before #1040, and it is the
    # one that fires for layout number four without anybody editing this file.
    _live="$(header_filter_coverage "${repo_root}/.clang-tidy" "$repo_root")"
    expect "this tree's own .clang-tidy covers every tracked header" \
        "covers-all" \
        "$([[ "${_live%% *}" == *"/"* && "${_live%%/*}" == "${_live##*/}" ]] \
            && echo covers-all || echo "$_live")"

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

    # -----------------------------------------------------------------------
    # A run that never concluded (#584), driven as a pure function over staged log
    # text. No signal is sent and no gate is run: `gate_outcome` reads lines and
    # writes a word, so every outcome is exercised in milliseconds -- the same
    # split #501's renderer took, for the same reason. That a REAL signal produces
    # this shape is a separate claim and was checked separately, by sending one.
    #
    # @param 1 case name. @param 2 expected outcome. @param 3 log text.
    outcome_case() {
        expect "$1" "$2" "$(printf '%s' "$3" | gate_outcome)"
    }

    _started="$gate_start_marker -- pid 1, tree /w, commit abc1234, 2026-01-01T00:00:00Z"
    _legs="== gate legs:
==   gate-clang-debug   passed"

    outcome_case "a run that reached the green line passed" \
        "passed" "$_started
$_legs
$gate_passed_marker"

    outcome_case "a run that refused through fail() failed" \
        "failed" "$_started
$gate_failed_marker gate-clang-debug tests
$_legs"

    # The whole ticket: a run with a start marker and NO terminal line. Before the
    # marker existed this log and a truncated one were the same object.
    outcome_case "a killed run did not conclude, and says so" \
        "did-not-conclude" "$_started
== gate-clang-debug: build"

    # ... and its converse, which is the half that gets skipped: absence of the
    # negative is not the positive. A log with no marker at all cannot be
    # attributed to a run, so it is not a quiet `did-not-conclude` either.
    outcome_case "a log with no start marker holds no run to classify" \
        "no-gate-run" "$_legs
$gate_passed_marker"

    outcome_case "an empty log holds no run to classify" "no-gate-run" ""

    # A log file gets REUSED. A previous run's durable copy sitting where the
    # current one goes has already reported a failure that was fixed, told apart
    # only by an accident of timestamps -- so the LAST marker wins. `grep -c
    # PASSED` gets this one backwards, which is why it is a case.
    outcome_case "a green run followed by a killed one did not conclude" \
        "did-not-conclude" "$_started
$gate_passed_marker
$_started
== gate-clang-debug: build"

    outcome_case "a killed run followed by a green one passed" \
        "passed" "$_started
== gate-clang-debug: build
$_started
$_legs
$gate_passed_marker"

    # A verdict is a line, not a substring. A log quoting the pass marker inside a
    # message must not be read as one -- this script's own header quotes all three.
    outcome_case "a quoted marker mid-line is not a verdict" \
        "did-not-conclude" "$_started
==   note: a run printing $gate_passed_marker would be green"

    # The ticket's own requirement, asserted over the TABLE rather than restated:
    # `did-not-conclude` must not render as a red gate, so no two outcomes may
    # share an exit status. Two rows drifting onto one number is how that
    # distinction would be lost without anybody writing a bug.
    expect "every outcome has an exit status of its own" \
        "${#gate_outcomes[@]}" \
        "$(printf '%s\n' "${gate_outcomes[@]}" | cut -d'|' -f2 | sort -u | grep -c .)"
    # The STATUS field, not the whole row. Comparing rows compares the sentences
    # too, which differ whatever the statuses do -- so the check would have read
    # green for exactly the collapse it names. A signal that cannot be false in
    # the failing case is not evidence, and this one was caught by being watched
    # rather than by being read.
    expect "did-not-conclude and failed do not share a status" \
        "no" "$([[ "$(gate_outcome_status did-not-conclude)" == "$(gate_outcome_status failed)" ]] && echo yes || echo no)"

    # ... and no outcome may sit on the USAGE status, because `--classify=$LOG`
    # with an unset variable exits with it. A caller checking only the status would
    # read "you typed that wrong" as "the run was killed" -- two facts that are not
    # commensurable and are fixed in different places.
    expect "no outcome sits on the usage exit status" "0" \
        "$(printf '%s\n' "${gate_outcomes[@]}" | cut -d'|' -f2 | grep -c "^${gate_usage_status}$")"

    # An outcome with no row renders as unrecognised rather than as the nearest
    # plausible verdict -- #501's defect rebuilt inside #584's fix.
    expect "an outcome the table cannot name is refused, not guessed" \
        "$(gate_outcome_status unrecognised)" "$(gate_outcome_status invented-outcome)"
    expect "an outcome the table cannot name says it is a bug" \
        "yes" "$([[ "$(gate_outcome_row invented-outcome)" == *"UNRECOGNISED OUTCOME"* ]] && echo yes || echo no)"

    # The table drives the launcher and analyser decisions above, the `leg_pairs`
    # checks, and the whole gate below, so a row that stopped parsing would make
    # those vacuous while every one of them passed. The `leg_summary` checks are
    # deliberately NOT among them -- they pass their pairs explicitly, which is what
    # lets them state what the renderer does independently of what the gate runs.
    # An empty file list is a REFUSAL, not a formatting failure (#1064). Both
    # directions, because a guard that only checks the populated case passes under
    # the bug: the count is what separates "formatted 683 files" from "formatted
    # none of them", and the old run printed the same sentence either way.
    expect "an empty list refuses instead of reaching the formatter" "refuse 0" "$(format_plan "")"
    expect "a single file is offered to the formatter" "format 1" "$(format_plan "a.cpp")"
    expect "every file in the list is counted" "format 3" \
        "$(format_plan "$(printf 'a.cpp\nb.hpp\nc.h')")"

    # -----------------------------------------------------------------------
    # #1130: the skipped NAMES a leg reports, driven as a pure function over
    # staged ctest output. No gate is run and no ctest is invoked -- `skip_report`
    # reads text and writes text, so every outcome is exercised in milliseconds.
    # The same split `gate_outcome` took, for the same reason.
    #
    # Tabs, because that is what ctest indents these entries with, and the
    # stripping is part of what is under test.
    _skips="$(printf '100%% tests passed, 0 tests failed out of 10\n\nThe following tests did not run:\n\t 82 - rulebook-open-work-state (Skipped)\n\t334 - a second skipped case (Skipped)\n')"
    _clean="$(printf '100%% tests passed, 0 tests failed out of 10\n\nTotal Test time (real) =  91.02 sec\n')"

    expect "a leg that skipped tests reports them BY NAME, not as a count" \
        "== gate-clang-debug: 2 test(s) did not run, by name --
==   82 - rulebook-open-work-state (Skipped)
==   334 - a second skipped case (Skipped)" \
        "$(skip_report gate-clang-debug <<< "$_skips")"

    # The direction a guard nobody has watched accept never proves. Silence here
    # would be indistinguishable from the reporter not running at all, so the
    # clean case makes a POSITIVE statement.
    expect "a leg that skipped nothing says so, rather than saying nothing" \
        "== gate-clang-debug: no tests skipped" \
        "$(skip_report gate-clang-debug <<< "$_clean")"

    # The state this project keeps collapsing. Output with no ctest totals line is
    # not a run that skipped nothing -- it is a run whose skip data is UNKNOWN,
    # which is the shape a killed or interrupted leg leaves. Concluding "nothing
    # skipped" from the absence of a block, without first asserting that something
    # which must be present IS, is how the file-was-not-there state disappears.
    _unknown_head="== gate-clang-debug: SKIPS UNKNOWN -- this output carries no ctest totals line, so"
    expect "output with no totals line is UNKNOWN, never nothing-skipped" \
        "$_unknown_head" \
        "$(skip_report gate-clang-debug <<< "a truncated log that stops mid-run" | head -1)"
    expect "empty output is UNKNOWN too -- the shape a killed leg leaves" \
        "$_unknown_head" \
        "$(skip_report gate-clang-debug <<< "" | head -1)"

    # ctest prints `The following tests FAILED:` AFTER the did-not-run block, so a
    # reader that took the block to end of input would report those failures as
    # skips -- #1128's defect rebuilt inside its own remedy. Both spacings, since
    # a blank line between the blocks is what would make the naive version look
    # correct.
    for _gap in '\n' ''; do
        _mixed="$(printf '99%% tests passed, 1 tests failed out of 10\n\nThe following tests did not run:\n\t 82 - a skipped one (Skipped)\n%bThe following tests FAILED:\n\t100 - a failed one (Failed)\n' "$_gap")"
        expect "a FAILED block after the skips is not read as a skip (gap='${_gap}')" \
            "== gate-clang-debug: 1 test(s) did not run, by name --
==   82 - a skipped one (Skipped)" \
            "$(skip_report gate-clang-debug <<< "$_mixed")"
    done

    # THE TICKET'S OWN ARGUMENT, encoded rather than described: two runs with the
    # SAME COUNT and different SETS. A tally reports "1" for both and cannot tell
    # them apart, which is how a branch adding a root-conditional skip -- one that
    # never fires on an unprivileged gate -- leaves the number unchanged and a
    # reader concludes correctly for the wrong reason. The names differ, so the
    # report differs. One more skip is not my skip.
    _skip_a="$(printf '100%% tests passed, 0 tests failed out of 10\n\nThe following tests did not run:\n\t 82 - the skip that was always there (Skipped)\n')"
    _skip_b="$(printf '100%% tests passed, 0 tests failed out of 10\n\nThe following tests did not run:\n\t 91 - a skip this branch introduced (Skipped)\n')"
    expect "two runs skipping the SAME NUMBER of tests report differently" \
        "different" \
        "$([[ "$(skip_report leg <<< "$_skip_a")" == "$(skip_report leg <<< "$_skip_b")" ]] && echo same || echo different)"
    # And the control that makes the line above mean something: a report compared
    # with itself is the same, so the check is not simply always saying different.
    expect "... while a run compared with itself reports identically" \
        "same" \
        "$([[ "$(skip_report leg <<< "$_skip_a")" == "$(skip_report leg <<< "$_skip_a")" ]] && echo same || echo different)"

    # The pointer is supplementary and appears only when there is a directory to
    # name. It points at `LastTest.log`, which every run rewrites -- never at
    # `LastTestsDisabled.log`, which ctest leaves untouched when nothing skipped,
    # so after a clean run it still names the PREVIOUS run's skips.
    expect "the pointer names LastTest.log when a build directory is given" \
        "==   each one's own output is in out/build/gate-clang-debug/Testing/Temporary/LastTest.log" \
        "$(skip_report gate-clang-debug out/build/gate-clang-debug <<< "$_skips" | tail -1)"
    expect "and no pointer line is invented when there is no directory" \
        "==   334 - a second skipped case (Skipped)" \
        "$(skip_report gate-clang-debug <<< "$_skips" | tail -1)"

    expect "the preset table still has two rows" "2" "${#gate_presets[@]}"
    for row in "${gate_presets[@]}"; do
        case "${row#*|}" in
            tidy|no-tidy) ;;
            *) echo "SELF-TEST FAILED: unknown analyser column in '$row'" >&2
               self_test_failures=$((self_test_failures + 1)) ;;
        esac
    done

    echo "local-gate --self-test: ${self_test_ran} checks ran, ${self_test_failures} failed"
    [[ "$self_test_failures" -eq 0 ]] || exit 1

    # The interpreter is named for the same reason the gate names its analyser: this
    # script is written to bash 3.2 because macOS ships one, and "it passed on some
    # bash" is the shape of claim this whole file exists to stop making. A runner
    # with a newer bash first on PATH proves the checks and not the constraint, and
    # the log is the only place that difference is visible.
    echo "LOCAL GATE SELF-TEST PASSED (bash ${BASH_VERSION})${self_test_skipped:+ -- SKIPPED: $self_test_skipped}"
    exit 0
fi

# The start marker, and it is the FIRST thing a real run does -- before the analyser
# is resolved, before the formatter runs, before anything that can `fail`. A marker
# printed after the first refusal would be absent from exactly the runs it exists to
# describe.
#
# It names its own subject. A verdict that does not say what it is a verdict about
# has already cost this repository a run: a `/tmp` wipe left one gate's log where
# another's goes and it reported a failure that was already fixed. The pid is here
# because #584's cause is a name-matched kill across worktrees on a shared machine,
# and the tree is here because that is the field such a kill does not look at.
#
# The commit is read BEFORE the `clang-format -i` pass below, so `-dirty` describes
# the tree as the developer handed it over rather than one this gate rewrote and
# then measured.
gate_commit="$(git -C "$repo_root" rev-parse --short HEAD 2>/dev/null || echo unknown)"
git -C "$repo_root" diff --quiet HEAD 2>/dev/null || gate_commit="${gate_commit}-dirty"
echo "$gate_start_marker -- pid $$, tree $repo_root, commit $gate_commit, $(date -u '+%Y-%m-%dT%H:%M:%SZ')"

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

        # Asked once and HERE, beside the tool it is about, rather than per preset:
        # the header filter is a property of `.clang-tidy` and of where this
        # checkout lives, neither of which a preset changes. Only when some preset
        # actually tidies -- a gate whose configurations run no analyser has no
        # reason to have an opinion about which headers it would report on.
        report_or_refuse header_filter_report "${repo_root}/.clang-tidy" \
            "$(header_filter_coverage "${repo_root}/.clang-tidy" "$repo_root")"
        break
    fi
done

if [[ "$format" -eq 1 ]]; then
    formatter="clang-format-${tools_version}"
    command -v "$formatter" >/dev/null 2>&1 \
        || fail "$formatter not found; install it or pass --no-format"
    # Captured and counted BEFORE the formatter is asked anything, so the two ways
    # of having no files to format are told apart and neither is reported as a
    # formatting failure. `set -uo pipefail` carries no `-e`, so an errored
    # `git ls-files` and an empty-but-successful one used to land on the identical
    # `fail "clang-format"` -- a sentence that reads as mundane and actionable, so
    # the response is to run the formatter by hand, which succeeds, which then reads
    # as the gate being flaky (#1064).
    format_list="$(git ls-files '*.h' '*.hpp' '*.cpp')" \
        || fail "git ls-files failed in $(pwd), so no source was offered to $formatter and this tree's formatting is UNEXAMINED"

    format_verdict="$(format_plan "$format_list")"
    format_count="${format_verdict#* }"
    if [[ "${format_verdict%% *}" == "refuse" ]]; then
        fail "git ls-files matched no C++ source in $(pwd), so $formatter was never run and this tree's formatting is UNEXAMINED -- this is NOT a formatting failure. A linked worktree whose .git file holds an absolute 'gitdir:' is how this happens: the other git cannot resolve it and lists nothing, where the same tree lists hundreds of files once the pointer is relative (#1064)"
    fi

    printf '%s\n' "$format_list" | xargs "$formatter" -i --style=file \
        || fail "clang-format"

    # The COUNT, not just the tool: zero and 683 must not render the same sentence,
    # which is the whole defect one line up.
    echo "== formatted ${format_count} file(s) with $formatter"
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
    #
    # The directory each check reads is spelled from the two names this function
    # declares, `$build_dir` and the `$ninja` derived from it. #1031 was the shim
    # clause below spelling it `$dir`, which is `local` to `configure_reason` and
    # unbound here -- and the substitution swallowed the error, so a correct tree
    # was refused as a compiler cache with no name.
    report_or_refuse launcher_report "$preset" "$ninja" "$(launcher_verdict "$ninja")"

    # And the OTHER way a cache fronts a reference build, which the check above
    # cannot see: no launcher at all, because the resolved compiler IS the cache.
    # Asked separately rather than folded into `launcher_verdict`, because they
    # read different files and answer different questions -- and a single verdict
    # would have to collapse "a launcher is configured" and "the compiler is a
    # shim" into one word, which is how the first one came to stand for both.
    report_or_refuse shim_report "$preset" "$build_dir" "$(compiler_shim_verdict "$build_dir")"

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
        # A failing leg's skips matter as much as a passing one's -- more, since a
        # skip is one of the ways a case stops reporting on the thing that broke.
        skip_report "$preset" "$build_dir" < "$log"
        fail "$preset tests (full log: $log)"
    fi
    grep -E 'tests passed' "$log" | head -1
    # #1130: the totals line above is the one #1128 makes untrustworthy, so the
    # skipped NAMES go in the log beside it, before the log this read is deleted.
    skip_report "$preset" "$build_dir" < "$log"
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
