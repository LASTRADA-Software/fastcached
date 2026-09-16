#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# `scripts/lib/workflow-walk.awk`, driven directly (#1456).
#
# ## Why a self-test of its own, and not the consumers'
#
# The library is to serve five checks, and the first of them --
# `check-workflow-step-env.sh` -- uses three of its eight event kinds. The other
# five are reached by nothing in this tree yet, and gawk's rule about calling an
# undefined function is **per reached path, not per program**: a hook a consumer
# never reaches exits 0 in silence, and is then a fatal error on somebody's
# unrelated pull request. The same asymmetry runs the other way for the library
# itself -- a kind nothing drives is a kind nothing has watched work, and it
# reads as covered because the consumer's own self-test is green.
#
# So every kind is driven HERE, and the run says how many of each. WfPath, the
# job record and the key event have no consumer until the second migration; they
# are tested from the day they ship or they ship untested.
#
# ## What the cases are for
#
# The ones the ticket asks for are where the five private readers each failed
# differently: block scalars with chomping indicators, a heredoc whose body
# contains a step, a flow mapping, an alias, a quoted key and a second document.
# To those this adds the records nothing else reads yet, and the two defects that
# reading a transcript of a rich case found -- a job flushed only after the NEXT
# job's key event, and a pass whose last job was never flushed at all.
#
# ## Proving it can fail
#
# Every assertion family has a NEUTER: the library is copied with one line
# disabled and the same assertions are required to go red. A neuter that provokes
# nothing is reported as its own failure, because that means either the line is
# not load-bearing or no assertion reaches it. A green self-test over an
# unneutered library says only that today's fixtures and today's code agree.
set -uo pipefail

FastCachedScriptDir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FastCachedWalkAwk="${FastCachedScriptDir}/lib/workflow-walk.awk"
FastCachedLexAwk="${FastCachedScriptDir}/lib/shell-lex.awk"

for awkProgram in "${FastCachedWalkAwk}" "${FastCachedLexAwk}"; do
    if [ ! -f "${awkProgram}" ]; then
        echo "check-workflow-walk-selftest: missing awk program ${awkProgram}; nothing was driven,"
        echo "  so this is a refusal and not a clean run."
        exit 2
    fi
done

FastCachedWork="$(mktemp -d)"
trap 'rm -rf "${FastCachedWork}"' EXIT

Failures=0
Cases=0
Assertions=0
Neuters=0
Quiet=0

Fail() {
    if [ "${Quiet}" -eq 0 ]; then
        echo "  FAIL: $*"
    fi
    Failures=$((Failures + 1))
}

Counted() {
    if [ "${Quiet}" -eq 0 ]; then
        Assertions=$((Assertions + 1))
    fi
}

