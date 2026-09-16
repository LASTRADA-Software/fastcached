#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Every SHIPPING configure in a workflow is followed, in its own job and before
# that job's build, by the instruction-set check (#1450).
#
# ## Why this exists
#
# #1442 put `scripts/check-instruction-set-flags.sh` into the three `Package`
# jobs, and that step is the only thing that reads what SHIPS: the macOS package
# is configured by a hand-written `cmake` line that no `ctest` leg configures, so
# nothing else in the tree sees those flags at all.
#
# Nothing made a FUTURE shipping configure get the step. A fourth packaging job, a
# new configure line in an existing one, or the step reordered below the build
# would ship with no instruction-set check, and **every required context would
# stay green, because `Package` jobs are not required contexts.** This is the
# rulebook's *a rule stated in the files that obey it reaches no file that does
# not*: the obligation lived only in the three steps already honouring it.
#
# ## The selector is DATA, not three job names
#
# A shipping configure is a step whose `run:` invokes `cmake` with
# `-DFASTCACHED_BUILD_TESTS=OFF` -- the one property separating what a customer
# receives from what `ctest` measures. A fourth packaging job is then something
# the selector FINDS rather than something somebody remembered to add.
#
# The step's body is JOINED before it is tested, and that is load-bearing: the
# macOS configure spells `-DFASTCACHED_BUILD_TESTS=OFF \` on a continuation line
# of its own, so a line-wise selector finds two of the three and reports clean.
# The joining is the shared walk's (`scripts/lib/workflow-walk.awk`, #1456), which
# is also why this check knows a step's ORDER in its job without counting columns.
#
# ## Four verdicts, not one flag
#
# The guard must exist in the same job, precede that job's build, carry no `if:`
# and carry no `continue-on-error:`. The last two are different mechanisms --
# reporting without gating, and gating without failing -- so they are named
# separately: a refusal naming the wrong one sends somebody to delete the wrong
# line.
#
# ## The Dockerfile, which is exempt by row and by reason
#
# `Dockerfile` carries a shipping configure too, and it cannot have this step: the
# image is built and run inside one CI job and pushed to no registry, so nothing
# it compiles reaches a customer CPU. It is a row in
# `scripts/check-shipping-configure-guard-exemptions.txt` with that reason, and a
# row that has stopped describing a real shipping configure is refused as STALE.
#
# ## Zero shipping configures is a REFUSAL
#
# Every verdict here is about something MISSING, so over a file this could not
# read each one would answer *nothing to vouch for*. **Absence of the negative is
# not the positive**: it reports how many shipping configures it read and how many
# steps the walk placed, and refuses on zero of either.
set -uo pipefail

FastCachedRoot="${FASTCACHED_SHIPPING_GUARD_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
FastCachedWorkflows="${FASTCACHED_SHIPPING_GUARD_WORKFLOWS:-${FastCachedRoot}/.github/workflows}"
FastCachedExemptions="${FASTCACHED_SHIPPING_GUARD_EXEMPTIONS:-${FastCachedRoot}/scripts/check-shipping-configure-guard-exemptions.txt}"
FastCachedWalkAwk="${FastCachedRoot}/scripts/lib/workflow-walk.awk"
FastCachedGuardAwk="${FastCachedRoot}/scripts/check-shipping-configure-guard.awk"

# What makes a configure a SHIPPING one, and which script must follow it. Both are
# data rather than literals spread through the code below, so a rename is one edit.
ShippingFlag="-DFASTCACHED_BUILD_TESTS=OFF"
GuardScript="scripts/check-instruction-set-flags.sh"
# Files other than a workflow that carry a shipping configure. `Dockerfile` is the
# one, and it is a plain text scan: the rule is about every shipping configure in
# the repository, and a scan that looked only at workflows would be silent about
# exactly the file the exemption exists for.
OtherSubjects="Dockerfile"

for awkProgram in "${FastCachedWalkAwk}" "${FastCachedGuardAwk}"; do
    if [ ! -f "${awkProgram}" ]; then
        echo "check-shipping-configure-guard: missing awk program ${awkProgram}; no workflow was" >&2
        echo "  read, so this is a refusal and not a clean run." >&2
        exit 2
    fi
done

Problems=0
Fail() { echo "  FAIL: $*" >&2; Problems=$((Problems + 1)); }

