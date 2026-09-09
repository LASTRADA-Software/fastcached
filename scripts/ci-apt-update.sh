#!/usr/bin/env bash
# `apt-get update` that a third-party source cannot fail.
#
# The GitHub runner image ships vendor sources PREINSTALLED -- `packages.microsoft.com`
# and `dl.google.com` among them. Nothing in this repository installs from either, but
# `apt-get update` exits non-zero when ANY configured source is unreachable, so a 403
# from Microsoft's mirror failed a step that only wanted the Ubuntu archive (#550), and
# Google's Chrome source did the same thing again afterwards (#1160). It presents as an
# unrelated red check on whatever branch happened to be building.
#
# ## Why an ALLOWLIST of hosts, and not a longer list of vendors to remove
#
# This was a list of two named FILES until #1160, and the Chrome source walked straight
# past it: the step printed `1 third-party source(s) removed of 2 known` and did exactly
# what it was told while the job died on the vendor nobody had listed. A third row would
# fix that vendor and leave the next one, and the runner image adds one whenever it likes.
#
# So the direction is inverted, on this project's own rule: **an exclusion list bets on
# the world's layout; an inclusion list states your own.** `AllowedHosts` is the whole set
# of hosts this repository installs from, and anything else configured on the runner is
# removed unread. That keeps the old header's claim and strengthens it -- it used to say
# *nothing we ask for comes from these vendors*, which is a claim about them, and now says
# *everything we ask for comes from these hosts*, which is a claim about us.
#
# A file this parser can read no host out of is removed too. Keeping it would be the #1160
# failure exactly: a source nobody decided about, surviving in silence. Over-removal is the
# safe direction here only because `VerifyArchiveSurvives` below turns it into a refusal.
#
# ## What this does NOT do
#
# It does not make `apt-get update` tolerant. A failure of the UBUNTU ARCHIVE is still
# fatal, because that one means the install that follows cannot work, and a job that
# continued would fail later and less clearly. Sources are removed BEFORE the update rather
# than having their errors ignored afterwards -- ignoring apt errors is how a real archive
# failure becomes a silent one.
#
# It does not touch `/etc/apt/sources.list`. That file is the distribution's own; vendor
# packages drop files in `sources.list.d/`, which is the whole population this sweeps.
#
# And it does not trust its own sweep, because the failure it replaces was silent in
# exactly the way a `-e` test on a guessed filename is silent -- the step reports success
# whether or not anything happened. Two guards run afterwards and REFUSE rather than tally:
#
#   * `VerifySwept` -- nothing outside `AllowedHosts` may survive. A removal that did not
#     happen (no permission, a read-only tree, a format this parser cannot read) looks
#     exactly like a removal that was never needed, and that is the bug this file is about.
#   * `VerifyArchiveSurvives` -- an Ubuntu archive source must still be there. If the
#     allowlist is wrong about this image's layout, `apt-get update` would otherwise
#     succeed against a partial index and the install would fail later, somewhere else.
#
# ## Why a script rather than a workflow step
#
# Eleven `apt-get update` call sites in `build.yml`, and `scripts/check-apt-update.sh`
# refuses a workflow that calls `apt-get update` directly -- which is what stops the
# twelfth call site from missing this.
#
# bash 3.2: macOS ships a 2007 /bin/bash and the self-test runs in the default ctest set.
#
# Usage:
#   bash scripts/ci-apt-update.sh
#   bash scripts/ci-apt-update.sh --self-test
#   bash scripts/ci-apt-update.sh --apt-root <dir> --sweep-only   # against a tree you own
set -euo pipefail

