#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# The self-test for `scripts/lib/e2e-common.sh`.
#
# A shared helper is a shared fake. `src/tests/ScriptedSocket.hpp` is the
# precedent: three private copies of one scripted socket, two of them carrying
# the same defect, found a day apart -- because a fake nobody exercises does not
# report its own bugs. Folding seven copies of `free_port`/`wait_for_port`/
# `http_get`/`fail` into one file makes every fixture depend on this file being
# right, and a helper library with no test that can go red is #449's own failure
# mode one level up: something that passes for a reason unrelated to what it
# guards.
#
# Two halves, and the split is the one `.agent/rules/testing.md` argues for after
# `node-scratch-isolation-e2e` spent a CI leg learning it:
#
#   * The DECISION -- which kind of failure a wait suffered -- is `_e2e_verdict`,
#     which is pure. It reads no clock, touches no process and opens no file, so
#     every one of its branches is a one-line record here, including the ones that
#     cannot be staged (a pid that is not this shell's child) and the ones whose
#     bounds have to be pinned on BOTH sides rather than demonstrated once from
#     the middle. That is where an `-and`/`-or` mistake actually lives.
#   * ACQUISITION -- the poll loop, the liveness check, the log-growth accounting
#     -- is driven for real, with real processes that really die and real logs
#     that really grow, but with one-second budgets rather than a fixture's.
#
# Registered as `e2e-helpers-selftest` and deliberately NOT labelled `smoke`: it
# starts no daemon, needs no compiler, and finishes in a few seconds, so it
# belongs in the default `ctest` set where a change to the helpers is caught by
# whoever made it.
#
# Usage:
#   check-e2e-helpers.sh              run every case
#   check-e2e-helpers.sh --case NAME  run one case in this process (used above)
set -uo pipefail

source_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
library="${source_dir}/scripts/lib/e2e-common.sh"

# ---------------------------------------------------------------------------
# The cases
# ---------------------------------------------------------------------------
#
# Each runs in its own bash process, because most of them end in `fail`, and
# `fail` ends the process -- that being the property under test.

