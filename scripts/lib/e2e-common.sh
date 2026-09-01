# SPDX-License-Identifier: Apache-2.0
#
# The shared helpers every POSIX end-to-end fixture under `scripts/` uses to draw
# a port, wait for a process to come up, ask an HTTP surface a question, and stop
# the run.
#
# SOURCED, never executed. There is no `#!` line and the file is not executable,
# so `./e2e-common.sh` is a shell error rather than a script that appears to do
# nothing.
#
# ---------------------------------------------------------------------------
# Why this is one file rather than one copy per fixture
# ---------------------------------------------------------------------------
#
# It was seven copies, and the duplication had already produced a defect that was
# silent (#449). Three waits in `dist-compile-e2e.sh` matched the bare word
# `registered`, which also matches `0 of 1 toolchain(s) registered` -- the line a
# node logs after every heartbeat round whatever the outcome. Those waits returned
# for a worker the scheduler had TURNED AWAY, and the fixture then proceeded
# against a fleet it believed was formed. Nothing failed; the property under test
# was simply not being tested.
#
# That is the shape: a fixture passing for a reason unrelated to the thing it
# guards. Seven copies of a wait is seven chances to write the next one slightly
# differently, and a wait that returns for the wrong reason is invisible until
# something downstream fails for an unrelated-looking reason.
#
# The copies had diverged already, and the divergences are worth naming because
# each is a fixture that could not report what it saw:
#
#   * `check-compile-cache-daemon-start.sh`'s `wait_for_port` checked NO liveness
#     at all. A daemon that died at `bind()` was reported as `never answered on
#     port N` after the full bound had burned -- which is precisely the
#     slow-machine-versus-wedged-process confusion `.agent/rules/testing.md`
#     requires be distinguishable, since the two are fixed in different places.
#   * `fleet-dashboard-e2e.sh`'s `http_get` learnt that `read` returns non-zero on
#     a final chunk with no trailing newline, so the loop drops it -- and the JSON
#     document that route serves is ONE line with no newline at all, so the whole
#     body vanished. The fix was never carried back to the other copies, where it
#     is latent today only by luck about which endpoints end in a newline.
#   * `check-compile-cache-daemon-start.sh`'s and `compile-cache-e2e.sh`'s
#     `free_port` had no issued-port ledger, so two draws before either is bound
#     can return the same number and the second process dies of `EADDRINUSE` --
#     a collision that reads as an unrelated flake.
#
# This file is the UNION of the correct behaviours, not their intersection. Every
# fixture gets the liveness check, the trailing-chunk fix and the ledger, because
# the alternative -- preserving each caller's current behaviour -- preserves the
# three bugs above under the name of compatibility.
#
# It has its own test (`ctest -R e2e-helpers-selftest`,
# `scripts/check-e2e-helpers.sh`), for the reason `src/tests/ScriptedSocket.hpp`
# does: a shared helper is a shared fake, a fake nobody exercises does not report
# its own bugs, and getting this one wrong breaks every fixture at once. A helper
# library with no test that can go red is this ticket's own failure mode one level
# up.
#
# ---------------------------------------------------------------------------
# bash 3.2
# ---------------------------------------------------------------------------
#
# macOS ships a 2007 `/bin/bash` and these fixtures run on every platform CI
# builds, so nothing here may use `mapfile`/`readarray`, `declare -A`,
# `${var^^}`, `local -n`, `[[ -v ]]`, or a fractional `read -t`. The self-test
# scans this file for those, because "we remembered" is not a check.
#
# `BASHPID` is in that list and is the interesting one; see `fail` below.

# ---------------------------------------------------------------------------
# Caller contract
# ---------------------------------------------------------------------------
#
# A fixture sources this file and then calls `e2e_begin` once, at the top level:
#
#     source "$(dirname "${BASH_SOURCE[0]}")/lib/e2e-common.sh"
#     workdir="$(mktemp -d)"
#     trap cleanup EXIT
#     e2e_begin "cluster E2E" "$workdir"
#
# `e2e_begin` after the EXIT trap, because it installs a TERM trap whose whole
# purpose is to let the EXIT trap run (see `fail`).

# The text every failure message is prefixed with, so a suite log says which
# fixture stopped. Set by `e2e_begin`.
_e2e_label="e2e"

