#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Watches the dependency-fetch transfer bound (#526) actually refuse a stall.

The bound, the measurements behind it and the reasoning for its numbers are in
`cmake/FetchTransferBound.cmake`, which is the one place they are stated. This
file is what shows the bound firing, because a guard nobody has watched refuse
is not a guard -- and the fix applied by hand on the night of the wedges was
never once exercised, which is exactly the gap this closes.

WHAT IT PROVES, precisely, because the distinction matters when this is cited:

  * The subject can be made to hang.  Each transport is run against the stall
    with its bound REMOVED and must still be running when the control window
    ends.  Without this the refusals below would prove nothing: a stall that
    ends on its own is refused by everything.
  * Each transport refuses that same stall once the bound is applied, at the
    rate floor this tree ships, and says so in words naming the transfer.
  * This tree WIRES the bound: the seam is reached before anything is fetched,
    and no download in the tree is left unbounded.  A bound nothing includes is
    a file, not a guard.

  * NOT the shipped WINDOW.  By default the window is compressed (``--window``)
    so this stays a seconds-long default-set test rather than a four-minute one.
    What is compressed is how long git and CMake count before giving up; the
    rate floor, the mechanism, the delivery channel and the wiring are all the
    shipped ones.  Pass ``--full`` to run it at the shipped window instead --
    that takes a little over ``GIT_HTTP_LOW_SPEED_TIME`` and is how the shipped
    number itself gets watched.  The window each run exercised is PRINTED, so a
    later citation cannot confuse the two figures.

The stall is a listener that accepts a connection and then answers nothing.
That is the observed shape rather than a convenient approximation: the wedged
configures had an ESTABLISHED connection which had delivered zero bytes, which
is why a refusal to connect, a reset and a closed socket are all the wrong
stand-in -- every one of those ends by itself.

