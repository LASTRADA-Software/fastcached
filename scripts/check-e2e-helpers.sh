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
    # The scratch directory AND the processes this case started. It was
    # `rm -rf "$scratch"` alone, which reaped nothing at all: `_selftest_listener`
    # below is an unbounded `accept()` loop, so every case that backgrounded one
    # stranded an immortal `perl` holding a 127.0.0.1 LISTEN socket for the life
    # of the machine. Measured on one dev host before this fix: 1368 orphans
    # holding 1444 loopback sockets, the oldest 30.5 hours old, and three more
    # from every `ctest -R e2e-helpers-selftest` (#839).
    #
    # The load is the visible half and the cheap half. The PORTS are the
    # expensive one. `free_port` draws by asking whether anything is listening,
    # so a held LISTEN socket is precisely what its probe refuses -- in growing
    # numbers, forever, on a machine several lanes share. The failure that
    # produces surfaces in whatever OTHER fixture next draws a port, which is the
    # worst shape available: nothing points back here. Two lanes had already
    # misread it, one as CPU contention and one as an unreproducible flake.
    #
    # `jobs -pr`, and not a pid ledger the call sites append to -- for the reason
    # `note_failure` below is a function rather than a convention. A ledger is
    # per-site, so the next background site reopens this by forgetting one line,
    # and #834 was already writing that site while this was being fixed. There is
    # nothing here to forget: every still-RUNNING background job of this shell is
    # reaped, whatever started it, on whichever path the case left by -- the path
    # the ticket names, `wait_for_port` calling `fail` before its `kill`,
    # included. That the trap runs on the `fail` paths at all is not assumed:
    # the three `fail-*` rows in the table below already assert `cleanup ran`.
    #
    # This is worth nothing without the `exec` on each helper below. `$!` for a
    # backgrounded FUNCTION is the subshell bash forks to run it, not the program
    # that subshell then runs, so reaping the job pid killed the wrapper and left
    # the `perl` -- which is also why the three `kill "$listener"` calls already
    # in this file never worked. Both halves or neither.
    #
    # `-r` rather than a bare `-p`. Bash reaps a finished job internally while
    # keeping it in the jobs table, so a finished job pid is free to have been
    # recycled and signalling it would hit a process belonging to somebody else
    # -- the `pgrep -f` misattribution lesson arriving through another door. Only
    # a RUNNING job is still ours to signal.
    #
    # Not `kill 0`: a case runs as `bash "$0" --case NAME` from a command
    # substitution in the driver, with no job control, so it shares the DRIVER
    # process group and `kill 0` would take the whole run down with it.
    #
    # It signals and does NOT wait, which is deliberate and is the one thing a
    # reader is likely to want to "fix". `stop_and_require_exit` in the library
    # is the richer recipe (TERM, poll, KILL, wait) and is wrong in a trap twice
    # over: it ends in `fail`, which does `kill -TERM $_e2e_top_pid; exit 1`
    # re-entrantly inside the EXIT trap already firing, and a bare `wait` here
    # would be an UNBOUNDED wait inside cleanup -- `bounded-outlasts-a-trapped-term`
    # below stages a child that ignores TERM on purpose, which is exactly the
    # process such a wait would hang on. Escalation is unnecessary for the same
    # reason it would be needed: these helpers install no TERM handler, and the
    # `alarm` below is the backstop for anything that outlives the signal.
    #
    # bash 3.2: `jobs -pr` is in that shell, and this is a plain `for` over word
    # splitting -- no `mapfile`, no arrays, no `wait -n`.
    cleanup() {
        echo "cleanup ran"
        for leftover in $(jobs -pr); do kill "$leftover" 2>/dev/null || true; done
        rm -rf "$scratch"
    }
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
    # WHAT PINS THE MARKER, precisely, because the obvious sentence here would be
    # wrong. These two cases stage ONE text, present against absent -- unlike
    # `registration-accepted` above, which stages `1 of 1` against `0 of 1` and so
    # catches a wait that matched the wrong line. A second spelling of the text
    # would not weaken this pair.
    #
    # So the constant is pinned by having READERS, not by being unspoken: the
    # assertion below matches `$E2eNodeReadyMarker` itself, and
    # `refuses-bound-only`'s table row requires the expiry message `wait_for_log`
    # builds FROM it. Reword the line in the library and both fail.
    #
    # `_selftest_node` spells the text literally and is not a third reader -- it is
    # staging what a real node writes, which is the same thing
    # `registration-accepted` does with its log line. Staging the product's output
    # and asserting the helper's behaviour are different jobs.
    node-ready-waits-for-marker)
        log="${scratch}/ready.log"
        : > "$log"
        p="$(free_port)"
        # stderr into the file the wait is told to dump, as the three socket cases
        # above do and for their reason: a stand-in that cannot bind otherwise
        # arrives as an unexplained death beside an EMPTY log, on a runner nobody
        # can reach. stdout is closed off, being the staged listener's own chatter.
        _selftest_node "$p" 1 "$log" >/dev/null 2>>"$log" &
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
        #
        # Through `$E2eNodeReadyMarker` and not the text, which is the whole reason
        # that constant exists: an assertion that spelled the marker a second time
        # would agree with itself whatever the helper does, and a marker changed in
        # the library would leave this case passing against a string nothing logs.
        case "$(<"$log")" in
            *"$E2eNodeReadyMarker"*) echo "wait_for_node_ready returned with the node serving" ;;
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
        _selftest_node "$p" never "$log" >/dev/null 2>>"$log" &
        staged=$!
        # Staged for the reason the case above gives, and here it is also what
        # makes the budget mean what it says: the 2s below must be spent waiting
        # for a MARKER that never comes, not waiting for perl to bind. The
        # required text in the table (`to log: ...`) is what pins that -- an
        # expiry in the port wait says `to listen on` and fails the row, so this
        # cannot quietly go back to timing the wrong thing.
        wait_for_port 127.0.0.1 "$p" "$staged" "a node that never finishes starting" "$log" 15
        # ONE second, not two. This case runs in the DEFAULT ctest set on every
        # platform CI builds, and the budget below is the one number here that is
        # spent in full by design rather than being a ceiling nothing reaches --
        # so a second of it is a second on every run everywhere. What makes the
        # case discriminate is the staged bind above, not the size of this: with
        # the port already bound and held, the only expiry reachable is the log
        # one, and its verdict text is identical at either value.
        wait_for_node_ready 127.0.0.1 "$p" "$staged" "a node that never finishes starting" "$log" 1
        echo "BUG: the wait returned for a node that had only bound"
        ;;

    # And the THIRD verdict, because the helper's header claims there are two and
    # the pair above cannot show it. Both of those stage the bind first, so a
    # `wait_for_node_ready` reduced to its marker wait alone would pass them both
    # -- exit 0 with the marker present, exit 1 with `to log:` in the output. The
    # claim that "a stall before the bind and a stall between the bind and
    # readiness are different faults, and the two remain two verdicts" had no
    # reader until this row.
    #
    # A process that is ALIVE and binds nothing at all: the wait must expire in
    # the PORT leg and say `to listen on`, not `to log`. That is the same
    # both-directions discipline the marker rows use, applied to the predicates
    # rather than to the text.
    node-ready-refuses-unbound)
        log="${scratch}/unbound.log"
        : > "$log"
        p="$(free_port)"
        # No listener, deliberately. `sleep` is the whole stand-in: the wait needs
        # a live pid so it reports a timeout rather than a death, and nothing is
        # supposed to answer the port.
        sleep 30 >/dev/null 2>&1 &
        staged=$!
        wait_for_node_ready 127.0.0.1 "$p" "$staged" "a node that never binds" "$log" 1
        echo "BUG: the wait returned for a node that never bound"
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
    #
    # `exec`, so the process that ignores TERM is the `sleep` ITSELF and the
    # stand-in is ONE process deep -- which is what `run_bounded` documents it
    # signals ("a caller that would not must not use this"), and the only depth
    # `cleanup` above can see. A forked `sleep` is a job of nobody: `run_bounded`
    # KILLs the `sh` alone, so the `sleep` reparents and outlives the run by up to
    # thirty seconds, and it is not in this shell's jobs table for the reap to
    # find. Smaller than #839 -- it holds no port and it does end -- but the same
    # shape, inside the change that claims to have closed it.
    #
    # STATED, not measured everywhere it matters. MEASURED here: `/bin/sh` is
    # bash on this host, and bash already execs the last command of a `-c` list,
    # so the fork was not happening and this changes nothing observable. NOT
    # measured: dash and the BSD shells, which are what `/bin/sh` is on the other
    # platforms CI builds. The `exec` is what makes the property hold without
    # depending on that optimisation being present.
    #
    # An ignored disposition is INHERITED across exec -- the rule
    # `.agent/rules/wire-and-protocol.md` states for SIGPIPE, arriving here -- so
    # the child still ignores TERM and this row still discriminates: a helper that
    # waited politely still takes thirty seconds.
    bounded-outlasts-a-trapped-term)
        rc=0
        run_bounded 1 sh -c 'trap "" TERM; exec sleep 30' >/dev/null || rc=$?
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

    # The silence probe brings back what a server volunteered without being asked.
    #
    # Staged against a listener that SPEAKS FIRST, because the interesting failure
    # is a probe that reads nothing whatever the server does -- which would look
    # identical to the property `fleet-dashboard-e2e` asserts, and would then pass
    # on a broken server forever. A listener that says nothing cannot tell those
    # apart; one that speaks can.
    http-silence)
        p="$(free_port)"
        _selftest_unprompted_listener "$p" answer "${scratch}/silence.log" \
            >/dev/null 2>>"${scratch}/silence.log" &
        listener=$!
        wait_for_port 127.0.0.1 "$p" "$listener" "the staged listener" "${scratch}/silence.log" 15
        body="$(http_response_to_silence 127.0.0.1 "$p")"
        kill "$listener" 2>/dev/null || true
        case "$body" in
            *UNASKED*) echo "http_response_to_silence reported what the server volunteered" ;;
            *) fail "http_response_to_silence brought back nothing from a server that spoke: '${body}'" ;;
        esac
        ;;

    # ... and REFUSES when its own bound is what ended the read.
    #
    # The arm that would otherwise never be watched, and the one that matters: an
    # expired `read -t` yields an empty body, which is byte-identical to "the server
    # said nothing" -- the very answer the probe exists to report. Against a listener
    # that accepts and then neither speaks nor closes, a probe without this arm
    # reports a clean pass for a question it could not answer. Costs the bound.
    http-silence-inconclusive)
        p="$(free_port)"
        _selftest_unprompted_listener "$p" hold "${scratch}/hold.log" \
            >/dev/null 2>>"${scratch}/hold.log" &
        listener=$!
        wait_for_port 127.0.0.1 "$p" "$listener" "the staged listener" "${scratch}/hold.log" 15
        http_response_to_silence 127.0.0.1 "$p" >/dev/null && rc=0 || rc=$?
        kill "$listener" 2>/dev/null || true
        # The STATUS, not merely non-zero. A refused connection is also non-zero, so
        # "it returned an error" would score this green against a listener that had
        # died -- the arm passing for the reason it exists to refuse.
        [ "$rc" -eq 1 ] \
            || fail "expected status 1 (the bound expired) from a server that never spoke or closed; got ${rc}"
        echo "http_response_to_silence refused rather than reporting silence it never observed"
        ;;

    # ... and says REFUSED apart from INCONCLUSIVE.
    #
    # The two are one non-zero unless something asserts otherwise, and the fixture
    # prints a different sentence for each -- a node that died and a surface that
    # would not answer are fixed by different people.
    http-silence-refused)
        p="$(free_port)"
        http_response_to_silence 127.0.0.1 "$p" >/dev/null 2>&1 && rc=0 || rc=$?
        [ "$rc" -eq 2 ] || fail "expected status 2 (refused) against an unbound port; got ${rc}"
        echo "http_response_to_silence told a refused connection from an expired bound"
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
    # `exec` for the reason `cleanup` above gives: without it `$!` is the subshell
    # bash forks for a backgrounded function, so the three `kill "$listener"`
    # calls below signalled a wrapper and left this perl reparented to init with
    # its listening socket still open (#839).
    #
    # Only ever called with `&`. In the FOREGROUND this would replace the calling
    # shell, so a new call site backgrounds it or does not use it.
    #
    # That sentence is the ONLY guard, and it is worth saying why rather than
    # leaving the next reader to wonder whether one was forgotten. Enforcing it
    # means asking "am I in a subshell", which needs `BASHPID` -- bash 4.0+, and
    # UNSET on the macOS 3.2 this script also runs under, so the check would hold
    # on Linux and be silently inert on the platform it matters on. That is the
    # exact trap this file already records in its own bash 3.2 table, where
    # `BASHPID` is a banned construct for the same reason: a guard that cannot
    # fire everywhere is worse than a comment, because it reads as enforcement.
    # A rule nothing can express is a rule nothing can be held to, so this one is
    # written down instead of pretended at.
    exec perl -e '
        use strict; use warnings; use IO::Socket::INET;
        my ($port, $delay, $logfile) = @ARGV;
        sleep $delay if $delay;
        # A self-imposed lifetime -- the belt-and-braces half of #839, and the
        # same shape `_selftest_node` below already has in its trailing
        # `sleep 30`. `cleanup` reaps this process on every path a case can take,
        # but a trap does not run on SIGKILL, and this fixture is in the DEFAULT
        # ctest set: it therefore also runs under `ctest --timeout`, under every
        # cancelled CI job and under any `kill -9`, on every platform CI builds,
        # and on those paths nothing whatever reaps it.
        #
        # A TIME bound and not a connection count. `port_answers` is `/dev/tcp`,
        # so every `free_port` draw and every `wait_for_port` poll costs this
        # loop an `accept()`; a loop bounded by connections would exit before the
        # request under test arrived, and the case would then fail for a reason
        # having nothing to do with the helper it exists to test.
        #
        # Armed AFTER the delay and never across it: the perl `sleep` builtin may
        # be implemented with `alarm` on some systems, and the two must not
        # overlap. There is deliberately no handler -- the default disposition of
        # SIGALRM terminates, and that is what interrupts the blocking `accept()`.
        #
        # NOTE: this program is a shell single-quoted string, so no apostrophe
        # may appear anywhere inside it, comments included. One here ended the
        # quote and the script died at PARSE time -- which leaks nothing, and is
        # therefore indistinguishable, by a count of survivors alone, from the
        # fix working -- which is the argument this file makes in its own
        # header, arriving from inside the file.
        alarm 30;
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

