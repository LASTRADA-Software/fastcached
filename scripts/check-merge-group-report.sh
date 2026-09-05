#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# The notifier for #684 is wired up, and it decides correctly.
#
# Usage:  scripts/check-merge-group-report.sh [--self-test]
#
#   (no arguments)  assert the SHAPE of `.github/workflows/merge-group-report.yml`
#                   and of the two scripts it drives.
#   --self-test     drive `scripts/ci-merge-group-report.sh` against staged job
#                   records -- including three captured from real failing
#                   merge-group runs -- and `scripts/ci-report-issue.sh` against
#                   a stub `gh`.
#
# ## Why the shape is asserted rather than the behaviour
#
# `workflow_run` only ever runs the copy of a workflow file that is on the
# DEFAULT branch. So nothing on a pull request exercises this notifier, and its
# first run anywhere is after it merges -- the same "cannot be demonstrated
# before the fact" that `check-merge-queue-contexts.sh` records about
# `merge_group`, arriving one door further out.
#
# The answer is the same one this tree reaches for whenever an instrument cannot
# be tested without paying for what it measures: split the DECISION out and test
# that exhaustively, leave acquisition as thin as it can be, and assert the
# wiring statically so it cannot be removed silently. `local-gate.sh`'s pure
# renderer and `node-scratch-isolation-e2e`'s readings record are the same move.
#
# ## The one claim here that was NOT measured
#
# That `workflow_run` fires at all for a run whose own event was `merge_group`.
# GitHub's documentation says the event fires on any workflow run completing and
# excludes only runs triggered by `GITHUB_TOKEN`; a merge queue dispatches as
# `github-merge-queue[bot]`, not as a workflow token. That is REASONED, not
# measured, and it cannot be measured from a branch.
#
# What makes it self-revealing rather than a silent bet is that the workflow
# carries no `branches:` filter: it therefore fires on ordinary pull-request
# `Build` runs too and prints "event is 'pull_request', not merge_group". So from
# the first run after it merges, "it never fires" and "it fires and has nothing
# to say" are two visible states rather than one indistinguishable silence. That
# is the four-states rule applied to the one thing here that could not be proven.
#
# ## bash 3.2
#
# Registered in the default ctest set, so it runs on macOS's 2007 `/bin/bash`.
# No `mapfile`, no `declare -A`, no `${var^^}`.

set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

Workflow=".github/workflows/merge-group-report.yml"
Decider="scripts/ci-merge-group-report.sh"
Reporter="scripts/ci-report-issue.sh"
BuildWorkflow=".github/workflows/build.yml"

problems=0
Fail() { echo "  FAIL: $*" >&2; problems=$((problems + 1)); }
Ok()   { echo "ok: $*"; }

# ---------------------------------------------------------------------------
# A COMMENT is not a setting. Every rule below reads the workflow with full-line
# comments removed, because this file's own header explains what it refuses --
# and it therefore contains the words `always()` and `cancel-in-progress`, which
# made two rules fire against a correct workflow the first time this ran.
#
# That is the second instance of one mistake in this branch: `check-gated-jobs.sh`
# attributed its own explanatory comment to a step and reported that step twice.
# A rule satisfied -- or refused -- by prose is a rule that passes with the code
# deleted, so both directions matter and both are self-tested.
#
# Full-line comments only (`^[ \t]*#`), never a trailing `#`: in YAML a `#`
# starts a comment only after whitespace, and a broader rule would eat `#` inside
# a value. Narrow on purpose.
NonComment() { grep -v '^[[:space:]]*#' "$1" || true; }

