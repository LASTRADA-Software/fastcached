#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Run one hygiene check and say which of FOUR things happened to it.
#
# ## Why this exists
#
# A check that was KILLED and a check that FOUND A PROBLEM produce the same ctest
# verdict — `***Failed` — and the only tell is the absence of output nobody is
# looking for. #1075 was that: `check-merge-queue-contexts.sh` died of SIGPIPE on
# its SUCCESS path, printed 12 of its 14 `ok:` lines and stopped, with no `FAIL:`,
# no summary and no stderr. Two people diagnosed it, one as host load and one as a
# defect in the tree under test, and neither was true (#1079).
#
# That is the four-states rule from `.agent/rules/metrics-and-observability.md`
# landing in the worst available place: the collapsing check's own subject is
# required contexts failing to report.
#
# ## Why a wrapper, rather than a terminal line in each check
#
# `local-gate.sh` already solved this — a start marker naming its pid, tree and
# commit, and `--classify` reading the rule back, so a log with a start marker and
# no terminal line DID NOT CONCLUDE. **This is that solution propagated, not a new
# mechanism**, and the propagation is deliberately not "every check prints its own
# marker":
#
#   * A marker each of 22 scripts spells itself is 22 chances to spell it
#     differently. They have no shared convention today — a survey looking for one
#     got `check-e2e-helpers.sh` wrong, which is the script that already does this
#     best.
#   * A marker printed BY the check is gone exactly when it is needed. This wrapper
#     is the check's PARENT, so a check killed by a signal cannot suppress it.
#   * And when the wrapper is killed too — a ctest timeout, an OOM taking the whole
#     group — there is no terminal marker at all, which is the correct answer
#     rather than a gap. The ABSENCE is load-bearing, which is why the marker is
#     printed on every path out.
#
# ## The classification, and why it is not the exit status alone
#
# The tempting rule is the rulebook's own "anything neither 0 nor 1 is the
# instrument failing". Measured across the 22 registered checks, that is unsound
# HERE: `2` and `3` are already in use for `mktemp` failing, usage errors and a
# missing nested tool. No status could be reserved for *did not conclude* without
# auditing and changing every one of them, and any status picked would collide.
#
# So the verdict travels in the MARKER, and the status is preserved untouched:
#
#     exit 0            -> passed
#     exit 77           -> skipped        (ctest's SKIP_RETURN_CODE convention)
#     exit 128 + signal -> did-not-conclude
#     any other nonzero -> failed         (the check ran and reported)
#
# **The status is passed through exactly, never rewritten.** A wrapper that
# invents a number is a verdict about the wrapper, which is the failure this whole
# family is made of. Preserving it also means `141` still reads as "neither 0 nor
# 1" to anything that only sees a number.
#
# The one case this reads generously: a check whose own child was killed and which
# propagates that status with `exit $?` is reported as did-not-conclude even though
# the check itself ran to its end. That is #1075 exactly, where it is the right
# answer, and a check that deliberately exits 128+N to mean something else would be
# misread — no such check exists here, and the marker prints the raw status so a
# reader can see what it was.
#
# ## bash 3.2
#
# macOS ships a 2007 `/bin/bash` and this runs on every platform CI builds. No
# `mapfile`, no `declare -A`, no `local -n`, no `BASHPID`.
#
# Usage:  run-check.sh <check-script> [args...]
#         run-check.sh --command -- <argv...>    # any command, same four outcomes
#         run-check.sh --self-test

# NOT `-e`: the whole job is to observe the child's status and classify it, and
# `-e` would end this script before it could say anything.
set -uo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# The interpreter this script is running under, so the check runs under the same
# one ctest chose rather than whatever `bash` resolves to. `bash <path>`, never a
# bare path (#723): a call that fails to START fails for reasons a `chmod` does not
# cover, and every such failure is indistinguishable from the rule firing.
interpreter="${BASH:-bash}"

# The markers. Spelled ONCE, because a reader greps for them and a second spelling
# is a second thing to keep in step.
StartMarker="== CHECK STARTED"
TerminalMarker="== CHECK CONCLUDED"

# Print the start marker for a check about to run.
#
# It names the commit for the reason `local-gate.sh`'s does: a previous run's
# output sitting where this one goes is told apart by nothing else. The commit is
# asked ONCE, below, with the question of whether this tree is readable at all.
# @param 1 The check's path, as invoked.
StartLine() {
    echo "${StartMarker} -- ${1} , pid $$, tree ${repo_root}, commit ${treeCommitLabel}"
}