# A listener that answers a client which has said NOTHING, or holds it open.
#
# `_selftest_listener` above reads a request line before it answers, so it cannot
# stage either half of `http_response_to_silence`'s contract. The two modes are the
# probe's two outcomes: `answer` is a server that volunteers a response (which is
# exactly the #824 defect), `hold` is one that neither speaks nor closes (which is
# the state the probe must REFUSE to report on rather than read as silence).
#
# Perl for the listener, for the reason the other stand-ins give: nc's listen flags
# differ between the BSD, GNU and OpenBSD builds, and one of those is on every
# platform CI runs but never the same one.
#
# @param 1 port
# @param 2 `answer` to speak first and close, `hold` to accept and do neither
# @param 3 the log to write failures to
#
# ## Two lifetime bounds, and neither closes the other's hole
#
# **`exec`, because `$!` for a backgrounded shell FUNCTION is the subshell bash
# forks, not the program that subshell goes on to run.** Every `kill "$listener"` in
# this file therefore reaped a wrapper and left `perl` alive, reparented, still
# holding its LISTEN socket. Measured: `$! comm=bash` with a `perl` child, and the
# port still held after the kill. `exec` replaces the subshell, so the pid the caller
# holds IS the perl. Safe ONLY because these helpers are always invoked with `&` --
# in the foreground `exec` would replace the calling shell and end the run.
#
# **`alarm`, because no trap runs under `SIGKILL`, a `ctest --timeout` or a cancelled
# CI job**, which are the paths a leak actually accumulates on. A bound on TIME and
# deliberately not on connection count: `port_answers` is `/dev/tcp`, so every
# `free_port` draw and every `wait_for_port` poll costs this listener an `accept()`,
# and a count bound would kill it before the request under test ever arrived.
#
# **They are independent, and a survivor COUNT cannot tell you whether either works**
# -- each alone drives it to zero, for a different reason, so a count reads as "both
# arms fine" while one is dead. Ask the process TREE. (#839)
_selftest_unprompted_listener() {
    exec perl -e '
        use strict; use warnings; use IO::Socket::INET;
        my ($port, $mode, $logfile) = @ARGV;
        # Outlives any case here; dies without one whatever killed the run.
        alarm 30;
        my $srv = IO::Socket::INET->new(
            LocalAddr => "127.0.0.1", LocalPort => $port,
            Listen => 5, ReuseAddr => 1, Proto => "tcp") or die "listen: $!";
        open(my $log, ">>", $logfile) or die $!;
        $log->autoflush(1);
        my @held;
        while (my $c = $srv->accept()) {
            if ($mode eq "answer") {
                print $c "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\nUNASKED";
                close $c;
            } else {
                # Kept in scope on purpose: letting it fall out of scope would close
                # it, which is the OTHER outcome and would stage the wrong case.
                push @held, $c;
            }
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
    # `exec` for the reason `_selftest_listener` above gives: `$!` must be this
    # perl and not the subshell bash forks for a backgrounded function, or the
    # `kill "$staged"` at the call site signals a wrapper and leaves this process
    # holding its port for the full 30 seconds below. Only ever called with `&`.
    exec perl -e '
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
# The NAMES of the checks that failed, so the summary can say which.
failed_cases=""

# Record one failed check by name.
#
# The count and the name used to live in different places: every site echoed
# `FAIL <case>: ...` to stderr and then incremented `failures` by hand, and the
# summary printed the counter alone. So the LAST line of a failing run -- the line
# that survives a truncated capture, the line a reader sees first, and the only one
# left once later runs have overwritten ctest's single `LastTest.log` -- said
# `1 failed` and named nothing (#678). Recovering the case then meant reproducing a
# rare failure and hoping.
#
# That is this repository's own rule about counts, broken inside the check that
# guards the rulebook: a count cannot say which of 119 things happened, exactly as
# `25 of 26 green` is arithmetic that is true and useless.
#
# One function rather than a convention, for the reason `Refuse` takes a row: there
# is no way to increment the counter without also naming the case, so a
# twenty-fifth site cannot reopen this by omission.
# @param 1 The case name, matching the `FAIL <name>:` line beside the call.
note_failure() {
    failures=$(( failures + 1 ))
    # De-duplicated: the scans below call this once per offending SCRIPT, so an
    # unconditional append printed `failed: bash32 bash32 bash32` -- a list that
    # says less than the count beside it.
    case " ${failed_cases} " in
        *" $1 "*) ;;
        *) failed_cases="${failed_cases}${failed_cases:+ }$1" ;;
    esac
}

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
    expect "${vname}|0|${vrest}" "$out" 0 || note_failure "${vname}"
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
    note_failure "verdict-branches"