Shape() {
    local file
    for file in "$Workflow" "$Decider" "$Reporter"; do
        [[ -f "$file" ]] || { Fail "$file does not exist"; return; }
    done

    local body
    body="$(NonComment "$Workflow")"

    # The trigger. A notifier listening for the wrong workflow, or only for a
    # `requested` run, produces nothing -- and produces it silently, which is the
    # failure it exists to remove.
    grep -q '^  workflow_run:' <<< "$body" \
        && Ok "$Workflow triggers on workflow_run" \
        || Fail "$Workflow has no \`workflow_run:\` trigger, so it would never run at all"

    grep -qE '^ *workflows: *\["?Build"?\]' <<< "$body" \
        && Ok "it listens for the \"Build\" workflow" \
        || Fail "$Workflow does not name the \"Build\" workflow in its trigger; \`workflow_run\` with no \`workflows:\` list is not what this is for, and a misspelled name produces no run and no error"

    grep -qE '^ *types: *\[completed\]' <<< "$body" \
        && Ok "it listens for completed runs" \
        || Fail "$Workflow does not say \`types: [completed]\`; a \`requested\` run has no job conclusions to read, and the default includes it"

    # A `branches:` filter here would narrow this to the queue's temporary
    # branches and save a runner start -- at the price of the one thing that
    # makes the untestable claim above observable. See this file's header.
    if grep -qE '^ *branches(-ignore)?:' <<< "$body"; then
        Fail "$Workflow filters its \`workflow_run\` trigger by branch. That silently stops matching if GitHub renames the queue's temporary branches, and it removes the ordinary pull-request runs that are the only evidence this notifier fires at all."
    else
        Ok "it filters no branches, so an ordinary run is evidence it fires"
    fi

    grep -q 'issues: write' <<< "$body" \
        && Ok "it may open an issue" \
        || Fail "$Workflow does not grant \`issues: write\`; the report would fail with a bare 403 and the failure it exists to surface would go unreported for a second reason"

    # `always()` runs even while the run is being cancelled; `!cancelled()` does
    # not. Both look like "run anyway".
    if grep -q 'always()' <<< "$body"; then
        Fail "$Workflow uses \`always()\`, which also runs while the run is being CANCELLED. Use \`!cancelled()\`."
    else
        grep -q '!cancelled()' <<< "$body" \
            && Ok "its job carries !cancelled(), not always()" \
            || Fail "$Workflow's job carries no \`!cancelled()\`; a \`needs:\` added later would then let a skipped dependency skip the notifier, which is #684 one level up"
    fi

    # `cancel-in-progress` on a per-run key would be harmless; on any key that
    # collapses two runs together it discards a report. Refused outright, because
    # a notifier has nothing to gain from cancellation.
    if grep -q 'cancel-in-progress' <<< "$body"; then
        Fail "$Workflow sets \`cancel-in-progress\`. Two Build runs finishing together are two independent reports, and cancelling one to make room for the other loses the finding this workflow exists to deliver."
    else
        Ok "it cancels no in-progress report"
    fi

    grep -q "$Decider" <<< "$body" \
        && Ok "it runs $Decider" \
        || Fail "$Workflow does not run $Decider; the decision would then live in the YAML, where nothing can test it"

    grep -q "$Reporter" <<< "$body" \
        && Ok "it runs $Reporter" \
        || Fail "$Workflow does not run $Reporter"

    # The required-context list must be READ, never restated. A second copy is
    # the thing that decides whether a failure is reported, so a drift would make
    # the notifier silent about exactly the jobs it exists for.
    grep -q 'check-merge-queue-contexts.sh' <<< "$(NonComment "$Decider")" \
        && Ok "$Decider reads the required-context list from check-merge-queue-contexts.sh" \
        || Fail "$Decider does not read \`scripts/check-merge-queue-contexts.sh\`; a second copy of the required-context list is not a cross-check, it is a second thing to be wrong"

    # And it must not be a job in build.yml, for the reason check-release-gate
    # exists: a notifier job is skipped on a tag push, always, and would skip the
    # release with it.
    if grep -qE '^  [A-Za-z0-9_-]*merge-group-report[A-Za-z0-9_-]*:' <<< "$(NonComment "$BuildWorkflow")"; then
        Fail "the notifier has become a JOB in $BuildWorkflow. \`check-release-gate\` requires every job there to appear in \`release.needs\`, and a notifier job is SKIPPED on a tag push -- which skips the release with it."
    else
        Ok "the notifier is not a job in $BuildWorkflow, so it cannot gate the release"
    fi
}

