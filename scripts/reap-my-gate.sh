#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Kill THIS lane's `local-gate.sh` and nobody else's.
#
# ---------------------------------------------------------------------------
# Why a script and not a note
# ---------------------------------------------------------------------------
#
# `pkill -f "local-gate.sh"` matches every lane's gate on the machine. Each
# worktree runs the same three script names, so a name match is guaranteed to hit
# somebody else's process, and it did twice in one night (#591) -- about ten
# minutes of gate time each time.
#
# The failure is quiet in a specific way: a gate killed mid-run prints NEITHER
# `GATE FAILED` nor `LOCAL GATE PASSED`, so the victim is holding output that
# describes nothing and reads like a crash in their own tree. They re-run, and lose
# the time twice.
#
# The rulebook already says a claim about a tool is checked against the tool. The
# defect was not ignorance of `pkill`; it was reasoning about the command's INTENT
# and never about its MATCH SET. A note asking people to be careful does not
# survive that, because the mistake feels correct while it is being made. So this
# is a helper that cannot express the wrong thing.
#
# ---------------------------------------------------------------------------
# What identifies a process, and what does not
# ---------------------------------------------------------------------------
#
# **A COMMAND LINE NARROWS; IT NEVER ATTRIBUTES.** Three measured traps, none of
# which a more careful `pkill` avoids:
#
#   * `pgrep -f "scripts/local.gate"` is a REGEX and the `.` matches the `-`. A
#     pattern is broader than its author reads it as.
#   * `pgrep -f` matches ITS OWN command line, and the bracket trick does NOT save
#     it -- see below, because the two spellings are one character apart and only
#     one is safe.
#   * two worktrees here are named `SAN-408` and `SAN-REQ-408`, one token apart, so
#     even an exact-looking path fragment matches a neighbour.
#
# **THE BRACKET TRICK WORKS IN ONE SPELLING AND NOT THE OTHER, FOR REASONS THAT ARE
# NOT THE ONE IT IS USUALLY CREDITED WITH.** Both measured on this machine:
#
#   * `pgrep -f '[l]ocal-gate.sh'` **self-matches.** pgrep tests the pattern against
#     every argv INCLUDING ITS OWN, where the bracket is still literally present, so
#     `[l]ocal-gate.sh` matches the text `[l]ocal-gate.sh`.
#   * `ps -eo cmd | grep -c '[l]ocal-gate.sh'` **does not.** grep searches the text
#     `ps` produced; its own argv holds `[l]ocal-gate.sh`, which does not contain
#     the literal `local-gate.sh` the regex matches.
#
# Stated in both directions on purpose: the next reader who finds only the safe
# spelling will otherwise copy it for the wrong reason and then simplify it back.
#
# The observed cost of the unsafe one is precise. A `pgrep -f` listing taken from a
# lane's own worktree came back with a fourth row -- its own `bash -lc`, `etime=00:00`,
# `cwd=/mnt/d/fastcached-worktrees/wire` -- which **attributed by cwd is
# indistinguishable from that lane's gate having just started.** A killer built on
# that targets its own shell.
#
# So the candidate set is built by reading each process's argv for a LITERAL, with
# no regex engine involved at all -- and then every candidate is ATTRIBUTED by
# something the command line cannot fake.
#
# **THE ASKING PROCESS IS IN THE CANDIDATE SET, and that is the root of both
# hazards above rather than two separate ones.** The self-match and the cwd
# coincidence are the same fact: a probe necessarily runs from the worktree of the
# lane asking the question, so it looks exactly like that lane's gate. Cwd
# attribution is therefore NECESSARY AND NOT SUFFICIENT. `gate_protected_pids`
# walks `$$` and its ancestors and removes them before any cwd is consulted, and
# `spare-self` is decided FIRST in `reap_verdict` for the same reason.
#
# **THE ROOT IS ATTRIBUTED BY ITS WORKING DIRECTORY; EVERYTHING ELSE BY ANCESTRY.**
# That reconciles two rules that read as contradictory. `team-run.md` says to reap
# by `/proc/<pid>/cwd` rather than `pkill -f`; the general rule says a process is
# attributed by its ancestor chain and never by a leaf `cwd`. Both are right about
# different halves:
#
#   * a gate's OWN cwd is its worktree, which is what tells two lanes apart, and no
#     other signal does;
#   * a gate's CHILDREN -- cmake, ninja, ctest and every test process -- have cwds
#     of their own, in build directories and scratch trees, so a cwd match would
#     miss them. Killing the parent alone orphans them, and an orphaned `ctest`
#     races the replacement run in the same build directory.
#
# Root by cwd, descendants by PPID. Neither alone is the rule.
#
# **READ THE LINK RAW.** `readlink -f` resolves the path and FAILS on a deleted
# directory, which is exactly the state a stranded gate reaches once its build tree
# is removed -- measured here: a sweep built on `readlink -f` reported 0 left while
# three were still holding ports. The raw link keeps the ` (deleted)` suffix and
# finds them.
#
# ---------------------------------------------------------------------------
# Failing closed
# ---------------------------------------------------------------------------
#
# **"Cannot determine the cwd" REFUSES TO KILL, and never falls back to a name
# match.** A fallback would restore the exact behaviour this exists to prevent, on
# the platforms where it is hardest to notice. That makes refuse-to-kill the
# DEFAULT answer, with Linux currently the one platform able to say otherwise.
#
# It also never signals itself or any of its own ancestors. Its own cwd is the
# worktree it was asked about, so without that it would be its own first victim --
# and would take the shell that invoked it with it.
#
# ---------------------------------------------------------------------------
# Why this does not read the gate lock
# ---------------------------------------------------------------------------
#
# The gate takes `$HOME/.fastcached-local-gate.lock` ITSELF now (#1379). This section
# used to name `~/gate-logs/.gate.lock` and to say a gate run by hand takes no lock --
# both were one lane's wrapper convention, which is exactly what that ticket retired.
# Reading the lock to find the victim is still declined, for three reasons rather than
# from not knowing about it:
#
#   * **A `flock` lock names no process a script can read.** Its holder is the gate's
#     own `flock` process, whose CHILD is the run, so finding anything worth signalling
#     needs the same ancestry walk this already does.
#   * **The descendants hold nothing.** The gate re-execs under `flock -o`, so cmake,
#     ninja and ctest -- what orphaning actually costs -- carry no lock and none names
#     them.
#   * **Killing the holder ALONE is the one wrong kill.** It frees the lock while the
#     run it serialised carries on, and a second gate starts beside it. This helper
#     takes every gate process rooted at the worktree, that `flock` included -- its
#     command line names this script and it runs from the tree the gate `cd`s into --
#     which is the right shape.
#
# Said here so the next reader knows it was weighed rather than missed.
#
# Usage:
#   scripts/reap-my-gate.sh [<worktree>]   kill the gate rooted at <worktree>
#                                          (default: this script's own repository)
#   scripts/reap-my-gate.sh --dry-run [<worktree>]
#   scripts/reap-my-gate.sh --self-test
#
# Exit: 0 whatever it found. Killing nothing is an ordinary outcome -- no gate was
# running -- and a status that distinguished it would be read as an error.
set -uo pipefail

