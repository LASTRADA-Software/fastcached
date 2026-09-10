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

# ---------------------------------------------------------------------------
# A deadline no clock can move, and the millisecond arithmetic that reports on it
# ---------------------------------------------------------------------------
#
# #1066. Every bound in this file used to be `SECONDS`, and `SECONDS` is
# `CLOCK_REALTIME`. On a host whose wall clock steps -- this project's own
# development host does, measured -- a bound read from it is not a duration:
#
#   * a BACKWARD step LENGTHENS the bound. A wedged process is waited on past its
#     ceiling and the fixture reports a slow pass instead of a hang. This is the
#     direction observed here, and its SIZE varies: ~1.28 s, then ~1.43-1.45 s,
#     then ~1.57-1.68 s, across three sessions and three different instruments.
#     There is not even a constant to widen against.
#   * a FORWARD step SHORTENS it. A healthy client is killed early and the fixture
#     reports a failure that did not happen -- a red on a good tree.
#
# Neither is distinguishable from the thing the bound exists to detect, which is
# what makes this different from a bound that is merely too tight.
#
# ## The primitive, and why it is a FILE rather than `kill -0` on the timer
#
# `sleep` is `nanosleep`: it counts down a RELATIVE interval, so a wall-clock step
# cannot move it. Measured, on the run that motivated this: 20 x `sleep 0.2` took
# 4.03 s of monotonic time in a round whose realtime read 2.76 s. That makes a
# background `sleep` a sound deadline whatever the clock does.
#
# The obvious way to READ it is `kill -0 "$timerPid"`, which #1066 itself suggests.
# It is rejected, and on the half that ticket did not argue. `sleep` being
# step-proof is a property of `nanosleep` and is measured; whether `kill -0` still
# answers for a FINISHED timer is a property of whether this bash has REAPED it,
# and an unreaped child is a zombie, which answers `kill -0` with SUCCESS. That
# would be a deadline that never expires -- an unbounded wait inside the thing that
# exists to bound one, which is strictly worse than the defect being fixed.
#
# Measured on bash 5.2.21: reaped in 5 of 5, and again with no intervening `wait`.
# (An earlier draft of this said 5.3.9, which is the version this file's own older
# comments claimed for this host and which nothing on it reports: WSL's bash is
# 5.2.21 and Git Bash's is 5.2.37. A wrong version attached to a real measurement
# is the same defect as a figure with no conditions, and it propagated by being
# copied rather than asked.)
# NOT measured on bash 3.2, which macOS ships as `/bin/bash` and which
# `FASTCACHED_BASH` resolves to on every non-Windows platform -- so the one
# interpreter this file is most careful about is the one the reading cannot be
# checked on. A marker FILE has no such question on any bash: the timer creates it
# or it does not, and `[ -e ]` is the whole reading.
#
# This is also the primitive #1048 shipped in this same file, in `_http_drain_fd3`
# -- 754 lines further down, which is a reason to say so here rather than to expect
# anyone to notice. One mechanism, one argument, one place to be wrong.
#
# ## It is NOT the watchdog `run_bounded` already rejected
#
# `_e2e_bounded_pauses`' header rejects `( sleep n; kill $pid ) &` because a pid may
# be REUSED after `wait` reaps it, so a watchdog that fires later can signal a
# stranger. That objection is about a timer that KILLS. This one touches a file and
# signals nothing; the loop that owns the pid does the killing, exactly as before.
# Stated here because the paragraph reads like a rejection of this design and is
# three hundred lines away from it.
#
# COST, and the figure is a PAIRED one because the unpaired ones were load. The
# same 20 commands read 285 ms, 830 ms and 1352 ms across a single session while
# nothing about them changed. Run against the unconverted version with the order
# alternated per round, and timed from `/proc/uptime` -- the guard's own meter is
# bash's `time`, which reads the steppable clock and produced a 0 ms reading in
# the same experiment -- the pairwise differences are within 80 ms and change
# sign, against a load-driven spread of 500-2100 ms for identical work. So: not
# measurably different, rather than faster or slower.