# ---------------------------------------------------------------------------
# Self-test.
SelfTest() {
    local scratch status=0 cases=0
    scratch="$(mktemp -d)" || { echo "cannot create a scratch directory" >&2; exit 2; }
    # shellcheck disable=SC2064  # expand $scratch now, not at trap time
    trap "rm -rf '$scratch'" EXIT

    # A STAND-IN table, pinned to the state the runs below were captured in
    # (2026-09-04), not a copy of the live one. Do not sync it: the cases assert
    # what the decider does with a GIVEN required set, so reading the production
    # table would make every verdict here move whenever a context is promoted --
    # which is how `clang-asan-ubsan` and `clang-tsan` joined it (#408, #629)
    # without a single case changing meaning. The production table is READ by the
    # decider itself, and `$Decider reads the required-context list` above is what
    # asserts that.
    local required="${scratch}/required.sh"
    cat > "$required" <<'REQ'
RequiredContexts=(
    "Windows-cl-release|.github/workflows/build.yml"
    "Windows-clangcl-release|.github/workflows/build.yml"
    "Linux-clang-release|.github/workflows/build.yml"
    "Linux-gcc-release|.github/workflows/build.yml"
    "macOS-clang-release|.github/workflows/build.yml"
    "clang-tidy|.github/workflows/build.yml"
    "sccache smoke (memcached text)|.github/workflows/build.yml"
    "sccache smoke (memcached binary)|.github/workflows/build.yml"
    "sccache smoke (redis RESP2)|.github/workflows/build.yml"
    "Check C++ style|.github/workflows/build.yml"
    "Require a type label|.github/workflows/pr-labels.yml"
)
REQ

    # The 24 job names below are the CAPTURED listing of merge-group run
    # 33749317968, read on 2026-09-04 -- and the other two runs staged here were
    # verified to carry the byte-identical name set, so one list serves all three
    # honestly. Each case names which job the real run concluded `failure` on;
    # everything else concluded `success` except `Draft GitHub release`, which is
    # skipped on a queue entry. The job URLs are synthesised, and that is stated
    # rather than glossed: the decision carries a URL through untouched and
    # decides nothing from it.
    CapturedRun() {
        local failing="$1" name
        for name in \
            "Check the release gate covers every job" \
            "Decide what this change can affect" \
            "Code coverage" \
            "clang-asan-ubsan" \
            "Linux-gcc-release" \
            "Docker image" \
            "compile-cache E2E (Linux)" \
            "Check C++ style" \
            "Windows-cl-debug" \
            "macOS-clang-release" \
            "Windows-clangcl-release" \
            "compile-cache E2E (Windows)" \
            "Package (Linux .deb/.rpm)" \
            "clang-tsan" \
            "clang-tidy" \
            "Package (macOS .pkg)" \
            "Windows-cl-release" \
            "Package (Windows .msi)" \
            "Linux-clang-release" \
            "sccache smoke (memcached text)" \
            "sccache smoke (memcached binary)" \
            "sccache smoke (redis RESP2)" \
            "fastcache-cc smoke (compile-cache 0xFC)" \
            "Draft GitHub release"
        do
            if [[ "$name" == "$failing" ]]; then
                printf '%s\tcompleted\tfailure\thttps://example.invalid/job\n' "$name"
            elif [[ "$name" == "Draft GitHub release" ]]; then
                printf '%s\tcompleted\tskipped\thttps://example.invalid/job\n' "$name"
            else
                printf '%s\tcompleted\tsuccess\thttps://example.invalid/job\n' "$name"
            fi
        done
    }

    # `bash "$Decider"` rather than executing it: these scripts are mode 644 in
    # git and ctest runs them as `bash <path>`. A bare exec exits 126 having run
    # nothing, and every negative case then passes because the SHELL refused --
    # measured, in this branch, where it made eight cases green while testing
    # nothing.
    Decide() {
        FASTCACHED_REQUIRED_CONTEXTS_FILE="$required" \
            bash "$Decider" "$1" "https://example.invalid/run" "$2"
    }

    Case() {
        # $1 = description, $2 = want-rows|want-none|want-refuse,
        # $3 = event, $4 = record file, $5 = expected substring (optional)
        local what="$1" want="$2" event="$3" record="$4" expect="${5:-}"
        cases=$((cases + 1))
        local out err got=0
        err="${scratch}/err.txt"
        out="$(Decide "$event" "$record" 2>"$err")" || got=$?

        local rows=0
        [[ -n "$out" ]] && rows="$(printf '%s' "$out" | grep -c '' || true)"

        local verdict="refuse"
        if [[ "$got" -eq 0 && "$rows" -gt 0 ]]; then verdict="rows"
        elif [[ "$got" -eq 0 ]]; then verdict="none"
        fi

        local wanted="${want#want-}"
        if [[ "$verdict" != "$wanted" ]]; then
            echo "  FAIL  (wanted $wanted, got $verdict, exit $got, $rows row(s)) $what" >&2
            sed 's/^/        /' "$err" >&2
            status=1
            return
        fi
        if [[ -n "$expect" ]] && ! grep -qF -- "$expect" "$err" && [[ "$out" != *"$expect"* ]]; then
            echo "  FAIL  $what: neither the output nor the narration mentions '$expect'" >&2
            sed 's/^/        /' "$err" >&2
            status=1
            return
        fi
        echo "  ok    ($wanted) $what"
    }

    # --- the three real runs -------------------------------------------------
    CapturedRun "Package (macOS .pkg)" > "${scratch}/r-pkg.tsv"
    Case "run 33749317968: Package (macOS .pkg) failed, is unrequired, #669 merged -- REPORTED" \
        want-rows merge_group "${scratch}/r-pkg.tsv" "Package (macOS .pkg)"

    CapturedRun "macOS-clang-release" > "${scratch}/r-req.tsv"
    Case "run 33782559943: macOS-clang-release failed and IS required -- the queue ejected #686, so nothing to report" \
        want-none merge_group "${scratch}/r-req.tsv" "IS a required context"

    CapturedRun "Windows-cl-debug" > "${scratch}/r-leg.tsv"
    Case "run 33760836218: Windows-cl-debug is an UNREQUIRED LEG of the same matrix job as the required Windows-cl-release -- REPORTED" \
        want-rows merge_group "${scratch}/r-leg.tsv" "Windows-cl-debug"

    # --- everything else -----------------------------------------------------
    Case "a pull-request run is not a defect and not a silence: it says so" \
        want-none pull_request "${scratch}/r-pkg.tsv" "not merge_group"

    CapturedRun "" > "${scratch}/r-green.tsv"
    Case "an all-green run reports nothing" want-none merge_group "${scratch}/r-green.tsv"

    : > "${scratch}/r-empty.tsv"
    Case "an EMPTY record is refused: zero rows is the absence of a verdict" \
        want-refuse merge_group "${scratch}/r-empty.tsv" "absence of a verdict"

    Case "a MISSING record is refused: a listing that could not be taken is not a listing of nothing" \
        want-refuse merge_group "${scratch}/r-nonexistent.tsv" "could not be taken"

    # `gh` renders a RUNNING job's conclusion as the EMPTY STRING, not null. A
    # `!= null` predicate once classified every in-progress job in this
    # repository as FAILED, on a byte-identical tree.
    printf 'Code coverage\tin_progress\t\thttps://example.invalid/job\n' > "${scratch}/r-running.tsv"
    printf 'clang-tidy\tcompleted\tsuccess\thttps://example.invalid/job\n' >> "${scratch}/r-running.tsv"
    Case "an UNFINISHED job in a completed run is refused, never sorted into failed or passed" \
        want-refuse merge_group "${scratch}/r-running.tsv" "still 'in_progress'"

    printf 'Code coverage\tcompleted\tsome_new_value\thttps://example.invalid/job\n' > "${scratch}/r-unknown.tsv"
    printf 'clang-tidy\tcompleted\tsuccess\thttps://example.invalid/job\n' >> "${scratch}/r-unknown.tsv"
    Case "a conclusion not in the table is refused BY NAME, not bucketed" \
        want-refuse merge_group "${scratch}/r-unknown.tsv" "some_new_value"

    # Absence of the negative is not the positive.
    printf 'Code coverage\tcompleted\tskipped\thttps://example.invalid/job\n' > "${scratch}/r-nothing.tsv"
    printf 'clang-tidy\tcompleted\tcancelled\thttps://example.invalid/job\n' >> "${scratch}/r-nothing.tsv"
    Case "nothing failed and nothing SUCCEEDED either: that is a run that did nothing, not a clean one" \
        want-refuse merge_group "${scratch}/r-nothing.tsv" "did nothing"

    printf '\tcompleted\tsuccess\thttps://example.invalid/job\n' > "${scratch}/r-noname.tsv"
    Case "a row with no job name is refused" want-refuse merge_group "${scratch}/r-noname.tsv" "no job name"

    # An empty required table would make every failure look unrequired, so the
    # notifier would be loud about the ones the queue already surfaced.
    local emptyRequired="${scratch}/empty-required.sh"
    : > "$emptyRequired"
    local got=0
    FASTCACHED_REQUIRED_CONTEXTS_FILE="$emptyRequired" \
        bash "$Decider" merge_group "https://example.invalid/run" "${scratch}/r-pkg.tsv" \
        >/dev/null 2>"${scratch}/err.txt" || got=$?
    if [[ "$got" -ne 0 ]] && grep -q "read 0 required contexts" "${scratch}/err.txt"; then
        echo "  ok    (refuse) an empty required-context table is refused, not read as 'nothing is required'"
    else
        echo "  FAIL  (exit $got) an empty required-context table was accepted" >&2
        sed 's/^/        /' "${scratch}/err.txt" >&2
        status=1
    fi

    # --- the issue reporter, against a stub gh -------------------------------
    #
    # A stub on PATH, the same shape as `tsan-gate-selftest`'s stub `nm`. What is
    # under test is the DECISION -- create, comment, or refuse -- and in
    # particular the direction the obvious spelling gets wrong: a query that
    # FAILED and a query that found NOTHING render identically, and the reporter
    # then opens a duplicate issue on every single run.
    local stubDir="${scratch}/bin"
    mkdir -p "$stubDir"
    printf 'body\n' > "${scratch}/body.md"

    StubGh() {
        # $1 = what `gh issue list` prints, $2 = its exit status
        cat > "${stubDir}/gh" <<STUB
#!/bin/bash
case "\$1 \$2" in
  "issue list") printf '%s' '$1'; exit $2 ;;
  "issue create") echo "CREATED"; exit 0 ;;
  "issue comment") echo "COMMENTED \$3"; exit 0 ;;
