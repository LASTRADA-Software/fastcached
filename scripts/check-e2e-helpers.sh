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
#   check-e2e-helpers.sh                   run every case
#   check-e2e-helpers.sh --case NAME       run one case and REPORT on it:
#                                          0 clean, 3 the case printed BUG:,
#                                          anything else the case aborted
#   check-e2e-helpers.sh --case-body NAME  the inner half, whose status answers
#                                          only *did this case run*. Used by
#                                          `--case`; not the mode to re-run by
#                                          hand, because its verdict is the
#                                          OUTPUT and nothing reads it for you.
set -uo pipefail

source_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
library="${source_dir}/scripts/lib/e2e-common.sh"

# How many immediate commands `fast_path_ms` puts through `run_bounded`.
#
# LOAD-BEARING, and therefore a NAME rather than a literal in the loop with the
# word "twenty" retyped in three comments and two messages. The ceiling below is
# `FastPathCommands` x `_e2e_poll_pause` (0.2s) at twenty; halving the count
# halves the separation and NOTHING goes red -- the staged flat-pause defect
# would then cost ~2.05s, still over a 2000ms ceiling, so `bounded-fast-path-bites`
# still passes while the gap it certifies has collapsed from 4058-vs-927 to
# ~2050-vs-500. The figure the two cases PRINT is this variable for the same
# reason: prose reading "twenty" beside a loop of ten is a check reporting on
# work it did not do.
FastPathCommands=20

# What `FastPathCommands` immediate commands may cost before `run_bounded` is
# judged to be sleeping through them. Shared by the two cases that measure it: the
# healthy one asserts UNDER this, the staged-defect one asserts OVER it, so the
# number cannot drift into meaning nothing without one of the two failing.
FastPathCeilingMs=2000

# What the STAGED defect must measure PER PAUSE for the ceiling above to be said
# to separate anything.
#
# PER PAUSE and not in total (#1196). The total is not a property of
# `run_bounded` alone: its loop polls only while the command is still alive, so a
# command that finishes before its first poll asks for no pause at all, and under
# load the PARENT is descheduled long enough for that to happen. Measured on one
# box, 2026-09-10, WSL2 / Ubuntu-24.04 / 32 CPUs: 20 pauses of 20 commands on
# nine runs of ten and 18 on the tenth. On a hosted `Linux-gcc-release` runner,
# 15 -- and 15 x 200ms is EXACTLY the 3000ms the old total floor demanded, which
# a `-le` then refused, reddening master on a tree nobody had touched.
#
# So the previous fix's premise was false exactly where it was most confident:
# *"twenty `sleep 0.2` calls were requested and performed every time -- counted"*
# is true of an idle host and of no other. Moving the verdict off the wall clock
# was right; moving it onto a quantity the host still decides was the half nobody
# re-derived. **Dividing by the count cancels the host out** and keeps every
# argument below intact, because the count is the only part the host touches.
#
# `bounded-fast-path-bites` used to assert only that the defect came in OVER the
# ceiling, and "over" is not "separated": at ten commands the flat-pause defect
# is 2050ms against a 2000ms ceiling, a 2.5% margin on an instrument whose
# HEALTHY end has been measured spread over 700ms. That case would have passed
# and certified nothing. The floor is 1.5x the ceiling, which the real defect
# clears by construction -- twenty `sleep`s of `_e2e_poll_pause` cannot come back
# in under 4000ms, and the best defective reading on the host in the table below
# was 4058ms, so 3000ms leaves 35% headroom at the tight end and every reading
# above it is further away.
#
# DERIVED from the ceiling rather than written out as 3000, because the sentence
# above is the whole point of the constant and a literal states it in prose while
# implementing nothing. Written out, the two numbers are independent: raising
# `FastPathCeilingMs` past the defect -- which is precisely the way this ceiling
# stops separating -- leaves the floor at 3000, the staged defect still clears it,
# and `bounded-fast-path-bites` goes on passing while certifying a ceiling that
# has stopped meaning anything. Measured, staging exactly that: with the floor
# written out, a ceiling of 10000ms was caught by nothing.
# The ceiling's share of ONE pause, times the same 1.5. Both hollowings-out the
# paragraphs above describe are still caught, and now for a reason no host can
# move: a ceiling raised to 10000ms puts this floor at 750ms against a defective
# pause of 200, and halving `FastPathCommands` puts it at 300 against the same
# 200. Integer division, so it is exact at these values (6000/40).
FastPathDefectFloorMsPerPause=$(( (FastPathCeilingMs * 3) / (FastPathCommands * 2) ))

# What a HEALTHY run may ask for before the ceiling above has stopped separating
# anything.
#
# `bounded-fast-path-bites` re-derives the DEFECT end on the machine that is
# running -- it stages the defect and asserts it clears the floor. Nothing re-derived
# the HEALTHY end: the case asserted only *healthy <= ceiling*, never that healthy sat
# FAR below it. The requested totals are quantised at 200 / 400 / 800 / 1800 / 3800 ms
# for twenty commands when every command consumes the same number of poll ticks, so
# **1800 ms was a legal passing verdict** -- nine times healthy, four ticks per command
# against one. A regression producing it passed this case AND `-bites`, which stages a
# FLAT pause and is therefore anchored at 4000 ms whatever the host does. Both guards
# green, separation collapsed from 20x to 2.2x, and no `SLOW:` line because the wall
# clock never moved. That is the ceiling's own defect at the other end: the floor's
# comment above argues that "over" is not "separated", and this end never got the
# same treatment.
#
# **The number is defensible because of the MARGIN, not because of the symmetry.**
# `ceiling * 2/3` mirrors the floor's `ceiling * 3/2`, which is tidy and is still one
# number justified by another number. What defends it is the size of the gap in the
# quantity that actually varies -- `f`, the fraction of commands consuming four ticks
# rather than one, where the total is `200 + 1600f`:
#
#     healthy, best                       200 ms     f = 0%
#     healthy, worst measured at load 34-62   230 ms     f = 1.9%
#     this bound                         1333 ms     f = 70.8%
#     the regression that must fail      1800 ms     f = 100%
#
# **A 38-fold margin.** To trip this a host would need seven commands in ten to
# survive four polls, against under two in a hundred at a load average of 62 on 32
# cores. That is not a slow host; that is the regression. The general worry -- that a
# threshold at the healthy end reintroduces the host sensitivity this case exists to
# remove -- is sound and does not bite at this magnitude, which is a figure rather
# than a hope.
#
# It admits 800 ms, four times healthy and a real regression that will go unnoticed
# here. Left deliberately: this bounds the SEPARATION collapsing, not every regression,
# and widening it is how a derived bound turns back into a tuned one.
FastPathHealthyMaxMs=$(( FastPathCeilingMs * 2 / 3 ))

# What `run_bounded` costs per call when nothing is wrong, which is what makes the
# `SLOW:` reading a NUMBER rather than an adjective.
#
# A CONSTANT and not a literal inside the message: the paragraph at the ceiling
# case says the baseline must live in one place, and the version of that message
# spelled `4-5ms` in the `echo` anyway -- restating it in exactly the way the
# comment beside it forbade, and drifting the moment the helper changed.
#
# A QUANTITY UNDER CONDITIONS, and both halves travel:
#
#   * pre-#1077, 32-core Linux, lightly loaded: 284-303ms for twenty commands
#     against 200ms requested, so about 4-5ms per call.
#   * #1077 arms the deadline INSIDE the poll loop, which is one extra fork per
#     call. Measured as a PAIRED step and not as two independent readings, the
#     two trees alternated within each pair so load drift cannot masquerade as
#     the difference: +2ms per call, 7 of 8 pairs positive, N=8, load ~26.
#   * ANOTHER MACHINE, same tree, minutes apart, reported by another lane rather
#     than measured here: WSL2 reads **5.5ms** per call and Git Bash **128ms** --
#     a 23x spread on one box, with the requested total at 200ms in BOTH. WSL2's
#     5.5ms lands inside this range and is the first independent check the figure
#     has had; it had rested on one machine.
#
#     That row is here for the reader rather than for the number. Somebody on Git
#     Bash meeting `128ms per call against a 6-7ms baseline` in the `SLOW:` line
#     below has nothing telling them it is their SHELL and not a broken host --
#     the MSYS fork path, on hardware that is otherwise ordinary.
#
# The figure below is DERIVED from that step rather than re-measured on an idle
# host, which is said out loud because it is the weaker of the two claims and a
# reader has no way to recover the difference afterwards.
#
# It is also the ceiling case's own comment coming true on a real change instead
# of as an argument: that paragraph says a change adding one fork per call
# "arrives here as a breach and reads exactly like a slow host", and #1077 then
# added exactly that fork. A STEP in this figure is the helper; a proportional
# rise across the whole run is the host.
FastPathBaselineMsPerCall="6-7"

# Is a timer reading the `<seconds>.<three digits>` shape `%3R` produces?
#
# PURE, and lifted out of `fast_path_ms` for the reason this file's header gives
# for `_e2e_verdict` being pure: a refusal nobody has watched fire is not a
# refusal, and this one cannot be staged THROUGH `fast_path_ms` without a hook
# into `TIMEFORMAT` that would exist only for the test. As a predicate it is one
# case row per arm, driven against readings rather than against a clock.
#
# @param 1 the reading, as `time` printed it.
# @return 0 when it is a duration this file can convert.
_is_duration_reading() {
    case "$1" in
        [0-9]*.[0-9][0-9][0-9]) return 0 ;;
        *) return 1 ;;
    esac
}

# Where `fast_path_ms`'s pause meter writes, one line per pause REQUESTED.
#
# A FUNCTION rather than a variable, and the reason is the bug it replaces:
# `fast_path_ms` is itself called as `fast_ms="$(fast_path_ms)"`, so it runs in a
# subshell and every variable it sets dies with it. A `_fast_path_ledger=...`
# assignment inside it left the reader looking at an empty name, and
# `fast_path_asked_ms` then reported a pause total of ZERO -- caught only because
# the positive control below refuses zero BY NAME rather than reading it as a very
# fast bound. Derived from `scratch` at both ends instead, so there is no value
# left to lose across the boundary.
fast_path_ledger() { printf '%s' "${scratch}/fast-path-pauses"; }

# How much pause `run_bounded` ASKED FOR during the last `fast_path_ms`, in whole
# milliseconds.
#
# **This is the verdict the two cases below assert on, and the elapsed time is
# only evidence beside it.** What the helper REQUESTED is a decision: no host, no
# scheduler and no time sync can move it, where every wall-clock reading in this
# file can be stepped (#1058). It is also strictly more than a timing proxy can
# see, since it counts the ticks as well as their length -- a `run_bounded` that
# polled five times per call instead of once is invisible to a ceiling the ramp
# still fits under, and shows up here as five times the pause.
#
# `LC_ALL=C` on the awk for the separator reason `fast_path_ms` gives above: a
# `de_DE` awk reads `0.01` as `0` and the total comes back plausible and short.
#
# A malformed line is REFUSED rather than skipped, because awk would otherwise
# read it as zero and the total would be short by an unknown amount -- the one
# wrong answer a caller has no way to detect.
#
# @return the total in whole milliseconds; `0` when nothing was recorded, which
#         the caller must read as a LOST ledger rather than as a fast bound.
fast_path_asked_ms() {
    local total ledger
    ledger="$(fast_path_ledger)"
    if [ ! -e "$ledger" ]; then
        printf '0'
        return 0
    fi
    total="$(LC_ALL=C awk '
        !/^[0-9]+(\.[0-9]+)?$/ { bad = $0; exit 1 }
        { sum += $1 }
        END { if (bad == "") printf "%d", (sum * 1000) + 0.5 }
    ' "$ledger")" || {
        echo "the pause ledger holds a line that is not a pause, so its total would be short by an unknown amount" >&2
        return 1
    }
    printf '%s' "$total"
}

# How many pauses `run_bounded` requested during the last `fast_path_ms`.
#
# The half of the total that the HOST decides, and the reason the verdict below
# divides by it (#1196). One line per pause, so this is the line count; a missing
# ledger is 0 rather than an error, which the verdict treats as its own outcome.
#
# `awk` reading the file directly. No pipe -- this file's own early-exit scan
# refuses `wc -l < f | ...` shapes, and a count is exactly what `END { NR }` is.
fast_path_pause_count() {
    local ledger
    ledger="$(fast_path_ledger)"
    if [ ! -e "$ledger" ]; then
        printf '0'
        return 0
    fi
    LC_ALL=C awk 'END { printf "%d", NR }' "$ledger"
}

# The verdict `bounded-fast-path-bites` reaches, as a PURE FUNCTION of what was
# measured (#1196).
#
# Split out so both directions can be driven from a staged record rather than by
# provoking a real defect and timing it -- the same argument this repository
# makes for `node-scratch-isolation-e2e-selftest`: a decision measured through an
# instrument whose own overhead is comparable to the quantity is not being
# measured at all, and branches that could not be staged become one line each.
#
# It is what makes the ACCEPTING direction assertable too. The old total floor
# had never been watched accepting a 15-pause run, because no local run produces
# one; the record table below states that case by name, and it is the exact
# reading that reddened master.
#
# @param 1 milliseconds of pause requested in total.
# @param 2 how many pauses that was spread over.
# @return prints the `BUG:` lines, or nothing at all when the defect is separable.
fast_path_defect_verdict() {
    local asked="$1" pauses="$2" per
    # ZERO is its own outcome and not a division. It is also not a defect in
    # `run_bounded`: it is every command finishing before its first poll, which is
    # the fast path working. What it is not is a MEASUREMENT, so it may not be
    # reported as a pass.
    if [ "$pauses" -lt 1 ]; then
        echo "BUG: no pause was requested at all, so nothing here measures the ceiling"
        return 0
    fi
    per=$(( asked / pauses ))
    if [ "$per" -lt "$FastPathDefectFloorMsPerPause" ]; then
        echo "BUG: a defective pause cost ${per}ms against a ${FastPathDefectFloorMsPerPause}ms floor, so the"
        echo "BUG: ${FastPathCeilingMs}ms ceiling no longer separates a healthy run from a broken one"
    fi
}