run_case() {
    # The fixtures all run under `set -euo pipefail`, so the helpers are
    # exercised under it here too. A helper that only behaves when errexit is off
    # is a helper no caller has.
    set -e
    local name="$1"
    # NOT `local`: the EXIT trap below runs after this function has returned, so
    # a scratch path scoped to the function is an unbound variable by the time
    # cleanup reads it -- which under `set -u` turns every passing case into a
    # failure at the very last moment, after its assertions have all held.
    scratch="$(mktemp -d)"
    # shellcheck source=lib/e2e-common.sh
    . "$library"
    cleanup() { echo "cleanup ran"; rm -rf "$scratch"; }
    trap cleanup EXIT
    e2e_begin "selftest" "$scratch"
    e2e_wait_seconds 1

    case "$name" in

    # --- `fail` stops the RUN, not the shell that raised it -----------------
    #
    # The defect this pins shipped here: a `fail` called from a `( ... )` group
    # ended the subshell only, the script carried on, and it then reported two
    # further failures about the artefacts the first one explains -- so a reader
    # working upward from the last line starts on the wrong question.
    #
    # All three contexts, because the fix has to hold in each and the obvious
    # guard (compare `BASHPID` with the top-level pid) does not: `BASHPID` is
    # bash 4.0+ and macOS ships 3.2, where it is unset and the comparison
    # silently always holds.
    #
    # WHERE THE SUBSHELL SITS MATTERS. The obvious spelling of this case --
    # a bare `( fail ... )` followed by a line that must not run -- proves
    # nothing, because `set -e` stops the script on the subshell's non-zero
    # status whether or not `fail` signalled anybody. Deleting the signal
    # outright left that version GREEN.
    #
    # So the subshell's status is CONSUMED, by `||` and by an `if`, which is
    # where errexit is switched off and where the real fixtures put it: the
    # construct that found this defect was a control build in a `( ... )` whose
    # result the script then examined. Only the signal can stop the run here.
    fail-top)
        fail "staged failure at the top level"
        echo "BUG: the top-level shell continued"
        ;;
    fail-subshell)
        ( fail "staged failure inside ( ... )"; echo "BUG: the subshell continued" ) \
            || echo "BUG: the top-level shell continued past the subshell"
        echo "BUG: the top-level shell reached the end"
        ;;
    fail-cmdsub)
        if captured="$( echo ignored; fail "staged failure inside \$( ... )" )"; then
            echo "BUG: the command substitution succeeded with '${captured}'"
        else
            echo "BUG: the top-level shell continued past the command substitution"
        fi
        ;;
    fail-hook)
        dump() { echo "the on-fail hook ran"; }
        e2e_on_fail dump
        fail "staged failure with a hook"
        ;;

    # --- `free_port` ---------------------------------------------------------
    #
    # The ledger, which two of the seven copies did not have. Nothing is
    # listening on a port issued a moment ago whose server has not bound yet, so
    # without it a fixture that draws every port it needs before binding any of
    # them can hand the same number out twice -- and the collision surfaces as a
    # process dying of EADDRINUSE, which reads as an unrelated flake.
    #
    # The range, and that every draw is recorded.
    ports)
        drawn=""
        n=0
        while [ "$n" -lt 40 ]; do
            p="$(free_port)"
            [ "$p" -ge 20000 ] || fail "drew ${p}, below the floor of 20000"
            [ "$p" -lt 32000 ] || fail "drew ${p}, at or above the ceiling of 32000"
            case " ${drawn} " in
                *" ${p} "*) fail "drew ${p} twice; the issued-port ledger is not working" ;;
            esac
            drawn="${drawn} ${p}"
            n=$(( n + 1 ))
        done
        # The ledger is a FILE and not a variable for a reason -- every call site
        # is a command substitution and a subshell's assignment is gone the moment
        # it exits -- so assert the file, not just the absence of repeats.
        count="$(grep -c . "${scratch}/.issued-ports")"
        [ "$count" = "40" ] || fail "the ledger holds ${count} ports, not 40"
        echo "40 distinct ports, all in range, all recorded"
        ;;

    # And that the ledger is what CONFINES the draw to ports not yet issued.
    #
    # The forty-draw case above cannot show this and was written believing it
    # could: with the ledger deleted it still passed. Forty draws from twelve
    # thousand numbers repeat about one run in sixteen, so it fails fifteen times
    # out of sixteen to notice a property that is entirely broken -- which is not
    # a weak test, it is a test of something else that happens to be in the room.
    #
    # Seeding `RANDOM` to force the collision does not work either: bash reseeds
    # the generator in subshells, and every call site of `free_port` is a command
    # substitution, so two draws from one seed are two different streams.
    #
    # So the ledger is PRE-LOADED instead, with every port in the range but the
    # top thousand. A draw that consults it can only come back from that
    # thousand; a draw that does not has eleven chances in twelve of coming back
    # from below it, and five draws make that 4 in 10^6. Nothing is listening on
    # any of them -- which is the whole point, and exactly the situation the
    # ledger exists for: a port issued a moment ago, whose server has not bound
    # yet, probes free.
    ports-ledger)
        seq 20000 30999 > "${scratch}/.issued-ports"
        n=0
        while [ "$n" -lt 5 ]; do
            p="$(free_port)"
            [ "$p" -ge 31000 ] \
                || fail "drew ${p}, which the ledger already held; the ledger is not consulted"
            n=$(( n + 1 ))
        done
        echo "the ledger confined five draws to the ports it had not issued"
        ;;

    # `port_answers` is the bool half: a closed port is an ordinary answer for
    # some callers rather than a fault, and they need something that does not end
    # the run. A port this process just drew and never bound is closed by
    # construction.
    port-answers-closed)
        p="$(free_port)"
        if port_answers 127.0.0.1 "$p"; then
            fail "port_answers said something is listening on the unbound port ${p}"
        fi
        echo "port_answers is false for an unbound port"
        ;;

    # --- the wait loop -------------------------------------------------------
    #
    # Driven through `wait_until` rather than through `wait_for_port`,
    # because the loop is where the liveness check, the cost accounting and the
    # log-growth reading live; `wait_for_port` and `wait_for_log` are two-line
    # wrappers that differ only in the predicate. The real-socket cases below
    # cover the wrappers.
    wait-success)
        marker="${scratch}/ready"
        ( sleep 0.6; : > "$marker" ) >/dev/null 2>&1 &
        writer=$!
        ready() { [ -e "$marker" ]; }
        wait_until ready "the staged marker" "-" "-" 5
        wait "$writer" 2>/dev/null || true
        echo "the wait returned when the predicate became true"
        ;;

    # A process that DIED is a third case beside "slow" and "stuck", and it is
    # reported the moment it is noticed rather than after the budget -- calling
    # it a timeout sends the reader to the budget, which is not the subject. The
    # driver asserts the elapsed time is nowhere near the ten seconds allowed.
    wait-death-is-prompt)
        ( exit 3 ) &
        corpse=$!
        sleep 0.4
        never() { return 1; }
        wait_until never "a process that is already gone" "$corpse" "-" 10
        echo "BUG: the wait returned"
        ;;

    # Alive, and it logged nothing at all for the whole budget: it reached the
    # point of being started and no further.
    wait-timeout-silent)
        log="${scratch}/silent.log"
        : > "$log"
        # Redirected, and short. A background process started here inherits this
        # shell's stdout, which is the pipe the driver reads the case's output
        # through -- so an orphan holding it open makes the driver's command
        # substitution block until the orphan exits, whatever the case did.
        sleep 5 >/dev/null 2>&1 &
        sleeper=$!
        never() { return 1; }
        wait_until never "a silent process" "$sleeper" "$log" 2
        echo "BUG: the wait returned"
        ;;

    # Alive, and still logging when the budget ran out. The distinction from the
    # case above is the whole reason a total cannot answer this: growth spread
    # over the whole wait and growth that stopped in the first tick are the same
    # `logGrew=yes` and opposite findings, so the reading that decides is the
    # STALL AGE.
    wait-timeout-progressing)
        log="${scratch}/busy.log"
        : > "$log"
        ( n=0; while [ "$n" -lt 25 ]; do echo "line ${n}" >> "$log"; sleep 0.2; n=$(( n + 1 )); done ) >/dev/null 2>&1 &
        chatty=$!
        never() { return 1; }
        wait_until never "a chatty process" "$chatty" "$log" 2
        echo "BUG: the wait returned"
        ;;

    # Neither reading is available. Reported as its own outcome rather than as
    # the nearest neighbour: skipped, absent, unstarted and failed are four
    # states, and a wait that cannot tell them apart says so.
    wait-timeout-no-pid)
        log="${scratch}/nopid.log"
        : > "$log"
        never() { return 1; }
        wait_until never "something nobody is watching" "-" "$log" 2
        echo "BUG: the wait returned"
        ;;
    wait-timeout-no-log)
        sleep 5 >/dev/null 2>&1 &
        sleeper=$!
        never() { return 1; }
        wait_until never "something with no log" "$sleeper" "-" 2
        echo "BUG: the wait returned"
        ;;

    # Neither reading, which is the shape `macos-package-e2e` uses: launchd owns
    # the job, so there is no pid this shell may watch and no log it writes. Its
    # own row here because that fixture cannot be run from this repository's
    # development machines at all, so the only thing that can exercise the
    # argument form it passes is this file. `wait_for_port` rather than
    # `wait_until`, since it is the wrapper's five-`-`-and-a-bound spelling that
    # is at issue.
    wait-nothing-watched)
        p="$(free_port)"
        wait_for_port 127.0.0.1 "$p" "-" "the installed service" "-" 2
        echo "BUG: the wait returned"
        ;;

    # A wait whose bound is a real duration. Driven from the DRIVER, which times
    # the whole process: nothing inside the wait can measure the wait, because it
    # ends in `fail`.
    wait-clock-bound)
        never() { return 1; }
        wait_until never "a bound that must be real" "-" "-" 4
        echo "BUG: the wait returned"
        ;;

    # `wait_for_log` over a real file, including the part `wait_for_port` alone
    # cannot show: a bound port does not mean a process has finished announcing
    # itself, so the marker has to be found after the fact rather than at the
    # moment it is written.
    wait-for-log)
        log="${scratch}/late.log"
        : > "$log"
        ( sleep 0.4; echo "1 of 1 toolchain(s) registered" >> "$log" ) >/dev/null 2>&1 &
        writer=$!
        sleep 5 >/dev/null 2>&1 &
        holder=$!
        wait_for_log "1 of 1 toolchain(s) registered" "$holder" "the staged node" "$log" 5
        wait "$writer" 2>/dev/null || true
        kill "$holder" 2>/dev/null || true
        echo "wait_for_log returned on the marker"
        ;;

    # --- `wait_for_registration` waits on the ACCEPTED count ------------------
    #
    # #449 in one line. A compile node logs its heartbeat summary after every
    # round WHATEVER the outcome, so a wait on the bare word `registered` also
    # matches `0 of 1 toolchain(s) registered` -- the line a node logs when the
    # scheduler TURNED IT AWAY. Three waits in `dist-compile-e2e.sh` were built
    # that way; they returned for a worker that was not in the fleet, the fixture
    # proceeded against a fleet it believed was formed, and nothing failed. The
    # property under test was simply not being tested.
    #
    # BOTH directions, because neither half means anything alone. A wait whose
    # marker matched nothing at all would pass `refuses-zero` and fail
    # `accepted`; a wait loosened back to the bare word would pass `accepted` and
    # fail `refuses-zero`. Only the pair pins the marker.
    #
    # The `refuses-zero` row asserts the marker text, and it can: what it matches
    # is the message `wait_for_registration` BUILDS from `E2eRegisteredMarker`
    # (`... to log: 1 of 1 toolchain(s) registered`), so a constant changed to
    # anything else fails that row rather than agreeing with it. Staging a log
    # line and then asserting the same string back is what would agree with
    # itself, and neither case does that.
    registration-accepted)
        log="${scratch}/register.log"
        echo "0 of 1 toolchain(s) registered" > "$log"
        ( sleep 0.4; echo "1 of 1 toolchain(s) registered" >> "$log" ) >/dev/null 2>&1 &
        writer=$!
        sleep 5 >/dev/null 2>&1 &
        holder=$!
        wait_for_registration "$holder" "the staged node" "$log" 5
        wait "$writer" 2>/dev/null || true
        kill "$holder" 2>/dev/null || true
        echo "wait_for_registration returned on the accepted round"
        ;;

    # And the discriminating half: a node that is refused every round, forever.
    # The log GROWS the whole time, which is what makes this the shape a loosened
    # marker cannot survive -- there is a matching-ish line available at every
    # poll and the wait still has to expire.
    registration-refuses-zero)
        log="${scratch}/refused.log"
        : > "$log"
        ( n=0
          while [ "$n" -lt 25 ]; do
              echo "0 of 1 toolchain(s) registered" >> "$log"
              sleep 0.2
              n=$(( n + 1 ))
          done ) >/dev/null 2>&1 &
        chatty=$!
        wait_for_registration "$chatty" "a node the scheduler turned away" "$log" 2
        echo "BUG: the wait returned for a round that was refused"
        ;;

    # --- `wait_for_node_ready` waits past the BIND ---------------------------
    #
    # #634. Since #365 a node binds FIRST and logs `compile node ready`
    # afterwards, so `wait_for_port` returning is strictly weaker than "the node
    # is serving". What had been covering that was an ACCIDENT: the old
    # per-fixture helper probed before the node had bound, so its first probe
    # always failed and it always slept 200 ms. `wait_until` does its setup before
    # probing and does not oversleep, so on a run where the port answers on the
    # first probe there is no sleep at all and the caller's next statement runs
    # inside the window.
    #
    # BOTH directions, because neither half means anything alone -- and here they
    # catch a loosened helper by DIFFERENT mechanisms, which is the reason to keep
    # both rather than a symmetry for its own sake. Measured against a
    # `wait_for_node_ready` written as `wait_for_port "$@"`:
    #
    #   * this case still EXITS 0 under that bug, so the status cannot see it. It
    #     is caught only by the assertion that the marker is PRESENT when the wait
    #     returns -- which is why the case reads the log rather than reporting
    #     that the wait came back.
    #   * `refuses-bound-only` below is caught by the status and not by any
    #     output.
    #
    # Each alone passes the broken helper. The pair does not.
    #
    # The marker is `E2eNodeReadyMarker`'s, reached through the helper rather than
    # spelled again here, for the reason `E2eRegisteredMarker` records: a test that
    # wrote the text a second time would agree with itself whatever the function
    # does.
    node-ready-waits-for-marker)
        log="${scratch}/ready.log"
        : > "$log"
        p="$(free_port)"
        _selftest_node "$p" 1 "$log" >/dev/null 2>&1 &
        staged=$!
        # THE BIND IS STAGED FIRST, and that is the point rather than setup.
        #
        # #634's condition is "a run where the port answers on the FIRST probe",
        # which is when `wait_until` sleeps nothing and the caller's next statement
        # lands inside the window. Waiting the port out here is what puts the
        # helper's own port wait into exactly that state, so the case exercises the
        # condition the ticket is about instead of whichever one the machine
        # happens to produce.
        #
        # It also stops the budget below from being spent on the wrong thing: perl
        # takes about a second to start and bind, and a combined budget tight
        # enough to keep `refuses-bound-only` quick expires in the PORT wait rather
        # than the marker wait -- which is this case passing for a reason that has
        # nothing to do with what it tests. Measured: with a 2s budget and no
        # staging, the verdict read "gave up waiting ... to listen on", not "to
        # log".
        wait_for_port 127.0.0.1 "$p" "$staged" "the staged node" "$log" 15
        wait_for_node_ready 127.0.0.1 "$p" "$staged" "the staged node" "$log" 15
        # THE assertion, and it is on the marker rather than on having returned:
        # a helper loosened back to the bind also RETURNS here, and exits 0, so the
        # status cannot tell the two apart and only this can.
        case "$(<"$log")" in
            *"compile node ready"*) echo "wait_for_node_ready returned with the node serving" ;;
            *) echo "BUG: it returned while the node had only bound" ;;
        esac
        kill "$staged" 2>/dev/null || true
        ;;

    # And the discriminating half: a node that binds and NEVER finishes starting.
    # The port answers for the whole budget, so there is a satisfied `wait_for_port`
    # available at every poll and the wait still has to EXPIRE. A helper loosened
    # back to the bind returns immediately and fails this by its exit status.
    node-ready-refuses-bound-only)
        log="${scratch}/never.log"
        : > "$log"
        p="$(free_port)"
        _selftest_node "$p" never "$log" >/dev/null 2>&1 &
        staged=$!
        # Staged for the reason the case above gives, and here it is also what
        # makes the budget mean what it says: the 2s below must be spent waiting
        # for a MARKER that never comes, not waiting for perl to bind. The
        # required text in the table (`to log: ...`) is what pins that -- an
        # expiry in the port wait says `to listen on` and fails the row, so this
        # cannot quietly go back to timing the wrong thing.
        wait_for_port 127.0.0.1 "$p" "$staged" "a node that never finishes starting" "$log" 15
        wait_for_node_ready 127.0.0.1 "$p" "$staged" "a node that never finishes starting" "$log" 2
        echo "BUG: the wait returned for a node that had only bound"
        ;;

    # --- `run_bounded` -------------------------------------------------------
    #
    # The helper that exists because `cluster-e2e.sh` bounded its probe with a
    # bare `timeout`, which macOS does not have. Every row below is a fact that
    # bug turned on.

    # A command that FINISHES owns the answer: its output and its own status
    # reach the caller unaltered. Status 3 rather than 1, so a helper that
    # collapsed every failure to "non-zero" would be visible.
    bounded-returns-status)
        rc=0
        out="$(run_bounded 5 sh -c 'echo carried; exit 3')" || rc=$?
        echo "run_bounded said '${out}' with status ${rc}"
        ;;

    # THE regression, and it is one line because the defect was one line. A
    # command that cannot be executed exits 127, and 127 must not be mistakable
    # for the bound expiring -- that confusion is what turned 591 unrun clients
    # into 591 refusals by a cluster and produced a precise wrong finding about
    # the product. Asserted as a NUMBER and against `E2eBoundExceeded`, because
    # the whole failure was two integers that were not compared.
    bounded-missing-command)
        rc=0
        run_bounded 5 "${scratch}/no-such-client" --cluster-status >/dev/null || rc=$?
        # The OUTCOME is asserted; the status is printed as evidence only. This
        # row first asserted `exited 127` and CI's macOS leg answered `exited 1`
        # -- so the number is a platform fact and not the thing under test, while
        # the outcome is the thing every caller actually branches on.
        echo "a missing command: outcome=$(e2e_bound_outcome) (status ${rc} on this platform)"
        if [ "$(e2e_bound_outcome)" = "exceeded" ]; then
            echo "BUG: a missing command is indistinguishable from the bound expiring"
        fi
        ;;

    # THE PRODUCTION SHAPE, and the row that would have caught the defect the
    # rows around it missed.
    #
    # `cluster-e2e`'s probe runs `answer="$(cluster …)"`, and `cluster` runs
    # `out="$(run_bounded …)"` inside that -- so `run_bounded` executes TWO
    # subshells below the fixture. The first version of the outcome was a shell
    # variable, and the assignment was discarded at the closing paren: every
    # unstartable probe read back as `finished`, and the fixture filed 870 of them
    # as the cluster declining. Every other row here calls `run_bounded` directly,
    # where a variable works perfectly, so all of them passed.
    #
    # A test that exercises a helper differently from its only caller is a test of
    # something else. This one is written in the caller's shape deliberately.
    bounded-outcome-survives-capture)
        probe() {
            local out rc=0
            out="$(run_bounded 5 "${scratch}/no-such-client")" || rc=$?
            printf '%s' "$(e2e_bound_outcome)"
        }
        echo "two subshells down, the outcome reads $(probe)"
        ;;

    # The bound expires, and it reports that rather than the child's status. A
    # `sleep` killed by a signal exits 143 on most shells, and 143 read as an
    # answer is exactly the shape of the bug above.
    bounded-expires)
        rc=0
        run_bounded 1 sleep 30 >/dev/null || rc=$?
        echo "an expired bound exited ${rc}, outcome $(e2e_bound_outcome)"
        ;;

    # A command that CHOOSES to exit 124 is not the ceiling expiring, and one
    # integer cannot say which happened -- so `e2e_bound_outcome` is what a caller
    # reads. Both rows here, because the interesting assertion is that the two
    # cases agree on `rc` and differ on the outcome; testing either alone passes
    # under a helper that never sets the outcome at all.
    bounded-124-is-not-a-timeout)
        rc=0
        run_bounded 5 sh -c 'exit 124' >/dev/null || rc=$?
        echo "a command exiting 124: rc=${rc} outcome=$(e2e_bound_outcome)"
        rc=0
        run_bounded 1 sleep 30 >/dev/null || rc=$?
        echo "a ceiling expiring:    rc=${rc} outcome=$(e2e_bound_outcome)"
        ;;

    # The ramp. The first version of `run_bounded` slept `_e2e_poll_pause` (0.2s)
    # before its second look, so a command taking 0ms cost 205ms -- against a
    # 16ms healthy probe made 74 times per run, in a fixture whose entire subject
    # is fitting inside a CTest budget. Timed from the driver.
    # TWENTY and not ten, and the count is load-bearing. `SECONDS` is a whole
    # number and truncates at both ends, so a gap the check straddles has to be
    # wider than that truncation: at ten, the flat-pause defect costs 2.05s, which
    # reads as 2 against a `-gt 2` threshold and does not fire. The guard written
    # to prove the fixture bites did not bite, on the first thing it was pointed
    # at. At twenty the two cases are ~0.3s and ~4.1s.
    bounded-fast-path)
        for _ in $(seq 1 20); do run_bounded 5 true >/dev/null; done
        echo "twenty immediate commands ran"
        ;;

    # And the child is DEAD, not merely abandoned. A helper that returns 124
    # while leaving the process running is worse than no bound: the run
    # continues, the process keeps competing for the machine, and the fixture's
    # own cleanup then waits on it. Measured by having the child keep writing:
    # the file must stop growing once `run_bounded` has returned.
    bounded-kills-the-child)
        marks="${scratch}/marks"
        : > "$marks"
        rc=0
        run_bounded 1 sh -c 'while true; do echo tick >> "$1"; sleep 0.1; done' _ "$marks" \
            >/dev/null || rc=$?
        before="$(wc -c < "$marks" | tr -d ' ')"
        sleep 1
        after="$(wc -c < "$marks" | tr -d ' ')"
        echo "the bound exited ${rc}; the child wrote ${before} bytes then ${after}"
        if [ "$before" != "$after" ]; then
            echo "BUG: the child was still running after run_bounded returned"
        fi
        ;;

    # A command that IGNORES TERM is still stopped. Not exotic: it is what a
    # wedged process looks like, and a helper that waits politely for such a
    # child has put an unbounded wait inside the thing that exists to bound one.
    # The driver times this row; the assertion here is only that it returned.
    bounded-outlasts-a-trapped-term)
        rc=0
        run_bounded 1 sh -c 'trap "" TERM; sleep 30' >/dev/null || rc=$?
        echo "a TERM-ignoring child exited ${rc}"
        ;;

    # --- the real-socket cases ----------------------------------------------
    #
    # `wait_for_port` and `http_get` against a listener that really binds, really
    # answers and really closes. Perl rather than nc: `nc`'s listen flags differ
    # between the BSD, GNU and OpenBSD builds, and one of those is on every
    # platform CI runs but never the same one.
    wait-for-port)
        p="$(free_port)"
        # stderr goes into the same file `wait_for_port` is told to dump, so a
        # listener that cannot bind explains itself in the failure rather than
        # arriving as an unexplained death. stdout is closed off for the reason
        # the silent case gives.
        _selftest_listener "$p" 1 "${scratch}/listener.log" \
            >/dev/null 2>>"${scratch}/listener.log" &
        listener=$!
        wait_for_port 127.0.0.1 "$p" "$listener" "the staged listener" "${scratch}/listener.log" 15
        kill "$listener" 2>/dev/null || true
        echo "wait_for_port returned on a real listener"
        ;;

    # THE regression. `read` sets its variable and returns non-zero on a final
    # chunk with no trailing newline, so a naive loop drops it -- and the fleet
    # dashboard's JSON document is ONE line with no newline at all, so the whole
    # body vanished. One of the seven copies of `http_get` learnt that; the other
    # six never did, and are latent today only by luck about which endpoints
    # happen to end in a newline. The staged response therefore ends WITHOUT one,
    # and the assertion is on the last byte rather than on the body being
    # non-empty -- which is what a version carrying the bug would still satisfy,
    # because the headers arrive with their newlines intact.
    http-last-chunk)
        p="$(free_port)"
        _selftest_listener "$p" 0 "${scratch}/http.log" \
            >/dev/null 2>>"${scratch}/http.log" &
        listener=$!
        wait_for_port 127.0.0.1 "$p" "$listener" "the staged listener" "${scratch}/http.log" 15
        body="$(http_get 127.0.0.1 "$p" /fleet.json)"
        kill "$listener" 2>/dev/null || true
        case "$body" in
            *"200 OK"*) ;;
            *) fail "the staged listener did not answer 200: '${body}'" ;;
        esac
        case "$body" in
            *NO-TRAILING-NEWLINE*) echo "http_get kept the final chunk" ;;
            *) fail "http_get dropped the final chunk; the body was '${body}'" ;;
        esac
        ;;

    # A header the caller adds reaches the server. `http_get` takes them
    # variadically so the dashboard's Authorization and If-None-Match do not each
    # need their own parameter -- and the second one is why: the fleet page's
    # parse loop once stopped at the first header it recognised, which stayed
    # correct exactly until there were two.
    http-headers)
        p="$(free_port)"
        _selftest_listener "$p" 0 "${scratch}/hdr.log" \
            >/dev/null 2>>"${scratch}/hdr.log" &
        listener=$!
        wait_for_port 127.0.0.1 "$p" "$listener" "the staged listener" "${scratch}/hdr.log" 15
        http_get 127.0.0.1 "$p" /echo "Authorization: Bearer staged" "If-None-Match: \"etag-staged\"" >/dev/null
        kill "$listener" 2>/dev/null || true
        wait "$listener" 2>/dev/null || true
        grep -q 'Authorization: Bearer staged' "${scratch}/hdr.log" \
            || { cat "${scratch}/hdr.log" >&2; fail "the Authorization header did not reach the server"; }
        grep -q 'If-None-Match: "etag-staged"' "${scratch}/hdr.log" \
            || { cat "${scratch}/hdr.log" >&2; fail "the second header did not reach the server"; }
        echo "both caller headers reached the server"
        ;;

    # A refused connection is a RETURN, not a stop: a fixture asking whether a
    # surface is up wants to decide for itself what that means.
    http-refused)
        p="$(free_port)"
        if http_get 127.0.0.1 "$p" /healthz >/dev/null 2>&1; then
            fail "http_get succeeded against the unbound port ${p}"
        fi
        echo "http_get returned non-zero for a refused connection"
        ;;

    # --- `ask_leader` asks whoever leads NOW ---------------------------------
    #
    # `$leader_endpoint` is pinned when a section derives it, and leadership can
    # legitimately move before that section finishes. A command put to the node
    # that led a moment ago then gets "ask somebody else", and the fixture
    # reported that as the cluster refusing a legitimate command (#117, #172).
    #
    # Driven with a stubbed `cluster` because the real failure cannot be summoned:
    # it needs an election to land inside one call. Stubbing the answer places the
    # interleaving instead of waiting for it, which is the only way this fix can be
    # shown to bite at all.
    #
    # `ask_leader` binds `cluster`, `find_leader` and `$leader_endpoint` late,
    # which is exactly why it lives in the library and not in the fixture.
    ask-leader-*)
        leader_endpoint="127.0.0.1:1111"
        calls="${scratch}/calls"
        rederived="${scratch}/rederived"
        : > "$calls"

        find_leader() {
            printf '%s\n' "re-derived: $1" >> "$rederived"
            leader_endpoint="127.0.0.1:2222"
        }

        # Answers come from a queue, one per call, so a case states the sequence
        # it is exercising rather than a predicate over the argument.
        answers=()
        cluster() {
            local n
            n="$(wc -l < "$calls" | tr -d ' ')"
            printf 'x\n' >> "$calls"
            printf '%s\n' "${answers[$n]}"
        }

        case "$name" in
        ask-leader-first-answer)
            answers=("accepted: done")
            ask_leader "--cluster-set=k=v" "accepted" "should not be reported"
            echo "took the first answer, asked $(wc -l < "$calls" | tr -d ' ') time(s)"
            # An `if`, not `[ ... ] && ...`: the good path is the file being ABSENT,
            # and a bare test returning 1 under the `set -e` this harness deliberately
            # keeps would fail the case for passing.
            if [ -e "$rederived" ]; then echo "BUG: re-derived the leader when the first answer was fine"; fi
            ;;

        # THE CASE THIS TICKET EXISTS FOR. Without the retry this fails.
        ask-leader-retries)
            answers=("rejected (not-leader): this node does not lead the cluster" "accepted: done")
            ask_leader "--cluster-admit=n4=127.0.0.1:9" "accepted" "the leader refused to admit a member"
            echo "recovered after a moved leadership, asked $(wc -l < "$calls" | tr -d ' ') time(s)"
            cat "$rederived"
            ;;

        # The SECOND spelling of the same refusal. A fixture that retried on a
        # recognised "not the leader" wording would have to know both, and would
        # stop retrying the day either is reworded. This one matches neither --
        # it retries because the answer is not what the caller asserts.
        ask-leader-election)
            answers=("the cluster has no leader right now; try again shortly" "accepted: done")
            ask_leader "--cluster-forget=n3" "accepted" "the leader refused to forget a member"
            echo "recovered from an election in progress, asked $(wc -l < "$calls" | tr -d ' ') time(s)"
            ;;

        # A refusal can BE the assertion: the typo case asserts that an unknown
        # setting is refused BY NAME, so the substring is the typo. Proof that the
        # contract is "the answer carries this", never "the command succeeded".
        ask-leader-refusal-is-the-assertion)
            answers=("rejected: unknown setting 'upsteam'")
            ask_leader "--cluster-set=upsteam=typo" "upsteam" "a typo'd setting was not refused by name"
            echo "a refusal naming the typo satisfied the assertion"
            if [ -e "$rederived" ]; then echo "BUG: retried an answer that was already what the caller asserted"; fi
            ;;

        # Two chances and no more: it reports the caller's sentence and the answer.
        ask-leader-never)
            answers=("rejected (not-leader): nope" "rejected (not-leader): still nope")
            ask_leader "--cluster-admit=n4=127.0.0.1:9" "accepted" "the leader refused to admit a member"
            echo "BUG: reached the line after a failing ask_leader"
            ;;
        esac
        ;;
    *)
        echo "unknown case: ${name}" >&2
        exit 2
        ;;
    esac
}