# The one `/proc/<pid>/stat` reader, shared with local-gate.sh's lock-holder walk.
# shellcheck source=lib/proc-stat.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/proc-stat.sh" \
    || { echo "reap: cannot read scripts/lib/proc-stat.sh, so no ancestry can be walked; REFUSING" >&2; exit 1; }

# ---------------------------------------------------------------------------
# The decision
# ---------------------------------------------------------------------------

# What to do with one candidate, from facts alone.
#
# PURE: no process, no filesystem, no clock. Every branch is one staged line in the
# self-test, which is what lets the refusing branches be driven at all -- a process
# whose cwd this user may not read cannot be arranged on purpose.
#
# @param 1 the candidate's cwd, or "" when it could not be determined
# @param 2 the worktree being reaped
# @param 3 "yes" when the candidate is this process or one of its ancestors
# @return echoes one of: kill | spare-self | spare-elsewhere | spare-unknown-cwd
reap_verdict() {
    local cwd="$1" root="$2" protected="$3"

    # Self first, and before the cwd is even consulted: this script's own cwd is
    # very often the root it was asked about, so any other order makes it its own
    # first victim.
    if [ "$protected" = "yes" ]; then
        echo "spare-self"
        return 0
    fi

    # Unknown BEFORE elsewhere, because "" is not a path outside the root -- it is
    # the absence of an answer, and reporting it as "somewhere else" would be a
    # confident claim drawn from nothing.
    if [ -z "$cwd" ]; then
        echo "spare-unknown-cwd"
        return 0
    fi

    # The root itself, or anything under it. A prefix test and not a `case` glob:
    # a worktree path may contain `*` or `[` in principle, and a glob would then
    # match by accident. The trailing separator is what stops `/w/fixtures` from
    # matching `/w/fixtures-2`, which is the `SAN-408` / `SAN-REQ-408` shape.
    if [ "$cwd" = "$root" ] || [ "${cwd#"${root}/"}" != "$cwd" ]; then
        echo "kill"
        return 0
    fi

    echo "spare-elsewhere"
}

# ---------------------------------------------------------------------------
# Acquisition
# ---------------------------------------------------------------------------

# The literal every candidate's argv must contain. A constant, so the narrowing
# step and the self-test's decoys cannot spell it differently.
ReapGateMarker="local-gate.sh"