Python rather than the usual .sh/.ps1 pair because this needs a listening
socket and process-tree control, which POSIX shell cannot express at all; a
shell version would be a Windows-only implementation with a POSIX stub.
"""

import argparse
import os
import platform
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
from collections import namedtuple

SKIP = 77

# Windows: git's own stall shows as `git.exe` spawning `git-remote-http.exe`.
# Killing only the parent leaves the grandchild holding the pipe write ends and
# a drain on those pipes never returns -- the same shape the rulebook records
# for a wedged compile (#239).  So every child is run with its output on FILES
# rather than pipes, and killed as a TREE.
IS_WINDOWS = platform.system() == "Windows"

# A window longer than this is not a bound, whatever its arithmetic says: the
# wedges this exists for were ended by a human at forty minutes and by a
# parker's deadline at fifty, so a bound that outlives the person who would
# otherwise have killed it changes nothing.  An order of magnitude below the
# observed wedges.
WINDOW_CEILING_SECONDS = 600

# And a floor above this is a magnitude judgement about healthy traffic that
# nothing in the evidence calibrates -- the argument for the shipped floor is
# that it is indistinguishable from ZERO, not that it is small.  A floor that
# fires on a working transfer trades a hang for a broken build, which is the
# worse direction, and this is the only thing guarding it.
FLOOR_CEILING_BYTES_PER_SECOND = 1024

#: Where this project's own CMake lives. A list, because the alternative --
#: walking everything and excluding build-directory names -- is a guess that
#: fails the moment somebody configures into `build/` or puts a
#: `CPM_SOURCE_CACHE` in the checkout.
AUTHORED_CMAKE_PATHS = ("CMakeLists.txt", "cmake", "src", "packaging")

#: Environment a child must not inherit, because every one of these can decide
#: a verdict this guard is supposed to draw from the transport itself.
AMBIENT_SETTINGS_TO_CLEAR = (
    "GIT_HTTP_LOW_SPEED_LIMIT",
    "GIT_HTTP_LOW_SPEED_TIME",
    "http_proxy",
    "https_proxy",
    "all_proxy",
    "HTTP_PROXY",
    "HTTPS_PROXY",
    "ALL_PROXY",
)

#: The same question at git's OTHER spelling.  `GIT_HTTP_LOW_SPEED_*` is the
#: environment form; `http.lowSpeedLimit`/`http.lowSpeedTime` is the canonical
#: one, and a developer with either in `~/.gitconfig` -- or a proxy -- would
#: otherwise hand the control leg a bound this guard believes it removed.
GIT_NEUTRALISING_CONFIG = [
    "-c", "http.lowSpeedLimit=0",
    "-c", "http.lowSpeedTime=0",
    "-c", "http.proxy=",
]

#: What one run of a transport against the stall did.  ``ended`` is the whole
#: verdict; the rest is the evidence a reader needs when it disagrees with what
#: was expected.
Outcome = namedtuple("Outcome", "ended seconds code output")

#: A child that has been started and not yet waited on.
Started = namedtuple("Started", "process log begun")

#: One transport: how it is run without the bound, how it is run with it, and
#: what its refusal has to say.  Both transports are asked the same two
#: questions, so a third one is a row rather than a branch.  ``refusal`` stays a
#: per-row column because the two transports genuinely word it differently --
#: git says "Operation too slow", CMake's libcurl says "Timeout was reached".
Leg = namedtuple("Leg", "label control_command bounded_command bound_environment refusal")

#: One thing this tree must be seen to do, expressed over a file's NON-COMMENT
#: text.  ``needle`` must appear; if ``before`` is given, it must appear before
#: the earliest of those needles.  Prose is stripped first: both needles below
#: appear in the comments explaining why they are there, and a comment must not
#: be able to satisfy a wiring check -- which is how this was got wrong on its
#: first run, so it is known rather than assumed.
Wiring = namedtuple("Wiring", "path needle before why")

WIRING_ROWS = [
    Wiring(
        path="CMakeLists.txt",
        needle="include(cmake/CPM.cmake)",
        before=("CPMAddPackage", "FetchContent_Declare", "ExternalProject_Add"),
        why="a dependency added above the seam is fetched unbounded, which is the "
        "whole claim the seam makes",
    ),
    Wiring(
        # The whole `include()`, not the bare file name: that name also appears
        # in the bootstrap's failure message, which is non-comment text, so the
        # short needle matched a string literal after the download and reported
        # an ordering violation for a file that had no include at all. Measured,
        # not foreseen -- the check still failed, with the wrong reason, which is
        # the harder defect to notice.
        path=os.path.join("cmake", "CPM.cmake"),
        needle='include("${CMAKE_CURRENT_LIST_DIR}/FetchTransferBound.cmake")',
        before=("file(DOWNLOAD",),
        why="the bootstrap download runs before the bound is defined, so it is "
        "unbounded and nothing says so",
    ),
    Wiring(
        path=os.path.join("cmake", "CPM.cmake"),
        needle='INACTIVITY_TIMEOUT "${FASTCACHED_FETCH_SILENCE_SECONDS}"',
        before=None,
        why="the bootstrap download would carry a number of its own, and the tree "
        "would state one policy in two places",
    ),
]


def strip_cmake_comments(text):
    """Drop whole-line comments, so prose cannot satisfy a wiring check."""
    return "\n".join(
        line for line in text.splitlines() if not line.lstrip().startswith("#")
    )


def read_text(path):
    with open(path, "rb") as source:
        return source.read().decode("utf-8", "replace")


class Stall:
    """A listener that accepts connections and answers nothing, ever."""

    def __init__(self):
        self._socket = socket.socket()
        self._socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._socket.bind(("127.0.0.1", 0))
        self._socket.listen(16)
        self.port = self._socket.getsockname()[1]
        self._accepted = []
        threading.Thread(target=self._accept_loop, daemon=True).start()

    def _accept_loop(self):
        while True:
            try:
                connection, _ = self._socket.accept()
            except OSError:
                return
            # Held, never read from and never written to.  Dropping it here
            # would close the socket and end the very stall being staged.
            self._accepted.append(connection)

    def close(self):
        for connection in self._accepted:
            try:
                connection.close()
            except OSError:
                pass
        self._socket.close()


def kill_tree(process):
    """Kill a child and everything it spawned. See the note on IS_WINDOWS."""
    if IS_WINDOWS:
        subprocess.run(
            ["taskkill", "/T", "/F", "/PID", str(process.pid)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
    else:
        try:
            os.killpg(os.getpgid(process.pid), signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            process.kill()
    try:
        process.wait(timeout=30)
    except subprocess.TimeoutExpired:
        pass


def start_against_stall(command, environment, workdir):
    """Start ``command`` against the stall and return without waiting.

    Starting is separate from waiting so the legs run CONCURRENTLY: each
    control leg's verdict is "still running when the window ended", and two
    such windows run one after the other is the same six seconds paid twice.
    Nothing couples the legs -- one listener with a backlog of 16, a temporary
    directory each, a process group each -- so the only ordering left is that
    findings are collected after every leg has been waited on.
    """
    child_environment = dict(os.environ)
    # Whatever the developer or CI has set must not decide the verdict: the
    # control leg has to be genuinely unbounded, and the bounded leg has to be
    # bounded by the values under test.  A proxy is the sharper of the two --
    # with one configured the clone never reaches 127.0.0.1 at all, and a proxy
    # error ends the control leg instantly, which this guard would report as
    # "the stall is not a stall": the instrument blaming the subject.
    for name in AMBIENT_SETTINGS_TO_CLEAR:
        child_environment.pop(name, None)
    child_environment.update(environment)
    child_environment["GIT_TERMINAL_PROMPT"] = "0"

    log = os.path.join(workdir, "output.txt")
    sink = open(log, "wb")
    try:
        process = subprocess.Popen(
            command,
            cwd=workdir,
            env=child_environment,
            stdout=sink,
            stderr=subprocess.STDOUT,
            **({} if IS_WINDOWS else {"start_new_session": True}),
        )
    finally:
        # The child holds its own descriptor; this one would otherwise keep the
        # file open until the interpreter exits.
        sink.close()
    return Started(process=process, log=log, begun=time.monotonic())


def finish(started, deadline):
    """Wait for a started child until ``deadline``, killing it if it overruns."""
    try:
        started.process.wait(timeout=max(0.0, deadline - time.monotonic()))
        ended = True
    except subprocess.TimeoutExpired:
        kill_tree(started.process)
        ended = False
    output = read_text(started.log).strip()
    return Outcome(
        ended=ended,
        seconds=time.monotonic() - started.begun,
        code=started.process.returncode,
        output=output,
    )


def read_shipped_bound(cmake, source_dir, workspace):
    """Read the bound by INCLUDING the module that defines it.

    Reading the numbers back out of the module is the difference between
    asserting the mechanism works and asserting THIS TREE applies it; a copy of
    the values here would be a second thing to go stale, not a cross-check. The
    probe lives here rather than as an output surface on the module itself,
    which is `scripts/check-debug-prefix-map.cmake`'s shape: a production module
    should not grow a printing mode that only a test ever reaches.
    """
    module = os.path.join(source_dir, "cmake", "FetchTransferBound.cmake")
    probe = os.path.join(workspace, "read-bound.cmake")
    with open(probe, "w", encoding="utf-8") as script:
        script.write(
            'include("%s")\n'
            'message("GIT_HTTP_LOW_SPEED_LIMIT=$ENV{GIT_HTTP_LOW_SPEED_LIMIT}")\n'
            'message("GIT_HTTP_LOW_SPEED_TIME=$ENV{GIT_HTTP_LOW_SPEED_TIME}")\n'
            % module.replace("\\", "/")
        )
    result = subprocess.run(
        [cmake, "-P", probe], capture_output=True, text=True, check=False
    )
    if result.returncode != 0:
        raise RuntimeError(
            "including %s failed (%s):\n%s" % (module, result.returncode, result.stderr)
        )
    table = {}
    for line in (result.stdout + result.stderr).splitlines():
        name, separator, value = line.strip().partition("=")
        if separator:
            table[name] = value
    return table


def check_number(name, raw, ceiling, unit, why_ceiling):
    """One shipped number, or a sentence saying what is wrong with it."""
    if not raw or not raw.isdigit() or int(raw) <= 0:
        return None, (
            "cmake/FetchTransferBound.cmake exports %s=%r, which is not a positive "
            "integer, so the tree exports no usable bound" % (name, raw)
        )
    if int(raw) > ceiling:
        return None, (
            "cmake/FetchTransferBound.cmake exports %s=%s %s, above the %d %s this "
            "guard will accept: %s" % (name, raw, unit, ceiling, unit, why_ceiling)
        )
    return int(raw), None


def check_downloads_are_bounded(source_dir):
    """Every `file(DOWNLOAD)` in the tree must carry an INACTIVITY_TIMEOUT.

    Stated as a property of the TREE rather than as two remembered line
    numbers. `cmake/portable/CompileCache.cmake` has two downloads of its own
    and must keep its own numbers -- it is required to stay stock-CMake-only, so
    it cannot include the module that states this one -- and that accepted
    duplicate is exactly the thing that goes quietly wrong. A THIRD download
    added anywhere, by anyone, is caught by the same row.

    The scan covers the directories this project AUTHORS, listed rather than
    derived by excluding build-directory names: a build tree configured
    anywhere inside the checkout, or an in-tree `CPM_SOURCE_CACHE`, drops
    third-party CMake into scope, and failing this project's guard for
    somebody else's `file(DOWNLOAD)` is a check nobody can satisfy.
    """
    violations = []
    for relative in AUTHORED_CMAKE_PATHS:
        base = os.path.join(source_dir, relative)
        if os.path.isfile(base):
            candidates = [base]
        elif os.path.isdir(base):
            candidates = [
                os.path.join(root, name)
                for root, _, files in os.walk(base)
                for name in files
                if name == "CMakeLists.txt" or name.endswith(".cmake")
            ]
        else:
            continue
        for path in candidates:
            body = strip_cmake_comments(read_text(path))
            for match in re.finditer(r"file\s*\(\s*DOWNLOAD", body):
                depth, end = 0, len(body)
                for index in range(match.start(), len(body)):
                    if body[index] == "(":
                        depth += 1
                    elif body[index] == ")":
                        depth -= 1
                        if depth == 0:
                            end = index
                            break
                call = body[match.start() : end]
                where = "%s (non-comment line %d)" % (
                    os.path.relpath(path, source_dir).replace("\\", "/"),
                    body.count("\n", 0, match.start()) + 1,
                )
                # The VALUE, not only the token. Zero is how CMake spells *no
                # bound*, so a call carrying `INACTIVITY_TIMEOUT 0` satisfies
                # every structural reading of this rule and reinstates #526.
                bound = re.search(r"INACTIVITY_TIMEOUT\s+\"?([^\s\")]+)", call)
                if bound is None:
                    violations.append(
                        "%s: a file(DOWNLOAD) with no INACTIVITY_TIMEOUT -- a stalled "
                        "response hangs the configure forever, which is #526" % where
                    )
                elif bound.group(1).isdigit() and int(bound.group(1)) == 0:
                    violations.append(
                        "%s: a file(DOWNLOAD) whose INACTIVITY_TIMEOUT is 0, which is "
                        "how CMake spells no bound at all" % where
                    )
    return violations


def check_wiring(source_dir):
    """Every WIRING_ROWS row, over the file's non-comment text."""
    violations = []
    for row in WIRING_ROWS:
        path = os.path.join(source_dir, row.path)
        if not os.path.isfile(path):
            violations.append("%s does not exist, so nothing wires the bound" % row.path)
            continue
        body = strip_cmake_comments(read_text(path))
        at = body.find(row.needle)
        if at < 0:
            violations.append("%s does not contain `%s`: %s" % (row.path, row.needle, row.why))
            continue
        if row.before is None:
            continue
        positions = [body.find(needle) for needle in row.before]
        positions = [position for position in positions if position >= 0]
        if positions and at > min(positions):
            violations.append(
                "%s has `%s` AFTER the first of %s: %s"
                % (row.path, row.needle, ", ".join(row.before), row.why)
            )
    return violations