# ---- the driver -------------------------------------------------------------
# One TSV line per event, with the per-line kinds tallied per PASS rather than
# printed: a transcript carrying one line per input line would be unreadable, and
# the tally is what says a kind fired at all.
cat > "${FastCachedWork}/drive.awk" <<'DRIVER'
function WorkflowOn(kind) {
    tally[WfPass, kind]++
    if (WfPass != 2) return
    if (kind == "text")
        blockKeys[WfBlockKey]++
    else if (kind == "key")
        printf "key\t%s\tscope=%s\tind=%d\tvalue=<%s>\n", WfPath, WfScope, WfIndent, WfValue
    else if (kind == "step")
        printf "step\tjob=%s\tn=%d\tname=%s\tshell=%s\tuses=%s\trun=%d\tenv=%d\tenvnames=%s\n",
               WfJob, WfStepIndex, WfStepName, WfStepShell, WfStepUses, WfBodyCount,
               WfStepEnvRows, Names(WfStepEnv)
    else if (kind == "job")
        printf "job\t%s\tat=%d\trunsOn=<%s>\tshell=<%s>\tenvnames=%s\n",
               WfJob, WfJobStart, WfRunsOn, WfJobShell, Names(WfJobEnv)
    else if (kind == "item")
        printf "item\t%s\t%s\n", WfPath, WfItem
    else if (kind == "refusal")
        printf "refusal\t%d\t%s\t%s\n", WfAt, WfRefuseKind, WfRefuseDetail
    else if (kind == "end") {
        printf "workflow\tshell=<%s>\tenvnames=%s\n", WfWorkflowShell, Names(WfWorkflowEnv)
        printf "blockkeys\t%s\n", Counts(blockKeys)
        Tally(1); Tally(2)
        printf "count\trunKeys=%d\tplaced=%d\n", WfRunKeys, Cardinality(WfPlaced)
    }
}
# Sorted, because awk leaves `for (k in a)` order unspecified and a transcript
# that is only usually in one order is a flake nobody would blame on the fixture.
function Names(a,   k, n, i, j, t, out, keys) {
    n = 0
    for (k in a) keys[++n] = k
    for (i = 2; i <= n; i++) {
        t = keys[i]
        for (j = i - 1; j >= 1 && keys[j] > t; j--) keys[j + 1] = keys[j]
        keys[j + 1] = t
    }
    for (i = 1; i <= n; i++) out = out (i > 1 ? "," : "") keys[i]
    return n ? out : "-"
}
function Cardinality(a,   k, n) { for (k in a) n++; return n + 0 }
# Owner=count pairs, sorted. A SET of owners cannot see a continuation attributed to the
# WRONG key when that key owns other continuations too; a count can.
function Counts(a,   k, n, i, j, t, out, keys) {
    n = 0
    for (k in a) keys[++n] = k
    for (i = 2; i <= n; i++) {
        t = keys[i]
        for (j = i - 1; j >= 1 && keys[j] > t; j--) keys[j + 1] = keys[j]
        keys[j + 1] = t
    }
    for (i = 1; i <= n; i++) out = out (i > 1 ? "," : "") keys[i] "=" a[keys[i]]
    return n ? out : "-"
}
function Tally(p) {
    printf "tally\tpass=%d\traw=%d\ttext=%d\tstep-line=%d\tkey=%d\tstep=%d\tjob=%d\trefusal=%d\titem=%d\n",
           p, tally[p, "raw"], tally[p, "text"], tally[p, "step-line"], tally[p, "key"],
           tally[p, "step"], tally[p, "job"], tally[p, "refusal"], tally[p, "item"]
}
DRIVER

# Drive the library @p 2 over the fixture @p 1, the file passed TWICE as every
# consumer passes it.
Drive() {
    awk -f "${FastCachedLexAwk}" -f "$2" -f "${FastCachedWork}/drive.awk" "$1" "$1" 2>&1
}

# ---- assertions -------------------------------------------------------------
# Each takes the transcript FILE rather than a pipe: `producer | grep -q` is a
# false negative under `pipefail` on its SUCCESS path, and reading a file is not
# a pipe at all.

# @param 1 transcript file  @param 2 fixed pattern  @param 3 what it proves
Require() {
    Counted
    if grep -Fq -- "$2" "$1"; then return 0; fi
    Fail "$3 -- no line matching <$2>"
    return 1
}

# @param 1 transcript file  @param 2 fixed pattern  @param 3 what it proves
Refute() {
    Counted
    if grep -Fq -- "$2" "$1"; then
        Fail "$3 -- a line matching <$2> is present and must not be"
        return 1
    fi
    return 0
}

# @param 1 transcript file  @param 2 how many  @param 3 fixed pattern  @param 4 what it proves
RequireCount() {
    local seen
    Counted
    seen="$(grep -Fc -- "$3" "$1" || true)"
    if [ "${seen:-0}" = "$2" ]; then return 0; fi
    Fail "$4 -- expected $2 line(s) matching <$3>, saw ${seen:-0}"
    return 1
}