# Hosts this repository is willing to install from. An entry here is a claim that a job
# here asks this host for packages; anything not named is removed from the runner.
#
# Derived from a real run rather than guessed -- every apt source contacted across run
# 34368983218 (master, 071f7079, 2026-09-09), counted off the `Get:`/`Hit:` lines:
#
#     229  azure.archive.ubuntu.com     the Ubuntu archive
#     118  apt.llvm.org                 clang/clang-tidy at the pinned version
#      25  dl.google.com                nobody asked for this -- #1160
#       4  packages.microsoft.com       nobody asked for this -- #550
#
# `apt.llvm.org` is on the list because five jobs install clang from it through
# `llvm.sh`. Today every `ci-apt-update.sh` call happens to run BEFORE its job's
# `llvm.sh`, so the source is not yet configured when the sweep runs and omitting it
# would change nothing -- which is exactly why it must be named anyway. Relying on that
# ordering is a bet on the workflow's layout, and a second wrapper call added after an
# LLVM install would silently sweep the source the next step needs. Its outage CAN now
# fail a job, and that is correct: we genuinely cannot build without clang, which is the
# whole distinction this file draws.
#
# The four Ubuntu rows are one archive under the spelling a given image picks: Azure-
# hosted runners answer everything from `azure.archive.ubuntu.com`, a plain image splits
# security out, and arm64 is served from `ports`. Only the first is contacted today.
AllowedHosts="
archive.ubuntu.com
azure.archive.ubuntu.com
security.ubuntu.com
esm.ubuntu.com
ports.ubuntu.com
apt.llvm.org
"

AptRoot=/etc/apt
Sudo=sudo
SelfTest=0
SweepOnly=0

while [ $# -gt 0 ]; do
    case "${1:-}" in
        --self-test)
            SelfTest=1; shift ;;
        --sweep-only)
            SweepOnly=1; shift ;;
        --apt-root)
            # Relocating the tree is how the self-test drives the REAL logic instead of a
            # fixture reimplementing it. A tree you name is a tree you own, so no sudo.
            AptRoot="${2:-}"; Sudo=""; shift 2 ;;
        *)
            printf 'ci-apt-update: unknown argument: %s\n' "${1:-}" >&2; exit 2 ;;
    esac
done

SourcesDir="${AptRoot}/sources.list.d"
MainSourcesList="${AptRoot}/sources.list"

Refuse() {
    printf 'ci-apt-update: REFUSED: %s\n' "$1" >&2
    exit 1
}

# Every host a source file names, one per line. Handles the one-line `deb` format and
# deb822 `URIs:` alike, because 24.04 ships the Ubuntu archive as the latter.
# Prints nothing for a file that names none -- which the caller treats as a REASON to
# remove, never as "no objection".
# $1: file to read
HostsIn() {
    local urls
    # No `producer | grep -q` and no bare pipeline verdict: under `pipefail` a `grep`
    # finding nothing fails the pipeline, and this function's empty answer is meaningful.
    # Full-line comments are stripped first. A COMMENT is not a call site, and apt does
    # not read one either -- a vendor file mentioning the Ubuntu archive in a comment must
    # not read as ours, which is the direction that would keep it.
    urls=$(sed -e 's/^[[:space:]]*#.*$//' "$1" 2>/dev/null \
        | grep -oE 'https?://[^[:space:]"]+' || true)
    [ -n "$urls" ] || return 0
    printf '%s\n' "$urls" \
        | sed -e 's#^[a-z][a-z]*://##' -e 's#^[^/@]*@##' -e 's#/.*$##' -e 's#:[0-9][0-9]*$##' \
        | sort -u
}

# $1: host. Exit 0 when it is a host this repository installs from.
IsAllowed() {
    local candidate
    for candidate in $AllowedHosts; do
        if [ "$1" = "$candidate" ]; then
            return 0
        fi
    done
    return 1
}

# Exit 0 when the file names only allowed hosts AND names at least one.
# $1: file to read
IsOurs() {
    local host found
    found=0
    for host in $(HostsIn "$1"); do
        found=1
        IsAllowed "$host" || return 1
    done
    [ "$found" -eq 1 ]
}

SweptCount=0
KeptCount=0
# Decided-to-remove-and-still-here is its own state, not a missing removal and not a keep.
# Folding it into either would undercount the total in exactly the case that matters, and
# a tally that cannot count the failure is the shape of the bug this file is about.
StuckCount=0

