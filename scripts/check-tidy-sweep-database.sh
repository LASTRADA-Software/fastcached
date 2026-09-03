#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# The two places that document how to configure the tidy sweep's compile
# database must document what the `clang-tidy` CI job actually configures.
#
# ## Why this is a test and not a review note
#
# `scripts/tidy-sweep.sh` opens by saying it is "for reproducing what CI will
# say". A person who wants that reads its header and runs the line it prints.
# Until #454 that line was hand-rolled, and it had drifted:
#
#   * 29 compile flags apart on the same translation unit. The preset carries
#     `-Wall -Wextra -Wconversion -pedantic -Werror` and the `-Wno-…`
#     suppressions accompanying them; the hand-rolled line inherited
#     `PEDANTIC_COMPILER`/`PEDANTIC_COMPILER_WERROR` OFF and carried none.
#     `.clang-tidy` enables `clang-diagnostic-*`, so those flags decide what the
#     sweep REPORTS.
#   * Four first-party translation units absent from the database rather than
#     clean, because it omitted the two default-OFF app targets CI turns on.
#
# Both directions read like a clean tree, which is the whole problem: one wastes
# a cycle chasing a finding CI suppresses, the other ships to CI code a local
# sweep called clean. A tool that reports things that are not true trains people
# to ignore it, and `tidy-sweep.sh` exists precisely because a sweep that cannot
# prove it ran is worth nothing.
#
# Nothing else connects the workflow to the two files that document it. The
# rulebook already carried the target-set rule twelve lines above a code block
# that violated it, so "the comment says so" is demonstrably not enough.
#
# ## Derived, not tabulated
#
# CI's line is READ from `.github/workflows/build.yml` rather than restated here.
# A second copy of it would not be a cross-check, it would be a second thing to be
# wrong -- the reasoning `scripts/check-tsan-scope.cmake` records for reading the
# gate's tag expression instead of repeating it, and `check-merge-queue-contexts.sh`
# for deriving its context-to-job mapping.
#
# ## What it compares, and what it deliberately ignores
#
# The preset name and the set of `-D` options, as a SET: order is not meaningful
# and neither is line wrapping. `-B` is ignored as a value but REQUIRED to be
# present in the documented lines -- it is what keeps the sweep's database out of
# the `out/build/clang-debug` tree `local-gate.sh` builds, and a documented line
# without it silently turns `ENABLE_TIDY` and module scanning off in that build.
# CI needs no `-B` because its runner has no such tree to protect.
#
# ## Usage
#
#   scripts/check-tidy-sweep-database.sh              # check the tree
#   scripts/check-tidy-sweep-database.sh --self-test  # prove the check bites
#
# bash 3.2: macOS still ships a 2007 /bin/bash and this runs in the default ctest
# set on every platform CI builds. No `mapfile`, no `declare -A`, no `${var^^}`.

set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

Workflow=".github/workflows/build.yml"
SweepScript="scripts/tidy-sweep.sh"
RulesDoc=".agent/rules/build-and-toolchain.md"

problems=0
Fail() { echo "  FAIL: $*" >&2; problems=$((problems + 1)); }

# ---------------------------------------------------------------------------
# The `run:` body of a named step inside a named job, folded to one line.
#
# Indentation is the discriminator, and it is reliable because this is the
# repository's own workflow: a job key is two spaces, a step's `- name:` is six,
# its `run:` eight, and a folded scalar's continuation ten.
#
# It deliberately carries NO `/^[ \t]*#/ { next }` rule, which both sibling
# walkers (`check-merge-queue-contexts.sh`, `check-gated-jobs.sh`) do have. Inside
# a `>-` block scalar a `#` is literal shell payload, not a YAML comment, so
# skipping those lines would silently truncate the very `run:` body being read --
# and a truncated command still compares as a command. Stated because the omission
# looks like an inconsistency, and tidying the three walkers into agreement is
# exactly how it would get "fixed" back into a defect.
ExtractWorkflowConfigure() {
    awk -v job="$2" -v step="$3" '
        /^jobs:[ \t]*$/                  { inJobs = 1; next }
        inJobs && /^[^ \t]/              { inJobs = 0 }
        !inJobs                          { next }
        /^  [A-Za-z0-9_-]+:[ \t]*$/      { key = $0
                                           sub(/^  /, "", key)
                                           sub(/:[ \t]*$/, "", key)
                                           inJob = (key == job)
                                           next }
        !inJob                           { next }
        /^      - name:[ \t]*/           { name = $0
                                           sub(/^      - name:[ \t]*/, "", name)
                                           gsub(/^"|"$/, "", name)
                                           pending = (name == step)
                                           collecting = 0
                                           next }
        pending && /^        run:[ \t]*/ { collecting = 1
                                           rest = $0
                                           sub(/^        run:[ \t]*/, "", rest)
                                           # `>-` / `|` introduce a block scalar;
                                           # anything else is the body itself.
                                           if (rest !~ /^(>-?|\|-?)$/) printf "%s ", rest
                                           next }
        collecting && /^[ \t]*$/         { collecting = 0; next }
        collecting && /^          /      { line = $0
                                           sub(/^[ \t]+/, "", line)
                                           printf "%s ", line
                                           next }
        collecting                       { collecting = 0 }
    ' "$1"
}