# One process's working directory, or nothing when it cannot be read.
#
# Linux reads the link RAW for the deleted-directory reason above. macOS has no
# `/proc`, so `lsof` answers instead; where neither works this prints nothing, and
# the verdict above turns that into a refusal rather than a guess.
#
# @param 1 pid
# @return echoes the cwd; non-zero when it could not be determined
gate_cwd_of() {
    local pid="$1" link=""
    if [ -e "/proc/${pid}/cwd" ]; then
        link="$(readlink "/proc/${pid}/cwd" 2>/dev/null)" || link=""
        [ -n "$link" ] || return 1
        printf '%s\n' "$link"
        return 0
    fi
    if command -v lsof >/dev/null 2>&1; then
        # `-Fn` is the machine-readable form: one field per line, the cwd on the
        # line beginning `n`. Parsed with a herestring rather than a pipe into an
        # early-exiting consumer.
        local out=""
        out="$(lsof -a -p "$pid" -d cwd -Fn 2>/dev/null)" || out=""
        local line=""
        while IFS= read -r line || [ -n "$line" ]; do
            case "$line" in
                n/*) printf '%s\n' "${line#n}"; return 0 ;;
            esac
        done <<< "$out"
    fi
    return 1
}

# Every pid whose argv contains the marker. NARROWING ONLY -- see the header.
#
# `/proc` is walked directly rather than through `pgrep -f`, which is a regex and
# which matches its own command line. There is no pattern here to be broader than
# its author reads it: `case` with a literal, over bytes read from the kernel.
gate_candidates() {
    local entry pid args
    if [ -d /proc ]; then
        for entry in /proc/[0-9]*; do
            pid="${entry#/proc/}"
            # NUL-separated; `tr` makes it greppable. A cmdline that cannot be read
            # is another process's, and skipping it is correct rather than lossy --
            # this helper only ever kills what it can attribute. `2>` BEFORE `<`:
            # redirections apply left to right, so a process that exited since the
            # glob printed "No such file or directory" past a `2>` written after.
            args="$(tr '\0' ' ' 2>/dev/null < "${entry}/cmdline")" || continue
            case "$args" in
                *"$ReapGateMarker"*) printf '%s\n' "$pid" ;;
            esac
        done
        return 0
    fi
    # macOS and anything else with a POSIX ps. `-o pid=,args=` so there is no
    # header line to skip and no locale-dependent column to count.
    local line rest
    while IFS= read -r line || [ -n "$line" ]; do
        line="${line# }"
        pid="${line%% *}"
        rest="${line#* }"
        case "$rest" in
            *"$ReapGateMarker"*) printf '%s\n' "$pid" ;;
        esac
    done <<< "$(ps -Ao pid=,args= 2>/dev/null)"
}

# One process's parent, or nothing.
#
# `/proc` FIRST, `ps -o` only where there is none. Git Bash's `ps` is not POSIX: it
# takes `[-aefls] [-u UID] [-p PID]` and nothing else, so `ps -o ppid=` there is a
# usage error that prints nothing -- and an empty parent ended the ancestry walk at
# `$$`, leaving the reaper's own CALLER unprotected. Measured: the self-test's
# wrapper case killed the wrapper that invoked the reaper on 20 of 20 Git Bash runs.
gate_ppid_of() {
    local pid="$1" out=""
    if [ -d /proc ]; then
        proc_stat_ppid "$pid" || return 1
        printf '%s\n' "$proc_stat_reply"
        return 0
    fi
    out="$(ps -o ppid= -p "$pid" 2>/dev/null)" || return 1
    out="${out// /}"
    [ -n "$out" ] || return 1
    printf '%s\n' "$out"
}

# This process and every one of its ancestors, so none of them can be a victim.
#
# Bounded rather than looped to a root condition: a pid whose parent chain is
# circular or unreadable would otherwise hang the reaper, and a hang here is worse
# than an over-broad spare list.
#
# Returns 1 when the walk STOPPED SHORT -- a parent it could not read, or the bound
# -- rather than reaching pid 1 or a parent of 0. A short list is not an over-broad
# one: every ancestor it failed to name is unprotected, and one of them is the
# caller. That is exactly how Git Bash's `ps` made the reaper kill its invoker, so
# the reaper refuses on this status instead of reaping with half a spare list.
#
# On Cygwin and MSYS2 a parent of `1` is NOT init: it is how the runtime spells "my
# parent is not one of my processes" -- a native Windows process, whose own parent
# this runtime cannot name. So there it is a walk that stopped short, never a root,
# told apart by `/proc/<pid>/winpid`, which only those runtimes write. Reading it as
# the root let a native INTERMEDIARY hide the caller: local-gate -> powershell.exe ->
# bash reap-my-gate.sh walked "completely" to 1 and listed its own wrapper as a
# victim (review I1, measured with `--dry-run`). The cost is that the reaper refuses
# on every Git Bash chain, which starts at a native process, and that is the
# direction to fail in: a refusal there is an inconvenience, the kill was the caller.
gate_protected_pids() {
    local pid="$$" depth=0 parent
    while [ "$depth" -lt 64 ]; do
        printf '%s\n' "$pid"
        [ "$pid" != "1" ] || return 0
        parent="$(gate_ppid_of "$pid")" || return 1
        [ -n "$parent" ] || return 1
        [ "$parent" != "0" ] || return 0
        if [ "$parent" = "1" ] && [ -e "/proc/${pid}/winpid" ]; then
            return 1
        fi
        pid="$parent"
        depth=$(( depth + 1 ))
    done
    return 1
}

# Every descendant of the given pids, from ONE snapshot of the process table.
#
# The gate's children have cwds of their own, so they are reached by ANCESTRY and
# not by the cwd test that selected their root. Bounded rather than looped to a
# termination condition: a circular or unreadable parent chain would otherwise hang
# the reaper, and a hang here is worse than an over-broad spare list.
#
# ONE `ps -Ao pid=,ppid=`, filtered in the shell, rather than `ps --ppid` per level:
# that flag is GNU-only and this script runs on macOS, where it is a usage error
# that prints nothing -- which reads exactly like a process with no children.
#
# The fields are SPLIT, never cut at the first space: `ps -Ao pid=,ppid=` right-aligns
# both columns (`      7       2` on procps), and trimming ONE leading space left the
# pid empty, so every row of a real table was skipped and no gate ever had a child.
# Measured on WSL at the base of this change: a live child of the walking shell, over
# a real `ps` table, came back as nothing. The staged tables had no padding.
#
# @param 1 newline-separated root pids
# @param 2 the process table, as `pid ppid` lines, padded or not (injected so the
#          self-test can stage a tree without forking one)
gate_descendants_of() {
    local roots="$1" table="$2"
    local depth=0 next found line pid parent
    found=""
    while [ "$depth" -lt 32 ] && [ -n "$roots" ]; do
        next=""
        while read -r pid parent _ || [ -n "$pid" ]; do
            [ -n "$pid" ] && [ -n "$parent" ] || continue
            while IFS= read -r line || [ -n "$line" ]; do
                [ -n "$line" ] || continue
                if [ "$parent" = "$line" ]; then
                    next="${next}${pid}"$'\n'
                    found="${found}${pid}"$'\n'
                fi
            done <<< "$roots"
        done <<< "$table"
        roots="$next"
        depth=$(( depth + 1 ))
    done
    printf '%s' "$found"
}

# The process table this run reads, as `pid ppid` lines -- from `/proc` where there
# is one, for the reason `gate_ppid_of` gives: Git Bash's `ps -Ao` is a usage error,
# and an empty table reads exactly like a gate with no children, so a reaped gate's
# `ctest` would be orphaned rather than signalled.
gate_process_table() {
    local entry pid parent
    if [ -d /proc ]; then
        for entry in /proc/[0-9]*; do
            pid="${entry#/proc/}"
            # Read through the reply variable, never `$(...)`: this runs once per process on
            # the machine, and a command substitution is a fork each time.
            proc_stat_ppid "$pid" || continue
            printf '%s %s\n' "$pid" "$proc_stat_reply"
        done
        return 0
    fi
    ps -Ao pid=,ppid= 2>/dev/null || true
}

# ---------------------------------------------------------------------------
# The run
# ---------------------------------------------------------------------------

# Report and reap.
#
# It prints what it LEFT ALONE as well as what it killed, and that is the point
# rather than politeness: the whole claim of this helper is that it declined to
# touch a neighbour, and a caller cannot see a refusal that prints nothing. A
# helper that silently killed nothing would produce identical output to one that
# correctly spared everything.
#
# @param 1 the worktree to reap
# @param 2 "dry-run" to decide and report without signalling
reap_my_gate() {
    local root="$1" mode="${2:-}"
    local protected candidates table pid cwd verdict
    local killed="" spared=0 unknown=0
    if ! protected="$(gate_protected_pids)"; then
        echo "reap: this reaper's own ancestry could not be read to its root (it stopped at $(printf '%s\n' "$protected" | tail -n 1):"
        echo "reap:   a parent it could not read, or -- on Cygwin/MSYS -- a native process, whose own parent"
        echo "reap:   that runtime cannot name), so the processes a kill would take from under it are not known."
        echo "reap:   REFUSING to signal anything."
        # The NEXT STEP, not only the refusal: under Git Bash every chain starts at a native
        # process, so this refusal is certain there -- and the gate runs under WSL, where the
        # ancestry is walkable. Relative to the worktree, so no path needs translating: MSYS
        # rewrites a `/mnt/...` argument handed to a Windows program, and the reaper's default
        # root is the repository it sits in.
        if [ -e "/proc/$$/winpid" ]; then
            echo "reap: the gate runs under WSL; run this reaper there, from the worktree:"
            echo "reap:   cd '${root}' && wsl.exe -e bash scripts/reap-my-gate.sh${mode:+ --${mode}}"
        fi
        return 1
    fi
    candidates="$(gate_candidates)"
    table="$(gate_process_table)"

    local line
    while IFS= read -r pid || [ -n "$pid" ]; do
        [ -n "$pid" ] || continue
        local isProtected="no"
        while IFS= read -r line || [ -n "$line" ]; do
            [ "$line" = "$pid" ] && isProtected="yes"
        done <<< "$protected"
        cwd="$(gate_cwd_of "$pid")" || cwd=""
        verdict="$(reap_verdict "$cwd" "$root" "$isProtected")"
        case "$verdict" in
            kill)
                killed="${killed}${pid}"$'\n'
                echo "reap: ${pid} is this lane's gate (cwd ${cwd})"
                ;;
            spare-self)
                echo "reap: sparing ${pid} -- it is this reaper or one of its ancestors"
                spared=$(( spared + 1 ))
                ;;
            spare-elsewhere)
                echo "reap: sparing ${pid} -- another lane's gate (cwd ${cwd})"
                spared=$(( spared + 1 ))
                ;;
            spare-unknown-cwd)
                echo "reap: sparing ${pid} -- its working directory could not be read, and a"
                echo "reap:   name match is not an attribution. REFUSING rather than guessing."
                unknown=$(( unknown + 1 ))
                ;;
        esac
    done <<< "$candidates"

    if [ -z "$killed" ]; then
        echo "reap: no gate of this lane's was running (${spared} spared, ${unknown} unattributable)"
        return 0
    fi

    # Descendants LAST and by ancestry: cmake, ninja, ctest and every test process
    # have cwds of their own, so the cwd test above cannot see them -- and a gate
    # killed alone orphans an in-flight `ctest` that then races the replacement run
    # in the same build directory.
    local victims
    victims="${killed}$(gate_descendants_of "$killed" "$table")"

    if [ "$mode" = "dry-run" ]; then
        echo "reap: DRY RUN -- would signal:"
        while IFS= read -r pid || [ -n "$pid" ]; do
            [ -n "$pid" ] && echo "reap:   ${pid}"
        done <<< "$victims"
        return 0
    fi

    # TERM, then KILL what is left. The gate traps EXIT and its cleanup is worth
    # letting run; bash DEFERS a trapped signal until the current child returns, so
    # a `ninja` mid-link can hold it for a while -- which is why the second pass
    # exists rather than an immediate `-KILL`.
    while IFS= read -r pid || [ -n "$pid" ]; do
        [ -n "$pid" ] && kill -TERM "$pid" 2>/dev/null || true
    done <<< "$victims"
    # A bounded pause, from `sleep`, which counts a RELATIVE interval and cannot be
    # moved by the wall clock this host steps.
    sleep 2
    while IFS= read -r pid || [ -n "$pid" ]; do
        [ -n "$pid" ] || continue
        if kill -0 "$pid" 2>/dev/null; then
            kill -KILL "$pid" 2>/dev/null || true
            echo "reap: ${pid} did not stop on TERM and was killed"
        fi
    done <<< "$victims"

    local n
    n="$(printf '%s\n' "$victims" | grep -c . || true)"
    echo "reap: signalled ${n} process(es) of this lane's gate; spared ${spared}, refused ${unknown} unattributable"
}

# ---------------------------------------------------------------------------
# The self-test
# ---------------------------------------------------------------------------

reap_self_test() {
    local ran=0 failed=0 skipped=""

    _reap_expect() {
        ran=$(( ran + 1 ))
        if [ "$2" != "$3" ]; then
            echo "REAP SELF-TEST FAILED: $1: expected '$2', got '$3'" >&2
            failed=$(( failed + 1 ))
        fi
    }

    # --- the decision, over staged facts ---------------------------------
    #
    # Every branch, including the two that cannot be arranged on a real machine:
    # a process whose cwd this user may not read, and one that is this reaper's
    # own ancestor.
    _reap_expect "a candidate inside the worktree is killed" \
        "kill" "$(reap_verdict /w/mine /w/mine no)"
    _reap_expect "and so is one in a subdirectory of it" \
        "kill" "$(reap_verdict /w/mine/out/build /w/mine no)"
    _reap_expect "a candidate in another worktree is spared" \
        "spare-elsewhere" "$(reap_verdict /w/other /w/mine no)"
    # The `SAN-408` / `SAN-REQ-408` shape, and the reason the prefix test carries a
    # separator: without it every neighbour whose path merely STARTS with this one
    # is killed, which is the defect in its most confident form.
    _reap_expect "a sibling worktree whose name extends this one is spared" \
        "spare-elsewhere" "$(reap_verdict /w/mine-2 /w/mine no)"
    # This reaper's own cwd is very often the root it was asked about, so self is
    # tested BEFORE the cwd and must win even when the cwd says kill.
    _reap_expect "this reaper is spared even standing in the worktree" \
        "spare-self" "$(reap_verdict /w/mine /w/mine yes)"
    # An unreadable cwd is REFUSED, and it is refused as its own outcome: reporting
    # it as `spare-elsewhere` would be a claim about where the process is, drawn
    # from having failed to find out.
    _reap_expect "an unreadable cwd refuses, and is not called 'elsewhere'" \
        "spare-unknown-cwd" "$(reap_verdict '' /w/mine no)"
    # A deleted directory is the state a stranded gate reaches, and `readlink -f`
    # fails on it -- so the raw link keeps the suffix and it must still match.
    _reap_expect "a deleted working directory is still attributed" \
        "kill" "$(reap_verdict '/w/mine (deleted)' '/w/mine (deleted)' no)"

    # Four outcomes, and they must be four. Three rows answering alike would pass
    # every assertion above while the helper decided one thing.
    local distinct
    distinct="$( { reap_verdict /w/mine /w/mine no
                   reap_verdict /w/other /w/mine no
                   reap_verdict /w/mine /w/mine yes
                   reap_verdict '' /w/mine no; } | sort -u | grep -c . )"
    _reap_expect "the decision has four distinct answers" "4" "$distinct"

    # --- an ancestry that cannot be read to its root -------------------------
    #
    # Staged by making the parent lookup answer nothing, in a subshell so the real
    # one is untouched: the walk must say it stopped short, and the reaper must then
    # refuse without signalling -- not reap with a spare list naming only itself.
    # `if`, never `case`, inside `$( )`: bash 3.2 reads a case pattern's `)` as the
    # substitution's end (#1224).
    _reap_expect "an ancestry that stops short says so" \
        "1" "$( ( gate_ppid_of() { return 1; }; gate_protected_pids >/dev/null; echo $? ) )"
    _reap_expect "and the reaper then refuses rather than reaping" \
        "1 refused" "$( ( gate_ppid_of() { return 1; }
                         out="$(reap_my_gate /nonexistent-root 2>&1)"; status=$?
                         if [[ "$out" == *REFUSING* ]]; then echo "$status refused"; else echo "$status [$out]"; fi ) )"
    # The walk from THIS shell: complete to the root, except on Cygwin/MSYS, where every
    # chain starts at a native process and so says it stopped short. The wanted answer is
    # derived from the runtime, and the real-process half below does not run where the
    # chain cannot be walked -- it would only watch the reaper refuse.
    local wantWalk=0 walkable=yes
    if [ -e "/proc/$$/winpid" ]; then wantWalk=1; fi
    _reap_expect "a walk from this shell is complete, or on Cygwin/MSYS says it is not" \
        "$wantWalk" "$( ( gate_protected_pids >/dev/null; echo $? ) )"
    ( gate_protected_pids >/dev/null ) || walkable=no

    # --- the ancestry walk, over a staged process table ------------------
    local table="100 1
200 100
300 200
400 1
500 400"
    _reap_expect "a child and a grandchild are both reached" \
        "200 300" "$(printf '%s' "$(gate_descendants_of '100' "$table")" | tr '\n' ' ' | sed 's/ $//')"
    _reap_expect "an unrelated tree is not" \
        "" "$(printf '%s' "$(gate_descendants_of '999' "$table")" | tr '\n' ' ' | sed 's/ $//')"
    # The shape `ps -Ao pid=,ppid=` really prints, right-aligned. The rows above have
    # no padding, which is why they passed while every real table was skipped.
    local padded="    100      1
    200    100
    300    200"
    _reap_expect "a right-aligned table, as ps prints it, is walked too" \
        "200 300" "$(printf '%s' "$(gate_descendants_of '100' "$padded")" | tr '\n' ' ' | sed 's/ $//')"

    # --- the process table and the parent lookup, against THIS process -------
    #
    # Asked of the running shell, whose parent the shell itself knows (`$PPID`), so
    # the answer is checked against a second source rather than against itself. A
    # lookup that answers nothing -- Git Bash's `ps -o` -- ends the ancestry walk at
    # `$$` and leaves the caller killable, and it did.
    _reap_expect "this shell's parent is read" "$PPID" "$(gate_ppid_of "$$")"
    local selfRow="no" rowPid rowParent
    while read -r rowPid rowParent _ || [ -n "$rowPid" ]; do
        [ "$rowPid" = "$$" ] && [ "$rowParent" = "$PPID" ] && selfRow="yes"
    done <<< "$(gate_process_table)"
    _reap_expect "and the process table holds this shell under its parent" "yes" "$selfRow"

    # --- and against real processes --------------------------------------
    #
    # THE TICKET'S OWN ACCEPTANCE, and both halves or neither: a helper that kills
    # everything passes the second on its own, and one that kills nothing passes
    # the first. Each is satisfied by a broken helper of the opposite kind.
    #
    # Both decoys carry the marker in their argv and differ ONLY in their working
    # directory, which is what makes this a test of the attribution rather than of
    # the narrowing.
    #
    # `spare-self` gets a LIVE case of its own below, and it is the one two lanes
    # independently pointed at: **the process doing the asking is in the candidate
    # set.** An earlier draft of this file called that arrangement recursive and
    # left it staged. It is not -- the wrapper calls the REAPER, not this
    # self-test -- and it is the case that matters most, because the failure is a
    # helper that kills the shell that invoked it.
    # Can this platform attribute ANOTHER process's working directory? Asked by
    # DOING it against a process staged in a known directory -- never by testing for
    # `/proc` or for an `lsof` binary.
    #
    # That distinction is the whole of #1224's macOS red. macOS has no `/proc` and
    # DOES ship `lsof`, so the old capability test answered yes while `gate_cwd_of`
    # answered nothing. Measured on `macOS-clang-release`: the two "must die" cases
    # failed and every "must be spared" case passed, because a reaper that attributes
    # nothing spares everything. **The break is in the direction that looks like
    # caution** -- the cases a reader takes as proof of care are exactly the ones that
    # pass when the attribution is dead -- so only a control can see it, and a
    # tool-presence test never could.
    #
    # It reports WHAT IT SAW rather than a bare no: "this platform cannot" and "the
    # probe never came up" are different states, and a skip reason that cannot tell
    # them apart sends the next reader to the wrong question. The comparison is on the
    # exact string because that is what `reap_verdict`'s prefix test needs -- a
    # platform that answers a different spelling of the same directory (a resolved
    # `/private/var` for a `/var` path, say) genuinely cannot attribute here, and
    # saying so is more use than a match that would not have held.
    _reap_attribution_detail=""
    # Every staged process sleeps this long BEFORE its `cd` and `exec`, which WIDENS
    # the window between the fork and the state the reaper reads. On purpose: left at
    # its natural width that window is milliseconds, so a wait on the wrong condition
    # (`kill -0`, a single ask) failed about 1 run in 10 -- and a neuter of it stayed
    # green over 10 runs. Widened, the wrong wait fails every time.
    local stageDelay=0.5
    _reap_can_attribute_cwd() {
        local dir probePid seen waited=0
        dir="$(mktemp -d)"
        ( sleep "$stageDelay"; cd "$dir" && exec sleep 30 ) >/dev/null 2>&1 &
        probePid=$!
        while [ "$waited" -lt 50 ]; do
            kill -0 "$probePid" 2>/dev/null && break
            sleep 0.1
            waited=$(( waited + 1 ))
        done
        if ! kill -0 "$probePid" 2>/dev/null; then
            _reap_attribution_detail="the probe process never came up, so this platform was never asked"
            rm -rf "$dir"
            return 1
        fi
        # A pid that EXISTS is not yet a process standing in `$dir`: between the fork
        # and the `exec`, MSYS2 answers no cwd at all, so asking once read a working
        # platform as one that cannot attribute -- 2 of 20 Git Bash runs skipped the
        # whole real-process half this way, and the gate's self-test then passed over
        # the wrapper defect those runs would have shown. So the answer is waited for,
        # bounded, and the skip reports what was seen when the bound ran out.
        seen=""
        waited=0
        while [ "$waited" -lt 50 ]; do
            seen="$(gate_cwd_of "$probePid" 2>/dev/null || true)"
            [ "$seen" = "$dir" ] && break
            sleep 0.1
            waited=$(( waited + 1 ))
        done
        kill -KILL "$probePid" 2>/dev/null || true
        wait "$probePid" 2>/dev/null || true
        if [ "$seen" = "$dir" ]; then
            rm -rf "$dir"
            return 0
        fi
        _reap_attribution_detail="gate_cwd_of answered '${seen:-<nothing>}' for a process staged in '${dir}', after 5 s of asking"
        rm -rf "$dir"
        return 1
    }

    # A decoy is STAGED when the reaper would see it as one: a candidate by argv
    # and attributable by cwd.
    _reap_staged() {
        local pid="$1" dir="$2" candidate found="no"
        [ "$(gate_cwd_of "$pid" 2>/dev/null || true)" = "$dir" ] || return 1
        while IFS= read -r candidate || [ -n "$candidate" ]; do
            [ "$candidate" = "$pid" ] && found="yes"
        done <<< "$(gate_candidates)"
        [ "$found" = "yes" ]
    }

    if [ "$walkable" = "no" ]; then
        skipped="this host's ancestry is not walkable to its root (on Cygwin/MSYS every chain starts at a native process), so the reaper refuses here and there is no reap to watch"
    elif ! _reap_can_attribute_cwd; then
        skipped="cannot attribute another process's working directory here -- ${_reap_attribution_detail}"
    else
        local scratch outside inside decoy pidOut pidIn reaper_self
        # This script, so the wrapper case below invokes the REAPER rather than a
        # copy of it -- the question is whether the shipped thing spares its caller.
        reaper_self="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"
        scratch="$(mktemp -d)"
        mkdir -p "${scratch}/mine" "${scratch}/other"
        decoy="${scratch}/${ReapGateMarker}"
        printf '#!/usr/bin/env bash\nsleep 30\n' > "$decoy"
        chmod +x "$decoy"

        ( sleep "$stageDelay"; cd "${scratch}/other" && exec bash "$decoy" ) >/dev/null 2>&1 &
        pidOut=$!
        ( sleep "$stageDelay"; cd "${scratch}/mine" && exec bash "$decoy" ) >/dev/null 2>&1 &
        pidIn=$!
        # Both must be up before the reaper looks, or a decoy that has not execed
        # yet carries no marker and the case passes for the wrong reason. "Up" is
        # what the REAPER would see -- the marker in its argv and its cwd readable --
        # never `kill -0`, which is true from the fork onward, before either holds.
        local waited=0
        while [ "$waited" -lt 50 ]; do
            if _reap_staged "$pidOut" "${scratch}/other" && _reap_staged "$pidIn" "${scratch}/mine"; then break; fi
            sleep 0.1
            waited=$(( waited + 1 ))
        done
        _reap_expect "both decoys are staged as the reaper sees them before it runs" \
            "yes" "$(_reap_staged "$pidOut" "${scratch}/other" && _reap_staged "$pidIn" "${scratch}/mine" && echo yes || echo no)"
        # The in-root decoy's own CHILD, its `sleep`, whose cwd is no evidence -- the
        # gate's cmake, ninja and ctest are in its position, and are reached only by
        # ANCESTRY. Staged before the reap, so "gone" afterwards is about the reap.
        local childIn=""
        waited=0
        while [ "$waited" -lt 50 ]; do
            childIn="$(gate_descendants_of "$pidIn" "$(gate_process_table)")"
            childIn="${childIn%%$'\n'*}"
            [ -n "$childIn" ] && break
            sleep 0.1
            waited=$(( waited + 1 ))
        done

        reap_my_gate "${scratch}/mine" >/dev/null 2>&1

        # BY PID, and not by a count of survivors: a count cannot say WHICH one
        # lived, and the whole claim is about a specific neighbour.
        sleep 0.3
        _reap_expect "the decoy in ANOTHER worktree is still alive, by pid" \
            "alive" "$(kill -0 "$pidOut" 2>/dev/null && echo alive || echo gone)"
        _reap_expect "the decoy in THIS worktree is gone, by pid" \
            "gone" "$(kill -0 "$pidIn" 2>/dev/null && echo alive || echo gone)"
        if [ -n "$childIn" ]; then
            _reap_expect "and its child, reached by ancestry, is gone too" \
                "gone" "$(kill -0 "$childIn" 2>/dev/null && echo alive || echo gone)"
            kill -KILL "$childIn" 2>/dev/null || true
        else
            _reap_expect "and its child, reached by ancestry, is gone too" \
                "gone" "never staged: no child of ${pidIn} in the process table after 5 s"
        fi


        # --- THE ASKING PROCESS IS IN THE CANDIDATE SET ----------------------
        #
        # A wrapper carrying the marker in its argv, standing IN the root, which
        # invokes the reaper. By cwd it is this lane's gate and would be killed; it
        # is also the reaper's own parent. It must survive, and the decoy beside it
        # must not -- a reaper that spared everything would pass the first half
        # alone.
        #
        # This is the shape both hazards other lanes measured reduce to.
        # `pgrep -f '[l]ocal-gate.sh'` SELF-MATCHES, because pgrep tests the pattern
        # against its own argv where the bracket is still present; and a probe run
        # from a lane's worktree is attributed BY CWD to that lane, so its own
        # `bash -lc` reads as "this lane's gate, just started" with `etime=00:00`.
        # Neither spelling is consulted here -- `gate_protected_pids` walks `$$` and
        # its ancestors and removes them from the set before any cwd is looked at --
        # and this case is what says so.
        local wrapdir="${scratch}/wrap"
        mkdir -p "$wrapdir"
        local wrapper="${wrapdir}/${ReapGateMarker}"
        printf '#!/usr/bin/env bash\nbash "%s" "%s" >/dev/null 2>&1\nsleep 30\n' \
            "$reaper_self" "${scratch}/mine" > "$wrapper"
        chmod +x "$wrapper"

        local victim wrapperPid
        ( sleep "$stageDelay"; cd "${scratch}/mine" && exec bash "$decoy" ) >/dev/null 2>&1 &
        victim=$!
        # Staged BEFORE the wrapper starts, since the wrapper reaps at once: a victim
        # still between fork and exec carries no marker, survives, and reads as a
        # reaper that failed to kill.
        waited=0
        while [ "$waited" -lt 50 ]; do
            _reap_staged "$victim" "${scratch}/mine" && break
            sleep 0.1
            waited=$(( waited + 1 ))
        done
        _reap_expect "the in-root victim is staged as the reaper sees it before the wrapper runs" \
            "yes" "$(_reap_staged "$victim" "${scratch}/mine" && echo yes || echo no)"
        ( cd "${scratch}/mine" && exec bash "$wrapper" ) >/dev/null 2>&1 &
        wrapperPid=$!

        # Wait for the reaping to have HAPPENED rather than for a duration: the
        # decoy dying is the signal that the wrapper's reaper ran to completion.
        waited=0
        while [ "$waited" -lt 150 ]; do
            kill -0 "$victim" 2>/dev/null || break
            sleep 0.1
            waited=$(( waited + 1 ))
        done

        _reap_expect "a decoy in the root dies when the reaper is invoked from a wrapper" \
            "gone" "$(kill -0 "$victim" 2>/dev/null && echo alive || echo gone)"
        _reap_expect "and the WRAPPER that invoked it -- a candidate by cwd -- survives" \
            "alive" "$(kill -0 "$wrapperPid" 2>/dev/null && echo alive || echo gone)"

        kill -KILL "$wrapperPid" 2>/dev/null || true
        wait "$wrapperPid" 2>/dev/null || true
        wait "$victim" 2>/dev/null || true
        kill -KILL "$pidOut" 2>/dev/null || true
        wait "$pidOut" 2>/dev/null || true
        wait "$pidIn" 2>/dev/null || true
        rm -rf "$scratch"
    fi

    # --- a NATIVE intermediary between the caller and the reaper (review I1) ----
    #
    # The shape that hid the caller: a wrapper carrying the marker, standing in the
    # root, starts the reaper THROUGH cmd.exe. The reaper's MSYS parent is then `1`,
    # and a walk that read that as init listed the wrapper -- its real caller -- as a
    # victim. It must REFUSE instead and name nothing. `--dry-run`, so the case
    # signals nothing whichever way it goes. Only where the runtime has native
    # processes; elsewhere it is said to be not applicable, never skipped silently.
    if [ -e "/proc/$$/winpid" ]; then
        if command -v cmd.exe >/dev/null 2>&1 && command -v cygpath >/dev/null 2>&1; then
            local nroot nwrap nout nwrapPid nwaited=0 nreaper nbash nbatch
            nroot="$(mktemp -d)"
            mkdir -p "${nroot}/mine"
            nreaper="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"
            nbash="$(cygpath -w "$BASH")"
            nout="${nroot}/reaper.out"
            # A batch file, so cmd.exe is handed ONE quoted path: its `/c` strips the outer
            # quotes of a longer quoted line, and the reaper's arguments are then its own.
            nbatch="${nroot}/run.cmd"
            printf '@"%s" "%s" --dry-run "%s" > "%s" 2>&1\r\n' \
                "$nbash" "$nreaper" "${nroot}/mine" "$(cygpath -w "$nout")" > "$nbatch"
            nwrap="${nroot}/${ReapGateMarker}"
            printf '#!/usr/bin/env bash\nMSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*" cmd.exe /c "%s"\nsleep 30\n' \
                "$(cygpath -w "$nbatch")" > "$nwrap"
            ( cd "${nroot}/mine" && exec bash "$nwrap" ) >/dev/null 2>&1 &
            nwrapPid=$!
            # Waits for the reaper's LAST line, whichever verdict it reached.
            while [ "$nwaited" -lt 150 ]; do
                grep -q 'REFUSING to signal anything\|would signal\|no gate of this lane\|signalled' "$nout" 2>/dev/null && break
                sleep 0.1
                nwaited=$(( nwaited + 1 ))
            done
            local nverdict
            nverdict="$(cat "$nout" 2>/dev/null)"
            if [[ "$nverdict" == *"REFUSING to signal anything"* && "$nverdict" != *"would signal"* ]]; then
                if [[ "$nverdict" == *"wsl.exe -e bash scripts/reap-my-gate.sh --dry-run"* ]]; then
                    nverdict="refused"
                else
                    nverdict="refused, but named no next step: ${nverdict}"
                fi
            fi
            _reap_expect "a reaper started through a native process (cmd.exe) refuses, lists nobody, and names the WSL run" \
                "refused" "$nverdict"
            kill -KILL "$nwrapPid" 2>/dev/null || true
            wait "$nwrapPid" 2>/dev/null || true
            rm -rf "$nroot"
        else
            skipped="${skipped:+${skipped}; }the native-intermediary case (no cmd.exe or cygpath here)"
        fi
    else
        echo "reap-my-gate --self-test: the native-intermediary case is not applicable (no Cygwin/MSYS runtime)"
    fi

    echo "reap-my-gate --self-test: ${ran} checks ran, ${failed} failed${skipped:+ -- SKIPPED: $skipped}"
    # 77 for a run that SKIPPED the real-process half, never 0 -- the repairer's rule
    # (#1231). Returning 0 there scored a run that never staged a process as Passed,
    # the SUCCEED-standing-in-for-a-skip shape, and the half it skips is the one that
    # found the Git Bash defect. A failure already recorded outranks the skip.
    [ "$failed" -eq 0 ] || return 1
    [ -z "$skipped" ] || return 77
    return 0
}

# ---------------------------------------------------------------------------
# Entry
# ---------------------------------------------------------------------------

reap_root=""
reap_mode=""
while [ "$#" -gt 0 ]; do
    case "$1" in
        --self-test) reap_self_test; exit $? ;;
        --dry-run)   reap_mode="dry-run"; shift ;;
        -*)          echo "usage: $0 [--dry-run] [<worktree>] | --self-test" >&2; exit 2 ;;
        *)           reap_root="$1"; shift ;;
    esac
done

if [ -z "$reap_root" ]; then
    # This script's own repository, so the ordinary invocation needs no argument
    # and cannot name the wrong tree by typo.
    reap_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fi

reap_my_gate "$reap_root" "$reap_mode"