def build_legs(arguments, url, workspace, window, floor):
    """One row per transport, given the window and floor this run exercises."""

    def download_script(inactivity_timeout):
        """A one-line `file(DOWNLOAD)` against the stall, bounded or not."""
        suffix = "unbounded" if inactivity_timeout is None else inactivity_timeout
        path = os.path.join(workspace, "download-%s.cmake" % suffix)
        bound = (
            ""
            if inactivity_timeout is None
            else "INACTIVITY_TIMEOUT %d" % inactivity_timeout
        )
        with open(path, "w", encoding="utf-8") as script:
            script.write(
                'file(DOWNLOAD "%s" "${CMAKE_CURRENT_LIST_DIR}/payload.bin" %s '
                "STATUS status)\n"
                "list(GET status 0 code)\n"
                "if(NOT code EQUAL 0)\n"
                '    message(FATAL_ERROR "download refused: ${status}")\n'
                "endif()\n" % (url, bound)
            )
        return path

    clone = [arguments.git] + GIT_NEUTRALISING_CONFIG + ["clone", url, "clone-dir"]
    return [
        Leg(
            label="git",
            control_command=clone,
            bounded_command=clone,
            bound_environment={
                "GIT_HTTP_LOW_SPEED_LIMIT": str(floor),
                "GIT_HTTP_LOW_SPEED_TIME": str(window),
            },
            refusal=re.compile(r"too slow|low speed|timed out|timeout", re.I),
        ),
        Leg(
            label="cmake file(DOWNLOAD)",
            control_command=[arguments.cmake, "-P", download_script(None)],
            bounded_command=[arguments.cmake, "-P", download_script(window)],
            bound_environment={},
            refusal=re.compile(r"inactivity|timeout|timed out", re.I),
        ),
    ]