# ## A git tree the git on PATH cannot read is refused before any check starts (#1355)
#
# A worktree created by the other git on this machine holds an ABSOLUTE `gitdir:`
# pointer in that git's spelling, and WSL git cannot follow `D:/...`. Every git
# command there answers nothing, and the checks do not fail alike -- measured under
# WSL over DrvFs, in such a worktree, by running each entry alone:
#
#   * `workflow-script-invocations` exited 128 with no message at all;
#   * `reloadable-docs` reported that no tracked file carries its marker;
#   * `unguarded-prerequisites-selftest` said its positive control's commit is
#     unreachable and that "a shallow clone must fetch depth 0" -- a remedy for a
#     state this tree is not in;
#   * `unguarded-prerequisites` enumerated its scripts by walking the directory
#     instead, which is a verdict about the files on disk rather than the files CI
#     checks out.
#
# Four readings, none of them the cause. The gate already refuses this state by name
# through `repair-worktree-pointers.sh`; a `ctest` run by hand had nothing between it
# and those readings. So the wrapper every script check runs through asks first, and
# a check that cannot ask git what this tree tracks does not start.
#
# REFUSED rather than skipped. A skip is green, and `ctest -L hygiene` reporting
# green over checks that never looked is the collapse this file exists to remove;
# the remedy is one command, which the diagnosis prints.
#
# What it does NOT cover, stated so nobody widens it: a tree with no `.git` entry (a
# source export) and a host with no git both still run, by the walk their checks
# already carry -- neither has a git answer to be wrong about. And a repository git
# can enumerate but whose HEAD names no commit yet still runs: the decision is the
# pointer diagnosis's, which counts tracked files rather than asking for HEAD.
#
# PURE, so every row is a self-test case: told what was found rather than looking.
# @param 1 "yes" | "no" -- whether a `git` answers on PATH
# @param 2 "present" | "absent" -- whether the tree root carries a `.git` entry
# @param 3 "yes" | "no" -- whether git named this tree's HEAD
# @param 4 the pointer diagnosis's exit status, or "none" when it was not asked
# @return echoes `run` or `refuse`; an input outside those words is refused
TreeVerdict() {
    local gitOnPath="$1" dotGit="$2" head="$3" diagnosis="$4"
    case "$gitOnPath" in yes|no) ;; *) echo "refuse"; return 0 ;; esac
    case "$dotGit" in present|absent) ;; *) echo "refuse"; return 0 ;; esac
    case "$head" in yes|no) ;; *) echo "refuse"; return 0 ;; esac
    if [ "$head" = "yes" ] || [ "$gitOnPath" = "no" ] || [ "$dotGit" = "absent" ]; then
        echo "run"
    elif [ "$diagnosis" = "0" ]; then
        echo "run"
    else
        echo "refuse"
    fi
}

# Refuse a check this tree cannot support, on STDOUT like every other path out, and
# with both markers -- a refusal is a check that concluded without starting.
# @param 1 The check's label, as the start marker prints it.
RefuseUnreadableTree() {
    StartLine "$1"
    echo "run-check: NOT STARTED -- ${repo_root} has a .git entry and the git on PATH cannot read it,"
    echo "run-check:   so this check cannot ask what the tree tracks. It would walk the directory or read"
    echo "run-check:   an empty set instead, and report on files CI never sees, in either direction."
    echo "run-check:   This is not a finding about the tree, and nothing was checked. git said:"
    ( cd "$repo_root" && git rev-parse --short HEAD 2>&1 ) | sed 's/^/run-check:     /'
    echo "run-check:   and the pointer diagnosis, which names the remedy:"
    printf '%s\n' "$treeDiagnosis" | sed 's/^/run-check:     /'
    echo "${TerminalMarker}: failed -- ${1} did not start, this work tree is not readable by git (exit 2)"
    exit 2
}

# Asked once per run. `git --version` rather than `command -v git`: the prerequisite
# scan reads a `command -v` as a guard, which would oblige this file to guard every
# other tool it names.
treeGitOnPath="no"
git --version >/dev/null 2>&1 && treeGitOnPath="yes"
treeDotGit="absent"
[ -e "${repo_root}/.git" ] && treeDotGit="present"
treeCommit=""
if [ "$treeGitOnPath" = "yes" ]; then
    treeCommit="$(cd "$repo_root" && git rev-parse --short HEAD 2>/dev/null)" || treeCommit=""
fi
treeHead="no"
[ -n "$treeCommit" ] && treeHead="yes"
# The diagnosis is a spawn, so it is asked only on the path that needs it: never on a
# tree git already reads, which is every CI run and every ordinary one.
treeDiagnosis=""
treeDiagnosisStatus="none"
if [ "$treeHead" = "no" ] && [ "$treeGitOnPath" = "yes" ] && [ "$treeDotGit" = "present" ]; then
    treeDiagnosis="$("$interpreter" "${repo_root}/scripts/repair-worktree-pointers.sh" "$repo_root" 2>&1)"
    treeDiagnosisStatus=$?