# Run `FastPathCommands` commands that finish immediately through `run_bounded`,
# and echo how many milliseconds they took.
#
# `%3R` through bash's own `time`, because `date +%s%N` is GNU-only (BSD `date`
# prints a literal `N`) and bash 3.2 has no `EPOCHREALTIME`. The decimal separator
# is LOCALE-DEPENDENT -- measured, `de_DE.UTF-8` yields `0,255` and the arithmetic
# below then fails -- so the substitution exports `LC_ALL=C` rather than hoping.
#
# `lib/e2e-common.sh` REFUSES this same construct and is right to (#851): a
# `0,243` reaching `$(( 10# ... ))` is a wrong SMALL number rather than an error,
# because bash arithmetic reads `,` as the comma operator, and integer `SECONDS`
# has no such path. The two files are not in disagreement -- the library needs
# only whole-second resolution and takes the option with no failure mode, while
# this check is discriminating 0.5s from 4.1s and cannot. What buys the exception
# is that BOTH hazards are closed here and watched: `LC_ALL=C` for the separator,
# `_is_duration_reading` for anything that still arrives malformed, and
# `duration-reading-shape` asserting that predicate refuses AND accepts. Sub-second
# timing without those three is the library's rule, and it stands.
#
# The loop's own output is closed off because the SUBSTITUTION is reading stderr,
# which is where `time` reports. A stray diagnostic from `run_bounded` would
# otherwise land in `elapsed` and corrupt the parse. It fails closed when it does,
# at two layers: `_is_duration_reading` refuses the reading BY NAME, and anything
# that slips past it aborts the case in the arithmetic below. Either way the
# driver reports a non-zero exit rather than a number nobody measured.
#
# @return The elapsed time of the `FastPathCommands` calls, in whole milliseconds.
fast_path_ms() {
    local elapsed
    # Truncated HERE rather than at the two call sites, so the pause total and the
    # elapsed reading cover the same twenty commands by construction rather than
    # by both cases remembering to say so.
    local ledger
    ledger="$(fast_path_ledger)"
    if ! : > "$ledger"; then
        echo "cannot open the pause ledger at ${ledger}" >&2
        return 1
    fi
    # The pause meter runs INSIDE the substitution below; its EXPLANATION lives
    # out here, and that placement is load-bearing rather than tidiness.
    #
    # **bash 3.2's command-substitution scanner does not skip comments.** An
    # apostrophe in a comment inside `$( )` therefore opens a single-quoted string
    # as far as that scanner is concerned, and it swallows the closing paren. This
    # block sat inside the substitution and carried five of them -- `lane's`,
    # `run_bounded`'s, `measurement's`, `timer's`, `file's` -- an odd count, so the
    # scan ended mid-quote and the parser reported `syntax error near unexpected
    # token '('` **135 lines further down**, at the first real paren in the file,
    # which is why the reported line was a valid line that master parses fine.
    #
    # macOS ships 3.2, so this was `e2e-helpers-selftest` dying at PARSE in 0.02s
    # on `macOS-clang-release` while every other platform ran it green. Measured
    # on a real bash 3.2.39 rather than reasoned: the two constructs that look
    # more suspicious both parse there -- a function definition inside `$( )`, and
    # a multi-line single-quoted `awk` program -- and only the comment apostrophe
    # fails. Keeping the body comment-free is what makes that unrepeatable: there
    # is nowhere for an apostrophe to hide.
    #
    # The file's `bash 3.2` scan cannot catch this and is not at fault: it checks
    # TOKENS (`mapfile`, `declare -A`, `local -n`), and this is a composition of
    # individually legal ones. That gap is #880.
    #
    # `run_bounded` calls `sleep` by bare name, so a function of that name takes
    # precedence -- and a function defined in there is confined to that subshell,
    # so no other case in this file runs against a shadowed `sleep`. `command
    # sleep` delegates to the real one, so the timing the expression is measuring
    # is unchanged: the meter records the REQUEST and changes nothing about the
    # pause.
    #
    # In THIS file and not in `lib/e2e-common.sh`, which would be the tidier home.
    # That library is another lane's subject right now (#1048, #1066), and a check
    # does not reach into a file it does not own to install the instrument it
    # needs. It is also the more honest coupling: the property under test is
    # `run_bounded`'s pacing, so the meter belongs to the test.
    #
    # It fails CLOSED if `run_bounded` ever stops spelling its pause `sleep` -- a
    # `command sleep`, a `/bin/sleep`, a background timer -- because the ledger
    # then reads zero and the positive control in the case below refuses BY NAME
    # rather than reporting a very fast bound.
    #
    # It records ONLY the pauses this measurement's own flow performs. A pause the
    # poll loop waits on is spent by `run_bounded`; a background timer's `sleep` is
    # a marker running CONCURRENTLY and is not time the helper spends between
    # polls, so counting it would be counting the wrong quantity.
    #
    # Not hypothetical: #1066, landed as #1077, gives `run_bounded` a real deadline
    # as a backgrounded `( ...; sleep "$seconds"; ... ) &`, so on the merged tree
    # twenty calls of `run_bounded 5 true` added 20 x 5 s and the meter read
    # **100200 ms** instead of 200. The healthy case then reported `BUG:` on a good
    # tree. Neither branch is wrong alone; the combination is, which is the one
    # tree nothing builds.
    #
    # `BASH_SUBSHELL` and deliberately NOT `BASHPID`: the pid is bash 4.0+ and is
    # silently inert on the 3.2 macOS ships -- this file's own header records
    # `fail` losing a guard exactly that way -- where `BASH_SUBSHELL` is bash 3.0+
    # and answers on every platform this runs on. Confirmed on the 3.2 built to
    # diagnose this: `BASHPID` is unset there and `BASH_SUBSHELL` reads 0. A
    # backgrounded subshell is one level deeper; the poll loop is not.
    #
    # It fails CLOSED if the poll pause ever moves into a subshell too: the ledger
    # reads zero and the positive control below refuses it by name.
    elapsed="$( export LC_ALL=C; TIMEFORMAT='%3R'
        _fast_path_level=$BASH_SUBSHELL
        sleep() {
            if [ "$BASH_SUBSHELL" -eq "$_fast_path_level" ]; then
                printf '%s\n' "$1" >> "$ledger"
            fi
            command sleep "$@"
        }
        { time { for _ in $(seq 1 "$FastPathCommands"); do run_bounded 5 true >/dev/null 2>&1; done; }; } 2>&1 )"
    # REFUSED unless the reading has the shape `%3R` produces. Measured, and this
    # is the fail-OPEN direction rather than a tidiness check:
    #
    #   * a comma reading -- `0,243`, what a `de_DE.UTF-8` that survived the
    #     `LC_ALL=C` above would give -- evaluates to **243**, because bash's
    #     arithmetic reads `,` as the COMMA OPERATOR and yields its right-hand
    #     side. Not an error. A wrong small number that passes the ceiling.
    #   * a reading with no separator at all -- `4` -- becomes 4 milliseconds
    #     for four seconds of sleeping, and also passes.
    #
    # Both would report a healthy bound over a broken one, which is the one
    # outcome this check exists to prevent. `%3R` cannot produce either; a future
    # `TIMEFORMAT` edit can, and it would look like nothing had happened.
    if ! _is_duration_reading "$elapsed"; then
        echo "the fast-path timer read '${elapsed}' rather than a duration" >&2
        return 1
    fi
    # `%3R` is always `<seconds>.<three digits>`, so DELETING the separator is the
    # millisecond count -- no multiply to keep in step with the width. `10#`
    # because `0.060` would otherwise be read as octal.
    echo "$(( 10#${elapsed/./} ))"
}

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
        _selftest_node "$p" 1 "$log" "$E2eNodeReadyMarker" >/dev/null 2>>"$log" &
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
        _selftest_node "$p" never "$log" "$E2eNodeReadyMarker" >/dev/null 2>>"$log" &
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

    # --- `wait_for_daemon_ready` ----------------------------------------------
    #
    # The daemon's readiness line is `ready, accepting connections`, emitted once
    # the LAST acceptor has armed. `dist-compile-e2e.sh` started five daemons and
    # waited on the PORT for each, which is strictly weaker (#644).
    #
    # Two cases and the second is the one that discriminates. `waits-for-marker`
    # alone would pass a `wait_for_daemon_ready` that greps the NODE's marker, or
    # any other string, because the stand-in eventually logs and the port answers
    # throughout; `refuses-the-nodes-marker` stages the node's line and requires
    # the wait to expire on it, which is what pins the two constants apart.
    daemon-ready-waits-for-marker)
        log="${scratch}/daemon.log"
        : > "$log"
        p="$(free_port)"
        _selftest_node "$p" 1 "$log" "$E2eDaemonReadyMarker" >/dev/null 2>>"$log" &
        staged=$!
        # The bind is staged first for the reason `node-ready-waits-for-marker`
        # records at length: it is what puts the helper's own port wait into
        # #634's condition, and it stops a tight budget being spent on perl.
        wait_for_port 127.0.0.1 "$p" "$staged" "the staged daemon" "$log" 15
        wait_for_daemon_ready 127.0.0.1 "$p" "$staged" "the staged daemon" "$log" 15
        # Through the constant, never the text: an assertion spelling the marker a
        # second time agrees with itself whatever the helper does.
        case "$(<"$log")" in
            *"$E2eDaemonReadyMarker"*) echo "wait_for_daemon_ready returned with the daemon accepting" ;;
            *) echo "BUG: it returned while the daemon had only bound" ;;
        esac
        kill "$staged" 2>/dev/null || true
        ;;

    # A process logging the NODE's readiness line, which is not this one. Both
    # waits share `_e2e_wait_ready` now, so the marker is the only thing telling
    # them apart -- and a shared body that lost the parameter would wait for
    # whichever constant it hard-coded and pass one of the two cases either way.
    daemon-ready-refuses-the-nodes-marker)
        log="${scratch}/wrongmarker.log"
        : > "$log"
        p="$(free_port)"
        _selftest_node "$p" 1 "$log" "$E2eNodeReadyMarker" >/dev/null 2>>"$log" &
        staged=$!
        wait_for_port 127.0.0.1 "$p" "$staged" "a daemon logging the wrong line" "$log" 15
        wait_for_daemon_ready 127.0.0.1 "$p" "$staged" "a daemon logging the wrong line" "$log" 3
        echo "BUG: the wait returned for a process that logged the node's marker"
        ;;

    # --- `wait_for_counter` --------------------------------------------------
    #
    # The wait `dist-compile-e2e.sh` kept hand-written after every other one in it
    # had been converted, and the two things that cost (#643): it ignored
    # `e2e_wait_seconds`, and it produced no slow-versus-wedged verdict.
    #
    # THE BUDGET IS WHAT THESE ASSERT, and it is the reading that distinguishes.
    # `run_case` sets `e2e_wait_seconds 1` above, so a wait that honours it says
    # `of a 1s budget` and the hand-written one -- `local seconds=10` -- would have
    # said `of a 10s budget`. Whether the wait eventually fails is the reading that
    # does NOT distinguish: both do.
    #
    # Nothing ever answered a scrape. No listener at all, so the port is closed by
    # construction; `sleep` is the whole stand-in, because the wait needs a live
    # pid or it reports a death instead of a timeout.
    counter-never-answered)
        log="${scratch}/quiet.log"
        : > "$log"
        p="$(free_port)"
        sleep 30 >/dev/null 2>&1 &
        staged=$!
        wait_for_counter 127.0.0.1 "$p" staged_counter_total 1 \
            "$staged" "an exporter that never answers" "$log"
        echo "BUG: the wait returned with nothing listening"
        ;;

    # A process that DIED is reported the moment it is noticed, and the counter
    # finding prints BESIDE that rather than instead of it. A separate case from
    # the one above because the two reach `_e2e_expire` by different paths -- the
    # death arm inside the loop and the expiry after it -- so a findings hook
    # wired into only one of them passes the other.
    counter-dies)
        log="${scratch}/corpse.log"
        : > "$log"
        p="$(free_port)"
        ( exit 7 ) &
        corpse=$!
        sleep 0.4
        wait_for_counter 127.0.0.1 "$p" staged_counter_total 1 \
            "$corpse" "an exporter that is already gone" "$log" 10
        echo "BUG: the wait returned for a process that had exited"
        ;;

    # --- the counter finding, against staged records -------------------------
    #
    # `_e2e_counter_finding` is pure, so its three branches cost three rows here
    # rather than three arranged stand-ins -- `_e2e_verdict`'s argument, applied to
    # the second decision a counter wait makes. The stand-in-backed cases below
    # cover acquisition; this covers the decision, on every platform, including the
    # ones with no perl.
    counter-findings)
        for record in "no||1|c_total|nothing ever answered" \
                      "yes||1|c_total|exports no c_total series at all" \
                      "yes|0|1|c_total|never reached 1; the last reading was 0" \
                      "yes|2|3|c_total|never reached 3; the last reading was 2"; do
            old="$IFS"
            IFS='|' read -r fanswered fvalue ffloor fname fwant <<< "$record"
            IFS="$old"
            got="$(_e2e_counter_finding "$fanswered" "$fvalue" "$ffloor" "$fname")"
            grep -qF -- "$fwant" <<< "$got" \
                || fail "the finding for '${record}' lacks '${fwant}': ${got}"
        done
        # Three rows that all answer the same way would satisfy every assertion
        # above while testing nothing -- `verdict-branches`' argument, one decision
        # over. The fourth row in the loop reaches the same branch as the third
        # with a different reading, which is why it is not counted here.
        #
        # `${one%%$'\n'*}` and never `| head -1`: a pipe into an early-exiting
        # consumer under `pipefail` reports the PRODUCER's status, and this file
        # scans for exactly that a thousand lines below.
        distinct="$(
            for record in "no||1|c_total" "yes||1|c_total" "yes|0|1|c_total"; do
                old="$IFS"
                IFS='|' read -r fanswered fvalue ffloor fname <<< "$record"
                IFS="$old"
                one="$(_e2e_counter_finding "$fanswered" "$fvalue" "$ffloor" "$fname")"
                printf '%s\n' "${one%%$'\n'*}"
            done | sort -u | grep -c .
        )"
        [ "$distinct" = "3" ] \
            || fail "the three records produced ${distinct} distinct findings, not 3"
        echo "the counter finding named all three terminal states"
        ;;

    # --- the Prometheus grammar, against staged bodies ------------------------
    #
    # `metric_value` is pure and its grammar was driven by nothing: #643 moved it
    # here from `dist-compile-e2e.sh` and this file names it once, in a comment a
    # thousand lines below, so the decisions it makes have been asserted on no tree
    # at all. That is #597's finding one file over -- the coverage gap did not close
    # when the function moved, it relocated.
    #
    # A grammar is where a reading goes wrong QUIETLY. A wrong number looks like a
    # counter that did not move, which is a statement about the subject rather than
    # about the instrument, and every caller here then explains a healthy worker's
    # silence with a confident sentence.
    metric-value-grammar)
        body=$'worker_jobs_total 7\nworker_jobs_refused_total 0\n'

        [ "$(metric_value "$body" worker_jobs_total)" = "7" ] \
            || fail "a series was not read by its exact name"

        # ABSENT IS NOT ZERO, which is why this returns TEXT rather than a number: a
        # counter is a tally, so 0 is the truth about events that never happened,
        # while nothing exported is a counter that does not exist. Folding the two
        # would make every caller's emptiness check dead code -- and
        # `_e2e_counter_finding` above spends a whole terminal state on the
        # difference, so a helper that could not express it would leave that state
        # unreachable.
        [ -z "$(metric_value "$body" worker_jobs_dispatched_total)" ] \
            || fail "an absent series did not read empty"
        [ "$(metric_value "$body" worker_jobs_refused_total)" = "0" ] \
            || fail "a real zero did not read zero"

        # A PREFIX is not the name. The day the exporter grows a longer sibling, a
        # reader matching prefixes silently changes series -- a wrong number, which
        # is worse than an empty one and invisible at every call site.
        [ -z "$(metric_value "$body" worker_jobs)" ] \
            || fail "a prefix of a series name matched something"

        # A LABELLED series is a different grammar and is deliberately not read
        # here: answering it would mean picking one label set with nobody having
        # said which.
        [ -z "$(metric_value $'worker_jobs_total{tier="l1"} 9\n' worker_jobs_total)" ] \
            || fail "a labelled series was read as the bare one"

        # LAST wins: a scrape may carry a series more than once and the later
        # reading is the current one.
        [ "$(metric_value $'c 1\nc 4\n' c)" = "4" ] \
            || fail "the last reading of a repeated series did not win"

        echo "the Prometheus grammar refused a prefix, a label and an absent series"
        ;;

    # --- `wait_for_counter` against a real exporter ---------------------------
    #
    # `wait_for_port` first, with a bound of its own, so the counter wait starts
    # against a listener that is definitely up. Without it a slow `perl` start
    # would leave the 1s budget with no answered scrape, and the expiring cases
    # would report `nothing ever answered` -- a true sentence about a case that was
    # staged to test something else.
    counter-rises)
        log="${scratch}/rise.log"
        : > "$log"
        p="$(free_port)"
        _selftest_metrics "$p" rise "$log" &
        server=$!
        wait_for_port 127.0.0.1 "$p" "$server" "the staged exporter" "$log" 10
        wait_for_counter 127.0.0.1 "$p" staged_counter_total 1 \
            "$server" "the staged exporter" "$log" 10
        [ "$E2eCounterReading" = "1" ] \
            || fail "E2eCounterReading is '${E2eCounterReading}', not the reading 1"
        echo "wait_for_counter returned and handed back the reading 1"
        ;;

    # Present, answered, and never moving: the last reading is the finding, because
    # "it sat at 0" and "it reached 2 of 3" send a reader to different places.
    counter-flat)
        log="${scratch}/flat.log"
        : > "$log"
        p="$(free_port)"
        _selftest_metrics "$p" flat "$log" &
        server=$!
        wait_for_port 127.0.0.1 "$p" "$server" "the staged exporter" "$log" 10
        wait_for_counter 127.0.0.1 "$p" staged_counter_total 1 \
            "$server" "a counter that never moves" "$log"
        echo "BUG: the wait returned for a counter that never moved"
        ;;

    # Answered, and the series is not there at all. An absent series is not a
    # reading of zero, and the stand-in exports a different one so the distinction
    # is a fact about the body rather than about the connection.
    counter-absent)
        log="${scratch}/absent.log"
        : > "$log"
        p="$(free_port)"
        _selftest_metrics "$p" absent "$log" &
        server=$!
        wait_for_port 127.0.0.1 "$p" "$server" "the staged exporter" "$log" 10
        wait_for_counter 127.0.0.1 "$p" staged_counter_total 1 \
            "$server" "an exporter carrying another series" "$log"
        echo "BUG: the wait returned for a series that is not exported"
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

    # #709. The helper built to keep three states apart had a FOURTH it mapped onto
    # the happiest of them: `cat` failing was swallowed, so a record that was never
    # written or could not be read came back `finished` -- a killed canary reading
    # as a completed run.
    #
    # All four states, because a fix asserted only on the broken one would pass a
    # helper that refused unconditionally, which breaks every caller that bounded
    # nothing.
    bounded-outcome-absent-is-finished)
        # Nothing bounded yet. This is the reading the default exists for, and it
        # must survive the fix.
        rm -f "${_e2e_workdir}/.bounded-outcome"
        echo "no record at all: outcome=$(e2e_bound_outcome)"
        ;;

    bounded-outcome-empty-refuses)
        # A truncated or failed write. `finished` here is the #709 defect exactly.
        : > "${_e2e_workdir}/.bounded-outcome"
        echo "empty record: outcome=$(e2e_bound_outcome)"
        ;;

    bounded-outcome-garbage-refuses)
        # A partial write is not a verdict, and must not be passed through.
        printf 'fini' > "${_e2e_workdir}/.bounded-outcome"
        echo "partial record: outcome=$(e2e_bound_outcome)"
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
    # is fitting inside a CTest budget.
    #
    # TWENTY and not ten, and the count is load-bearing: at ten, the flat-pause
    # defect costs 2.05s, which is too close to the ceiling to separate from a
    # healthy run. The guard written to prove the fixture bites did not bite, on
    # the first thing it was pointed at. It is `FastPathCommands` and not a
    # literal in the loop, for the reason stated at that constant.
    #
    # TIMED HERE, not by the driver, which is #678. The driver used to wrap the
    # whole `--case` process in a `SECONDS` delta, and `SECONDS` is a whole
    # number sampled off a wall-clock boundary this process did not choose -- so
    # the SAME work reports `floor(T)` or `floor(T)+1` depending on where it
    # lands in the second. Measured on the host below, thirty healthy runs of
    # ~600ms: the old instrument reported `0` fourteen times and `1` sixteen
    # times for work that never varied by more than 400ms. That is a one-second
    # quantisation laid across a gap four seconds wide, and it is what made a
    # healthy run report `took 3s` and fail.
    #
    # Nothing forced the measurement outside. The two blocks that ARE timed by
    # the driver have the reason stated where they stand; it does not apply
    # here, because every command in this loop finishes normally. So the case
    # can hold a clock across its own loop -- at millisecond resolution, and
    # without paying for `bash` startup, `mktemp` and sourcing the library.
    #
    bounded-fast-path)
        # Worst healthy against best defective, measured on a 32-core Fedora
        # host, local ext4, staging the real defect (`_e2e_bounded_pauses=()`,
        # so every tick falls back to the flat `_e2e_poll_pause`):
        #
        #     load average    healthy          defective
        #     ~8-18           234-240 ms       4058-4073 ms
        #     ~19-51          388-436 ms       4231-4426 ms
        #     ~133-138        393-927 ms       not sampled; it only grows
        #
        # So: worst healthy 927 ms, at a load average of 138 on 32 cores -- four
        # times the core count, well past anything CI offers. Best defective
        # 4058 ms, and it is the IDLE row because load pushes the defect the
        # other way; both ends of the gap were sampled loaded and idle, so the
        # separation is not an artefact of one quiet moment.
        #
        # 2000 ms is 2.2x the worst healthy and 2.0x below the best defective --
        # the geometric midpoint of 927 and 4058 is 1939. The number the check
        # enforces has not moved from the old `-gt 2`; what moved is the
        # resolution, which is where the flake was.
        #
        # That table is one host on one afternoon, so it is not what the ceiling
        # RESTS on: `bounded-fast-path-bites` below re-derives the defective end
        # on every machine this check runs on, and fails if 2000 ms has stopped
        # separating. The table says where the number came from; that case says
        # it is still true here.
        #
        # And the resolution cuts BOTH ways, which is why this is not a widening.
        # Staging a PARTIAL regression -- the ramp raised "just a little", which
        # is `_e2e_bounded_pauses=(0.1)` -- costs 2064 ms. The old check timed
        # that as a `SECONDS` delta of 2 or 3 depending on phase, and `-gt 2`
        # fires on the 3 alone, so it caught a REAL regression about half the
        # time. This one fires on it every time.
        # THE VERDICT IS THE PAUSE ASKED FOR; THE ELAPSED TIME IS EVIDENCE (#1058).
        #
        # Everything above this line is true and none of it was enough, because
        # every figure in it is a WALL-CLOCK reading and a wall clock can be
        # STEPPED. Measured on a contended WSL2 host: twenty `sleep 0.2` calls,
        # no `run_bounded` and no subshell, timed by bash's `time`, by
        # `date +%s.%N` and by `/proc/uptime` at once --
        #
        #     run   time %3R   realtime   monotonic
        #     6     4.020      4.021      4.020
        #     7     2.761      2.763      4.030
        #     8     4.023      4.026      4.030
        #
        # -- the two CLOCK_REALTIME readers agreeing with each other and losing
        # 1.27s against CLOCK_MONOTONIC, which cannot be stepped. The sleeps were
        # real; the reading was not a duration. That is #678 coming back through
        # the other door: #678 was this verdict ruined by `SECONDS`'s one-second
        # quantisation and was fixed by keeping the clock and raising the
        # resolution, which is why the paragraphs above talk about resolution.
        # Resolution was never the whole defect.
        #
        # So the assertion is on what `run_bounded` ASKED FOR -- the sum of its
        # own pauses, recorded where it sleeps and read back exactly. That number
        # is a DECISION rather than a measurement, so no host and no clock can
        # move it, and it answers the question this case actually asks: does the
        # bound sleep through a command that has already finished. It is also
        # STRICTLY MORE than the timing proxy could see, since it counts the
        # ticks as well as their length -- a `run_bounded` that polled five times
        # per call instead of once fails here and was invisible to a ceiling that
        # the ramp still fitted under.
        #
        # The elapsed time keeps being measured and printed, and can no longer
        # FAIL anything. `.agent/rules/testing.md` requires a bounded wait to say
        # which kind of failure it met; a slow host and a helper that did not
        # bite are fixed by different people, and now the two are separate lines.
        # A reading this host cannot produce is REPORTED, not fatal -- and that is
        # the other half of demoting the clock. `fast_path_ms` refuses a malformed
        # reading and returns non-zero, which under `set -e` used to end the case:
        # exactly how `/.044` took this case red once in 30 runs. A refusal is
        # right while the reading is the VERDICT and wrong once it is only
        # evidence, so the status is taken here instead of ending the run.
        if fast_ms="$(fast_path_ms)"; then reading=1; else reading=0; fast_ms=""; fi
        asked_ms="$(fast_path_asked_ms)"
        if [ "$reading" -eq 1 ]; then
            echo "${FastPathCommands} immediate commands asked for ${asked_ms}ms of pause; the wall clock read ${fast_ms}ms (ceiling ${FastPathCeilingMs}ms)"
        else
            echo "${FastPathCommands} immediate commands asked for ${asked_ms}ms of pause; the wall clock produced no usable reading, and is not the verdict (ceiling ${FastPathCeilingMs}ms)"
        fi
        # A census returning zero needs a positive control. Without this the whole
        # case passes vacuously the day the record stops being written: the
        # ceiling comparison would read `0 -gt 2000` and be delighted, which is
        # precisely the regression a reviewer replacing `_e2e_bounded_pause` with
        # a bare `sleep` would introduce.
        #
        # It reads zero as LOST rather than as a very fast bound, and the one host
        # property that would make that wrong is bash reaping the background child
        # before the very next builtin runs, which would cost a call zero polls.
        # Measured, 40 runs of this case: **800 of 800 calls polled exactly once**,
        # every total 200 ms, none lower. Stated rather than assumed, because it is
        # a property of the shell's SIGCHLD timing and not of anything here -- and
        # a host that did reap that fast would report this as a defect when the
        # fast path had in fact become perfect.
        if [ "$asked_ms" -le 0 ]; then
            echo "BUG: run_bounded recorded no pause at all over ${FastPathCommands} commands,"
            echo "BUG: so the ledger was lost and this case asserted nothing"
        elif [ "$asked_ms" -gt "$FastPathCeilingMs" ]; then
            echo "BUG: the bound is sleeping through commands that have already finished"
        elif [ "$asked_ms" -gt "$FastPathHealthyMaxMs" ]; then
            # NOT the same failure as the arm above, and the two are fixed by
            # different people. Over the CEILING says the bound is broken. Over this
            # bound says the bound still passes and has stopped separating anything --
            # the healthy end has drifted up towards a ceiling that is standing still,
            # which no reading in this file would otherwise report.
            echo "BUG: a healthy run asked for ${asked_ms}ms, over the ${FastPathHealthyMaxMs}ms"
            echo "BUG: bound, so the ${FastPathCeilingMs}ms ceiling no longer separates a healthy"
            echo "BUG: run from a broken one even though this case still passes it"
        elif [ "$reading" -eq 1 ] && [ "$fast_ms" -gt "$FastPathCeilingMs" ]; then
            # THE FOURTH READING. Three causes can put the measured cost over the
            # ceiling while the pause asked for stays normal, and they are fixed
            # by three different people:
            #
            #   * the host was slow, or its clock stepped forward (#1058);
            #   * `run_bounded` got MORE EXPENSIVE PER CALL -- twenty of them land
            #     inside this measurement, so a change that adds one fork per call
            #     arrives here as a breach and reads exactly like a slow host.
            #
            # Neither can be told from the other in ONE reading, so this does not
            # guess. It prints the per-call overhead, which is the quantity that
            # actually separates them, and names both causes -- the rule this
            # repository already states for a lower Raft term: report what is
            # OBSERVED, name both causes, and say that the RATE separates them. A
            # confident wrong signal is worse than a vague right one.
            #
            # The baseline is `FastPathBaselineMsPerCall`, which carries the
            # measurement and the conditions it was taken under. NAMED here and
            # not restated: the version of this that wrote the number out in the
            # message below drifted the day #1077 added a fork per call, while
            # the paragraph forbidding exactly that sat four lines above it.
            overhead_ms=$(( fast_ms - asked_ms ))
            echo "SLOW: run_bounded asked for ${asked_ms}ms of pause, well inside the ${FastPathCeilingMs}ms"
            echo "SLOW: ceiling, and the wall clock still read ${fast_ms}ms -- ${overhead_ms}ms of overhead"
            echo "SLOW: over ${FastPathCommands} commands, $(( overhead_ms / FastPathCommands ))ms per call against a ${FastPathBaselineMsPerCall}ms baseline."
            echo "SLOW: That is the host, its clock, or a run_bounded that got more expensive per"
            echo "SLOW: call -- one reading cannot say which, and the rate does. Reported, not failed."
        fi
        ;;

    # THE GUARD ABOVE, WATCHED REFUSING -- on this machine, on this run, rather
    # than on the one host the table was measured on.
    #
    # Without this, the whole separation lives in a comment: nothing on a 2-core
    # runner ever confirms that 2000 ms still sits between a healthy `run_bounded`
    # and a broken one, and the first sign it does not would be #678 coming back
    # wearing the other face -- a guard that no longer fires. This file has that
    # exact history: the comment above records the twenty-count being raised from
    # ten precisely because the guard written to prove the fixture bites did not
    # bite, on the first thing it was pointed at.
    #
    # The defect is staged the way it would really arrive, by emptying the ramp so
    # every tick falls back to the flat `_e2e_poll_pause` -- the shape
    # `run_bounded` had before the ramp existed. The assignment is visible inside
    # `fast_path_ms` because a command substitution is a subshell of this one, and
    # an empty array is already the case `run_bounded` handles by falling through
    # to `${...:-$_e2e_poll_pause}`.
    #
    # It asserts against `FastPathDefectFloorMs` and not against the ceiling
    # itself, for the reason stated at that constant: a defect that clears the
    # ceiling by 2.5% has demonstrated no separation, and that is exactly what a
    # halved `FastPathCommands` would produce.
    #
    # An ORDINARY ROW rather than a second bespoke block: it asserts a threshold
    # and prints nothing anyone needs on the passing path, so `expect` covers it.
    bounded-fast-path-bites)
        # Against the pause ASKED FOR, for the reason the case above gives at
        # length -- and PER PAUSE, for the reason `FastPathDefectFloorMsPerPause`
        # gives (#1196). The acquisition happens here; the DECISION is
        # `fast_path_defect_verdict`, driven in both directions from staged
        # records so this case is not the only thing that has ever exercised it.
        #
        # The count is REPORTED whatever the verdict, because it is the quantity
        # that moves between an idle box and a loaded runner, and a line stating
        # only the total invites exactly the inference that made the old floor
        # look host-independent.
        _e2e_bounded_pauses=()
        # Reported and not fatal, for the reason the case above gives.
        if fast_ms="$(fast_path_ms)"; then reading=1; else reading=0; fast_ms=""; fi
        asked_ms="$(fast_path_asked_ms)"
        pauses="$(fast_path_pause_count)"
        if [ "$reading" -eq 1 ]; then
            clock="the wall clock read ${fast_ms}ms"
        else
            clock="the wall clock produced no usable reading"
        fi
        if [ "$pauses" -ge 1 ]; then
            per_pause=" ($(( asked_ms / pauses ))ms each)"
        else
            per_pause=""
        fi
        echo "the staged flat-pause defect asked for ${asked_ms}ms of pause over ${pauses} pause(s) of ${FastPathCommands} commands${per_pause}; ${clock} (ceiling ${FastPathCeilingMs}ms, floor ${FastPathDefectFloorMsPerPause}ms per pause)"
        fast_path_defect_verdict "$asked_ms" "$pauses"
        ;;

    # THE SHAPE GUARD IN `fast_path_ms`, WATCHED REFUSING -- and watched
    # ACCEPTING, because a predicate that refuses everything passes the refusing
    # half on its own.
    #
    # Every rejected reading here is a WRONG SMALL NUMBER rather than an error:
    # `0,243` evaluates to 243 through bash's comma operator, and a separatorless
    # `4` becomes 4ms for four seconds of sleeping. Both would report a healthy
    # bound over a broken one, which is this check's one intolerable outcome, so
    # the arm that stops them is asserted rather than assumed.
    #
    # **"`%3R` produces none of them today" stood here and is FALSE** -- it was
    # written expecting a `TIMEFORMAT` edit to be what broke the shape tomorrow.
    # `%3R` produced `/.044` on an ordinary run of this check (#1058): the wall
    # clock stepped backwards far enough that the interval ended before it began,
    # and bash renders a negative second by decrementing the digit character, so
    # `'0' - 1` is `/`. The guard caught it and refused BY NAME, which is the only
    # reason the run said something true rather than reporting 44 ms for a third
    # of a second of work.
    #
    # So this is no longer a predicate kept against a hypothetical future edit. It
    # has been watched firing on real output, and what produces the malformed
    # reading is the host, not the format string.
    duration-reading-shape)
        # A TALLY and not a fixed sentence, so the summary line is a second
        # signal rather than a marker that prints whatever the arms did: a
        # predicate stuck on one answer moves the count as well as raising
        # `BUG:`, and the record below asserts both.
        accepted=0
        refused=0
        for reading in 0.000 0.243 12.345; do
            if _is_duration_reading "$reading"; then
                accepted=$(( accepted + 1 ))
            else
                echo "BUG: '${reading}' is what %3R produces and was refused"
            fi
        done
        # In order: the de_DE separator, no separator at all, nothing at all, two
        # fractional digits, four of them, a leading diagnostic, a trailing one.
        for reading in "0,243" "4" "" "0.24" "0.2431" "x0.243" "0.243 stray"; do
            if _is_duration_reading "$reading"; then
                echo "BUG: '${reading}' is not a duration and was accepted"
            else
                refused=$(( refused + 1 ))
            fi
        done
        echo "the timer shape guard accepted ${accepted} readings and refused ${refused}"
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

    # `reap_background_jobs` reaps what NOBODY RECORDED, which is the whole of
    # #845: the ledger it replaces was per-site, so the next background site
    # reopened the leak by forgetting one line.
    #
    # The assertion is therefore about a job this case deliberately keeps no
    # record of beyond what it needs to CHECK the outcome. A case that handed the
    # pid to the reaper would pass under a reaper that only read a ledger, which
    # is the thing being removed -- assert what DISTINGUISHES.
    #
    # Both directions in one case, because a reaper that killed nothing and one
    # that hung are different defects and the pass has to exclude both:
    #
    #   * an ORDINARY child, which dies on TERM inside the grace;
    #   * a TERM-IGNORING child, which must be escalated to SIGKILL -- the shape
    #     `bounded-outlasts-a-trapped-term` above stages for the same reason, and
    #     the one a bare `wait` in cleanup would hang on forever.
    #
    # `E2eReapKilled` is what says the escalation ran rather than the grace being
    # long enough by luck: a reaper whose KILL arm was dead would still reach zero
    # survivors for the polite child and would leave the stubborn one alive, so
    # the two numbers together discriminate where either alone does not.
    reap-takes-what-nothing-recorded)
        sleep 30 &
        polite=$!
        sh -c 'trap "" TERM; exec sleep 30' &
        stubborn=$!
        # Not `wait_for_port`: nothing binds here. A short pause so both children
        # have execed before the signal, since a TERM delivered to a shell that
        # has not yet run `exec` would kill the wrapper and prove nothing about
        # the escalation.
        sleep 0.5
        reap_background_jobs 2
        killed="$E2eReapKilled"
        alive=""
        kill -0 "$polite" 2>/dev/null && alive="${alive} polite"
        kill -0 "$stubborn" 2>/dev/null && alive="${alive} stubborn"
        echo "reaped without a ledger: escalated=${killed}, still alive:${alive:- none}"
        [ -z "$alive" ] || echo "BUG: reap_background_jobs left${alive} running"
        [ "$killed" = "1" ] \
            || echo "BUG: expected exactly one SIGKILL escalation, got ${killed} -- a dead KILL arm and a lucky grace read alike"
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

    # --- the two canaries for `--case`'s own verdict ------------------------
    #
    # In no table, and driven only by the `--case verdict` block far below. They
    # are the pair that makes `--case` a guard rather than a mode nobody has
    # watched refuse (#1104): they differ in ONE thing, the `BUG:` prefix, so the
    # refusing arm cannot be passing for some other reason and the accepting arm
    # cannot be passing because everything passes.
    #
    # A canary and not a real case, because every real case here reports `BUG:`
    # only when a helper is broken -- so there is no case that can be relied on to
    # print one, and asserting the refusing direction against a genuine defect
    # would mean keeping a broken helper in the tree.
    #
    # `run_case` has already sourced the library and armed cleanup by this point,
    # so both arms exercise the whole `--case` path and not a short-circuit.
    selftest-case-clean)
        echo "the canary ran and found nothing"
        ;;
    selftest-case-bug)
        echo "the canary ran and found something"
        echo "BUG: this line is what --case must refuse on"
        ;;

    *)
        echo "unknown case: ${name}" >&2
        exit 2
        ;;
    esac
}