# The FIRST line matching @p 2 must come before the first matching @p 3. Both are
# asserted to match at all, because two absent patterns compare equal and an
# ordering assertion over nothing passes.
# @param 1 transcript file  @param 2 earlier  @param 3 later  @param 4 what it proves
RequireOrder() {
    local first later
    Counted
    # `grep -n` prints `<lineno>:<text>`, so cutting the longest suffix from the
    # first colon leaves the FIRST match's line number however many matched --
    # never `| head -1`, which is a false NEGATIVE under `pipefail` on its SUCCESS
    # path and is what `check-e2e-helpers.sh` refuses. This function was caught by
    # that scan on its first run, which is the scan doing exactly its job.
    first="$(grep -nF -- "$2" "$1" || true)"; first="${first%%:*}"
    later="$(grep -nF -- "$3" "$1" || true)"; later="${later%%:*}"
    if [ -z "${first}" ] || [ -z "${later}" ]; then
        Fail "$4 -- one of the two patterns matched nothing (<$2>: ${first:-none}, <$3>: ${later:-none})"
        return 1
    fi
    if [ "${first}" -lt "${later}" ]; then return 0; fi
    Fail "$4 -- <$2> is at line ${first} and <$3> at line ${later}"
    return 1
}

# ---- the fixtures -----------------------------------------------------------
Stage() {
    Cases=$((Cases + 1))
    cat > "${FastCachedWork}/$1.yml"
}

Stage rich <<'YML'
name: demo
on:
  pull_request:
    branches: [master]
  merge_group:
env:
  TOP: 1
defaults:
  run:
    shell: bash
jobs:
  first:
    if: ${{ github.event_name == 'push' }}
    name: The First Job
    needs:
      - earlier
      - "also-earlier"
    runs-on:
      - ubuntu-24.04
    defaults:
      run:
        shell: sh
    env:
      JOBWIDE: 2
    steps:
      - name: folded chomped
        run: >-
          echo one
          echo two
      - name: literal kept
        run: |+
          echo three
      - uses: actions/checkout@v4
        with:
          fetch-depth: 0
      - name: heredoc that writes a step
        env:
          MINE: 3
          ALSO: 4
        run: |
          cat > f.yml <<'EOF'
            - run: echo not a step
          EOF
  second:
    runs-on: windows-2022
    steps:
      - name: pwsh
        shell: pwsh
        run: Write-Host 'hi'
YML

Stage edges <<'YML'
name: edges
on: push
jobs:
  one:
    runs-on: ubuntu-24.04
    steps:
      - { name: flow, run: echo flow }
      - name: alias
        run: *someAnchor
      - "quoted key step":
          nothing: here
      - name: quoted run
        run: 'echo it''s fine'
      - name: broken quote
        run: "echo unterminated
  two:
    runs-on: ubuntu-24.04
    steps:
      - run: echo second job
---
name: a second document
YML

Stage folded <<'YML'
name: folded
on: push
jobs:
  only:
    runs-on: ubuntu-24.04
    if: github.event_name == 'push'
        && github.ref_type != 'branch'
    steps:
      - name: a folded condition and a folded env value
        id: sweep
        if: >-
          github.ref == 'refs/heads/master'
          && needs.changes.outputs.code != 'false'
        env:
          NOTE: >-
            one
            two
        run: |
          echo body
      - name: a plain scalar that continues
        working-directory: some/path
          and/more
        run: echo two
      - name: a run whose plain scalar continues
        run: echo three
          echo four
      - name: a run whose plain scalar starts on the NEXT line
        run:
          echo five
YML