# Milliseconds for a `sleep` argument, without a fork and without floating point.
#
# Sets `_e2e_ms` rather than echoing it: this is called once per poll, and a
# command substitution is a fork -- which is the very cost `_e2e_bounded_pauses`
# exists to avoid, so paying it to MEASURE that cost would be self-defeating.
#
# `10#` forces base ten. Without it `0.05` yields the fraction `050`, which bash
# reads as OCTAL, and `0.08` would then be a syntax error rather than 80 -- a
# ramp value away from breaking every bound in this file.
#
# Anything below a millisecond TRUNCATES to zero, which is a floor rather than a
# rounding error: the figure this feeds is compared against half a bound to say
# whether the HOST was slow, so under-counting biases that reading toward "the
# host was slow", which is the direction that sends a reader to look at the
# machine rather than at their own code. Every pause here is 10 ms or more.
#
# @param 1 a `sleep` argument, e.g. `0.2`, `0.01`, `3`
_e2e_ms=0
_e2e_ms_of() {
    local v="$1" int frac
    int="${v%%.*}"
    if [ "$v" = "$int" ]; then frac=""; else frac="${v#*.}"; fi
    frac="${frac}000"
    frac="${frac:0:3}"
    _e2e_ms=$(( ${int:-0} * 1000 + 10#${frac} ))
}

# Arm a deadline.
#
# `mktemp` CREATES the marker and the arm removes it, so the name is unique by the
# OPERATING SYSTEM and the file's existence afterwards is this timer's statement and
# nothing else's. That matters because every other way of spelling a unique name here
# fails on the one interpreter this file is most careful about.
#
# `$$` is NOT unique: bash keeps it at the PARENT's value inside `( ... )`, which is
# the same fact that makes `BASHPID` unusable on bash 3.2, and this file's callers run
# `run_bounded` inside command substitutions -- `cluster-e2e`'s probe loop does it 261
# times in one wait. A sequence number does not rescue it, because a subshell inherits
# the counter and advances its own copy: measured, two command substitutions calling
# one `draw` function both answered `1695426.1`. `$RANDOM` does not rescue it either,
# and that one is a VERSION-DEPENDENT trap -- bash 5.0+ reseeds it in a subshell and
# 3.2 does not, so three separate `$( )` gave `15789`, `1266`, `28734` here and would
# give one value three times on macOS.
#
# AN EARLIER VERSION BUILT THE NAME BY PARAMETER EXPANSION TO AVOID THE FORK, and the
# measurement it cited for that was wrong. It claimed `mktemp` cost 830 ms across the
# 20 calls `run_bounded`'s fast-path guard times. Paired against the fork-free form,
# order-alternated and timed from `/proc/uptime` rather than from the guard's own
# steppable meter, the real difference is **about 50 ms across 20 calls** -- 2.5 ms
# each, against a ceiling of 2000 ms that the guard currently meets at about 390 ms.
# The 830 ms was unpaired, taken at a different load, and taken while a separate
# defect was still in the loop. A robustness property was dropped on a number that
# could not support the decision, and the number felt sufficient because it was large.
#
# What the fork-free form left behind was a safety property that lived in its CALLERS
# rather than in this function: a colliding name is harmless only while every caller
# retires its timer with an uncatchable signal before arming the next, so no stale
# timer is ever alive to write a marker a later wait is watching. That held, and it is
# not a property anything checks. The failure it would eventually produce is a false
# POSITIVE -- a bound that appears to fire correctly while reporting a previous
# probe's answer -- which is the one direction nothing else in this file guards.
#
# A FAILED arm is refused rather than returned, for #709's reason one level up:
# absence of the marker is read as "the deadline has not passed", so an unusable
# workdir -- removed, full, permissions changed -- would present as a bound that
# never expires. The state that cannot happen must not look like the state that has
# not happened yet.
#
# The background job's stdout and stderr are redirected. It inherits this shell's
# otherwise, and `run_bounded`'s stdout is the bounded command's output -- so a
# timer that outlives a disarm would hold that pipe open and a reader would wait
# for EOF it is not going to get until the deadline expires. The first version of
# this WAS wrapped in a command substitution, where the same redirect was load
# bearing for a sharper reason: `$( )` reads until every holder of the pipe closes
# it, so a 240 s deadline took 240 s to arm.
#
# It SETS `_e2e_deadline_armed` rather than echoing, because a command substitution
# is a fork and this is armed once per `run_bounded` call: measured, echoing cost
# about 200 ms across the 20 calls that function's own fast-path guard times, on a
# 2000 ms ceiling another ticket is defending. The global is copied into a local by
# the caller on the very next line, and a caller that arms twice without copying
# gets the second answer twice -- which is why every call site here is arm-then-copy
# and nothing holds this variable across a call.
#
# @param 1 seconds
# @return sets `_e2e_deadline_armed` to "<pid> <marker>"; the caller holds both
_e2e_deadline_armed=""
_e2e_deadline_arm() {
    local seconds="$1" marker=""
    marker="$(mktemp "${_e2e_workdir}/.deadline.XXXXXX" 2>/dev/null)" || marker=""
    [ -n "$marker" ] || fail "cannot arm a ${seconds}s deadline under '${_e2e_workdir}'"
    rm -f "$marker"
    # A SUBSHELL FORKED HERE INHERITS THIS SHELL'S TRAPS, AND A FIXTURE'S EXIT TRAP
    # IS ITS CLEANUP. So a timer that dies runs that cleanup and `rm -rf`s the run's
    # workdir out from under the run. Observed as `node-ready-waits-for-marker`
    # failing about a third of the time with `cannot arm a 15s deadline:
    # '/tmp/tmp.XXXX' is not a directory` -- the SECOND arm refusing because the first
    # arm's timer had deleted the directory.
    #
    # The trap's own stack is what identified it: `cleanup _e2e_deadline_arm
    # wait_until wait_for_port run_case main`, with a `BASHPID` that was not `$$`.
    #
    # TWO GUARDS, FOR TWO WINDOWS, and only the second one is load bearing:
    #
    #   * `trap -` covers a subshell that has STARTED. It is first, so nothing else
    #     can run before it.
    #   * `kill -KILL` in `_e2e_deadline_disarm` covers the window BEFORE that, which
    #     is the one that fires: the disarm follows the arm within microseconds on a
    #     wait that returns at once, so the subshell is usually signalled before it
    #     has run a single command, and an inherited `trap 'exit 1' TERM` then takes
    #     it straight to the inherited EXIT trap. `trap -` cannot help there because
    #     it has not run yet.
    #
    # Measured rather than argued, and the argument came second: `trap -` ALONE was
    # A/B'd against the unfixed version, interleaved and order-alternated at load 47,
    # and came back 10 pass / 2 fail against 9 pass / 3 fail -- indistinguishable, so
    # the tempting fix does nothing on its own. The same harness at load 43 gives
    # 12 pass / 0 fail with the uncatchable signal against 7 pass / 5 fail without it.
    #
    # An earlier standalone probe of `( ... ) &` under a `trap cleanup EXIT` failed to
    # reproduce any of this -- twice, the first time because the subshell's output was
    # redirected to /dev/null, which is where its evidence went. A fake more permissive
    # than the thing it stands for; the real fixture found in one run what two probes
    # could not.
    # `3>&-` for the reason `_http_drain_fd3` gives at length: a forked timer inherits
    # the caller's descriptors, and an orphaned `sleep` then holds whatever socket the
    # caller had open for the rest of its interval. No caller of THIS arm holds one
    # today -- `wait_until` and `run_bounded` open no fd 3 -- so this is a guard
    # against the next caller rather than a fix for a live leak, and it is here
    # because the two timers must not differ in a property one of them needed.
    ( trap - EXIT TERM INT HUP; sleep "$seconds"; : > "$marker" ) >/dev/null 2>&1 3>&- &
    _e2e_deadline_armed="$! $marker"
}

# Has an armed deadline passed?
# @param 1 the marker path from `_e2e_deadline_arm`
_e2e_deadline_passed() { [ -e "$1" ]; }

# Retire a deadline, fired or not. Safe to call twice.
#
# `rm` is an exec, so it is spent only when there is something to remove -- which
# on the fast path there never is, the timer not having fired.
#
# @param 1 the timer pid
# @param 2 the marker path
# UNCATCHABLE, and that is the whole guard rather than a preference. This subshell
# inherits the caller's traps and is usually signalled before it has run the `trap -`
# that would drop them, so a catchable signal takes it through the fixture's own EXIT
# trap and deletes the run's workdir. Measured: 12 pass / 0 fail against 7 pass / 5
# fail, interleaved at the same load. The `sleep` child is orphaned by this and ends
# on its own; its output is already redirected, so it holds nothing open.
_e2e_deadline_disarm() {
    kill -KILL "$1" 2>/dev/null || true
    wait "$1" 2>/dev/null || true
    if [ -e "$2" ]; then rm -f "$2"; fi
}

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
    local requestedMs="${9:--}"

    echo "waited ${elapsed}s (measured, over ${polls} polls) of a ${bound}s budget for ${what}"
    echo "  evidence: alive=${alive} exit=${status} logGrew=${grew} sinceGrowth=${stall}"

    # Whether the loop polled at the rate it intended is itself a reading, and one
    # that used to be invisible: a bound counted in iterations reports the duration it
    # INTENDED whatever the machine did, so a runner too loaded to poll at the assumed
    # rate looked exactly like one that was not. Named here rather than folded into the
    # findings below, because it is a fact about the MEASUREMENT and each of those is a
    # fact about the subject.
    #
    # It is derived from the pauses the loop ASKED FOR, which is exact and consults no
    # clock (#1066). That is an IMPROVEMENT rather than a repair: the elapsed-based
    # version it replaces -- `elapsed > bound + max(1, bound/10)` -- goes on working
    # under a real deadline, since both the old loop and the new one exit having
    # overshot by at most the last poll. What it could never see is the case this one
    # is for. A host taking two seconds over every `sleep 0.2` inside a 20 s budget
    # overshoots the last poll by two seconds and fires nothing, while asking for about
    # 2 s of pauses where 20 s were intended -- a tenfold signal the old reading had no
    # access to. It is also the discriminator #1066 asks for at `run_bounded`, and this
    # is the second bound that can be exceeded.
    #
    #   * asked for most of the budget -> the loop paced as intended, so the subject is
    #     whatever it was waiting FOR.
    #   * asked for a fraction of it   -> the loop could not poll at that rate, so the
    #     subject is the MACHINE, and the findings below are about a starved observer.
    #
    # `-` means the caller took no such reading, and then nothing is claimed either way.
    #
    # AND A WAIT THAT ENDED EARLY CLAIMS NOTHING EITHER, which `alive = no` is the
    # whole of. The comparison is against the BUDGET, so it only means anything for
    # a wait that spent one: a process noticed dead on the first poll has asked for
    # 0 ms of pauses by design, and the note then reads
    #
    #   the loop asked for only 0ms of pauses inside a 10s budget, so this machine
    #   could not poll at the rate the wait assumed
    #
    # directly above `the process DIED` -- a confident claim about the MACHINE
    # stacked on top of a finding that says the machine is not the subject. It
    # fires on every prompt death, which is the one case this verdict gets exactly
    # right, and `a confident wrong signal is worse than a vague right one` is the
    # rule it breaks. `unknown` is deliberately NOT exempt: no pid was watched, so
    # that wait ran to its budget and the pacing reading is real.
    if [ "$alive" != "no" ] && [ "$requestedMs" != "-" ] && [ "$requestedMs" -lt $(( bound * 500 )) ]; then
        echo "  NOTE: the loop asked for only ${requestedMs}ms of pauses inside a ${bound}s budget, so this"
        echo "        machine could not poll at the rate the wait assumed. Read the findings below with"
        echo "        that in mind."
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

# Retire the deadline this wait ARMED, and never one it was lent. Reads
# `wait_until`'s locals by dynamic scope, which is how bash works and is the same
# late binding `submit_setting` relies on -- a named function rather than three
# copies of the same two-line conditional on three exit paths, one of which never
# returns.
_e2e_wait_retire_deadline() {
    [ "$ownsDeadline" = "yes" ] || return 0
    _e2e_deadline_disarm "$dpid" "$dmark"
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
# So the reported duration is MEASURED rather than assumed, and `SECONDS` is what
# measures it: a bash builtin, so it costs no fork, in bash 3.2, one-second
# resolution, which is ample against budgets of 20 s and 240 s. `sleep` stays, as
# pacing.
#
# **`SECONDS` no longer BOUNDS the loop, and that half of this paragraph was wrong
# for a reason it could not state (#1066).** It is `CLOCK_REALTIME`, so on a host
# that steps its wall clock a bound taken from it is not a duration at all: a
# backward step lengthens the wait past its ceiling and a forward one cuts it
# short, and neither is distinguishable from the thing the bound exists to detect.
# The bound is `_e2e_deadline_arm` -- a background `sleep`, which counts down a
# relative interval and cannot be moved by either.
#
# The two halves are independent and both still hold. The original argument was
# against a bound counted in ITERATIONS, which reports a duration nobody observed;
# that is still right, and the deadline is still a duration rather than a count.
# What it did not consider is that a clock can be a fiction too.
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
# @param 6 optional pre-armed deadline, as `_e2e_deadline_arm` returns it. Given
#          one, this wait SHARES it and does not retire it -- which is how a
#          caller spends ONE budget across two waits without subtracting two clock
#          readings to split it. Absent, this wait arms and retires its own.
# @param 7 optional function name printing SUBJECT-specific findings when the wait
#          ends badly, or "". See below.
#
# WHY THERE IS A FINDINGS HOOK AT ALL, since one more parameter on the general
# wait wants justifying. `_e2e_verdict` answers one question -- was this a slow
# machine, a wedged process or a dead one -- and it answers it from readings every
# wait takes. Some waits can say something MORE, and it is a different fact about
# a different subject: a counter wait knows whether anything answered a scrape,
# whether the series exists at all, and what the last reading was, and those three
# are three terminal states that must not collapse into "timed out" (#643).
#
# The alternative spellings were both worse. `e2e_on_fail` is a FIXTURE-wide hook
# -- `cluster-e2e` dumps every node's log through it -- so a wait that claimed it
# would silently displace whatever the fixture had installed, and it fires for
# every `fail` rather than for this one. A bespoke loop beside `wait_until` is how
# six copies of `wait_for_port` came to have three different failure semantics.
#
# The hook takes no arguments and reads what it needs from its caller's locals by
# dynamic scope, exactly as `_e2e_wait_retire_deadline` does. It runs AFTER the
# verdict and BEFORE the log dump, on stderr, on every bad exit -- expiry and
# death alike, because a process that died with its counter at 3 is a fact worth
# printing beside the death.
wait_until() {
    local ready="$1" what="$2" pid="$3" logfile="$4" seconds="$5"
    local armed="${6:-}" findings="${7:-}"
    local started="$SECONDS" grewAt="$SECONDS"
    local elapsed=0 polls=0 size=0 baseline=0 grew="no" stall=0 alive="yes" status="-"
    local dpid="" dmark="" ownsDeadline="yes" requestedMs=0

    if [ "$logfile" = "-" ]; then
        grew="unknown"
    else
        baseline="$(_e2e_size "$logfile")"
        size="$baseline"
    fi
    if [ "$pid" = "-" ]; then alive="unknown"; fi

    # THE BOUND IS THE DEADLINE, NOT `elapsed` (#1066). `elapsed` is still read
    # from `SECONDS` and still REPORTED -- the verdict's whole point is that the
    # duration it prints was measured rather than assumed -- but it no longer
    # decides when to stop. `SECONDS` is `CLOCK_REALTIME` and this host steps it,
    # so a bound taken from it lengthens on a backward step and shortens on a
    # forward one, and neither is distinguishable from what the bound exists to
    # detect.
    if [ -n "$armed" ]; then
        ownsDeadline="no"
    else
        _e2e_deadline_arm "$seconds"
        armed="$_e2e_deadline_armed"
    fi
    dpid="${armed%% *}"
    dmark="${armed#* }"

    while ! _e2e_deadline_passed "$dmark"; do
        polls=$(( polls + 1 ))
        if "$ready"; then
            _e2e_wait_retire_deadline
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
            # Before the expiry, which never returns: otherwise the timer outlives
            # the run and a 240 s `sleep` is still on the machine when the next
            # fixture starts.
            _e2e_wait_retire_deadline
            _e2e_expire "$what" "$seconds" "$elapsed" "$polls" \
                "$alive" "$status" "$grew" "$stall" "$logfile" "$requestedMs" "$findings"
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
        # What the loop ASKED FOR. Exact, clock-free, and what tells a starved
        # observer from a subject that never became ready -- see `_e2e_verdict`.
        _e2e_ms_of "$_e2e_poll_pause"
        requestedMs=$(( requestedMs + _e2e_ms ))
        elapsed=$(( SECONDS - started ))
        stall=$(( SECONDS - grewAt ))
    done

    _e2e_wait_retire_deadline
    if [ "$grew" = "no" ]; then stall="-"; fi
    _e2e_expire "$what" "$seconds" "$elapsed" "$polls" \
        "$alive" "$status" "$grew" "$stall" "$logfile" "$requestedMs" "$findings"
}

# Bytes in a file, as a bare integer. Fails the run when the file is missing.
#
# **`wc` pads on BSD and does not on GNU**, so `$(wc -c < f)` is `11` on Linux and
# `      11` on macOS. A fixture that captures that and compares it as a STRING is
# green on one platform and red on the other, saying `wrote       11 bytes, expected
# 11` -- a message whose two numbers are equal, which reads as the instrument being
# broken rather than as the shell. `fastcache-cli-e2e` shipped that way and it was
# the fixture's first run on macOS that found it.
#
# Arithmetic contexts (`$(( ))`) and numeric tests (`[ x -eq y ]`) normalise on
# their own; a bare capture does not. Use these rather than remembering which.
#
# @param 1 path to read
# @return the byte count on stdout
count_bytes() {
    [ -r "$1" ] || fail "count_bytes: cannot read $1"
    wc -c < "$1" | tr -d ' '
}

# Lines in a file, as a bare integer. Same padding reason as `count_bytes`.
#
# @param 1 path to read
# @return the line count on stdout
count_lines() {
    [ -r "$1" ] || fail "count_lines: cannot read $1"
    wc -l < "$1" | tr -d ' '
}

# Size of a file in bytes, or 0 when it does not exist yet. `wc -c` rather than
# `stat`, whose flags differ between GNU and BSD.
#
# Not `count_bytes`: a missing file is ORDINARY here (a wait polls a log before
# anything has written it) where for `count_bytes` it is a fixture bug. Two
# contracts, so two functions.
_e2e_size() {
    if [ -r "$1" ]; then
        wc -c < "$1" 2>/dev/null | tr -d ' '
    else
        echo 0
    fi
}

# Print the verdict, dump the log, and stop the run. Never returns.
#
# @param 10 the total the loop asked for in ms, or "-"
# @param 11 a function name printing subject-specific findings, or "". See
#           `wait_until`'s param 7 for why one exists.
_e2e_expire() {
    local what="$1" seconds="$2" elapsed="$3" polls="$4"
    local alive="$5" status="$6" grew="$7" stall="$8" logfile="$9"
    local requestedMs="${10:--}" findings="${11:-}"
    _e2e_verdict "$what" "$seconds" "$elapsed" "$polls" "$alive" "$status" "$grew" "$stall" "$requestedMs" >&2
    # AFTER the verdict and BEFORE the log, so a reader meets the general
    # diagnosis, then what this particular wait knows, then the evidence.
    if [ -n "$findings" ]; then "$findings" >&2; fi
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
# @param 7 optional pre-armed deadline to SHARE; see `wait_until`
wait_for_port() {
    local host="$1" port="$2" pid="$3" what="$4" logfile="$5"
    local seconds="${6:-$_e2e_wait_seconds}" armed="${7:-}"
    _e2e_port_ready() { port_answers "$host" "$port"; }
    wait_until _e2e_port_ready "${what} to listen on ${host}:${port}" \
        "$pid" "$logfile" "$seconds" "$armed"
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
# @param 6 optional pre-armed deadline to SHARE; see `wait_until`
wait_for_log() {
    local marker="$1" pid="$2" what="$3" logfile="$4"
    local seconds="${5:-$_e2e_wait_seconds}" armed="${6:-}"
    _e2e_log_ready() { grep -q "$marker" "$logfile" 2>/dev/null; }
    wait_until _e2e_log_ready "${what} to log: ${marker}" \
        "$pid" "$logfile" "$seconds" "$armed"
}

# The line a compile node logs when a heartbeat round was ACCEPTED by its
# scheduler, and the marker `wait_for_registration` below waits on.
#
# A named constant rather than a literal inside the function, because it is what
# the self-test stages both halves of: this text must be reached by
# `1 of 1 toolchain(s) registered` and must NOT be reached by
# `0 of 1 toolchain(s) registered`, and a test that spelled the marker out a
# second time would agree with itself whatever the function does.
E2eRegisteredMarker="1 of 1 toolchain(s) registered"

# Wait until a node's heartbeat round was ACCEPTED by its scheduler.
#
# THE wait #449 is about, and it is here rather than in a fixture for the reason
# the divergence happened at all. `dist-compile-e2e.sh` matched the bare word
# `registered` at three sites; the summary line is logged after every heartbeat
# round whatever the outcome, so `0 of 1 toolchain(s) registered` matches it too
# and those waits returned for a worker the scheduler had TURNED AWAY. The
# fixture then proceeded against a fleet it believed was formed, nothing failed,
# and the property under test was simply not being tested.
#
# Correcting that in one fixture and leaving the other copies alone would fix the
# instance and reproduce the cause, so the corrected wait lives here and there is
# one of it.
#
# WHY THE COUNT IS LITERAL rather than a parameter. `1 of 1` is
# accepted-of-served, and the fact being waited on is "the scheduler took
# everything this node offered". Every node these fixtures start serves exactly
# one toolchain, so the accepted form is `1 of 1`. A fixture that some day starts
# a node serving two grows a parameter here -- and the parameter is then the
# ACCEPTED count, never the served one and never zero, because a marker naming
# zero accepted toolchains is the defect above wearing a parameter.
#
# @param 1 pid to watch, or "-"
# @param 2 what it is, for the messages
# @param 3 the log to watch
# @param 4 optional bound in seconds; defaults to `e2e_wait_seconds`
wait_for_registration() {
    wait_for_log "$E2eRegisteredMarker" "$1" "$2" "$3" ${4+"$4"}
}

# The line a compile node logs once it is SERVING, and the marker
# `wait_for_node_ready` below waits on.
#
# A named constant, but NOT for the reason `E2eRegisteredMarker` above gives, and
# the difference is worth stating because the shapes are otherwise identical.
# That constant's self-test stages TWO texts -- `1 of 1` against `0 of 1` -- so
# naming it catches a wait that matched the wrong line. This one's self-test
# stages ONE text, present against absent, which catches a wait that does not
# wait; a second spelling of the text would not weaken that pair.
#
# What the constant buys HERE is that a rename has more than one reader. The
# self-test asserts through it (`node-ready-waits-for-marker`) and against the
# expiry message `wait_for_log` BUILDS from it (`node-ready-refuses-bound-only`'s
# required `to log: ...` text), so changing this line's wording fails the suite
# instead of quietly leaving three fixtures waiting for a string nothing logs.
#
# `_selftest_node` spells the text literally on purpose and is not a third
# reader: it is STAGING what a real node writes, the way `registration-accepted`
# stages its line. Staging the product's output and asserting the helper's
# behaviour are different jobs.
E2eNodeReadyMarker="compile node ready"

# The line `fastcached` logs once every acceptor is armed, and the marker
# `wait_for_daemon_ready` below waits on.
#
# The second row of `Core/ReadinessMarker.hpp`, and the two constants here are that
# table read from the shell. Both are wire constants in all but name: this build
# recompiles no fixture, so rewording either breaks its waiters by TIMEOUT -- the
# slowest and least informative failure there is -- with a green build behind it.
#
# `ready, accepting connections` reports ACCEPTING, which is strictly later than the
# bind (#646). That is why a daemon gets a readiness wait rather than the port wait
# five call sites used: a bound port with nobody accepting answers a connect probe
# out of the backlog and then holds the client.
E2eDaemonReadyMarker="ready, accepting connections"

# Wait until a compile node is SERVING, not merely bound.
#
# BOUND IS NOT READY. Since #365 a node binds FIRST and logs this line
# afterwards, so `wait_for_port` returning has been strictly weaker than "the
# node is ready" ever since, and every assertion placed straight after one has
# been resting on the gap being small rather than on the property it needs.
#
# What kept that safe was an ACCIDENT, and #449 removed it without anything being
# able to notice. The old per-fixture helper opened `for _ in $(seq 1 100)` and
# probed before the node had bound, so its first probe always failed and it
# always slept 200 ms. `wait_until` does its setup before probing and does not
# oversleep: on a run where the port answers on the FIRST probe it proceeds with
# no sleep at all, and the assertion then runs inside the window. Measured on the
# `cluster-e2e` n4 shape, that window is single-digit milliseconds -- ample, since
# what has to fit inside it is a shell reaching its next statement.
#
# So this is TWO waits and not one, and they stay two on purpose: a stall before
# the bind and a stall between the bind and readiness are different faults, and
# `wait_until` names the predicate that expired, so the two remain two verdicts.
#
# WHAT THE SECOND WAIT BUYS, concretely. Between the bind and this line the node
# starts the reactor thread that ACCEPTS on the ports it has already bound -- its
# own comment reads "a client that dials the instant a port is bound must not
# find a listener nobody is accepting on" -- brings up its registrars and its
# heartbeat, and installs its stop handlers. A fixture that signals a node inside
# that window is signalling one that has not installed a handler yet, which is
# what #451 found; a fixture that dials one is dialling a listener nobody is
# accepting on.
#
# Registration is deliberately NOT here. Some nodes are started to hold a port and
# a cache tier and never join a fleet, so that stays `wait_for_registration` at the
# call sites that want it.
#
# THE BOUND IS THE TOTAL, and it is split rather than handed to each wait.
#
# Passing it to both is the obvious spelling and it means a stated N enforces up
# to 2N -- the declared number not being the quantity enforced, which is a defect
# this repository has already had and already fixed once: `cluster-e2e.sh`'s
# `enclosing_deadline` exists because a 30 s replication loop could call a 60 s
# `find_leader` twice, "reintroduced by composition rather than by arithmetic".
# Composing two bounded waits reintroduces it the same way.
#
# It is not academic here. `fleet-dashboard-e2e` runs with a 240 s ambient budget
# under a 600 s ctest timeout: at 2x, a node that binds and never becomes ready
# burns 480 s, and if ctest fires first the fixture is KILLED -- losing the
# verdict and the log dump, which is the entire point of the bounded wait. A test
# that hangs reports less than a test that fails.
#
# ONE DEADLINE, SHARED, rather than a budget split by subtracting two clock
# readings (#1066). The old spelling was `remaining = seconds - (SECONDS - started)`,
# and it fails in both directions on a host whose wall clock steps: a BACKWARD step
# makes `remaining` too large, so the stated total silently overruns, and a FORWARD
# one makes it too small -- floored at one second, which then expires without ever
# testing the predicate and reports a readiness failure for a node nobody had asked
# about readiness yet.
#
# That floor was the old code's guard against the same wrong answer arriving by a
# different route (a bind that consumed the whole budget), and the shared deadline
# removes the need for it entirely: there is no remainder to compute, so there is
# nothing to floor. The budget is spent by whichever leg is running, which is what
# "for BOTH waits together" meant in the first place.
#
# @param 1 host
# @param 2 port
# @param 3 pid to watch, or "-"
# @param 4 what it is, for the messages
# @param 5 the log to watch -- required, unlike `wait_for_port`'s, because the
#          marker is read out of it and there is nothing to wait on without it
# @param 6 optional bound in seconds for BOTH waits together; defaults to
#          `e2e_wait_seconds`
wait_for_node_ready() {
    _e2e_wait_ready "$E2eNodeReadyMarker" wait_for_node_ready ${@+"$@"}
}

# Wait until a `fastcached` daemon is ACCEPTING, not merely bound.
#
# The daemon's half of the rule above, and it is not a weaker version of it: its
# marker is emitted by `ReadinessAnnouncer` once the LAST acceptor has armed, so
# between the bind and this line sit every reactor thread and every acceptor the
# daemon runs. `Core/ReadinessMarker.hpp` states both facts in one table and has
# no `Bound` enumerator at all, precisely so a fixture cannot claim the weaker one.
#
# It exists because `dist-compile-e2e.sh` started five daemons and waited on the
# PORT for each (#644). A connect that lands in the backlog with nobody accepting
# answers the probe and then holds the client, and the fixture's very next act is
# to point a launcher at that port. The window is small on a warm machine, which
# is exactly the property #634 found had been quietly true of the node for months.
#
# Same signature, same shared deadline, same required log as
# `wait_for_node_ready`; the marker is the only difference, so they are two rows
# rather than two functions.
wait_for_daemon_ready() {
    _e2e_wait_ready "$E2eDaemonReadyMarker" wait_for_daemon_ready ${@+"$@"}
}

# The body both readiness waits are: bind, then the marker, on ONE deadline.
#
# @param 1 the marker to wait for
# @param 2 the caller's own name, for the refusal below -- a message naming this
#          function would send a reader to a helper they did not call
# @param 3.. the caller's arguments, unchanged
_e2e_wait_ready() {
    local marker="$1" caller="$2"
    shift 2
    local host="$1" port="$2" pid="$3" what="$4" logfile="$5"
    local seconds="${6:-$_e2e_wait_seconds}"
    # A log is REQUIRED, and `-` is refused rather than passed through. It is the
    # sentinel `wait_for_port` accepts in this very position, and two fixtures
    # already pass it there -- so a call site converted from `wait_for_port`
    # without noticing would reach `grep -q "$marker" -`, which reads the
    # FIXTURE'S OWN STDIN once per poll, never matches, and ends the run as a
    # readiness timeout for a process that was perfectly healthy.
    [ "$logfile" != "-" ]         || fail "${caller} needs a log to read the marker out of; ${what} was given '-'"
    local armed dpid dmark
    _e2e_deadline_arm "$seconds"
    armed="$_e2e_deadline_armed"
    dpid="${armed%% *}"
    dmark="${armed#* }"
    wait_for_port "$host" "$port" "$pid" "$what" "$logfile" "$seconds" "$armed"
    # The readiness leg runs on what the bind LEFT, and its verdict says so -- the
    # budget it prints is otherwise a number no caller configured, and a slow start
    # would be reported as "gave up ... of a 2s budget" against a stated 30, which
    # misattributes a slow bind to a readiness stall.
    #
    # It now says it by naming the SHARED budget rather than a remainder, because a
    # remainder is exactly the subtraction of two clock readings this function
    # stopped doing. The deadline is retired here, by the leg that armed it.
    wait_for_log "$marker" "$pid"         "${what} (readiness, on what is left of a ${seconds}s budget shared with the bind)"         "$logfile" "$seconds" "$armed"
    _e2e_deadline_disarm "$dpid" "$dmark"
}


# ---------------------------------------------------------------------------
# The counter wait
# ---------------------------------------------------------------------------

# Pull one counter out of a Prometheus body.
#
# THE GRAMMAR LIVES HERE AND NOWHERE ELSE. It was written out at three call sites
# inside `dist-compile-e2e.sh` before that fixture grew a function for it, and the
# day the exporter grows a label or renders `1` as `1.0`, a copy that still
# matches nothing does not say "the series changed shape" -- it says "the counter
# did not move", which is a statement about the SUBJECT rather than about the
# instrument.
#
# It is in the library rather than in that fixture because `wait_for_counter`
# below needs it, and a second grammar inside the library would be the same defect
# one level down: the wait's "the series is absent" and the fixture's direct reads
# would then disagree about what absent MEANS, silently, in the run where it
# matters (#643).
#
# NOTHING ON STDOUT MEANS THE SERIES WAS ABSENT, which is a different fact from a
# reading of zero and must not be folded into one: a counter is a tally, so zero
# is the truth about events that never happened, while an absent series is a
# counter nothing exports. Every caller checks for the empty string separately.
#
# The body is a whole HTTP response, headers included, because that is what
# `http_get` echoes; the expression is anchored at the start of a line, so no
# header can match it.
#
# @param 1 a whole /metrics response
# @param 2 the Prometheus series name
# @return echoes the reading, or nothing when the series is absent
metric_value() {
    local body="$1" name="$2"
    sed -n "s/^${name} \([0-9][0-9]*\)\$/\1/p" <<< "$body" | tail -1
}

# Read one counter off an admin endpoint: one scrape, one series.
#
# The two outcomes are told apart by the STATUS and not by the output, and both
# callers depend on it: a non-zero return is "nothing answered the request at
# all", an empty echo with status zero is "it answered and the series is not
# there". Folding those two into an empty string is what makes a wait unable to
# say which of its three terminal states it reached.
#
# A caller that wants several series from ONE instant does not use this: it takes
# a body with `http_get` and reads it with `metric_value` as many times as it
# likes, because three requests are three different instants and comparing series
# taken a round trip apart asserts something nobody meant.
#
# @param 1 host
# @param 2 port
# @param 3 the Prometheus series name
# @return echoes the reading; returns 1 if the scrape itself failed
counter_value() {
    local host="$1" port="$2" name="$3" body=""
    body="$(http_get "$host" "$port" /metrics)" || return 1
    metric_value "$body" "$name"
}

# Which of the three ways a counter wait can end badly this one was.
#
# PURE: it reads no clock, opens no socket and touches no process. Everything it
# says comes from its arguments, for the reason `_e2e_verdict` above is pure --
# a decision worth several named outcomes is worth separating from the ambient
# facts it reads, and each branch then costs one staged row rather than a stand-in
# that has to be arranged to exhibit it.
#
# THREE STATES, and the whole of #643 is that they must not collapse:
#
#   * nothing ever answered a scrape -- the admin surface is down, or the port is
#     wrong. Nothing observed here is a statement about the counter at all.
#   * it answered and the series is ABSENT -- the process exports no such counter.
#     A worker with no cache tier exports no cache series, so this is an ordinary
#     answer as well as a possible defect, and it is never a reading of zero.
#   * the series is present and never reached the floor -- and then the LAST
#     READING is the finding, because "it sat at 0" and "it reached 2 of 3" send a
#     reader to different places.
#
# These are printed BESIDE `_e2e_verdict`'s slow-versus-wedged finding rather than
# instead of it: one says what this wait saw, the other says whether the machine
# was in a state to see anything.
#
# @param 1 whether any scrape was answered at all: yes | no
# @param 2 the last reading, or "" when the series was never present
# @param 3 the floor the reading had to reach
# @param 4 the series name
_e2e_counter_finding() {
    local answered="$1" value="$2" floor="$3" name="$4"

    if [ "$answered" != "yes" ]; then
        echo "  COUNTER: nothing ever answered a /metrics request, so ${name} was never read."
        echo "           Nothing above is a statement about the counter; the admin surface is"
        echo "           the subject."
        return 0
    fi

    if [ -z "$value" ]; then
        echo "  COUNTER: /metrics answered and exports no ${name} series at all."
        echo "           An absent series is not a reading of zero."
        return 0
    fi

    echo "  COUNTER: ${name} was read and never reached ${floor}; the last reading was ${value}."
}

# What the last `wait_for_counter` read, and the only way it hands one back.
#
# A global rather than a value on stdout, and the reason is not style. `wait_until`
# announces its own success on stdout -- `waited 0s (3 polls) for ...` -- so a wait
# called in `$( )` would hand its caller that line with the reading appended, and
# every consumer would have to strip it. `start_node` in `dist-compile-e2e.sh` sets
# `started_pid` for a neighbouring reason and records it at length: a function
# called in a command substitution runs in a SUBSHELL, and that fixture's own
# header notes its private counter wait was called that way, so a counter that
# never moved ended the run only because `set -e` happened to notice the
# assignment's status.
#
# Read it on the line after the call.
E2eCounterReading=""

# Whether any scrape during the last `wait_for_counter` was answered at all.
# Private; `_e2e_counter_finding` is what reads it.
_e2e_counter_answered="no"

# Wait until a counter on an admin endpoint reaches a floor, the way
# `wait_for_port` waits for a listener.
#
# The third member of the bounded-wait family rather than a fourth hand-written
# poll loop. `dist-compile-e2e.sh` kept one of those after every other wait in it
# had been converted (#451), and it cost two things (#643):
#
#   * IT DID NOT HONOUR `e2e_wait_seconds`. It opened `local seconds=10`, so an
#     operator raising the fixture's budget -- the documented remedy for a slow
#     box -- scaled every wait in the file except this one. And this is the wait a
#     slow machine lengthens MOST: it waits for a compile to finish and a counter
#     to rise, where the others wait for a process to bind.
#   * IT PRODUCED NO SLOW-VERSUS-WEDGED VERDICT, so its timeout could not say
#     which kind of failure it was -- the distinction `.agent/rules/testing.md`
#     requires of every wait, because a loaded machine and a wedged process are
#     fixed in completely different places.
#
# What it DID have is the reason it was not simply converted: three terminal
# states, and `wait_until` cannot say them. They are `_e2e_counter_finding`'s
# now, reached through the findings hook, so this wait keeps all three AND gains
# the verdict, rather than trading one for the other.
#
# A DEAD PROCESS is `wait_until`'s to report and is not special-cased here: it is
# noticed on the poll it happens on rather than after the budget, and the counter
# finding prints beside it, so "it exited with its counter at 2 of 3" is one
# reading rather than two.
#
# @param 1 host of the admin endpoint
# @param 2 port of the admin endpoint
# @param 3 the Prometheus series name
# @param 4 the floor the reading must reach
# @param 5 pid to watch, or "-"; the rules on `wait_until` apply unchanged
# @param 6 what it is, for the messages
# @param 7 the log to dump when it does not get there, or "-"
# @param 8 optional bound in seconds; defaults to `e2e_wait_seconds`
# @return sets `E2eCounterReading`; never returns on failure
wait_for_counter() {
    local host="$1" port="$2" name="$3" floor="$4" pid="$5" what="$6" logfile="$7"
    local seconds="${8:-$_e2e_wait_seconds}"

    E2eCounterReading=""
    _e2e_counter_answered="no"

    # A FAILED SCRAPE DOES NOT END THE WAIT, because not ending it is what a retry
    # loop is for -- but it is remembered, so a bound that expires having never
    # had an answer says THAT rather than blaming the counter.
    _e2e_counter_ready() {
        local reading=""
        # `2>/dev/null` on the POLL and not inside `counter_value`, so a fixture
        # that scrapes once still sees what bash says. A refused `/dev/tcp` writes
        # a diagnostic carrying exactly the fact the status already carries, and a
        # wait polling a dead admin surface for its whole budget would print it
        # once per poll -- burying the verdict and the COUNTER finding that say the
        # same thing once, at the end, in a sentence.
        reading="$(counter_value "$host" "$port" "$name" 2>/dev/null)" || return 1
        _e2e_counter_answered="yes"
        E2eCounterReading="$reading"
        [ -n "$reading" ] || return 1
        [ "$reading" -ge "$floor" ]
    }
    _e2e_counter_findings() {
        _e2e_counter_finding "$_e2e_counter_answered" "$E2eCounterReading" "$floor" "$name"
    }

    # The deadline is this wait's own, so the sixth argument is empty; the seventh
    # is the hook. Spelled positionally because that is what bash has.
    wait_until _e2e_counter_ready "${what} to reach ${name} >= ${floor}" \
        "$pid" "$logfile" "$seconds" "" _e2e_counter_findings
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
    # Bounded by a DURATION for the reason `wait_until` is: this bound is an
    # assertion about how promptly a process stops, so a loop that silently ran
    # longer than it claimed would let a wedged one through. That duration is a
    # background `sleep` and not `SECONDS` (#1066) -- `SECONDS` is `CLOCK_REALTIME`
    # and this host steps it, which lengthens the bound in one direction and
    # shortens it in the other.
    #
    # `elapsed` is still read from the clock and still reported, because the
    # failure message has to state a duration somebody MEASURED. It no longer
    # decides anything.
    local started="$SECONDS" elapsed=0 armed dpid dmark
    _e2e_deadline_arm "$seconds"
    armed="$_e2e_deadline_armed"
    dpid="${armed%% *}"
    dmark="${armed#* }"
    while ! _e2e_deadline_passed "$dmark"; do
        kill -0 "$pid" 2>/dev/null || {
            _e2e_deadline_disarm "$dpid" "$dmark"
            wait "$pid" 2>/dev/null || true
            return 0
        }
        sleep "$_e2e_poll_pause"
        elapsed=$(( SECONDS - started ))
    done
    _e2e_deadline_disarm "$dpid" "$dmark"
    kill -9 "$pid" >/dev/null 2>&1 || true
    wait "$pid" 2>/dev/null || true
    fail "${what} was still running ${elapsed}s (measured) after being asked to stop, against a ${seconds}s bound"
}

# ---------------------------------------------------------------------------

# How long any one read here may block before the probe gives up.
#
# Named once so the read and the code that INTERPRETS the read cannot disagree
# about it. A bound in one place and a threshold in another is two numbers that
# have to stay equal forever.
#
# IT IS ALSO COUPLED TO A NUMBER IN THE DAEMON, and nothing checks that. It must
# stay STRICTLY ABOVE `AdminHttpServer::RequestTimeout`
# (`src/FastCache/Server/AdminHttpServer.hpp`, 2000 ms today), because
# `http_response_to_silence` reads a silent server's CLOSE as its answer: if the
# server's deadline ever reaches this bound, the probe returns
# `$E2eSilenceInconclusive` and `fleet-dashboard-e2e.sh` FAILS rather than skips.
# #828 proposes splitting that deadline and is exactly the change that would do it,
# so raise this number in the same commit -- the two files are a pair.
_e2e_http_read_bound=5

# The second read that tells a CLOSED peer from a HOLDING one where the exit status
# cannot (bash 3.2). Paid only on that interpreter and on a genuine close, never on the
# common held path under bash 4+, which arm 1 answers for free.
#
# The ordering the probe needs is `fast-return << marker-delay < probe-bound`, and the
# two margins are NOT alike, so both are stated rather than the comfortable one:
#
#   * BELOW the marker: a closed peer returns in microseconds against a 1 s marker, a
#     factor of about a thousand. Losing this needs the marker to fire inside a read
#     that came back at once, and it fails to "the peer is holding" -- a false PASS,
#     the silent direction. Nothing plausible closes a thousandfold gap.
#   * ABOVE the marker: the marker must appear before this bound expires, which is 1 s
#     against 4 s -- a factor of four, and the tight side. It is lost only if a
#     `sleep 1` plus one file creation is delayed past 4 s. This host was measured at
#     load 31 with three lanes gating and that did not happen, but load is not bounded
#     by anything here. When it IS lost, a HOLDING peer reads as closed: a false RED,
#     the loud direction, and the same symptom #1048 had -- now needing a three-second
#     scheduling stall rather than a 1.3 s clock step.
#
# So the failure mode was chosen rather than inherited: the tight margin fails the way
# somebody notices, and the wide margin guards the way nobody would.
#
# The marker sits at 1 s because it is the balance point. Shorter crowds the
# thousandfold side for no gain on the tight one; longer buys margin above and spends
# it below, where a fast close is only microseconds away.
#
# None of these margins has to absorb a clock step, and that is the design rather than
# luck: neither side of the comparison reads the wall clock. `read -t` and `sleep` both
# count down relative intervals, so a `CLOCK_REALTIME` step moves neither.
_e2e_http_probe_bound=4

# Did the bound end that read, or did the peer?
#
# The verdict, split out as a pure function over a READING, for the reason
# `.agent/rules/testing.md` gives for `node-scratch-isolation-e2e`'s classifier: a
# decision that can only be reached by staging the real thing can only be tested
# where the real thing misbehaves. This one misbehaved on macOS ALONE -- bash 3.2
# is what breaks the status-based version -- so inlined in `http_response_to_silence`
# it is a property no Linux or Windows run can exercise, and the guard against it
# coming back is a CI leg that already went red once.
#
# Driven directly, every answer is one line on any platform, and it needs no listener,
# no port and no five-second wait.
#
# **It no longer reads the elapsed TIME, and that is #1048.** The old rule was
# `elapsed >= bound`, timed with `SECONDS` -- which is `CLOCK_REALTIME`, and this host
# steps it. Measured against `CLOCK_MONOTONIC`: a time-sync step of about -1.28 s,
# roughly 1 read in 14; and on this predicate's own case, 7 of 45 reads reported a
# `SECONDS` elapsed of 4 while the monotonic clock measured 5.01-5.06 s. Those reads
# had consumed their whole 5 s bound and carried status 142 to say so -- they only
# LOOKED short -- but `4 >= 5` was false, so a server that had held perfectly was
# reported as having CLOSED. 9 failures in 60 probes against one settled node, every
# body zero bytes, matching `fleet-dashboard-e2e`'s own 3-in-24. The reading was never
# a duration; `_http_drain_fd3` carries the mechanism.
#
# The two readings it takes instead are the ones that are not a race against a margin:
#
# @param 1 the exit status of the read that ended the loop. Above 128 is a timeout and
#          cannot be anything else -- measured disjoint from EOF's 1 on bash 5.2.21,
#          including the reads a stepped clock had made LOOK short. bash 3.2 answers 1
#          for both, which is why there is a second parameter at all.
# @param 2 whether the follow-up probe read BLOCKED: 1 it did, 0 it came back at once,
#          `-` no probe was taken (the peer had spoken, so nothing was ambiguous).
#          Judged against a background `sleep`, never a clock. EOF is sticky, so a
#          closed peer answers in microseconds and a holding one consumes its bound.
# @return 0 when OUR bound ended the read, 1 when the PEER did
_e2e_read_ended_at_bound() {
    if [ "$1" -gt 128 ]; then
        return 0
    fi
    [ "$2" != "-" ] && [ "$2" -ne 0 ]
}

# Read one whole response off fd 3 and close it.
#
# The read half of every helper below, in ONE place. Called directly rather than
# through a command substitution, so its two output variables reach the caller: a
# subshell's variable does not.
#
# `read -t` with a whole number and not a fractional timeout, because bash 3.2
# rejects one. Every read is bounded because the endpoint closes the connection
# itself (`Connection: close`), so a healthy server ends the loop on its own -- and
# a WEDGED one, which is exactly the state these probes exist to detect, would
# otherwise hang the suite instead of failing it.
#
# THE LAST CHUNK. `read` sets its variable and returns non-zero on a final chunk
# with no trailing newline, so a naive loop drops it. That is not a corner case
# here: the fleet dashboard's JSON document is ONE line with no newline at all, so
# without the line below the whole body vanishes and every assertion about it
# fails for a reason that has nothing to do with the server. One of the seven
# copies this file replaced learnt that; the other six never did -- which is the
# whole argument for there being one copy now rather than one per helper.
#
# ## What ended the read, and why NOT the elapsed time
#
# "The peer closed" and "our own bound expired" are different facts, and this file
# used to tell them apart by TIMING the read against `SECONDS`: one that consumed its
# whole bound was the bound's, anything shorter was the peer's. That is #1048.
#
# The argument it rested on was stated here at length, and its algebra is correct:
# `floor(a + BOUND) - floor(a)` really is exactly `BOUND`, and 200k simulated
# placements really did confirm it. **What it never states is the premise it needs:
# that the clock does not move.** On a host whose wall clock steps, the algebra is
# still true and the conclusion is still false, because `a` and `a + BOUND` are no
# longer readings of the same timeline.
#
# `SECONDS` is `CLOCK_REALTIME`. Measured against `CLOCK_MONOTONIC` on a WSL2 host: a
# host time-sync step of about **-1.28 s**, roughly 1 read in 14. Measured directly on
# this predicate's own case, 7 of 45 reads reported a `SECONDS` elapsed of 4 while the
# monotonic clock measured 5.01-5.06 s. The reads consumed their whole bound. They
# only looked short.
#
# So a 5 s bound "measured" 3.7 s, `4 >= 5` was false, and a server that had held
# perfectly was reported as having CLOSED -- 9 failures in 60 probes against one
# settled node, every one with a ZERO-byte body, so the server had said nothing at
# all. The reads all carried status 142, a genuine full-bound timeout, which is the
# confirmation: nothing returned early, the clock moved underneath the measurement.
#
# A wider tolerance is not the fix. The step size is a property of the host's time
# sync, not of this code, and nothing bounds it.
#
# ## The two readings that replace it, and what each depends on
#
# **Arm 1, the exit status.** A timeout is above 128; EOF is 1. Measured on bash
# 5.2.21: EOF carried 1 in 5 of 5, a timeout carried 142 in 40 of 40, including every
# read a stepped clock had made look short. **Depends on no clock at all**, which is
# why it is first.
#
# **Arm 2, a follow-up probe**, because arm 1 is unavailable where this has done its
# damage: bash 3.2 answers a plain `1` for both, macOS runs `/bin/bash`, and macOS is
# where #1048 and #1058 each ejected a pull request from the merge queue within an
# hour. Arm 2 is the load-bearing path, not a fallback.
#
# EOF is STICKY -- a closed peer leaves the fd readable forever -- so a second read
# returns at once where a holding peer consumes its whole bound. That is the signal;
# the difficulty is reading it on an interpreter with no monotonic clock.
#
# **It is NOT read from `SECONDS`.** Doing that would put the verdict back on the
# clock that caused this ticket, in the arm that runs where the ticket hurt. Instead a
# background `sleep 1` creates a marker file: `sleep` counts down a RELATIVE interval,
# so a wall-clock step cannot move it. If the marker exists when the read returns, the
# read blocked for about a second or more and the peer is holding; if not, the read
# came back at once and the peer had closed.
#
# The separation is a closed peer's microseconds against a held peer's whole
# `_e2e_http_probe_bound`, with the one-second marker between them, and neither side
# of that comparison consults the wall clock.
#
# No version test. The arms are selected by what was OBSERVED -- a status above 128,
# or its absence -- so 3.2 falls to arm 2 by construction rather than by this file
# asserting a version number about the interpreter running it. After #1048 that
# distinction is the point: the old rule failed because it asserted a property of the
# environment instead of reading one.
#
# What is NOT established: bash 3.2's own behaviour. There was no 3.2 to measure on,
# and the claim that it answers 1 for both is this file's own and remains untested.
# Arm 2 is built not to care either way, which is why it consults no status.
#
# It probes the LAST read rather than the loop, because a server that dribbles a line
# every two seconds would otherwise accumulate past the bound and be called a timeout.
#
# `_http_drain_elapsed` is still set, and it is an OBSERVATION rather than a verdict.
# It is a `SECONDS` difference, so on a stepping host it is not a duration; do not
# reintroduce it into any decision.
#
# @return echoes the body, and sets `_http_drain_ended` to `bound` when our own
#         deadline expired or `peer` when the server closed or spoke.
_http_drain_fd3() {
    local line="" body="" before=0 status=0
    _http_drain_ended="peer"
    while :; do
        # `|| status=$?` rather than a bare read: a command whose failure
        # is TESTED is exempt from `set -e`, and a bare one is not. This file is
        # sourced by fixtures on both settings -- `check-e2e-helpers.sh` sets `-e`,
        # `fleet-dashboard-e2e.sh` sets only `-uo pipefail` -- so a bare read here
        # ended the whole shell on EOF in one and returned normally in the other.
        #
        # And the status is taken AT the read: a `while read ...; do ...; done`
        # leaves `$?` holding the last command of the BODY, so on a peer that never
        # says anything the body never runs and `$?` is a clean 0 -- which reads as
        # "the peer closed" for a read that in fact timed out.
        status=0
        before=$SECONDS
        IFS= read -r -t "$_e2e_http_read_bound" line <&3 || status=$?
        _http_drain_elapsed=$(( SECONDS - before ))
        [ "$status" -eq 0 ] || break
        body+="${line}"$'\n'
    done
    if [ -n "$line" ]; then body+="$line"; fi

    # WHICH ended the read, and never from the elapsed time -- see the block above.
    #
    # Arm 1: a status above 128 is a timeout and nothing else. Measured on bash 5.2.21:
    # EOF carries 1 (5 of 5), a timeout carries 142 (40 of 40) -- including the six the
    # stepped clock had made look short, which is what showed they were never short.
    # Disjoint sets, and no clock is consulted. A bash that never reports it simply
    # never takes this arm: no version test, because the arm is selected by what was
    # observed rather than by a number asserted about the interpreter.
    #
    # Arm 2, and it is the LOAD-BEARING one: bash 3.2 answers a plain 1 for both, macOS
    # runs `/bin/bash`, and macOS is where #1048 and #1058 each ejected a pull request
    # within an hour. EOF is STICKY -- a closed peer leaves the fd ready forever -- so a
    # second read returns at once where a holding peer consumes its whole bound.
    local probeBlocked="-" marker="" sleeper=""
    if [ "$status" -le 128 ] && [ -z "$body" ]; then
        # The marker is made by `sleep`, which counts down a RELATIVE interval and is
        # therefore immune to the wall clock stepping. Reading `SECONDS` here instead
        # would put the drain's verdict back on the clock that caused #1048. Both
        # primitives are measured-immune rather than documented-immune: 20 x `sleep 0.2`
        # took 4.03 s monotonic in a run whose realtime read 2.76 s, and `read -t 5`
        # measured 5010 ms monotonic in all of 20 reads while realtime was short in 3.
        #
        # `mktemp`, not a fixed name or a `$$` suffix. ctest runs this suite in parallel
        # and one fixture drives dozens of probes, so a reused path can find a STALE
        # marker -- which reads as "the peer is holding", a false PASS and the silent
        # direction. The C++ side has `src/tests/ScratchPath.hpp` for this and the
        # argument does not change for a shell fixture.
        marker="$(mktemp "${_e2e_workdir}/.silence-probe.XXXXXX")"
        rm -f "$marker"
        # `>/dev/null 2>&1` IS LOAD BEARING, and it is not the redirection that looks
        # it. `_http_drain_fd3` echoes the body, so every caller runs it inside `$( )`,
        # and a command substitution ends when the last WRITER closes the pipe rather
        # than when the command exits. This background job inherits that pipe.
        #
        # Killing the subshell does not necessarily close it: `( sleep 1; ... )` forks a
        # subshell which forks `sleep`, so `kill "$sleeper"` reaps the SUBSHELL and can
        # orphan the `sleep`, which goes on holding the inherited fd until it ends.
        # Measured in isolation, with the arm and the kill separated by 150 ms of work:
        # three calls inside `$( )` cost 3240 ms without this redirect and 560 ms with
        # it -- one marker interval each.
        #
        # NOT REPRODUCED HERE, and that is stated rather than glossed: this arm kills
        # the sleeper in the same breath as reading the marker, so the orphan window is
        # narrow, and five probes measured 590 ms against 570 ms for a peer that closes
        # at once and 1750 ms against 1720 ms for one that closes after 200 ms. The
        # redirect is kept on cost rather than on evidence of harm -- it is free, the
        # window demonstrably exists, and on bash 3.2 every probe reaches this arm,
        # which is the platform this cannot be measured on and the one both #1048 and
        # #1058 ejected a pull request from.
        # AND THE SIGNAL THAT RETIRES IT MUST BE UNCATCHABLE, which is a second
        # defect in the same three lines and a worse one. A subshell forked here
        # inherits this shell's traps, and a fixture's EXIT trap is its cleanup -- so
        # a timer killed with a catchable signal runs `trap 'exit 1' TERM`, reaches
        # the EXIT trap and `rm -rf`s the run's workdir out from under the run.
        #
        # The `trap -` below covers a subshell that has STARTED; it is not the guard.
        # The disarm follows the arm within microseconds whenever the read returns at
        # once, so the subshell is usually signalled before it has run a single
        # command, and `trap -` has not executed yet. `kill -KILL` is what closes it.
        #
        # Measured on the sibling timer #1066 adds, which has the identical shape and
        # is where this was found: interleaved and order-alternated at one load,
        # 12 pass / 0 fail with the uncatchable signal against 7 pass / 5 fail
        # without it -- and `trap -` alone was indistinguishable from no fix at all.
        # This arm is unreachable on bash 4+ whenever the status is decisive, so the
        # platform where it would fire on every probe is the one it cannot be
        # measured on.
        # `3>&-` CLOSES THE CALLER'S SOCKET, and this function always has one open:
        # `http_get` and `http_response_to_silence` both `exec 3<>/dev/tcp/...` before
        # calling it. A forked timer inherits that descriptor, and the `sleep` orphaned
        # by the KILL below then holds the peer's socket for the rest of its interval,
        # after the fixture believes it has closed it.
        #
        # Measured against a real listener, the timer's child attributed by ancestry
        # rather than by a global `pgrep`: without `3>&-` it holds 1 socket fd and its
        # fd 3 reads `socket:[...]`; with it, 0 and none.
        ( trap - EXIT TERM INT HUP; sleep 1; : > "$marker" ) >/dev/null 2>&1 3>&- &
        sleeper=$!
        read -r -t "$_e2e_http_probe_bound" _ <&3 || true
        if [ -e "$marker" ]; then probeBlocked=1; else probeBlocked=0; fi
        # Reaped here on the ordinary path, and what is reaped is the SUBSHELL: its
        # `sleep` child can outlive the signal and end on its own, which is why the
        # redirect above is the part that matters and this `kill` is not.
        #
        # An ABANDONED one needs no ledger and is not a new site for #845: it is a
        # `sleep 1`, so it is gone within a second whatever happens to this shell,
        # which a daemon-shaped background job would not be.
        kill -KILL "$sleeper" 2>/dev/null
        wait "$sleeper" 2>/dev/null
        rm -f "$marker"
    fi
    if _e2e_read_ended_at_bound "$status" "$probeBlocked"; then
        _http_drain_ended="bound"
    else
        _http_drain_ended="peer"
    fi

    exec 3<&-
    printf '%s' "$body"
}

# What an HTTP surface says, unprompted, to a peer that says NOTHING.
#
# Connect and read; never send. That is an unused browser preconnect exactly, and
# it is the DETERMINISTIC form of the #824 guard. The shape a report describes --
# connect, wait, then ask -- cannot be asserted on: once the server has answered
# and closed, the client's late write draws an RST, and the RST discards the
# client's receive buffer before the response in it can be read. Measured both
# ways against one unfixed binary: a `python` client saw the `400`, a bash client
# saw nothing. A guard built on that passes on the bug about half the time.
#
# No idle parameter. `_e2e_http_read_bound` outlasts any request deadline this tree
# sets, so a server that means to answer a silent peer has answered by the time the
# bound expires and one that means to close has closed. Sleeping first would only
# make the case slower.
#
# It REFUSES rather than answering when that bound is what ended the read: an
# expired `read -t` yields an empty body, which is byte-identical to "the server
# volunteered nothing" -- so without this the probe would go vacuous, silently,
# the day anyone raises the server's deadline past five seconds.
#
# **Three outcomes, three statuses, and the statuses are NAMED.** "The connection
# was refused" and "the bound expired" are different facts about different machines,
# and folding them into one non-zero is the same mistake one layer up as the `bool
# ok` this whole change is about: the caller then prints a diagnosis it has not
# established -- a dead node reported as a surface that would not answer.
#
# The names exist for the reason `E2eBoundOutcomeExceeded` below gives, and this
# function is the case that argument was written about. A bare literal FAILS OPEN:
# writing `1)` where `2)` was meant makes a fixture print "the node is gone" for
# "the surface would not answer" -- the exact mis-bucketing the three statuses exist
# to prevent, and no shell setting catches it. Compared against a name instead, a
# typo is an unbound variable and every caller here runs under `set -u`, so the same
# mistake stops the run and says where.
E2eSilenceAnswered=0
E2eSilenceInconclusive=1
E2eSilenceRefused=2

# @param 1 host
# @param 2 port
# @return echoes whatever the server said unprompted, EMPTY when it said nothing.
#         `$E2eSilenceAnswered` the server answered or closed, so the body is its
#         answer; `$E2eSilenceInconclusive` the read bound expired first, so there
#         is no answer to report; `$E2eSilenceRefused` the connection was refused,
#         so nothing was ever asked.
http_response_to_silence() {
    local host="$1" port="$2"
    exec 3<>"/dev/tcp/${host}/${port}" || return "$E2eSilenceRefused"
    _http_drain_fd3
    # What ENDED the read, never how long it took. `_http_drain_ended` is `bound` when
    # our own deadline expired -- which is not an answer about the server -- and `peer`
    # when the server closed or spoke, which is.
    [ "$_http_drain_ended" = "peer" ]
}

# ---------------------------------------------------------------------------

# GET one path and echo the whole response, headers included.
#
# `/dev/tcp` rather than curl, because a fixture that skips when curl is absent
# tests nothing on the machine that lacks it, and this needs no more than one
# request. The read half, its bound and the last-chunk recovery are
# `_http_drain_fd3`'s.
#
# @param 1 host
# @param 2 port
# @param 3 path
# @param 4.. extra request header lines, without CRLF, e.g. "Authorization: x"
# @return echoes the response; returns 1 if the connection was refused
http_get() {
    local host="$1" port="$2" path="$3"
    shift 3
    local header=""
    exec 3<>"/dev/tcp/${host}/${port}" || return 1
    {
        printf 'GET %s HTTP/1.1\r\nHost: %s\r\n' "$path" "$host"
        for header in ${@+"$@"}; do
            printf '%s\r\n' "$header"
        done
        printf 'Connection: close\r\n\r\n'
    } >&3
    _http_drain_fd3
}

# ---------------------------------------------------------------------------

# The status `run_bounded` returns when the ceiling expired before the command
# did. 124 is `timeout(1)`'s, so a reader who knows that convention reads this
# one for free.
#
# It DOES collide with a command that exits 124 of its own accord -- one integer
# cannot carry both facts, which is this ticket's whole subject one layer down.
# So the status is the convenience and `e2e_bound_outcome` below is the answer: a
# caller for which the difference matters reads that instead, and `cluster-e2e`
# does, because reporting "the probe did not finish" about a client that answered
# is exactly the mis-bucketing #457 is about.
E2eBoundExceeded=124

# The three outcomes, NAMED -- for the reason the status above is named, carried
# one level further. This file argued carefully for `E2eBoundExceeded` and then
# left the outcomes as bare strings on both sides: `printf 'exceeded'` at the
# producer and `[[ "$outcome" == "exceeded" ]]` at every consumer.
#
# A string comparison against a literal FAILS OPEN. `"exceed"` matches nothing,
# falls through whatever the caller does for `finished`, and reads as a probe
# that completed -- in `tsan-gate.sh`, whose entire subject is refusing to
# conclude "fine" from "I could not tell". Compared against a name instead,
# `$E2eBoundExceded` is an unbound variable and every caller here runs under
# `set -u`, so the same typo stops the run and says where.
#
# The VALUES are unchanged, deliberately: a caller still comparing to a literal
# keeps working, so this can be adopted per caller rather than in one sweep.
E2eBoundFinished="finished"
E2eBoundOutcomeExceeded="exceeded"
E2eBoundUnstartable="unstartable"

# Which of THREE things happened in the last `run_bounded`: `finished`,
# `exceeded`, or `unstartable`. Read with `e2e_bound_outcome`, never as a
# variable. Unambiguous where the status cannot be, because each is recorded from
# something observed rather than inferred from a number that means several
# things.
#
# **A FILE, and the first version of this was a shell variable that could not
# work.** Its comment claimed `cluster` "calls both inside one `$( ... )`, so the
# subshell that sets it is the subshell that reads it". That is false and the
# fixture proved it: `cluster` runs in one subshell, and `out="$(run_bounded …)"`
# opens ANOTHER inside it, so the assignment was discarded at the closing paren
# and every unstartable probe read back as `finished`. Verified end to end --
# 870 probes filed as `declined`, `0 NEVER STARTED`, against a client that did
# not exist.
#
# That is the defect this file's own `probe_log` comment was written about, in
# the helper written to fix it, with the wrong claim spelled out beside it. The
# self-test did not catch it because it called `run_bounded` directly while the
# only real caller captures its output -- a test exercising the helper
# differently from production is a test of something else.
#
# `unstartable` is the one that had to be measured. A command that cannot be
# executed exits **127 on Linux and 1 on macOS** -- observed, on this repository's
# own CI, in the commit that introduced this function. A caller matching 126/127
# therefore files it as an ordinary refusal on macOS, which is #457's defect for
# the third time: a state that cannot be reported gets reported as its neighbour,
# and the platform it breaks on is the one nobody here can run.
#
# And 127 is doubly ambiguous even on Linux -- `wait` answers 127 for a pid it
# cannot speak for, which `wait_until` above already records. So this is not
# inferred at all: `run_bounded` checks that the command is executable BEFORE
# spawning it.
#
# Per RUN and not per shell, so a caller must read it before its next
# `run_bounded` -- the same discipline `$?` already imposes. These fixtures probe
# sequentially; two concurrent `run_bounded`s would race for it, exactly as they
# would for `probe_log`.
_e2e_bound_outcome_path() { printf '%s' "${_e2e_workdir}/.bounded-outcome"; }

# What the last `run_bounded` did: finished | exceeded | unstartable.
#
# This is the ONE helper built to keep those three apart, and it used to have a
# fourth state it silently mapped onto the happiest of the three: `cat` failing was
# swallowed by `2>/dev/null || true`, so a record that was never written or could
# not be read came back `finished`. A canary that hung and was killed, in a run
# where the outcome file could not be written, read as a completed run (#709).
#
# The four states are now separated rather than a fourth being added to the
# vocabulary, which would change the contract every consumer already depends on:
#
#   * absent          -- nothing was bounded. `run_bounded` writes the record as its
#                        FIRST act and refuses if that write fails, so absence can
#                        only mean it was never called. That is what a caller which
#                        bounded nothing should read, and it stays `finished`.
#   * present, sound  -- the recorded word.
#   * present, unreadable or empty  -- the instrument cannot answer, so it REFUSES.
#   * present, unrecognised         -- likewise. A truncated write is not a verdict.
#
# Refusing rather than inventing a value is how this repository handles "the
# instrument could not answer" everywhere else, and it keeps the three-word
# vocabulary the callers were written against.
e2e_bound_outcome() {
    local path recorded
    path="$(_e2e_bound_outcome_path)"

    if [ ! -e "$path" ]; then
        printf '%s' "$E2eBoundFinished"
        return 0
    fi

    if [ ! -r "$path" ]; then
        fail "e2e_bound_outcome: ${path} exists and cannot be read, so this fixture has no outcome to report"
    fi

    recorded="$(cat "$path" 2>/dev/null || true)"
    case "$recorded" in
        "$E2eBoundFinished" | "$E2eBoundOutcomeExceeded" | "$E2eBoundUnstartable")
            printf '%s' "$recorded"
            ;;
        "")
            fail "e2e_bound_outcome: ${path} is empty, so the recorded outcome was lost rather than being 'finished'"
            ;;
        *)
            fail "e2e_bound_outcome: ${path} holds '${recorded}', which is not one of ${E2eBoundFinished}/${E2eBoundOutcomeExceeded}/${E2eBoundUnstartable}"
            ;;
    esac
}

# Run a command under a wall-clock ceiling. Echoes its combined output.
#
# @param 1 the ceiling, in seconds
# @param 2.. the command and its arguments
# @return the command's own exit status, or `E2eBoundExceeded` if the ceiling
#         expired first
#
# ---------------------------------------------------------------------------
# Why this is bash and not `timeout(1)`
# ---------------------------------------------------------------------------
#
# Because `timeout(1)` IS NOT ON macOS, and reaching for it cost this repository
# a red CI leg with a confident wrong diagnosis attached (#457's own first fix).
# `cluster-e2e.sh` bounded its probe with a bare `timeout`; on macOS every probe
# was instead bash reporting `command not found`, which is a status of 127 --
# not 124, so it was not read as a bound expiring, and not the cluster's own
# words, so it matched none of the patterns the caller tested for. The fixture
# then reported "no node ever named a leader" about a cluster whose own dumped
# logs showed a leader elected in term 1 with both followers naming it.
#
# The obvious repair is to look for `timeout` and then `gtimeout`, which
# `scripts/tsan-gate.sh` used to do and which this file deliberately does NOT do.
# Measured rather than assumed: GitHub's `macos-14` image ships **neither** --
# `gtimeout` comes from Homebrew's `coreutils`, which is not in that image, and
# `tsan-gate.sh`'s macOS branch had never executed anywhere because the
# `clang-tsan` job is `runs-on: ubuntu-24.04`. (That resolver is gone as of #488;
# the gate calls `run_bounded` and the scan below no longer exempts it. The
# measurement above is this comment's own and stays here -- it is what #488 cites
# rather than restates.) So a resolver that refuses when it
# finds nothing would refuse on exactly the platform this was written for, and
# one that falls back to running unbounded would restore the unbounded probe
# while looking like it had a bound.
#
# A bound implemented here needs no binary, is the same bound on every platform
# CI builds, and can be shown expiring on a developer's machine. That removes the
# failure mode rather than detecting it, so there is no fourth state to report
# and no platform to refuse.
#
# This function is therefore also the CHECK, in #469's sense: with a bounded run
# in the shared library there is no reason for a fixture to spell `timeout`
# again, and `check-e2e-helpers.sh` scans for one that does. A paragraph in
# `tsan-gate.sh` did not travel to the next script that needed it, and a fourth
# private copy is how the three `ScriptedSocket` copies each carried the same
# defect. That paragraph was originally described here as CORRECT; #488 measured
# it and it was not -- it claimed the `clang-tsan` preset runs on macOS, which it
# does not, so the fallback it justified had never executed anywhere. Worth
# keeping in view, because it makes the case stronger rather than weaker: what
# failed to travel was not a fact but a plausible sentence, and a scan cannot
# tell those apart either, which is why the remedy is a shared implementation
# rather than a better comment.
#
# ---------------------------------------------------------------------------
#
# The output goes through a FILE rather than a pipe. A pipe would have to be
# read while the command runs -- a reader blocked on it is a second thing that
# can hang, and it is the thing that would hang first, since a wedged command is
# precisely one that has stopped writing.
#
# `mktemp` and not a counter this function increments. Every caller so far
# invokes it inside `$( ... )` to read the output, which is a SUBSHELL: a counter
# incremented here is discarded at the closing paren, so every call would name
# the same file and two overlapping runs would read each other's output. That is
# the same defect `cluster-e2e.sh`'s own `probe_log` comment was written about,
# one level down and in the helper written to fix it.
#
# ---------------------------------------------------------------------------
#
# **The cadence RAMPS, and that is a measurement.** `_e2e_poll_pause` is 0.2 s,
# which is right for `wait_until` -- it waits on a service coming up, where a
# fifth of a second is nothing. Here it was catastrophic: a command that returns
# in 16 ms is still only observed on the next tick, so the FIRST version of this
# function cost **205 ms for a command taking 0 ms** (measured: 10 x
# `run_bounded 5 true` = 2046 ms). The healthy probe `cluster-e2e` records is
# 16 ms and it makes 74 of them, so a fixture whose entire subject is fitting
# inside a 300 s CTest budget had just multiplied its own polling cost by twelve.
#
# `wait -n` would remove the polling outright and is **bash 4.3+**, so it is out
# on the platform this whole function exists for. A watchdog subshell
# (`( sleep n; kill $pid ) &`) removes it too and was rejected on a worse ground
# than portability: after `wait` reaps the child, the pid may be REUSED, and a
# watchdog that then fires signals an unrelated process. A bound that can kill a
# stranger is not a bound.
#
# So the pause starts at 10 ms and grows to `_e2e_poll_pause`. A command that
# finishes immediately costs one 10 ms tick; a five-second wait costs about a
# dozen `sleep` spawns instead of 250. Neither end is a compromise.
_e2e_bounded_pauses=(0.01 0.01 0.02 0.05 0.1)

# How long a TERM is given before KILL, in seconds.
_e2e_bounded_grace=2

run_bounded() {
    local seconds="$1"; shift
    local capture pid status=0 exceeded=0 tick=0
    local armed="" dpid="" dmark="" pause="" requestedMs=0

    # Written FIRST, and the write is CHECKED. `e2e_bound_outcome` reads absence as
    # "nothing was bounded", which is only sound if a failed write cannot also
    # produce absence -- a removed workdir, ENOSPC, a permissions change. Refusing
    # here is what makes that reading unambiguous (#709).
    if ! printf '%s' "$E2eBoundFinished" > "$(_e2e_bound_outcome_path)" 2>/dev/null; then
        fail "run_bounded: cannot write $(_e2e_bound_outcome_path), so this run could not report its own outcome"
    fi

    # ASKED, not inferred. `command -v` answers whether this name resolves to
    # something executable -- a path, a PATH lookup, a function, a builtin -- and
    # it answers the same on every shell. The alternative is reading the status
    # afterwards, and that number is not a fact:
    #
    #   * measured on this repository's CI, a missing command surfaces as 127 on
    #     ubuntu-24.04 and as 1 on macos-14;
    #   * and 127 is ambiguous even on Linux, where `wait` also returns it for a
    #     pid it cannot speak for -- which `wait_until` above already records, and
    #     which measurement confirmed across all five layers of this function.
    #
    # A caller that matched 126/127 would therefore file an unstartable client as
    # an ordinary refusal on macOS: the four-state classification silently
    # degrading to three, on the one platform this whole function exists for.
    # That is #457's defect returning by a different route, and it is what the
    # self-test caught.
    if ! command -v "$1" >/dev/null 2>&1; then
        printf '%s' "$E2eBoundUnstartable" > "$(_e2e_bound_outcome_path)"
        return 127
    fi

    capture="$(mktemp "${_e2e_workdir}/bounded.XXXXXX")"

    "$@" > "$capture" 2>&1 &
    pid=$!

    # A DURATION, and not one read off a clock. Counting polls enforces a duration
    # nobody chose -- a sleep costs what the host's timer granularity says -- and
    # `SECONDS` is `CLOCK_REALTIME`, which this host steps (#1066). The deadline is
    # a background `sleep`, which counts down a relative interval and cannot be
    # moved by either.
    # ARMED ON THE FIRST POLL, NOT BEFORE THE LOOP, and that is a measurement
    # rather than a preference. Arming costs a fork, and a command that has already
    # finished is reaped during it -- so an eager arm made `kill -0` fail on the
    # first test and the loop body never ran at all. Measured: the fast path
    # dropped from ~350 ms to ~200 ms, which reads as a free speed-up and is not
    # one. The same skipped first tick collapsed the STAGED defect that
    # `bounded-fast-path-bites` uses to prove this ceiling still discriminates,
    # from ~2500 ms to 878 ms, against a 3000 ms floor -- so the version that
    # looked faster had disarmed the guard protecting the thing it looked faster
    # than. One cause, two readings, and only one of them looked like good news.
    #
    # Arming inside the loop means the deadline starts one `kill -0` late. That is
    # a lengthening, which is the direction #1066 complains about -- but by a
    # bounded amount this code chooses, not by an unbounded step a host chooses,
    # and it is paid only by a command that was still running when first asked.
    while kill -0 "$pid" 2>/dev/null; do
        if [ -z "$dmark" ]; then
            _e2e_deadline_arm "$seconds"
            armed="$_e2e_deadline_armed"
            dpid="${armed%% *}"
            dmark="${armed#* }"
        elif _e2e_deadline_passed "$dmark"; then
            exceeded=1
            break
        fi
        # Past the end of the ramp the subscript is empty, and the default is the
        # shared pause. bash 3.2 has arrays; it is `declare -A` that it lacks.
        pause="${_e2e_bounded_pauses[$tick]:-$_e2e_poll_pause}"
        sleep "$pause"
        # What the loop ASKED FOR, which is exact and consults no clock. It is what
        # separates the two ways a bound is exceeded; see the outcome below.
        _e2e_ms_of "$pause"
        requestedMs=$(( requestedMs + _e2e_ms ))
        tick=$(( tick + 1 ))
    done
    if [ -n "$dmark" ]; then _e2e_deadline_disarm "$dpid" "$dmark"; fi

    if [ "$exceeded" -eq 1 ]; then
        # TERM, a grace, then KILL. Waiting on a TERM the command ignores is an
        # unbounded wait inside the thing that exists to bound one -- and a
        # command that ignores TERM is not exotic here, it is what a wedged
        # process looks like.
        #
        # ONE PROCESS DEEP, stated rather than implied: this signals the child,
        # not its process group, so a bounded command that forks leaves
        # grandchildren running -- which is #239's shape. Setting up a group
        # needs job control, and a bound that turns `set -m` on inside a fixture
        # changes that fixture's own signal handling. Every caller here spawns a
        # single client process; a caller that would not must not use this.
        kill -TERM "$pid" 2>/dev/null || true
        _e2e_deadline_arm "$_e2e_bounded_grace"
        armed="$_e2e_deadline_armed"
        dpid="${armed%% *}"
        dmark="${armed#* }"
        while kill -0 "$pid" 2>/dev/null && ! _e2e_deadline_passed "$dmark"; do
            sleep "$_e2e_poll_pause"
        done
        _e2e_deadline_disarm "$dpid" "$dmark"
        kill -KILL "$pid" 2>/dev/null || true
    fi

    wait "$pid" 2>/dev/null || status=$?
    cat "$capture"
    rm -f "$capture"

    if [ "$exceeded" -eq 1 ]; then
        # WHICH way the bound was exceeded, because the two are fixed by different
        # people. The loop asked for `requestedMs` of pauses inside a bound that
        # really did last `seconds`; that figure is exact and consults no clock.
        #
        #   * asked for most of the bound -> the loop paced as intended and the
        #     WORK did not finish. The subject is the command.
        #   * asked for a fraction of it -> the loop could not poll at the rate it
        #     intended, so the HOST was slow. The subject is the machine.
        #
        # Half the bound is the split, which is deliberately not a tight threshold:
        # this separates "roughly all of it" from "a fraction", and a reading near
        # the middle is a machine that was somewhat slow either way.
        #
        # ONE LINE EITHER WAY, on STDERR. Either way, because a diagnostic that is
        # printed only in the slow-host case makes silence ambiguous between "the
        # work did not finish" and "the line was lost"; and stderr, because this
        # function's STDOUT is the bounded command's own output and callers parse
        # it -- `e2e_note` writes to stdout and must not be used here.
        if [ "$requestedMs" -lt $(( seconds * 500 )) ]; then
            printf '   run_bounded: the %ss bound expired having asked for only %sms of pauses, so the HOST could not poll at the rate the bound assumed
'                 "$seconds" "$requestedMs" >&2
        else
            printf '   run_bounded: the %ss bound expired having asked for %sms of pauses, so the loop paced as intended and the WORK did not finish
'                 "$seconds" "$requestedMs" >&2
        fi
        printf '%s' "$E2eBoundOutcomeExceeded" > "$(_e2e_bound_outcome_path)"
        return "$E2eBoundExceeded"
    fi
    return "$status"
}

# Put a command to whoever leads NOW, and assert what comes back.
#
# Generalised from `cluster-e2e.sh`'s `submit_setting`, which was this logic with
# the verb hard-coded to `--cluster-set`. The name changed with it: that one was
# already wrong before the generalisation, because it is also what asserts a
# REFUSAL (a typo'd setting refused by name), so it never only submitted settings.
#
# The caller supplies `cluster`, `find_leader` and `$leader_endpoint`; bash binds
# them late, so this stays a pure control-flow helper and the selftest can drive
# it with stubs. That is the whole reason it lives here rather than in the fixture:
# `cluster-e2e.sh` defines its functions BETWEEN executable sections, so sourcing
# it to test one helper would run three sections of a real cluster first.
#
# Why the retry is on "the answer is not what the caller asserts" rather than on a
# recognised "not the leader" refusal: that refusal has TWO spellings, one for
# "somebody else leads" and one for "an election is in progress", and a fixture
# matching them stops retrying the day either sentence is reworded -- silently.
# Inherited verbatim from `submit_setting`, where it was learned the hard way.
#
# `$leader_endpoint` is pinned when a section derives it, and leadership may
# legitimately move before that section finishes: a slow enough runner blows any
# election timeout, and the rulebook's own note is that a cluster which has ELECTED
# is not one that has FORMED. So a command put to the endpoint that led a moment
# ago is a command put to a node that now answers "ask somebody else" (#117, #172).
#
# @param 1 the `--cluster-*` argument to send
# @param 2 the substring an answer carries when the command did what was asked --
#          which for a refusal-asserting caller is the refusal's own wording
# @param 3 what to report when it never does
ask_leader() {
    local answer
    answer="$(cluster "$leader_endpoint" "$1")"
    if [[ "$answer" != *"$2"* ]]; then
        find_leader "whoever leads now, to re-offer a command the previous leader did not take"
        answer="$(cluster "$leader_endpoint" "$1")"
    fi
    [[ "$answer" == *"$2"* ]] || fail "$3 (asked ${leader_endpoint}): ${answer}"
}