# ---------------------------------------------------------------------------
# The one door every perl stand-in goes through
# ---------------------------------------------------------------------------
#
# Run a perl program as THIS process, under a lifetime bound it cannot omit.
#
# ## What it owns, and why it is one function rather than a convention
#
# Every stand-in below needs the same PAIR, and neither half is optional:
#
#   `exec`  -- `$!` for a backgrounded shell FUNCTION is the subshell bash forks,
#              not the program that subshell goes on to run. Without it every
#              `kill "$listener"` in this file reaps a wrapper and leaves perl
#              alive, reparented, still holding its LISTEN socket (#839).
#   `alarm` -- no trap runs under `SIGKILL`, a `ctest --timeout` or a cancelled
#              CI job, and those are the paths a leak actually accumulates on.
#              #839 measured what that costs: **1368 orphan listeners holding
#              loopback ports, the oldest 30.5 hours old**, on a fixture in the
#              DEFAULT ctest set on every platform CI builds. The expensive half
#              was the PORTS -- these fixtures draw from below the ephemeral
#              range, so the next run meets a port held by a process nobody knows
#              about and fails somewhere else entirely.
#
# They are INDEPENDENT and a survivor count cannot tell you whether either works:
# each alone drives the count to zero for a different reason, so a count reads as
# "both arms fine" while one is dead. #839's arm-independence table is what shows
# the third arm is doing real work rather than belt-and-braces, and it is quoted
# in #843 rather than restated here.
#
# Three stand-ins each spelled that pair by hand, so a fix to one reached none of
# the others (#1214) -- and the arm that can be reopened by omission is `alarm`,
# because a stand-in written without `exec` fails LOUDLY the moment the existing
# `kill` stops working. #843 is the ticket, and it happened rather than being
# hypothetical: PR #834 added `_selftest_unprompted_listener` with no bound at
# all, while the ticket about bounds was open and its diagnosis was written down.
#
# So the pair rides on the thing every stand-in must do anyway -- launching its
# perl -- and there is no argument to pass an unbounded program to. This is the
# same idiom as `Refuse` taking a row and `SigningDomain` being a required
# parameter. `check-e2e-perl-bounds` (further down) is what stops a new stand-in
# spelling `perl` for itself and bypassing the door.
#
# ## How the bound is injected without touching the program
#
# `perl` accepts several `-e` chunks and joins them, in order, into ONE program.
# So the bound is its own chunk and the caller's body is passed through verbatim:
# the three bodies stay textually distinct, which is #1214's own constraint --
# they model three different things and concatenating perl program text as
# strings is the hazard the ticket exists to avoid, not the fix.
#
# Measured (perl 5.38.2, Linux): the two chunks compose in order, `@ARGV` after
# `--` is exactly the caller's arguments, `alarm(0)` read from the SECOND chunk
# reports 30 still pending, and a program that would run 60 s dies at 3 s with
# status 142 when armed for 3. Control: the same program with no bound chunk
# survives.
#
# ## Why the bodies wait with `select` and not `sleep`
#
# perldoc warns that `sleep` may be implemented with `alarm` on some systems, and
# the two must not then overlap -- which is why `_selftest_listener` used to arm
# its own alarm AFTER its delay rather than before. Arming here means arming
# first, so that ordering is no longer available and the question has to be
# closed rather than sequenced around.
#
# Measured on this platform it is a non-issue: `alarm 3; sleep 1; sleep 30` dies
# at exactly 3.00 s over three runs, with both controls (alarm alone dies at
# 3.00, no alarm survives). But macOS ships its own perl and cannot be measured
# from here, so the bodies use `select(undef, undef, undef, N)` -- perldoc's own
# alarm-safe spelling of a pause -- and the question does not arise on any
# platform. Stated as MEASURED on Linux and INFERRED nowhere else, deliberately.
#
# @param 1 the lifetime bound in whole seconds; refused unless positive
# @param 2 the perl program, single-quoted at the call site so the shell expands
#          nothing in it
# @param 3.. arguments, which the program reads from @ARGV
_selftest_bounded_perl() {
    local seconds="$1" program="$2"
    shift 2
    # A bound is REQUIRED and must be a positive whole number. `alarm 0` is
    # perl's spelling of *cancel the alarm*, so a `0` here would read at the call
    # site as a bound and be the absence of one -- an escape hatch wearing the
    # shape of the guard, which is the failure this whole door exists to close.
    case "$seconds" in
        ''|*[!0-9]*) fail "_selftest_bounded_perl: '${seconds}' is not a whole number of seconds" ;;
        0) fail "_selftest_bounded_perl: a bound of 0 cancels the alarm; there is no unbounded spelling" ;;
    esac
    [ -n "$program" ] || fail "_selftest_bounded_perl: no program given"
    # The trailing marker is what `perl-bounds-scan` further down reads. This is
    # the one `perl` command position in the tree allowed to name a program of
    # its own, because it is the line that ARMS the bound every other one
    # inherits -- so the scan cannot simply refuse every `perl`, and the claim
    # has to be stated where it can be read back.
    exec perl -e "alarm ${seconds};" -e "$program" -- "$@" # perl-lifetime: this line IS the injector
}

# The bound every stand-in below runs under, in seconds. One number, because
# nothing here has measured a reason to differ and a per-stand-in constant is a
# second thing to drift. It outlives every case in this file by a wide margin:
# the longest staged delay is 1 s and the longest wait a case arms is far short
# of this.
_selftest_perl_lifetime=30