# Where the issued-port ledger lives. The fixture's own workdir, so the cleanup
# it already has takes the ledger away with everything else.
_e2e_workdir=""

# The pid of the shell that sourced this file. See `fail`.
_e2e_top_pid=$$

# A function name to run before a failure is reported, or empty. `cluster-e2e`
# uses it to dump every node's log: a consensus defect that reproduces
# intermittently is diagnosable from the logs or it is not diagnosable at all,
# and cleanup takes them away.
_e2e_on_fail=""

# The default bound for a wait, in seconds. Every wait takes an optional
# per-call override; this is what a fixture that has one budget sets once.
#
# 20 s is the historical default of four of the seven copies. It is deliberately
# NOT a number this file argues for: a bound is an assumption about the machine,
# and the machine differs per fixture. `fleet-dashboard-e2e` sets 240 because the
# node walks a compiler's include tree before it binds, and that is minutes on a
# cold runner. A fixture that has measured its own cost should say so where the
# measurement is, which is in the fixture.
_e2e_wait_seconds=20

# Begin a run. Call once, at the top level, after the EXIT trap is installed.
#
# @param 1 label for failure messages, e.g. "cluster E2E"
# @param 2 a writable directory that lives for the run; the port ledger goes here
e2e_begin() {
    _e2e_label="$1"
    _e2e_workdir="$2"
    _e2e_top_pid=$$

    # A `fail` raised inside a subshell reaches this shell as SIGTERM, and the
    # default disposition for SIGTERM terminates the shell WITHOUT running the
    # EXIT trap -- which is the trap that kills the daemons and removes the
    # workdir. So the signal is turned back into an ordinary exit here: one
    # failing status whichever shell raised it, and cleanup still runs.
    trap 'exit 1' TERM
}

# Name a function to run immediately before a failure is printed.
# @param 1 function name, or "" to clear
e2e_on_fail() { _e2e_on_fail="$1"; }

# Set the default bound, in seconds, for waits that do not name their own.
# @param 1 seconds
e2e_wait_seconds() { _e2e_wait_seconds="$1"; }

# Print a note, indented, on stdout.
e2e_note() { echo "   $*"; }

# ---------------------------------------------------------------------------

# Stop the run, wherever it is raised.
#
# `exit` inside `( ... )` ends the subshell only, and a fixture that carries on
# then reports a SECOND failure about the artefacts the first one explains -- so
# a reader working upward from the last line starts on the wrong question. That
# has happened here (`launcher-replay-e2e`, its first run anywhere).
#
# The obvious guard is to compare `BASHPID` with the top-level pid and signal the
# parent only when they differ. `$$` cannot do it -- bash keeps `$$` at the
# parent's value inside a subshell, so that comparison always holds and the guard
# silently does nothing. But `BASHPID` was introduced in **bash 4.0**, and macOS
# ships 3.2, where it is unset: `[ "${BASHPID:-$$}" = "$top_pid" ]` is then
# `[ "$$" = "$top_pid" ]`, which always holds -- the same guard silently doing
# nothing, one level down, on the one platform the fixtures cannot be run on
# locally from here.
#
# So there is no detection. The signal is sent UNCONDITIONALLY and the local
# `exit 1` follows it. At the top level that is a self-signal caught by the TERM
# trap `e2e_begin` installed, which exits 1 and runs the EXIT trap; in a subshell
# the parent takes it and does the same while the subshell exits on its own. Both
# paths end at status 1 with cleanup run, and neither asks which shell it is in.
# Exercised in all three contexts -- top level, `( ... )`, and `$( ... )` -- by
# the self-test.
fail() {
    if [ -n "$_e2e_on_fail" ]; then
        "$_e2e_on_fail" || true
    fi
    echo "${_e2e_label} FAILED: $*" >&2
    kill -TERM "$_e2e_top_pid" 2>/dev/null || true
    exit 1
}

# ---------------------------------------------------------------------------

# Is anything answering on host:port right now? Returns 0 or 1; never fails the
# run.
#
# The ONE place that knows how to ask. `free_port` draws through it and
# `wait_for_port` polls through it, so there is a single definition of what "a
# port is open" means here rather than the three near-identical `(exec 3<>...)`
# spellings this file replaced -- and a fixture that wants the answer as a bool,
# because a closed port is an expected outcome there rather than a fault, calls
# the same thing the waits do.
#
# A connect probe, not a bind probe: bind-then-close leaves the port in TIME_WAIT
# on some systems, and a caller drawing a port is about to hand it to a
# *different* process anyway, so the only question this can honestly answer is
# "is anything answering here right now".
#
# @param 1 host
# @param 2 port
port_answers() {
    (exec 3<>"/dev/tcp/${1}/${2}") 2>/dev/null
}