def run_phase(legs, workspace, pick_command, pick_environment, seconds, prefix):
    """Start every leg, then wait on all of them behind ONE shared deadline.

    Running the transports concurrently is what keeps this a seconds-long
    default-set test: a control leg's verdict is "still running when the window
    ended", and two such windows run one after the other is the same wait paid
    twice. Nothing couples the legs -- one listener with a backlog of 16, a
    temporary directory each, a process group each -- so the only ordering that
    remains is that findings are collected after every leg has been waited on.
    """
    started = [
        start_against_stall(
            pick_command(leg),
            pick_environment(leg),
            tempfile.mkdtemp(prefix=prefix, dir=workspace),
        )
        for leg in legs
    ]
    deadline = time.monotonic() + seconds
    return [finish(one, deadline) for one in started]


def run_transports(arguments, stall, workspace, window, floor):
    """Both phases against the stall. Returns (failures, notes, inconclusive).

    THREE lists out, because there are three outcomes. The controls keep deciding as
    they always did: `CONTROL: unbounded ... ended by itself` stays a hard failure
    whatever the host's speed, since a stall that is not a stall makes everything
    below vacuous and that verdict does not depend on how fast anything ran.
    """
    failures, notes, inconclusive = [], [], []
    legs = build_legs(arguments, stall_url(stall), workspace, window, floor)

    for leg, control in zip(
        legs,
        run_phase(
            legs,
            workspace,
            lambda leg: leg.control_command,
            lambda leg: {},
            arguments.control,
            "control-",
        ),
    ):
        if control.ended:
            failures.append(
                "CONTROL: unbounded %s ended by itself after %.1fs (rc=%s), so the "
                "stall is not a stall and nothing below proves anything.\n%s"
                % (leg.label, control.seconds, control.code, control.output)
            )
        else:
            notes.append(
                "control: unbounded %s still running after %ds -- the stall holds"
                % (leg.label, arguments.control)
            )

    # Generous on purpose: this deadline exists so a broken bound REPORTS
    # rather than hangs, not to measure anything. Which is exactly why missing it
    # cannot be read as a broken bound on its own -- see `classify_bounded`.
    patience = window + 30
    results = run_phase(
        legs,
        workspace,
        lambda leg: leg.bounded_command,
        lambda leg: leg.bound_environment,
        patience,
        "bounded-",
    )
    verdicts = classify_bounded(
        [BoundedReading(leg.label, one.ended, one.seconds) for leg, one in zip(legs, results)],
        window,
        patience,
    )
    for leg, bounded, verdict in zip(legs, results, verdicts):
        if verdict.outcome == OutcomeInconclusive:
            inconclusive.append(
                "bounded %s did not end within %ds of a %ds silence window, and this run "
                "cannot say whether that is the bound or the machine: %s.\n%s"
                % (leg.label, patience, window, verdict.detail, bounded.output)
            )
        elif verdict.outcome == OutcomeUnreachableBound:
            failures.append(
                "bounded %s did NOT end within %ds of a %ds silence window; the "
                "bound is not reaching it -- %s.\n%s"
                % (leg.label, patience, window, verdict.detail, bounded.output)
            )
        # The OUTPUT is the verdict, and the exit code is only evidence. A
        # `cmake -P` leg reports its refusal through message(FATAL_ERROR),
        # whose exit status this repository has recorded as differing between
        # CMake versions -- so a check that read the status could call a
        # correct refusal a success on some minimum-version machine and pass on
        # others, which is the worst available split. Measured here: WSL CMake
        # 3.28.3 exits 1 and 4.3.1 exits 1, both against a real stall; the
        # ordering below means it would not have mattered either way.
        elif not leg.refusal.search(bounded.output):
            failures.append(
                "bounded %s ended after %.1fs (rc=%s) but its message names no "
                "transfer refusal -- against a server that sent nothing, that is "
                "either a success or a failure an operator cannot tell from an "
                "unrelated one.\n%s"
                % (leg.label, bounded.seconds, bounded.code, bounded.output)
            )
        else:
            notes.append(
                "bounded %s refused after %.1fs (window %ds, floor %d B/s): %s"
                % (
                    leg.label,
                    bounded.seconds,
                    window,
                    floor,
                    bounded.output.splitlines()[-1][:160],
                )
            )
    return failures, notes, inconclusive