fi
ran=$(( ran + 1 ))

# --- the drain's verdict, against staged readings ---------------------------
#
# `_e2e_read_hit_bound` decides whether OUR bound or the PEER ended a read, from
# the CLOCK. `read -t`'s exit status cannot carry that: a timeout is above 128 on
# bash 4.0+ and a plain `1` -- byte-identical to EOF -- on the 3.2 that macOS ships.
# The status-based version therefore failed on macOS ALONE and passed everywhere
# else, which is why the decision is driven DIRECTLY here rather than through a
# staged listener: on this platform the real path cannot exhibit the difference.
# `.agent/rules/testing.md`, on splitting the decision out as a pure function over
# a record so a verdict needing a rare machine to reproduce needs one line here.
#
# The listener-backed arm of the same property is `http-silence-inconclusive`,
# which spends the whole five-second bound. These cost nothing and cover the
# branch on every platform, which that case cannot.
echo "== the drain's verdict, against staged readings"
read_bound_rows=(
    "hit-bound-exact|5|5|0|a read that consumed its whole bound is the bound's"
    "hit-bound-over|6|5|0|a read that overran its bound is still the bound's"
    "hit-bound-eof|0|5|1|a peer that closed at once is the peer's"
    "hit-bound-early|4|5|1|a peer that closed inside the bound is the peer's"
)
# Two empty lists agree perfectly: a table that loses its rows reports every
# reading clean, exactly like a scan that found no readings.
ran=$(( ran + 1 ))
if [ "${#read_bound_rows[@]}" -lt 1 ]; then
    echo "FAIL read-bound: the reading table is empty, so every row 'passed'." >&2
    note_failure "read-bound"
fi

for row in ${read_bound_rows[@]+"${read_bound_rows[@]}"}; do
    old="$IFS"
    IFS='|' read -r rname relapsed rbound rwant rwhat <<< "$row"
    IFS="$old"
    ( . "$library"; _e2e_read_hit_bound "$relapsed" "$rbound" ) && rgot=0 || rgot=$?
    ran=$(( ran + 1 ))
    if [ "$rgot" -ne "$rwant" ]; then
        echo "FAIL ${rname}: ${rwhat} -- elapsed ${relapsed}s against a ${rbound}s bound returned ${rgot}, wanted ${rwant}" >&2
        note_failure "${rname}"
    fi
done

# Four rows that all answer the same way pass every assertion above while testing
# nothing -- the `verdict-branches` argument thirty lines up, applied to a
# predicate with exactly two answers. Both must appear.
read_bound_answers="$(
    for row in ${read_bound_rows[@]+"${read_bound_rows[@]}"}; do
        old="$IFS"
        IFS='|' read -r rname relapsed rbound rwant rwhat <<< "$row"
        IFS="$old"
        printf '%s\n' "$rwant"
    done | sort -u | grep -c .
)"
ran=$(( ran + 1 ))
if [ "$read_bound_answers" -ne 2 ]; then
    echo "FAIL read-bound-branches: the readings produced ${read_bound_answers} distinct answers, not 2" >&2
    note_failure "read-bound-branches"