# The records the walk yields for the workflow @p 1, of kind @p 2, kind column
# dropped. `grep | cut` and never `grep -q` or `| head`: a pipe into a consumer
# that exits early is a false negative under `pipefail` on its SUCCESS path.
Records() {
    local out
    out="$(awk -v guard="${GuardScript}" -v shipping="${ShippingFlag}" \
        -f "${FastCachedWalkAwk}" -f "${FastCachedGuardAwk}" "$1" "$1")"
    grep -F -- "$2	" <<< "${out}" | cut -f2- || true
}

# ---------------------------------------------------------------------------
if [ "${1:-}" = "--self-test" ]; then
    me="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"
    scratch="$(mktemp -d)" || { echo "cannot create a scratch directory" >&2; exit 2; }
    # shellcheck disable=SC2064  # expand $scratch now, not at trap time
    trap "rm -rf '$scratch'" EXIT
    selfTestCases=0
    selfTestStatus=0

    # The shipping flag and the guard path are written into the fixtures from the
    # SAME variables the check reads, so a rename cannot leave the fixtures
    # describing a rule that no longer exists while every case still passes.
    Stage() {
        rm -rf "$scratch/wf"
        mkdir -p "$scratch/wf"
        : > "$scratch/exemptions.txt"
    }

    # @param 1 the job body, with @SHIP@ and @GUARD@ substituted
    Workflow() {
        printf '%s\n' "$1" \
            | sed -e "s|@SHIP@|${ShippingFlag}|g" -e "s|@GUARD@|${GuardScript}|g" \
            > "$scratch/wf/build.yml"
    }

    # @param 1 what is staged  @param 2 want-pass|want-fail|want-refuse
    Case() {
        local what="$1" want="$2" out got=0
        selfTestCases=$((selfTestCases + 1))
        out="$(FASTCACHED_SHIPPING_GUARD_WORKFLOWS="$scratch/wf" \
               FASTCACHED_SHIPPING_GUARD_EXEMPTIONS="$scratch/exemptions.txt" \
               FASTCACHED_SHIPPING_GUARD_OTHER="" \
               bash "$me" 2>&1)" || got=$?
        case "$want" in
            want-pass)   [ "$got" -eq 0 ] && { echo "  ok    ($want) $what"; return; } ;;
            want-fail)   [ "$got" -eq 1 ] && { echo "  ok    ($want) $what"; return; } ;;
            want-refuse) [ "$got" -eq 2 ] && { echo "  ok    ($want) $what"; return; } ;;
        esac
        echo "  FAIL  ($want, exit $got) $what" >&2
        printf '%s\n' "$out" | sed 's/^/        /' >&2
        selfTestStatus=1
    }

    # @param 1 what is staged  @param 2 want  @param 3 a substring the output must carry
    CaseNaming() {
        local what="$1" want="$2" needle="$3" out got=0
        selfTestCases=$((selfTestCases + 1))
        out="$(FASTCACHED_SHIPPING_GUARD_WORKFLOWS="$scratch/wf" \
               FASTCACHED_SHIPPING_GUARD_EXEMPTIONS="$scratch/exemptions.txt" \
               FASTCACHED_SHIPPING_GUARD_OTHER="" \
               bash "$me" 2>&1)" || got=$?
        if [ "$want" = want-fail ] && [ "$got" -ne 1 ]; then
            echo "  FAIL  ($want, exit $got) $what" >&2
            printf '%s\n' "$out" | sed 's/^/        /' >&2
            selfTestStatus=1
            return
        fi
        # A refusal that does not SAY which job lost its step sends whoever meets it
        # to read all three. Asserted as its own half, because both the defect and
        # a nameless refusal exit 1.
        if ! grep -Fq -- "$needle" <<< "$out"; then
            echo "  FAIL  $what: refused, but its output does not name <$needle>" >&2
            printf '%s\n' "$out" | sed 's/^/        /' >&2
            selfTestStatus=1
            return
        fi
        echo "  ok    ($want) $what"
    }

    Correct='name: fixture
on: push
jobs:
  package-thing:
    runs-on: ubuntu-24.04
    steps:
      - name: "Configure"
        run: cmake --preset gcc-release @SHIP@ -DFASTCACHED_PACKAGE_ROOT_PREFIX=ON
      - name: "No global instruction-set flag in what ships"
        run: bash scripts/run-check.sh @GUARD@ out/build/gcc-release/compile_commands.json
      - name: "Build"
        run: cmake --build out/build/gcc-release --target fastcached'

    Stage; Workflow "$Correct"
    Case "a shipping configure with the guard before its build passes" want-pass

    # The arm this check exists for.
    Stage
    Workflow "$(printf '%s\n' "$Correct" | grep -v 'instruction-set\|run-check.sh @GUARD@')"
    Case "a shipping configure with NO guard step is REFUSED" want-fail

    # Order, which is the half a step reordering during a refactor breaks.
    Stage
    Workflow 'name: fixture