SweepSources() {
    local file
    SweptCount=0
    KeptCount=0
    StuckCount=0
    [ -d "$SourcesDir" ] || return 0

    for file in "$SourcesDir"/*.list "$SourcesDir"/*.sources; do
        [ -e "$file" ] || continue
        if IsOurs "$file"; then
            KeptCount=$((KeptCount + 1))
            continue
        fi
        # The removal's exit status is deliberately NOT the verdict. `VerifySwept` re-reads
        # the tree afterwards, which is strictly stronger: it also catches an `rm` that
        # reported success over a file that is still there.
        $Sudo rm -f "$file" 2>/dev/null || true
        if [ -e "$file" ]; then
            StuckCount=$((StuckCount + 1))
            continue
        fi
        printf 'ci-apt-update: removed %s (not a host this repository installs from)\n' "$file"
        SweptCount=$((SweptCount + 1))
    done
}

# GUARD ONE. Nothing this repository does not install from may survive the sweep.
VerifySwept() {
    local file hosts
    [ -d "$SourcesDir" ] || return 0
    for file in "$SourcesDir"/*.list "$SourcesDir"/*.sources; do
        [ -e "$file" ] || continue
        IsOurs "$file" && continue
        hosts=$(HostsIn "$file" | tr '\n' ' ')
        [ -n "$hosts" ] || hosts="(none this parser can read)"
        Refuse "${file} survived the sweep, naming: ${hosts}
       The sweep decided to remove it and it is still here, so removing it by hand is not
       the fix -- find out why. A source left standing is an outage this repository eats
       for a vendor it never asked for, which is the whole of #1160."
    done
}

# GUARD TWO. The Ubuntu archive must have survived, or the allowlist is wrong about this
# image and `apt-get update` below would succeed against nothing.
VerifyArchiveSurvives() {
    local file host
    for file in "$SourcesDir"/*.list "$SourcesDir"/*.sources "$MainSourcesList"; do
        [ -e "$file" ] || continue
        for host in $(HostsIn "$file"); do
            case "$host" in
                *.ubuntu.com|ubuntu.com) return 0 ;;
            esac
        done
    done
    Refuse "no Ubuntu archive source survives in ${SourcesDir} or ${MainSourcesList}.
       Either the sweep removed something it should not have, or this image does not lay
       its sources out where this script looks. Either way \`apt-get update\` would now
       succeed against a partial index and the install would fail later and less clearly,
       which is the failure this script exists to prevent."
}

# ---------------------------------------------------------------------------
# Self-test.
#
# Every case drives THIS script over a tree it owns, through `--apt-root`. A fixture that
# reimplemented the sweep would be a second thing to be wrong rather than a test of this
# one. Each case states which of the two guards, or which behaviour, it establishes.
# ---------------------------------------------------------------------------

SelfTestCases=0
SelfTestTmp=""

# $1: directory to build an apt root in. Plants a healthy Ubuntu source in deb822 form.
StageRoot() {
    mkdir -p "$1/sources.list.d"
    printf 'Types: deb\nURIs: http://azure.archive.ubuntu.com/ubuntu/\nSuites: noble\nComponents: main\n' \
        > "$1/sources.list.d/ubuntu.sources"
}

Ok()  { SelfTestCases=$((SelfTestCases + 1)); printf 'ok   %s\n' "$1"; }
Bad() { printf 'FAIL %s\n' "$1"; [ -z "${2:-}" ] || printf '%s\n' "$2" | sed 's/^/     /'; exit 1; }

RunSelfTest() {
    local tmp out rc arrangeable
    tmp=$(mktemp -d) || { printf 'ci-apt-update: mktemp failed\n' >&2; exit 1; }
    SelfTestTmp="$tmp"
    trap 'chmod u+w "$SelfTestTmp"/d/sources.list.d 2>/dev/null || true; rm -rf "$SelfTestTmp"' EXIT

    # Case 1 -- the vendor that caused #1160. The whole point of the ticket.
    StageRoot "$tmp/a"
    printf 'deb [arch=amd64] https://dl.google.com/linux/chrome/deb/ stable main\n' \
        > "$tmp/a/sources.list.d/google-chrome.list"
    out=$(bash "$0" --apt-root "$tmp/a" --sweep-only 2>&1) && rc=0 || rc=$?
    if [ "$rc" -eq 0 ] && [ ! -e "$tmp/a/sources.list.d/google-chrome.list" ] \
        && [ -e "$tmp/a/sources.list.d/ubuntu.sources" ]; then
        Ok "case 1: the Chrome source is removed and the Ubuntu source is kept"
    else
        Bad "case 1: rc=$rc" "$out"
    fi

    # Case 2 -- the vendor the two named rows already handled, kept so the inversion is
    # shown not to have lost the case it replaced.
    StageRoot "$tmp/b"
    printf 'deb [arch=amd64] https://packages.microsoft.com/ubuntu/24.04/prod noble main\n' \
        > "$tmp/b/sources.list.d/microsoft-prod.list"
    out=$(bash "$0" --apt-root "$tmp/b" --sweep-only 2>&1) && rc=0 || rc=$?
    if [ "$rc" -eq 0 ] && [ ! -e "$tmp/b/sources.list.d/microsoft-prod.list" ]; then
        Ok "case 2: the source the named-file version handled is still handled"
    else
        Bad "case 2: rc=$rc" "$out"
    fi

    # Case 3 -- a vendor on nobody's list. This is the case a named table cannot have, and
    # the only reason to prefer an allowlist at all.
    StageRoot "$tmp/c"
    printf 'deb https://apt.example-vendor.invalid/repo noble main\n' \
        > "$tmp/c/sources.list.d/some-future-vendor.list"
    out=$(bash "$0" --apt-root "$tmp/c" --sweep-only 2>&1) && rc=0 || rc=$?
    if [ "$rc" -eq 0 ] && [ ! -e "$tmp/c/sources.list.d/some-future-vendor.list" ]; then
        Ok "case 3: a vendor named on no list is removed, which a named table cannot do"
    else
        Bad "case 3: rc=$rc" "$out"
    fi

    # Case 4 -- GUARD ONE, disallowed host. The requirement this ticket turns on: something
    # must FAIL when the source is present and not removed. Arranged with a read-only
    # parent, and the arrangement is MEASURED rather than assumed -- as root, or on a
    # filesystem that ignores modes, `chmod a-w` does not bite and the case would pass for
    # the wrong reason, which is the defect this whole file is about.
    StageRoot "$tmp/d"
    printf 'deb https://dl.google.com/linux/chrome/deb/ stable main\n' \
        > "$tmp/d/sources.list.d/google-chrome.list"
    chmod a-w "$tmp/d/sources.list.d"
    if touch "$tmp/d/sources.list.d/.arrangement-canary" 2>/dev/null; then
        arrangeable=0
        rm -f "$tmp/d/sources.list.d/.arrangement-canary" 2>/dev/null || true
    else
        arrangeable=1
    fi
    if [ "$arrangeable" -eq 1 ]; then
        out=$(bash "$0" --apt-root "$tmp/d" --sweep-only 2>&1) && rc=0 || rc=$?
        chmod u+w "$tmp/d/sources.list.d"
        case "$out" in
            *"survived the sweep, naming: dl.google.com"*) : ;;
            *) Bad "case 4: guard one did not name the survivor (rc=$rc)" "$out" ;;
        esac
        [ "$rc" -ne 0 ] || Bad "case 4: guard one printed its complaint and exited 0" "$out"
        Ok "case 4: a source the sweep could not remove is a REFUSAL, not a tally"
    else
        chmod u+w "$tmp/d/sources.list.d" 2>/dev/null || true
        printf 'SKIP case 4: a read-only directory does not bite here (root, or a filesystem\n'
        printf '     that ignores modes), so guard one could not be arranged. This run does NOT\n'
        printf '     establish the property #1160 is about. Re-run as a non-root user on a\n'
        printf '     filesystem that honours permissions.\n'
        printf '\nself-test: %d case(s) ran, 1 could not be arranged\n' "$SelfTestCases"
        return 77
    fi

    # Case 5 -- GUARD ONE, unreadable survivor. A directory named like a source file cannot
    # be removed by `rm -f` for ANY user, so this arm always bites. It also pins the
    # decision that a file naming no readable host is removed rather than silently kept.
    StageRoot "$tmp/e"
    mkdir -p "$tmp/e/sources.list.d/opaque.list"
    out=$(bash "$0" --apt-root "$tmp/e" --sweep-only 2>&1) && rc=0 || rc=$?
    case "$out" in
        *"survived the sweep, naming: (none this parser can read)"*) : ;;
        *) Bad "case 5: an unreadable survivor was not refused (rc=$rc)" "$out" ;;
    esac
    [ "$rc" -ne 0 ] || Bad "case 5: exited 0 while complaining" "$out"
    Ok "case 5: a source this parser cannot read is removed, and refused if it survives"

    # Case 6 -- GUARD TWO. An allowlist that is wrong about the image's layout must refuse
    # rather than leave apt updating against nothing.
    mkdir -p "$tmp/f/sources.list.d"
    printf 'deb https://dl.google.com/linux/chrome/deb/ stable main\n' \
        > "$tmp/f/sources.list.d/google-chrome.list"
    out=$(bash "$0" --apt-root "$tmp/f" --sweep-only 2>&1) && rc=0 || rc=$?
    case "$out" in
        *"no Ubuntu archive source survives"*) : ;;
        *) Bad "case 6: guard two did not fire (rc=$rc)" "$out" ;;
    esac
    [ "$rc" -ne 0 ] || Bad "case 6: exited 0 while complaining" "$out"
    Ok "case 6: a sweep that leaves no Ubuntu archive is a REFUSAL"

    # Case 7 -- the passing direction. A guard nobody has watched ACCEPT is not known to
    # work (#1031), and this one refuses on two independent conditions.
    StageRoot "$tmp/g"
    printf 'deb http://security.ubuntu.com/ubuntu noble-security main\n' \
        > "$tmp/g/sources.list.d/security.list"
    out=$(bash "$0" --apt-root "$tmp/g" --sweep-only 2>&1) && rc=0 || rc=$?
    if [ "$rc" -eq 0 ] && [ -e "$tmp/g/sources.list.d/ubuntu.sources" ] \
        && [ -e "$tmp/g/sources.list.d/security.list" ]; then
        Ok "case 7: an already-clean tree is accepted and nothing is removed"
    else
        Bad "case 7: a clean tree was refused or lost a source (rc=$rc)" "$out"
    fi

    # Case 8 -- the main sources.list is the distribution's and is never swept, and it can
    # satisfy guard two on its own. That is the layout of the runner image this runs on.
    mkdir -p "$tmp/h/sources.list.d"
    printf 'deb http://azure.archive.ubuntu.com/ubuntu noble main\n' > "$tmp/h/sources.list"
    printf 'deb https://dl.google.com/linux/chrome/deb/ stable main\n' \
        > "$tmp/h/sources.list.d/google-chrome.list"
    out=$(bash "$0" --apt-root "$tmp/h" --sweep-only 2>&1) && rc=0 || rc=$?
    if [ "$rc" -eq 0 ] && [ -e "$tmp/h/sources.list" ] \
        && [ ! -e "$tmp/h/sources.list.d/google-chrome.list" ]; then
        Ok "case 8: sources.list is untouched and satisfies guard two by itself"
    else
        Bad "case 8: rc=$rc" "$out"
    fi

    # Case 9 -- the LLVM source SURVIVES. Five jobs install clang from apt.llvm.org, and
    # the only thing standing between this sweep and removing it today is that every
    # wrapper call happens to run before its job's `llvm.sh`. This pins the allowlist row
    # rather than the ordering, so moving a call site cannot quietly break the clang
    # install.
    StageRoot "$tmp/i"
    printf 'deb https://apt.llvm.org/noble/ llvm-toolchain-noble-22 main\n' \
        > "$tmp/i/sources.list.d/llvm.list"
    out=$(bash "$0" --apt-root "$tmp/i" --sweep-only 2>&1) && rc=0 || rc=$?
    if [ "$rc" -eq 0 ] && [ -e "$tmp/i/sources.list.d/llvm.list" ]; then
        Ok "case 9: the LLVM source a clang job needs is kept, not swept"
    else
        Bad "case 9: rc=$rc" "$out"
    fi

    printf '\nself-test: %d case(s) ran, all passed\n' "$SelfTestCases"
    return 0
}

if [ "$SelfTest" -eq 1 ]; then
    RunSelfTest
    exit $?
fi

SweepSources
# What was FOUND and what was REMOVED, with no denominator. `removed 1 of 2 known` was a
# tally against the size of a hand-kept table, which is why it read as thorough while the
# vendor that failed the job was not in the table at all. There is no fixed M here: the
# population is whatever the image configured, and that is the number worth printing.
printf 'ci-apt-update: %d source(s) found, %d removed, %d kept, %d could not be removed\n' \
    "$((SweptCount + KeptCount + StuckCount))" "$SweptCount" "$KeptCount" "$StuckCount"
VerifySwept
VerifyArchiveSurvives

[ "$SweepOnly" -eq 0 ] || exit 0

# The Ubuntu archive is still allowed to fail the job, loudly.
$Sudo apt-get update