# ---- the assertions, per case ----------------------------------------------
# Kept in ONE place so a neuter re-runs exactly what the clean arm ran. A second
# copy for the neuters would drift, and the neuter would then be holding up a
# stale list while reporting on the current library.
Judge() {
    local name="$1" lib="$2" t
    t="${FastCachedWork}/$1-$3.out"
    Drive "${FastCachedWork}/${name}.yml" "${lib}" > "${t}"

    if [ "${name}" = "rich" ]; then
        # The key PATH. Nothing consumes it in this tree yet; it is what will let
        # the four absolute-indentation readers state a path instead of a column.
        Require "${t}" 'key	on/pull_request/branches	scope=	ind=4' "a nested non-job key carries its ancestry"
        Require "${t}" 'key	defaults/run/shell	scope=	ind=4' "a workflow default's path is three deep"
        Require "${t}" 'key	jobs/first/if	scope=job' "a job's own if: is reachable as a key event"
        Require "${t}" 'key	jobs/first/name	scope=job' "a job's own name: is reachable as a key event"
        Require "${t}" 'key	jobs/first/steps/with/fetch-depth	scope=step	ind=10' "a with: row is reachable, at step scope"

        # A bare scalar sequence entry, which the walk PLACED and reported nothing
        # about until `check-gated-jobs.sh` needed `release.needs`. Both of its two
        # sites are driven -- `needs:`, and `runs-on:`, whose entries additionally
        # accumulate into `WfRunsOn` -- because they are different code paths, and
        # a quoted entry is asserted unquoted.
        Require "${t}" 'item	jobs/first/needs	earlier' "a needs: entry carries its owning key's path"
        Require "${t}" 'item	jobs/first/needs	also-earlier' "a quoted sequence entry arrives unquoted"
        Require "${t}" 'item	jobs/first/runs-on	ubuntu-24.04' "a runs-on: entry fires the same kind as any other"

        # The step record. `>-`, `|+` and `|` are three chomping spellings and
        # each owns a different number of body lines.
        Require "${t}" 'step	job=first	n=1	name=folded chomped	shell=	uses=	run=2	env=0' "a folded chomped scalar owns both its lines"
        Require "${t}" 'step	job=first	n=2	name=literal kept	shell=	uses=	run=1	env=0' "a literal kept scalar owns its one line"
        Require "${t}" 'step	job=first	n=3	name=(unnamed)	shell=	uses=actions/checkout	run=0' "a uses: step has no run body and its tag is stripped"
        Require "${t}" 'run=3	env=2	envnames=ALSO,MINE' "the heredoc body is three lines and the step env: block has two rows"

        # A heredoc that WRITES a step is text. Placing it would be a fifth step.
        RequireCount "${t}" 4 'job=first	n=' "the job has four steps and the heredoc body holds no fifth"
        Refute "${t}" 'name=not a step' "a heredoc body never becomes a step"

        # The step index restarts per job, and the second job's shell is its own.
        Require "${t}" 'step	job=second	n=1	name=pwsh	shell=pwsh' "the step index restarts in the next job"

        # The job record: its runs-on list joined, its own defaults shell, its env.
        Require "${t}" 'job	first	at=12	runsOn=< ubuntu-24.04>	shell=<sh>	envnames=JOBWIDE' "the job record carries its runs-on list, its own defaults shell and its env"
        Require "${t}" 'job	second	at=44	runsOn=<windows-2022>	shell=<>	envnames=-' "a job with no defaults and no env of its own says so rather than inheriting"

        # And the ORDER: a job is closed BEFORE the next job's key event fires.
        # Asserted as an order rather than as presence, because both orders
        # produce the same two lines.
        RequireOrder "${t}" 'job	first	at=12' 'key	jobs/second	scope=job' "a job is closed before the next job's key event"

        # The workflow scope, which the second pass reads from the first.
        Require "${t}" 'workflow	shell=<bash>	envnames=TOP' "the workflow-level defaults shell and env are kept"

        # Every kind, in BOTH passes, and the job kind the same number of times in
        # each -- a pass whose last job is never flushed is what this refuses.
        Require "${t}" 'tally	pass=1	raw=49	text=6	step-line=15	key=40	step=5	job=2	refusal=0	item=3' "pass 1 drove every structural kind"
        Require "${t}" 'tally	pass=2	raw=49	text=6	step-line=15	key=40	step=5	job=2	refusal=0	item=3' "pass 2 drove the same kinds the same number of times as pass 1"

        # The completeness cross-check. Six run: keys -- four steps, and the
        # workflow and job defaults; the one in the heredoc is inside a scalar.
        Require "${t}" 'count	runKeys=6	placed=6' "every run: key the count found was placed"
    fi

    if [ "${name}" = "folded" ]; then
        # `WfBlockKey` is the only way to tell a folded `if:`'s second line from
        # any other scalar's, and nothing in the tree consumed it when it shipped.
        # Three owners here rather than one, so an implementation that hardcoded
        # "run" cannot pass: `if`, a step `env:` VALUE, and the `run:` itself.
        # Counted per owner, not merely listed. A SET cannot see a continuation
        # attributed to the WRONG key when that key owns other continuations too,
        # and that is the bug this field prevents: a consumer joining a folded
        # `if:` must not swallow a `run:` body's lines. Neutering the `run:` site
        # moves `run` from 3 to 2 and adds `name=1`, which a set would have hidden.
        #
        # Four owners over the four shapes that open a scalar: a block indicator
        # (the step `if:` and the `env:` VALUE), a plain scalar with an inline
        # value continuing below (`working-directory:` and the JOB-level `if:`),
        # a `run:` whose value starts on its key line, and a `run:` whose value
        # starts on the next one -- the last being the only path where the `run:`
        # site's assignment is not already made by the plain-scalar branch above
        # it, which is why a neuter of it provoked nothing until this shape was
        # in the fixture.
        Require "${t}" 'blockkeys	NOTE=2,if=3,run=3,working-directory=1' "a continuation names the key whose scalar it belongs to"

        # The step record, and its own keys as key events -- what the next
        # migration reads instead of `/^        id:/` and `/^        if:/`.
        Require "${t}" 'key	jobs/only/steps/id	scope=step	ind=8	value=<sweep>' "a step id: is a key at step scope"
        Require "${t}" 'key	jobs/only/steps/if	scope=step	ind=8	value=<>->' "a folded step if: is a key whose value is the indicator"
        Require "${t}" 'key	jobs/only/steps/env/NOTE	scope=step	ind=10	value=<>->' "a folded env value is a key inside the step env block"
        Require "${t}" 'step	job=only	n=1	name=a folded condition and a folded env value	shell=	uses=	run=1	env=1	envnames=NOTE' "the step record has one run line and one env row"
        Require "${t}" 'tally	pass=2	raw=29	text=9	step-line=13	key=20	step=4	job=1	refusal=0	item=0' "the fixture drives nine continuations over four steps"
        # The JOB-level folded condition, whose continuation is outside any step --
        # a  event nothing would report while that kind was step-only, and the
        # shape two of the readers still to migrate join on purpose.
        Require "${t}" 'key	jobs/only/if	scope=job	ind=4	value=<github.event_name == '"'"'push'"'"'>' "a job-level condition is a key at job scope"
    fi

    if [ "${name}" = "edges" ]; then
        Require "${t}" 'refusal	7	unreadable-yaml	a step written as `{ name: flow, run: echo flow }`' "a flow mapping step is refused, not misread"
        Require "${t}" 'refusal	9	unreadable-run	`run: *someAnchor`' "an alias as a run: value is refused"
        Require "${t}" 'refusal	10	unreadable-yaml	a step written as `"quoted key step":`' "a quoted step key is refused"
        Require "${t}" 'refusal	15	unreadable-run	`run: "echo unterminated`' "a quoted scalar left open is refused"
        Require "${t}" 'refusal	20	unreadable-yaml	a document marker after the first document' "a second document is refused"

        # A key under a REFUSED line must not inherit the ancestry of whatever
        # key last sat at that indent -- which here belonged to another step.
        Require "${t}" 'key	jobs/one/steps/?/nothing' "an unreadable ancestor is marked in the path"
        Refute "${t}" 'key	jobs/one/steps/run/nothing' "the path must not claim a readable ancestor it does not have"

        # A doubled apostrophe in a YAML single-quoted scalar is ONE apostrophe,
        # and the step is read rather than refused.
        Require "${t}" 'step	job=one	n=4	name=quoted run	shell=	uses=	run=1' "a single-quoted run: with a doubled apostrophe is read"
        Require "${t}" 'job	one	at=4' "the first job is closed although its last step was refused"
        Require "${t}" 'job	two	at=16' "the last job of the file is closed in END"
        Require "${t}" 'tally	pass=1	raw=21	text=0	step-line=10	key=18	step=6	job=2	refusal=5	item=0' "pass 1 drove five refusals and six steps"
        Require "${t}" 'tally	pass=2	raw=21	text=0	step-line=10	key=18	step=6	job=2	refusal=5	item=0' "pass 2 drove the same"
    fi
}