on: push
jobs:
  package-thing:
    runs-on: ubuntu-24.04
    steps:
      - name: "Configure"
        run: cmake --preset gcc-release @SHIP@
      - name: "Build"
        run: cmake --build out/build/gcc-release --target fastcached
      - name: "No global instruction-set flag in what ships"
        run: bash scripts/run-check.sh @GUARD@ out/build/gcc-release/compile_commands.json'
    Case "a guard step AFTER the build is REFUSED" want-fail

    # Reporting without gating.
    Stage
    Workflow 'name: fixture
on: push
jobs:
  package-thing:
    runs-on: ubuntu-24.04
    steps:
      - name: "Configure"
        run: cmake --preset gcc-release @SHIP@
      - name: "No global instruction-set flag in what ships"
        if: ${{ github.event_name == '"'"'push'"'"' }}
        run: bash scripts/run-check.sh @GUARD@ out/build/gcc-release/compile_commands.json
      - name: "Build"
        run: cmake --build out/build/gcc-release --target fastcached'
    Case "a guard step carrying an if: is REFUSED" want-fail

    # Gating without failing, which is a different mechanism and its own refusal.
    Stage
    Workflow 'name: fixture
on: push
jobs:
  package-thing:
    runs-on: ubuntu-24.04
    steps:
      - name: "Configure"
        run: cmake --preset gcc-release @SHIP@
      - name: "No global instruction-set flag in what ships"
        continue-on-error: true
        run: bash scripts/run-check.sh @GUARD@ out/build/gcc-release/compile_commands.json
      - name: "Build"
        run: cmake --build out/build/gcc-release --target fastcached'
    Case "a guard step carrying continue-on-error is REFUSED" want-fail

    # The guard in ANOTHER job says nothing: the database it reads is that job's
    # working tree, so a cross-job step is a check over a directory that is empty.
    Stage
    Workflow 'name: fixture
on: push
jobs:
  package-thing:
    runs-on: ubuntu-24.04
    steps:
      - name: "Configure"
        run: cmake --preset gcc-release @SHIP@
      - name: "Build"
        run: cmake --build out/build/gcc-release --target fastcached
  elsewhere:
    runs-on: ubuntu-24.04
    steps:
      - name: "No global instruction-set flag in what ships"
        run: bash scripts/run-check.sh @GUARD@ out/build/gcc-release/compile_commands.json'
    Case "a guard step in ANOTHER job is REFUSED, not counted" want-fail

    # A guard naming a DIFFERENT configure's database. The likeliest shape after a
    # copy-paste, and the one a per-job existence test cannot see.
    Stage
    Workflow 'name: fixture
on: push
jobs:
  package-thing:
    runs-on: ubuntu-24.04
    steps:
      - name: "Configure"
        run: cmake -S . -B out/build/thing @SHIP@
      - name: "No global instruction-set flag in what ships"
        run: bash scripts/run-check.sh @GUARD@ out/build/somewhere-else/compile_commands.json
      - name: "Build"
        run: cmake --build out/build/thing --target fastcached'
    Case "a guard naming another configure's database is REFUSED" want-fail

    # A multi-line configure, which the three real ones include: the macOS package
    # spells the flag on a continuation line of its own, so a line-wise selector
    # finds two of three and reports clean.
    Stage
    Workflow 'name: fixture
on: push
jobs:
  package-thing:
    runs-on: ubuntu-24.04
    steps:
      - name: "Configure"
        run: |
          cmake -S . -B out/build/thing -G Ninja \
            -DCMAKE_BUILD_TYPE=Release \
            @SHIP@ \
            -DFASTCACHED_PACKAGE_ROOT_PREFIX=ON
      - name: "No global instruction-set flag in what ships"
        run: bash scripts/run-check.sh @GUARD@ out/build/thing/compile_commands.json
      - name: "Build"
        run: cmake --build out/build/thing --target fastcached'
    Case "a configure whose flag sits on a continuation line is FOUND and passes" want-pass

    # And the same shape with the guard removed, so the case above is not passing
    # because the configure was invisible. Without this pair a selector that finds
    # NOTHING passes the case above for the opposite reason.
    Stage
    Workflow 'name: fixture