# A listener that answers one request per connection with a body ending in NO
# newline, and records the request headers it was sent.
#
# @param 1 port
# @param 2 seconds to wait before binding -- so a caller can prove the wait
#          POLLS rather than happening to be called after the bind
# @param 3 file to record request lines in
_selftest_listener() {
    perl -e '
        use strict; use warnings; use IO::Socket::INET;
        my ($port, $delay, $logfile) = @ARGV;
        sleep $delay if $delay;
        my $srv = IO::Socket::INET->new(
            LocalAddr => "127.0.0.1", LocalPort => $port,
            Listen => 5, ReuseAddr => 1, Proto => "tcp") or die "listen: $!";
        open(my $log, ">>", $logfile) or die $!;
        $log->autoflush(1);
        while (my $c = $srv->accept()) {
            while (defined(my $l = <$c>)) { print $log $l; last if $l =~ /^\r?\n?$/; }
            print $c "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                   . "Connection: close\r\n\r\n{\"tail\":\"NO-TRAILING-NEWLINE\"}";
            close $c;
        }
    ' "$1" "$2" "$3"
}

# A stand-in for a compile node: BINDS first, then logs `compile node ready`
# after a delay -- or never, when the delay is `never`.
#
# The shape is the node's and not an approximation of it. Since #365 a node binds
# and logs that line afterwards, so a stand-in that logged first would stage the
# one ordering the helper is not about, and both cases below would pass whatever
# the helper did.
#
# It holds the port open afterwards, because `wait_for_node_ready` polls the log
# with the process still under liveness watch: a stand-in that exited once it had
# written the marker would be reported as having DIED rather than as ready, which
# is a different verdict and a passing test for the wrong reason.
#
# Perl for the listener, for the reason the other socket cases give: nc's listen
# flags differ between the BSD, GNU and OpenBSD builds and one of those is on
# every platform CI runs, but never the same one.
#
# @param 1 port
# @param 2 seconds to wait before logging the marker, or `never`
# @param 3 the log to write it to
_selftest_node() {
    perl -e '
        use strict; use warnings; use IO::Socket::INET;
        my ($port, $delay, $logfile) = @ARGV;
        my $srv = IO::Socket::INET->new(
            LocalAddr => "127.0.0.1", LocalPort => $port,
            Listen => 5, ReuseAddr => 1, Proto => "tcp") or die "listen: $!";
        if ($delay ne "never") {
            sleep $delay;
            open(my $log, ">>", $logfile) or die $!;
            print $log "compile node ready on 127.0.0.1:$port, advertising ...
";
            close $log;
        }
        sleep 30;
    ' "$1" "$2" "$3"
}

# ---------------------------------------------------------------------------
# The driver
# ---------------------------------------------------------------------------

if [ "${1:-}" = "--case" ]; then
    run_case "$2"
    exit 0
fi

failures=0
ran=0
skipped=0

# What each case must exit with and what its combined output must and must not
# say. A table rather than a function per case, so adding a branch to
# `_e2e_verdict` is adding a row.
#
# Fields are `|`-separated: name, expected exit status, then patterns. A pattern
# beginning with `!` must be ABSENT. Patterns are `grep -F` fixed strings, so a
# regular expression that quietly matches more than it should cannot creep in.
expect() {
    local record="$1" out="$2" status="$3"
    local name wanted rest pattern

    name="${record%%|*}"; record="${record#*|}"
    wanted="${record%%|*}"; rest="${record#*|}"

    if [ "$status" != "$wanted" ]; then
        echo "FAIL ${name}: exited ${status}, expected ${wanted}" >&2
        printf '%s\n' "$out" | sed 's/^/     | /' >&2
        return 1
    fi

    # Split on `|` by parameter expansion rather than by word splitting. An
    # unquoted `$rest` under `IFS='|'` would also glob, so a pattern containing a
    # `*` would quietly become whatever the working directory happens to
    # contain -- and turning globbing off for the loop leaves `set -f` behind on
    # every early `return`.
    rest="${rest}|"
    while [ -n "$rest" ]; do
        pattern="${rest%%|*}"
        rest="${rest#*|}"
        [ -n "$pattern" ] || continue

        # `grep -qF <<<` and never `printf ... | grep -q`. Under `pipefail`,
        # `grep -q` exits at its first match, the producer dies of SIGPIPE, and
        # the pipeline reports the PRODUCER's status -- so the pipeline fails
        # precisely when the pattern is present. A false negative on the success
        # path is the worst shape a check can have, and this repository has one
        # on record (`nm "$b" | grep -q __tsan_init`).
        local hit=0
        grep -qF -- "${pattern#!}" <<< "$out" || hit=1

        case "$pattern" in
            '!'*)
                if [ "$hit" -eq 0 ]; then
                    echo "FAIL ${name}: output contains '${pattern#!}' and must not" >&2
                    printf '%s\n' "$out" | sed 's/^/     | /' >&2
                    return 1
                fi
                ;;
            *)
                if [ "$hit" -ne 0 ]; then
                    echo "FAIL ${name}: output lacks '${pattern}'" >&2
                    printf '%s\n' "$out" | sed 's/^/     | /' >&2
                    return 1
                fi
                ;;
        esac
    done
    return 0
}