fi
treeVerdict="$(TreeVerdict "$treeGitOnPath" "$treeDotGit" "$treeHead" "$treeDiagnosisStatus")"
if [ -n "$treeCommit" ]; then
    treeCommitLabel="$treeCommit"
elif [ "$treeVerdict" = "refuse" ]; then
    treeCommitLabel="<a git tree this git cannot read>"
elif [ "$treeGitOnPath" = "no" ]; then
    treeCommitLabel="<no git on PATH>"
elif [ "$treeDotGit" = "absent" ]; then
    treeCommitLabel="<not a git tree>"
else
    treeCommitLabel="<no commit yet>"
fi

# Classify a child's exit status.
#
# @param 1 The raw wait status.
# @return Prints one of: passed, skipped, failed, did-not-conclude.
Classify() {
    local status="$1"
    if [ "$status" -eq 0 ]; then
        echo "passed"
    elif [ "$status" -eq 77 ]; then
        echo "skipped"
    elif [ "$status" -gt 128 ] && [ "$status" -lt 192 ]; then
        echo "did-not-conclude"
    else
        echo "failed"
    fi
}

# Say what a status means beyond its number, when it means anything more.
# @param 1 The raw wait status.
StatusDetail() {
    local status="$1"
    if [ "$status" -gt 128 ] && [ "$status" -lt 192 ]; then
        echo ", killed by signal $((status - 128))"
    else
        echo ""
    fi
}