fi

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
    "http-silence|0|http_response_to_silence reported what the server volunteered"
    "http-silence-inconclusive|0|refused rather than reporting silence it never observed"
    "http-silence-refused|0|told a refused connection from an expired bound"
    "node-ready-waits-for-marker|0|wait_for_node_ready returned with the node serving|!BUG:"
    "node-ready-refuses-bound-only|1|to log: compile node ready|!BUG:"
    "node-ready-refuses-unbound|1|to listen on 127.0.0.1:|!to log:|!BUG:"
)

echo "== the helpers, in real shells"
for record in "${cases[@]}"; do
    name="${record%%|*}"
    out="$( bash "${BASH_SOURCE[0]}" --case "$name" 2>&1 )"
    status=$?
    ran=$(( ran + 1 ))
    expect "$record" "$out" "$status" || note_failure "${record%%|*}"
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
    note_failure "wait-clock-bound"
elif [ "$clock_took" -lt 3 ]; then
    echo "FAIL wait-clock-bound: a 4s bound came back after ${clock_took}s of wall clock;" >&2
    echo "     the loop is counting iterations rather than reading a clock" >&2
    printf '%s\n' "$out" | sed 's/^/     | /' >&2
    note_failure "wait-clock-bound"
else
    # And the number it PRINTS is the measured one. A loop could be bounded
    # correctly and still report the budget, which is the half with teeth --
    # a timeout message stating a duration nobody measured is a fixture lying
    # to the person diagnosing it.
    reported="$(printf '%s\n' "$out" | sed -n 's/^waited \([0-9][0-9]*\)s (measured.*/\1/p')"
    if [ -z "$reported" ] || [ "$reported" -lt 3 ]; then
        echo "FAIL wait-clock-bound: it waited ${clock_took}s and reported '${reported:-nothing}'" >&2
        printf '%s\n' "$out" | sed 's/^/     | /' >&2
        note_failure "wait-clock-bound"
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
    note_failure "bounded-clock"
elif [ "$bounded_took" -lt 1 ]; then
    echo "FAIL bounded-clock: a 1s bound over a 30s child returned in ${bounded_took}s;" >&2
    echo "     the command cannot have been run" >&2
    note_failure "bounded-clock"
elif [ "$bounded_took" -gt 10 ]; then
    echo "FAIL bounded-clock: a 1s bound over a TERM-ignoring child took ${bounded_took}s;" >&2
    echo "     the bound is waiting for a child that will not die, which is an" >&2
    echo "     unbounded wait inside the thing that exists to bound one" >&2
    note_failure "bounded-clock"
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
    note_failure "bounded-fast-path"
elif [ "$fast_took" -gt 2 ]; then
    echo "FAIL bounded-fast-path: twenty immediate commands took ${fast_took}s;" >&2
    echo "     the bound is sleeping through commands that have already finished" >&2
    note_failure "bounded-fast-path"
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
    note_failure "timeout-scan-canary"
fi
ran=$(( ran + 1 ))
spurious="$(_timeout_invocations "${canary_dir}/must-not-catch.sh" || true)"
if [ -n "$spurious" ]; then
    echo "FAIL timeout-scan-canary: the scan fired on text that runs nothing" >&2
    printf '%s\n' "$spurious" | sed 's/^/     | /' >&2
    note_failure "timeout-scan-canary"
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
# Every shell script in this repository, sorted, one per line.
#
# ONE generator for all three scans below. It used to be three copies of
# `"${source_dir}"/scripts/*.sh`, which is not recursive -- so `lib/e2e-common.sh`
# was outside every one of them. Measured, that delta is exactly one file and it
# is the worst possible one: the `timeout` scan's own failure message says "use
# run_bounded from scripts/lib/e2e-common.sh", and it could not read the file
# that DEFINES `run_bounded`. A `timeout 5 ...` added inside `run_bounded` was
# invisible to the check written to ban it, on the one platform that check
# exists for.
#
# `find` and not a glob, because a glob cannot recurse portably and this has to
# work in an exported tarball where there is no git. Scoped to `scripts/`, which
# is a choice and is therefore checked further down rather than assumed.
_shell_scripts() {
    find "${source_dir}/scripts" -type f -name '*.sh' 2>/dev/null | LC_ALL=C sort
}

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

# `tsan-gate.sh` was a row here, exempted because it resolved timeout/gtimeout for
# itself. #488 deleted that resolver -- the second half of its reason was that the
# macOS branch had never executed anywhere, which is an argument for removing the
# branch and not for excusing it -- so the gate goes through `run_bounded` like
# everything else and this scan now covers it. An exemption outlives the shape it
# was granted for, so it is deleted with the shape rather than reworded.
timeout_allowed="check-e2e-helpers.sh:this file, which stages the scan's own canary invocations above. They are heredoc text and run nothing; the canary asserting all seven are caught is what covers them."
timeout_scanned=0
while IFS= read -r script; do
    [ -n "$script" ] || continue
    base="${script##*/}"
    _scan_exempt "$base" "$timeout_allowed" && continue
    timeout_scanned=$(( timeout_scanned + 1 ))
    ran=$(( ran + 1 ))
    hits="$(_timeout_invocations "$script")"
    if [ -n "$hits" ]; then
        echo "FAIL timeout-scan: ${base} invokes timeout(1), which macOS does not have." >&2
        echo "     Use run_bounded from scripts/lib/e2e-common.sh." >&2
        printf '%s\n' "$hits" | sed 's/^/     | /' >&2
        note_failure "timeout-scan"
    fi
done < <( _shell_scripts )

# This scan was the one of the three with no census. If `source_dir` ever
# resolved wrong it reported clean over zero files and nothing said so -- the
# "two empty lists agree perfectly" failure its neighbours already guard against.
ran=$(( ran + 1 ))
if [ "$timeout_scanned" -lt 1 ]; then
    echo "FAIL timeout-scan: the walk matched no shell scripts, so every one of them 'passed'." >&2
    note_failure "timeout-scan"
fi

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
# under a DIFFERENT name is invisible to this. `migrate-storage-e2e.sh` spelled
# `free_port` as `port`, drawing from 40000-59999 -- entirely inside Linux's
# default ephemeral range, which is the `bind(...) failed: 98` the shared one
# moved to 20000-31999 to avoid. This scan never saw it, and could not: no regex
# over shell can. It took a person reading the file (#628, since landed), which
# is what the allowlist's per-row reasons are for -- and it is why a row is
# deleted when its defect is fixed rather than left standing as a description of
# the file. That row outlived #628 by a day and had become an exemption for a
# fixture with nothing to exempt.
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
    note_failure "helper-scan"
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
    note_failure "helper-scan-canary"
fi
ran=$(( ran + 1 ))
spurious="$(_helper_redefinitions "${canary_dir}/must-not-catch.sh")"
if [ -n "$spurious" ]; then
    echo "FAIL helper-scan-canary: the scan fired on a script that defines no copy" >&2
    printf '%s\n' "$spurious" | sed 's/^/     | /' >&2
    note_failure "helper-scan-canary"
fi
rm -rf "$canary_dir"