# --- the pure verdict ------------------------------------------------------
#
# `_e2e_verdict` takes a record and returns lines. Every branch is one row here,
# and each threshold is pinned on BOTH sides rather than demonstrated once from
# the middle -- at the boundary and one second past it -- because that is where a
# comparison mistake lives. Arguments are:
#
#     what  bound  elapsed  polls  alive  exit  logGrew  stall
#
# all durations in MEASURED seconds. With bound=20 the "recent" window is 20/4 =
# 5s, so a 5s stall is progressing and a 6s one is stalled; and the overrun note
# fires above bound + max(1, bound/10), so 22s is quiet and 23s is not.
verdicts=(
    "died|worker|20|20|98|no|3|no|-|the process DIED"
    "died-status-unknown|worker|20|20|98|no|-|no|-|exit=-"
    "no-process|worker|20|20|98|unknown|-|unknown|-|INCONCLUSIVE|No process was watched"
    "no-log|worker|20|20|98|yes|-|unknown|-|INCONCLUSIVE|no log was watched"
    "silent|worker|20|20|98|yes|-|no|-|logged NOTHING for the whole 20s"
    "progressing-at-bound|worker|20|20|98|yes|-|yes|5|still making progress"
    "stalled-one-second-past|worker|20|20|98|yes|-|yes|6|stopped making observable progress"
    "progressing-fresh|worker|20|20|98|yes|-|yes|0|still making progress"
    "stalled-cold|worker|20|20|98|yes|-|yes|20|stopped making observable progress"
    # The measured elapsed is what gets printed, never the budget.
    "reports-measured-not-nominal|worker|20|37|61|yes|-|no|-|waited 37s (measured, over 61 polls) of a 20s budget"
    # And the overrun note, pinned on both sides of bound + max(1, bound/10).
    "overrun-quiet-at-slack|worker|20|22|98|yes|-|no|-|!NOTE: the loop overran"
    "overrun-named-past-slack|worker|20|23|98|yes|-|no|-|NOTE: the loop overran its own budget by 3s"
)