# Find a port nothing is listening on, and remember it.
#
# Racy in principle, since `port_answers` above cannot reserve anything; these
# tests are RUN_SERIAL and the range is wide, and the alternative -- fixed ports
# -- races with every other smoke test in the suite rather than only with itself.
#
# Ports already handed out THIS RUN are remembered and skipped. Without that, the
# only question asked is "is anything listening", and nothing is listening on a
# port issued a moment ago whose server has not bound yet -- so two calls could
# return the same number and the second process to start died with
# `bind(...) failed: 98`. A fixture that draws every port it needs before binding
# any of them is exactly the window that makes it reachable: rare enough to read
# as an unrelated flake, and it did.
#
# The ledger is a FILE rather than a variable because every call site is a command
# substitution, and a subshell's assignment is gone the moment it exits -- which
# is how a first attempt at this fixed nothing at all.
#
# The range stops BELOW the kernel's ephemeral port range, which is the half a
# connect probe cannot cover. A port can be the local endpoint of an OUTBOUND
# connection -- ESTABLISHED or TIME_WAIT -- with nothing listening on it, so the
# probe says "free" and the `bind()` that follows still fails with EADDRINUSE.
# The ledger does not help either: that port was never issued by this fixture.
#
# CI caught exactly that: `bind(127.0.0.1:33174) failed: 98` for a daemon started
# after five cases' worth of launcher and probe connections had consumed ephemeral
# ports. 33174 is inside Linux's default `ip_local_port_range` of 32768-60999,
# which the old draw of 20000-39999 overlapped by its top 7232 numbers -- worse
# than one draw in three.
#
# 20000-31999 is below that floor and below macOS's 49152, and 12000 numbers is
# ample for a fixture that draws a dozen. A machine that lowered the sysctl below
# 32000 would need this to move with it, and
# `cat /proc/sys/net/ipv4/ip_local_port_range` is the check.
#
# @return echoes the port
free_port() {
    local port ledger="${_e2e_workdir}/.issued-ports"
    local floor=20000 ceiling=32000
    for _ in $(seq 1 200); do
        port=$(( floor + RANDOM % (ceiling - floor) ))
        if grep -qx "$port" "$ledger" 2>/dev/null; then
            continue
        fi
        if ! port_answers 127.0.0.1 "$port"; then
            echo "$port" >> "$ledger"
            echo "$port"
            return 0
        fi
    done
    fail "could not find a free port in ${floor}-${ceiling}"
}

# ---------------------------------------------------------------------------
# Bounded waits
# ---------------------------------------------------------------------------
#
# Every wait here is bounded, names what it waited for, records what it COST on
# success, and on expiry says which KIND of failure it was. Those are four
# separate obligations from `.agent/rules/testing.md` and the last two are the
# ones that get dropped:
#
#   * The cost, because nothing recorded it, so no budget in any fixture could be
#     set from data -- every number was a guess that survived by being generous.
#     A green run now measures itself.
#   * The kind, because a wait reporting only `waited 20s for the worker` cannot
#     tell a loaded machine from a wedged process, and those are fixed in
#     completely different places. Reading such a failure nobody can responsibly
#     choose between raising the budget and opening a defect, so the budget gets
#     raised -- that being the action that makes the red go away.
#
# A process that DIED is a third case and is reported the moment it is noticed
# rather than after the budget, because a timeout is not what happened.
#
# What is deliberately NOT here is a CPU-consumption signal. `node-scratch-\
# isolation-e2e` needs one because the step it waits on -- walking a compiler's
# include tree -- logs NOTHING while it runs, so a slow walk and a wedge produce
# identical logs and log growth alone diagnoses that case confidently and wrongly.
# The waits here are for a socket to bind and for named lines to appear, where log
# growth can be false in the failing case and is therefore evidence. Adding a CPU
# reading that cannot be calibrated portably -- `ps -o time=` is whole seconds on
# Linux, so a cold I/O-bound step reads as zero and would be called BLOCKED -- is
# how a signal that cannot be false in the failing case gets printed beside a
# conclusion and lends it authority it has not earned. `_e2e_verdict` is pure and
# takes a record, so a reading that CAN be calibrated is one argument away.