on: push
jobs:
  package-thing:
    runs-on: ubuntu-24.04
    steps:
      - name: "Configure"
        run: |
          cmake -S . -B out/build/thing -G Ninja \
            -DCMAKE_BUILD_TYPE=Release \
            @SHIP@ \
            -DFASTCACHED_PACKAGE_ROOT_PREFIX=ON
      - name: "Build"
        run: cmake --build out/build/thing --target fastcached'
    Case "the same continuation-line configure with no guard is REFUSED, so the case above found it" want-fail

    # A configure that is NOT a shipping one must not be required to have a guard.
    Stage
    Workflow 'name: fixture
on: push
jobs:
  ordinary:
    runs-on: ubuntu-24.04
    steps:
      - name: "Configure"
        run: cmake --preset clang-debug
      - name: "Build"
        run: cmake --build out/build/clang-debug
  package-thing:
    runs-on: ubuntu-24.04
    steps:
      - name: "Configure"
        run: cmake --preset gcc-release @SHIP@
      - name: "No global instruction-set flag in what ships"
        run: bash scripts/run-check.sh @GUARD@ out/build/gcc-release/compile_commands.json
      - name: "Build"
        run: cmake --build out/build/gcc-release --target fastcached'
    Case "a configure without the shipping flag needs no guard and passes" want-pass

    # A configure this cannot resolve to a directory is REFUSED rather than
    # vouched for: an unresolved database is a comparison nobody can make, and
    # passing it is the silent half.
    Stage
    Workflow 'name: fixture
on: push
jobs:
  package-thing:
    runs-on: ubuntu-24.04
    steps:
      - name: "Configure"
        run: cmake @SHIP@ .
      - name: "No global instruction-set flag in what ships"
        run: bash scripts/run-check.sh @GUARD@ out/build/thing/compile_commands.json
      - name: "Build"
        run: cmake --build out/build/thing --target fastcached'
    Case "a shipping configure whose build directory cannot be resolved is REFUSED" want-fail

    # Zero shipping configures. Every verdict above is about something missing, so
    # a file with none would answer 'nothing to vouch for' for each of them.
    Stage
    Workflow 'name: fixture