# One leg's reading, as the classifier sees it. A record rather than the live
# `finish()` result, because the whole point is that this decision can be driven
# against readings nobody had to produce with a real stall and a real clock.
BoundedReading = namedtuple("BoundedReading", "label ended seconds")

# What a bounded leg turned out to be. THREE, and the third is why this exists.
BoundedVerdict = namedtuple("BoundedVerdict", "label outcome detail")

# A leg that did not end, on a host fast enough that the deadline was fair.
OutcomeUnreachableBound = "unreachable-bound"
# A leg that did not end, on a host too slow for the deadline to have meant anything.
OutcomeInconclusive = "inconclusive"
# A leg that ended inside the deadline. Whether its MESSAGE is right is a separate
# question, judged where it always was.
OutcomeEnded = "ended"


def classify_bounded(readings, window, patience):
    """Which non-ending legs are broken bounds, and which cannot be judged at all.

    A slow machine and a wedged bound are fixed in different places, so a wait that
    ran out has to say which it met. Before #1469 this had two answers and gave the
    confident one: `Windows-cl-debug` reported "the bound is not reaching it" about
    git on an attempt where the SIBLING leg refused after 35.1s against a 5s window
    -- a host running about 7x slow -- and passed on a re-run at the same commit.
    That message sends whoever reads it to `cmake/FetchTransferBound.cmake` and to
    git's `http.lowSpeedTime`, neither of which was involved.

    **The evidence is already in hand: a sibling's own refusal time is a reading of
    how slow the host is.** A leg refusing at `seconds` against `window` has
    demonstrated that this host needs `seconds - window` of overhead to notice and
    act. The phase hands every leg `patience - window` of slack for that. So:

    - no leg ended at all -> there is no speed reading, and that is its own reason to
      be inconclusive rather than a reason to believe the fastest explanation;
    - a leg that DID end consumed the whole slack -> a leg exactly that slow could not
      have ended inside `patience`, so a sibling that did not tells us nothing;
    - otherwise the host was demonstrably fast enough, and a non-ending leg is a bound
      that is not reaching its transport.

    **This rule has no constant, deliberately.** The comparison is the phase's own
    slack against a measurement the phase itself produced, so there is no number to
    raise -- and raising one is what restores the original wrong answer by the front
    door, since the next loaded runner is slower still.

    What it does NOT cover, said here so the next reader does not over-apply it: a host
    slow enough to eat only PART of the slack still yields `unreachable-bound`. Closing
    that gap needs a fraction-of-slack constant, which is the thing this avoids.

    @param readings A `BoundedReading` per leg of one phase.
    @param window The silence window in force, in seconds.
    @param patience The phase's deadline, in seconds.
    @return A `BoundedVerdict` per leg, in the order given.
    """
    slack = patience - window
    concluded = [one for one in readings if one.ended]
    worst = max((one.seconds - window for one in concluded), default=None)
    slowest = None
    if concluded:
        slowest = max(concluded, key=lambda one: one.seconds)

    verdicts = []
    for one in readings:
        if one.ended:
            verdicts.append(BoundedVerdict(one.label, OutcomeEnded, ""))
        elif worst is None:
            verdicts.append(
                BoundedVerdict(
                    one.label,
                    OutcomeInconclusive,
                    "no leg of this phase ended, so there is no reading of how fast this "
                    "host was and nothing says whether %ds was a fair deadline" % patience,
                )
            )
        elif worst >= slack:
            verdicts.append(
                BoundedVerdict(
                    one.label,
                    OutcomeInconclusive,
                    "%s refused after %.1fs against a %ds window, spending %.1fs of the "
                    "%ds this phase allows for overhead -- all of it, so a leg this slow "
                    "could not have ended within %ds either"
                    % (slowest.label, slowest.seconds, window, worst, slack, patience),
                )
            )
        else:
            verdicts.append(
                BoundedVerdict(
                    one.label,
                    OutcomeUnreachableBound,
                    "%s refused after %.1fs against the same %ds window, spending %.1fs of "
                    "the %ds allowed, so this host was fast enough and the bound is not "
                    "reaching this transport"
                    % (slowest.label, slowest.seconds, window, worst, slack),
                )
            )
    return verdicts