if [ "${1:-}" = "--self-test" ]; then
    scratch="$(mktemp -d)" || { echo "run-check: cannot create a scratch directory" >&2; exit 2; }
    # shellcheck disable=SC2064  # expand $scratch now, not at trap time
    trap "rm -rf '$scratch'" EXIT
    selfTestStatus=0
    selfTestCases=0
    me="${repo_root}/scripts/$(basename "${BASH_SOURCE[0]}")"

    # Every case below runs this wrapper from this tree, so in a tree git cannot read
    # each would be REFUSED rather than judged, and read as a wrapper that is broken.
    if [ "$treeVerdict" = "refuse" ]; then
        echo "run-check: self-test NOT RUN -- git cannot read ${repo_root}, so every case would be refused:"
        printf '%s\n' "$treeDiagnosis" | sed 's/^/run-check:     /'
        echo "run-check: self-test ran 0 case(s)"
        exit 2
    fi

    # Stage a check that behaves exactly one way.
    # @param 1 File name under the scratch directory.
    # @param 2 The body, after the shebang.
    Stage() {
        printf '#!/bin/bash\n%s\n' "$2" > "${scratch}/$1"
        chmod +x "${scratch}/$1"
    }

    # Assert the wrapper reports `want` for a staged check, and that its output
    # carries both markers.
    # @param 1 What is being checked, for the report.
    # @param 2 The staged file name.
    # @param 3 The outcome word expected in the terminal marker.
    # @param 4 The exit status expected from the wrapper itself.
    Case() {
        local what="$1" staged="$2" want="$3" wantStatus="$4" out="" got=0
        selfTestCases=$((selfTestCases + 1))
        out="$("$interpreter" "$me" "${scratch}/${staged}" 2>&1)" || got=$?

        local ok=1
        case "$out" in
            *"${StartMarker}"*) ;;
            *) echo "  FAIL  '$what' printed no start marker" >&2; ok=0 ;;
        esac
        case "$out" in
            *"${TerminalMarker}: ${want}"*) ;;
            *) echo "  FAIL  '$what' did not conclude '${want}'" >&2; ok=0 ;;
        esac
        if [ "$got" -ne "$wantStatus" ]; then
            echo "  FAIL  '$what' exited ${got}, expected ${wantStatus} (the status is passed through)" >&2
            ok=0
        fi

        if [ "$ok" -eq 1 ]; then
            echo "  ok    ${what}"
        else
            printf '%s\n' "$out" | sed 's/^/        /' >&2
            selfTestStatus=1
        fi
    }

    Stage "pass.sh" 'echo "did the work"; exit 0'
    Case "a check that passes" pass.sh "passed" 0

    Stage "fail.sh" 'echo "FAIL: something is wrong" >&2; exit 1'
    Case "a check that finds a problem" fail.sh "failed" 1

    Stage "skip.sh" 'echo "prerequisite missing"; exit 77'
    Case "a check that skips" skip.sh "skipped" 77

    # The status this repository's checks already use for "I could not run" -- a
    # `mktemp` failure, a usage error. It CONCLUDED: it said so.
    Stage "instrument.sh" 'echo "cannot create a scratch directory" >&2; exit 2'
    Case "a check that reports it could not run" instrument.sh "failed" 2

    # #1075 itself: SIGPIPE on the success path, silent.
    Stage "sigpipe.sh" 'echo "ok: one"; echo "ok: two"; kill -PIPE $$; sleep 5'
    Case "a check killed by SIGPIPE mid-run" sigpipe.sh "did-not-conclude" 141

    Stage "sigkill.sh" 'echo "ok: one"; kill -KILL $$; sleep 5'
    Case "a check killed outright" sigkill.sh "did-not-conclude" 137

    # #1355: the caller's MSYS path conversion does not reach the check. Asserted on the
    # MECHANISM, which every platform can show -- on Linux and macOS the two variables mean
    # nothing, and that is fine, because what is tested is that the check never sees them.
    # The EFFECT is native Windows only, and was measured there rather than inferred.
    Stage "conversion.sh" 'echo "pathconv=${MSYS_NO_PATHCONV-unset} excl=${MSYS2_ARG_CONV_EXCL-unset}"; exit 0'
    selfTestCases=$((selfTestCases + 1))
    conversionOut="$(MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*' "$interpreter" "$me" "${scratch}/conversion.sh" 2>&1)" || true
    case "$conversionOut" in
        *"pathconv=unset excl=unset"*) echo "  ok    a caller's MSYS path conversion does not reach the check" ;;
        *)
            echo "  FAIL  the check inherited the caller's MSYS path conversion" >&2
            printf '%s\n' "$conversionOut" | sed 's/^/        /' >&2
            selfTestStatus=1
            ;;
    esac

    # #1355: a tree the git on PATH cannot read. The DECISION, row by row, in both
    # directions -- every `run` row is a population that must not be refused.
    # @param 1 What is being checked.
    # @param 2 The verdict wanted.
    # @param 3.. The inputs, in `TreeVerdict`'s order.
    TreeCase() {
        local what="$1" want="$2" got=""
        shift 2
        selfTestCases=$((selfTestCases + 1))
        got="$(TreeVerdict "$@")"
        if [ "$got" = "$want" ]; then
            echo "  ok    ${what}"
        else
            echo "  FAIL  ${what}: expected '${want}', got '${got}'" >&2
            selfTestStatus=1
        fi
    }
    TreeCase "a tree git names HEAD for runs its checks" run yes present yes none
    TreeCase "a source export with no .git entry runs its checks" run yes absent no none
    TreeCase "a host with no git runs its checks" run no present no none
    TreeCase "a repository with no commit yet, which git still enumerates, runs its checks" run yes present no 0
    TreeCase "a .git entry the git on PATH cannot read is REFUSED" refuse yes present no 1
    # One per input, each arranged so that WITHOUT that input's vocabulary guard the
    # other inputs would say `run` -- an unnamed word that reaches `refuse` anyway
    # tests nothing about the guard.
    TreeCase "an unnamed git answer is refused rather than run" refuse maybe absent no none
    TreeCase "an unnamed .git entry kind is refused rather than run" refuse yes maybe no 0
    TreeCase "an unnamed HEAD answer is refused rather than run" refuse yes absent maybe none

    # And the WIRING, which the rows cannot see: this wrapper, copied into a tree
    # whose `.git` points somewhere no git can follow, must not start the check --
    # and the same copy with the entry removed must. The pointer is POSIX-absolute
    # because that is stageable on every host; the Windows-dialect one it stands for
    # reaches the same diagnosis row. GIT_CEILING_DIRECTORIES keeps git from finding
    # a repository ABOVE the scratch directory in the passing half.
    selfTestCases=$((selfTestCases + 1))
    treeCopy="${scratch}/tree"
    mkdir -p "${treeCopy}/scripts"
    cp "$me" "${repo_root}/scripts/repair-worktree-pointers.sh" "${treeCopy}/scripts/"
    echo "gitdir: /nonexistent-1355/.git/worktrees/x" > "${treeCopy}/.git"
    if ! git --version >/dev/null 2>&1; then
        echo "  FAIL  no git on PATH, so the unreadable-tree refusal was never driven -- inconclusive, not a pass" >&2
        selfTestStatus=1
    else
        treeStatus=0
        treeOut="$(GIT_CEILING_DIRECTORIES="$scratch" "$interpreter" "${treeCopy}/scripts/run-check.sh" "${scratch}/pass.sh" 2>&1)" || treeStatus=$?
        case "$treeStatus/$treeOut" in
            *"did the work"*) treeOk=0 ;;
            2/*"did not start, this work tree is not readable by git"*) treeOk=1 ;;
            *) treeOk=0 ;;
        esac
        case "$treeOut" in *"ABSOLUTE pointer"*) ;; *) treeOk=0 ;; esac
        if [ "$treeOk" -eq 1 ]; then
            echo "  ok    a check in a tree git cannot read is refused before it starts, naming the diagnosis"
        else
            echo "  FAIL  a check in a tree git cannot read must not start (exit ${treeStatus})" >&2
            printf '%s\n' "$treeOut" | sed 's/^/        /' >&2
            selfTestStatus=1
        fi
    fi
    # `--command` asks through its own call site, so it gets its own case.
    selfTestCases=$((selfTestCases + 1))
    treeStatus=0
    treeOut="$(GIT_CEILING_DIRECTORIES="$scratch" "$interpreter" "${treeCopy}/scripts/run-check.sh" --command -- "$interpreter" "${scratch}/pass.sh" 2>&1)" || treeStatus=$?
    case "$treeStatus/$treeOut" in
        *"did the work"*) echo "  FAIL  --command started a command in a tree git cannot read" >&2; selfTestStatus=1 ;;
        2/*"did not start, this work tree is not readable by git"*) echo "  ok    --command: a command in a tree git cannot read is refused too" ;;
        *)
            echo "  FAIL  --command in a tree git cannot read must be refused (exit ${treeStatus})" >&2
            printf '%s\n' "$treeOut" | sed 's/^/        /' >&2
            selfTestStatus=1
            ;;
    esac
    selfTestCases=$((selfTestCases + 1))
    rm -f "${treeCopy}/.git"
    treeStatus=0
    treeOut="$(GIT_CEILING_DIRECTORIES="$scratch" "$interpreter" "${treeCopy}/scripts/run-check.sh" "${scratch}/pass.sh" 2>&1)" || treeStatus=$?
    case "$treeStatus/$treeOut" in
        0/*"commit <not a git tree>"*"did the work"*) echo "  ok    the same tree with no .git entry runs the check" ;;
        *)
            echo "  FAIL  a tree with no .git entry must run the check (exit ${treeStatus})" >&2
            printf '%s\n' "$treeOut" | sed 's/^/        /' >&2
            selfTestStatus=1
            ;;
    esac

    # The distinction this whole file exists for, asserted as a DISTINCTION rather
    # than as two separate outcomes: exit 2 and exit 141 are both nonzero and both
    # `***Failed` to ctest, and a wrapper that reported them alike would pass every
    # case above except this one.
    selfTestCases=$((selfTestCases + 1))
    concluded="$("$interpreter" "$me" "${scratch}/instrument.sh" 2>&1)" || true
    killed="$("$interpreter" "$me" "${scratch}/sigpipe.sh" 2>&1)" || true
    # The OUTCOME WORD alone, never the whole marker line. That line also carries
    # the raw status and a `killed by signal N` detail, and those differ between
    # these two runs whether or not the CLASSIFICATION does -- so comparing whole
    # lines passes under exactly the collapse this case exists to catch. Measured:
    # neutering `Classify` reddened the two outcome cases and left this one GREEN
    # until it was narrowed to the word.
    concludedWord="$(printf '%s\n' "$concluded" | sed -n "s/^${TerminalMarker}: \([a-z-]*\).*/\1/p" | tail -1)"
    killedWord="$(printf '%s\n' "$killed" | sed -n "s/^${TerminalMarker}: \([a-z-]*\).*/\1/p" | tail -1)"
    if [ -n "$concludedWord" ] && [ -n "$killedWord" ] && [ "$concludedWord" != "$killedWord" ]; then
        echo "  ok    a failing check and a killed one are reported DIFFERENTLY"
    else
        echo "  FAIL  a failing check and a killed one are reported the same way" >&2
        echo "        failing outcome: ${concludedWord:-<no terminal marker>}" >&2
        echo "        killed outcome:  ${killedWord:-<no terminal marker>}" >&2
        selfTestStatus=1
    fi

    # A check that is not THERE, read the way the absence case reads it: STDOUT
    # alone. Every other case here captures `2>&1`, which cannot see a stream at
    # all -- so a terminal marker written to stderr would satisfy all eight of them
    # while a reader capturing only stdout saw a start marker and no terminal one,
    # which is this file's own spelling of DID NOT CONCLUDE. That is the state
    # collapse this script exists to remove, arriving through the one path out that
    # nothing exercised.
    selfTestCases=$((selfTestCases + 1))
    absent="$("$interpreter" "$me" "${scratch}/no-such-check.sh" 2>/dev/null)" || true
    absentStart=0
    absentTerminal=0
    case "$absent" in *"${StartMarker}"*) absentStart=1 ;; esac
    case "$absent" in *"${TerminalMarker}: failed"*) absentTerminal=1 ;; esac
    if [ "$absentStart" -eq 1 ] && [ "$absentTerminal" -eq 1 ]; then
        echo "  ok    a check that is ABSENT concludes on stdout, like every other path out"
    else
        echo "  FAIL  an absent check must conclude 'failed' on STDOUT" >&2
        echo "        start=${absentStart} terminal=${absentTerminal}" >&2
        printf '%s\n' "$absent" | sed 's/^/        /' >&2
        selfTestStatus=1
    fi

    # And the case no exit status can carry: the WRAPPER dies too, so nothing
    # writes a terminal marker at all. Staged rather than waited for -- the check
    # kills its own parent, which is this wrapper, so the outcome is deterministic
    # and costs no sleep.
    selfTestCases=$((selfTestCases + 1))
    # The staged check releases the captured stdout/stderr before its safety-net
    # sleep: it inherits this command substitution's pipe, so a sleeping orphan
    # would hold it open and the case would cost the full five seconds it exists
    # to avoid waiting.
    Stage "kill-parent.sh" 'echo "ok: one"; kill -KILL "$PPID"; exec >/dev/null 2>&1; sleep 5'
    orphaned="$("$interpreter" "$me" "${scratch}/kill-parent.sh" 2>&1)" || true
    hasStart=0
    hasTerminal=0
    case "$orphaned" in *"${StartMarker}"*) hasStart=1 ;; esac
    case "$orphaned" in *"${TerminalMarker}"*) hasTerminal=1 ;; esac
    if [ "$hasStart" -eq 1 ] && [ "$hasTerminal" -eq 0 ]; then
        echo "  ok    a killed WRAPPER leaves a start marker and no terminal marker"
    else
        echo "  FAIL  a killed wrapper should leave a start marker and no terminal marker" >&2
        echo "        start=${hasStart} terminal=${hasTerminal}" >&2
        printf '%s\n' "$orphaned" | sed 's/^/        /' >&2
        selfTestStatus=1
    fi

    # ---- `--command` mode (#1103) ------------------------------------------
    #
    # Assert `--command` reports `want` and passes `wantStatus` through.
    # @param 1 What is being checked, for the report.
    # @param 2 The outcome word expected in the terminal marker.
    # @param 3 The exit status expected from the wrapper itself.
    # @param 4.. The command to run.
    CommandCase() {
        local what="$1" want="$2" wantStatus="$3" out="" got=0
        shift 3
        selfTestCases=$((selfTestCases + 1))
        out="$("$interpreter" "$me" --command -- "$@" 2>&1)" || got=$?
        local ok=1
        case "$out" in
            *"${StartMarker}"*) ;;
            *) echo "  FAIL  '$what' printed no start marker" >&2; ok=0 ;;
        esac
        case "$out" in
            *"${TerminalMarker}: ${want}"*) ;;
            *) echo "  FAIL  '$what' did not conclude '${want}'" >&2; ok=0 ;;
        esac
        if [ "$got" -ne "$wantStatus" ]; then
            echo "  FAIL  '$what' exited ${got}, expected ${wantStatus}" >&2
            ok=0
        fi
        if [ "$ok" -eq 1 ]; then
            echo "  ok    ${what}"
        else
            printf '%s\n' "$out" | sed 's/^/        /' >&2
            selfTestStatus=1
        fi
    }

    CommandCase "--command: a command that succeeds" passed 0 \
        "$interpreter" -c 'exit 0'
    CommandCase "--command: a command that reports a problem" failed 1 \
        "$interpreter" -c 'exit 1'
    CommandCase "--command: a command that skips" skipped 77 \
        "$interpreter" -c 'exit 77'
    # The whole point of the mode. A killed command must be told from a failing
    # one by something PRESENT, which is the marker rather than the absence of
    # `CMake Error`.
    CommandCase "--command: a command killed by a signal" did-not-conclude 143 \
        "$interpreter" -c 'kill -TERM $$; sleep 5'
    CommandCase "--command: a command that is not there" failed 2 \
        "no-such-command-1103"

    # BYTE IDENTITY, and it is the constraint #1103 turns on: 74 registrations are
    # judged by a regex over the command's OUTPUT, so a wrapper that buffered and
    # re-printed -- or moved a byte between streams -- would change every one of
    # those verdicts while every outcome above still passed.
    #
    # The needles live in a STAGED FILE and not in the command line. The start
    # marker echoes the argv, so a `-c 'printf "on stderr"'` spelling puts the
    # needle into the marker itself and the case then matches its own label --
    # measured, and it reported a stderr leak on a wrapper whose streams were
    # correctly separated. A probe that matches its own text.
    selfTestCases=$((selfTestCases + 1))
    Stage "streams.sh" 'printf "CMake Error: pretend\nsecond line\n"; printf "e-r-r-needle\n" >&2'
    cmdOut="$("$interpreter" "$me" --command -- "$interpreter" "${scratch}/streams.sh" 2>/dev/null)"
    cmdErr="$("$interpreter" "$me" --command -- "$interpreter" "${scratch}/streams.sh" 2>&1 >/dev/null)"
    byteOk=1
    case "$cmdOut" in
        *"CMake Error: pretend"*"second line"*) ;;
        *) echo "  FAIL  the command's stdout did not survive on stdout" >&2; byteOk=0 ;;
    esac
    # stderr must stay stderr. If the wrapper merged the streams, the fail-pattern
    # would still match and this would be the only case that noticed.
    case "$cmdErr" in
        *"e-r-r-needle"*) ;;
        *) echo "  FAIL  the command's stderr did not survive on stderr" >&2; byteOk=0 ;;
    esac
    case "$cmdOut" in
        *"e-r-r-needle"*) echo "  FAIL  stderr leaked into stdout" >&2; byteOk=0 ;;
    esac
    if [ "$byteOk" -eq 1 ]; then
        echo "  ok    --command: the command's bytes survive, on the streams it wrote them to"
    else
        selfTestStatus=1
    fi

    # And the marker must not itself match the live fail-pattern, or wrapping a
    # PASSING check would turn it red. `check-run-check-coverage.sh` asserts this
    # against the patterns resolved from the registrations; this asserts it
    # against the one spelling that matters most, here, where the marker is
    # defined -- a guard in one file about a constant in another goes stale.
    selfTestCases=$((selfTestCases + 1))
    # A HERESTRING, never `printf ... | grep -q`. That idiom is a false NEGATIVE
    # under `pipefail` on the SUCCESS path -- `grep -q` exits at the first match,
    # the producer dies of SIGPIPE, and `pipefail` reports the producer's status.
    # `check-e2e-helpers.sh` scans for it, and it caught this exact line here.
    markerText="${StartMarker}