# A listener that answers one request per connection with a body ending in NO
# newline, and records the request headers it was sent.
#
# @param 1 port
# @param 2 seconds to wait before binding -- so a caller can prove the wait
#          POLLS rather than happening to be called after the bind
# @param 3 file to record request lines in
_selftest_listener() {
    # `exec` and the `alarm` bound both come from `_selftest_bounded_perl`, which
    # is where the whole argument for them lives. What stays here is the one part
    # that is about THIS stand-in.
    #
    # Only ever called with `&`. The door `exec`s, so in the FOREGROUND this would
    # replace the calling shell -- a new call site backgrounds it or does not use
    # it. That sentence is the ONLY guard on that, and it is worth saying why
    # rather than leaving the next reader to wonder whether one was forgotten.
    # Enforcing it means asking "am I in a subshell", which needs `BASHPID` --
    # bash 4.0+, and UNSET on the macOS 3.2 this script also runs under, so the
    # check would hold on Linux and be silently inert on the platform it matters
    # on. That is the exact trap this file already records in its own bash 3.2
    # table, where `BASHPID` is a banned construct for the same reason: a guard
    # that cannot fire everywhere is worse than a comment, because it reads as
    # enforcement. A rule nothing can express is a rule nothing can be held to,
    # so this one is written down instead of pretended at.
    _selftest_bounded_perl "$_selftest_perl_lifetime" '
        use strict; use warnings; use IO::Socket::INET;
        my ($port, $delay, $logfile) = @ARGV;
        # The delay is what lets a caller prove a wait POLLS rather than
        # happening to be called after the bind, so it runs BEFORE the listen.
        #
        # `select` rather than `sleep`: the bound is already armed by the time
        # this program starts, and perldoc warns that `sleep` may be implemented
        # with `alarm` on some systems. The reasoning, and what was measured, is
        # at `_selftest_bounded_perl`.
        select(undef, undef, undef, $delay) if $delay;
        # The bound is a TIME and deliberately not a connection count.
        # `port_answers` is `/dev/tcp`, so every `free_port` draw and every
        # `wait_for_port` poll costs this loop an `accept()`; a loop bounded by
        # connections would exit before the request under test arrived, and the
        # case would then fail for a reason having nothing to do with the helper
        # it exists to test. There is deliberately no SIGALRM handler -- the
        # default disposition terminates, and that is what interrupts the
        # blocking `accept()`.
        #
        # NOTE: this program is a shell single-quoted string, so no apostrophe
        # may appear anywhere inside it, comments included. One here ended the
        # quote and the script died at PARSE time -- which leaks nothing, and is
        # therefore indistinguishable, by a count of survivors alone, from the
        # fix working -- which is the argument this file makes in its own
        # header, arriving from inside the file. Passing the body as an ARGUMENT
        # to the door does not change that: it is still single-quoted here.
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
# Its lifetime pair -- `exec` and the `alarm` bound, why neither closes the other's
# hole, and why a survivor COUNT cannot tell you whether either works -- is
# `_selftest_bounded_perl`'s, which every stand-in here goes through since #1214.
# This one is the reason that ticket was filed: #834 added it with no bound at all
# while #843 was open, and its `hold` mode is the worst of the three to leak, since
# it accumulates accepted CLIENT sockets as well as the listening port.
_selftest_unprompted_listener() {
    _selftest_bounded_perl "$_selftest_perl_lifetime" '
        use strict; use warnings; use IO::Socket::INET;
        my ($port, $mode, $logfile) = @ARGV;
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


# A stand-in for an admin endpoint serving `/metrics`, in one of three shapes.
#
# `_selftest_listener` above cannot stage any of them: it answers one fixed JSON
# body, so every scrape reads the same thing and neither the rising case nor the
# absent-series case exists. The three modes are `wait_for_counter`'s three
# terminal states seen from the other side of the socket:
#
#   rise    the series starts at 0 and reaches 1 from the second request on, so
#           the wait RETURNS and hands back a reading
#   flat    the series is there and never moves, so the wait expires knowing the
#           last reading
#   absent  the body carries a different series, so the wait expires knowing the
#           scrape was answered and this counter is not exported
#
# The fourth state -- nothing ever answered -- needs no listener at all and is
# staged by drawing a port and not binding it, which is why that case is in the
# plain table rather than here.
#
# The body's lines end with `\n` and the headers with `\r\n`, because that is what
# a real exporter writes and `metric_value` anchors its expression on `$`: a body
# with CRLF line endings would match nothing, and a stand-in that used them would
# make every case here fail for a reason having nothing to do with the wait.
#
# It APPENDS a line to the log per request served, so the expiring cases exercise
# a log that grows -- which is what makes their output show a general verdict
# saying the process was making progress beside a COUNTER finding saying what
# actually went wrong. A stand-in that logged nothing would collapse the two.
#
# `exec` and the lifetime bound come from `_selftest_bounded_perl`. This is the
# FOURTH stand-in and the one that made #1214 a ticket rather than a tidy-up: it
# was added by a branch in flight while the door was being written on another, so
# it spelled the pair by hand and nothing but a scan could have said so. That the
# collision announced itself here -- `perl-bounds-scan` refusing this line by name
# at the rebase -- is the intended failure and is why the scan walks rather than
# reading a list. Only ever called with `&`, since the door `exec`s.
#
# NOTE: a shell single-quoted string, so no apostrophe may appear anywhere inside
# it, comments included.
#
# @param 1 port
# @param 2 mode: rise | flat | absent
# @param 3 the log to append one line per request to
_selftest_metrics() {
    _selftest_bounded_perl "$_selftest_perl_lifetime" '
        use strict; use warnings; use IO::Socket::INET;
        my ($port, $mode, $logfile) = @ARGV;
        my $srv = IO::Socket::INET->new(
            LocalAddr => "127.0.0.1", LocalPort => $port,
            Listen => 5, ReuseAddr => 1, Proto => "tcp") or die "listen: $!";
        open(my $log, ">>", $logfile) or die $!;
        $log->autoflush(1);
        my $served = 0;
        while (my $c = $srv->accept()) {
            my $lines = 0;
            while (defined(my $l = <$c>)) { $lines++; last if $l =~ /^\r?\n?$/; }
            # A CONNECT THAT SENT NOTHING IS NOT A REQUEST. port_answers opens the
            # socket and closes it, so every free_port draw and every wait_for_port
            # poll reaches this accept -- and counting those would push the rise
            # mode past its threshold before the first scrape, leaving a case that
            # returns on its first poll and demonstrates no polling at all.
            if (!$lines) { close $c; next; }
            $served++;
            my $body;
            if ($mode eq "absent") {
                $body = "some_other_total 7\n";
            } elsif ($mode eq "flat") {
                $body = "staged_counter_total 0\n";
            } else {
                $body = "staged_counter_total " . ($served >= 2 ? 1 : 0) . "\n";
            }
            print $log "served request $served\n";
            print $c "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                   . "Connection: close\r\n\r\n" . $body;
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
# THE MARKER IS A PARAMETER, because `fastcached` has one of these lines too and
# it is a different one -- `ready, accepting connections`, from `ReadinessAnnouncer`
# (#644). Two stand-ins would be two copies of a program whose every subtlety is
# recorded here and in `_selftest_listener`, and the `exec`/lifetime pair is exactly
# the kind that gets fixed in one copy. The CALLER passes the library constant, so
# nothing here spells the node's text a third time -- the line below is what a real
# node writes, which is staging rather than asserting.
#
# @param 1 port
# @param 2 seconds to wait before logging the marker, or `never`
# @param 3 the log to write it to
# @param 4 the marker text to log, e.g. `$E2eNodeReadyMarker`
_selftest_node() {
    # `exec` and the `alarm` bound come from `_selftest_bounded_perl`. Only ever
    # called with `&`, since the door `exec`s: `$!` must be this perl and not the
    # subshell bash forks for a backgrounded function, or the `kill "$staged"` at
    # the call site signals a wrapper and leaves this process holding its port.
    _selftest_bounded_perl "$_selftest_perl_lifetime" '
        use strict; use warnings; use IO::Socket::INET;
        my ($port, $delay, $logfile, $marker) = @ARGV;
        my $srv = IO::Socket::INET->new(
            LocalAddr => "127.0.0.1", LocalPort => $port,
            Listen => 5, ReuseAddr => 1, Proto => "tcp") or die "listen: $!";
        if ($delay ne "never") {
            select(undef, undef, undef, $delay);
            open(my $log, ">>", $logfile) or die $!;
            print $log "$marker on 127.0.0.1:$port, and then whatever else the line carries
";
            close $log;
        }
        # The HOLD, which is what this stand-in is for: `wait_for_node_ready`
        # polls the log with the process still under liveness watch, so one that
        # exited after writing the marker would be reported as having DIED --
        # a different verdict, and a passing test for the wrong reason.
        #
        # It is also a SECOND bound, and that is deliberate rather than
        # redundant: it was this stand-in ONLY bound until #1214, and if the
        # injected alarm ever stopped arming, this ends the process anyway. Two
        # arms, each sufficient, which is #839 arriving at the same shape from
        # the other side. `select` for the reason the other bodies give.
        select(undef, undef, undef, 30);
    ' "$1" "$2" "$3" "$4"
}

# ---------------------------------------------------------------------------
# The driver
# ---------------------------------------------------------------------------

# `--case-body NAME` is the INNER half: it runs one case and its status answers
# only *did this case run* -- non-zero when the case aborted (a `fail`, or the
# `set -e` inside `run_case` firing), zero when it reached the end. That question
# is a real one and the five internal consumers below have always read it
# correctly, which is why the bare `exit 0` survived so long.
#
# It is not, however, the question somebody re-running a case is asking. The
# failure block at the bottom of this file prints
#
#     re-run one alone with: bash check-e2e-helpers.sh --case <name>
#
# and sends an investigator to a mode whose status could not say whether the case
# found a defect: a run printing `BUG: the bound is sleeping through commands that
# have already finished` exited 0, exactly as a healthy run did. A lane followed
# that advice, built a harness on the status, and reported 6 of 6 PASS including a
# run that should have failed -- nothing errored, the output was well-formed, and
# the status was simply not a verdict (#1104).
#
# So `--case` WRAPS it and applies the SAME rule the driver applies: the absence of
# `BUG:` from the case's output. One rule, one text, two readers -- rather than a
# marker beside the printing, which would be a second mechanism free to disagree
# with the first.
#
# THREE outcomes, not two, because *aborted* and *ran and reported a defect* are
# different diagnoses fixed in different places:
#
#     0  the case ran and printed no BUG:
#     3  the case ran to completion and reported a defect
#     *  whatever the case exited with -- it aborted, and did not reach the end
#
# An inner PROCESS rather than a redirection: `fail` sends SIGTERM to
# `_e2e_top_pid`, which `e2e_begin` sets to `$$` -- so the case must own that pid,
# or a `fail` would kill the wrapper mid-read and take the output with it. A
# `tee` keeps the case streaming live, which matters because several cases are
# about waits and hangs. Its cost is one extra process per case.
#
# The two streams are MERGED here, as all five consumers already merge them with
# their own `2>&1`, so a `BUG:` on either is seen and the ordering a consumer
# captures is the ordering of one stream rather than a race between two.
if [ "${1:-}" = "--case-body" ]; then
    run_case "$2"
    exit 0
fi

if [ "${1:-}" = "--case" ]; then
    caseLog="$(mktemp)" || { echo "could not create a scratch file for --case" >&2; exit 2; }
    trap 'rm -f "$caseLog"' EXIT
    bash "${BASH_SOURCE[0]}" --case-body "$2" 2>&1 | tee "$caseLog"
    caseStatus=${PIPESTATUS[0]}
    if [ "$caseStatus" -ne 0 ]; then
        exit "$caseStatus"
    fi
    # A FILE, not a pipe: `grep -q` over a pipe is the SIGPIPE hazard this file's
    # own early-exit scan refuses.
    if grep -q 'BUG:' "$caseLog"; then
        echo "--case ${2}: the case ran to completion and printed BUG:, so it reported a defect" >&2
        exit 3
    fi
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
#     what  bound  elapsed  polls  alive  exit  logGrew  stall  requestedMs
#
# all durations in MEASURED seconds except the last, which is the total the loop
# ASKED FOR in milliseconds and is the one figure here that consults no clock
# (#1066). With bound=20 the "recent" window is 20/4 = 5s, so a 5s stall is
# progressing and a 6s one is stalled; and the starvation note fires BELOW
# bound x 500 ms, so 10000 is quiet and 9999 is not.
#
# Every row states the last field even where it is irrelevant, rather than letting
# it default: a row that omits a field is one nobody can read the intent of, and
# `-` here is a THIRD state -- the caller took no such reading -- which gets its own
# row rather than being the shape every other row happens to have.
verdicts=(
    "died|worker|20|20|98|no|3|no|-|19000|the process DIED"
    "died-status-unknown|worker|20|20|98|no|-|no|-|19000|exit=-"
    "no-process|worker|20|20|98|unknown|-|unknown|-|19000|INCONCLUSIVE|No process was watched"
    "no-log|worker|20|20|98|yes|-|unknown|-|19000|INCONCLUSIVE|no log was watched"
    "silent|worker|20|20|98|yes|-|no|-|19000|logged NOTHING for the whole 20s"
    "progressing-at-bound|worker|20|20|98|yes|-|yes|5|19000|still making progress"
    "stalled-one-second-past|worker|20|20|98|yes|-|yes|6|19000|stopped making observable progress"
    "progressing-fresh|worker|20|20|98|yes|-|yes|0|19000|still making progress"
    "stalled-cold|worker|20|20|98|yes|-|yes|20|19000|stopped making observable progress"
    # The measured elapsed is what gets printed, never the budget.
    "reports-measured-not-nominal|worker|20|37|61|yes|-|no|-|19000|waited 37s (measured, over 61 polls) of a 20s budget"
    # The starvation note, pinned on both sides of bound x 500 ms and one unit apart,
    # plus the reading the caller did not take.
    "starved-quiet-at-threshold|worker|20|22|98|yes|-|no|-|10000|!NOTE: the loop asked"
    "starved-named-below-threshold|worker|20|22|98|yes|-|no|-|9999|NOTE: the loop asked for only 9999ms of pauses inside a 20s budget"
    "starved-unknown-claims-nothing|worker|20|22|98|yes|-|no|-|-|!NOTE: the loop asked"
    # A wait that ended EARLY spent no budget, so the pacing comparison -- which is
    # against the budget -- claims nothing. Pinned in both directions and one row
    # apart in `alive` alone: the death row must NOT carry the note while the
    # no-pid row, which did run to its budget, must. Without the second the
    # suppression could be unconditional and both rows would still pass.
    "starved-silent-on-death|worker|10|0|1|no|7|no|-|0|the process DIED|!NOTE: the loop asked"
    "starved-named-when-nothing-watched|worker|10|11|3|unknown|-|unknown|-|0|NOTE: the loop asked for only 0ms"
)

echo "== the verdict, against staged records"
for row in "${verdicts[@]}"; do
    old="$IFS"
    IFS='|' read -r vname vwhat vbound vsecs vpolls valive vstatus vgrew vstall vreq vrest <<< "$row"
    IFS="$old"
    out="$( . "$library"
            _e2e_verdict "$vwhat" "$vbound" "$vsecs" "$vpolls" "$valive" "$vstatus" "$vgrew" "$vstall" "$vreq" 2>&1 )"
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
        IFS='|' read -r vname vwhat vbound vsecs vpolls valive vstatus vgrew vstall vreq vrest <<< "$row"
        IFS="$old"
        ( . "$library"
          _e2e_verdict "$vwhat" "$vbound" "$vsecs" "$vpolls" "$valive" "$vstatus" "$vgrew" "$vstall" "$vreq" 2>&1 ) \
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
# `_e2e_read_ended_at_bound` decides whether OUR bound or the PEER ended a read. It
# is driven DIRECTLY here rather than through a staged listener because half of it
# cannot be exhibited on this platform at all: bash 3.2, which macOS ships as
# `/bin/bash`, answers a plain `1` for both a timeout and EOF, so the status arm is
# unreachable there and the probe arm is unreachable on bash 4+.
# `.agent/rules/testing.md`, on splitting the decision out as a pure function over a
# record so a verdict needing a rare machine to reproduce needs one line here.
#
# **These rows no longer carry an elapsed time, and that is #1048.** The predicate
# used to read `elapsed >= bound`, timed with `SECONDS` -- which is `CLOCK_REALTIME`,
# and the host steps it. Measured against `CLOCK_MONOTONIC`, 7 of 45 reads reported a
# `SECONDS` elapsed of 4 while the monotonic clock measured 5.01-5.06 s: full-bound
# timeouts carrying status 142, which only LOOKED short. `4 >= 5` was false, so a
# server that held perfectly was reported as having closed. 9 failures in 60 probes
# against one settled node.
#
# The two staged readings are the read's exit status and whether the follow-up probe
# BLOCKED -- 1 it did, 0 it came back at once, `-` no probe was taken because the peer
# had already spoken. Neither is a clock reading, which is the whole repair.
#
# The listener-backed arm of the same property is `http-silence-inconclusive`, which
# spends the whole bound. These cost nothing and cover both branches on every
# platform, which that case cannot.
echo "== the drain's verdict, against staged readings"
read_bound_rows=(
    "ended-timeout-status|142|-|0|a status above 128 is a timeout, so the bound's"
    "ended-timeout-status-shortread|142|-|0|a read a stepped clock made LOOK short still carries 142, and is still the bound's"
    "ended-eof-status|1|0|1|EOF status with an instant follow-up probe is the peer's"
    "ended-32-held|1|1|0|bash 3.2 cannot say, and a probe that BLOCKED is the bound's"
    "ended-32-closed|1|0|1|bash 3.2 cannot say, and an instant probe is the peer's"
    "ended-spoke|0|-|1|a peer that spoke needs no probe and is the peer's"
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
    IFS='|' read -r rname rstatus rprobe rwant rwhat <<< "$row"
    IFS="$old"
    ( . "$library"; _e2e_read_ended_at_bound "$rstatus" "$rprobe" ) && rgot=0 || rgot=$?
    ran=$(( ran + 1 ))
    if [ "$rgot" -ne "$rwant" ]; then
        echo "FAIL ${rname}: ${rwhat} -- status ${rstatus} with probe delta ${rprobe} returned ${rgot}, wanted ${rwant}" >&2
        note_failure "${rname}"
    fi
done

# Four rows that all answer the same way pass every assertion above while testing
# nothing -- the `verdict-branches` argument thirty lines up, applied to a
# predicate with exactly two answers. Both must appear.
read_bound_answers="$(
    for row in ${read_bound_rows[@]+"${read_bound_rows[@]}"}; do
        old="$IFS"
        IFS='|' read -r rname rstatus rprobe rwant rwhat <<< "$row"
        IFS="$old"
        printf '%s\n' "$rwant"
    done | sort -u | grep -c .
)"
ran=$(( ran + 1 ))
if [ "$read_bound_answers" -ne 2 ]; then
    echo "FAIL read-bound-branches: the readings produced ${read_bound_answers} distinct answers, not 2" >&2
    note_failure "read-bound-branches"
fi

# --- a bound is a DURATION, and not a clock reading -------------------------
#
# #1066. Every bound in the library used to be `SECONDS`, which is
# `CLOCK_REALTIME`, and this host steps it. Three arms, because no one of them can
# carry the property on its own:
#
#   1. the deleted rule, driven over STAGED clock traces, shown answering wrongly
#      in BOTH directions. Staged rather than waited for: a backward step happens
#      about once in fourteen reads here and a forward one has never been observed
#      at all, so a test that waits for one is a test that usually does not run.
#   2. the replacement primitive, exercised for real, reading no clock.
#   3. a DERIVED scan over the library, so a fourth bound cannot arrive on
#      `SECONDS` by omission.
#
# The deleted rule is spelled out here as a negative control. That is a copy of
# code that no longer exists, which is normally the defect this file has rules
# about -- but a check that only drives the SURVIVING rule cannot show that the
# rule was changed for a reason, and `testing.md` asks for the asymmetry rather
# than for a green run.

# The rule that was removed: `elapsed >= bound`, over two readings of a clock.
# NOT a wrapper for anything in the library -- there is nothing left to wrap.
_e2e_deleted_clock_bound() { [ $(( $2 - $1 )) -ge "$3" ]; }

echo "== a bound is a duration, not a clock reading"
#     name | first | last | bound | real seconds elapsed | deleted rule | a duration
#
# `deleted` and `duration` are the two verdicts, 0 for expired and 1 for not. Every
# row is a case where they DISAGREE, which is the whole point: a row where they
# agree says nothing about which one is being used.
clock_bound_rows=(
    "step-backward|0|3|5|5|1|0|a backward step makes the reading short, so the bound SILENTLY LENGTHENS and a wedged process is waited on past its ceiling"
    "step-forward|0|7|5|2|0|1|a forward step makes the reading long, so the bound SHORTENS and a healthy client is killed early"
    "step-backward-late|10|12|5|6|1|0|the step lands near the end of the wait, which is where a wait spends most of its time"
    "step-forward-huge|0|3600|5|1|0|1|a resync is not bounded in size, so neither is the error"
)
ran=$(( ran + 1 ))
if [ "${#clock_bound_rows[@]}" -lt 1 ]; then
    echo "FAIL clock-bound: the reading table is empty, so every row 'passed'." >&2
    note_failure "clock-bound"
fi
for row in ${clock_bound_rows[@]+"${clock_bound_rows[@]}"}; do
    old="$IFS"
    IFS='|' read -r cname cfirst clast cbound creal cdeleted cduration cwhy <<< "$row"
    IFS="$old"
    # THE PROPERTY, asserted directly on the ROW rather than inferred from a count.
    # Every row here exists because the deleted rule and a true duration DISAGREE on
    # it; a row where they agree demonstrates nothing about which of the two is in
    # use, and would sit in the table looking like coverage forever. The branch guard
    # below counts distinct answers, which catches most ways this breaks and is a
    # proxy -- this is the thing itself.
    ran=$(( ran + 1 ))
    if [ "$cdeleted" -eq "$cduration" ]; then
        echo "FAIL ${cname}: the row expects the deleted rule and a duration to AGREE (${cdeleted}), so it" >&2
        echo "     asserts nothing about which one the bound is read from. Every row must disagree." >&2
        note_failure "${cname}"
    fi
    _e2e_deleted_clock_bound "$cfirst" "$clast" "$cbound" && gotDeleted=0 || gotDeleted=$?
    [ "$creal" -ge "$cbound" ] && gotDuration=0 || gotDuration=$?
    ran=$(( ran + 1 ))
    if [ "$gotDeleted" -ne "$cdeleted" ] || [ "$gotDuration" -ne "$cduration" ]; then
        echo "FAIL ${cname}: ${cwhy} -- deleted rule said ${gotDeleted} (wanted ${cdeleted}), a duration said ${gotDuration} (wanted ${cduration})" >&2
        note_failure "${cname}"
    fi
done
# BOTH DIRECTIONS ARE PRESENT. With every row now asserted to disagree, the only
# pairs reachable are `(1,0)` and `(0,1)` -- a bound silently lengthened and a
# healthy client killed early -- so requiring two distinct pairs is what stops the
# table drifting to a set that only exercises the backward step. That is the
# direction every measurement on this host has produced, and the forward one is the
# half nobody would notice going missing.
#
# It is NOT a duplicate of the per-row assertion above: that one says each row
# discriminates, this one says the rows between them cover both ways the clock can
# move.
clock_bound_answers="$(
    for row in ${clock_bound_rows[@]+"${clock_bound_rows[@]}"}; do
        old="$IFS"
        IFS='|' read -r cname cfirst clast cbound creal cdeleted cduration cwhy <<< "$row"
        IFS="$old"
        printf '%s%s\n' "$cdeleted" "$cduration"
    done | sort -u | grep -c .
)"
ran=$(( ran + 1 ))
if [ "$clock_bound_answers" -ne 2 ]; then
    echo "FAIL clock-bound-branches: the rows produced ${clock_bound_answers} distinct (deleted,duration) pairs, not 2" >&2
    note_failure "clock-bound-branches"
fi

# --- the replacement primitive, for real -----------------------------------
#
# One second, and the two readings that matter: not passed the moment it is armed,
# passed once it has elapsed. No clock is read on either side -- `sleep` counts a
# relative interval and `[ -e ]` is a file test -- which is the property, so a
# version of this that timed anything would be testing something else.
# Driven inline rather than as a `--case`, so it adds no row to the case table --
# that array is shared with another lane this round, and appending to it is the one
# edit guaranteed to conflict.
echo "== the deadline primitive answers without a clock"
out="$( . "$library"
        _e2e_workdir="$(mktemp -d)"
        _e2e_deadline_arm 1
        primed="$_e2e_deadline_armed"
        ppid="${primed%% *}"
        pmark="${primed#* }"
        if _e2e_deadline_passed "$pmark"; then echo "BUG: passed the moment it was armed"; else echo "armed: not passed"; fi
        sleep 2
        if _e2e_deadline_passed "$pmark"; then echo "elapsed: passed"; else echo "BUG: not passed two seconds into a one-second deadline"; fi
        _e2e_deadline_disarm "$ppid" "$pmark"
        if [ -e "$pmark" ]; then echo "BUG: the marker outlived the disarm"; else echo "disarmed: cleaned up"; fi
        rmdir "$_e2e_workdir" 2>/dev/null || true )"
status=$?
ran=$(( ran + 1 ))
expect "deadline-primitive|0|armed: not passed|elapsed: passed|disarmed: cleaned up|!BUG:" "$out" "$status" \
    || note_failure "deadline-primitive"

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
    # `of a 1s budget` is the assertion, not decoration: `run_case` sets
    # `e2e_wait_seconds 1`, and the hand-written wait this replaced carried its own
    # `local seconds=10` and would print `of a 10s budget` here (#643).
    "counter-never-answered|1|of a 1s budget|nothing ever answered a /metrics request|!BUG:"
    "counter-dies|1|the process DIED|exit=7|COUNTER: nothing ever answered|!BUG:"
    "counter-findings|0|the counter finding named all three terminal states"
    # The grammar under the finding. Driven by nothing until now: #643 moved
    # `metric_value` into the library and left its decisions asserted on no tree
    # (#597, which found the same gap on the fixture side before the move).
    "metric-value-grammar|0|the Prometheus grammar refused a prefix, a label and an absent series|!BUG:"
    "bounded-returns-status|0|run_bounded said 'carried' with status 3"
    "bounded-missing-command|0|a missing command: outcome=unstartable|!BUG:"
    "bounded-outcome-survives-capture|0|two subshells down, the outcome reads unstartable"
    "bounded-expires|0|an expired bound exited 124, outcome exceeded"
    "bounded-outcome-absent-is-finished|0|no record at all: outcome=finished|!BUG:"
    "bounded-outcome-empty-refuses|1|is empty, so the recorded outcome was lost"
    "bounded-outcome-garbage-refuses|1|which is not one of finished/exceeded/unstartable"
    "bounded-124-is-not-a-timeout|0|a command exiting 124: rc=124 outcome=finished|a ceiling expiring:    rc=124 outcome=exceeded"
    "bounded-kills-the-child|0|the bound exited 124|!BUG:"
    "duration-reading-shape|0|accepted 3 readings and refused 7|!BUG:"
    "bounded-fast-path-bites|0|the staged flat-pause defect asked for|!BUG:"
    "bounded-outlasts-a-trapped-term|0|a TERM-ignoring child exited 124"
    "reap-takes-what-nothing-recorded|0|escalated=1, still alive: none|!BUG:"
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
    "daemon-ready-waits-for-marker|0|wait_for_daemon_ready returned with the daemon accepting|!BUG:"
    "daemon-ready-refuses-the-nodes-marker|1|to log: ready, accepting connections|!BUG:"
    "counter-rises|0|wait_for_counter returned and handed back the reading 1|!BUG:"
    "counter-flat|1|of a 1s budget|never reached 1; the last reading was 0|!BUG:"
    "counter-absent|1|of a 1s budget|exports no staged_counter_total series at all|!BUG:"
)