SELF_TEST_CASES = (
    # (name, readings, window, patience, expected outcomes in order)
    (
        "both legs refused promptly",
        [BoundedReading("git", True, 6.1), BoundedReading("cmake", True, 6.4)],
        5,
        35,
        [OutcomeEnded, OutcomeEnded],
    ),
    (
        "the observed failure: a sibling ate the whole slack",
        [BoundedReading("git", False, 35.0), BoundedReading("cmake file(DOWNLOAD)", True, 35.1)],
        5,
        35,
        [OutcomeInconclusive, OutcomeEnded],
    ),
    (
        "a healthy host, so a non-ending leg IS a broken bound",
        [BoundedReading("git", False, 35.0), BoundedReading("cmake file(DOWNLOAD)", True, 6.2)],
        5,
        35,
        [OutcomeUnreachableBound, OutcomeEnded],
    ),
    (
        "nothing concluded, so there is no speed reading",
        [BoundedReading("git", False, 35.0), BoundedReading("cmake", False, 35.0)],
        5,
        35,
        [OutcomeInconclusive, OutcomeInconclusive],
    ),
    (
        "exactly at the slack boundary is inconclusive, not failed",
        [BoundedReading("git", False, 35.0), BoundedReading("cmake", True, 35.0)],
        5,
        35,
        [OutcomeInconclusive, OutcomeEnded],
    ),
    (
        "one tick inside the boundary is a broken bound",
        [BoundedReading("git", False, 35.0), BoundedReading("cmake", True, 34.9)],
        5,
        35,
        [OutcomeUnreachableBound, OutcomeEnded],
    ),
)