# ---------------------------------------------------------------------------
# The documented configure line out of a file that documents one, folded to one
# line. Anchored on `cmake --preset` plus `-B`, which is the sweep's database and
# nothing else -- the rulebook shows plenty of other `cmake --preset` lines, and
# an anchor that matched those would compare the wrong thing and pass.
#
# Continuation is a trailing backslash. A leading `#` (the script's header) or
# nothing (the rulebook's fenced block) is stripped either way.
ExtractDocumentedConfigure() {
    awk '
        function strip(s) {
            sub(/^[ \t]*#?[ \t]*/, "", s)
            return s
        }
        !started && /cmake --preset/ && /-B / { started = 1
                                                line = strip($0)
                                                more = (line ~ /\\$/)
                                                sub(/\\$/, "", line)
                                                printf "%s ", line
                                                if (!more) exit
                                                next }
        started                              { line = strip($0)
                                               more = (line ~ /\\$/)
                                               sub(/\\$/, "", line)
                                               printf "%s ", line
                                               if (!more) exit }
    ' "$1"
}

# ---------------------------------------------------------------------------
# A configure command reduced to what is being compared: the preset name, and the
# `-D` options as a sorted set. Prints `preset <name>` then one `-D…` per line.
#
# Everything else is dropped deliberately -- `cmake`, `-S`, `-B`, `-G`, wrapping
# and ordering are not the contract.
NormaliseConfigure() {
    printf '%s\n' "$1" | tr ' \t' '\n\n' | awk '
        $0 == ""            { next }
        takePreset          { print "preset " $0; takePreset = 0; next }
        $0 == "--preset"    { takePreset = 1; next }
        /^-D/               { print }
    ' | sort
}

# Does this command name a `-B`?
HasBinaryDirOption() {
    case " $1 " in
        *" -B "*) return 0 ;;
        *) return 1 ;;
    esac
}

# ---------------------------------------------------------------------------
# Compare one documented line against CI's. Prints its own verdict; returns
# non-zero when they disagree, so the self-test can drive it directly.
CompareConfigure() {
    label="$1"
    ciCommand="$2"
    docCommand="$3"
    requireBinaryDir="$4"

    if [[ -z "${docCommand// /}" ]]; then
        echo "  FAIL: $label documents no \`cmake --preset … -B …\` line at all" >&2
        return 1
    fi

    ciNormal="$(NormaliseConfigure "$ciCommand")"
    docNormal="$(NormaliseConfigure "$docCommand")"

    bad=0
    if [[ "$ciNormal" != "$docNormal" ]]; then
        echo "  FAIL: $label does not document what the clang-tidy job configures." >&2
        echo "        A person following it gets a database whose flags and target set differ from CI's," >&2
        echo "        so the sweep reports findings CI suppresses and misses findings CI raises." >&2
        # `diff` on process substitutions rather than a pipeline into `grep`:
        # under `pipefail` a short-circuiting reader kills the producer with
        # SIGPIPE and the pipeline reports the PRODUCER's status.
        diff <(printf '%s\n' "$ciNormal") <(printf '%s\n' "$docNormal") \
            | sed 's/^/          /' >&2 || true
        bad=1
    fi

    if [[ "$requireBinaryDir" == "require-B" ]] && ! HasBinaryDirOption "$docCommand"; then
        echo "  FAIL: $label documents no \`-B\`, so following it would configure the preset into" >&2
        echo "        out/build/clang-debug and turn ENABLE_TIDY and module scanning off in the tree" >&2
        echo "        local-gate.sh builds." >&2
        bad=1
    fi

    if [[ $bad -eq 0 ]]; then
        echo "ok: $label documents the clang-tidy job's Configure step"
    fi
    return $bad
}