echo "== the helpers, in real shells"
for record in "${cases[@]}"; do
    name="${record%%|*}"
    out="$( bash "${BASH_SOURCE[0]}" --case "$name" 2>&1 )"
    status=$?
    ran=$(( ran + 1 ))
    expect "$record" "$out" "$status" || note_failure "${record%%|*}"
done

# --- `--case` is a verdict, in BOTH directions -----------------------------
#
# The mode this file's own failure block tells an investigator to use. It exited
# a literal 0 whatever the case printed, so a run reporting `BUG: the bound is
# sleeping through commands that have already finished` was indistinguishable
# from a healthy one -- and a lane that followed the advice and keyed a harness
# on the status reported 6 of 6 PASS including a run that should have failed
# (#1104). Nothing errored; the status was simply not a verdict.
#
# Both directions, against the two canaries in `run_case` whose only difference
# is the `BUG:` prefix. Refusing alone would leave a guard that might refuse
# everything (#1031); accepting alone is the defect being fixed.
#
# The status is asserted EXACTLY, not merely as non-zero: 3 is *ran and reported
# a defect* and 1 is *aborted*, and folding them would be this repository's
# once-a-session state collapse inside the fix for one.
echo "== --case reports on the case, in both directions"

ran=$(( ran + 1 ))
verdict_clean="$( bash "${BASH_SOURCE[0]}" --case selftest-case-clean 2>&1 )"
verdict_clean_status=$?
if [ "$verdict_clean_status" -ne 0 ]; then
    echo "FAIL case-verdict-accepts: --case exited ${verdict_clean_status} for a clean case, expected 0" >&2
    printf '%s\n' "$verdict_clean" | sed 's/^/     | /' >&2
    note_failure "case-verdict-accepts"
fi

ran=$(( ran + 1 ))
verdict_bug="$( bash "${BASH_SOURCE[0]}" --case selftest-case-bug 2>&1 )"
verdict_bug_status=$?
if [ "$verdict_bug_status" -ne 3 ]; then
    echo "FAIL case-verdict-refuses: --case exited ${verdict_bug_status} for a case that printed BUG:," >&2
    echo "     expected 3. The mode this file tells people to re-run in cannot report a defect (#1104)." >&2
    printf '%s\n' "$verdict_bug" | sed 's/^/     | /' >&2
    note_failure "case-verdict-refuses"
fi

# The case's own output still reaches the caller -- a wrapper that swallowed it
# would pass both assertions above while making every `expect` row vacuous.
ran=$(( ran + 1 ))
if ! grep -qF "the canary ran and found something" <<< "$verdict_bug"; then
    echo "FAIL case-verdict-streams: --case did not pass the case's output through" >&2
    printf '%s\n' "$verdict_bug" | sed 's/^/     | /' >&2
    note_failure "case-verdict-streams"
fi

# And the advertised mode is the mode in use. `--case-body` is the inner half,
# whose status answers a different question; a consumer routed around the wrapper
# would be reading that question while this block vouches for the other one --
# "the mode under test was not the mode in use", which is #499's shape.
# DERIVED from this file rather than restated: seven self-invocations today, and
# the number moves with the file, so only a FLOOR is asserted.
#
# The flag names are ASSEMBLED rather than spelled, because the first version of
# this block counted its own grep lines: three "variable" invocations where two
# exist and two "inner" ones where one does. A scan whose needle appears in its
# own source is reporting on itself, which is the same defect as a comment being
# read as a call site -- caught by the block failing on a correct tree, which is
# the direction a guard is least often watched in.
ran=$(( ran + 1 ))
caseFlag="--case"
advertised="$(grep -cF "bash \"\${BASH_SOURCE[0]}\" ${caseFlag} \"" "${BASH_SOURCE[0]}" || true)"
advertised_named="$(grep -cE "bash \"\\\$\\{BASH_SOURCE\\[0\\]\\}\" ${caseFlag} [a-z]" "${BASH_SOURCE[0]}" || true)"
inner="$(grep -cF "bash \"\${BASH_SOURCE[0]}\" ${caseFlag}-body \"" "${BASH_SOURCE[0]}" || true)"
if [ "$(( advertised + advertised_named ))" -lt 5 ] || [ "$inner" != "1" ]; then
    echo "FAIL case-mode-in-use: ${advertised} variable and ${advertised_named} named ${caseFlag}" >&2
    echo "     self-invocations and ${inner} ${caseFlag}-body ones. Expected at least five of the" >&2
    echo "     former and exactly one of the latter (the wrapper); a consumer reaching" >&2
    echo "     ${caseFlag}-body directly reads 'did this case run', not 'was it clean'." >&2
    note_failure "case-mode-in-use"
fi

# --- the fast-path defect verdict, in BOTH directions ----------------------
#
# `bounded-fast-path-bites` stages one defect and reaches one verdict, so on any
# given host it exercises exactly one row of this table -- and the row it cannot
# reach locally is the row that reddened master (#1196). Driven from staged
# readings instead, every row is reachable everywhere, and the boundary is stated
# rather than left to a host to stumble onto.
#
# The readings are REAL where they can be: 4000/20 and 3600/18 were measured here
# on 2026-09-10, and 3000/15 is what `Linux-gcc-release` reported on f9f47ebf.
#
# `asked|pauses|expect-bug|why`
fastPathVerdicts=(
    "4000|20|0|the ordinary reading on an idle box: every command polled once, 200ms each"
    "3600|18|0|two commands finished before their first poll -- seen here, one run in ten"
    "3000|15|0|what the hosted runner reported, and what the old total floor refused"
    "3000|20|0|exactly the floor, 150ms per pause: clearing it is passing it"
    "2999|20|1|one millisecond under the floor, so the guard is watched refusing at the edge"
    "1000|20|1|a real defect the ceiling could no longer separate: 50ms a pause"
    "0|0|1|no pause requested at all -- not a defect, and not a measurement either"
)
echo "== the fast-path defect verdict, against staged readings"
for record in "${fastPathVerdicts[@]}"; do
    IFS='|' read -r vAsked vPauses vWantBug vWhy <<< "$record"
    ran=$(( ran + 1 ))
    vOut="$(fast_path_defect_verdict "$vAsked" "$vPauses")"
    # A HERESTRING, not a pipe: this file's own early-exit scan refuses the other
    # spelling, and a scan that its own driver violates is a scan nobody believes.
    if grep -q 'BUG:' <<< "$vOut"; then vGotBug=1; else vGotBug=0; fi
    if [ "$vGotBug" -ne "$vWantBug" ]; then
        echo "FAIL fast-path-verdict [${vAsked}ms over ${vPauses}]: wanted bug=${vWantBug}, got ${vGotBug} -- ${vWhy}" >&2
        printf '%s
' "$vOut" | sed 's/^/     | /' >&2
        note_failure "fast-path-verdict-${vAsked}-${vPauses}"
    fi
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
# A four-second bound, required to have taken at least three seconds -- and the
# three seconds are counted MONOTONICALLY, which is the whole of this change.
#
# `SECONDS` is `CLOCK_REALTIME`, which a VM guest's host time sync steps. Measured
# on this host: backwards by ~2.03 s, periodically, so ~12% of any 4 s window
# contains one (#1108). This floor's slack measured 1.10-1.41 s over ten runs of
# this very case, so EVERY step observed exceeds EVERY slack observed -- the floor
# is crossed whenever a step lands in the window, and the case then prints
# `the loop is counting iterations rather than reading a clock`: a confidently
# worded FALSE cause, naming the subject as broken when the meter moved.
#
# The reason this row used to carry -- "load pushes a wait AWAY from the
# threshold" -- is true of load and silent about the step, which is
# load-INDEPENDENT (#1108 refutes load across a twentyfold excursion). A reason
# that argues the wrong variable is worse than none, because it reads as
# considered. The old "1s slack for truncation" argument was sound about
# truncation and was never about a step.
#
# The remedy is a co-timer, not a wider margin: a background `sleep` counts down
# on `CLOCK_MONOTONIC` and cannot be stepped, so arming N seconds before the
# child and asking afterwards whether it fired answers "did at least N seconds
# pass" with no clock read at all. #354 refuses a bigger margin, and no margin
# can outrun a step whose magnitude is not a constant.
#
# NOT `_e2e_deadline_arm`, though it is the same mechanism. Two reasons, and the
# second is load bearing:
#
#   * the library is not sourced at this file's top level -- every case runs it
#     in a subshell, and the one `. lib/e2e-common.sh` further down is staged
#     heredoc content -- so the function is out of scope and `_e2e_workdir` unset;
#   * this measurement exists precisely to time the library from OUTSIDE, the one
#     reading it cannot take about itself. Borrowing the library's own timer would
#     make the meter part of the subject.
#
# `/proc/uptime` is not the answer either: it occurs in this tree only inside
# comments, and macOS -- where this file runs, on bash 3.2 -- does not have it.
_outer_floor_armed=""
_outer_floor_arm() {
    local seconds="$1" marker=""
    marker="$(mktemp "${TMPDIR:-/tmp}/e2e-floor.XXXXXX" 2>/dev/null)" || marker=""
    [ -n "$marker" ] || { echo "FAIL: cannot arm a ${seconds}s monotonic floor" >&2; exit 2; }
    rm -f "$marker"
    # `trap -` here and an UNCATCHABLE signal in the disarm, for the reason
    # `_e2e_deadline_arm` records at length: a forked subshell inherits this
    # shell's traps, so a timer killed by a catchable signal runs the fixture's
    # own cleanup.
    ( trap - EXIT TERM INT HUP; sleep "$seconds"; : > "$marker" ) >/dev/null 2>&1 3>&- &
    _outer_floor_armed="$! $marker"
}
_outer_floor_reached() { [ -e "$1" ]; }
_outer_floor_disarm() {
    kill -KILL "$1" 2>/dev/null || true
    wait "$1" 2>/dev/null || true
    if [ -e "$2" ]; then rm -f "$2"; fi
}

echo "== the bound is a duration, not an iteration count"
_outer_floor_arm 3
clock_armed="$_outer_floor_armed"
clock_pid="${clock_armed%% *}"
clock_mark="${clock_armed#* }"
clock_started="$SECONDS"
out="$( bash "${BASH_SOURCE[0]}" --case wait-clock-bound 2>&1 )"
status=$?
clock_took=$(( SECONDS - clock_started ))
clock_floor=0
_outer_floor_reached "$clock_mark" && clock_floor=1
_outer_floor_disarm "$clock_pid" "$clock_mark"
ran=$(( ran + 1 ))
if [ "$status" != "1" ]; then
    echo "FAIL wait-clock-bound: exited ${status}, expected 1" >&2
    note_failure "wait-clock-bound"
elif [ "$clock_floor" -ne 1 ]; then
    echo "FAIL wait-clock-bound: a 4s bound came back before 3s of MONOTONIC time had" >&2
    echo "     passed (realtime read ${clock_took}s, a diagnostic and not the verdict);" >&2
    echo "     the loop is counting iterations rather than reading a clock" >&2
    printf '%s\n' "$out" | sed 's/^/     | /' >&2
    note_failure "wait-clock-bound"
else
    # And the number it PRINTS is the measured one. A loop could be bounded
    # correctly and still report the budget, which is the half with teeth --
    # a timeout message stating a duration nobody measured is a fixture lying
    # to the person diagnosing it.
    #
    # That figure is the library's own `SECONDS` delta, so it is steppable too,
    # and comparing it against 3 used to be a SECOND false-red path in this one
    # case -- the fix for the outer timing alone would have left the ticket's
    # symptom live. It is no longer a verdict on the subject: the co-timer above
    # has already established that three seconds really passed, so a child
    # reporting less than three has a realtime reading that DISAGREES with
    # monotonic time. That is an observation about the clock, not a defect in the
    # wait, and it is reported as its own outcome rather than as the nearest
    # neighbour -- which here would be a red naming the wrong subject.
    reported="$(printf '%s\n' "$out" | sed -n 's/^waited \([0-9][0-9]*\)s (measured.*/\1/p')"
    if [ -z "$reported" ]; then
        echo "FAIL wait-clock-bound: it waited ${clock_took}s and reported nothing" >&2
        printf '%s\n' "$out" | sed 's/^/     | /' >&2
        note_failure "wait-clock-bound"
    elif [ "$reported" -lt 3 ]; then
        echo "INCONCLUSIVE wait-clock-bound: 3s of monotonic time passed and the wait" >&2
        echo "     reported ${reported}s, read from CLOCK_REALTIME. The meter stepped" >&2
        echo "     (#1108); this says nothing about the bound, so it is not a failure." >&2
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
# BOTH bounds are monotonic, and the floor is why. Its slack was argued as a
# "3-4x margin" -- which is the CEILING's margin; the row was silent about the
# floor of 1, the direction nobody re-reads. Re-measured on this host through the
# shell that actually steps: this case runs 3.08-3.33 s, so the floor's slack is
# 2.08-2.33 s against a step observed as large as 2.09 s. Those overlap, so the
# floor is defeatable too -- rarely, needing a large step to meet a fast run, and
# rare is not safe. The earlier 3.875 s reading was taken under Git Bash, which
# does not step; measuring a margin through the one shell without the defect is
# what made this look comfortable.
echo "== run_bounded's ceiling is wall clock, and it escalates"
_outer_floor_arm 1
bfloor_armed="$_outer_floor_armed"
bfloor_pid="${bfloor_armed%% *}"
bfloor_mark="${bfloor_armed#* }"
_outer_floor_arm 10
bceil_armed="$_outer_floor_armed"
bceil_pid="${bceil_armed%% *}"
bceil_mark="${bceil_armed#* }"
bounded_started="$SECONDS"
out="$( bash "${BASH_SOURCE[0]}" --case bounded-outlasts-a-trapped-term 2>&1 )"
status=$?
bounded_took=$(( SECONDS - bounded_started ))
bounded_floor=0
bounded_over=0
_outer_floor_reached "$bfloor_mark" && bounded_floor=1
_outer_floor_reached "$bceil_mark" && bounded_over=1
_outer_floor_disarm "$bfloor_pid" "$bfloor_mark"
_outer_floor_disarm "$bceil_pid" "$bceil_mark"
ran=$(( ran + 1 ))
if [ "$status" != "0" ]; then
    echo "FAIL bounded-clock: exited ${status}, expected 0" >&2
    printf '%s\n' "$out" | sed 's/^/     | /' >&2
    note_failure "bounded-clock"
elif [ "$bounded_floor" -ne 1 ]; then
    echo "FAIL bounded-clock: a 1s bound over a 30s child returned before 1s of" >&2
    echo "     MONOTONIC time had passed (realtime read ${bounded_took}s);" >&2
    echo "     the command cannot have been run" >&2
    note_failure "bounded-clock"
elif [ "$bounded_over" -eq 1 ]; then
    echo "FAIL bounded-clock: a 1s bound over a TERM-ignoring child took more than" >&2
    echo "     10s of MONOTONIC time (realtime read ${bounded_took}s);" >&2
    echo "     the bound is waiting for a child that will not die, which is an" >&2
    echo "     unbounded wait inside the thing that exists to bound one" >&2
    note_failure "bounded-clock"
fi

