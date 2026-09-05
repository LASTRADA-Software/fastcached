#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# TEMPORARY measurement harness for #379. Not a test, and it must not survive
# the branch it is on.
#
# #379 rests on one unmeasured fact: `COMMAND bash` in a ctest registration
# resolves through PATH, and on GitHub's macOS image Homebrew's bash 5.x may sit
# ahead of Apple's 2007 `/bin/bash`. If it does, every script test on macOS runs
# under bash 5 and the repository's bash-3.2 rule has no executor -- the rule is
# enforced by careful reading and nothing else.
#
# Both halves in one run: this file is registered TWICE, once as `COMMAND bash`
# and once as `COMMAND /bin/bash`. It reports the interpreter it is running
# under and whether `mapfile` (bash 4.0+, and the builtin whose use took
# `merge-queue-contexts` red on macOS once already) exists there.
#
# It exits 1 unconditionally, so BOTH registrations fail and ctest prints their
# output. A passing test's stdout is swallowed, which is precisely why the
# `BASH_VERSION` print added to a script during #378 never appeared in a macOS
# log and bought nothing.
set -uo pipefail

echo "PROBE interpreter=${BASH:-<unset>}"
echo "PROBE version=${BASH_VERSION:-<unset>}"
echo "PROBE versinfo_major=${BASH_VERSINFO[0]:-<unset>}"

if mapfile -t probeLines < <(printf 'a\nb\n') 2>/dev/null; then
    echo "PROBE mapfile=SUPPORTED count=${#probeLines[@]}"
else
    echo "PROBE mapfile=UNSUPPORTED"
fi

# Always non-zero: this is a measurement, and a measurement whose output ctest
# hides has measured nothing.
exit 1
