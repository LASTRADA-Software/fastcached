#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Watches the dependency-fetch transfer bound (#526) actually refuse a stall.

Three configures wedged in one evening on fresh worktrees, each parked in
``read()`` on a socket that had delivered nothing, because git and CMake both
wait forever on a transfer that stops delivering. ``cmake/FetchTransferBound.cmake``
bounds both; this is what shows the bound firing, because a guard nobody has
watched refuse is not a guard -- and the fix applied by hand on the night of the
wedges was never once exercised, which is exactly the gap this closes.

WHAT IT PROVES, precisely, because the distinction matters when this is cited:

  * The subject can be made to hang.  Each transport is run against the stall
    with its bound REMOVED and must still be running when the control window
    ends.  Without this the refusals below would prove nothing: a stall that
    ends on its own is refused by everything.
  * Each transport refuses that same stall once the bound is applied, at the
    rate floor this tree ships, and says so in words naming the transfer.
  * This tree wires the module to both transports -- a bound nothing includes
    is a file, not a guard.

  * NOT the shipped WINDOW.  By default the window is compressed (``--window``)
    so this stays a seconds-long default-set test rather than a four-minute one.
    What is compressed is how long git and CMake count before giving up; the
    rate floor, the mechanism, the delivery channel and the wiring are all the
    shipped ones.  Pass ``--full`` to run it at the shipped window instead --
    that takes a little over twice ``FASTCACHED_FETCH_SILENCE_SECONDS`` and is
    how the shipped number itself gets watched.

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

SKIP = 77

# Windows: git's own stall shows as `git.exe` spawning `git-remote-http.exe`.
# Killing only the parent leaves the grandchild holding the pipe write ends and
# a drain on those pipes never returns -- the same shape the rulebook records
# for a wedged compile (#239).  So every child is run with its output on FILES
# rather than pipes, and killed as a TREE.
IS_WINDOWS = platform.system() == "Windows"


class Stall:
    """A listener that accepts connections and answers nothing, ever."""

    def __init__(self):
        self._socket = socket.socket()
        self._socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._socket.bind(("127.0.0.1", 0))
        self._socket.listen(16)
        self.port = self._socket.getsockname()[1]
        self._accepted = []
        self._thread = threading.Thread(target=self._accept_loop, daemon=True)
        self._thread.start()

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


class Outcome:
    """What one run of a transport against the stall did.

    ``ended`` is the whole verdict; ``seconds`` and ``output`` are the evidence
    a reader needs when it disagrees with what was expected.
    """

    def __init__(self, ended, seconds, code, output):
        self.ended = ended
        self.seconds = seconds
        self.code = code
        self.output = output


def run_against_stall(command, environment, patience, workdir):
    """Run ``command``, waiting at most ``patience`` seconds for it to end."""
    child_environment = dict(os.environ)
    # Whatever the developer or CI has set for these must not decide the
    # verdict: the control leg has to be genuinely unbounded and the bounded
    # leg has to be bounded by the values under test.
    for name in ("GIT_HTTP_LOW_SPEED_LIMIT", "GIT_HTTP_LOW_SPEED_TIME"):
        child_environment.pop(name, None)
    child_environment.update(environment)
    child_environment["GIT_TERMINAL_PROMPT"] = "0"

    log = os.path.join(workdir, "output.txt")
    started = time.monotonic()
    with open(log, "wb") as sink:
        process = subprocess.Popen(
            command,
            cwd=workdir,
            env=child_environment,
            stdout=sink,
            stderr=subprocess.STDOUT,
            **({} if IS_WINDOWS else {"start_new_session": True}),
        )
        try:
            process.wait(timeout=patience)
            ended = True
        except subprocess.TimeoutExpired:
            kill_tree(process)
            ended = False
    with open(log, "rb") as source:
        output = source.read().decode("utf-8", "replace").strip()
    return Outcome(ended, time.monotonic() - started, process.returncode, output)