echo "check-workflow-walk-selftest: driving scripts/lib/workflow-walk.awk"
Judge rich "${FastCachedWalkAwk}" clean
Judge folded "${FastCachedWalkAwk}" clean
Judge edges "${FastCachedWalkAwk}" clean
CleanFailures=${Failures}

# ---- the neuters ------------------------------------------------------------
# One line disabled per neuter, and the case that covers it is required to go RED
# with the SAME assertions the clean arm ran.
#
# @param 1 a name  @param 2 a sed expression that disables one line
# @param 3 the case to re-drive  @param 4 what that line holds up
Neuter() {
    local name="$1" expr="$2" case="$3" why="$4" lib before
    Neuters=$((Neuters + 1))
    lib="${FastCachedWork}/neuter-${name}.awk"
    sed "${expr}" "${FastCachedWalkAwk}" > "${lib}"
    if cmp -s "${lib}" "${FastCachedWalkAwk}"; then
        Fail "neuter ${name}: the sed expression changed nothing, so no line was neutered"
        return 1
    fi
    before=${Failures}
    Quiet=1
    Judge "${case}" "${lib}" "neuter-${name}"
    Quiet=0
    if [ "${Failures}" -eq "${before}" ]; then
        Fail "neuter ${name}: nothing failed, so no assertion holds up ${why}"
        return 1
    fi
    # The failures a neuter provokes are the point, so they are not the tree's.
    echo "  ok: neuter ${name} provoked $((Failures - before)) failure(s) -- ${why}"
    Failures=${before}
    return 0
}

