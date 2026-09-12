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

# The runner's own vocabulary: names a step may read without declaring, because
# GitHub sets them. An ALLOWLIST and not a pattern, so it fails CLOSED -- a
# `GITHUB_`-prefixed regex would have admitted nothing here, but the general
# shape (an exclusion list bets on the world's layout; an inclusion list states
# your own) is what stopped `EVENT` being waved through by a broad rule.
# Both scans over `scripts/` read this list as CODE, and it is data: it spells
# `BASHPID` (bash 4.0+, and inert on macOS) and `SECONDS` (a clock reading, never
# a bound -- #1066), neither of which this script executes. A declared REGION and
# never a whole-file exemption, for the reason `check-e2e-helpers.sh` gives about
# its own token table: exempting the file would make the one file a scan can
# never read the one most likely to break its rule.
# bash32-scan: data-begin
# seconds-scan: data-begin
RunnerProvided="HOME PATH PWD SHELL USER TMPDIR LANG IFS OSTYPE HOSTNAME
RUNNER_TEMP RUNNER_OS RUNNER_ARCH RUNNER_TOOL_CACHE RUNNER_WORKSPACE RUNNER_DEBUG
GITHUB_OUTPUT GITHUB_ENV GITHUB_PATH GITHUB_STEP_SUMMARY GITHUB_WORKSPACE
GITHUB_REPOSITORY GITHUB_SHA GITHUB_REF GITHUB_REF_NAME GITHUB_EVENT_NAME
GITHUB_EVENT_PATH GITHUB_RUN_ID GITHUB_RUN_NUMBER GITHUB_ACTOR GITHUB_JOB
GITHUB_SERVER_URL GITHUB_API_URL GITHUB_HEAD_REF GITHUB_BASE_REF GITHUB_TOKEN
BASH_VERSION BASH_SOURCE BASH_REMATCH BASHPID FUNCNAME LINENO RANDOM SECONDS PIPESTATUS"
# seconds-scan: data-end
# bash32-scan: data-end

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

    # The two halves of #774's push report, both of which are wiring the decision
    # script cannot check about itself.
    #
    # The BRANCH, because the decider refuses a push run without one rather than
    # assuming master -- so a workflow that stopped passing it would turn every
    # master push into a hard failure of this notifier. That is the safe direction
    # and it is still a break, and nothing else would say which.
    grep -q 'HEAD_BRANCH' <<< "$(NonComment "$Workflow")" \
        && Ok "it passes the head branch, so a push to a scratch branch is not reported as master" \
        || Fail "$Workflow does not pass \`HEAD_BRANCH\`; $Decider refuses a push run with no branch rather than guessing, so every master push would fail this notifier"

    # And the de-duplication mode, which is the whole of #774's objection to
    # reporting pushes: a context that goes on failing on every subsequent push
    # would otherwise comment on one issue indefinitely and say nothing new. A
    # push report is a TRANSITION; a merge-group report is an EVENT, so it keeps
    # commenting and must NOT set this.
    grep -q 'FASTCACHED_REPORT_ONLY_IF_NEW' <<< "$(NonComment "$Workflow")" \
        && Ok "a push report is opened once and never updated" \
        || Fail "$Workflow never sets \`FASTCACHED_REPORT_ONLY_IF_NEW\`; a context failing on every push to master would then comment on one report forever, which is #774's own objection to reporting pushes at all"

    # And it must not be a job in build.yml, for the reason check-release-gate
    # exists: a notifier job is skipped on a tag push, always, and would skip the
    # release with it.
    if grep -qE '^  [A-Za-z0-9_-]*merge-group-report[A-Za-z0-9_-]*:' <<< "$(NonComment "$BuildWorkflow")"; then
        Fail "the notifier has become a JOB in $BuildWorkflow. \`check-release-gate\` requires every job there to appear in \`release.needs\`, and a notifier job is SKIPPED on a tag push -- which skips the release with it."
    else
        Ok "the notifier is not a job in $BuildWorkflow, so it cannot gate the release"
    fi

    # Per STEP, and last, because every rule above is a whole-file grep -- which
    # is exactly how #1174 passed them all.
    StepEnvComplete
}