on: push
jobs:
  ordinary:
    runs-on: ubuntu-24.04
    steps:
      - name: "Configure"
        run: cmake --preset clang-debug
      - name: "Build"
        run: cmake --build out/build/clang-debug'
    Case "a workflow set with NO shipping configure is REFUSED, not read as clean" want-refuse

    # An exemption row naming nothing.
    Stage; Workflow "$Correct"
    printf 'no-such-file\ta row for a subject that is not there\n' > "$scratch/exemptions.txt"
    Case "an exemption row naming no shipping configure is REFUSED as stale" want-fail

    # And the enumeration: an empty workflow directory.
    Stage
    Case "an empty workflow directory is REFUSED, never read as 'every configure is guarded'" want-refuse

    # ---- and the same rule over the REAL workflows -------------------------
    # Synthetic fixtures prove the rules; only the real file proves the rules reach
    # the file they were written for. The CONTROL first: the tree as it stands must
    # pass through the same staging, or the neuter below says only that a copied
    # tree behaves differently.
    realWorkflows="${FastCachedRoot}/.github/workflows"
    if [ -d "$realWorkflows" ]; then
        Stage
        cp "$realWorkflows"/*.yml "$scratch/wf/"
        Case "the real workflows, copied unmodified, pass" want-pass

        # Delete ONE packaging job's guard step and require the refusal to name that
        # job. The deletion is asserted to have changed the file: a `sed` matching
        # nothing leaves a correct workflow and the case passes for the opposite
        # reason, which is the trap this repository has paid for more than once.
        Stage
        cp "$realWorkflows"/*.yml "$scratch/wf/"
        cp "$scratch/wf/build.yml" "$scratch/before.yml"
        # ONE line of lookahead: drop the first `run:` line naming the guard and the
        # `- name:` line held above it, which in `build.yml` is adjacent to it. Small
        # enough to read, and the SHIPPING count is what confirms it deleted a guard
        # rather than a configure -- the first draft of this deleted a configure, and
        # the count falling from 3 to 2 is how that showed.
        awk -v guard="${GuardScript}" '
            done { print; next }
            # A `run:` line, never any line naming the script: the guard step in
            # `build.yml` is preceded by a COMMENT that names it, so an unanchored
            # match dropped the comment and the line above it and left the real
            # step standing -- the check then passed and the case reported the
            # neuter inert. A COMMENT is not a call site, in a neuter as anywhere.
            /^[ 	]*run:/ && index($0, guard) { done = 1; prev = ""; next }
            { if (prev != "") print prev; prev = $0 }
            END { if (prev != "") print prev }
        ' "$scratch/before.yml" > "$scratch/wf/build.yml"
        if cmp -s "$scratch/wf/build.yml" "$scratch/before.yml"; then
            echo "  FAIL  the real-file neuter deleted nothing, so the case stages no defect" >&2
            selfTestStatus=1
        else
            CaseNaming "deleting one packaging job's guard step from the real build.yml is REFUSED, naming that job" \
                want-fail "package-linux"
            # The shipping count must be UNCHANGED, or the neuter deleted a configure
            # and the refusal above is about the wrong thing entirely.
            selfTestCases=$((selfTestCases + 1))
            neuteredOut="$(FASTCACHED_SHIPPING_GUARD_WORKFLOWS="$scratch/wf" \
                FASTCACHED_SHIPPING_GUARD_EXEMPTIONS="$scratch/exemptions.txt" \
                FASTCACHED_SHIPPING_GUARD_OTHER="" bash "$me" 2>&1)" || true
            if grep -Fq -- "3 shipping configure(s)" <<< "$neuteredOut"; then
                echo "  ok    (want-fail) the neuter left all 3 shipping configures in place, so it deleted a GUARD"
            else
                echo "  FAIL  the neuter changed the shipping count, so it deleted a configure rather than a guard" >&2
                printf '%s\n' "$neuteredOut" | sed 's/^/        /' >&2
                selfTestStatus=1
            fi
        fi
    else
        echo "  FAIL  ${realWorkflows} is not a directory, so the real-file cases could not run" >&2
        selfTestStatus=1
    fi

    echo "check-shipping-configure-guard --self-test: ${selfTestCases} case(s) ran"
    if [ "${selfTestStatus}" -ne 0 ]; then
        echo "check-shipping-configure-guard --self-test: FAILED" >&2
        exit 1
    fi
    echo "check-shipping-configure-guard --self-test: every verdict as it must be"
    exit 0
fi

# ---------------------------------------------------------------------------
Workflows="$(find "${FastCachedWorkflows}" -maxdepth 1 -name '*.yml' 2>/dev/null | LC_ALL=C sort)"
if [ -z "${Workflows}" ]; then
    echo "check-shipping-configure-guard: ${FastCachedWorkflows} holds no *.yml, so no configure was" >&2
    echo "  read. Refused rather than reported as 'every shipping configure is guarded'." >&2
    exit 2
fi

ExemptPaths=""
if [ -r "${FastCachedExemptions}" ]; then
    ExemptPaths="$(sed -e 's/^[[:space:]]*#.*$//' -e '/^[[:space:]]*$/d' "${FastCachedExemptions}" | cut -f1)"
fi

ShippingCount=0
PlacedSteps=0
ExemptUsed=""

while IFS= read -r workflow; do
    [ -n "${workflow}" ] || continue
    name="${workflow##*/}"
    placed="$(Records "${workflow}" STEPS)"
    PlacedSteps=$((PlacedSteps + ${placed:-0}))
    shipping="$(Records "${workflow}" SHIPPING)"
    [ -n "${shipping}" ] || continue
    guards="$(Records "${workflow}" GUARD)"
    builds="$(Records "${workflow}" BUILD)"
    while IFS=$'\t' read -r job index step dir; do
        [ -n "${job}" ] || continue
        ShippingCount=$((ShippingCount + 1))
        if [ -z "${dir}" ]; then
            Fail "${name}: the shipping configure in job '${job}' (step ${index}, '${step}') names neither a \`-B\` directory nor a \`--preset\`, so which database the guard must read cannot be decided. Passing it would be vouching for a comparison nobody can make."
            continue
        fi
        # The guard for THIS configure: same job, naming this configure's directory.
        # Matched on the directory and not merely on the job, because a copied step
        # naming another configure's database is the likeliest wrong shape and a
        # per-job existence test cannot see it.
        match=""
        while IFS=$'\t' read -r gjob gindex garg gif gcont; do
            [ "${gjob}" = "${job}" ] || continue
            case "${garg}" in *"${dir}"*) ;; *) continue ;; esac
            match="${gindex}	${gif}	${gcont}"
            break
        done <<< "${guards}"
        if [ -z "${match}" ]; then
            Fail "${name}: the shipping configure in job '${job}' (step ${index}, '${step}', into ${dir}) is followed by no \`${GuardScript}\` step reading ${dir}. That configure is the only thing that reads what SHIPS, and \`Package\` jobs are not required contexts, so its absence is green everywhere."
            continue
        fi
        IFS=$'\t' read -r gindex gif gcont <<< "${match}"
        if [ "${gindex}" -le "${index}" ]; then
            Fail "${name}: job '${job}' runs the guard at step ${gindex}, before its shipping configure at step ${index}. A check over a database that does not exist yet reports nothing and passes."
        fi
        # BEFORE the job's first build.
        firstBuild=""
        while IFS=$'\t' read -r bjob bindex; do
            [ "${bjob}" = "${job}" ] || continue
            [ -z "${firstBuild}" ] && firstBuild="${bindex}"
        done <<< "${builds}"
        if [ -n "${firstBuild}" ] && [ "${gindex}" -gt "${firstBuild}" ]; then
            Fail "${name}: job '${job}' runs the guard at step ${gindex}, AFTER its build at step ${firstBuild}. The objects it would have refused are already built, and on a packaging job already signed."
        fi
        if [ -n "${gif}" ]; then
            Fail "${name}: the guard step in job '${job}' carries \`if: ${gif}\`. A condition is how a step REPORTS without gating -- on the event it is skipped for, the package ships unchecked and the job is green."
        fi
        if [ "${gcont}" = "true" ]; then
            Fail "${name}: the guard step in job '${job}' carries \`continue-on-error: true\`. That is how a step GATES without failing: it runs, it refuses, and the package is built anyway."
        fi
    done <<< "${shipping}"