# The same `_scan_exempt` the timeout scan above uses. Each row names why, and the
# two that are defects name the ISSUE, so an exclusion cannot rot into folklore and
# closing the ticket has an obvious row to delete.
helper_copy_allowed="e2e-common.sh:the library itself, which defines every one of these names -- that being what the scan reads them out of. It entered this scan's set when the three enumerations were folded into one recursive walk; the row is what keeps that fold from reporting the definitions as copies.
local-gate.sh:not an e2e fixture. It sources nothing, starts no daemon and opens no socket; its 'fail' prints a build-gate verdict and its own selftest (local-gate-selftest) is what covers it.
launcher-replay-e2e.sh:#813. It is the last POSIX fixture that does not source the library at all, so its 'fail' and 'note' are private. The DEFECT that row used to name is gone (#627 made its 'fail' signal unconditionally, which is what the library does); what is left is the duplication, and converting a fixture that builds three CMake trees is its own change."
scanned=0
while IFS= read -r script; do
    [ -n "$script" ] || continue
    base="${script##*/}"
    _scan_exempt "$base" "$helper_copy_allowed" && continue
    scanned=$(( scanned + 1 ))
    ran=$(( ran + 1 ))
    hits="$(_helper_redefinitions "$script")"
    if [ -n "$hits" ]; then
        echo "FAIL helper-scan: ${base} defines its own copy of a shared helper." >&2
        echo "     Source scripts/lib/e2e-common.sh and delete the copy (#449, #451)." >&2
        printf '%s\n' "$hits" | sed 's/^/     | /' >&2
        note_failure "helper-scan"
    fi
done < <( _shell_scripts )
ran=$(( ran + 1 ))
if [ "$scanned" -lt 1 ]; then
    echo "FAIL helper-scan: the walk matched no shell scripts, so every one of them 'passed'." >&2
    note_failure "helper-scan"
fi

if command -v perl >/dev/null 2>&1 && perl -MIO::Socket::INET -e1 >/dev/null 2>&1; then
    echo "== the helpers, against a real listener"
    for record in "${socket_cases[@]}"; do
        name="${record%%|*}"
        out="$( bash "${BASH_SOURCE[0]}" --case "$name" 2>&1 )"
        status=$?
        ran=$(( ran + 1 ))
        expect "$record" "$out" "$status" || note_failure "${record%%|*}"
    done
else
    for record in "${socket_cases[@]}"; do
        echo "SKIPPED ${record%%|*}: perl with IO::Socket::INET is not available to stage a listener" >&2
        skipped=$(( skipped + 1 ))
    done
fi

# --- bash 3.2 --------------------------------------------------------------
#
# macOS ships a 2007 `/bin/bash` and these scripts run on every platform CI
# builds, so a construct newer than that is a script that does not start there.
# Scanned rather than remembered: the constraint was already written down in
# `coverage.sh`'s comments, where nobody writing a new script would find it.
#
# `BASHPID` is in the table because it is the trap that looks like the fix. A
# `[ "${BASHPID:-$$}" = "$top_pid" ]` guard is CORRECT on bash 4 and silently
# inert on 3.2, where BASHPID is unset and the test reduces to comparing `$$`
# with itself -- so the subshell defect it was written to close is closed on
# Linux and open on macOS. The unconditional `kill -TERM` avoids the question.
#
# And this row is LOAD-BEARING rather than belt-and-braces, which is only visible
# from having tried it: staging that guard back into `fail` and running this file
# on a bash 5 runner leaves `fail-subshell` GREEN, because on bash 5 the guard
# works. The behavioural case cannot see a 3.2-only defect from a 4-or-later
# shell, and every machine this is developed and tested on is a 4-or-later shell.
# The scan is the only check here that can.
#
# ## The scope, which is the half that was wrong (#627)
#
# This scan read ONE file -- `$library` -- for two tickets, while
# `launcher-replay-e2e.sh` carried the exact guard the paragraph above bans by
# name, under a comment arguing it was correct. So the repository had the rule
# written down, had a check enforcing it, and the check did not read the file
# that broke it: a confident verdict over the wrong set, the same shape as a
# clang-tidy sweep whose database was missing five of CI's targets.
#
# The set is therefore DERIVED, by walking `scripts/` for `*.sh`. Not a list: a
# list is exact about the files it knows and silent about the ones it does not,
# and silence reads identically to complete coverage (#492) -- #379 named six
# call sites where the tree had thirty. Not the two-directory glob its
# neighbours use either, which would miss the next subdirectory.
#
# Restricting the walk to `scripts/` is a scope choice, so it is CHECKED rather
# than assumed: when git is available, a tracked `*.sh` living anywhere else is
# refused by name. A classifier that cannot see a file must say so instead of
# passing it, and the day one appears outside `scripts/` this says which rather
# than quietly excluding it. Where git is not available -- an exported tarball --
# that half is reported as not run, which is a third state and not a pass.
#
# ## The whole family, and what this deliberately does NOT cover
#
# `BASHPID` is one of at least three ways bash 3.2 makes a script silently inert,
# and the general form is worth stating because it is what the next instance will
# look like: **inside a want-fail assertion, any failure to run is
# indistinguishable from the rule firing.** #723 reached it through a mode bit (a
# bare `"$0"` exited 126 and eight cases passed because the SHELL refused);
# `set -u` reaches it through expanding an empty array, which is an unbound
# variable before 4.4; `BASHPID` reaches it by making the abort a no-op.
#
# The token table covers the first and the third. It does NOT cover the empty
# array, and that is a decision rather than an omission. MEASURED on this tree:
# `"${x[@]}"` and `"$@"` occur about 110 times across the 22 scripts that set
# `-u`, and essentially every one of them is an array that cannot be empty. No
# regex over shell can tell a possibly-empty array from a never-empty one, so a
# row for it would be ~110 findings on a correct tree -- and a scan that reports
# noise is a scan somebody deletes, which costs the eight rows that do work. That
# leaves the empty-array sites as per-site tickets, which is where they already
# are (#793, #794); the remedy spelling is `${1+"$@"}`.
echo "== bash 3.2 constructs"
# bash32-scan: data-begin
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
# bash32-scan: data-end

# Two empty lists agree perfectly, and this one has a second way to empty itself:
# a table that loses its rows reports every script clean, exactly like a scan that
# found no scripts.
ran=$(( ran + 1 ))
if [ "${#banned[@]}" -lt 1 ]; then
    echo "FAIL bash32: the banned-construct table is empty, so every script 'passed'." >&2
    note_failure "bash32"
fi

# The prefilter's needles, written once rather than per file.
bash32_needles="$(mktemp)"
for entry in ${banned[@]+"${banned[@]}"}; do
    printf '%s\n' "${entry%%:*}" >> "$bash32_needles"
done

# What of a file this scan may read.
#
# Comment lines are dropped, because the header above explains WHY several of
# these are banned and a scan that fails on its own rationale is a scan nobody
# can write the rationale for. Indented comments too: the reason for the guard
# now sits inside a comment in `launcher-replay-e2e.sh`'s `fail`.
#
# And a file may declare a DATA REGION, between
#
#     # bash32-scan: data-begin
#     # bash32-scan: data-end
#
# which is dropped as well. That exists because this file has to hold the token
# table and the canary's staged text, and both are data rather than constructs
# this shell executes. The alternative -- and what this replaced -- was an
# allowlist row exempting the whole file, which is the wrong altitude by three
# orders of magnitude: `check-e2e-helpers.sh` is over 1700 lines, is the heaviest
# array user under `scripts/`, and IS RUN BY CTEST through `/bin/bash`, so on
# macOS it is a bash 3.2 script like any other. Exempting it wholesale would make
# the one file the scan can never read the one file most likely to break the rule
# -- and the neighbouring helper scan explicitly refused to pay that price 200
# lines up.
#
# Blank lines are substituted rather than deleted, so `grep -n` keeps reporting
# the real line numbers.
#
# A region opened and never closed would blank the rest of the file, so it is
# refused below rather than tolerated, and the number of regions is REPORTED --
# a region nobody can see added is this mechanism's own way of becoming an
# exemption.
_bash32_readable() {
    awk '
        /^[[:space:]]*# bash32-scan: data-begin[[:space:]]*$/ { skip = 1; print ""; next }
        /^[[:space:]]*# bash32-scan: data-end[[:space:]]*$/   { skip = 0; print ""; next }
        skip                                                 { print ""; next }
        /^[[:space:]]*#/                                     { print ""; next }
        { print }
    ' "$1"
}