# --- `run_bounded` is not slower than what it bounds ------------------------
#
# The bound's cadence. The measurement, not the constant, is what this pins -- a
# future pause raised "just a little" is exactly how the 74 healthy probes became
# twenty seconds of sleeping.
#
# The clock is INSIDE the case (#678); the case body carries the reasoning and
# the measured separation. This block is still its own block rather than a row in
# the table above for one reason: the figure has to be printed on the PASSING
# path, which the table loop does not do. The record grammar is
# `name|status|pattern|pattern|...` with variadic trailing patterns, so a "report
# this" column cannot simply be appended -- it needs a grammar change to a loop
# every other row goes through, to serve one row. THE TRIGGER for making that
# change is a SECOND case wanting a figure on the passing path; until then this
# is the cheaper shape, and the defect row below reuses the table as it stands.
#
# THE SURVEY, so the next reader does not re-derive it. Stated in the tense it
# was taken: BEFORE this change the `SECONDS`-delta-over-a-whole-process idiom
# appeared exactly three times in this file. Two of them are still here.
#
#   * `wait-clock-bound`             LOWER bound. Was argued "safe by
#                                    construction -- load pushes a wait AWAY from
#                                    the threshold". That is true of LOAD and
#                                    silent about a clock STEP, which #1108
#                                    measured as load-INDEPENDENT: slack
#                                    1.10-1.41s against a step of 1.98-2.09s, so
#                                    every step exceeds every slack. CONVERTED to
#                                    a monotonic co-timer; the realtime delta is
#                                    printed and compared against nothing.
#   * `bounded-outlasts-a-trapped-term`
#                                    Argued as an UPPER bound with a 3-4x margin,
#                                    which is the ceiling's margin -- the row was
#                                    silent about its floor of 1. Re-measured
#                                    through the shell that steps: 3.08-3.33s, so
#                                    the floor's slack is 2.08-2.33s against a
#                                    step reaching 2.09s. Those OVERLAP. The
#                                    earlier 2.5-3.1s was Git Bash, which does not
#                                    step. CONVERTED, both bounds.
#
# The general finding, which outlives both rows: a margin cannot justify a
# realtime bound here, because the step's magnitude is not a constant and #354
# refuses widening one anyway. Neither row was wrong about the variable it
# named; each was silent about the only one that reaches it.
#   * `bounded-fast-path`            UPPER bound with no margin to spare. The
#                                    only one that had to move, and it is now
#                                    timed inside its own case -- so a reader
#                                    grepping this file today finds two, not
#                                    three, and that is not a survey that has
#                                    gone stale.
#
# Outside this file the idiom exists but never as a tight threshold, so nothing
# else needed the same fix:
#
#   * `tsan-canary-rate.sh`   computes the delta and echoes it. Those are its
#                             only two mentions of `elapsed` -- it reports the
#                             figure and compares it against nothing.
#   * `cluster-e2e.sh`        `SECONDS` bounds waits (`deadline=$(( SECONDS + N ))`)
#                             and `firstNamedAt` feeds diagnostic prose. Neither is
#                             a discriminator, and the BUDGET is now a known
#                             remaining site: #1066 established that a bound read
#                             from `SECONDS` is not a duration, and this file's
#                             scan covers the library rather than the fixtures.
#   * `migrate-storage-e2e.sh`
#                             does NOT use it. Its one `SECONDS` is a comment
#                             about `wait_until`'s bound; the script itself polls
#                             through the shared helper.
#   * `lib/e2e-common.sh`     the library this whole file guards, and the biggest
#                             match count of any of them. Every match is a clock
#                             read INSIDE the loop it bounds rather than a wrapper
#                             around a whole process, which is what this survey was
#                             looking for; listed anyway, because "the file with the
#                             most matches is absent from the survey" is not a thing
#                             a reader should have to re-derive.
#
#                             THE ROW USED TO SAY THIRTEEN AND USED TO CALL THEM
#                             SAFE. #1066 is why it no longer does: the reads that
#                             BOUND a loop have gone, and what is left only REPORTS.
#                             `no bound is decided from SECONDS` below is the check
#                             that keeps it that way, and it carries the live count
#                             so this prose does not have to hold a number that
#                             drifts.
#   * `tsan-gate.sh`, `node-socket-activation-e2e.sh`, `doc-subject-checks.sh`
#                             matched by the WORD and not by the idiom: a comment
#                             about `run_bounded`'s integer `SECONDS`, a
#                             `READY_SECONDS` budget handed to `wait_until`, and a
#                             `..._SECONDS` CMake variable named in prose.
#
# The scan behind those rows is `grep -rn SECONDS scripts/ --include=*.sh`, which
# also matches `deadline=$(( SECONDS + N ))`.
#
# **THAT USED TO BE CALLED THE CORRECT IDIOM HERE, and #1066 is the ticket that
# made it false.** The sentence was "reading a clock rather than timing a process
# from outside", and it was right about the distinction it drew and wrong about the
# conclusion: `SECONDS` is `CLOCK_REALTIME`, so a deadline computed from it
# lengthens on a backward step and shortens on a forward one, and neither is
# distinguishable from what the bound exists to detect. A checker whose own comment
# argues against the change it is checking is a citation for reverting that change,
# which is why this is corrected in the same commit rather than left as a residual.
#
# It also makes the scan this block DECLINED tractable. The objection was that the
# word cannot separate the idiom from the correct one -- but after #1066 there is no
# correct one in the library, so `no bound is decided from SECONDS` below can require
# every remaining read to be a named REPORTED reading and refuse the rest.
#
# State the pattern with the count: the
# two are not distinguishable by grepping for the word, and the pattern matches
# SIX files under `scripts/` where the idiom this block is about lives in one.
# Enumerating three of the six and stopping is the shape this repository already
# has a rule about -- a list is exact about what it names and silent about what it
# does not, and silence reads identically to complete coverage.
echo "== run_bounded does not sleep away the fast path"
out="$( bash "${BASH_SOURCE[0]}" --case bounded-fast-path 2>&1 )"
status=$?
ran=$(( ran + 1 ))
expect "bounded-fast-path|0| immediate commands asked for |!BUG:" "$out" "$status" \
    || note_failure "bounded-fast-path"
# UNCONDITIONALLY, pass included. A bound that prints its figure only when it
# breaks cannot show its margin eroding until the day it fails -- the same shape
# as a counter this project exports and nobody scrapes. The number is the
# evidence; `PASSED` on its own is not.
#
# **AND THE `SLOW:` LINES, which this filter silently dropped.** `SLOW:` is not a
# failure -- that is the whole point of it, the host being slow is not the
# helper's fault -- so it never reaches `expect`'s failure dump either, and a
# filter anchored on the measurement line alone made the one report that
# distinguishes *the host was slow* from *the helper did not bite* invisible on
# every run that produced it. Measured: 30 runs of the after-rate for #1058
# produced a 2631 ms healthy reading against the 2000 ms ceiling -- a `SLOW:`
# report, emitted by the case and shown to nobody. A separation nothing prints is
# the paragraph above happening to the line below it.
sed -n -e 's/^[0-9][0-9]* immediate/   &/p' -e 's/^SLOW:/   &/p' <<< "$out"

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

# Both directions of one scan's canary, against fixtures the caller staged.
#
# A scan that has never been seen to fire is a scan reporting PASS over a set in which
# nothing could fail -- `.agent/rules/build-and-toolchain.md` on `script-check-canary`.
# The negative half is not optional: a pattern that matched EVERYTHING would pass the
# positive half alone, and the two halves fail in opposite directions.
#
# One driver rather than three copies (#642). The three canaries here differed only in
# an extractor, a fixture, a count and two nouns -- which is the copy-pasted-block-that-
# differs-only-in-constants shape AGENT.md calls a defect outright. It had already grown
# from the two the ticket counted to three, which is the argument for lifting it now:
# each copy is a place the next scan's canary can be written slightly weaker, and the
# guard nobody re-reads is the one that matters.
#
# The fixtures stay with their scans. They are the part that is genuinely per-scan, and
# hoisting them would put the staged text a long way from the extractor it is staged for.
#
# FOUR canaries go through it: timeout, early-exit, helper-redefinition and
# seconds-scan. Two do not, and both are decisions rather than omissions -- a canary
# left out silently is the next weak copy, which is the whole argument above:
#
#   * `bash32-canary` asks SEVEN questions, not two. Beyond catch/not-catch it drives the
#     declared-region machinery and the outside-scripts walk, so its shape is not this
#     one and forcing it through would mean parameterising five more behaviours.
#   * `early-exit-scan-canary` keeps a THIRD half of its own for the `pipefail` predicate,
#     which decides whether a file is EXAMINED at all. That is a different question from
#     "does the extractor fire", and it sits beside the driver call rather than inside it.
#
# @param 1 case name, as `note_failure` records it
# @param 2 extractor function to drive, by name
# @param 3 file staging what MUST be caught
# @param 4 file staging what must NOT be caught
# @param 5 how many hits $3 is expected to yield
# @param 6 plural noun for what $3 stages -- "invocations", "pipelines", "copies"
# @param 7 why a hit on $4 is wrong, as the tail of "the scan fired on ..."
_scan_canary() {
    local name extractor mustCatch mustNotCatch want noun spuriousWhy caught spurious
    name="$1"; extractor="$2"; mustCatch="$3"; mustNotCatch="$4"
    want="$5"; noun="$6"; spuriousWhy="$7"

    # `grep -c` reads all of its input, so this is not the `producer | grep -q` SIGPIPE
    # hazard the scan below exists to find: no early exit, nothing to lose under pipefail.
    ran=$(( ran + 1 ))
    caught="$( "$extractor" "$mustCatch" | grep -c . || true )"
    if [ "$caught" -ne "$want" ]; then
        echo "FAIL ${name}: the scan caught ${caught} of ${want} staged ${noun}," >&2
        echo "     so it cannot be trusted to have found none in the real scripts" >&2
        "$extractor" "$mustCatch" | sed 's/^/     | /' >&2
        note_failure "$name"
    fi

    ran=$(( ran + 1 ))
    spurious="$( "$extractor" "$mustNotCatch" || true )"
    if [ -n "$spurious" ]; then
        echo "FAIL ${name}: the scan fired on ${spuriousWhy}" >&2
        printf '%s\n' "$spurious" | sed 's/^/     | /' >&2
        note_failure "$name"
    fi
}

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
_scan_canary "timeout-scan-canary" _timeout_invocations \
    "${canary_dir}/must-catch.sh" "${canary_dir}/must-not-catch.sh" \
    7 "invocations" "text that runs nothing"
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

# ---------------------------------------------------------------------------
# Declared data regions, for the scans that have to read THIS file
# ---------------------------------------------------------------------------
#
# What of a file a scan may read: everything except full-line comments and any
# region the file declared as DATA for that scan, with blanks substituted so
# `grep -n` keeps reporting real line numbers.
#
# Three scans need this, because this file holds the very text three of them look
# for -- in a token table, in canary fixtures and in failure messages -- so a scan
# without the filter matches itself. AGENT.md's rule and #492's shape: a file that
# matches its own scan by construction exempts a REGION, never itself. A whole-file
# allowlist row would blind the scan to the 4000-line script ctest runs on macOS,
# which is where the stand-ins it guards actually live.
#
# The MARKER is a parameter and not a constant, and that is the load-bearing part
# rather than a tidy-up: a region says which SCAN its text is data FOR. `bash32`
# and `seconds` each had their own copy of this awk, and `_seconds_readable`'s
# comment argued -- correctly -- that it must not simply call the bash32 one,
# because sharing a marker would blank this file's `seconds` implementation for the
# bash-3.2 check as well, exempting it from a check it needs. That argument is for
# a PARAMETER; two copies of it were the second-best reading, and a third copy for
# the perl scan is where it stops being defensible.
#
# @param 1 the scan's marker, e.g. `bash32-scan`
# @param 2 the file
_readable_dropping_regions() {
    awk -v m="$1" '
        $0 ~ ("^[[:space:]]*# " m ": data-begin[[:space:]]*$") { skip = 1; print ""; next }
        $0 ~ ("^[[:space:]]*# " m ": data-end[[:space:]]*$")   { skip = 0; print ""; next }
        skip                                                   { print ""; next }
        /^[[:space:]]*#/                                       { print ""; next }
        { print }
    ' "$2"
}

# The fault in one file's regions for one scan, as text; nothing when well formed.
#
# A STATE MACHINE, and it has to be, because the reader above is one. Counting the
# two markers and comparing totals is the obvious spelling and it is wrong in the
# direction that HIDES things: a `data-end` sitting above the first `data-begin` is
# a no-op for the reader and leaves a region open to EOF, while the counts come out
# 1 and 1 and the file reads as balanced. Measured -- staged into this very file,
# the whole run's output was byte-identical to a clean one, census line included,
# with a `mapfile` hidden after the stray marker.
#
# So it asks the same questions the reader does, in the same order: a close with
# nothing open, a second open inside one, and anything still open at the end.
#
# @param 1 the scan's marker
# @param 2 the file
# @return 0 when a fault was printed, 1 when the file is well formed
_region_fault() {
    awk -v m="$1" '
        $0 ~ ("^[[:space:]]*# " m ": data-begin[[:space:]]*$") {
            if (open) { print "line " NR ": a data-begin inside a region opened at line " at; exit }
            open = 1; at = NR; next
        }
        $0 ~ ("^[[:space:]]*# " m ": data-end[[:space:]]*$") {
            if (!open) { print "line " NR ": a data-end with no region open"; exit }
            open = 0; next
        }
        END { if (open) print "the region opened at line " at " is never closed" }
    ' "$2" | grep . || return 1
}

# One refusal, spelled once, for every scan that reads through a declared region.
# A region opened and never closed blanks the rest of the file for that scan, and
# nothing about the output says so -- which is this mechanism's own way of becoming
# an exemption. So the fault is a REFUSAL and the file is not scanned on a fault:
# reading it anyway would report on a file half of which was silently invisible.
#
# @param 1 the scan's name, as note_failure records it
# @param 2 the scan's marker
# @param 3 the file
# @param 4 the file's basename
# @return 0 when the file is safe to scan
_region_ok() {
    local fault=""
    fault="$(_region_fault "$2" "$3")" && {
        echo "FAIL $1: ${4} declares a malformed '$2' data region, so part of it is" >&2
        echo "     invisible to this scan and the file 'passes' whatever is in there." >&2
        printf '%s\n' "$fault" | sed 's/^/     | /' >&2
        note_failure "$1"
        return 1
    }
    return 0
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

# --- no script pipes into a consumer that exits EARLY, under pipefail -------
#
# `grep -q` exits at its FIRST match. The producer is then writing into a closed
# pipe, takes SIGPIPE, and under `pipefail` the pipeline reports the PRODUCER's
# status -- so the test fails on the SUCCESS path, and only when the producer is
# still writing when grep leaves. Output size decides that, which is why it passes
# on a developer's machine and reddens a runner (#970).
#
# THE CONSUMER IS NOT THE POINT; LEAVING EARLY IS. `head -N` returns after N lines
# for the identical reason, and so do `grep -m N` and a `sed` script carrying `q`.
# #970 closed a SPELLING and this scan then enumerated ONE consumer, so it was
# exact about `grep -q` and silent about every other one -- which is the same
# a-list-is-silent-about-what-it-does-not-name shape the scan exists to replace.
# `check-script-modes.sh` exited **141** under MSYS2 bash, deterministically and
# with no output at all, on `mode=$(printf '%s\n' "$out" | head -1)`, and this scan
# read the file clean (#1111).
#
# Measured, 200 runs per size, `printf '%s\n' "$big" | head -1` under
# `set -uo pipefail` (#1181): **0 of 200** nonzero at 13,892 bytes and **200 of
# 200** at 211,893 bytes. The boundary is the 64 KB pipe buffer -- while the whole
# payload fits, the producer's `write()` completes before the consumer leaves and
# nothing happens. That is the same size dependence the rulebook already records
# for `grep -q` (wrong 20 of 20 at 101 KB, right 20 of 20 at 1.1 KB), and it is
# why a small fixture reports the idiom working and a census by eye is not enough.
#
# `.agent/rules/build-and-toolchain.md` has carried this rule since the TSan gate's
# `nm "$b" | grep -q __tsan_init`, and five scripts here carry a comment explaining
# why they do NOT do it. It came back in ten files anyway -- which is #627's lesson
# exactly: a rule stated in the files that obey it never reaches the file that does
# not. So it is a scan.
#
# The remedy is a HERESTRING (`grep -q P <<< "$text"`, `head -1 <<< "$text"`) when
# the producer is a variable -- no pipe, so no SIGPIPE -- or capture-then-match when
# it is a command. For a FIRST LINE specifically, `${text%%$'\n'*}` is pure bash,
# forks nothing at all and cannot SIGPIPE by construction.
echo "== no script pipes into an early-exiting consumer under pipefail"

# Does this script actually TURN pipefail ON? Not "mentions pipefail": five files
# here name it only in a comment saying why they avoid the idiom, and flagging
# those would spend the scan's credibility on rows that are already right.
_enables_pipefail() {
    grep -qE '^[[:space:]]*set[[:space:]]+-[A-Za-z]*o[A-Za-z]*[[:space:]]+pipefail' "$1"
}

# A pipe into a consumer that can leave before its producer is finished writing.
# Four spellings, one grep each, merged and deduplicated by line number:
#
#   `| head`      -- `head -1`, `head -n 1`, `head -c 128`, and a bare `head`
#   `| grep -*q`  -- any bundling: `-q`, `-qi`, `-qx`, `-qFf`, `-Fq`
#   `| grep -*m N`-- leaves after N matches
#   `| sed ... q` -- a `q`/`Q` COMMAND, addressed or not
#
# Four greps rather than one ERE: a single alternation over all four would be
# unreadable and could carry no comment per shape, and the `sed` one needs a
# different quoting style from the rest. They are `sort`ed and made unique on the
# LINE NUMBER, because one line may match two of them and would otherwise be
# reported and counted twice.
#
# The bundled grep forms are not decoration -- the hand census that opened #970
# spelled the pattern `| grep -q` and was blind to the five `grep -Fxq` sites in
# `check-gated-jobs.sh` and `check-merge-queue-contexts.sh`, which is to say to the
# two checks that decide which contexts are REQUIRED. This scan found them.
#
# `head` reading a FILE is not this defect and is deliberately not matched: there
# is no pipe, so there is nothing to SIGPIPE, and `check-script-modes.sh`'s own
# `first=$(head -1 "$root/$path")` is correct as it stands. The `|` is required.
#
# The `sed` pattern demands that the `q` be a COMMAND -- preceded by the start of
# the script, an address, a `;`, a `{` or a `/`, and followed by a quote, a `;`, a
# `}`, a `)`, whitespace or end of line. `sed 's/q//'` and `sed -n 's/.*q\(.*\)/\1/p'`
# therefore do not match, which was checked in both directions against the 125
# `| sed` lines in this tree before the pattern was adopted (all clean).
#
# Comment lines are stripped first -- a COMMENT is not a call site, and two checks
# in this tree have already matched their own headers.
#
# What it does NOT do, stated rather than left to be discovered: it cannot see that
# a `| grep -q` inside a double-quoted STRING runs nothing. The `timeout` scan
# solves its version of that by demanding command position; there is no equivalent
# here, because the defect's own shape IS a pipe. Such a line would have to be
# reworded or the file exempted with a reason. None exists today. Nor can it read a
# `sed` script whose own delimiter is `|`, which ends the scan of the argument at
# the delimiter; none exists today either.
_pipe_into_early_exit() {
    {
        grep -nE '\|[[:space:]]*head([[:space:]]|$)' "$1"
        grep -nE '\|[[:space:]]*grep[[:space:]]+([^|;&]*[[:space:]])?-[A-Za-z]*q' "$1"
        grep -nE '\|[[:space:]]*grep[[:space:]]+([^|;&]*[[:space:]])?-[A-Za-z]*m[[:space:]]*[0-9]' "$1"
        grep -nE "\|[[:space:]]*sed([[:space:]][^|;&]*)?([[:space:]'\";{/])[0-9]*[qQ](['\";})[:space:]]|\$)" "$1"
    } 2>/dev/null \
        | grep -v '^[0-9][0-9]*: *#' \
        | LC_ALL=C sort -t: -k1,1n -u \
        || true
}

# The canary, both directions. A scan nobody has watched refuse is a scan
# reporting PASS over a set in which nothing could fail.
#
# The must-not-catch half carries the REMEDIES as well as the near misses, so a
# pattern broadened until it fires on the fix is caught here rather than in a
# reviewer's head: a herestring, a capture-then-match, `${var%%$'\n'*}`, and a
# `head` reading a file.
earlyexit_canary_dir="$(mktemp -d)"
cat > "${earlyexit_canary_dir}/must-catch.sh" <<'CANARY'
printf '%s' "$out" | grep -q -- "$want"
echo "$x" | grep -qi "feature"
pkgutil --pkgs | grep -q "^${LABEL}\."
foo | grep -Fq bar
printf '%s\n' "$t" | grep -qFf "$needles" || return 0
if ! printf '%s\n' "$legs" | grep -qx -- "$leg"; then :; fi
mode=$(printf '%s\n' "$out" | head -1)
first="$(find . -type f | head -n 1)"
grep -E 'error:|FAILED' "$log" | head -40
banner="$(producer | head -c 128)"
producer | head
one="$(producer | sed -n '1p;q')"
marker="$(producer | sed '/ready/q')"
two="$(producer | sed 1q)"
three="$(producer | sed -e q)"
hit="$(producer | grep -m 1 "$pattern")"
CANARY
cat > "${earlyexit_canary_dir}/must-not-catch.sh" <<'CANARY'
grep -q "does match" <<< "$answer"
hits="$(printf '%s\n' "$text" | grep -n -F -- "$token" || true)"
# printf '%s' "$out" | grep -q -- "$want"
out="$(producer)"; case "$out" in *"$want"*) : ;; esac
grep -q -- "$wantMsg" <<< "$out"
first="$(head -1 "$path")"
head -40 <<< "$matches"
mode="${out%%$'\n'*}"
label="$(producer | header_for "$x")"
a="$(producer | sed 's/q//')"
b="$(producer | sed -n 's/.*q\(.*\)/\1/p')"
c="$(printf '%s\n' "$hits" | sed 's/^/     | /')"
d="$(producer | tail -1)"
CANARY
_scan_canary "early-exit-scan-canary" _pipe_into_early_exit \
    "${earlyexit_canary_dir}/must-catch.sh" "${earlyexit_canary_dir}/must-not-catch.sh" \
    16 "pipelines" "a shape that is not the defect"

