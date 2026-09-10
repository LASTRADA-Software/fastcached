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
#   * `pgrep -f` inside `bash -lc` matches ITS OWN command line, and the
#     `[p]attern` bracket trick fails the same way.
#   * two worktrees here are named `SAN-408` and `SAN-REQ-408`, one token apart, so
#     even an exact-looking path fragment matches a neighbour.
#
# So the candidate set is built by reading each process's argv for a LITERAL, with
# no regex engine involved at all -- and then every candidate is ATTRIBUTED by
# something the command line cannot fake.
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
# Usage:
#   scripts/reap-my-gate.sh [<worktree>]   kill the gate rooted at <worktree>
#                                          (default: this script's own repository)
#   scripts/reap-my-gate.sh --dry-run [<worktree>]
#   scripts/reap-my-gate.sh --self-test
#
# Exit: 0 whatever it found. Killing nothing is an ordinary outcome -- no gate was
# running -- and a status that distinguished it would be read as an error.
set -uo pipefail

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
            # this helper only ever kills what it can attribute.
            args="$(tr '\0' ' ' < "${entry}/cmdline" 2>/dev/null)" || continue
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
gate_ppid_of() {
    local pid="$1" out=""
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
gate_protected_pids() {
    local pid="$$" depth=0 parent
    while [ "$depth" -lt 64 ]; do
        printf '%s\n' "$pid"
        [ "$pid" != "1" ] || break
        parent="$(gate_ppid_of "$pid")" || break
        [ -n "$parent" ] && [ "$parent" != "0" ] || break
        pid="$parent"
        depth=$(( depth + 1 ))
    done
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
# @param 1 newline-separated root pids
# @param 2 the process table, as `pid ppid` lines (injected so the self-test can
#          stage a tree without forking one)
gate_descendants_of() {
    local roots="$1" table="$2"
    local depth=0 next found line entry pid parent
    found=""
    while [ "$depth" -lt 32 ] && [ -n "$roots" ]; do
        next=""
        while IFS= read -r entry || [ -n "$entry" ]; do
            entry="${entry# }"
            pid="${entry%% *}"
            parent="${entry##* }"
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

# The process table this run reads, as `pid ppid` lines.
gate_process_table() {
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
    protected="$(gate_protected_pids)"
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
    # WHAT STAYS STAGED, said rather than left to be discovered: `spare-self` is
    # covered by its decision row above and by no live process. Exercising it needs
    # the reaper to be invoked BY something carrying the marker and standing in the
    # root -- a gate calling the reaper from its own cleanup -- and arranging that
    # here means a script named `local-gate.sh` that runs this self-test, which is
    # recursive. The row is the coverage; the live failure mode is loud rather than
    # silent, since a reaper that killed its own ancestors would take the run down
    # and print no verdict at all.
    if [ ! -d /proc ] && ! command -v lsof >/dev/null 2>&1; then
        skipped="no way to read another process's cwd on this platform"
    else
        local scratch outside inside decoy pidOut pidIn
        scratch="$(mktemp -d)"
        mkdir -p "${scratch}/mine" "${scratch}/other"
        decoy="${scratch}/${ReapGateMarker}"
        printf '#!/usr/bin/env bash\nsleep 30\n' > "$decoy"
        chmod +x "$decoy"

        ( cd "${scratch}/other" && exec bash "$decoy" ) >/dev/null 2>&1 &
        pidOut=$!
        ( cd "${scratch}/mine" && exec bash "$decoy" ) >/dev/null 2>&1 &
        pidIn=$!
        # Both must be up before the reaper looks, or a decoy that has not execed
        # yet carries no marker and the case passes for the wrong reason.
        local waited=0
        while [ "$waited" -lt 50 ]; do
            if kill -0 "$pidOut" 2>/dev/null && kill -0 "$pidIn" 2>/dev/null; then break; fi
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

        kill -KILL "$pidOut" 2>/dev/null || true
        wait "$pidOut" 2>/dev/null || true
        wait "$pidIn" 2>/dev/null || true
        rm -rf "$scratch"
    fi

    echo "reap-my-gate --self-test: ${ran} checks ran, ${failed} failed${skipped:+ -- SKIPPED: $skipped}"
    [ "$failed" -eq 0 ]
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