# Every hit in one file, as `<table index> <lineno>:<text>`. The INDEX rather than
# the token, because three of the tokens contain a space and a caller splitting on
# one would report the wrong row for them.
#
# WHOLE-FILE FIRST, for the reason the helper scan above makes the same trade and
# `check-worker-refusals-counted.cmake` records it: without it this is two greps
# per token per script, and eight tokens over thirty scripts is ~480 processes.
# This test is in the DEFAULT set on every platform CI builds.
#
# MEASURED, with its conditions, because a bare ratio gets quoted at the wrong
# thing: on this 32-core Linux host with a warm cache, over the 31 scripts in
# this tree, the eight-grep loop costs 0.147 s and the prefiltered form 0.066 s
# -- about 2.2x. Not the 18x the naive comparison suggests, and the difference is
# worth stating: `_bash32_readable` is itself an `awk` per file, so the prefilter
# buys the greps back and then pays part of it out again for the region and
# comment filtering it also does. Today 15 of the 31 hold no token at all and
# stop after one `grep -q`.
#
# The prefilter is EXACT in both directions rather than merely cheap: it is the
# literal set of the same fixed strings, over the same filtered text, so it can
# produce neither a false negative nor a false positive.
_bash32_hits() {
    local file="$1" i=0 token="" hits="" text=""
    text="$(_bash32_readable "$file")"
    printf '%s\n' "$text" | grep -qFf "$bash32_needles" || return 0
    while [ "$i" -lt "${#banned[@]}" ]; do
        token="${banned[$i]%%:*}"
        hits="$(printf '%s\n' "$text" | grep -n -F -- "$token" || true)"
        [ -n "$hits" ] && printf '%s\n' "$hits" | sed "s/^/${i} /"
        i=$(( i + 1 ))
    done
    return 0
}

# A malformed region, in any scanned file. Prints what is wrong and returns 0
# when there is a fault; returns 1 when the file is well formed.
#
# A STATE MACHINE, and it has to be, because `_bash32_readable` is one. Counting
# the two markers and comparing totals is the obvious spelling and it is wrong in
# the direction that hides things: a `data-end` sitting ABOVE the first
# `data-begin` is a no-op for the reader and leaves a region open to EOF, while
# the counts come out 1 and 1 and the file reads as balanced. Measured -- staged
# into this very file, the whole run's output was byte-identical to a clean one,
# census line included, with a `mapfile` hidden after the stray marker.
#
# So this asks the same question the reader does, in the same order: a close with
# nothing open, a second open inside one, and anything still open at the end.
_bash32_region_fault() {
    awk '
        /^[[:space:]]*# bash32-scan: data-begin[[:space:]]*$/ {
            if (open) { print "line " NR ": a data-begin inside a region opened at line " at; exit }
            open = 1; at = NR; next
        }
        /^[[:space:]]*# bash32-scan: data-end[[:space:]]*$/ {
            if (!open) { print "line " NR ": a data-end with no region open"; exit }
            open = 0; next
        }
        END { if (open) print "the region opened at line " at " is never closed" }
    ' "$1" | grep . || return 1
}

# Print one file's hits in full, resolving each index back to its reason.
_bash32_report() {
    local base="$1" hits="$2" hit="" idx="" rest="" entry=""
    echo "FAIL bash32: ${base} uses a construct newer than bash 3.2." >&2
    while IFS= read -r hit; do
        [ -n "$hit" ] || continue
        idx="${hit%% *}"
        rest="${hit#* }"
        entry="${banned[$idx]}"
        echo "     | ${rest}" >&2
        echo "         '${entry%%:*}' -- ${entry#*:}" >&2
    done < <( printf '%s\n' "$hits" )
}

# The canary, both directions. A scan that has never been seen to fire is a scan
# reporting PASS over a set in which nothing could fail -- and this one has to be
# watched catching the EXACT text #627 was filed about, not a stand-in, because
# the guard's whole property is that it reads as correct.
canary_dir="$(mktemp -d)"
#
# Written with `printf` and not a heredoc, for the reason the helper-scan canary
# above gives: heredoc text is still text in THIS file, the helper scan reads
# whole scripts, and a staged `fail() {` at column zero is a genuine hit against
# it. Observed -- the first run of this block took that scan red. Every line
# below begins with `printf` in the source, so there is no definition here to
# find.
# bash32-scan: data-begin
{
    printf '%s\n' 'fail() {'
    printf '%s\n' '    echo "staged FAILED: $*" >&2'
    printf '%s\n' '    [ "${BASHPID:-$$}" = "$top_pid" ] || kill -TERM "$top_pid" 2>/dev/null'
    printf '%s\n' '    exit 1'
    printf '%s\n' '}'
    printf '%s\n' 'mapfile -t rows < /dev/null'
} > "${canary_dir}/must-catch.sh"
# bash32-scan: data-end
# The negative half. `$$` alone, a `#` comment naming a banned token, and an
# ordinary array expansion -- none of which is a construct newer than 3.2. The
# comment line is the one that matters: without it, a scan that stopped excluding
# comments would still pass every arm here.
cat > "${canary_dir}/must-not-catch.sh" <<'CANARY'
top_pid=$$
# BASHPID would be wrong here, and mapfile too.
for arg in ${1+"$@"}; do echo "$arg"; done
CANARY
# And the third arm, which is the one the DATA REGION mechanism needs: text
# inside a declared region is not read, and text outside one still is. Without
# it, a region that swallowed the whole file would pass both arms above.
# bash32-scan: data-begin
#
# Staged with `printf` and the marker SPLIT (`bash32-%s`), so the region markers
# this file needs to write do not read as region markers OF this file. A heredoc
# holding them verbatim would close the enclosing region four lines early and
# expose the `declare -A` below it -- which is exactly what happened on the first
# run of this block, caught by the scan now reading its own source.
{
    printf '# bash32-%s: data-begin\n' scan
    printf '%s\n' 'rows="mapfile is data here"'
    printf '# bash32-%s: data-end\n' scan
    printf '%s\n' 'declare -A real=()'
} > "${canary_dir}/region.sh"
# bash32-scan: data-end
ran=$(( ran + 1 ))
staged="$(_bash32_hits "${canary_dir}/must-catch.sh")"
caught="$(printf '%s\n' "$staged" | grep -c . || true)"
if [ "$caught" -ne 2 ]; then
    echo "FAIL bash32-canary: the scan caught ${caught} of 2 staged constructs," >&2
    echo "     so it cannot be trusted to have found none in the real scripts" >&2
    printf '%s\n' "$staged" | sed 's/^/     | /' >&2
    note_failure "bash32-canary"