# How long a poll pauses before trying again. A PACING device, not the unit the
# bound is measured in -- see `wait_until`.
_e2e_poll_pause=0.2

# Render the verdict for an expired or aborted wait. PURE: it reads no clock,
# touches no process and opens no file. Everything it says comes from its
# arguments, which is what makes it testable at all -- the acquisition around it
# needs a real process in a real state, and every branch here needs only a record.
#
# Separated for the reason `.agent/rules/testing.md` gives for `Get-WaitVerdict`:
# a decision worth several named outcomes is worth separating from the ambient
# facts it reads, and branches that cannot be staged become one line each.
#
# @param 1 what was being waited for
# @param 2 the bound, in seconds
# @param 3 MEASURED elapsed seconds
# @param 4 how many times the loop polled
# @param 5 alive: yes | no | unknown
# @param 6 exit status if known, else "-"
# @param 7 log grew during the wait: yes | no | unknown
# @param 8 measured seconds since the log last grew, or "-"
_e2e_verdict() {
    local what="$1" bound="$2" elapsed="$3" polls="$4" alive="$5" status="$6" grew="$7" stall="$8"

    echo "waited ${elapsed}s (measured, over ${polls} polls) of a ${bound}s budget for ${what}"
    echo "  evidence: alive=${alive} exit=${status} logGrew=${grew} sinceGrowth=${stall}"

    # A loop that took materially longer than it was told to is itself a reading,
    # and one that used to be invisible: a bound counted in iterations reports the
    # duration it INTENDED whatever the machine did, so a runner too loaded to
    # poll at the assumed rate looked exactly like one that was not. Named here
    # rather than folded into the findings below, because it is a fact about the
    # measurement and each of those is a fact about the subject.
    local slack=$(( bound / 10 ))
    [ "$slack" -ge 1 ] || slack=1
    if [ "$elapsed" -gt $(( bound + slack )) ]; then
        echo "  NOTE: the loop overran its own budget by $(( elapsed - bound ))s, so this machine could"
        echo "        not poll at the rate the wait assumed. Read the findings below with that in mind."
    fi

    if [ "$alive" = "no" ]; then
        echo "  FINDING: the process DIED. This is not a timeout and the budget is not"
        echo "           the subject; its exit status is ${status} and its log is below."
        return 0
    fi

    if [ "$alive" = "unknown" ]; then
        echo "  FINDING: INCONCLUSIVE. No process was watched, so this cannot say whether"
        echo "           the thing being waited for died, stalled or was merely slow."
        return 0
    fi

    if [ "$grew" = "unknown" ]; then
        echo "  FINDING: INCONCLUSIVE. The process is alive, and no log was watched, so"
        echo "           there is nothing here that separates slow from stuck."
        return 0
    fi

    if [ "$grew" = "no" ]; then
        echo "  FINDING: the process is ALIVE and logged NOTHING for the whole ${bound}s."
        echo "           It reached the point of being started and no further."
        return 0
    fi

    # It grew. Whether it was still growing AT THE DEADLINE is the question, and a
    # total cannot answer it -- growth spread over the whole wait and growth that
    # stopped in the first second are the same `logGrew=yes` and opposite
    # findings. So the reading that decides is the stall age, and the threshold is
    # named rather than implied.
    local recent=$(( bound / 4 ))
    [ "$recent" -ge 1 ] || recent=1
    if [ "$stall" != "-" ] && [ "$stall" -le "$recent" ]; then
        echo "  FINDING: the process is ALIVE and its log grew within the last ${recent}s."
        echo "           It was still making progress when the budget ran out."
        return 0
    fi

    echo "  FINDING: the process is ALIVE, it logged during the wait, and it has logged"
    echo "           nothing for the last ${stall}s of a ${bound}s budget. It started and"
    echo "           then stopped making observable progress."
}