echo "== the verdict, against staged records"
for row in "${verdicts[@]}"; do
    old="$IFS"
    IFS='|' read -r vname vwhat vbound vsecs vpolls valive vstatus vgrew vstall vrest <<< "$row"
    IFS="$old"
    out="$( . "$library"
            _e2e_verdict "$vwhat" "$vbound" "$vsecs" "$vpolls" "$valive" "$vstatus" "$vgrew" "$vstall" 2>&1 )"
    ran=$(( ran + 1 ))
    expect "${vname}|0|${vrest}" "$out" 0 || failures=$(( failures + 1 ))
done

# A verdict that never says BLOCKED cannot report a hang, and a table of rows
# that all pass says nothing about whether the rows differ. So assert the set of
# findings is as large as the set of branches: nine rows collapsing to two
# distinct findings would pass every row above.
distinct="$(
    for row in "${verdicts[@]}"; do
        old="$IFS"
        IFS='|' read -r vname vwhat vbound vsecs vpolls valive vstatus vgrew vstall vrest <<< "$row"
        IFS="$old"
        ( . "$library"
          _e2e_verdict "$vwhat" "$vbound" "$vsecs" "$vpolls" "$valive" "$vstatus" "$vgrew" "$vstall" 2>&1 ) \
            | grep 'FINDING:'
    done | sort -u | grep -c .
)"
if [ "$distinct" -ne 6 ]; then
    echo "FAIL verdict-branches: the records produced ${distinct} distinct findings, not 6" >&2
    failures=$(( failures + 1 ))
fi
ran=$(( ran + 1 ))

# --- the cases -------------------------------------------------------------