done <<< "${Workflows}"

# Shipping configures outside a workflow. `Dockerfile` has one and cannot carry
# the step, so it is exempt by row -- and the scan looks for it anyway, because a
# check that only read workflows would be silent about exactly the file the
# exemption exists for.
for other in ${FASTCACHED_SHIPPING_GUARD_OTHER-${OtherSubjects}}; do
    [ -n "${other}" ] || continue
    path="${FastCachedRoot}/${other}"
    [ -r "${path}" ] || continue
    grep -Fq -- "${ShippingFlag}" "${path}" || continue
    ShippingCount=$((ShippingCount + 1))
    if grep -Fqx -- "${other}" <<< "${ExemptPaths}"; then
        ExemptUsed="${ExemptUsed}${other}"$'\n'
        continue
    fi
    Fail "${other} carries a shipping configure and no exemption row. Either it must run ${GuardScript} on its build directory, or scripts/check-shipping-configure-guard-exemptions.txt must say why what it compiles reaches no customer CPU."
done

while IFS= read -r row; do
    [ -n "${row}" ] || continue
    if ! grep -Fqx -- "${row}" <<< "${ExemptUsed}"; then
        Fail "the exemption row for '${row}' matched no shipping configure: either it no longer carries one, or the file is gone. Delete the row rather than leaving it to excuse the next subject at that path."
    fi
done <<< "${ExemptPaths}"

if [ "${PlacedSteps}" -eq 0 ]; then
    echo "check-shipping-configure-guard: the walk placed 0 steps across $(grep -c . <<< "${Workflows}") workflow file(s)," >&2
    echo "  so every verdict above would be 'nothing to vouch for'. Refused." >&2
    exit 2
fi
if [ "${ShippingCount}" -eq 0 ]; then
    echo "check-shipping-configure-guard: 0 shipping configures found across $(grep -c . <<< "${Workflows}") workflow" >&2
    echo "  file(s) and ${PlacedSteps} step(s). Every verdict here is about something MISSING, so a" >&2
    echo "  tree with none would answer 'nothing to vouch for' for each of them. Refused." >&2
    exit 2
fi

echo "check-shipping-configure-guard: ${ShippingCount} shipping configure(s) across $(grep -c . <<< "${Workflows}") workflow file(s) and ${PlacedSteps} placed step(s), $(grep -c . <<< "${ExemptUsed}" || true) exempt by row"
if [ "${Problems}" -ne 0 ]; then
    echo "check-shipping-configure-guard: ${Problems} problem(s); what ships would be compiled with no instruction-set check and every required context would stay green" >&2
    exit 1
fi
echo "check-shipping-configure-guard: every shipping configure is checked for a global instruction-set flag before it builds"
exit 0