# The general bounded wait, and the body both `wait_for_port` and `wait_for_log`
# are. One loop, because those two differ only in what they test each pass, and
# two loops is two chances for the liveness check, the cost accounting and the
# verdict to drift apart -- which is how six copies of `wait_for_port` came to
# have three different failure semantics.
#
# PUBLIC, because a fixture waiting on something neither of those covers is the
# case that hand-rolls a loop and reintroduces every defect this file exists to
# remove. `fleet-dashboard-e2e` had exactly that -- a poll for a worker to appear
# in `/fleet.json`, with its own bound, its own message and no liveness check --
# so a bespoke CONDITION is a predicate passed to this, never a bespoke loop.
#
# THE BOUND COMES FROM A CLOCK, NOT FROM AN ITERATION COUNT. Every copy this
# replaces counted iterations and called the product a duration -- "100 x 0.2s =
# 20s", "2400 x 0.1s = 240s" -- and none of those equalities holds. `sleep` is an
# external command, so each pass forks and execs it, and `sleep 0.2` itself
# overshoots on a busy machine: the product is a LOWER bound that drifts most
# exactly when a fixture is timing out, which is when somebody is reading it.
#
# `fleet-dashboard-e2e` had the defect in its purest form:
#
#     fail "timed out after $((WAIT_TICKS / 10))s waiting for $what ..."
#
# a duration computed from the loop shape and never observed. Whatever the loop
# actually took, the operator was told `WAIT_TICKS / 10`. And `testing.md`
# requires that message to separate a slow machine from a wedged process -- which
# the one reading that would show a slow machine cannot do when it is derived
# from the assumption that the machine was fast.
#
# So `SECONDS` bounds the loop and `SECONDS` is what gets reported. It is a bash
# builtin, so it costs no fork, and it is in bash 3.2. Its resolution is one
# second, which is the price: it is ample for budgets of 20s and 240s, and it is
# a MEASURED second rather than an assumed one. `sleep` stays, as pacing.
#
# A PID PASSED HERE MUST BE ONE THIS USER CAN SIGNAL. Liveness is `kill -0`, and
# `kill -0` answers non-zero for a process that is alive but owned by somebody
# else -- EPERM and ESRCH are the same reading from a shell. This loop treats
# non-zero as DIED, so a pid running under a service account would be reported
# as having died, on the first poll, with an exit status of `-` and its log
# dumped: a confident wrong answer of exactly the kind the verdict exists to
# avoid, and one no bound or budget would soften.
#
# Measured rather than reasoned: `kill -0 1` as an ordinary user answers
# `Operation not permitted` and returns non-zero, and pid 1 is not dead.
#
# So the rule is "a pid this shell may signal, or `-`", and `-` is not a lesser
# answer -- the verdict reports it as its own outcome, which is honest, where a
# borrowed pid is simply wrong. `macos-package-e2e` is the live case: its
# LaunchDaemon runs as `_fastcached`, so it passes `-`, while
# `macos-service-e2e`'s agent runs as the test user and is `kill -0`-ed by the
# fixture itself three lines earlier, so it passes the pid.
#
# @param 1 a function name; returns 0 when the wait is over
# @param 2 what is being waited for, for the messages
# @param 3 pid to watch, or "-" for none. Must be signallable by this user; see
#          above -- a pid owned by another account reads as DEAD.
# @param 4 log to watch and dump, or "-" for none
# @param 5 bound in seconds
wait_until() {
    local ready="$1" what="$2" pid="$3" logfile="$4" seconds="$5"
    local started="$SECONDS" grewAt="$SECONDS"
    local elapsed=0 polls=0 size=0 baseline=0 grew="no" stall=0 alive="yes" status="-"

    if [ "$logfile" = "-" ]; then
        grew="unknown"
    else
        baseline="$(_e2e_size "$logfile")"
        size="$baseline"
    fi
    if [ "$pid" = "-" ]; then alive="unknown"; fi

    while [ "$elapsed" -lt "$seconds" ]; do
        polls=$(( polls + 1 ))
        if "$ready"; then
            e2e_note "waited ${elapsed}s (${polls} polls) for ${what}"
            return 0
        fi

        # A death is reported when it is NOTICED, not when the budget expires: a
        # process that is gone will not come back, and calling that a timeout
        # sends the reader to the budget.
        if [ "$pid" != "-" ] && ! kill -0 "$pid" 2>/dev/null; then
            alive="no"
            # `wait` answers with the child's status, and only for a child of THIS
            # shell that has not been reaped; for anything else it answers 127,
            # which is "I am not able to say" rather than a status the process
            # had. A launchd job and an already-reaped pid both land there, and
            # the verdict prints `-` rather than inventing an exit code.
            #
            # Not in a command substitution: that is a subshell, and a subshell
            # cannot `wait` for its parent's children -- it would answer 127 for
            # every process, including the ones whose status is right there.
            local rc=0
            wait "$pid" 2>/dev/null || rc=$?
            if [ "$rc" -ne 127 ]; then status="$rc"; fi
            if [ "$grew" = "no" ]; then stall="-"; fi
            _e2e_expire "$what" "$seconds" "$elapsed" "$polls" \
                "$alive" "$status" "$grew" "$stall" "$logfile"
        fi

        if [ "$logfile" != "-" ]; then
            local now
            now="$(_e2e_size "$logfile")"
            if [ "$now" -gt "$size" ]; then
                size="$now"
                grewAt="$SECONDS"
                if [ "$now" -gt "$baseline" ]; then grew="yes"; fi
            fi
        fi

        sleep "$_e2e_poll_pause"
        elapsed=$(( SECONDS - started ))
        stall=$(( SECONDS - grewAt ))
    done

    if [ "$grew" = "no" ]; then stall="-"; fi
    _e2e_expire "$what" "$seconds" "$elapsed" "$polls" \
        "$alive" "$status" "$grew" "$stall" "$logfile"
}