# The pipefail predicate needs its own canary, because it is what decides whether a
# file is EXAMINED at all: read wrong in the quiet direction it exempts everything
# and the scan reports clean over nothing, which is the failure its neighbours'
# censuses guard against.
printf 'set -euo pipefail\n' > "${earlyexit_canary_dir}/on.sh"
printf 'set -o pipefail\n' > "${earlyexit_canary_dir}/on2.sh"
printf '# set -o pipefail is deliberately NOT used here\nset -u\n' > "${earlyexit_canary_dir}/off.sh"
ran=$(( ran + 1 ))
if ! _enables_pipefail "${earlyexit_canary_dir}/on.sh" \
    || ! _enables_pipefail "${earlyexit_canary_dir}/on2.sh" \
    || _enables_pipefail "${earlyexit_canary_dir}/off.sh"; then
    echo "FAIL early-exit-scan-canary: the pipefail predicate misreads a staged script," >&2
    echo "     so which files the scan examines is not what it claims" >&2
    note_failure "early-exit-scan-canary"
fi
rm -rf "$earlyexit_canary_dir"

earlyexit_allowed="check-e2e-helpers.sh:this file, which stages the scan's own canary pipelines above. They are heredoc text and run nothing; the canary asserting all sixteen are caught is what covers them."
earlyexit_scanned=0
while IFS= read -r script; do
    [ -n "$script" ] || continue
    base="${script##*/}"
    _scan_exempt "$base" "$earlyexit_allowed" && continue
    _enables_pipefail "$script" || continue
    earlyexit_scanned=$(( earlyexit_scanned + 1 ))
    ran=$(( ran + 1 ))
    hits="$(_pipe_into_early_exit "$script")"
    if [ -n "$hits" ]; then
        echo "FAIL early-exit-scan: ${base} sets pipefail and pipes into a consumer that exits early." >&2
        echo "     head, grep -q, grep -m and sed's q all leave before the producer is done; the" >&2
        echo "     producer then takes SIGPIPE and pipefail reports the PRODUCER's status -- a" >&2
        echo "     false negative on the SUCCESS path (#970, #1111, #1181)." >&2
        echo "     Use a herestring: grep -q PATTERN <<< \"\$text\"; head -1 <<< \"\$text\"; or" >&2
        echo "     capture, then match. For a first line, \${text%%\$'\\n'*} forks nothing at all." >&2
        printf '%s\n' "$hits" | sed 's/^/     | /' >&2
        note_failure "early-exit-scan"
    fi
done < <( _shell_scripts )

# A census, for the reason its neighbour records: a walk that matched nothing
# reports clean over zero files and reads exactly like complete coverage.
ran=$(( ran + 1 ))
if [ "$earlyexit_scanned" -lt 1 ]; then
    echo "FAIL early-exit-scan: no script under scripts/ sets pipefail, which cannot be true." >&2
    echo "     Either the walk found no files or the pipefail predicate stopped matching;" >&2
    echo "     either way every script 'passed' without being read." >&2
    note_failure "early-exit-scan"
fi

# --- no script captures a `wc` count without normalising it -----------------
#
# **BSD `wc` pads its count with leading blanks; GNU `wc` does not.** So
# `$(wc -c < f)` is `11` on Linux and `      11` on macOS, and a fixture that
# captures it and compares it as a STRING is green on one platform and red on the
# other. `fastcache-cli-e2e` shipped exactly that and failed on its first macOS run
# with `--raw wrote       11 bytes, expected 11` -- a message whose two numbers are
# EQUAL, which reads as the check being broken rather than as the shell, and is the
# reading that gets an assertion deleted.
#
# The trap is asymmetric in a way that hides it: the same line computed `expected`
# inside `$(( ))`, which normalises, and `actual` as a bare capture, which does not.
# One of the two spellings was suspicious and it was not obvious which.
#
# The rule is normalisation AT CAPTURE, not "normalise where it is compared". A
# regex cannot see where a captured count is later used, and a scan that guesses is
# noise -- so this demands the stronger, uniform property. 12 of the 18 sites in the
# tree already satisfied it by hand, which is what makes it cheap: the idiom was
# already the house style and one file simply did not know.
#
# Remedy: `count_bytes` / `count_lines` from `scripts/lib/e2e-common.sh` in a
# fixture, or `| tr -d ' '` inside the substitution anywhere else.
echo "== no script captures a wc count without normalising it"

# A `$(wc ...)` or backticked `wc` that neither pipes through `tr -d` nor sits in an
# arithmetic context, and is not the operand of a numeric test.
#
# The three accepted forms are each visible on the line, which is what keeps this
# from needing dataflow:
#   * `| tr -d ' '` inside the substitution -- normalised at capture
#   * `$(( $(wc ...) ... ))` -- arithmetic evaluation skips blanks
#   * `[ "$(wc ...)" -le N ]` -- so does a numeric test
#
# Comment lines are stripped first. A COMMENT IS NOT A CALL SITE, and this file's
# own neighbours have twice matched their own headers -- the three surviving `wc`
# mentions in the tree after this scan landed are all prose explaining the trap,
# including the one in `e2e-common.sh` that documents the remedy.
_bare_wc_capture() {
    grep -nE '(\$\(|`)[[:space:]]*wc[[:space:]]' "$1" \
        | grep -v '^[0-9][0-9]*: *#' \
        | grep -vE 'tr[[:space:]]+-d' \
        | grep -vE '\$\(\(' \
        | grep -vE '\)"?[[:space:]]+-(eq|ne|lt|le|gt|ge)[[:space:]]' \
        || true
}

# The canary, both directions. A scan nobody has watched refuse is a scan reporting
# PASS over a set in which nothing could fail.
wc_canary_dir="$(mktemp -d)"
cat > "${wc_canary_dir}/must-catch.sh" <<'CANARY'
actual=$(wc -c < "$WORK/raw")
n=`wc -l < "$file"`
fields=$(wc -l < "$out")
echo "read $(wc -c < "$blob") bytes"
if [ "$(wc -l < "$f")" = "1" ]; then :; fi
CANARY
cat > "${wc_canary_dir}/must-not-catch.sh" <<'CANARY'
actual=$(count_bytes "$WORK/raw")
expected=$(( $(wc -c < "$payload") - 1 ))
n="$(wc -l < "$calls" | tr -d ' ')"
seen=$(wc -l < "$work/submits" | tr -d " ")
if [ "$(wc -l < "$f")" -le 1 ]; then :; fi
# actual=$(wc -c < "$WORK/raw")
CANARY
_scan_canary "wc-scan-canary" _bare_wc_capture \
    "${wc_canary_dir}/must-catch.sh" "${wc_canary_dir}/must-not-catch.sh" \
    5 "captures" "a spelling that is already normalised"
rm -rf "$wc_canary_dir"

wc_allowed="check-e2e-helpers.sh:this file, which stages the scan's own canary captures above. They are heredoc text and run nothing; the canary asserting all five are caught is what covers them."
wc_scanned=0
while IFS= read -r script; do
    [ -n "$script" ] || continue
    base="${script##*/}"
    _scan_exempt "$base" "$wc_allowed" && continue
    wc_scanned=$(( wc_scanned + 1 ))
    ran=$(( ran + 1 ))
    hits="$(_bare_wc_capture "$script")"
    if [ -n "$hits" ]; then
        echo "FAIL wc-scan: ${base} captures a wc count without normalising it." >&2
        echo "     BSD wc pads with blanks and GNU wc does not, so the capture compares" >&2
        echo "     equal to itself only on Linux -- a red macOS leg whose message reads" >&2
        echo "     'wrote       11 bytes, expected 11'." >&2
        echo "     Use count_bytes/count_lines from scripts/lib/e2e-common.sh, or pipe" >&2
        echo "     the substitution through | tr -d ' '." >&2
        printf '%s\n' "$hits" | sed 's/^/     | /' >&2
        note_failure "wc-scan"
    fi
done < <( _shell_scripts )

# A census, for the reason its neighbours record: a walk that matched nothing
# reports clean over zero files and reads exactly like complete coverage.
ran=$(( ran + 1 ))
if [ "$wc_scanned" -lt 1 ]; then
    echo "FAIL wc-scan: the walk matched no shell scripts, so every one of them 'passed'." >&2
    note_failure "wc-scan"
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
_scan_canary "helper-scan-canary" _helper_redefinitions \
    "${canary_dir}/must-catch.sh" "${canary_dir}/must-not-catch.sh" \
    2 "copies" "a script that defines no copy"
rm -rf "$canary_dir"

# The same `_scan_exempt` the timeout scan above uses. Each row names why. Neither
# remaining row names an issue, because neither is a defect waiting to be fixed --
# and that is the state to keep this list in: an exemption is blind to the SECOND
# divergence as well as the first, so a row standing for a file that ought to
# source the library buys silence about drift nobody is watching for. The
# `launcher-replay-e2e.sh` row was exactly that and is gone (#813); it read "the
# last POSIX fixture that does not source the library", which was not quite true
# even then -- `node-config-file-e2e.sh` does not either, and needs nothing from
# it: no daemon, no port, no background process, and not one of these names.
helper_copy_allowed="e2e-common.sh:the library itself, which defines every one of these names -- that being what the scan reads them out of. It entered this scan's set when the three enumerations were folded into one recursive walk; the row is what keeps that fold from reporting the definitions as copies.
local-gate.sh:not an e2e fixture. It sources nothing, starts no daemon and opens no socket; its 'fail' prints a build-gate verdict and its own selftest (local-gate-selftest) is what covers it."
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

# --- every perl invocation is bounded, or SAYS why it is not ----------------
#
# `_selftest_bounded_perl` makes the `exec` + `alarm` pair impossible to omit for
# anything that goes through it. Nothing makes a new stand-in go through it, and
# that is exactly the gap #843 is about: **a rule stated in the files that obey it
# reaches no file that does not** (#970). The three stand-ins here now carry the
# rule; the fourth, written next month in another fixture, would not -- and #834
# is the proof this is not hypothetical, having added an unbounded listener while
# the ticket about bounds was open, with the diagnosis written down.
#
# So the rule is a SCAN and not a comment, and it asks the question the door can
# answer rather than the one it cannot: not *does this perl body contain a bound*
# -- which #843 notes a naive `grep` gets wrong, since `alarm 30` appears in prose
# in this very file -- but **does any script name a perl PROGRAM outside the one
# line that arms the bound**. That is a question about command positions, which is
# the same shape as the `timeout` scan above and needs no region machinery.
#
# TWO spellings, each a claim, which is `Refuse` / `RefuseWithoutCounter` in a
# shell script:
#
#   * route through `_selftest_bounded_perl` -- bounded, nothing to say;
#   * carry `# perl-lifetime: <reason>` on the invocation line -- deliberately
#     unbounded, and WHY.
#
# The second is TALLIED and printed on every run, exactly as the refusal-rationale
# check prints its per-issue totals: a marker whose reason is a placeholder must be
# visible, or "decided" and "forgot" are one spelling again. Two rows today, and
# both are read out below rather than counted.
#
# ## The census, and why it is a check rather than a sentence
#
# Measured on this tree, two constructions with a positive control: `perl` in a
# COMMAND position appears in exactly one tracked shell script -- this one -- and
# `git grep -c -i perl` over every tracked file agrees that no other script
# mentions it at all. A scan whose subject has left the tree reports every file
# clean, and that reads identically to complete coverage, so the tally below
# REFUSES at zero rather than passing. Finding one instance by other means before
# believing a zero is what that clause is.
#
# ## Why this reads through a declared region and the other scans' canaries do not
#
# The helper-copy canary one screen up stages its fixture with `printf` at column
# zero, and that is enough for IT because its pattern is anchored at column zero.
# This one is not: `out="$(perl -e ...)"` is a real invocation shape and has to be
# caught, so the `$(` entry point matches inside a quoted `printf` argument too --
# measured, the first run of this block failed on its own canary staging. So the
# staging sits in a `perl-scan` declared region, which is the mechanism this file
# already uses twice for exactly this, and the region is BALANCE-CHECKED and
# COUNTED below, or it is just an exemption nobody can see being added.
#
# ## The entry-point class, and the hole the mutation found
#
# `{` and `)` are ENTRY POINTS and not merely keywords, which is the correction
# this pattern needed rather than a nicety. The first draft copied the `timeout`
# scan's class -- `^ ; & | ( ` $( && ||` -- with `{` in the optional-keyword
# alternation, and the arm written to prove the rule REACHES another fixture
# planted `stage_thing() { perl -e "sleep 99" & }` in `cluster-e2e.sh` and the
# scan reported CLEAN. The `perl` there follows `) { `, and neither `)` nor `{`
# could open a match: `^` was consumed by the function name, so nothing anchored.
# A one-line brace group is the shape a hurried new stand-in takes, so the scan
# was blind to precisely its own subject in precisely the file it exists to reach.
# `)` comes with it because a `case` arm (`foo) perl -e ...`) is a command
# position by the same argument -- found by asking what ELSE the missing anchor
# would have cost rather than by patching the one shape that failed.
#
# What it does NOT reach, stated rather than left for somebody to discover: a perl
# behind a PREFIX COMMAND with flags of its own -- `nice -n 10 perl -e ...`,
# `env FOO=1 perl -e ...`, `xargs perl`. `exec` is in the alternation because it is
# the spelling every stand-in here used before the door existed; the rest are not,
# and no regex over shell text reaches them without matching the word `perl` in
# prose and in `command -v perl`, which is the noise that gets a scan deleted. That
# residue is an EVASION rather than an accident -- nobody writes `nice perl` for a
# fixture stand-in by mistake -- and the accidental shapes are the six the canary
# pins. Stating the limit is the remedy; widening the pattern is not.
_perl_invocations() {
    _perl_readable "$1" \
        | grep -nE '(^|[;&|(){}`]|\$\(|&&|\|\|)[[:space:]]*(if|then|else|elif|while|until|do|!|exec)?[[:space:]]*perl[[:space:]]' \
        || true
}
_perl_readable() { _readable_dropping_regions "perl-scan" "$1"; }

# The same set, split on whether the line states a reason. `grep -F` on the marker
# and not a pattern: the marker is a literal and a regex here would be one more
# thing to be wrong about.
_perl_unmarked_invocations() {
    _perl_invocations "$1" | grep -vF '# perl-lifetime:' || true
}
_perl_marked_invocations() {
    _perl_invocations "$1" | grep -F '# perl-lifetime:' || true
}

# The canary, both directions. SIX entry points staged, because each is a way a
# perl program reaches a shell: `exec`, a command substitution, a background job,
# a bare command in an `if`, a one-line brace group, and a `case` arm. The
# must-catch COUNT is what makes a narrowed pattern fail here rather than in six
# months -- and the last two are in this list because the pattern that shipped in
# the first draft passed all four of the others while being blind to them.
# perl-scan: data-begin
canary_dir="$(mktemp -d)"
cat > "${canary_dir}/must-catch.sh" <<'CANARY'
exec perl -e "while (1) {}"
out="$(perl -e "print 1")"
perl -e "sleep 99" &
if perl -e "exit 0"; then :; fi
stage_thing() { perl -e "sleep 99" & }
case "$x" in thing) perl -e "exit 0" ;; esac
CANARY
# The negative half. Five shapes that must NOT fire, and each is a way this scan
# could have been written too wide: a routed stand-in, a marked invocation, `perl`
# as an ARGUMENT rather than a command, the word inside a string, and a comment.
cat > "${canary_dir}/must-not-catch.sh" <<'CANARY'
_selftest_bounded_perl 30 "print 1" "$@"
perl -e "exit 0" # perl-lifetime: a probe, and here is the reason
command -v perl >/dev/null 2>&1 || skip "no perl"
echo "SKIPPED: perl with IO::Socket::INET is not available"
# exec perl -e "commented out"
CANARY
# perl-scan: data-end
_scan_canary "perl-bounds-canary" _perl_unmarked_invocations \
    "${canary_dir}/must-catch.sh" "${canary_dir}/must-not-catch.sh" \
    6 "invocations" "a routed, marked, argument-position, quoted or commented mention of perl"
rm -rf "$canary_dir"

perl_scanned=0
perl_marked_total=0
perl_regions=""
while IFS= read -r script; do
    [ -n "$script" ] || continue
    base="${script##*/}"
    ran=$(( ran + 1 ))
    _region_ok "perl-bounds-scan" "perl-scan" "$script" "$base" || continue
    case "$(grep -c '^[[:space:]]*# perl-scan: data-begin[[:space:]]*$' "$script")" in
        0) ;;
        *) perl_regions="${perl_regions:+${perl_regions}, }${base}" ;;
    esac
    perl_scanned=$(( perl_scanned + 1 ))
    ran=$(( ran + 1 ))
    hits="$(_perl_unmarked_invocations "$script")"
    if [ -n "$hits" ]; then
        echo "FAIL perl-bounds-scan: ${base} runs a perl program of its own." >&2
        echo "     A stand-in needs 'exec' (or the kill at its call site reaps a wrapper)" >&2
        echo "     and a lifetime bound (or nothing reaps it under SIGKILL, ctest --timeout" >&2
        echo "     or a cancelled job) -- #839 measured 1368 orphan listeners holding ports." >&2
        echo "     Use _selftest_bounded_perl <seconds> '<program>' <args...>, or state a" >&2
        echo "     reason on the line as '# perl-lifetime: why this one needs no bound'." >&2
        printf '%s\n' "$hits" | sed 's/^/     | /' >&2
        note_failure "perl-bounds-scan"
    fi
    marked="$(_perl_marked_invocations "$script")"
    if [ -n "$marked" ]; then
        while IFS= read -r row; do
            [ -n "$row" ] || continue
            perl_marked_total=$(( perl_marked_total + 1 ))
            echo "   perl-lifetime: ${base}:${row%%:*} -- ${row#*# perl-lifetime: }"
        done <<EOF
$marked
EOF
    fi
done < <( _shell_scripts )

# Both empties, and they are different failures. No scripts means the walk broke;
# no marked invocation means the SUBJECT left the tree, and a scan whose subject is
# gone reports every file clean.
ran=$(( ran + 1 ))
if [ "$perl_scanned" -lt 1 ]; then
    echo "FAIL perl-bounds-scan: the walk matched no shell scripts, so every one of them 'passed'." >&2
    note_failure "perl-bounds-scan"
fi
ran=$(( ran + 1 ))
if [ "$perl_marked_total" -lt 1 ]; then
    echo "FAIL perl-bounds-scan: not one perl invocation was found anywhere under scripts/." >&2
    echo "     The injector in _selftest_bounded_perl carries a '# perl-lifetime:' marker," >&2
    echo "     so zero means the pattern has stopped matching rather than that the tree is" >&2
    echo "     clean -- which is the reading that passes over everything." >&2
    note_failure "perl-bounds-scan"
fi
# Reported, not merely tolerated: a region nobody can see added is this mechanism's
# own way of becoming an exemption, which is the bash-3.2 scan's argument for the
# same line.
echo "   perl bounds: scanned ${perl_scanned} script(s) under scripts/ (walked, not listed)"
echo "   perl bounds: declared data region(s) in: ${perl_regions:-none}"
echo "   perl bounds: a tracked *.sh outside scripts/ is refused by shell-walk-scope, below"

# And the DOOR itself, asked behaviourally rather than by reading it. A scan that
# refuses every unrouted `perl` is worth nothing if what they are routed INTO has
# stopped arming anything, and that failure is silent in exactly the way #839 was.
#
# The verdict is an EXIT STATUS and not a duration: 142 is 128 + SIGALRM, which is
# host- and clock-independent -- and this repository has a standing finding that a
# wall clock under WSL2 steps ~2 s backwards every ~32 s, so a bound asserted by
# timing is a flake waiting for a runner. Both directions, because a door that
# killed everything would pass the refusing half alone.
if command -v perl >/dev/null 2>&1; then
    ran=$(( ran + 1 ))
    bounded_out="$( ( _selftest_bounded_perl 2 'select(undef, undef, undef, 60); print "SURVIVED\n";' ) 2>/dev/null )"
    bounded_status=$?
    if [ "$bounded_status" -ne 142 ] || [ -n "$bounded_out" ]; then
        echo "FAIL perl-bounds-door: a program that would run 60s under a 2s bound exited" >&2
        echo "     ${bounded_status} (want 142 = 128 + SIGALRM) and printed '${bounded_out}'." >&2
        echo "     _selftest_bounded_perl is not arming the alarm, so every stand-in that" >&2
        echo "     goes through it is unbounded while the scan above reports clean." >&2
        note_failure "perl-bounds-door"
    fi

    ran=$(( ran + 1 ))
    alive_out="$( ( _selftest_bounded_perl 30 'print "ALIVE:", join(",", @ARGV), "\n";' one two ) 2>/dev/null )"
    alive_status=$?
    if [ "$alive_status" -ne 0 ] || [ "$alive_out" != "ALIVE:one,two" ]; then
        echo "FAIL perl-bounds-door: the ACCEPTING direction. A short program under a 30s" >&2
        echo "     bound exited ${alive_status} and printed '${alive_out}', want 0 and" >&2
        echo "     'ALIVE:one,two'. A guard nobody has watched accept is not known to work," >&2
        echo "     and the arguments after -- are what every stand-in reads from @ARGV." >&2
        note_failure "perl-bounds-door"
    fi