fi
ran=$(( ran + 1 ))
spurious="$(_bash32_hits "${canary_dir}/must-not-catch.sh")"
if [ -n "$spurious" ]; then
    echo "FAIL bash32-canary: the scan fired on a script that uses nothing newer than 3.2" >&2
    printf '%s\n' "$spurious" | sed 's/^/     | /' >&2
    note_failure "bash32-canary"
fi
ran=$(( ran + 1 ))
region_hits="$(_bash32_hits "${canary_dir}/region.sh")"
region_count="$(printf '%s\n' "$region_hits" | grep -c . || true)"
if [ "$region_count" -ne 1 ]; then
    echo "FAIL bash32-canary: a declared data region should hide exactly its own text;" >&2
    echo "     the scan reported ${region_count} hit(s) where 1 is right (the one OUTSIDE it)" >&2
    printf '%s\n' "$region_hits" | sed 's/^/     | /' >&2
    note_failure "bash32-canary"
fi
# The control for the region check, staged against the file that actually HAS a
# balanced pair. It used to point at `must-catch.sh`, which carries no marker at
# all -- so the arm read "a file with no markers is not reported unbalanced",
# which is not what its own message claimed and is a far weaker thing.
ran=$(( ran + 1 ))
if _bash32_region_fault "${canary_dir}/region.sh" >/dev/null; then
    echo "FAIL bash32-canary: a balanced pair was reported as malformed" >&2
    _bash32_region_fault "${canary_dir}/region.sh" | sed 's/^/     | /' >&2
    note_failure "bash32-canary"
fi
# And the three malformed shapes, each of which the READER treats differently
# from the way a count would.
ran=$(( ran + 1 ))
printf '%s\n' '# bash32-scan: data-begin' > "${canary_dir}/unclosed.sh"
if ! _bash32_region_fault "${canary_dir}/unclosed.sh" >/dev/null; then
    echo "FAIL bash32-canary: an unclosed data region was not reported; one would blank" >&2
    echo "     the rest of a file and hide every construct after it" >&2
    note_failure "bash32-canary"
fi
ran=$(( ran + 1 ))
# bash32-scan: data-begin
{
    printf '# bash32-%s: data-end\n' scan
    printf '# bash32-%s: data-begin\n' scan
    printf '%s\n' 'mapfile -t hidden < /dev/null'
} > "${canary_dir}/swapped.sh"
# bash32-scan: data-end
if ! _bash32_region_fault "${canary_dir}/swapped.sh" >/dev/null; then
    echo "FAIL bash32-canary: a data-end ABOVE the first data-begin was not reported." >&2
    echo "     The counts balance and the reader still blanks everything after the open," >&2
    echo "     so a construct below it is invisible with nothing saying so." >&2
    note_failure "bash32-canary"
fi
ran=$(( ran + 1 ))
{
    printf '# bash32-%s: data-begin\n' scan
    printf '# bash32-%s: data-begin\n' scan
    printf '# bash32-%s: data-end\n' scan
} > "${canary_dir}/nested.sh"
if ! _bash32_region_fault "${canary_dir}/nested.sh" >/dev/null; then
    echo "FAIL bash32-canary: a second data-begin inside an open region was not reported" >&2
    note_failure "bash32-canary"
fi
rm -rf "$canary_dir"

# The allowlist, in the `_scan_exempt` shape the two scans above use. Each row
# names WHY, and a row that is a deferred defect names the ISSUE so an exclusion
# cannot rot into folklore. Neither of these two is a defect.
bash32_allowed="tidy-sweep.sh:not a bash 3.2 script and does not claim to be. It declares a bash 4.4 floor in its own header (wait -n is 4.3; expanding an empty array under set -u stops erroring at 4.4), and its 'tidy-sweep-selftest' registration carries SKIP_RETURN_CODE 77 so a stock macOS runner reports SKIPPED rather than red. A declared exception with an enforcement mechanism, not an omission."

# DERIVED, by walking the tree rather than by listing files or naming
# directories. `find` because a glob cannot recurse portably and this must work in
# an exported tarball, where there is no git.
bash32_scanned=0
bash32_regions=""
while IFS= read -r script; do
    [ -n "$script" ] || continue
    base="${script##*/}"
    _scan_exempt "$base" "$bash32_allowed" && continue
    bash32_scanned=$(( bash32_scanned + 1 ))
    ran=$(( ran + 1 ))

    fault="$(_bash32_region_fault "$script")" && {
        echo "FAIL bash32: ${base} has a malformed data region -- ${fault}." >&2
        echo "     An unclosed one blanks the rest of the file, so every construct after" >&2
        echo "     it would be invisible to this scan." >&2
        note_failure "bash32"
    }
    if grep -q '^[[:space:]]*# bash32-scan: data-begin[[:space:]]*$' "$script"; then
        bash32_regions="${bash32_regions}${bash32_regions:+, }${base}"
    fi

    hits="$(_bash32_hits "$script")"
    if [ -n "$hits" ]; then
        _bash32_report "$base" "$hits"
        note_failure "bash32"
    fi
done < <( _shell_scripts )
rm -f "$bash32_needles"

ran=$(( ran + 1 ))
if [ "$bash32_scanned" -lt 1 ]; then
    echo "FAIL bash32: the walk of ${source_dir}/scripts matched no shell scripts," >&2
    echo "     so every one of them 'passed'. This is the clause that decides whether" >&2
    echo "     widening the scan bought anything (#627)." >&2
    note_failure "bash32"
else
    echo "   bash 3.2: scanned ${bash32_scanned} script(s) under scripts/ (walked, not listed)"
    # Named rather than counted. A declared region is an exemption with a smaller
    # blast radius, not no exemption, so it is visible on every run.
    echo "   bash 3.2: declared data region(s) in: ${bash32_regions:-none}"
fi

# And the scope choice itself, checked rather than assumed. A tracked `*.sh`
# outside `scripts/` is outside the walk, and a file the classifier cannot see is
# refused by name rather than silently excluded.
#
# The claim states its SEARCH, because a census that does not is one somebody
# quotes at the wrong set. The pattern is `git ls-files '*.sh'` -- tracked files
# whose NAME ends `.sh` -- so this says nothing about the ten `#!/bin/sh`
# templates under `packaging/` (`*.sh.in`, `*.in`), which are installer text
# rather than scripts ctest runs and are not bash at all. Widening it to classify
# by shebang would report those ten on a correct tree, which is the noise that
# gets a scan deleted.
if git -C "$source_dir" rev-parse --git-dir >/dev/null 2>&1; then
    ran=$(( ran + 1 ))
    stray="$(git -C "$source_dir" ls-files '*.sh' | grep -v '^scripts/' || true)"
    if [ -n "$stray" ]; then
        echo "FAIL bash32-scope: tracked shell script(s) live outside scripts/, where the walk" >&2
        echo "     above does not reach them:" >&2
        printf '%s\n' "$stray" | sed 's/^/     | /' >&2
        echo "     Widen the walk, or give each one an allowlist row saying why it is exempt." >&2
        note_failure "bash32-scope"
    else
        echo "   bash 3.2: scope confirmed -- git ls-files '*.sh' finds none outside scripts/"
    fi
else
    # Not a pass and not a failure: the question could not be asked. Said out
    # loud, because "no strays found" and "nothing looked" read identically --
    # and counted as SKIPPED only, never also as run.
    echo "   bash 3.2: NOT CHECKED whether any *.sh lives outside scripts/ -- no git repository here" >&2
    skipped=$(( skipped + 1 ))