# Size of a file in bytes, or 0 when it does not exist yet. `wc -c` rather than
# `stat`, whose flags differ between GNU and BSD.
_e2e_size() {
    if [ -r "$1" ]; then
        wc -c < "$1" 2>/dev/null | tr -d ' '
    else
        echo 0
    fi
}

# Print the verdict, dump the log, and stop the run. Never returns.
_e2e_expire() {
    local what="$1" seconds="$2" elapsed="$3" polls="$4"
    local alive="$5" status="$6" grew="$7" stall="$8" logfile="$9"
    _e2e_verdict "$what" "$seconds" "$elapsed" "$polls" "$alive" "$status" "$grew" "$stall" >&2
    if [ "$logfile" != "-" ] && [ -r "$logfile" ]; then
        { echo "--- ${logfile}"; cat "$logfile"; } >&2
    fi
    fail "gave up waiting for ${what}"
}

# Block until something answers on host:port, or the process behind it dies.
#
# Waiting on the listener rather than sleeping a fixed amount: a cold CI runner
# takes noticeably longer to get a process from spawned to listening than a warm
# developer machine, and a fixed sleep is either flaky or slow.
#
# The host is a parameter rather than `127.0.0.1`, because a fixture that binds a
# worker on this machine's own non-loopback address has to probe the address it
# bound -- and a probe hard-coded to loopback would answer about a different
# socket, or about nothing.
#
# @param 1 host
# @param 2 port
# @param 3 pid to watch, or "-" if there is no pid this user may signal -- a
#          launchd job under a service account, say. "-" is an honest reading and
#          the verdict says so; a pid that is available, signallable and not
#          passed is a diagnosis thrown away, and a pid that is NOT signallable
#          is worse than none. See `wait_until` above.
# @param 4 what it is, for the messages
# @param 5 log to dump on expiry, or "-"
# @param 6 optional bound in seconds; defaults to `e2e_wait_seconds`
wait_for_port() {
    local host="$1" port="$2" pid="$3" what="$4" logfile="$5"
    local seconds="${6:-$_e2e_wait_seconds}"
    _e2e_port_ready() { port_answers "$host" "$port"; }
    wait_until _e2e_port_ready "${what} to listen on ${host}:${port}" \
        "$pid" "$logfile" "$seconds"
}