else
    echo "   perl-bounds-door: NOT CHECKED -- no perl on this host" >&2
    skipped=$(( skipped + 1 ))
fi

if command -v perl >/dev/null 2>&1 && perl -MIO::Socket::INET -e1 >/dev/null 2>&1; then # perl-lifetime: a foreground availability probe; it exits at once and is never backgrounded
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
# can write the rationale for. Indented comments too, since a rationale is as
# likely to sit inside a function as above one: `lib/e2e-common.sh`'s own `fail`
# names `BASHPID` twice in the paragraph explaining why it does not use it, and
# `launcher-replay-e2e.sh` names it in the preamble recording why its private
# copy of that `fail` is gone (#813).
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
_bash32_readable() { _readable_dropping_regions "bash32-scan" "$1"; }

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
    grep -qFf "$bash32_needles" <<< "$text" || return 0
    while [ "$i" -lt "${#banned[@]}" ]; do
        token="${banned[$i]%%:*}"
        hits="$(printf '%s\n' "$text" | grep -n -F -- "$token" || true)"
        [ -n "$hits" ] && printf '%s\n' "$hits" | sed "s/^/${i} /"
        i=$(( i + 1 ))
    done
    return 0
}

# A malformed `bash32-scan` region, in any scanned file. Prints what is wrong and
# returns 0 when there is a fault; returns 1 when the file is well formed. The
# state machine, and why counting the two markers is the wrong spelling, is at
# `_region_fault`; the canaries below drive this name because they are the ones
# that have watched it refuse.
_bash32_region_fault() { _region_fault "bash32-scan" "$1"; }

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
bash32_allowed="tidy-sweep.sh:not a bash 3.2 script and does not claim to be. It declares a bash 4.4 floor in its own header (wait -n is 4.3; expanding an empty array under set -u stops erroring at 4.4). Its 'tidy-sweep-selftest' registration runs it through FASTCACHED_BASH44 -- a version-CHECKED interpreter rather than the /bin/bash pin every other registration uses -- and keeps SKIP_RETURN_CODE 77 for a host where no candidate clears the floor. Until #598 that registration used the 3.2 pin and therefore reported SKIPPED on macOS in perpetuity; this sentence used to cite that skip as the enforcement mechanism, which stopped being the arrangement when the interpreter moved. A declared exception with an enforcement mechanism, not an omission."

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
# It is asked ONCE and it answers for the WALK, not for one scan: `_shell_scripts`
# is the set every scan in this file reads -- timeout, early-exit, wc, helper
# copies, perl bounds, bash 3.2 constructs and SECONDS -- so a stray file is
# outside all of them at once. Hence the name is about the walk. A second copy
# beside each scan would be six more things to be wrong rather than a cross-check,
# and its position after the loops costs nothing: a stray reddens the whole run,
# so no scan's clean verdict is read as final.
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
        echo "FAIL shell-walk-scope: tracked shell script(s) live outside scripts/, where the" >&2
        echo "     _shell_scripts walk every scan in this file reads does not reach them:" >&2
        printf '%s\n' "$stray" | sed 's/^/     | /' >&2
        echo "     Widen the walk, or give each one an allowlist row saying why it is exempt." >&2
        note_failure "shell-walk-scope"
    else
        echo "   walk scope confirmed -- git ls-files '*.sh' finds none outside scripts/"
    fi
else
    # Not a pass and not a failure: the question could not be asked. Said out
    # loud, because "no strays found" and "nothing looked" read identically --
    # and counted as SKIPPED only, never also as run.
    echo "   walk scope NOT CHECKED -- no git repository here, so whether any *.sh lives outside scripts/ is unknown" >&2
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

# The third arm of `a bound is a DURATION, and not a clock reading` (#1066). The
# other two -- the deleted rule driven over staged clock traces, and the
# replacement primitive exercised for real -- are staged-record tables and live
# with the others above the case divider. This one is a SCAN, so it lives here
# with its three siblings and reuses their `_shell_scripts` generator and
# `_scan_exempt`, for the reason that generator's own header gives.
# --- no bound is decided from SECONDS --------------------------------------
#
# DERIVED from the tree rather than from a list of the four functions that were
# converted: a list is exact about what it names and silent about what it does not,
# and the next bound is the one nobody adds to the list.
#
# It walks `_shell_scripts` like the three scans above rather than reading the
# library alone -- one enumeration for all of them, for the reason that generator's
# own header gives -- and it splits its subjects two ways:
#
#   * a file on the EXEMPTION list is skipped with a stated reason. Those are the
#     fixtures, which #1066 did not convert; the reasons name what is true of each.
#   * everything else must have every non-comment `SECONDS` read match a listed
#     REPORTED reading, with the reason it is not a bound. A read that matches no
#     row is refused.
#
# It fails closed four ways: no scripts walked, no reads examined, an exemption for
# a file the walk never saw (an allowlist that has outlived its subject), and an
# empty reported-reading table. Two empty lists agree perfectly, and each of those
# is a way to become one.
# seconds-scan: data-begin
echo "== no bound is decided from SECONDS"

#
# EVERYTHING from here to the matching `data-end` is this scan's own implementation,
# and it holds the text being scanned for -- in the exemption reasons, in the table,
# in two canary heredocs and in three failure messages. Declared as data so the scan
# does not match itself, and declared as a REGION rather than a whole-file exemption
# so the rest of this file, its own two timings above included, stays scanned.

# What counts as a read of bash's `SECONDS`, spelled ONCE and used by both the
# per-file walk and the extractor -- two sites, and a scan whose two halves can
# disagree about what it is looking for is two scans.
#
# A WHOLE WORD, and that is the correction rather than a nicety. It was a bare
# `grep -n 'SECONDS'`, which is a substring: `FASTCACHED_SCAN_BUDGET_SECONDS`,
# `READY_SECONDS` and any other identifier ENDING in the token matched, and so
# would one beginning with it. `pgrep -f` in a `grep` -- a pattern is broader than
# its author reads it as -- and it is the same family as this file's own
# `perl-bounds-scan` needing `{` and `)` as entry points.
#
# The tell was not a red run. **It was an exemption**: the row below for
# `node-socket-activation-e2e.sh` said, in its own words, *"a READY_SECONDS budget
# handed to wait_until, which is a NAME matched by the word rather than a clock
# read of its own"* -- an allowlist row whose stated reason IS the false positive,
# blinding the scan to that whole fixture to silence one identifier. A false
# positive teaches people to work around it, and working around it is what
# disarms a scan as thoroughly as deleting it. Another lane hit the same pattern
# on a `..._BUDGET_SECONDS` CMake variable and renamed AROUND it, which is the
# second instance of the same cost.
#
# So the row is gone with the defect that produced it, and the file it named is
# scanned again -- measured, it has no wall-clock read at all. Deleting it is not
# optional either: the "an exemption for a file with no SECONDS read left in it"
# guard one screen down would refuse the tree for a row naming a file the fixed
# pattern no longer matches, which is that guard doing exactly its job.
_seconds_word='(^|[^A-Za-z0-9_])SECONDS([^A-Za-z0-9_]|$)'

# One row per exemption, `basename:reason`, matched per row by `_scan_exempt`.
seconds_exempt="tsan-canary-rate.sh:computes a delta and echoes it. It reports the figure and compares it against nothing, so there is no bound to be wrong."

# Every `SECONDS` read that is not a bound decided INSIDE the loop it bounds, with
# the reason. TWO kinds appear and the distinction is the point:
#
#   * readings the library only REPORTS -- a bound COMPARES, and these only ever
#     subtract into a variable that is printed;
#   * this file's own timings of a whole process from OUTSIDE, which is the one
#     measurement the library cannot make about itself and is what `wait-clock-bound`
#     and `bounded-outlasts-a-trapped-term` exist to take. Their VERDICTS are now
#     monotonic co-timers, so what is left of them here is the realtime figure they
#     PRINT beside it -- the first kind, arrived at from the second.
#
# `cluster-e2e.sh` has no file-level exemption any more: its eight waits are armed
# co-timers, so the only reads left in it are the two below, which feed prose.
#
seconds_reported=(
    'local started="$SECONDS" grewAt="$SECONDS"|wait_until: the two origins for the durations it REPORTS'
    'grewAt="$SECONDS"|wait_until: when the log last grew, for the stall reading'
    'elapsed=$(( SECONDS - started ))|the elapsed a verdict PRINTS, so it is measured rather than assumed'
    'stall=$(( SECONDS - grewAt ))|how long since the log grew, a reported reading'
    'local started="$SECONDS" elapsed=0 armed dpid dmark|stop_and_require_exit: the origin for the duration it reports'
    'local started="$SECONDS"|cluster-e2e.sh: the origin for firstNamedAt, which feeds diagnostic prose only'
    'firstNamedAt=$(( SECONDS - started ))|cluster-e2e.sh: how long until a leader was first named, printed in the formation diagnosis'
    'before=$SECONDS|_http_drain_fd3: the observation #1048 left with no reader'
    '_http_drain_elapsed=$(( SECONDS - before ))|_http_drain_fd3: that same observation, marked not to be read'
    'clock_started="$SECONDS"|check-e2e-helpers.sh: the realtime origin wait-clock-bound PRINTS; its verdict is the monotonic co-timer'
    'clock_took=$(( SECONDS - clock_started ))|check-e2e-helpers.sh: that same realtime delta, printed as a diagnostic and compared against nothing'
    'bounded_started="$SECONDS"|check-e2e-helpers.sh: the realtime origin bounded-clock PRINTS; both its bounds are monotonic co-timers'
    'bounded_took=$(( SECONDS - bounded_started ))|check-e2e-helpers.sh: that same realtime delta, printed as a diagnostic and compared against nothing'
)
ran=$(( ran + 1 ))
if [ "${#seconds_reported[@]}" -lt 1 ]; then
    echo "FAIL seconds-bounds: the reported-reading table is empty, so every read would be refused" >&2
    note_failure "seconds-bounds"
fi

# Every `SECONDS` read in one file that is on no list, as `<lineno>:<text>`.
#
# A FUNCTION rather than a loop body, so the canary below can drive it over staged
# files. A scan nobody has watched refuse is a scan reporting PASS over nothing, and
# the two scans above already learnt that.
#
# Read through the shared region filter under this scan's OWN marker. A region says
# which SCAN its text is data for, so reusing `bash32-scan` markers here would blank
# this scan's implementation for the bash-3.2 check as well, exempting it from a
# check it needs. That argument is why the marker is a PARAMETER of
# `_readable_dropping_regions` rather than why this is a third copy of the awk.
#
# And a region IS needed: this file holds the very text this scan looks for, in a
# table, in two canary heredocs and in three failure messages, so without one the
# scan matches itself. AGENT.md's rule, and #492's shape -- a file that matches its
# own scan by construction exempts a REGION, never itself. The other 3000 lines stay
# scanned, which is exactly what a whole-file row would have cost.
_seconds_readable() { _readable_dropping_regions "seconds-scan" "$1"; }

_seconds_unlisted() {
    local file="$1" line="" body="" trimmed="" pair="" matched=0 out=""
    while IFS= read -r line; do
        [ -n "$line" ] || continue
        body="${line#*:}"
        trimmed="${body#"${body%%[![:space:]]*}"}"
        matched=0
        for pair in ${seconds_reported[@]+"${seconds_reported[@]}"}; do
            if [ "$trimmed" = "${pair%%|*}" ]; then matched=1; break; fi
        done
        [ "$matched" -eq 1 ] && continue
        out="${out}${out:+
}${line}"
    done <<EOF
$( _seconds_readable "$file" | grep -nE "$_seconds_word" || true )
EOF
    printf '%s' "$out"
}

seconds_files=0
seconds_exempted=0
seconds_reads=0
seconds_seen=""
seconds_hit=""
seconds_regions=""
while IFS= read -r script; do
    [ -n "$script" ] || continue
    base="${script##*/}"
    seconds_seen="${seconds_seen}${base}
"
    # This scan reads through a declared region and, until #843, checked no
    # region's BALANCE and reported no region's existence -- while declaring one
    # of its own that spans about two hundred lines of this file. A lost
    # `data-end` would have blanked everything after it for this scan alone, with
    # a census line that still read normally and nothing anywhere naming a region.
    # `bash32` already had both guards; sharing the mechanism is what carried them
    # here, which is the argument for parameterising it rather than copying it.
    ran=$(( ran + 1 ))
    _region_ok "seconds-bounds" "seconds-scan" "$script" "$base" || continue
    case "$(grep -c '^[[:space:]]*# seconds-scan: data-begin[[:space:]]*$' "$script")" in
        0) ;;
        *) seconds_regions="${seconds_regions:+${seconds_regions}, }${base}" ;;
    esac
    hits="$(_seconds_readable "$script" | grep -nE "$_seconds_word" || true)"
    [ -n "$hits" ] || continue
    seconds_files=$(( seconds_files + 1 ))
    seconds_hit="${seconds_hit}${base}
"
    if _scan_exempt "$base" "$seconds_exempt"; then
        seconds_exempted=$(( seconds_exempted + 1 ))
        continue
    fi
    while IFS= read -r line; do
        [ -n "$line" ] || continue
        seconds_reads=$(( seconds_reads + 1 ))
    done <<EOF
$hits
EOF
    ran=$(( ran + 1 ))
    unlisted="$(_seconds_unlisted "$script")"
    if [ -n "$unlisted" ]; then
        echo "FAIL seconds-bounds: ${base} reads SECONDS on a line that is on no list:" >&2
        printf '%s
' "$unlisted" | sed 's/^/     | /' >&2
        echo "     A bound is a duration, not a clock reading (#1066). If this is a reported" >&2
        echo "     reading and not a bound, add it to seconds_reported WITH the reason." >&2
        note_failure "seconds-bounds"
    fi
done < <( _shell_scripts )

# An exemption that outlives its subject is an allowlist nobody can read, so a row
# naming a file the walk never saw is refused rather than ignored.
while IFS= read -r row; do
    [ -n "$row" ] || continue
    exempt_base="${row%%:*}"
    ran=$(( ran + 1 ))
    case "
${seconds_hit}" in
        *"
${exempt_base}
"*) : ;;
        *) echo "FAIL seconds-bounds: the exemption for '${exempt_base}' names a file with no SECONDS read left in it" >&2
           note_failure "seconds-bounds" ;;
    esac
    ran=$(( ran + 1 ))
    case "
${seconds_seen}" in
        *"
${exempt_base}
"*) : ;;
        *) echo "FAIL seconds-bounds: the exemption for '${exempt_base}' names a file the walk never saw" >&2
           note_failure "seconds-bounds" ;;
    esac
done <<EOF
$seconds_exempt
EOF

# THE CANARY, both directions, because either alone passes a broken scan: one that
# catches nothing passes the must-not-catch half, and one that fires on everything
# passes the must-catch half.
#
# The staged bounds are the three shapes this ticket converted -- a deadline, the
# comparison against it, and a grace window -- and the must-not-catch file holds the
# listed reported readings plus a COMMENT containing a bound, which is the shape the
# filter has to remove rather than the shape the scan has to forgive.
seconds_canary_dir="$(mktemp -d)"
cat > "${seconds_canary_dir}/must-catch.sh" <<'CANARY'
deadline=$(( SECONDS + 5 ))
while [ "$SECONDS" -lt "$deadline" ]; do :; done
grace=$(( SECONDS + 2 ))
CANARY
# The accepting half, and the last three rows are the ones nobody writes. An
# IDENTIFIER that merely contains the token is not a clock read, in either
# direction -- ending in it, beginning with it, and named in prose -- and skipping
# those is what let a substring `grep` stand: the refusing direction was pinned
# from the first draft and passed throughout, so the pattern looked tested. Which
# direction you skip decides which way a predicate lies, and this one lay toward
# PRESENT, which is the direction that gets acted on. It cost one lane a rename
# and this file an allowlist row that blinded a whole fixture.
cat > "${seconds_canary_dir}/must-not-catch.sh" <<'CANARY'
# a comment that names a bound: deadline=$(( SECONDS + 5 ))
elapsed=$(( SECONDS - started ))
stall=$(( SECONDS - grewAt ))
readonly READY_SECONDS=240
wait_until worker_ready "the worker" "$pid" "$log" "$READY_SECONDS"
cmake -DFASTCACHED_SCAN_BUDGET_SECONDS=90 -P check.cmake
SECONDS_PER_TICK=5
CANARY
_scan_canary "seconds-scan-canary" _seconds_unlisted \
    "${seconds_canary_dir}/must-catch.sh" "${seconds_canary_dir}/must-not-catch.sh" \
    3 "bounds" "a listed reading or on a comment"
rm -rf "$seconds_canary_dir"

ran=$(( ran + 1 ))
if [ "$seconds_files" -lt 1 ]; then
    echo "FAIL seconds-bounds: the walk found no script mentioning SECONDS at all, which is what a" >&2
    echo "     broken walk and a converted tree look like alike" >&2
    note_failure "seconds-bounds"
fi
ran=$(( ran + 1 ))
if [ "$seconds_reads" -lt 1 ]; then
    echo "FAIL seconds-bounds: every file with a SECONDS read was exempted, so nothing was examined" >&2
    note_failure "seconds-bounds"
fi
echo "   SECONDS: ${seconds_reads} read(s) examined across $(( seconds_files - seconds_exempted )) file(s), ${seconds_exempted} exempted by name"
echo "   SECONDS: declared data region(s) in: ${seconds_regions:-none}"
# seconds-scan: data-end

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
# Matches the counter NAME on any assignment or arithmetic, not ONE rendering of
# the increment (#808). The old needle was the literal `failures=$(( failures + 1
# ))`, so `failures=$((failures+1))`, `failures=$(( failures+1 ))` and
# `(( failures++ ))` each incremented without naming the failure and passed this
# scan silently -- which is the reopen-by-omission it exists to make impossible.
#
# `failure[s]` rather than `failures`: the needle must not appear verbatim in
# itself, which is the self-counting trap the paragraph above records. The
# bracket makes the regex match "failures" while the literal text of these lines
# does not match the regex.
#
# Full-line COMMENTS are stripped first. Writing this very comment, which spells
# the bad renderings out, made the scan report five sites -- so the rule "a
# comment is not a call site" was broken two lines below where it is cited.
#
# Three exclusions, all narrow: the `failures=0` initialisation is not an
# increment, and this scan's own lines (which name BASH_SOURCE) are not call
# sites -- a COMMENT or a grep pattern is not a call site, which this tree has
# been caught on twice.
own_increments="$(grep -nE 'failure[s][[:space:]]*(=|\+\+|\+=)' "${BASH_SOURCE[0]}" \
    | grep -vE '^[0-9]+:[[:space:]]*#' \
    | grep -vE ':[[:space:]]*failure[s]=0[[:space:]]*$' \
    | grep -vF 'BASH_SOURCE' \
    | wc -l | tr -d ' ')"
if [ "$own_increments" = "0" ]; then
    echo "FAIL failure-recording: found no direct increment of the counter in ${BASH_SOURCE[0]} at all;" >&2
    echo "     the counter has been renamed and this scan is now guarding nothing" >&2
    note_failure "failure-recording"
elif [ "$own_increments" != "1" ]; then
    echo "FAIL failure-recording: ${own_increments} sites increment the failure counter directly;" >&2
    echo "     only note_failure may, or a failure is counted without being named (#678)" >&2
    grep -nE 'failure[s][[:space:]]*(=|\+\+|\+=)' "${BASH_SOURCE[0]}" \
        | grep -vE '^[0-9]+:[[:space:]]*#' \
        | grep -vE ':[[:space:]]*failure[s]=0[[:space:]]*$' \
        | grep -vF 'BASH_SOURCE' | sed 's/^/     | /' >&2
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
    echo "  (that mode exits 0 clean, 3 when the case printed BUG:, else the case aborted)"

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
        echo "      which exits 0 clean, 3 when the case printed BUG:,"
        echo "      and otherwise with whatever the case aborted on"
    } > "$durable" 2>/dev/null; then
        echo "  a copy the next run will not overwrite: ${durable}"
    fi
fi
[ "$failures" -eq 0 ] || exit 1
exit 0
