#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Drive `check-tidy-blind-spots.sh` against staged trees and assert WHICH refusal each
# gets, not merely that it refused. A check that refuses everything passes a
# "did it refuse" test while catching nothing, and that is the shape #668's gate
# selftest was written to avoid; the same rule applies here.
#
# It stages a synthetic compile database and synthetic objects rather than using the
# real build, so it costs no build and cannot be quietly weakened by the tree changing
# underneath it.
set -uo pipefail

# Defaults to the sibling check. Locating a SIBLING SCRIPT by `BASH_SOURCE` is sound
# -- they ship together -- and is not the trap the check itself avoids, which is
# deriving the TREE UNDER TEST from where the script happens to live.
check="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/check-tidy-blind-spots.sh}"
[[ -f $check ]] || { echo "CMake Error: no check script at '$check'"; exit 1; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cases=0
failures=0

# A fake `nm` whose answer is driven by the object's own contents, so a staged object
# can be made to look analysed or blind without a compiler.
cat > "$work/nm" <<'NM'
#!/bin/sh
# args: --defined-only <object>
for a in "$@"; do case "$a" in --*) ;; *) obj="$a" ;; esac; done
cat "$obj" 2>/dev/null
NM
chmod 0755 "$work/nm"

# @param 1 case name  @param 2 expect pass|refuse  @param 3 substring the output must carry
run_case() {
    name="$1"; want="$2"; wantMsg="$3"
    cases=$((cases + 1))
    out="$(NM="$work/nm" FASTCACHED_SOURCE_DIR="$work" bash "$check" "$work/build" 2>&1)"
    rc=$?
    got=pass; [[ $rc -ne 0 ]] && got=refuse
    if [[ $got != "$want" ]]; then
        echo "CMake Error: check-tidy-blind-spots-selftest: case '$name' expected $want, got $got" >&2
        echo "$out" | sed 's/^/    /' >&2
        failures=$((failures + 1))
        return
    fi
    if ! grep -q -- "$wantMsg" <<< "$out"; then
        echo "CMake Error: check-tidy-blind-spots-selftest: case '$name' gave the right verdict for the wrong reason -- expected '$wantMsg'" >&2
        echo "$out" | sed 's/^/    /' >&2
        failures=$((failures + 1))
    fi
}

# Stage a tree: $1 = table contents, then pairs of <relpath> <symbol-count>.
sanflag="-fsanitize=address"