${TerminalMarker}"
    if grep -Eq 'CMake Error|CMake Warning' <<< "$markerText"; then
        echo "  FAIL  a marker matches the script-check fail-pattern, so wrapping a PASSING check would redden it" >&2
        selfTestStatus=1
    else
        echo "  ok    --command: no marker matches CMake Error|CMake Warning"
    fi

    # A self-test that stopped early must not look like one that judged something.
    echo "run-check: self-test ran ${selfTestCases} case(s)"
    if [ "$selfTestStatus" -ne 0 ]; then
        echo "run-check: self-test FAILED" >&2
        exit 1
    fi
    echo "run-check: self-test passed"
    exit 0
fi

# ## `--command`: the same four outcomes for an arbitrary command (#1103)
#
# #1079 covered the registered `scripts/check-*.sh` invocations. The registered
# `cmake -P` ones were left out, and the argument for leaving them is that they
# are ALREADY distinguishable: a `cmake -P` check's verdict is read from its
# output (`FAIL_REGULAR_EXPRESSION`, because `message(WARNING)` exits 0 while
# printing `CMake Warning`), so a killed one prints no `CMake Error` where a
# failing one does.
#
# That is true and it is not sufficient. **The tell is an ABSENCE** -- there is a
# positive signal for the failing case and none for the killed one -- which is
# this file's opening sentence one step weaker rather than a different situation.
# And it is the half where the absence reads most innocently: a `cmake -P` check
# killed early looks exactly like one that ran and found nothing, right up until
# ctest reports `***Failed` on the status alone.
#
# ## Why this is a MODE here and not a second script
#
# #1103 argued a command-taking wrapper is "a different tool rather than a wider
# pattern". Only the SPAWN differs -- the markers, the four-way classification,
# the status pass-through, the stdout discipline and the self-test are the same
# object. A second script would duplicate all of that, and the duplicate this
# file's own header warns about is precisely a second spelling of the marker:
# *"a marker each of 22 scripts spells itself is 22 chances to spell it
# differently"*. So the invocation is DATA and there is one tool, which is also
# the repository's data-driven rule rather than a preference.
#
# ## What must not change, and does not
#
# The child INHERITS stdout and stderr; nothing here buffers or re-prints, so the
# command's bytes reach ctest byte-identical and on the stream it wrote them to.
# That is load-bearing: 74 registrations are judged by a regex over those bytes,
# and a wrapper that re-emitted them would change every one of those verdicts.
# The markers cannot collide with the fail-patterns either -- asserted by
# `MarkersAreSafe` in `check-run-check-coverage.sh`, against the patterns
# RESOLVED from the registrations rather than a copy of them.
if [ "${1:-}" = "--command" ]; then
    shift
    [ "${1:-}" = "--" ] && shift
    if [ $# -eq 0 ]; then
        echo "usage: $0 --command -- <argv...>" >&2
        exit 2
    fi
    # The whole argv is the label. A `cmake -P` invocation's interesting part is
    # its `-D` arguments as much as its script -- two registrations of one script
    # differing only in a `-D` are ordinary here -- so truncating to the script
    # path would make two different runs print the same marker.
    commandLabel="$*"
    if ! command -v "$1" >/dev/null 2>&1 && [ ! -x "$1" ]; then
        # Same reasoning as the missing-script path below, including the stream:
        # on stderr, a reader capturing only stdout would see a start marker and
        # no terminal one, which is this file's own spelling of DID NOT CONCLUDE.
        StartLine "$commandLabel"
        echo "${TerminalMarker}: failed -- no such command: ${1}"
        exit 2
    fi
    [ "$treeVerdict" = "refuse" ] && RefuseUnreadableTree "$commandLabel"
    StartLine "$commandLabel"
    "$@"
    commandStatus=$?
    echo "${TerminalMarker}: $(Classify "$commandStatus") -- ${commandLabel} (exit ${commandStatus}$(StatusDetail "$commandStatus"))"
    exit "$commandStatus"
fi

check="${1:-}"
if [ -z "$check" ]; then
    echo "usage: $0 <check-script> [args...]" >&2
    echo "       $0 --command -- <argv...>" >&2
    echo "       $0 --self-test" >&2
    exit 2
fi
shift

if [ ! -f "$check" ]; then
    # A check that is not there has not passed, and saying so by name beats the
    # interpreter's own message about a path.
    StartLine "$check"
    # The marker goes to STDOUT, like every other path out. On stderr a reader
    # capturing only stdout would see a start marker and no terminal one, which is
    # this file's own spelling of DID NOT CONCLUDE -- a state collapse in the
    # script written to remove one.
    echo "${TerminalMarker}: failed -- no such check script: ${check}"
    exit 2
fi

[ "$treeVerdict" = "refuse" ] && RefuseUnreadableTree "$check ${*:-}"
StartLine "$check ${*:-}"

# The check decides its own MSYS path conversion; it never inherits the caller's (#1355).
#
# `.agent/rules/build-and-toolchain.md` tells anyone driving MSVC from Git Bash to export
# `MSYS_NO_PATHCONV=1` and `MSYS2_ARG_CONV_EXCL='*'`, and a developer who did so and then ran
# `ctest` handed that decision to every check. A check spelling a path the POSIX way --
# `pwd` answers `/d/...`, `mktemp` answers `/tmp/...` -- and giving it to a NATIVE program
# relies on the conversion those two variables switch off. Measured on native Windows with the
# switch exported, five hygiene entries failed and each read as a defect in the tree:
# `mkdocs-validation` reported `/d/...\mkdocs.yml does not exist`, `git.exe` could not `cd`
# into Git Bash's `/tmp`, `workflow-script-invocations` exited 128 saying nothing, and two
# self-tests refused their own fixtures.
#
# Under `ctest` this is already done: `src/tests/CMakeLists.txt` starts every test on Windows
# without the switch. This is the same rule for a check run BY HAND through this wrapper.
# Unset here, just before the check starts, so the default applies to what the CHECK spawns.
# A check that must spawn with conversion off -- `check-banner-probe-identity.sh` drives a
# compiler with `/`-led flags -- already pins it itself, which is the only place that can
# know it needs to. `--command` above is deliberately left alone: its argv is native already,
# and conversion is exactly what could mangle it. Starting bash with bash converts nothing,
# so this reaches the check's children and not the check's own arguments.
unset MSYS_NO_PATHCONV MSYS2_ARG_CONV_EXCL

"$interpreter" "$check" ${1+"$@"}
status=$?

outcome="$(Classify "$status")"
echo "${TerminalMarker}: ${outcome} -- ${check} (exit ${status}$(StatusDetail "$status"))"

exit "$status"
