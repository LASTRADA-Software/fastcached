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
        needle="INACTIVITY_TIMEOUT ${FASTCACHED_FETCH_SILENCE_SECONDS}",
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
    # Whatever the developer or CI has set for these must not decide the
    # verdict: the control leg has to be genuinely unbounded, and the bounded
    # leg has to be bounded by the values under test.
    for name in ("GIT_HTTP_LOW_SPEED_LIMIT", "GIT_HTTP_LOW_SPEED_TIME"):
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
    """
    violations = []
    for root, directories, files in os.walk(source_dir):
        directories[:] = [
            d for d in directories if not d.startswith(".") and d not in ("out",)
        ]
        for name in files:
            if name != "CMakeLists.txt" and not name.endswith(".cmake"):
                continue
            path = os.path.join(root, name)
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
                if "INACTIVITY_TIMEOUT" not in call:
                    line = body.count("\n", 0, match.start()) + 1
                    violations.append(
                        "%s (non-comment line %d): a file(DOWNLOAD) with no "
                        "INACTIVITY_TIMEOUT -- a stalled response hangs the configure "
                        "forever, which is #526"
                        % (os.path.relpath(path, source_dir).replace("\\", "/"), line)
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

    clone = [arguments.git, "clone", url, "clone-dir"]
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
    """Both phases against the stall. Returns (failures, notes)."""
    failures, notes = [], []
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
    # rather than hangs, not to measure anything.
    patience = window + 30
    for leg, bounded in zip(
        legs,
        run_phase(
            legs,
            workspace,
            lambda leg: leg.bounded_command,
            lambda leg: leg.bound_environment,
            patience,
            "bounded-",
        ),
    ):
        if not bounded.ended:
            failures.append(
                "bounded %s did NOT end within %ds of a %ds silence window; the "
                "bound is not reaching it.\n%s"
                % (leg.label, patience, window, bounded.output)
            )
        elif bounded.code == 0:
            failures.append(
                "bounded %s ended after %.1fs but reported SUCCESS against a server "
                "that sent nothing.\n%s"
                % (leg.label, bounded.seconds, bounded.output)
            )
        elif not leg.refusal.search(bounded.output):
            failures.append(
                "bounded %s ended after %.1fs (rc=%s) but its message names no "
                "transfer refusal, so an operator cannot tell this from an unrelated "
                "failure.\n%s"
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
    return failures, notes


def stall_url(stall):
    """The URL a transport is pointed at to reach the stall."""
    return "http://127.0.0.1:%d/stall.git" % stall.port


def read_shipped_numbers(arguments, workspace):
    """The shipped window and floor, or complaints saying why there are none."""
    table = read_shipped_bound(arguments.cmake, arguments.source_dir, workspace)
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
    return window, floor, complaints


def main():
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
    arguments = parser.parse_args()

    for name, tool in (("git", arguments.git), ("cmake", arguments.cmake)):
        if shutil.which(tool) is None:
            print("SKIP: %s was not found, so this guard cannot run" % name)
            return SKIP

    failures = check_wiring(arguments.source_dir)
    failures += check_downloads_are_bounded(arguments.source_dir)
    notes = []
    exercised = shipped = None

    workspace = tempfile.mkdtemp(prefix="fetch-transfer-bound-")
    stall = Stall()
    try:
        shipped, floor, complaints = read_shipped_numbers(arguments, workspace)
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
                ran, said = run_transports(arguments, stall, workspace, exercised, floor)
                failures += ran
                notes += said
        # else: the runs are SKIPPED rather than run against a number the tree
        # disowns -- a green leg proving a number nobody ships is the worse
        # outcome, and `failures` already names why.
    finally:
        stall.close()
        shutil.rmtree(workspace, ignore_errors=True)

    for note in notes:
        print("  " + note)
    if failures:
        print("")
        for failure in failures:
            print("FAIL: " + failure)
        print("")
        print("fetch-transfer-bound: %d of the guard's assertions failed" % len(failures))
        return 1

    print(
        "fetch-transfer-bound: both transports refuse a stalled transfer; window "
        "exercised was %ds%s, shipped window is %ds"
        % (exercised, "" if arguments.full else " (compressed)", shipped)
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
