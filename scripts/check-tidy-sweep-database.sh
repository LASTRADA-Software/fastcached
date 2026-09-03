#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Every place that documents how to configure the tidy sweep's compile database
# must document what the `clang-tidy` CI job actually configures.
#
# ## Why this is a test and not a review note
#
# `scripts/tidy-sweep.sh` opens by saying it is "for reproducing what CI will
# say". A person who wants that reads its header and runs the line it prints.
# Until #454 that line was hand-rolled and had drifted -- in compile flags and in
# target set, in both directions at once, so a local sweep both invented findings
# CI suppresses and stayed silent on findings CI raises. **The measurements are in
# `.agent/rules/build-and-toolchain.md`** beside the configure line itself; they
# are deliberately not restated here, because a figure restated in four places is
# a figure that gets updated in one.
#
# Nothing else connects the workflow to the files that document it, and the
# rulebook had carried the target-set rule twelve lines above a code block that
# violated it -- so "the comment says so" is demonstrably not enough.
#
# ## Derived, not tabulated -- twice
#
# CI's line is READ from `.github/workflows/build.yml` rather than restated here.
# A second copy would not be a cross-check, it would be a second thing to be wrong
# -- the reasoning `scripts/check-tsan-scope.cmake` records for reading the TSan
# gate's tag expression instead of repeating it.
#
# And WHICH files document a line is discovered by scanning for the sweep's own
# database directory, never by a list. #492's rule: a list is exact about the
# files it knows and silent about the ones it does not, and silence reads
# identically to complete coverage. A guide or a README growing a third copy of
# the configure line is exactly the drift this check exists for, and a two-entry
# list would pass green forever while it rotted.
#
# One file is named, and only one: `scripts/tidy-sweep.sh` must be among the sites
# found. That is not a tabulated set, it is the assertion that the header this
# check was written for still documents anything at all -- a scan alone would go
# quietly green if that header lost its configure line entirely.
#
# ## What it compares, and what it deliberately ignores
#
# The preset name and the `-D` options as a SET, values included: order and line
# wrapping are not the contract. `-B` is ignored as a value here because the scan
# anchor already pins it -- a documented line that does not name the sweep's
# database directory is not found as a site at all, and `tidy-sweep.sh` defaults
# `DB` to that same directory. CI needs no `-B` because its runner has no
# `out/build/clang-debug` tree to protect.
#
# ## What it cannot assert, stated rather than left as an apparent omission
#
# Command equality is not database equality. `clang-debug` names bare `clang++`,
# and `cmake/portable/PedanticCompiler.cmake` adds each warning flag only if
# `check_cxx_compiler_flag` accepts it -- so following the documentation perfectly
# on an older clang still yields a flag set that can differ from CI's, along the
# very `clang-diagnostic-*` axis #454 is about. The rulebook pins the ANALYSER's
# version; the CONFIGURING compiler is whatever `PATH` offers. Unchanged by #454
# and not made worse by it, but not closed by this check either.
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

# The sweep's database directory, which is what makes a `cmake --preset` line a
# documented TIDY SWEEP line rather than any other preset invocation. It is
# `tidy-sweep.sh`'s own `DB` default.
DatabaseDir="out/build/tidy22"

# The one site named rather than discovered; see the header.
RequiredSite="scripts/tidy-sweep.sh"

# This file quotes configure lines -- correct ones and deliberately wrong ones --
# as self-test fixtures, so scanning it would compare the check against its own
# counter-examples.
SelfPath="scripts/check-tidy-sweep-database.sh"

problems=0
Fail() { echo "  FAIL: $*" >&2; problems=$((problems + 1)); }

# Report the tally and leave. Spelled once: three exit points printing the same
# sentence three ways is how the wording drifts.
Abort() {
    echo "check-tidy-sweep-database: $problems problem(s); $1" >&2
    exit 1
}