# ---------------------------------------------------------------------------
# Every name a step's script READS is one that step's own `env:` DEFINES.
#
# A step's environment is its own: a name declared by the step next door is not
# in scope, and `${{ ... }}` is substituted before bash ever sees the script, so
# nothing about the file's appearance says which names will exist. #1174 is what
# that cost -- `EVENT` was defined on the `decide` step and read by the reporting
# step, which died on its first line under `set -u`:
#
#     line 7: EVENT: unbound variable
#
# The notifier therefore opened no report in its entire life. Measured over 200
# runs at the time of the fix: 190 concluded `success` on the `reportable == 0`
# path, which never enters that step, and all 10 that had something to report
# died on that line. #684's whole subject is a failure nobody was told about, and
# it was announcing itself as a red run in the Actions tab that nothing points at
# -- #774's complaint one door further out.
#
# **Why no rule here caught it.** The rule that motivated the `$EVENT` read is a
# whole-file `grep -q 'FASTCACHED_REPORT_ONLY_IF_NEW'`, and the line satisfying it
# is IN the step that cannot run. A rule satisfied by a line that never executes
# is the same defect as a rule satisfied by prose, which this file's header
# already records making twice -- one level up, in the file that recorded it. So
# this rule is per STEP and never whole-file; that is the whole of it.
#
# It applies to every `run:`, not only the ones turning on `set -u`. Without `-u`
# an undefined name expands to EMPTY and the branch is silently taken the wrong
# way, which is the worse of the two failures and the one nothing would report.
#
# ## What it does NOT cover
#
# Names a step could legitimately have without an `env:` row of its own, all of
# which it accepts:
#
#   * workflow-level and job-level `env:`, which do propagate -- collected and
#     honoured, so adding one is not refused;
#   * assignments the script makes itself, including `export`, `local`,
#     `declare`, `readonly`, `mapfile`/`readarray`, `read`, and `for x in`;
#   * the runner's own vocabulary (`$RUNNER_TEMP`, `$GITHUB_OUTPUT`, `$HOME` ...),
#     which is an ALLOWLIST below and therefore fails CLOSED: a runner variable
#     nobody has listed is refused and gets added deliberately, where an
#     open-ended pattern would have admitted `EVENT`.
#
# And what it genuinely cannot see, so that nobody reads a pass as more than it
# is: a name reached by `eval` or indirect expansion, one an action's `outputs`
# supply through `${{ steps.x.outputs.y }}` (substituted, so invisible here), one
# a sourced file defines, and `${#arr[@]}`, whose `#` this deliberately does not
# treat as a read. It is one workflow's rule, not the repository's -- 28 `run:`
# blocks across six workflow files are outside it, and generalising the scan is
# its own ticket rather than a wider glob bolted on here.
StepEnvComplete() {
    local report scanned violations
    report="$(
        # The list is FLATTENED before it becomes an `awk -v` value. BSD awk --
        # macOS ships it, and this check is in the default ctest set -- refuses a
        # raw newline in one, so the multi-line definition above would have made
        # the program never run on exactly one platform (#1153 is the same awk
        # and the same mistake, one lane over).
        awk -v provided="$(printf '%s' "$RunnerProvided" | tr '\n' ' ')" '
        function indentOf(s,   n) { n = match(s, /[^ ]/); return n ? n - 1 : length(s) }

        function collectDefs(s,   t, rest, name, i, declarator) {
            t = s
            sub(/^[ \t]+/, "", t)

            # A DECLARATOR names several variables at once; anything else names
            # at most one, and only with an `=` after it. Taking bare words off
            # any line would have made every command word a definition -- `echo`,
            # `gh`, `api` -- which is a model MORE PERMISSIVE than bash, and this
            # check exists because a permissive model waves through the one read
            # that matters. Narrow on purpose.
            declarator = 0
            if (t ~ /^(export|readonly|local|declare|typeset)[ \t]/) {
                declarator = 1
                sub(/^(export|readonly|local|declare|typeset)[ \t]+(-[A-Za-z]+[ \t]+)*/, "", t)
            }
            if (declarator) {
                while (match(t, /^[A-Za-z_][A-Za-z0-9_]*/)) {
                    name = substr(t, RSTART, RLENGTH)
                    rest = substr(t, RSTART + RLENGTH)
                    defs[name] = 1
                    if (rest !~ /^[ \t]/) break
                    sub(/^[ \t]+/, "", rest)
                    t = rest
                }
            } else if (match(t, /^[A-Za-z_][A-Za-z0-9_]*=/)) {
                defs[substr(t, RSTART, RLENGTH - 1)] = 1
            }
            # The two spellings of the array-reading builtin, NAMED so a
            # workflow using one is understood. Naming a construct is not using
            # it, and this file is bash 3.2 throughout.
            # bash32-scan: data-begin
            if (match(s, /(mapfile|readarray)[ \t]+(-[A-Za-z]+[ \t]+)*[A-Za-z_][A-Za-z0-9_]*/)) {
            # bash32-scan: data-end
                rest = substr(s, RSTART, RLENGTH)
                if (match(rest, /[A-Za-z_][A-Za-z0-9_]*$/)) defs[substr(rest, RSTART, RLENGTH)] = 1
            }
            # for NAME in ... / while read NAME NAME ...
            if (match(s, /(^|[^A-Za-z0-9_])for[ \t]+[A-Za-z_][A-Za-z0-9_]*[ \t]+in([ \t]|$)/)) {
                rest = substr(s, RSTART, RLENGTH)
                sub(/^[^A-Za-z0-9_]*for[ \t]+/, "", rest)
                if (match(rest, /^[A-Za-z_][A-Za-z0-9_]*/)) defs[substr(rest, RSTART, RLENGTH)] = 1
            }
            i = index(s, "read ")
            if (i > 0 && (i == 1 || substr(s, i - 1, 1) !~ /[A-Za-z0-9_]/)) {
                rest = substr(s, i + 5)
                while (match(rest, /^-[A-Za-z]+[ \t]+/)) { rest = substr(rest, RLENGTH + 1) }
                while (match(rest, /^[A-Za-z_][A-Za-z0-9_]*/)) {
                    defs[substr(rest, RSTART, RLENGTH)] = 1
                    rest = substr(rest, RSTART + RLENGTH)
                    if (rest !~ /^[ \t]/) break
                    sub(/^[ \t]+/, "", rest)
                }
            }
        }

        # A quoted YAML scalar, with the quote named rather than written: this
        # whole program is one shell-quoted string and an apostrophe would end
        # it. `sprintf("%c", 39)` is also what keeps this off `\047`, which is
        # not portable across every awk this check runs on.
        function unquote(t,   sq, first, last) {
            sq = sprintf("%c", 39)
            first = substr(t, 1, 1)
            last = substr(t, length(t), 1)
            if (length(t) >= 2 && first == last && (first == "\"" || first == sq))
                return substr(t, 2, length(t) - 2)
            return t
        }

        function collectRefs(s,   t, name) {
            # A `${{ ... }}` expression is substituted before bash sees it, so it
            # is not a read. Removed first, or its inner text is scanned as one.
            gsub(/\$\{\{[^}]*\}\}/, "", s)
            t = s
            while (match(t, /\$\{?[A-Za-z_][A-Za-z0-9_]*\}?/)) {
                name = substr(t, RSTART, RLENGTH)
                gsub(/[${}]/, "", name)
                refs[name] = 1
                t = substr(t, RSTART + RLENGTH)
            }
        }

        function flush(   i, name, missing, n) {
            if (!hasRun) return
            scanned++
            for (i = 0; i < nbody; i++) collectDefs(body[i])
            for (i = 0; i < nbody; i++) {
                if (body[i] ~ /^[ \t]*#/) continue
                collectRefs(body[i])
            }
            missing = ""; n = 0
            for (name in refs) {
                if (name in stepEnv) continue
                if (name in globalEnv) continue
                if (name in defs) continue
                if (name in providedSet) continue
                missing = missing (n++ ? ", " : "") "$" name
            }
            if (n) printf "VIOLATION\t%d\t%s\t%s\n", stepLine, stepName, missing
        }

        function resetStep() {
            split("", stepEnv); split("", defs); split("", refs); split("", body)
            nbody = 0; hasRun = 0; stepName = "(unnamed)"; stepLine = NR
        }

        BEGIN {
            n = split(provided, p, " ")
            for (i = 1; i <= n; i++) if (p[i] != "") providedSet[p[i]] = 1
            resetStep(); inStep = 0; inRun = 0; inEnv = 0
        }

        {
            line = $0
            blank = (line ~ /^[ \t]*$/)
            ind = indentOf(line)

            if (inRun) {
                if (blank || ind > runIndent) { body[nbody++] = line; next }
                inRun = 0
            }
            if (inEnv) {
                if (blank) next
                if (ind > envIndent) {
                    if (match(line, /^[ ]*[A-Za-z_][A-Za-z0-9_]*[ \t]*:/)) {
                        name = line
                        sub(/^[ ]*/, "", name); sub(/[ \t]*:.*$/, "", name)
                        if (envIsStep) stepEnv[name] = 1; else globalEnv[name] = 1
                    }
                    next
                }
                inEnv = 0
            }

            # A steps list item carries its FIRST key on the dash line, so
            # `- env:` and `- run: |` are a step boundary AND a key. Turning the
            # dash into two spaces re-dispatches it through the key handling
            # below, rather than duplicating that handling here -- which the first
            # version did, and it therefore read the decider step of the OWN
            # fixture of this check as declaring no `env:` at all. An
            # apostrophe cannot appear in this comment either: the whole awk
            # program is one shell-quoted string, and one would end it.
            if (line ~ /^[ ]*- /) {
                flush(); resetStep(); inStep = 1; stepIndent = ind
                sub(/- /, "  ", line)
                ind = indentOf(line)
            }

            # The step NAME, which is the only thing telling a reader which step
            # to edit: a refusal naming "(unnamed)" is a guard whose remedy text
            # cannot be acted on.
            if (inStep && ind > stepIndent && match(line, /^[ ]*name[ \t]*:[ \t]*/)) {
                t = substr(line, RLENGTH + 1)
                sub(/[ \t]+$/, "", t); t = unquote(t)
                if (t != "") stepName = t
                next
            }

            if (match(line, /^[ ]*env[ \t]*:[ \t]*$/)) {
                inEnv = 1; envIndent = ind
                envIsStep = (inStep && ind > stepIndent)
                next
            }
            if (match(line, /^[ ]*run[ \t]*:[ \t]*\|/)) {
                hasRun = 1; inRun = 1; runIndent = ind; next
            }
            if (match(line, /^[ ]*run[ \t]*:[ \t]*[^ \t|>]/)) {
                hasRun = 1
                t = line; sub(/^[ ]*run[ \t]*:[ \t]*/, "", t)
                body[nbody++] = t
                next
            }
            # A key at or above the step indent ends the step for env purposes
            # only; the step itself is closed by the next `- ` or by EOF.
        }

        END { flush(); printf "SCANNED\t%d\n", scanned }
        ' "$Workflow"
    )"

    scanned="$(printf '%s\n' "$report" | awk -F'\t' '$1 == "SCANNED" { print $2 }')"
    violations="$(printf '%s\n' "$report" | awk -F'\t' '$1 == "VIOLATION"' || true)"

    # A scan that matched nothing agrees perfectly with a clean file, so the
    # count is asserted before its silence is read as a pass. The positive
    # control is the self-test's `no-event-env` case, which must be REFUSED.
    if [[ -z "$scanned" || "$scanned" -eq 0 ]]; then
        Fail "$Workflow: found no \`run:\` block at all. Either the file has no steps or this rule stopped parsing it -- and a rule that parses nothing reports clean, which is the state it exists to remove."
        return
    fi

    if [[ -n "$violations" ]]; then
        while IFS="$(printf '\t')" read -r _ line name missing; do
            [[ -n "$name" ]] || continue
            Fail "$Workflow:$line step \"$name\" reads $missing, which its own \`env:\` does not define. A step's environment is its own -- a sibling step's \`env:\` does not lend it one -- so under \`set -u\` the step dies on that line and under no \`-u\` the name expands to EMPTY and the branch is silently taken the wrong way. That is #1174: #684's notifier read \$EVENT from the decide step's \`env:\` and opened no report in its entire life. Add the row to THIS step's \`env:\`."
        done <<< "$violations"
    else
        Ok "every name each of the $scanned \`run:\` block(s) reads is defined by its own step"
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
# required-context-table: fixture -- a stand-in pinned to the state these cases were
# captured in (2026-09-04). Deliberately a strict SUBSET of the live table, which is
# what makes it the dangerous decoy of #1360: every row is a real context name, so a
# reader that lands here reports a true statement about a set nobody asked about.
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

    # `bash "$Decider"` rather than executing it. The reason is no longer the
    # mode -- `ctest -R script-modes` requires every tracked shell script with a
    # shebang to be 100755 (#720; #1033 corrected this comment) -- but the rule
    # stands on what a chmod never covered: a bare exec that fails to START, for
    # ANY reason, runs nothing, and every negative case then passes because the
    # SHELL refused. Measured in #723, where it made eight cases green while
    # testing nothing.
    Decide() {
        FASTCACHED_REQUIRED_CONTEXTS_FILE="$required" \
            bash "$Decider" "$1" "https://example.invalid/run" "$2" ${3+"$3"} ${4+"$4"}
    }

    Case() {
        # $1 = description, $2 = want-rows|want-none|want-refuse,
        # $3 = event, $4 = record file, $5 = expected substring (optional)
        local what="$1" want="$2" event="$3" record="$4" expect="${5:-}" branch="${6:-}" conclusion="${7:-}"
        cases=$((cases + 1))
        local out err got=0
        err="${scratch}/err.txt"
        out="$(Decide "$event" "$record" ${branch:+"$branch"} ${conclusion:+"$conclusion"} 2>"$err")" || got=$?

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
    # **The expectation MOVED rather than loosened.** It read "not merge_group",
    # which was the whole of the old decision; `pull_request` is now a row of
    # `EventPolicy` saying `none`, so what has to be asserted is that the decision
    # is DELIBERATE and named, not merely that nothing came out. A case checking
    # only `want-none` would pass on a build that had silently dropped the event
    # table altogether.
    Case "a pull-request run reports nothing DELIBERATELY, and says why" \
        want-none pull_request "${scratch}/r-pkg.tsv" "where somebody is already looking"

    Case "an event with no row at all is answered 'not applicable', not 'nothing wrong'" \
        want-none schedule "${scratch}/r-pkg.tsv" "not in this script's event table"

    # --- a push to master: #774 ---------------------------------------------
    #
    # The SAME record that is reported under `merge_group` because the context is
    # unrequired, and a record whose failure IS required -- which a queue does not
    # report and a push must. Both, because one alone cannot tell "the push policy
    # works" from "the push policy is the queue policy under another name".
    Case "a push to master reports an unrequired failure, exactly as a queue does" \
        want-rows push "${scratch}/r-pkg.tsv" "Package (macOS .pkg)" master

    Case "a push to master ALSO reports a REQUIRED failure, which a queue would not" \
        want-rows push "${scratch}/r-req.tsv" "macOS-clang-release" master

    Case "a push to a scratch branch is somebody's own red, and is not reported" \
        want-none push "${scratch}/r-pkg.tsv" "belongs to whoever pushed it" fix-ci

    # The guess this refuses is the dangerous one: defaulting an absent branch to
    # master would report every `fix-ci` push, which is the branch CI experiments
    # are EXPECTED to fail on -- a notifier that cries wolf is one that gets muted.
    Case "a push run with no branch is REFUSED rather than assumed to be master" \
        want-refuse push "${scratch}/r-pkg.tsv" "refusing to guess"


    CapturedRun "" > "${scratch}/r-green.tsv"
    Case "an all-green run reports nothing" want-none merge_group "${scratch}/r-green.tsv"
    # Here rather than beside the other push cases, because the green record is
    # created on the line above: the first placement referenced it earlier in the
    # file and the decider refused a MISSING record -- correctly, and loudly, which
    # is the behaviour its own "a listing that could not be taken is not a listing
    # of nothing" rule exists for.
    Case "an all-green push to master reports nothing" \
        want-none push "${scratch}/r-green.tsv" "nothing failed" master

    # A run nobody let FINISH is not a run that found nothing. Its jobs are all
    # `cancelled`, which is `inert`, so the positive assertion below would refuse
    # it by name -- right about the record and wrong about the tree. Cancelling a
    # superseded run is ordinary, and this edge exists only because the `push`
    # policy reads those records at all, so it is closed with the change that
    # opened it rather than found later as a notifier going red on master whenever
    # somebody cancels something.
    printf 'Code coverage\tcompleted\tcancelled\thttps://example.invalid/job\n' > "${scratch}/r-cancelled.tsv"
    printf 'clang-tidy\tcompleted\tcancelled\thttps://example.invalid/job\n' >> "${scratch}/r-cancelled.tsv"
    Case "a CANCELLED run reports nothing rather than being refused" \
        want-none push "${scratch}/r-cancelled.tsv" "nobody let finish" master cancelled

    # And the control: the same record WITHOUT the conclusion is refused, which is
    # what makes the arm above a statement about the run rather than about the
    # record. Without this, an arm that ignored its argument would pass.
    Case "the same record with no run conclusion is still refused" \
        want-refuse push "${scratch}/r-cancelled.tsv" "run that did nothing" master

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
            # #774's two wiring facts. The branch is what lets a push run be told
            # from a scratch one, and the only-if-new mode is what stops a context
            # failing on every push from commenting on one report forever. Staged
            # here rather than asserted only against the real file, so the fixture
            # models a CORRECT workflow rather than the one that happens to exist.
            [[ "$steps" != "no-decider" ]]  && {
                echo "      - env:"
                echo "          HEAD_BRANCH: \${{ github.event.workflow_run.head_branch }}"
                echo "        run: scripts/ci-merge-group-report.sh x y z \"\$HEAD_BRANCH\""
            }
            # The reporter step, and its `env:` is the case rather than scenery.
            # This fixture shipped WITHOUT it, modelling the exact defect as the
            # CORRECT workflow: `$EVENT` read where nothing defines it, which is
            # #1174 -- so the baseline vouched for the bug and the `no-event-env`
            # variant below is what a positive control for the new rule looks
            # like. A fixture more permissive than the thing it stands for.
            [[ "$steps" != "no-reporter" ]] && {
                echo "      - name: \"Open or update one report per unreported failure\""
                [[ "$steps" != "no-event-env" ]] && {
                    echo "        env:"
                    echo "          EVENT: \${{ github.event.workflow_run.event }}"
                }
                echo "        run: |"
                echo "          [[ \"\$EVENT\" == push ]] && export FASTCACHED_REPORT_ONLY_IF_NEW=1"
                echo "          scripts/ci-report-issue.sh t b"
            }
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

    # #1174's positive control, and the only case here whose subject is a step
    # rather than the file: the reporter step reads `$EVENT` while its own `env:`
    # defines nothing. Every whole-file rule above passes on this workflow --
    # which is precisely how the shipped one passed them for its entire life --
    # so a green run of the other twelve cases says nothing about this one.
    GenerateWorkflow "${scratch}/wf.yml" workflow_run cancelled plain issues no-event-env yes
    ShapeCase "a step reading a name its own env: does not define is refused" want-fail "${scratch}/wf.yml"

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
