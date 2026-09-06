#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# Which clang-tidy legs EXIST, derived from `.github/workflows/build.yml` rather
# than restated anywhere. A second copy of that list would be a second thing to be
# wrong, and the failure would be silent: rows claiming coverage from a leg that
# was renamed away.
#
# A separate file rather than a heredoc inside `check-tidy-blind-spots.sh`, for the
# reason its sibling `tidy-blind-spots-scan.py` already gives: bash 3.2 -- which
# macOS ships -- cannot parse a here-document inside a command substitution. It
# scans the heredoc BODY looking for the closing paren, so any stray quote or paren
# in what is meant to be inert text ends the parse.
#
# That is not hypothetical here and it is worth the specifics, because the trap is
# invisible on every platform anybody develops on. This scanner lived as such a
# heredoc and parsed fine for as long as its Python comments happened to contain no
# apostrophe. One was added -- the word `sweep's`, in a COMMENT -- and bash 3.2
# reported `line 100: unexpected EOF while looking for matching \`)\'` followed by a
# syntax error at the last line of the file, so the whole check died before running
# and every selftest case refused for a reason unrelated to what it tests. Measured
# against bash 3.2.57 in a container; removing that single apostrophe and changing
# nothing else made it parse again. The wording is not the fix -- the construct is.
import re, sys

# A leg is a job whose steps invoke the sweep IN A SWEEPING MODE. Scanned line by
# line, tracking the current job key, rather than split on a regex -- the first
# version split on the job separator and the split CONSUMED the indent the match
# then required, so it found nothing and the check refused a correct tree.
#
# Read as text rather than through a YAML parser so this needs no third-party
# module on any platform.
#
# `--ci` ALONE was the test until a second leg existed, and it was wrong the moment
# one did: the Windows leg sweeps with `--only=`, so it was not a leg by this
# reading and every row claiming its coverage was refused -- a correct tree called
# broken, and a detector keyed on the only spelling that existed when it was
# written. The general shape is the same one the sweep's own flag strip had
# (#858): ONE spelling of a thing, standing in for the class.
#
# An UNRECOGNISED mode REFUSES rather than being assumed non-sweeping. A mode
# added to `tidy-sweep.sh` and used here would otherwise make a real leg invisible
# and take every row that names it down with it -- which is this defect again, one
# release later. `--self-test` is the one mode that is deliberately not a sweep.
SWEEPING = ("--ci", "--all", "--only=")
NOT_A_SWEEP = ("--self-test",)

# A COMMENT IS NOT A CALL SITE. `build.yml` carries the string
# `scripts/tidy-sweep.sh --self-test` inside a comment explaining why a step does
# not do something, and a scan that reads comments attributes invocations to
# whatever job they are discussed in.
def code(line):
    stripped = line.lstrip()
    return "" if stripped.startswith("#") else line

job, found, unknown = None, [], []
for raw in open(sys.argv[1]):
    line = code(raw)
    m = re.match(r"  ([A-Za-z0-9_-]+):\s*$", line)
    if m:
        job = m.group(1)
        continue
    if not job or "tidy-sweep.sh" not in line:
        continue
    flags = re.findall(r"--[A-Za-z0-9-]+=?", line)
    if not flags:
        unknown.append(job + ": " + line.strip())
        continue
    for flag in flags:
        if flag in SWEEPING:
            if job not in found:
                found.append(job)
        elif flag not in NOT_A_SWEEP:
            unknown.append(job + ": " + flag)
if unknown:
    for u in unknown:
        print("UNKNOWN-SWEEP-MODE " + u, file=sys.stderr)
    sys.exit(2)
for j in found:
    print(j)