fi

# A fractional `read -t` is on the library's own banned list and was the one entry
# nothing checked: the table above is `grep -F`, and this needs a pattern. So it was
# REMEMBERED rather than scanned, in a file whose whole argument is that remembering
# does not work -- and #824 then added a `read -t` whose comment cites bash 3.2 as
# the reason it is an integer, resting on a check that was not there.
#
# Not folded into `banned`: every row there is a literal by construction, and giving
# one of them regex meaning would make the other eight silently regexes too.
#
# The scan covers `$library` and not this file. Scanning this one would match the
# `banned` table's own rows, which are data rather than uses -- the "a comment is not
# a call site" problem in a form a comment filter cannot solve. That gap is real and
# is not closed here; see the reported findings.
#
# THREE CLAUSES, because a literal scan alone could not see the very call it was
# written for. #824's `read -t` takes `"$_e2e_http_read_bound"`, so a pattern looking
# for a digit after `-t` matched nothing on the fixed tree AND would match nothing on
# a tree that set that bound to `0.5` -- the regression this exists to catch, passing
# in silence. So the bound is followed through the variable, and the scan asserts it
# saw at least one `read -t` at all: zero is the spelling of "this stopped reading
# what it thinks it reads", which is the same fail-closed clause the `note_failure`
# census below uses on itself.
echo "== bash 3.2: a fractional read -t"
ran=$(( ran + 1 ))
bash32_read_uses="$(grep -nE 'read [^;|&]*-t' "$library" | grep -v '^[0-9][0-9]*: *#' || true)"

# (1) the bound written as a literal.
fractional="$(printf '%s\n' "$bash32_read_uses" | grep -E -- '-t *[0-9]*\.' || true)"

# (2) the bound written as a variable whose value is fractional. The name is read
#     off the call site rather than remembered, so a second bound gets checked too.
for bound_var in $(printf '%s\n' "$bash32_read_uses" \
    | grep -oE -- '-t[[:space:]]*"?\$\{?[A-Za-z_][A-Za-z0-9_]*' \
    | sed -E 's/.*\$\{?//' | sort -u); do
    frac_assign="$(grep -nE "^[[:space:]]*(local[[:space:]]+)?${bound_var}=[^#]*[0-9]\." "$library" || true)"
    if [ -n "$frac_assign" ]; then
        fractional="${fractional}${fractional:+
}${frac_assign}"
    fi
done

if [ -n "$fractional" ]; then
    echo "FAIL bash32: ${library} uses a fractional 'read -t' (bash 4.0+; 3.2 rejects it)" >&2
    printf '%s\n' "$fractional" | sed 's/^/     | /' >&2
    note_failure "bash32-fractional-read"
elif [ -z "$bash32_read_uses" ]; then
    # (3) the positive control. A clean verdict is only worth something if the scan
    #     can still find the construct it is judging.
    echo "FAIL bash32: the fractional-'read -t' scan found no 'read -t' in ${library} at all," >&2
    echo "     so its clean verdict describes nothing. Fix the pattern, not the library." >&2
    note_failure "bash32-fractional-read"
else
    echo "   bash 3.2: every 'read -t' bound in ${library##*/} is a whole number (literals and variables)"
fi

# --- every failure is recorded BY NAME ------------------------------------
#
# `note_failure` exists so the count and the name cannot be recorded in different
# places, and that only holds while it is the ONLY thing that touches the counter.
# Nineteen sites incremented `failures` by hand before #678; a twentieth written the
# old way would compile, run, pass, and go back to reporting `1 failed` with no name
# -- which is the omission the function was introduced to make impossible.
#
# So this scans its own source, the way the bash-3.2 check above scans the library.
# One increment is expected: the one inside `note_failure` itself.
#
# The pattern is written as a regex with `[+]` rather than as the literal text, so
# this scan cannot COUNT ITSELF -- which it did on the first run, reporting three
# increments where there is one, because the needle appeared verbatim in the needle.
#
# It fails in BOTH directions. An extra increment names a site that bypassed the
# helper; ZERO increments means the scan has stopped seeing what it thinks it is
# reading -- a rename of the counter would otherwise leave this passing forever
# while guarding nothing.
ran=$(( ran + 1 ))
own_increments="$(grep -cE 'failures=\$\(\( failures [+] 1 \)\)' "${BASH_SOURCE[0]}" | tr -d ' ')"
if [ "$own_increments" = "0" ]; then
    echo "FAIL failure-recording: found no direct increment of the counter in ${BASH_SOURCE[0]} at all;" >&2
    echo "     the counter has been renamed and this scan is now guarding nothing" >&2
    note_failure "failure-recording"
elif [ "$own_increments" != "1" ]; then
    echo "FAIL failure-recording: ${own_increments} sites increment the failure counter directly;" >&2
    echo "     only note_failure may, or a failure is counted without being named (#678)" >&2
    grep -nE 'failures=\$\(\( failures [+] 1 \)\)' "${BASH_SOURCE[0]}" | sed 's/^/     | /' >&2
    note_failure "failure-recording"
fi

# And the library must not be executable or carry a `#!`: it is sourced, and a
# copy that looks runnable invites someone to run it, which does nothing and
# says nothing.
ran=$(( ran + 1 ))
first_line="$(head -1 "$library")"
case "$first_line" in
    '#!'*)
        echo "FAIL shebang: ${library} is sourced, not executed, and must carry no '#!' line" >&2
        note_failure "shebang"
        ;;
esac

# ---------------------------------------------------------------------------

echo
echo "e2e-helpers-selftest: ${ran} checks ran, ${failures} failed, ${skipped} skipped"
if [ "$skipped" -gt 0 ]; then
    echo "  (a skip is not a pass: the ${skipped} skipped checks were not run at all)"
fi
if [ "$failures" -gt 0 ]; then
    # Named on the LAST lines, which are what survive a truncated capture and what
    # somebody reads first.
    echo "  failed: ${failed_cases}"
    echo "  re-run one alone with: bash ${BASH_SOURCE[0]} --case <name>"

    # And a copy the NEXT run cannot overwrite. ctest keeps one
    # `Testing/Temporary/LastTest.log`, so #678's evidence was destroyed by the
    # three clean runs that followed the failure; all that survived was
    # `LastTestsFailed.log`, which names the TEST and not the case. A
    # process-unique path accumulates instead of clobbering, which is what makes a
    # rare failure diagnosable at all.
    #
    # Best effort by design: a selftest that has already FAILED must not also fail
    # to report because a directory was unwritable, so this tolerates an error and
    # the names above are printed either way.
    durable="${TMPDIR:-/tmp}/e2e-helpers-selftest-failed-$(date +%Y%m%d-%H%M%S)-$$.log"
    if {
        echo "e2e-helpers-selftest: ${ran} checks ran, ${failures} failed, ${skipped} skipped"
        echo "failed: ${failed_cases}"
        echo "host: $(uname -srm 2>/dev/null || echo unknown)"
        echo "note: per-case detail went to stderr. Re-run under"
        echo "      ctest --output-on-failure, or one case alone as"
        echo "      bash ${BASH_SOURCE[0]} --case <name>"
    } > "$durable" 2>/dev/null; then
        echo "  a copy the next run will not overwrite: ${durable}"
    fi
fi
[ "$failures" -eq 0 ] || exit 1
exit 0