cases=(
    "fail-top|1|selftest FAILED: staged failure at the top level|cleanup ran|!BUG:"
    "fail-subshell|1|selftest FAILED: staged failure inside ( ... )|cleanup ran|!BUG:"
    "fail-cmdsub|1|selftest FAILED: staged failure inside \$( ... )|cleanup ran|!BUG:"
    "fail-hook|1|the on-fail hook ran|selftest FAILED: staged failure with a hook"
    "ports|0|40 distinct ports, all in range, all recorded"
    "ports-ledger|0|the ledger confined five draws to the ports it had not issued"
    "port-answers-closed|0|port_answers is false for an unbound port"
    "wait-success|0|the wait returned when the predicate became true|polls) for the staged marker"
    "wait-death-is-prompt|1|the process DIED|exit=3|of a 10s budget|!BUG:|!waited 9s|!waited 10s"
    "wait-timeout-silent|1|logged NOTHING for the whole 2s|!BUG:"
    "wait-timeout-progressing|1|still making progress|!BUG:"
    "wait-timeout-no-pid|1|No process was watched|!BUG:"
    "wait-timeout-no-log|1|no log was watched|!BUG:"
    "wait-nothing-watched|1|No process was watched|to listen on 127.0.0.1:|!BUG:"
    "wait-for-log|0|wait_for_log returned on the marker"
    "registration-accepted|0|wait_for_registration returned on the accepted round"
    "registration-refuses-zero|1|to log: 1 of 1 toolchain(s) registered|!BUG:"
    "bounded-returns-status|0|run_bounded said 'carried' with status 3"
    "bounded-missing-command|0|a missing command: outcome=unstartable|!BUG:"
    "bounded-outcome-survives-capture|0|two subshells down, the outcome reads unstartable"
    "bounded-expires|0|an expired bound exited 124, outcome exceeded"
    "bounded-124-is-not-a-timeout|0|a command exiting 124: rc=124 outcome=finished|a ceiling expiring:    rc=124 outcome=exceeded"
    "bounded-kills-the-child|0|the bound exited 124|!BUG:"
    "bounded-outlasts-a-trapped-term|0|a TERM-ignoring child exited 124"
    "ask-leader-first-answer|0|asked 1 time(s)|!BUG:"
    "ask-leader-retries|0|recovered after a moved leadership|asked 2 time(s)|re-derived: whoever leads now|!BUG:"
    "ask-leader-election|0|recovered from an election in progress|asked 2 time(s)|!BUG:"
    "ask-leader-refusal-is-the-assertion|0|a refusal naming the typo satisfied the assertion|!BUG:"
    "ask-leader-never|1|the leader refused to admit a member|still nope|!BUG:"
)

# Perl is what stages a real listener. Where it is absent those cases are
# SKIPPED and said to be skipped, by name and with a count -- never folded into
# the pass, which is the collapse this repository makes about once a session.
socket_cases=(
    "wait-for-port|0|wait_for_port returned on a real listener"
    "http-last-chunk|0|http_get kept the final chunk"
    "http-headers|0|both caller headers reached the server"
    "http-refused|0|http_get returned non-zero for a refused connection"
    "node-ready-waits-for-marker|0|wait_for_node_ready returned with the node serving|!BUG:"
    "node-ready-refuses-bound-only|1|to log: compile node ready|!BUG:"
)

echo "== the helpers, in real shells"
for record in "${cases[@]}"; do
    name="${record%%|*}"
    out="$( bash "${BASH_SOURCE[0]}" --case "$name" 2>&1 )"
    status=$?
    ran=$(( ran + 1 ))
    expect "$record" "$out" "$status" || failures=$(( failures + 1 ))
done

# --- the bound is a duration, not an iteration count -----------------------
#
# The one property in this file that has to be timed from OUTSIDE, and the
# reason it needs its own block rather than a row in the table above: a wait
# that expires ends the process, so nothing inside it can report how long it
# took, and every verdict row is a staged record that never ran a loop at all.
# Between them they cover what the wait SAYS and say nothing about how long it
# waited -- which is exactly the gap the defect lives in. Replacing the clock
# with `elapsed=$(( elapsed + 1 ))` leaves all forty other checks green.
#
# A four-second bound, required to have taken at least three seconds of wall
# clock. Under an iteration count it is four polls of 0.2s and comes back in
# under a second. The slack is one second because `SECONDS` truncates at both
# ends; the gap being measured is three seconds wide, so no truncation reaches
# across it.
echo "== the bound is a duration, not an iteration count"
clock_started="$SECONDS"
out="$( bash "${BASH_SOURCE[0]}" --case wait-clock-bound 2>&1 )"
status=$?
clock_took=$(( SECONDS - clock_started ))
ran=$(( ran + 1 ))
if [ "$status" != "1" ]; then
    echo "FAIL wait-clock-bound: exited ${status}, expected 1" >&2
    failures=$(( failures + 1 ))
elif [ "$clock_took" -lt 3 ]; then
    echo "FAIL wait-clock-bound: a 4s bound came back after ${clock_took}s of wall clock;" >&2
    echo "     the loop is counting iterations rather than reading a clock" >&2
    printf '%s\n' "$out" | sed 's/^/     | /' >&2
    failures=$(( failures + 1 ))
else
    # And the number it PRINTS is the measured one. A loop could be bounded
    # correctly and still report the budget, which is the half with teeth --
    # a timeout message stating a duration nobody measured is a fixture lying
    # to the person diagnosing it.
    reported="$(printf '%s\n' "$out" | sed -n 's/^waited \([0-9][0-9]*\)s (measured.*/\1/p')"
    if [ -z "$reported" ] || [ "$reported" -lt 3 ]; then
        echo "FAIL wait-clock-bound: it waited ${clock_took}s and reported '${reported:-nothing}'" >&2
        printf '%s\n' "$out" | sed 's/^/     | /' >&2
        failures=$(( failures + 1 ))
    fi
fi

# --- `run_bounded`'s ceiling is a ceiling ----------------------------------
#
# Timed from OUTSIDE, for the reason the block above gives, and pinned on BOTH
# sides. Only the upper bound has teeth here and it is the whole point: the child
# ignores TERM and would sleep for 30 s, so a helper that waits for it to die
# politely -- or that never escalates to KILL -- takes 30 s and passes every
# assertion inside the case. The lower bound is there so a helper that returned
# 124 immediately, without running the command at all, cannot pass either.
echo "== run_bounded's ceiling is wall clock, and it escalates"
bounded_started="$SECONDS"
out="$( bash "${BASH_SOURCE[0]}" --case bounded-outlasts-a-trapped-term 2>&1 )"
status=$?
bounded_took=$(( SECONDS - bounded_started ))
ran=$(( ran + 1 ))
if [ "$status" != "0" ]; then
    echo "FAIL bounded-clock: exited ${status}, expected 0" >&2
    printf '%s\n' "$out" | sed 's/^/     | /' >&2
    failures=$(( failures + 1 ))
elif [ "$bounded_took" -lt 1 ]; then
    echo "FAIL bounded-clock: a 1s bound over a 30s child returned in ${bounded_took}s;" >&2
    echo "     the command cannot have been run" >&2
    failures=$(( failures + 1 ))
elif [ "$bounded_took" -gt 10 ]; then
    echo "FAIL bounded-clock: a 1s bound over a TERM-ignoring child took ${bounded_took}s;" >&2
    echo "     the bound is waiting for a child that will not die, which is an" >&2
    echo "     unbounded wait inside the thing that exists to bound one" >&2
    failures=$(( failures + 1 ))
fi

# --- `run_bounded` is not slower than what it bounds ------------------------
#
# The bound's cadence, timed from outside because nothing inside ten immediate
# commands can see the pauses between them. Measured, ten `true`s: 129 ms with
# the ramp against 2046 ms with a flat `_e2e_poll_pause`. The measurement, not the
# constant, is what this pins -- a future pause raised "just a little" is exactly
# how the 74 healthy probes became twenty seconds of sleeping.
echo "== run_bounded does not sleep away the fast path"
fast_started="$SECONDS"
out="$( bash "${BASH_SOURCE[0]}" --case bounded-fast-path 2>&1 )"
status=$?
fast_took=$(( SECONDS - fast_started ))
ran=$(( ran + 1 ))
if [ "$status" != "0" ]; then
    echo "FAIL bounded-fast-path: exited ${status}, expected 0" >&2
    printf '%s\n' "$out" | sed 's/^/     | /' >&2
    failures=$(( failures + 1 ))
elif [ "$fast_took" -gt 2 ]; then
    echo "FAIL bounded-fast-path: twenty immediate commands took ${fast_took}s;" >&2
    echo "     the bound is sleeping through commands that have already finished" >&2
    failures=$(( failures + 1 ))
fi