def run_self_test():
    """Drive every verdict against synthesised readings. Returns an exit code.

    In the DEFAULT set, and driven against records rather than against a stall,
    because a classifier that cannot be made to say INCONCLUSIVE cannot report a
    starved runner -- and a fixture that can only answer the two outcomes its author
    expected will answer one of them whatever it sees. The second case below is the
    measured `Windows-cl-debug` run this ticket was filed from, entered as data.
    """
    failed = 0
    for name, readings, window, patience, expected in SELF_TEST_CASES:
        got = [one.outcome for one in classify_bounded(readings, window, patience)]
        if got == list(expected):
            print("  ok   %s" % name)
        else:
            failed += 1
            print("  FAIL %s: expected %s, got %s" % (name, list(expected), got))

    # A self-test that stopped early must not look like one that judged something.
    print(
        "fetch-transfer-bound --self-test: %d case(s) ran, %d failed"
        % (len(SELF_TEST_CASES), failed)
    )
    # Every outcome must appear, or the table has quietly stopped exercising one.
    seen = set()
    for _, readings, window, patience, _expected in SELF_TEST_CASES:
        seen.update(one.outcome for one in classify_bounded(readings, window, patience))
    for outcome in (OutcomeEnded, OutcomeInconclusive, OutcomeUnreachableBound):
        if outcome not in seen:
            print("FAIL: no case produces %s, so that verdict is unwatched" % outcome)
            failed += 1
    return 1 if failed else 0


def stall_url(stall):
    """The URL a transport is pointed at to reach the stall."""
    return "http://127.0.0.1:%d/stall.git" % stall.port


def read_shipped_numbers(arguments, workspace):
    """The window and floor THIS TREE will use, or why there are none.

    The values come from the build tree when it passed them
    (``--configured-window``/``--configured-floor``, filled in by
    `src/tests/CMakeLists.txt` where they are resolved against `CMakeCache.txt`)
    and from the module otherwise. That distinction is the whole point: the
    module's numbers are CACHE variables, and a `cmake -P` has no cache, so a
    probe alone always reads the defaults -- it would have reported "shipped
    window is 120s" for a tree configured with `-DFASTCACHED_FETCH_SILENCE_SECONDS=0`,
    which is the one tree where the answer matters.

    The probe still runs, because it is what asserts the DELIVERY CHANNEL: that
    including the module puts these two names into the environment at all.
    """
    exported = read_shipped_bound(arguments.cmake, arguments.source_dir, workspace)
    source = "the module's defaults"
    table = dict(exported)
    if arguments.configured_window or arguments.configured_floor:
        source = "this build tree"
        table["GIT_HTTP_LOW_SPEED_TIME"] = arguments.configured_window
        table["GIT_HTTP_LOW_SPEED_LIMIT"] = arguments.configured_floor
    missing = [
        name
        for name in ("GIT_HTTP_LOW_SPEED_TIME", "GIT_HTTP_LOW_SPEED_LIMIT")
        if not exported.get(name)
    ]
    window, window_complaint = check_number(
        "GIT_HTTP_LOW_SPEED_TIME",
        table.get("GIT_HTTP_LOW_SPEED_TIME", ""),
        WINDOW_CEILING_SECONDS,
        "second(s)",
        "a bound that outlives the person who would otherwise have killed the "
        "configure changes nothing",
    )
    floor, floor_complaint = check_number(
        "GIT_HTTP_LOW_SPEED_LIMIT",
        table.get("GIT_HTTP_LOW_SPEED_LIMIT", ""),
        FLOOR_CEILING_BYTES_PER_SECOND,
        "byte(s)/second",
        "the argument for the shipped floor is that it is indistinguishable from "
        "zero, and a floor that fires on a working transfer trades a hang for a "
        "broken build",
    )
    complaints = [c for c in (window_complaint, floor_complaint) if c]
    if missing:
        complaints.append(
            "including cmake/FetchTransferBound.cmake leaves %s unset, so the module "
            "no longer delivers the bound to git at all"
            % " and ".join(sorted(missing))
        )
    return window, floor, complaints, source


