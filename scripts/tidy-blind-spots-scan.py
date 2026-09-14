#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# The measurement behind `check-tidy-blind-spots.sh`: which translation units in a
# compile database contributed nothing, asked of the OBJECTS rather than the source.
#
# A separate file rather than a heredoc inside the shell script, and that is not a
# style choice: bash 3.2 -- which macOS still ships, and which `FASTCACHED_BASH`
# resolves to there -- cannot parse a here-document inside a command substitution.
# It fails at PARSE time with "unexpected EOF while looking for matching quote", so
# the whole check dies before running and every selftest case refuses for a reason
# that has nothing to do with what it tests.
#
# Reads NM_BIN, DB and REPO from the environment. Prints one path per blind unit on
# stdout; diagnostics prefixed `!ERROR` on stderr with exit 2.
import json, os, subprocess, sys
db = json.load(open(os.environ["DB"]))
nm, repo = os.environ["NM_BIN"], os.environ["REPO"]


def address_sanitized(cmd):
    """Whether AddressSanitizer is ON for this compile, the LAST flag deciding.

    The two-symbol signature below is the sanitizer's, so it measures only an object the
    sanitizer instrumented. A unit compiled with it turned off -- `sanitizer-absent-probe`,
    the vendor-sanitized control, is `-fno-sanitize=all` ON PURPOSE -- defines exactly its
    own code, and one small function is 1 symbol: scored, it read as blind while clang-tidy
    reads its source like any other. Order matters because target options follow the
    directory-scoped `-fsanitize=address`, and a later `-fsanitize=` turns it back on.

    The exemption is THE COMPILE LINE DISABLED THE SANITIZER, read from the command, and
    nothing else. It is not "objects with few symbols": an instrumented object with one
    symbol is exactly what the signature exists to call blind, and it still is.
    """
    on = False
    for token in cmd:
        if token.startswith("-fsanitize="):
            if "address" in token.split("=", 1)[1].split(","):
                on = True
        elif token.startswith("-fno-sanitize="):
            values = token.split("=", 1)[1].split(",")
            if "all" in values or "address" in values:
                on = False
    return on


best = {}
unmeasured = set()
for e in db:
    src = e["file"]
    cmd = e["command"].split()
    if "-o" not in cmd:
        continue
    obj = os.path.join(e["directory"], cmd[cmd.index("-o") + 1])
    if not os.path.exists(obj):
        continue
    r = subprocess.run([nm, "--defined-only", obj], capture_output=True, text=True)
    n = len([l for l in r.stdout.splitlines() if l.strip()])
    # A file compiled into several targets is blind only if it is blind everywhere,
    # so the smallest count is the honest one.
    key = os.path.relpath(src, repo)
    # Only this project's own sources. Anything else is somebody else's code and
    # not this check's business -- and the exclusion is stated POSITIVELY, as "under
    # src/", rather than as a list of third-party directories to skip. A skip-list
    # was the first version and it named `_deps`, which is where CPM puts its
    # sources locally; CI caches them under `.cache/CPM/` INSIDE the repository, so
    # the filter looked right here and admitted 40-odd Catch2 units there. A
    # denylist is exact about what it knows and silent about what it does not.
    if not key.startswith("src" + os.sep):
        continue
    if not address_sanitized(cmd):
        unmeasured.add(key)
        continue
    best[key] = min(best.get(key, 1 << 30), n)
# Named, never dropped: a unit this cannot measure is a third answer beside blind and
# analysed. A unit measured by ANOTHER, sanitized compile keeps that measurement.
for key in sorted(unmeasured - best.keys()):
    print(f"NOTE {key}: not measured -- the compile line disabled the sanitizer, so "
          f"AddressSanitizer's empty-object signature does not apply to its object. That is the "
          f"whole exemption: an instrumented object with few symbols is still scored", file=sys.stderr)
if not best:
    print("!ERROR no objects found; run this after a build", file=sys.stderr)
    sys.exit(2)
# The positive control. If `nm` returns nothing for everything -- wrong binary, wrong
# object format -- every TU reads as blind and this check would "find" the whole tree.
# A run in which nothing is analysed is a broken instrument, not a discovery.
# TWO positive controls, one per axis, because the signature has two preconditions
# and they fail differently.
#
#   `EpollSocket.cpp`   -- a big, unconditionally-compiled unit. Blind here means the
#                          MEASUREMENT is broken (wrong `nm`, wrong object format), and
#                          without this the check would report the whole tree.
#   `TlsSocket_test.cpp` -- empty without `FASTCACHED_ENABLE_TLS` and analysed with it.
#                          Blind here means this BUILD is not configured like the sweep,
#                          so its blind set answers a different question than the table
#                          records. Measured: the same tree gives 11 in the sweep's
#                          configuration and 26 in a Release one without ASan, because
#                          the two-symbol signature is the sanitizer's.
for control, why in (("src/FastCache/Net/EpollSocket.cpp",
                      "the measurement is broken, not the tree -- check `nm`"),
                     ("src/FastCache/Net/TlsSocket_test.cpp",
                      "this build is not configured like the clang-tidy sweep "
                      "(needs FASTCACHED_ENABLE_TLS=ON), so its blind set is not the one "
                      "scripts/tidy-blind-spots.txt describes")):
    if control in best and best[control] <= 2:
        print(f"!ERROR positive control {control} measured as blind "
              f"({best[control]} symbols); {why}", file=sys.stderr)
        sys.exit(2)
for k in sorted(k for k, n in best.items() if n <= 2):
    print(k)