# Wait for a line to appear in a log, the way `wait_for_port` waits for a listener.
#
# A bound port does NOT mean a process has finished announcing itself. A tier
# binds its listener and logs what it bound *afterwards*, because the message
# names the endpoint and the endpoint is not known until the bind returns. So a
# `grep` run straight after `wait_for_port` is a race -- and one that widens
# exactly where it is least welcome: under a sanitizer, or with the log on a slow
# filesystem, the gap between the two stops being instant.
#
# WHAT THE MARKER MUST BE. A wait waits on what a line MEANS, not on its wording,
# and the two come apart without anything being renamed. `compile node ready` meant
# *surveyed* until #365 made a node bind first and survey afterwards; the line was
# not renamed, not moved and not reworded, it simply stopped carrying the fact a
# fixture was reading out of it, and three include-tree walks that had been
# serialised began running at once. And a marker can be too LOOSE from the start:
# the bare word `registered` also matches `0 of 1 toolchain(s) registered`, which
# is logged after every heartbeat round whatever the outcome, so three waits
# returned for a worker that had been turned away (#449). Match the fact, not the
# sentence that currently coincides with it.
#
# @param 1 the text to wait for (a `grep` basic regular expression)
# @param 2 pid to watch, or "-"
# @param 3 what it is, for the messages
# @param 4 the log to watch
# @param 5 optional bound in seconds; defaults to `e2e_wait_seconds`
wait_for_log() {
    local marker="$1" pid="$2" what="$3" logfile="$4"
    local seconds="${5:-$_e2e_wait_seconds}"
    _e2e_log_ready() { grep -q "$marker" "$logfile" 2>/dev/null; }
    wait_until _e2e_log_ready "${what} to log: ${marker}" \
        "$pid" "$logfile" "$seconds"
}

# Stop a process and require it to actually exit, within a bound.
#
# `kill` then a bare `wait` is the obvious spelling and it HANGS when the signal
# is handled but the process never finishes stopping -- which is a real failure
# mode and was a real bug: a worker installs a SIGTERM handler, and if its accept
# loop cannot be woken the handler sets a flag nobody comes back to read. A test
# that hangs reports less than a test that fails.
#
# @param 1 pid
# @param 2 what it is, for the message
# @param 3 seconds to allow
stop_and_require_exit() {
    local pid="$1" what="$2" seconds="$3"
    kill "$pid" >/dev/null 2>&1 || true
    # Clock-bounded for the reason `wait_until` is: this bound is an
    # assertion about how promptly a process stops, so a loop that silently ran
    # longer than it claimed would let a wedged one through.
    local started="$SECONDS" elapsed=0
    while [ "$elapsed" -lt "$seconds" ]; do
        kill -0 "$pid" 2>/dev/null || { wait "$pid" 2>/dev/null || true; return 0; }
        sleep "$_e2e_poll_pause"
        elapsed=$(( SECONDS - started ))
    done
    kill -9 "$pid" >/dev/null 2>&1 || true
    wait "$pid" 2>/dev/null || true
    fail "${what} was still running ${elapsed}s (measured) after being asked to stop, against a ${seconds}s bound"
}

# ---------------------------------------------------------------------------

# GET one path and echo the whole response, headers included.
#
# `/dev/tcp` rather than curl, because a fixture that skips when curl is absent
# tests nothing on the machine that lacks it, and this needs no more than one
# request. Every read is bounded with `read -t`: the endpoint closes the
# connection itself (`Connection: close`), so a healthy server ends the loop on
# its own -- and a WEDGED one, which is exactly the state this probe exists to
# detect, would otherwise hang the suite instead of failing it.
#
# `read -t 5` and not a fractional timeout, because bash 3.2 rejects one.
#
# THE LAST CHUNK. `read` sets its variable and returns non-zero on a final chunk
# with no trailing newline, so a naive loop drops it. That is not a corner case
# here: the fleet dashboard's JSON document is ONE line with no newline at all, so
# without the line below the whole body vanishes and every assertion about it
# fails for a reason that has nothing to do with the server. One of the seven
# copies of this function learnt that; the other six never did.
#
# @param 1 host
# @param 2 port
# @param 3 path
# @param 4.. extra request header lines, without CRLF, e.g. "Authorization: x"
# @return echoes the response; returns 1 if the connection was refused
http_get() {
    local host="$1" port="$2" path="$3"
    shift 3
    local line="" body="" header=""
    exec 3<>"/dev/tcp/${host}/${port}" || return 1
    {
        printf 'GET %s HTTP/1.1\r\nHost: %s\r\n' "$path" "$host"
        for header in ${@+"$@"}; do
            printf '%s\r\n' "$header"
        done
        printf 'Connection: close\r\n\r\n'
    } >&3
    while IFS= read -r -t 5 line <&3; do body+="${line}"$'\n'; done
    if [ -n "$line" ]; then body+="$line"; fi
    exec 3<&-
    printf '%s' "$body"
}