esac
echo "unexpected gh invocation: \$*" >&2; exit 99
STUB
        chmod +x "${stubDir}/gh"
    }

    ReportCase() {
        # $1 = description, $2 = want-ok|want-refuse, $3 = expected substring
        local what="$1" want="$2" expect="$3" out got=0
        cases=$((cases + 1))
        out="$(PATH="${stubDir}:$PATH" bash "$Reporter" "a title" "${scratch}/body.md" type/bug 2>&1)" || got=$?
        local verdict="ok"
        [[ "$got" -ne 0 ]] && verdict="refuse"
        if [[ "$verdict" != "${want#want-}" ]]; then
            echo "  FAIL  (wanted ${want#want-}, got $verdict, exit $got) $what" >&2
            printf '%s\n' "$out" | sed 's/^/        /' >&2
            status=1
            return
        fi
        if [[ "$out" != *"$expect"* ]]; then
            echo "  FAIL  $what: output does not mention '$expect'" >&2
            printf '%s\n' "$out" | sed 's/^/        /' >&2
            status=1
            return
        fi
        echo "  ok    (${want#want-}) $what"
    }

    StubGh '3	' 0
    ReportCase "no title match: a new issue is opened" want-ok "CREATED"

    StubGh '3	41' 0
    ReportCase "an exact title match: a comment, not a second issue" want-ok "COMMENTED 41"

    StubGh '' 1
    ReportCase "a FAILED listing is refused, never read as 'no match' (which opens a duplicate every run)" \
        want-refuse "did not answer"

    StubGh 'parse error near unexpected token' 0
    ReportCase "a listing whose SHAPE is unrecognised is refused before anything is read out of it" \
        want-refuse "not '<count><TAB><number>'"

    StubGh '500	' 0
    ReportCase "a listing AT its cap says so: a real answer about a set that is not the whole set" \
        want-ok "::warning::"

    # --- the shape rules, against generated workflows ------------------------
    #
    # Generated rather than edited from a copy of the real one: an edit matching
    # no anchor changes nothing and reports success. Each case asserts its
    # workflow DIFFERS from the correct one, so a knob that stopped working is a
    # failure rather than a pass.
    #
    #   $1 = output file
    #   $2 = trigger:   workflow_run | wrong-workflow | requested | branch-filtered
    #   $3 = condition: cancelled | always | none
    #   $4 = concurrency: plain | cancelling
    #   $5 = permissions: issues | readonly
    #   $6 = steps:     both | no-decider | no-reporter
    #   $7 = a comment naming every forbidden spelling: yes | no
    GenerateWorkflow() {
        local out="$1" trigger="$2" condition="$3" concurrency="$4"
        local permissions="$5" steps="$6" comment="$7"
        {
            echo "name: Merge-group failure report"
            [[ "$comment" == "yes" ]] && {
                echo "# This header names always() and cancel-in-progress and"
                echo "# branches: on purpose. A comment is not a setting."
            }
            echo "on:"
            echo "  workflow_run:"
            case "$trigger" in
                wrong-workflow)   echo '    workflows: ["Buld"]' ;;
                *)                echo '    workflows: ["Build"]' ;;
            esac
            case "$trigger" in
                requested) echo "    types: [requested, completed]" ;;
                *)         echo "    types: [completed]" ;;
            esac
            [[ "$trigger" == "branch-filtered" ]] && echo "    branches: ['gh-readonly-queue/**']"
            echo "concurrency:"
            echo "  group: merge-group-report-\${{ github.event.workflow_run.id }}"
            [[ "$concurrency" == "cancelling" ]] && echo "  cancel-in-progress: true"
            echo "permissions:"
            echo "  contents: read"
            [[ "$permissions" == "issues" ]] && echo "  issues: write"
            echo "jobs:"
            echo "  report:"
            echo "    runs-on: ubuntu-24.04"
            case "$condition" in
                cancelled) echo "    if: \${{ !cancelled() }}" ;;
                always)    echo "    if: \${{ always() }}" ;;
            esac
            echo "    steps:"
            [[ "$steps" != "no-decider" ]]  && echo "      - run: scripts/ci-merge-group-report.sh x y z"
            [[ "$steps" != "no-reporter" ]] && echo "      - run: scripts/ci-report-issue.sh t b"
        } > "$out"
        # Explicit, and load-bearing under `set -e`: the group above ends in a
        # `[[ ... ]] && echo` whose status is 1 whenever the condition is false,
        # so without this the `no-reporter` case took the whole self-test down
        # after eight cases -- exit 1, no case named, indistinguishable from a
        # real failure. A fixture that stops early must not look like one that
        # judged something.
        return 0
    }

    local correctWf="${scratch}/correct.yml"
    GenerateWorkflow "$correctWf" workflow_run cancelled plain issues both yes

    ShapeCase() {
        # $1 = description, $2 = want-pass|want-fail, $3 = file
        local what="$1" want="$2" file="$3" out got=0
        cases=$((cases + 1))
        if [[ "$file" != "$correctWf" ]] && diff -q "$correctWf" "$file" >/dev/null 2>&1; then
            echo "  FAIL  '$what' generated a workflow identical to the correct one" >&2
            status=1
            return
        fi
        out="$(bash "$0" --workflow "$file" 2>&1)" || got=$?
        if [[ "$want" == "want-pass" && "$got" -eq 0 ]] || [[ "$want" == "want-fail" && "$got" -ne 0 ]]; then
            echo "  ok    ($want) $what"
        else
            echo "  FAIL  ($want, exit $got) $what" >&2
            printf '%s\n' "$out" | sed 's/^/        /' >&2
            status=1
        fi
    }

    # The baseline, and it carries a comment naming always(), cancel-in-progress
    # and branches: -- so it is simultaneously the case that a COMMENT neither
    # satisfies nor refuses a rule. Both directions of the mistake this branch
    # made twice.
    ShapeCase "the correct shape passes, with a comment naming every forbidden spelling" want-pass "$correctWf"

    GenerateWorkflow "${scratch}/wf.yml" wrong-workflow cancelled plain issues both yes
    ShapeCase "a misspelled workflow name is refused (it would produce no run and no error)" want-fail "${scratch}/wf.yml"

    GenerateWorkflow "${scratch}/wf.yml" requested cancelled plain issues both yes
    ShapeCase "a trigger including 'requested' is refused (no conclusions to read yet)" want-fail "${scratch}/wf.yml"

    GenerateWorkflow "${scratch}/wf.yml" branch-filtered cancelled plain issues both yes
    ShapeCase "a branches: filter is refused" want-fail "${scratch}/wf.yml"

    GenerateWorkflow "${scratch}/wf.yml" workflow_run always plain issues both yes
    ShapeCase "always() is refused in favour of !cancelled()" want-fail "${scratch}/wf.yml"

    GenerateWorkflow "${scratch}/wf.yml" workflow_run none plain issues both yes
    ShapeCase "no status function at all is refused" want-fail "${scratch}/wf.yml"

    GenerateWorkflow "${scratch}/wf.yml" workflow_run cancelled cancelling issues both yes
    ShapeCase "cancel-in-progress is refused: it discards a report" want-fail "${scratch}/wf.yml"

    GenerateWorkflow "${scratch}/wf.yml" workflow_run cancelled plain readonly both yes
    ShapeCase "no issues: write is refused" want-fail "${scratch}/wf.yml"

    GenerateWorkflow "${scratch}/wf.yml" workflow_run cancelled plain issues no-decider yes
    ShapeCase "the decision moved into the YAML, where nothing can test it" want-fail "${scratch}/wf.yml"

    GenerateWorkflow "${scratch}/wf.yml" workflow_run cancelled plain issues no-reporter yes
    ShapeCase "nothing opens the issue" want-fail "${scratch}/wf.yml"

    # And a workflow with NO comment at all still passes, so the rules are
    # satisfied by the settings rather than by the prose beside them.
    GenerateWorkflow "${scratch}/wf-nocomment.yml" workflow_run cancelled plain issues both no
    ShapeCase "the correct shape passes with no comments at all" want-pass "${scratch}/wf-nocomment.yml"

    # The count is printed rather than compared against a number restated here:
    # a second copy of the expected total is a second thing to be wrong. What it
    # buys is that a run cut short -- by `set -e`, by a missing tool -- ends
    # without this line, so truncation cannot be mistaken for a verdict.
    if [[ "$status" -ne 0 ]]; then
        echo "check-merge-group-report: self-test FAILED after $cases case(s)" >&2
        exit 1
    fi
    echo "check-merge-group-report: self-test passed, $cases case(s)"
}

# ---------------------------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --self-test) SelfTest; exit 0 ;;
        --workflow)  [[ $# -ge 2 ]] || { echo "--workflow needs a file" >&2; exit 2; }
                     Workflow="$2"; shift 2 ;;
        *)           echo "usage: $(basename "${BASH_SOURCE[0]}") [--workflow FILE] [--self-test]" >&2; exit 2 ;;
    esac
done

Shape

if [[ $problems -gt 0 ]]; then
    echo "check-merge-group-report: $problems problem(s); a merge-group failure would go unreported" >&2
    exit 1
fi
echo "check-merge-group-report: the notifier is wired up"