# --- no fixture spells `timeout` again -------------------------------------
#
# `run_bounded` above is not only a helper, it is this check's subject. macOS has
# neither `timeout(1)` nor `gtimeout` -- GitHub's `macos-14` image carries no
# Homebrew `coreutils` -- so a fixture reaching for either gets `command not
# found`, status 127, on the one platform nobody here can run it on. That
# happened, in the fix for #457 itself, and the fixture then reported a confident
# wrong finding about consensus.
#
# `tsan-gate.sh` had the correct paragraph about this in its own header and it
# did not travel to the next script that needed it -- which is why this is a scan
# and not a fourth comment. The allowlist carries a REASON per row rather than a
# bare path, so an exemption cannot be added silently.
echo "== no fixture invokes timeout(1)"

# What counts as an invocation. A COMMAND POSITION, not the word: `--drain-timeout=`,
# `DialTimeout`, `wait-timeout-silent` and `echo "timeout ${endpoint}"` are all
# ordinary here and none of them runs anything. Command position is the start of a
# line, or after a separator (`;` `|` `&` `(` backtick `$(`), or after one of the
# keywords that can precede a command -- and that keyword set is the half the first
# version of this scan omitted, so `if timeout 5 x` and `while timeout 5 x` went
# unseen.
#
# What it does NOT catch, said plainly rather than left to be discovered: a command
# reached through a variable (`"$TimeoutCommand"`). No regex over the word `timeout`
# can, and that shape is a deliberate resolver rather than an accident -- which is
# what the allowlist is for.
_timeout_invocations() {
    grep -nE '(^|[;&|(`]|\$\(|&&|\|\|)[[:space:]]*(if|then|else|elif|while|until|do|!|\{)?[[:space:]]*g?timeout[[:space:]]' "$1" \
        | grep -v '^[0-9][0-9]*: *#' || true
}

# The canary. A scan that has never been seen to fire is a scan reporting PASS over
# a set in which nothing could fail -- `.agent/rules/build-and-toolchain.md` on
# `script-check-canary`. Both directions, because a pattern that matches everything
# would pass the positive half alone.
canary_dir="$(mktemp -d)"
cat > "${canary_dir}/must-catch.sh" <<'CANARY'
timeout 5 foo
out="$(timeout 5 foo)"
if timeout 5 foo; then :; fi
while timeout 5 foo; do :; done
! timeout 5 foo
x && timeout 5 foo
y; gtimeout 5 foo
CANARY
cat > "${canary_dir}/must-not-catch.sh" <<'CANARY'
stated_drain="--drain-timeout=${worker_drain_seconds}"
readonly DialTimeout=10
echo "timeout ${endpoint}" >> "$probe_log"
cases=("wait-timeout-silent|1|logged NOTHING")
# timeout 5 foo
TargetTimeoutSeconds=900
CANARY
ran=$(( ran + 1 ))
caught="$(_timeout_invocations "${canary_dir}/must-catch.sh" | grep -c . || true)"
if [ "$caught" -ne 7 ]; then
    echo "FAIL timeout-scan-canary: the scan caught ${caught} of 7 staged invocations," >&2
    echo "     so it cannot be trusted to have found none in the real scripts" >&2
    _timeout_invocations "${canary_dir}/must-catch.sh" | sed 's/^/     | /' >&2
    failures=$(( failures + 1 ))
fi
ran=$(( ran + 1 ))
spurious="$(_timeout_invocations "${canary_dir}/must-not-catch.sh" || true)"
if [ -n "$spurious" ]; then
    echo "FAIL timeout-scan-canary: the scan fired on text that runs nothing" >&2
    printf '%s\n' "$spurious" | sed 's/^/     | /' >&2
    failures=$(( failures + 1 ))
fi
rm -rf "$canary_dir"

# Is this script exempt from a scan, per that scan's allowlist?
#
# One row per exemption, `basename:reason`, matched PER ROW. A single scalar could
# hold only ONE row however many were appended to it -- a second exemption would
# silently fail to exempt and the check would go red for a file its author believed
# was allowlisted. Written once because both scans in this file need it, and a
# subtlety that has to be re-explained beside each copy is a subtlety one copy will
# eventually be missing.
#
# @param 1 the script's basename
# @param 2 the allowlist text, one `basename:reason` per line
# @return 0 when exempt
_scan_exempt() {
    local base="$1" row=""
    while IFS= read -r row; do
        [ -n "$row" ] || continue
        case "$row" in "${base}:"*) return 0 ;; esac
    done <<EOF
$2
EOF
    return 1
}