Neuter pass-flush 's/^    WorkflowFlushStep(); WorkflowFlushJob()$/    #&/' rich \
    "a pass boundary closing the step and job it is ending"
Neuter job-hoist '/WorkflowFlushStep(); WorkflowFlushJob(); WorkflowResetJob(); WfJob = key/s/^/#/' rich \
    "a job being closed before the next job's key event"
Neuter path 's/^    WorkflowPushPath(ind, key)$/    #&/' rich \
    "every mapping line pushing its key onto the path"
Neuter step-index 's/^    WfStepIndex++$/    #&/' rich \
    "a step knowing its position in its job"
Neuter job-env '/WfJobEnv\[name\] = 1; WfJobEnvAt\[WfJobStart, name\] = 1/s/^/#/' rich \
    "a job-level env: name reaching the job record"
# One per site where a scalar opens. Three of them, because a neuter of a site the
# fixture never reaches provokes nothing -- which this self-test reports as ITS OWN
# failure, and did, which is how the third shape came to be in the fixture at all.
Neuter block-key-indicator 's/^        WfBlockOwner = ind; WfBlockKey = key$/        WfBlockOwner = ind/' folded \
    "a block-indicator continuation knowing which key's scalar it is part of"
Neuter block-key-plain 's/^        WfBlockOwner = ind; WfBlockKind = "skip"; WfBlockKey = key$/        WfBlockOwner = ind; WfBlockKind = "skip"/' folded \
    "a plain-scalar continuation knowing which key's scalar it is part of"
Neuter block-key-run 's/^            WfBlockOwner = ind; WfBlockKind = "run"; WfBlockKey = key$/            WfBlockOwner = ind; WfBlockKind = "run"/' folded \
    "a run: body line knowing it belongs to a run:"
Neuter item-scalar '/WfItem = WorkflowUnquote(WorkflowTrim(line))/s/^/#/' rich \
    "a bare sequence entry carrying its text"
Neuter item-runs-on '/WfItem = WorkflowUnquote(WorkflowTrim(t))/s/^/#/' rich \
    "a runs-on: entry carrying its text, which is a second site"
Neuter path-mark '/if (kind == "unreadable-yaml" || kind == "unreadable-run") WorkflowPushPath/s/^/#/' edges \
    "a refused line marking the path so nothing below it claims a readable ancestor"
Neuter second-document '/a document marker after the first document/s/^/#/' edges \
    "a second document being refused"
Neuter yaml-quoted 's/^        if (ch == q) {$/        if (0) {/' edges \
    "a YAML-quoted run: scalar being read rather than refused"

echo "check-workflow-walk-selftest: ${Cases} fixture(s), ${Assertions} assertion(s), ${Neuters} neuter(s)"
if [ "${Failures}" -ne 0 ]; then
    echo "check-workflow-walk-selftest: ${Failures} failure(s) (${CleanFailures} on the clean library)"
    exit 1
fi
echo "check-workflow-walk-selftest: every verdict as it must be"
exit 0