stage() {
    rm -rf "$work/build" "$work/src" "$work/scripts" "$work/.github"
    mkdir -p "$work/build" "$work/scripts" "$work/.github/workflows"
    # A workflow the leg-derivation can read. Staged rather than copied from the real
    # tree, so a case can name a leg that does or does not exist without depending on
    # what CI happens to run today.
    #
    # EACH LEG IS SPELLED THE WAY THE REAL ONE IS. This fake used to give both jobs
    # `--ci`, and the real Windows leg sweeps with `--only=` -- so the fake was more
    # PERMISSIVE than the thing it stood for, every case here passed, and CI refused
    # a correct tree by name. A staged workflow that agrees with the derivation
    # rather than with the workflow tests the derivation against itself.
    printf 'jobs:\n  clang-tidy:\n    steps:\n      - run: scripts/tidy-sweep.sh --ci\n  clang-tidy-windows:\n    steps:\n      - run: bash scripts/tidy-sweep.sh --self-test\n      - run: bash scripts/tidy-sweep.sh "--only=$WORKFILE"\n' \
        > "$work/.github/workflows/build.yml"
    printf '%s\n' "$1" > "$work/scripts/tidy-blind-spots.txt"
    shift
    entries=""
    while [[ $# -gt 0 ]]; do
        rel="$1"; count="$2"; shift 2
        mkdir -p "$work/$(dirname "$rel")" "$work/build/obj/$(dirname "$rel")"
        : > "$work/$rel"
        obj="$work/build/obj/$rel.o"
        : > "$obj"
        i=0; while [[ $i -lt $count ]]; do echo "0000 T sym$i" >> "$obj"; i=$((i + 1)); done
        [[ -n $entries ]] && entries="$entries,"
        # The staged database carries `-fsanitize=address` because the check requires
        # it: the empty-object signature is the sanitizer's, and a staged tree that
        # omitted it would exercise the precondition instead of the comparison.
        entries="$entries{\"directory\":\"$work/build\",\"command\":\"c++ $sanflag -c $work/$rel -o obj/$rel.o\",\"file\":\"$work/$rel\"}"
    done
    printf '[%s]' "$entries" > "$work/build/compile_commands.json"
}

ctrl="src/FastCache/Net/EpollSocket.cpp"

# Everything blind is listed, and the control is analysed: the only passing shape.
stage "$(printf 'src/FastCache/Net/IocpSocket.cpp\tnone\tguarded\n')" \
    "$ctrl" 50 "src/FastCache/Net/IocpSocket.cpp" 2
run_case accounted pass "1 translation unit(s) contribute nothing in this configuration, all accounted for"

# A TU goes blind with no row. The hole this check exists to find.
stage "$(printf '# empty\n')" "$ctrl" 50 "src/FastCache/Net/IocpSocket.cpp" 2
run_case unlisted refuse "is analysed by nothing and is not in scripts/tidy-blind-spots.txt"

# A row that is no longer true. A stale exemption hides the next real one.
stage "$(printf 'src/FastCache/Net/IocpSocket.cpp\tnone\tguarded\n')" \
    "$ctrl" 50 "src/FastCache/Net/IocpSocket.cpp" 40
run_case stale refuse "is listed as analysed by nothing, but it is analysed now"

# A SWAP: one unit leaves the blind set, another joins, and the COUNT is unchanged.
# The reason this case exists is that a check reporting only a number would read 1 -> 1
# and call it unchanged, while the set it describes is a different set. Both halves
# must be named.
stage "$(printf 'src/FastCache/Net/IocpSocket.cpp\tnone\tguarded\n')" \
    "$ctrl" 50 "src/FastCache/Net/IocpSocket.cpp" 40 "src/FastCache/Net/KqueueSocket.cpp" 2
run_case swap refuse "KqueueSocket.cpp' is analysed by nothing and is not in"
stage "$(printf 'src/FastCache/Net/IocpSocket.cpp\tnone\tguarded\n')" \
    "$ctrl" 50 "src/FastCache/Net/IocpSocket.cpp" 40 "src/FastCache/Net/KqueueSocket.cpp" 2
run_case swap_names_both refuse "IocpSocket.cpp' is listed as analysed by nothing, but it is analysed now"

# A row naming a file that is gone. Distinguished from `stale`, because the fix is
# different: one is a deletion, the other is a file that grew an analyser.
stage "$(printf 'src/FastCache/Net/Vanished.cpp\tnone\tguarded\n')" "$ctrl" 50
run_case vanished refuse "which does not exist"

# The instrument itself broken: `nm` answers nothing, so EVERY TU reads as blind.
# Without the positive control this reports the whole tree as a discovery.
stage "$(printf '# empty\n')" "$ctrl" 0 "src/FastCache/Net/IocpSocket.cpp" 0
run_case broken_instrument refuse "positive control"

# The precondition itself, which is otherwise unwatched: without ASan the two-symbol
# signature is not the sanitizer's and an empty unit is indistinguishable from one
# defining only inline entities. Measured on the real tree as 11 against 26.
sanflag=""
stage "$(printf '# empty\n')" "$ctrl" 50 "src/FastCache/Net/IocpSocket.cpp" 2
run_case no_sanitizer refuse "no AddressSanitizer"
sanflag="-fsanitize=address"

# The reached-by column. A row may name a leg that EXISTS, or `none`; naming one
# that does not is a claim of coverage nothing provides, and it fails silently --
# the unit stays listed and the table stays plausible while nobody analyses it.
stage "$(printf 'src/FastCache/Net/IocpSocket.cpp\tclang-tidy-windows\tguarded\n')" \
    "$ctrl" 50 "src/FastCache/Net/IocpSocket.cpp" 2
run_case reached_by_real_leg pass "contribute nothing in this configuration"

stage "$(printf 'src/FastCache/Net/IocpSocket.cpp\tclang-tidy-solaris\tguarded\n')" \
    "$ctrl" 50 "src/FastCache/Net/IocpSocket.cpp" 2
run_case reached_by_absent_leg refuse "which is not a clang-tidy leg"

# And the number #858 is about is reported on every run, not only on a refusal.
stage "$(printf 'src/FastCache/Net/IocpSocket.cpp\tnone\tguarded\n')" \
    "$ctrl" 50 "src/FastCache/Net/IocpSocket.cpp" 2
run_case reports_unreached_count pass "reached by NO analyser leg"

# WHICH SPELLINGS COUNT AS A LEG. The staged workflow above already exercises the
# passing direction -- `clang-tidy-windows` sweeps with `--only=` and nothing else,
# so every `clang-tidy-windows` case above is red under a detector that knows only
# `--ci`. These three are the directions a staged tree cannot reach.
stageWorkflow() {
    rm -rf "$work/build" "$work/src" "$work/scripts" "$work/.github"
    mkdir -p "$work/build" "$work/scripts" "$work/.github/workflows"
    printf '%b' "$1" > "$work/.github/workflows/build.yml"
    printf 'src/FastCache/Net/IocpSocket.cpp\tclang-tidy-windows\tguarded\n' \
        > "$work/scripts/tidy-blind-spots.txt"
    entries=""
    for pair in "$ctrl:50" "src/FastCache/Net/IocpSocket.cpp:2"; do
        rel="${pair%:*}"; count="${pair##*:}"
        mkdir -p "$work/$(dirname "$rel")" "$work/build/obj/$(dirname "$rel")"
        : > "$work/$rel"
        obj="$work/build/obj/$rel.o"
        : > "$obj"
        i=0; while [[ $i -lt $count ]]; do echo "0000 T sym$i" >> "$obj"; i=$((i + 1)); done
        [[ -n $entries ]] && entries="$entries,"
        entries="$entries{\"directory\":\"$work/build\",\"command\":\"c++ -fsanitize=address -c $work/$rel -o obj/$rel.o\",\"file\":\"$work/$rel\"}"
    done
    printf '[%s]' "$entries" > "$work/build/compile_commands.json"
}

# A mode the check cannot classify REFUSES. Assumed non-sweeping it would make a
# real leg invisible and take every row naming it down with it -- which is the
# defect above, one release later.
stageWorkflow 'jobs:\n  clang-tidy:\n    steps:\n      - run: scripts/tidy-sweep.sh --ci\n  clang-tidy-windows:\n    steps:\n      - run: bash scripts/tidy-sweep.sh --dry-run\n'
run_case unknown_sweep_mode refuse "does not recognise"

# A COMMENT IS NOT A CALL SITE. `build.yml` really does carry the string
# `scripts/tidy-sweep.sh --self-test` inside a comment, and a scan that reads
# comments attributes an invocation to whatever job it is discussed in.
stageWorkflow 'jobs:\n  clang-tidy:\n    steps:\n      - run: scripts/tidy-sweep.sh --ci\n  clang-tidy-windows:\n    steps:\n      # bash scripts/tidy-sweep.sh --only=x is what the other lane does\n      - run: echo nothing\n'
run_case leg_claimed_by_a_comment refuse "which is not a clang-tidy leg"

# The pair that keeps the one above from passing for the wrong reason: the SAME
# invocation, uncommented, IS a leg.
stageWorkflow 'jobs:\n  clang-tidy:\n    steps:\n      - run: scripts/tidy-sweep.sh --ci\n  clang-tidy-windows:\n    steps:\n      - run: bash scripts/tidy-sweep.sh --only=x\n'
run_case leg_sweeping_with_only pass "contribute nothing in this configuration"

echo "check-tidy-blind-spots-selftest: ran $cases case(s), $failures failure(s)"
[[ $failures -eq 0 ]] || exit 1