def main():
    # Ahead of the parser, whose `--source-dir` is required: the self-test judges a pure
    # function and wants no tree, no git and no stall.
    if "--self-test" in sys.argv[1:]:
        return run_self_test()

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", required=True)
    parser.add_argument("--git", default="git")
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument(
        "--window",
        type=int,
        default=5,
        help="seconds of silence the bounded legs are told to tolerate; the shipped "
        "window is used instead when --full is given",
    )
    parser.add_argument(
        "--control",
        type=int,
        default=None,
        help="seconds an UNBOUNDED transport must still be running after; must "
        "exceed the window in force, or the control cannot say the BOUND is what "
        "ended the bounded leg. Defaults to that window plus one second, so it "
        "follows --full rather than having to be moved with it.",
    )
    parser.add_argument(
        "--full",
        action="store_true",
        help="run the bounded legs at the shipped window rather than a compressed "
        "one; slow by construction",
    )
    parser.add_argument(
        "--configured-window",
        default="",
        help="FASTCACHED_FETCH_SILENCE_SECONDS as THIS BUILD TREE resolved it; "
        "passed by the ctest registration, because a `cmake -P` probe has no cache "
        "and would always read the module's default instead",
    )
    parser.add_argument(
        "--configured-floor",
        default="",
        help="FASTCACHED_FETCH_MIN_BYTES_PER_SECOND as this build tree resolved it",
    )
    arguments = parser.parse_args()

    # The static halves are pure file reads and answer for the TREE, so they run
    # before anything can skip. Losing them behind a missing git would take the
    # "no download in this tree is left unbounded" assertion with it, silently,
    # behind a green Skipped -- skipped is not passed.
    inconclusive = []
    failures = check_wiring(arguments.source_dir)
    failures += check_downloads_are_bounded(arguments.source_dir)
    notes = []
    exercised = shipped = None
    source = None

    for name, tool in (("git", arguments.git), ("cmake", arguments.cmake)):
        if shutil.which(tool) is None:
            if failures:
                return report(failures, notes, None)
            print(
                "SKIP: the tree's wiring is intact, but %s was not found, so no "
                "transport could be watched refusing a stall" % name
            )
            return SKIP

    workspace = tempfile.mkdtemp(prefix="fetch-transfer-bound-")
    stall = Stall()
    try:
        shipped, floor, complaints, source = read_shipped_numbers(arguments, workspace)
        failures += complaints
        if shipped is not None and floor is not None:
            exercised = shipped if arguments.full else arguments.window
            # The two are one setting in two halves. The control leg's whole
            # job is to say the BOUND is what ended the bounded leg, and it can
            # only say that by outlasting it: an unbounded transport still
            # running at 5s tells you nothing about a bounded one that ended at
            # 120s. Defaulted from the window rather than pinned, because it was
            # pinned at 6 and `--full` then produced a 120s refusal against a 6s
            # control -- weaker evidence than it read as, which the guard caught
            # on itself.
            if arguments.control is None:
                arguments.control = exercised + 1
            if arguments.control <= exercised:
                failures.append(
                    "--control (%ds) does not exceed the window in force (%ds), so "
                    "the control leg cannot say the bound is what ended the bounded "
                    "leg" % (arguments.control, exercised)
                )
            else:
                ran, said, undecided = run_transports(arguments, stall, workspace, exercised, floor)
                failures += ran
                notes += said
                inconclusive += undecided
        # else: the runs are SKIPPED rather than run against a number the tree
        # disowns -- a green leg proving a number nobody ships is the worse
        # outcome, and `failures` already names why.
    finally:
        stall.close()
        shutil.rmtree(workspace, ignore_errors=True)

    return report(failures, notes, inconclusive, (exercised, shipped, source, arguments.full))


def report(failures, notes, inconclusive, verdict):
    """Print the evidence, then the verdict. Returns the exit code.

    The verdict names the window that was actually exercised AND the window the
    bound is set to, because they differ by default: quoting one for the other
    is the failure this project has already paid for once, and a figure that
    states its own conditions is what prevents it.

    A real FAILURE outranks an inconclusive leg: if any bound was shown broken on a
    host demonstrably fast enough, that is the answer whatever else could not be
    judged.

    **The exit status collapses INCONCLUSIVE onto 77, and that is ctest's vocabulary
    rather than a decision.** A registered test can be passed, failed or skipped, and
    `SKIP_RETURN_CODE` takes exactly one value -- 77 here, already meaning *a
    prerequisite is missing*. Green would be worse: it would read as *the bound is
    proven* on precisely the loaded runs where nothing was proven. So the two share a
    status and the OUTPUT is what tells them apart, which is the same
    channel-is-fully-occupied problem as #1152 and wants the same kind of fix.
    """
    for note in notes:
        print("  " + note)
    for undecided in inconclusive:
        print("")
        print("INCONCLUSIVE: " + undecided)
    if failures:
        print("")
        for failure in failures:
            print("FAIL: " + failure)
        print("")
        print("fetch-transfer-bound: %d of the guard's assertions failed" % len(failures))
        return 1

    if inconclusive:
        print("")
        print(
            "fetch-transfer-bound: %d leg(s) could not be judged on this host, so this run "
            "proves nothing about the bound and is reported as SKIPPED rather than as a pass "
            "or a failure. Nothing here says the bound is broken." % len(inconclusive)
        )
        return 77

    exercised, configured, source, full = verdict
    print(
        "fetch-transfer-bound: both transports refuse a stalled transfer. Window "
        "exercised: %ds%s. Window in force per %s: %ds."
        % (exercised, "" if full else " (compressed)", source, configured)
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