def read_exported_table(cmake, source_dir):
    """Read what cmake/FetchTransferBound.cmake exports, by running it.

    Reading the numbers back out of the module that sets them is the difference
    between asserting the mechanism works and asserting THIS TREE applies it.
    A copy of the values here would be a second thing to go stale, not a
    cross-check.
    """
    module = os.path.join(source_dir, "cmake", "FetchTransferBound.cmake")
    result = subprocess.run(
        [cmake, "-P", module], capture_output=True, text=True, check=False
    )
    if result.returncode != 0:
        raise RuntimeError(
            "running %s failed (%s):\n%s" % (module, result.returncode, result.stderr)
        )
    table = {}
    for line in (result.stdout + result.stderr).splitlines():
        if "=" in line:
            name, _, value = line.strip().partition("=")
            table[name] = value
    return table


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", required=True)
    parser.add_argument("--git", default="git")
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument(
        "--window",
        type=int,
        default=5,
        help="seconds of silence the bounded legs are told to tolerate; the "
        "shipped window is used instead when --full is given",
    )
    parser.add_argument(
        "--control",
        type=int,
        default=6,
        help="seconds an UNBOUNDED transport must still be running after",
    )
    parser.add_argument(
        "--full",
        action="store_true",
        help="run the bounded legs at the shipped window rather than a "
        "compressed one; slow by construction",
    )
    arguments = parser.parse_args()

    for name, tool in (("git", arguments.git), ("cmake", arguments.cmake)):
        if shutil.which(tool) is None and not os.path.isfile(tool):
            print("SKIP: %s was not found, so this guard cannot run" % name)
            return SKIP

    table = read_exported_table(arguments.cmake, arguments.source_dir)
    failures = []
    notes = []

    # ---------------------------------------------------------------- wiring
    shipped_window = table.get("FASTCACHED_FETCH_SILENCE_SECONDS", "")
    shipped_floor = table.get("FASTCACHED_FETCH_MIN_BYTES_PER_SECOND", "")
    for name, expected in (
        ("GIT_HTTP_LOW_SPEED_TIME", shipped_window),
        ("GIT_HTTP_LOW_SPEED_LIMIT", shipped_floor),
    ):
        if not expected or not expected.isdigit() or int(expected) <= 0:
            failures.append(
                "cmake/FetchTransferBound.cmake does not state a positive value for "
                "the source of %s (got %r)" % (name, expected)
            )
        elif table.get(name) != expected:
            failures.append(
                "cmake/FetchTransferBound.cmake exports %s=%r but states %r; the "
                "environment git reads and the number the tree documents have "
                "diverged" % (name, table.get(name), expected)
            )

    # Comments are dropped before this is read.  Both needles below appear in
    # the prose of that file explaining why they are there, and prose is not
    # wiring -- matching it would let the comment satisfy the check while the
    # code stopped doing so.  It also had the ordering backwards on the first
    # run, which is how this is known rather than assumed.
    with open(os.path.join(arguments.source_dir, "cmake", "CPM.cmake"), "rb") as source:
        raw = source.read().decode("utf-8", "replace")
    bootstrap = "\n".join(
        line for line in raw.splitlines() if not line.lstrip().startswith("#")
    )
    include_at = bootstrap.find("FetchTransferBound.cmake")
    download_at = bootstrap.find("file(DOWNLOAD")
    if include_at < 0:
        failures.append(
            "cmake/CPM.cmake does not include FetchTransferBound.cmake, so nothing "
            "applies the bound to any dependency fetch"
        )
    elif download_at < 0 or include_at > download_at:
        failures.append(
            "cmake/CPM.cmake includes FetchTransferBound.cmake at or after its "
            "file(DOWNLOAD), so the bootstrap download is unbounded"
        )
    if "INACTIVITY_TIMEOUT ${FASTCACHED_FETCH_SILENCE_SECONDS}" not in bootstrap:
        failures.append(
            "cmake/CPM.cmake's file(DOWNLOAD) does not take its INACTIVITY_TIMEOUT "
            "from FASTCACHED_FETCH_SILENCE_SECONDS"
        )

    # --------------------------------------------------------------- the runs
    window = int(shipped_window) if arguments.full and shipped_window.isdigit() else arguments.window
    # A bounded leg is given the window plus room for the connect, the request
    # and process teardown.  Generous on purpose: this deadline exists so a
    # broken bound reports rather than hangs, not to measure anything.
    patience = window + 30

    stall = Stall()
    workspace = tempfile.mkdtemp(prefix="fetch-transfer-bound-")
    url = "http://127.0.0.1:%d/stall.git" % stall.port
    try:
        def download_script(inactivity_timeout):
            """A one-line `file(DOWNLOAD)` against the stall, bounded or not."""
            path = os.path.join(
                workspace,
                "download-%s.cmake" % (inactivity_timeout or "unbounded"),
            )
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

        # One row per transport: how it is run without the bound, how it is run
        # with it, and what its refusal has to say.  Both transports are asked
        # the same two questions, so a third one is a row rather than a branch.
        legs = [
            (
                "git",
                [arguments.git, "clone", url, "clone-dir"],
                {},
                [arguments.git, "clone", url, "clone-dir"],
                {
                    "GIT_HTTP_LOW_SPEED_LIMIT": shipped_floor,
                    "GIT_HTTP_LOW_SPEED_TIME": str(window),
                },
                re.compile(r"too slow|low speed|timed out|timeout", re.IGNORECASE),
            ),
            (
                "cmake file(DOWNLOAD)",
                [arguments.cmake, "-P", download_script(None)],
                {},
                [arguments.cmake, "-P", download_script(window)],
                {},
                re.compile(r"inactivity|timeout|timed out", re.IGNORECASE),
            ),
        ]

        for (
            label,
            control_command,
            control_environment,
            bounded_command,
            bound_environment,
            refusal,
        ) in legs:
            control_dir = tempfile.mkdtemp(prefix="control-", dir=workspace)
            control = run_against_stall(
                control_command, control_environment, arguments.control, control_dir
            )
            if control.ended:
                failures.append(
                    "CONTROL: unbounded %s ended by itself after %.1fs (rc=%s), so the "
                    "stall is not a stall and nothing below proves anything.\n%s"
                    % (label, control.seconds, control.code, control.output)
                )
            else:
                notes.append(
                    "control: unbounded %s still running after %ds -- the stall holds"
                    % (label, arguments.control)
                )

            bounded_dir = tempfile.mkdtemp(prefix="bounded-", dir=workspace)
            bounded = run_against_stall(
                bounded_command, bound_environment, patience, bounded_dir
            )
            if not bounded.ended:
                failures.append(
                    "bounded %s did NOT end within %ds of a %ds silence window; the "
                    "bound is not reaching it.\n%s"
                    % (label, patience, window, bounded.output)
                )
            elif bounded.code == 0:
                failures.append(
                    "bounded %s ended after %.1fs but reported SUCCESS against a "
                    "server that sent nothing.\n%s"
                    % (label, bounded.seconds, bounded.output)
                )
            elif not refusal.search(bounded.output):
                failures.append(
                    "bounded %s ended after %.1fs (rc=%s) but its message names no "
                    "transfer refusal, so an operator cannot tell this from an "
                    "unrelated failure.\n%s"
                    % (label, bounded.seconds, bounded.code, bounded.output)
                )
            else:
                notes.append(
                    "bounded %s refused after %.1fs (window %ds, floor %s B/s): %s"
                    % (
                        label,
                        bounded.seconds,
                        window,
                        shipped_floor,
                        bounded.output.splitlines()[-1][:160],
                    )
                )
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
        print(
            "fetch-transfer-bound: %d of the guard's assertions failed" % len(failures)
        )
        return 1

    print(
        "fetch-transfer-bound: both transports refuse a stalled transfer; window "
        "exercised was %ds%s, shipped window is %ss"
        % (window, "" if arguments.full else " (compressed)", shipped_window)
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