# ---------------------------------------------------------------------------
# The `run:` body of a named step inside a named job, folded to one line.
#
# Indentation is the discriminator, and it is reliable because this is the
# repository's own workflow: a job key is two spaces, a step's `- name:` is six,
# its `run:` eight, and a folded scalar's continuation ten. The job scoping is
# load-bearing rather than defensive -- `build.yml` carries a step named
# `Configure` in a dozen jobs.
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
# line. Anchored on `cmake --preset` plus the sweep's database directory, which
# together mean this database and nothing else -- the rulebook shows several other
# `cmake --preset` lines, and an anchor matching those would compare the wrong
# thing and pass.
#
# Continuation is a trailing backslash. A leading `#` (a script header) or nothing
# (a fenced markdown block) is stripped either way.
ExtractDocumentedConfigure() {
    awk -v dir="$2" '
        function strip(s) {
            sub(/^[ \t]*#?[ \t]*/, "", s)
            return s
        }
        !started && /cmake --preset/ && index($0, "-B " dir) { started = 1 }
        started { line = strip($0)
                  more = (line ~ /\\$/)
                  sub(/\\$/, "", line)
                  # Trim what the backslash left behind, so the folded result is
                  # single-spaced rather than carrying the wrapping as blanks.
                  sub(/[ \t]+$/, "", line)
                  printf "%s ", line
                  if (!more) exit }
    ' "$1"
}

# ---------------------------------------------------------------------------
# Every file documenting a configure line for this database. Prints `mode:<how>`
# first, then one path per line.
#
# A scan rather than a list, per the header. `git ls-files` is the preferred
# source and not merely the faster one: it is the only one that answers for THIS
# checkout. A configured build tree carries the string in its own generated
# files, and `.claude/worktrees/<name>/` holds entire sibling checkouts in the
# primary clone -- both are gitignored, so the index excludes them by
# construction, while a walk has to be told and can only be told about the ones
# somebody thought of.
#
# The fallback exists for an exported tarball with no index. It says which mode
# produced the answer because the two are not interchangeable and a check that
# cannot say which one it used is a check reporting on a file set nobody can
# reconstruct -- `check-catch-skip-return-code` had six green self-test cases
# that all exercised the FALLBACK while CI exercised git, and neither side
# contradicted the other. Both modes are self-tested here for that reason.
#
# This file is always excluded: its self-test fixtures quote wrong lines on
# purpose, so scanning it would compare the check against its own counter-examples.
DocumentationSites() {
    local tracked matches
    if tracked="$(git ls-files 2>/dev/null)" && [[ -n "$tracked" ]]; then
        echo "mode:git ls-files"
        matches="$(printf '%s\n' "$tracked" | tr '\n' '\0' \
                   | xargs -0 grep -l -e "-B ${DatabaseDir}" -- 2>/dev/null || true)"
    else
        echo "mode:directory walk (no git index)"
        matches="$(grep -rl --exclude-dir=.git --exclude-dir=.claude \
                        --exclude-dir=out --exclude-dir=_deps \
                        -e "-B ${DatabaseDir}" . 2>/dev/null | sed 's|^\./||' || true)"
    fi
    printf '%s\n' "$matches" | grep -v "^${SelfPath}$" | grep -v '^$' | sort || true
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

# ---------------------------------------------------------------------------
# Compare one documented line against CI's. Prints its own verdict; returns
# non-zero when they disagree, so the self-test can drive it directly.
#
# It reports rather than counting, because the caller owns the tally -- a file can
# disagree in more than one way and is still one site that needs fixing.
CompareConfigure() {
    label="$1"
    ciCommand="$2"
    docCommand="$3"

    if [[ -z "${docCommand// /}" ]]; then
        echo "  FAIL: $label documents no \`cmake --preset … -B ${DatabaseDir}\` line at all" >&2
        return 1
    fi

    ciNormal="$(NormaliseConfigure "$ciCommand")"
    docNormal="$(NormaliseConfigure "$docCommand")"

    if [[ "$ciNormal" == "$docNormal" ]]; then
        echo "ok: $label documents the clang-tidy job's Configure step"
        return 0
    fi

    echo "  FAIL: $label does not document what the clang-tidy job configures." >&2
    echo "        A person following it gets a database whose flags and target set differ from CI's," >&2
    echo "        so the sweep reports findings CI suppresses and misses findings CI raises." >&2
    # `diff` on process substitutions rather than a pipeline into `grep`: under
    # `pipefail` a short-circuiting reader kills the producer with SIGPIPE and the
    # pipeline then reports the PRODUCER's status.
    diff <(printf '%s\n' "$ciNormal") <(printf '%s\n' "$docNormal") \
        | sed 's/^/          /' >&2 || true
    return 1
}

# ---------------------------------------------------------------------------
# Self-test: the check must be shown to BITE. A guard that cannot be made to fail
# is indistinguishable from one that passes because nothing is wrong -- which is
# how the drift it now catches survived in the first place.
SelfTest() {
    ci='cmake --preset clang-debug -DENABLE_TIDY=OFF -DCMAKE_CXX_SCAN_FOR_MODULES=OFF -DFASTCACHED_ENABLE_TLS=ON'
    failures=0

    # One reporter for every kind of case, so a second helper cannot drift out of
    # argument order with the first. `(name, want, got)` throughout.
    Report() {
        if [[ "$2" == "$3" ]]; then
            echo "ok: self-test $1"
        else
            echo "  FAIL: self-test $1" >&2
            echo "        wanted: [$2]" >&2
            echo "        got:    [$3]" >&2
            failures=$((failures + 1))
        fi
    }

    # The comparison: does a documented line agree with CI's?
    ExpectCompare() {
        if CompareConfigure "synthetic" "$ci" "$3" >/dev/null 2>&1; then
            Report "compare '$1'" "$2" agree
        else
            Report "compare '$1'" "$2" differ
        fi
    }

    ExpectCompare "identical but for -B and wrapping" agree \
        "cmake --preset clang-debug -B ${DatabaseDir} -DENABLE_TIDY=OFF -DCMAKE_CXX_SCAN_FOR_MODULES=OFF -DFASTCACHED_ENABLE_TLS=ON"
    ExpectCompare "same options in a different order" agree \
        "cmake --preset clang-debug -B ${DatabaseDir} -DFASTCACHED_ENABLE_TLS=ON -DENABLE_TIDY=OFF -DCMAKE_CXX_SCAN_FOR_MODULES=OFF"
    ExpectCompare "a different preset" differ \
        "cmake --preset clang-release -B ${DatabaseDir} -DENABLE_TIDY=OFF -DCMAKE_CXX_SCAN_FOR_MODULES=OFF -DFASTCACHED_ENABLE_TLS=ON"
    ExpectCompare "a -D CI passes that the documentation drops" differ \
        "cmake --preset clang-debug -B ${DatabaseDir} -DENABLE_TIDY=OFF -DCMAKE_CXX_SCAN_FOR_MODULES=OFF"
    ExpectCompare "a -D the documentation adds that CI does not pass" differ \
        "cmake --preset clang-debug -B ${DatabaseDir} -DENABLE_TIDY=OFF -DCMAKE_CXX_SCAN_FOR_MODULES=OFF -DFASTCACHED_ENABLE_TLS=ON -DFASTCACHED_BUILD_NODE=OFF"
    ExpectCompare "a changed -D VALUE, not just a changed name" differ \
        "cmake --preset clang-debug -B ${DatabaseDir} -DENABLE_TIDY=ON -DCMAKE_CXX_SCAN_FOR_MODULES=OFF -DFASTCACHED_ENABLE_TLS=ON"
    ExpectCompare "nothing documented at all" differ ''
    # The regression itself: the pre-#454 hand-rolled line names no preset, so it
    # agrees with nothing.
    ExpectCompare "the pre-#454 hand-rolled line" differ \
        "cmake -S . -B ${DatabaseDir} -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_CXX_SCAN_FOR_MODULES=OFF -DFASTCACHED_ENABLE_TLS=ON"

    scratch="$(mktemp -d)"
    trap 'rm -rf "$scratch"' EXIT

    # -----------------------------------------------------------------------
    # The DOCUMENTATION-side extractor. `-B <database dir>` is half the anchor, so
    # a line that omits it is not a documented site at all -- which is what makes
    # a separate "does it name -B" assertion unnecessary rather than missing.
    ExpectDocument() {
        printf '%s\n' "$3" > "$scratch/doc.md"
        got="$(ExtractDocumentedConfigure "$scratch/doc.md" "$DatabaseDir")"
        Report "document '$1'" "$2" "${got%"${got##*[![:space:]]}"}"
    }

    ExpectDocument "a fenced block with backslash continuations" \
        "cmake --preset clang-debug -B ${DatabaseDir} -DENABLE_TIDY=OFF -DFASTCACHED_ENABLE_TLS=ON" \
        "Configure it like so:

\`\`\`sh
cmake --preset clang-debug -B ${DatabaseDir} -DENABLE_TIDY=OFF \\
      -DFASTCACHED_ENABLE_TLS=ON
\`\`\`"
    ExpectDocument "a shell header comment" \
        "cmake --preset clang-debug -B ${DatabaseDir} -DENABLE_TIDY=OFF" \
        "# And the database:
#
#   cmake --preset clang-debug -B ${DatabaseDir} -DENABLE_TIDY=OFF
#"
    ExpectDocument "an earlier unrelated preset line is not the anchor" \
        "cmake --preset clang-debug -B ${DatabaseDir} -DENABLE_TIDY=OFF" \
        "Build it with \`cmake --preset gcc-release\` first.
Then \`cmake --preset clang-coverage\`.

  cmake --preset clang-debug -B ${DatabaseDir} -DENABLE_TIDY=OFF"
    ExpectDocument "a preset line naming no -B extracts nothing" "" \
        "  cmake --preset clang-debug -DENABLE_TIDY=OFF"
    ExpectDocument "a preset line naming a DIFFERENT -B extracts nothing" "" \
        "  cmake --preset clang-debug -B out/build/somewhere-else -DENABLE_TIDY=OFF"

    # -----------------------------------------------------------------------
    # BOTH scan modes, against a synthetic tree. `check-catch-skip-return-code`
    # is the recorded reason: its self-test drove the fallback six times while CI
    # drove git, so the mode under test was never the mode in use and nothing
    # disagreed. The mode is part of the assertion here, not just the file set.
    #
    # The git leg also pins the property that motivates preferring git: an
    # IGNORED directory holding another checkout -- which `.claude/worktrees/` is
    # in the primary clone -- must not contribute sites.
    ExpectScan() {
        name="$1"; wantMode="$2"; useGit="$3"
        tree="$scratch/scan-$4"
        mkdir -p "$tree/scripts" "$tree/.claude/worktrees/other/scripts"
        printf '#   cmake --preset clang-debug -B %s -DENABLE_TIDY=OFF\n' "$DatabaseDir" \
            > "$tree/scripts/tidy-sweep.sh"
        printf '  cmake --preset clang-debug -B %s -DSTALE=YES\n' "$DatabaseDir" \
            > "$tree/.claude/worktrees/other/scripts/tidy-sweep.sh"
        printf '.claude/\n' > "$tree/.gitignore"
        if [[ "$useGit" == git ]]; then
            ( cd "$tree" && git init -q . && git add -A && \
              git -c user.email=t@t -c user.name=t commit -qm t ) >/dev/null 2>&1
        fi
        got="$( cd "$tree" && DocumentationSites )"
        gotMode="$(printf '%s\n' "$got" | sed -n 's/^mode:\(.*\)/\1/p')"
        gotSites="$(printf '%s\n' "$got" | grep -v '^mode:' | grep -v '^$' | tr '\n' ' ')"
        Report "scan '$name' mode" "$wantMode" "$gotMode"
        Report "scan '$name' sites" "scripts/tidy-sweep.sh " "$gotSites"
    }

    if command -v git >/dev/null 2>&1; then
        ExpectScan "git index, ignored sibling checkout excluded" \
            "git ls-files" git g
    else
        echo "ok: self-test scan 'git index' skipped -- no git on PATH"
    fi
    ExpectScan "no git index, ignored sibling checkout excluded" \
        "directory walk (no git index)" nogit w

    # -----------------------------------------------------------------------
    # The WORKFLOW-side extractor, which decides what everything else is compared
    # AGAINST. Its failure mode is an empty string, which the main path treats as
    # fatal -- but "returns empty when it should" and "returns the RIGHT step when
    # several jobs have one" are different questions, and `build.yml` really does
    # carry a `Configure` step in the linux and windows matrix jobs too.
    #
    # The added steps are not hypothetical: a queued pull request adds an `id:` and
    # a further step to this very job, so a reader can see neither moves the answer.
    ExpectWorkflow() {
        printf '%s\n' "$3" > "$scratch/wf.yml"
        got="$(ExtractWorkflowConfigure "$scratch/wf.yml" "clang-tidy" "Configure")"
        Report "workflow '$1'" "$2" "${got%"${got##*[![:space:]]}"}"
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
    echo "check-tidy-sweep-database --self-test: comparison, both extractors and both scan modes all bite"
}

if [[ "${1:-}" == "--self-test" ]]; then
    SelfTest
    exit 0
fi

# ---------------------------------------------------------------------------
[[ -f "$Workflow" ]] || Abort "$Workflow does not exist"

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
[[ $problems -eq 0 ]] || Abort "the check cannot say anything about the documentation without it"

# A read loop and not `mapfile`, which is bash 4+; macOS ships 3.2. Process
# substitution rather than a pipeline, so the scan's exit status cannot take the
# script down under `pipefail`.
sites=()
scanMode="(not reported)"
while IFS= read -r site; do
    case "$site" in
        mode:*) scanMode="${site#mode:}"; continue ;;
        "") continue ;;
    esac
    sites+=("$site")
done < <(DocumentationSites)
echo "ok: documentation sites discovered by ${scanMode}"

if [[ "${#sites[@]}" -eq 0 ]]; then
    Fail "no file in the tree documents a \`cmake --preset … -B ${DatabaseDir}\` line, so this check would pass having compared nothing"
    Abort "the scan found no documentation to check"
fi

foundRequired=no
for site in "${sites[@]}"; do
    [[ "$site" == "$RequiredSite" ]] && foundRequired=yes
    CompareConfigure "$site" "$CiConfigure" \
        "$(ExtractDocumentedConfigure "$site" "$DatabaseDir")" || problems=$((problems + 1))
done

if [[ "$foundRequired" != yes ]]; then
    Fail "$RequiredSite documents no configure line for the sweep's database; it is the header a person reads to reproduce CI's sweep, and a scan alone would go green with it missing"
fi

if [[ $problems -gt 0 ]]; then
    Abort "a local sweep following the documentation would not agree with CI"
fi
echo "check-tidy-sweep-database: ${#sites[@]} documented line(s) match the clang-tidy job's Configure step"
