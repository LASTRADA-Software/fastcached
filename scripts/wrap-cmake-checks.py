#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# #1103's SECOND HALF, COMMITTED BUT NOT YET APPLIED.
#
# Nothing runs this. It is here because it is dry-run-proven and expensive to
# rebuild, and because the alternative -- a migration script living in one
# developer's scratch directory -- is a machine-local artefact that dies with the
# machine.
#
# WHAT IT DOES: routes every registered `cmake -P` check through
# `scripts/run-check.sh --command --`, so a KILLED one is told from a FAILING one
# by something PRESENT in the log rather than by the absence of `CMake Error`.
# The wrapper it depends on has landed; the 74 registrations have not.
#
# WHY IT IS HELD RATHER THAN APPLIED: it rewrites 74 blocks in
# `src/tests/CMakeLists.txt`, which several branches are appending to at any
# time, and landing it turns their clean appends into real conflicts. It waits
# for a drained queue.
#
# WHEN IT IS APPLIED, ONE THING MUST TRAVEL WITH IT: `check-run-check-coverage.sh`
# flips from COUNTING uncovered `cmake -P` invocations to REFUSING them, in the
# SAME change. Ahead of the rewrite that check is red on arrival against a master
# where none of the 74 is wrapped yet; behind it, a branch adding an old-style
# registration merges cleanly and is silently uncovered.
#
# MEASURED before being trusted, on the tree at `9f0191ba`:
#   * 74 blocks would be wrapped, agreeing with the figure
#     `check-run-check-coverage.sh` derives independently;
#   * the `-P` filter DISCRIMINATES rather than matching everything -- proven on a
#     staged fixture holding a `${CMAKE_COMMAND} -E` utility registration beside
#     two real checks: 2 wrapped, 1 left alone, both the single-line and
#     multi-line COMMAND forms transformed with the argv preserved exactly. That
#     control is load-bearing: against the real file the "left alone" arm reports
#     0, so it cannot by itself tell a correct filter from one that matches
#     everything.
#
# There is no one-place alternative, measured rather than assumed:
# `get_test_property(<test> COMMAND out)` reads NOTFOUND, and SETTING that
# property makes ctest report `Test command: NOT_AVAILABLE` -- it does not fail to
# wrap the test, it silently destroys it.
#
# Usage:  python3 scripts/wrap-cmake-checks.py src/tests/CMakeLists.txt --check
#         python3 scripts/wrap-cmake-checks.py src/tests/CMakeLists.txt --expect 74
"""Route every registered `cmake -P` check through `run-check.sh --command --`.

Held rather than run: the manager decides whether this lands now, because it
rewrites 74 blocks in the file 14 other live branches are editing.

THE TRANSFORM, stated so the diff can be read rather than trusted. Only the
SPAWN changes; every `-D` argument, the script path, and the block's shape are
untouched:

    COMMAND ${CMAKE_COMMAND}                        COMMAND ${FASTCACHED_BASH} "${CMAKE_SOURCE_DIR}/scripts/run-check.sh" --command --
        "-DFOO=..."                     ->              ${CMAKE_COMMAND}
        -P ".../check-x.cmake"                          "-DFOO=..."
                                                        -P ".../check-x.cmake"

A block qualifies only if it is an `add_test` COMMAND whose argv reaches a
`-P <script>`. `${CMAKE_COMMAND} -E ...` utility invocations are NOT checks and
are left alone -- an over-broad match here would wrap things whose verdict is not
read from their output at all.

Every count is asserted. `--check` reports without writing.
"""
import argparse
import io
import re
import sys

WRAP = ('COMMAND ${FASTCACHED_BASH} "${CMAKE_SOURCE_DIR}/scripts/run-check.sh" '
        '--command --')


def rewrite(text):
    lines = text.split("\n")
    out, i, wrapped, skipped_e = [], 0, [], 0

    while i < len(lines):
        line = lines[i]
        m = re.match(r"^(\s*)COMMAND \$\{CMAKE_COMMAND\}(.*)$", line)
        if not m:
            out.append(line)
            i += 1
            continue

        indent, tail = m.group(1), m.group(2)

        # Collect the block's argv lines: this line plus continuations, up to the
        # closing paren or the next keyword at the same level.
        block = [tail]
        j = i + 1
        while j < len(lines):
            nxt = lines[j]
            if re.match(r"^\s*\)", nxt) or re.match(
                    r"^\s*(NAME|COMMAND|WORKING_DIRECTORY|CONFIGURATIONS)\b", nxt):
                break
            if not nxt.strip():
                break
            block.append(nxt)
            j += 1

        joined = "\n".join(block)
        if not re.search(r"(^|\s)-P\s", joined):
            # `${CMAKE_COMMAND} -E ...` and friends: not a check.
            skipped_e += 1
            out.append(line)
            i += 1
            continue

        # Wrap. The original `${CMAKE_COMMAND}` and everything after it move down
        # one line, indented one step further, so the argv reads unchanged.
        script = re.search(r"-P\s+\"?\$\{CMAKE_SOURCE_DIR\}/(scripts/[\w./-]+)", joined)
        wrapped.append(script.group(1) if script else "<unresolved>")
        out.append(indent + WRAP)
        out.append(indent + "    ${CMAKE_COMMAND}" + tail)
        out.extend(lines[i + 1:j])
        i = j

    return "\n".join(out), wrapped, skipped_e


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ap.add_argument("--check", action="store_true", help="report, write nothing")
    ap.add_argument("--expect", type=int, default=None,
                    help="required number of wrapped blocks; a mismatch is an error")
    args = ap.parse_args()

    text = io.open(args.path, encoding="utf-8", newline="").read()
    if WRAP in text:
        sys.stderr.write("refusing: the file already contains the wrapper spelling, "
                         "so this would wrap already-wrapped registrations\n")
        return 2

    new, wrapped, skipped = rewrite(text)
    sys.stdout.write("would wrap %d `cmake -P` check block(s)\n" % len(wrapped))
    sys.stdout.write("left alone: %d `${CMAKE_COMMAND}` block(s) with no -P "
                     "(utility invocations, not checks)\n" % skipped)
    unresolved = [w for w in wrapped if w == "<unresolved>"]
    if unresolved:
        sys.stderr.write("refusing: %d block(s) have a -P whose script path this "
                         "script could not read; a silent partial rewrite is worse "
                         "than none\n" % len(unresolved))
        return 2
    if args.expect is not None and len(wrapped) != args.expect:
        sys.stderr.write("refusing: wrapped %d, expected %d\n"
                         % (len(wrapped), args.expect))
        return 2
    if args.check:
        for w in sorted(set(wrapped)):
            sys.stdout.write("    %s\n" % w)
        return 0
    io.open(args.path, "w", encoding="utf-8", newline="").write(new)
    sys.stdout.write("written\n")
    return 0


sys.exit(main())