# ---------------------------------------------------------------------------
# Self-test: the check must be shown to BITE. A guard that cannot be made to fail
# is indistinguishable from one that passes because nothing is wrong.
SelfTest() {
    ci='cmake --preset clang-debug -DENABLE_TIDY=OFF -DCMAKE_CXX_SCAN_FOR_MODULES=OFF -DFASTCACHED_ENABLE_TLS=ON'
    failures=0

    Expect() {
        want="$1"; name="$2"; doc="$3"; mode="${4:-require-B}"
        if CompareConfigure "synthetic" "$ci" "$doc" "$mode" >/dev/null 2>&1; then
            got=agree
        else
            got=differ
        fi
        if [[ "$got" == "$want" ]]; then
            echo "ok: self-test '$name' -> $got"
        else
            echo "  FAIL: self-test '$name' expected $want, got $got" >&2
            failures=$((failures + 1))
        fi
    }

    Expect agree  "identical but for -B and wrapping" \
        'cmake --preset clang-debug -B out/build/tidy22 -DENABLE_TIDY=OFF -DCMAKE_CXX_SCAN_FOR_MODULES=OFF -DFASTCACHED_ENABLE_TLS=ON'
    Expect agree  "same options in a different order" \
        'cmake --preset clang-debug -B out/build/tidy22 -DFASTCACHED_ENABLE_TLS=ON -DENABLE_TIDY=OFF -DCMAKE_CXX_SCAN_FOR_MODULES=OFF'
    Expect differ "a different preset" \
        'cmake --preset clang-release -B out/build/tidy22 -DENABLE_TIDY=OFF -DCMAKE_CXX_SCAN_FOR_MODULES=OFF -DFASTCACHED_ENABLE_TLS=ON'
    Expect differ "a -D CI passes that the documentation drops" \
        'cmake --preset clang-debug -B out/build/tidy22 -DENABLE_TIDY=OFF -DCMAKE_CXX_SCAN_FOR_MODULES=OFF'
    Expect differ "a -D the documentation adds that CI does not pass" \
        'cmake --preset clang-debug -B out/build/tidy22 -DENABLE_TIDY=OFF -DCMAKE_CXX_SCAN_FOR_MODULES=OFF -DFASTCACHED_ENABLE_TLS=ON -DFASTCACHED_BUILD_NODE=OFF'
    Expect differ "a changed -D VALUE, not just a changed name" \
        'cmake --preset clang-debug -B out/build/tidy22 -DENABLE_TIDY=ON -DCMAKE_CXX_SCAN_FOR_MODULES=OFF -DFASTCACHED_ENABLE_TLS=ON'
    Expect differ "no -B, which would clobber the clang-debug tree" \
        'cmake --preset clang-debug -DENABLE_TIDY=OFF -DCMAKE_CXX_SCAN_FOR_MODULES=OFF -DFASTCACHED_ENABLE_TLS=ON'
    Expect differ "nothing documented at all" ''
    # And the hand-rolled line #454 removed, which is the actual regression: it
    # names no preset, so it agrees with nothing.
    Expect differ "the pre-#454 hand-rolled line" \
        'cmake -S . -B out/build/tidy22 -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_CXX_SCAN_FOR_MODULES=OFF -DFASTCACHED_ENABLE_TLS=ON'

    # -----------------------------------------------------------------------
    # The WORKFLOW-side extractor, which the cases above do not touch at all.
    #
    # It is the half that decides what everything else is compared AGAINST, and
    # its failure mode is an empty string -- which the main path treats as fatal
    # precisely because it would otherwise compare nothing against nothing. But
    # "it returns empty when it should" and "it returns the RIGHT step when there
    # are several" are different questions, and build.yml really does have a
    # `Configure` step in more than one job.
    #
    # The added steps are not hypothetical: a queued pull request adds an `id:`
    # and a further step to this very job, so a reader must be able to see that
    # neither moves the answer.
    workflowDir="$(mktemp -d)"
    trap 'rm -rf "$workflowDir"' EXIT

    ExpectWorkflow() {
        name="$1"; want="$2"; content="$3"
        printf '%s\n' "$content" > "$workflowDir/wf.yml"
        got="$(ExtractWorkflowConfigure "$workflowDir/wf.yml" "clang-tidy" "Configure")"
        # Collapse trailing whitespace: the extractor joins with a trailing space.
        got="${got%"${got##*[![:space:]]}"}"
        if [[ "$got" == "$want" ]]; then
            echo "ok: self-test workflow '$name'"
        else
            echo "  FAIL: self-test workflow '$name'" >&2
            echo "        wanted: [$want]" >&2
            echo "        got:    [$got]" >&2
            failures=$((failures + 1))
        fi
    }

    ExpectWorkflow "the clang-tidy job's step, not another job's" \
        'cmake --preset clang-debug -DENABLE_TIDY=OFF' \
        'jobs:
  linux:
    name: "Linux"
    steps:
      - name: "Configure"
        run: >-
          cmake --preset clang-release -DSOMETHING=ELSE
  clang-tidy:
    name: "clang-tidy"
    steps:
      - name: "Configure"
        run: >-
          cmake --preset clang-debug -DENABLE_TIDY=OFF
  windows:
    name: "Windows"
    steps:
      - name: "Configure"
        run: >-
          cmake --preset cl-debug'

    ExpectWorkflow "steps and an id: added around it" \
        'cmake --preset clang-debug -DENABLE_TIDY=OFF -DFASTCACHED_ENABLE_TLS=ON' \
        'jobs:
  clang-tidy:
    name: "clang-tidy"
    steps:
      - uses: actions/checkout@v4
      - name: "Install build tools"
        run: sudo apt-get update
      - name: "Configure"
        run: >-
          cmake --preset clang-debug -DENABLE_TIDY=OFF
          -DFASTCACHED_ENABLE_TLS=ON
      - name: "Sweep"
        id: sweep
        run: scripts/tidy-sweep.sh'

    ExpectWorkflow "an unfolded single-line run:" \
        'cmake --preset clang-debug -DENABLE_TIDY=OFF' \
        'jobs:
  clang-tidy:
    steps:
      - name: "Configure"
        run: cmake --preset clang-debug -DENABLE_TIDY=OFF'

    # The fatal direction: the job or the step going away must yield EMPTY, which
    # the main path refuses rather than passing vacuously.
    ExpectWorkflow "the Configure step renamed away" '' \
        'jobs:
  clang-tidy:
    steps:
      - name: "Set up"
        run: >-
          cmake --preset clang-debug -DENABLE_TIDY=OFF'

    ExpectWorkflow "the job renamed away" '' \
        'jobs:
  tidy:
    steps:
      - name: "Configure"
        run: >-
          cmake --preset clang-debug -DENABLE_TIDY=OFF'

    if [[ $failures -gt 0 ]]; then
        echo "check-tidy-sweep-database --self-test: $failures case(s) wrong" >&2
        exit 1
    fi
    echo "check-tidy-sweep-database --self-test: the comparison and the workflow extraction both bite"
}

if [[ "${1:-}" == "--self-test" ]]; then
    SelfTest
    exit 0
fi

# ---------------------------------------------------------------------------
for file in "$Workflow" "$SweepScript" "$RulesDoc"; do
    [[ -f "$file" ]] || { Fail "$file does not exist"; }
done
[[ $problems -eq 0 ]] || { echo "check-tidy-sweep-database: $problems problem(s)" >&2; exit 1; }

CiConfigure="$(ExtractWorkflowConfigure "$Workflow" "clang-tidy" "Configure")"

# A silently empty extraction would compare nothing against nothing and pass --
# the exact shape of failure this repository keeps paying for, so it is fatal.
if [[ -z "${CiConfigure// /}" ]]; then
    Fail "could not read the clang-tidy job's \`Configure\` step out of $Workflow; the check would otherwise compare nothing against nothing and pass"
elif [[ "$CiConfigure" != *"--preset"* ]]; then
    Fail "the clang-tidy job's \`Configure\` step in $Workflow no longer names a --preset: $CiConfigure"
else
    echo "ok: read the clang-tidy job's Configure step from $Workflow"
fi
[[ $problems -eq 0 ]] || { echo "check-tidy-sweep-database: $problems problem(s)" >&2; exit 1; }

CompareConfigure "$SweepScript" "$CiConfigure" \
    "$(ExtractDocumentedConfigure "$SweepScript")" require-B || problems=$((problems + 1))
CompareConfigure "$RulesDoc" "$CiConfigure" \
    "$(ExtractDocumentedConfigure "$RulesDoc")" require-B || problems=$((problems + 1))

if [[ $problems -gt 0 ]]; then
    echo "check-tidy-sweep-database: $problems problem(s); a local sweep following the documentation would not agree with CI" >&2
    exit 1
fi
echo "check-tidy-sweep-database: both documented lines match the clang-tidy job's Configure step"