timeout_allowed="tsan-gate.sh:not an e2e fixture, and it resolves timeout/gtimeout itself rather than assuming one. Its clang-tsan job is runs-on: ubuntu-24.04, so its macOS branch has never executed anywhere.
check-e2e-helpers.sh:this file, which stages the scan's own canary invocations above. They are heredoc text and run nothing; the canary asserting all seven are caught is what covers them."
for script in "${source_dir}"/scripts/*.sh; do
    base="$(basename "$script")"
    _scan_exempt "$base" "$timeout_allowed" && continue
    ran=$(( ran + 1 ))
    hits="$(_timeout_invocations "$script")"
    if [ -n "$hits" ]; then
        echo "FAIL timeout-scan: ${base} invokes timeout(1), which macOS does not have." >&2
        echo "     Use run_bounded from scripts/lib/e2e-common.sh." >&2
        printf '%s\n' "$hits" | sed 's/^/     | /' >&2
        failures=$(( failures + 1 ))
    fi
done

# --- no script keeps its own copy of a shared helper ------------------------
#
# The other half of #449, and the half a conversion cannot enforce on its own.
# Folding seven copies into one file says nothing about the EIGHTH, which is
# written by somebody who has never opened this file. `dist-compile-e2e.sh` WAS
# that eighth for two tickets -- #449 deferred it, #451 finished it -- and while
# it waited, its private `http_get` went on dropping a body with no trailing
# newline that another copy had already learnt to keep.
#
# What is scanned is a NAME COLLISION: a `scripts/*.sh` that defines a function
# the library already defines has a second implementation of it, whatever the
# body says. That is exact in one direction and blind in the other, and the
# blindness is said here rather than left to be found: a helper reimplemented
# under a DIFFERENT name is invisible to this. `migrate-storage-e2e.sh` spells
# `free_port` as `port`, drawing from 40000-59999 -- entirely inside Linux's
# default ephemeral range, which is the `bind(...) failed: 98` the shared one
# moved to 20000-31999 to avoid (#628). No regex over shell can see that, which
# is what the allowlist's per-row reasons are for.
#
# The names are READ from the library rather than restated here. A second copy of
# the list is not a cross-check, it is a second thing to be wrong -- and wrong in
# the silent direction, because a helper ADDED to the library would simply never
# be scanned for.
echo "== no script keeps its own copy of a shared helper"

# Every function the library defines, at column zero. The nested predicates
# inside `wait_for_port` and `wait_for_log` are indented and are deliberately not
# in this set: they are locals in all but name, and a fixture will not collide
# with one by accident.
_library_helper_names() {
    grep -oE '^[a-zA-Z_][a-zA-Z0-9_]*\(\)' "$library" | sed 's/()$//'
}

# Definitions of those names in one script, at ANY indentation. A fixture's
# helpers are not all at column zero -- `dist-compile-e2e.sh` defines two inside
# its `--case membership` block -- so anchoring at column zero would read a
# nested copy as absent, which is the direction that fails silently.
#
# WHOLE-FILE FIRST, because this test is in the DEFAULT set and runs on every
# platform CI builds. The per-name loop is one `grep` plus one `sed` per name per
# script -- 20 names over 21 scripts is 840 processes -- and measured on Linux
# that is 0.90 s against 0.06 s with the line below, about 45% of the CPU this
# whole test burns. `check-worker-refusals-counted.cmake` made the same trade for
# the same reason and records the same argument.
#
# Unlike the usual cheap prefilter this one is EXACT in both directions, and that
# is worth stating because the next reader will assume it is lossy: the filter is
# the literal disjunction of the twenty per-name regexes, every name is
# `[A-Za-z0-9_]` only with no metacharacter to widen it, and the trailing `\(\)`
# is in both. So it can produce neither a false negative nor a false positive.
# Today nothing matches, so the loop never runs at all; it costs something only on
# the run that is about to fail, which is exactly when the per-name prefix earns
# its keep.
_helper_redefinitions() {
    grep -qE "^[[:space:]]*(${helper_alternation})\(\)" "$1" || return 0
    local script="$1" name=""
    for name in $helper_names; do
        grep -nE "^[[:space:]]*${name}\(\)" "$script" | sed "s/^/${name}: /" || true
    done
}

helper_names="$(_library_helper_names)"
helper_alternation="$(printf '%s\n' "$helper_names" | tr '\n' '|' | sed 's/|$//')"

# Both scans, asserted non-empty before either verdict is read. Two empty lists
# agree perfectly: a `_library_helper_names` that matched nothing would report
# every script clean, and so would a glob that found no scripts.
ran=$(( ran + 1 ))
helper_name_count="$(printf '%s\n' "$helper_names" | grep -c . || true)"
if [ "$helper_name_count" -lt 1 ]; then
    echo "FAIL helper-scan: read ${helper_name_count} function names out of ${library}." >&2
    echo "     With no names there is nothing to scan for and every script reads clean." >&2
    failures=$(( failures + 1 ))
fi

# The canary, both directions. A scan that has never been seen to fire is a scan
# reporting PASS over a set in which nothing could fail.
canary_dir="$(mktemp -d)"
# Written with `printf` and not a heredoc, which is the one place this scan
# differs from the `timeout` one above. Heredoc text is still text in THIS file,
# and this scan reads whole scripts rather than command positions -- so a staged
# `fail() { ... }` at column zero is a genuine hit against `check-e2e-helpers.sh`
# itself. Observed: the first run of this block failed on its own canary.
#
# The `timeout` scan answers that with an allowlist row for this file, and that
# row costs it the ability to see a real invocation here. This one does not need
# to pay that: every line below begins with `printf` in the source, so there is
# no definition here to find and no blind spot to exempt.
{
    printf '%s\n' 'fail() { echo "a private copy"; exit 1; }'
    printf '%s\n' '    wait_for_port() { :; }'
} > "${canary_dir}/must-catch.sh"
# The negative half stays a heredoc, because none of it is a definition of a
# library name -- that being exactly what it is staged to demonstrate.
cat > "${canary_dir}/must-not-catch.sh" <<'CANARY'
. "$(dirname "$0")/lib/e2e-common.sh"
dash_get() { http_get 127.0.0.1 "$1" "$2"; }
probe_hostname() { hostname -I 2>/dev/null; }
# fail() { }
CANARY
ran=$(( ran + 1 ))
caught="$(_helper_redefinitions "${canary_dir}/must-catch.sh" | grep -c . || true)"
if [ "$caught" -ne 2 ]; then
    echo "FAIL helper-scan-canary: the scan caught ${caught} of 2 staged copies," >&2
    echo "     so it cannot be trusted to have found none in the real scripts" >&2
    _helper_redefinitions "${canary_dir}/must-catch.sh" | sed 's/^/     | /' >&2
    failures=$(( failures + 1 ))
fi
ran=$(( ran + 1 ))
spurious="$(_helper_redefinitions "${canary_dir}/must-not-catch.sh")"
if [ -n "$spurious" ]; then
    echo "FAIL helper-scan-canary: the scan fired on a script that defines no copy" >&2
    printf '%s\n' "$spurious" | sed 's/^/     | /' >&2
    failures=$(( failures + 1 ))
fi
rm -rf "$canary_dir"

# The same `_scan_exempt` the timeout scan above uses. Each row names why, and the
# two that are defects name the ISSUE, so an exclusion cannot rot into folklore and
# closing the ticket has an obvious row to delete.
helper_copy_allowed="local-gate.sh:not an e2e fixture. It sources nothing, starts no daemon and opens no socket; its 'fail' prints a build-gate verdict and its own selftest (local-gate-selftest) is what covers it.
launcher-replay-e2e.sh:#627. Its 'fail' carries the '[ \"\${BASHPID:-\$\$}\" = \"\$top_pid\" ]' guard the bash-3.2 table below bans by name -- correct on bash 4, silently inert on macOS 3.2, where a 'fail' inside ( ... ) then ends only the subshell.
migrate-storage-e2e.sh:#628. Its 'fail' is the ordinary private copy; the sharper defect is its 'port', which this NAME scan cannot see -- see the header above."
scanned=0
for script in "${source_dir}"/scripts/*.sh; do
    base="$(basename "$script")"
    _scan_exempt "$base" "$helper_copy_allowed" && continue
    scanned=$(( scanned + 1 ))
    ran=$(( ran + 1 ))
    hits="$(_helper_redefinitions "$script")"
    if [ -n "$hits" ]; then
        echo "FAIL helper-scan: ${base} defines its own copy of a shared helper." >&2
        echo "     Source scripts/lib/e2e-common.sh and delete the copy (#449, #451)." >&2
        printf '%s\n' "$hits" | sed 's/^/     | /' >&2
        failures=$(( failures + 1 ))
    fi
done
ran=$(( ran + 1 ))
if [ "$scanned" -lt 1 ]; then
    echo "FAIL helper-scan: the glob matched no scripts, so every one of them 'passed'." >&2
    failures=$(( failures + 1 ))
fi

if command -v perl >/dev/null 2>&1 && perl -MIO::Socket::INET -e1 >/dev/null 2>&1; then
    echo "== the helpers, against a real listener"
    for record in "${socket_cases[@]}"; do
        name="${record%%|*}"
        out="$( bash "${BASH_SOURCE[0]}" --case "$name" 2>&1 )"
        status=$?
        ran=$(( ran + 1 ))
        expect "$record" "$out" "$status" || failures=$(( failures + 1 ))
    done
else
    for record in "${socket_cases[@]}"; do
        echo "SKIPPED ${record%%|*}: perl with IO::Socket::INET is not available to stage a listener" >&2
        skipped=$(( skipped + 1 ))
    done
fi

# --- bash 3.2 --------------------------------------------------------------
#
# macOS ships a 2007 `/bin/bash` and these fixtures run on every platform CI
# builds, so a construct newer than that is a fixture that does not start there.
# Scanned rather than remembered: the constraint was already written down in
# `coverage.sh`'s comments, where nobody writing a new script would find it.
#
# `BASHPID` is in the table because it is the trap that looks like the fix. A
# `[ "${BASHPID:-$$}" = "$top_pid" ]` guard is CORRECT on bash 4 and silently
# inert on 3.2, where BASHPID is unset and the test reduces to comparing `$$`
# with itself -- so the subshell defect it was written to close is closed on
# Linux and open on macOS. `fail` avoids the question entirely.
#
# And this row is LOAD-BEARING rather than belt-and-braces, which is only visible
# from having tried it: staging that guard back into `fail` and running this file
# on a bash 5 runner leaves `fail-subshell` GREEN, because on bash 5 the guard
# works. The behavioural case cannot see a 3.2-only defect from a 4-or-later
# shell, and every machine this is developed and tested on is a 4-or-later shell.
# The scan is the only check here that can.
echo "== bash 3.2 constructs"
banned=(
    "mapfile:reads into an array; bash 4.0+"
    "readarray:the same builtin under its other name; bash 4.0+"
    "declare -A:associative arrays; bash 4.0+"
    "local -n:name references; bash 4.3+"
    "BASHPID:bash 4.0+, and unset on macOS -- a guard using it is silently inert there"
    "[[ -v :bash 4.2+"
    "^^}:case modification; bash 4.0+"
    ",,}:case modification; bash 4.0+"
)
for entry in "${banned[@]}"; do
    token="${entry%%:*}"
    why="${entry#*:}"
    # Comment lines are excluded, because the header explains WHY several of
    # these are banned and a scan that fails on its own rationale is a scan
    # nobody can write the rationale for. Indented comments too: the reason for
    # `BASHPID` is inside `fail`.
    hits="$(grep -n -F -- "$token" "$library" | grep -v '^[0-9][0-9]*: *#' || true)"
    ran=$(( ran + 1 ))
    if [ -n "$hits" ]; then
        echo "FAIL bash32: ${library} uses '${token}' (${why})" >&2
        printf '%s\n' "$hits" | sed 's/^/     | /' >&2
        failures=$(( failures + 1 ))
    fi
done

# And the library must not be executable or carry a `#!`: it is sourced, and a
# copy that looks runnable invites someone to run it, which does nothing and
# says nothing.
ran=$(( ran + 1 ))
first_line="$(head -1 "$library")"
case "$first_line" in
    '#!'*)
        echo "FAIL shebang: ${library} is sourced, not executed, and must carry no '#!' line" >&2
        failures=$(( failures + 1 ))
        ;;
esac

# ---------------------------------------------------------------------------

echo
echo "e2e-helpers-selftest: ${ran} checks ran, ${failures} failed, ${skipped} skipped"
if [ "$skipped" -gt 0 ]; then
    echo "  (a skip is not a pass: the ${skipped} skipped checks were not run at all)"
fi
[ "$failures" -eq 0 ] || exit 1
exit 0
