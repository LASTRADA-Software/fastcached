#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# On every driver this machine has that the launcher MERGES, `-###`'s first line
# must be byte-identical to `--version`'s.
#
# ## What this is for, and why it is not a test of the change that motivated it
#
# #1237 takes the compiler banner and the target triple from ONE `-###` spawn
# instead of two, for `TargetDiscovery::ClangDriverLine` drivers. The banner is the
# compiler's IDENTITY -- it is folded into the launcher's cache key and into the
# toolchain fingerprint a worker advertises. So if `-###`'s first line is ever NOT
# what `--version` printed, every client silently stops matching every worker, with
# no counter moving. That is #226's failure mode.
#
# **This check is what makes a fingerprint bump unnecessary.** The alternative was
# to bump unconditionally, which invalidates every stored object in order to record
# a change that -- if the two lines really are identical -- did not happen. The
# evidence for that identity is measured and narrow (see the table below), which is
# not enough to bet a fleet on by itself. So the property is CHECKED rather than
# assumed or paid for: the day a clang build breaks it, it breaks on the platform
# that breaks it, loudly, instead of shipping a wrong banner.
#
# It is therefore not a regression test for #1237's diff. It is a standing assertion
# about the world that #1237 relies on, and it must keep running long after that
# change is old.
#
# **If this check ever fires, do not adjust it.** A driver whose two spellings
# disagree is the finding that argues for the fingerprint bump #1237 declined, and
# it needs a person: see that ticket's "bump, or checked property?" section, which
# states both positions with the evidence.
#
# ## The evidence this was written against
#
# Measured 2026-09-11, seven drivers over two hosts, first line of each spelling
# compared byte for byte. Pinned here rather than pointed at: these are the state of
# two machines at one instant and must not track anything (`AGENT.md` on a
# measurement's conditions).
#
#   host                        driver         first lines
#   --------------------------- -------------- ----------------------------------
#   Windows 11, VS 18 LLVM      clang          identical  (clang 22.1.3)
#   Windows 11, VS 18 LLVM      clang++        identical  (clang 22.1.3)
#   Windows 11, VS 18 LLVM      clang-cl       identical  (clang 22.1.3)
#   WSL2 Ubuntu 24.04           clang          identical  (clang 20.1.2)
#   WSL2 Ubuntu 24.04           clang++        identical  (clang 20.1.2)
#   WSL2 Ubuntu 24.04           clang-22       identical  (clang 22.1.8)
#   WSL2 Ubuntu 24.04           clang++-22     identical  (clang 22.1.8)
#
# And the DISCRIMINATING row, which is why GCC is out of scope rather than
# overlooked -- same hosts, same day:
#
#   WSL2 Ubuntu 24.04           gcc, g++       DIFFER: `--version` gives
#                                              "gcc (Ubuntu 14.2.0-...) 14.2.0",
#                                              `-###` gives "Using built-in specs."
#
# ## The subject set is production's own merge condition, not a name heuristic
#
# A driver is a subject exactly when the launcher would merge for it, which is two
# clauses and both are checked here the way `ProbeDriverIdentity` checks them:
#
#   1. its NAME classifies to `TargetDiscovery::ClangDriverLine`. That is
#      `ClassifyCompilerImpl`'s rule, reproduced in `DriverNameScope` below: the
#      first matching stem of the table wins, and the remainder must be empty or
#      begin with `-`; and
#   2. the `-###` spawn EXITS 0 with non-empty stderr. That is the literal condition
#      in `ProbeDriverIdentity`, and a driver failing it takes the `--version`
#      fallback in production, so the property does not apply to it.
#
# Clause 2 is not a convenience. Clause 1 alone classifies **`clang-format` and
# `clang-tidy` as Clang** -- `-format` and `-tidy` begin with `-`, so they satisfy
# the version-suffix rule exactly as `-22` does -- and asking those two for a `-###`
# banner would report a difference on a healthy machine. Measured, same hosts:
# `clang-format`, `clang-format-22`, `clang-tidy` and `clang-tidy-22` all exit **1**
# on `-###` ("Unknown command line argument"), while all seven real drivers above
# exit **0**. So clause 2 separates them, and it separates them for the same reason
# production does rather than for a reason invented here.
#
# ## The candidate set is narrower than clause 1, deliberately, and here is the cost
#
# Clause 1 is production's rule and `DriverNameScope` reproduces it exactly. It is
# not, on its own, a usable way to pick what to SPAWN. Measured on this WSL host,
# which carries two full LLVM installs and a Windows toolchain on its PATH: globbing
# every executable clause 1 accepts gave **152 candidates, 132 of them out of
# scope** -- `clang-apply-replacements-22`, `clang-extdef-mapping-20`,
# `clang-include-cleaner-22` and 129 more, none of which a build system has ever
# handed to a compiler launcher. Two spawns each, 13 s for the run.
#
# So `IsProbeCandidate` narrows it, and the narrowing is this file's own decision
# rather than a claim about production: an exact stem, optionally followed by `-`
# and a DIGIT, which is what a distribution's version suffix looks like. That is an
# inclusion rule stating what we ask for, not a denylist betting on what LLVM ships.
#
# What it does not reach, said plainly rather than left to be discovered: a driver
# whose suffix is not a version -- `clang-cpp` is the real example, a genuine merged
# -path driver on both hosts here, and any vendor-renamed driver would be another.
# `clang-cpp` is measured identical on both hosts and is not a compiler any build
# names, and it ships from the same install as the `clang` that IS checked, so the
# install is covered even though that name is not. A vendor driver under a name this
# rule cannot guess is genuinely unchecked, and the printed count is what was
# actually checked rather than a claim about the machine.
#
# Clause 2 still runs on everything that gets through, so the narrowing is a cost
# ceiling and not the safety rule.
#
# A `.exe` candidate is a subject only on a Windows shell. WSL's PATH carries
# `/mnt/c/...`, so a POSIX run finds the Windows toolchain, spawns it, and gets
# `-###-exited-1` -- because a Windows child cannot open the `/dev/null` a POSIX
# host chose. That is an artefact of the mixed PATH, and reporting it as "production
# falls back to --version here" would be a false statement about a driver that is
# healthy on its own host.
#
# `cc` and `c++` are NOT subjects, and that is the carve-out worth stating because
# it looks like a gap. On macOS they are Apple clang, so the intuition is that they
# belong here. They do not: `ClassifyCompilerImpl` matches them to `Flavor::Gcc` BY
# NAME, `DriverOf(Gcc).targetDiscovery` is `GnuTargetLine`, and `ProbeDriverIdentity`
# dispatches on the NAME classification -- it has no banner yet, so it cannot know
# better. `ClassifyCompilerFromBanner` corrects `Gcc` to `Clang` afterwards, for the
# key, but by then the identity probe has already taken the `--version` path. So a
# macOS `cc` never reaches the merged spawn and this check must not claim it did.
#
# ## Why a shell check rather than `cmake -P`
#
# Chosen rather than defaulted to, because the VERDICT CHANNEL differs per runner
# and getting it wrong is silent. A `cmake -P` check is judged by its OUTPUT and its
# registration must carry `FAIL_REGULAR_EXPRESSION`; a script behind `run-check.sh`
# is judged by its EXIT STATUS and must not carry one. Two reasons for this side:
#
#   * this check SPAWNS COMPILERS and compares captured bytes, which is a shell's
#     job rather than CMake's; and
#   * it has to be able to say SKIP. A machine with no such driver cannot answer the
#     question, and that is a third outcome -- a `-P` script cannot choose its exit
#     code before 3.29, while a script can exit 77 and the registration says so.
#
# ## Skipped is not passed, and nor is a control that never fired
#
# A host with no merged-path driver reports SKIPPED, by name, saying what it looked
# for. A check that silently passed there would report "the property holds" on
# precisely the machines where it was never tested.
#
# And the acquisition is CONTROLLED before any real driver is judged. The pure
# verdict is driven by `--self-test`, but nothing there exercises the two spawns,
# the stdout/stderr split or the first-line extraction -- so a stand-in driver whose
# two spellings are known to differ, and a second whose two spellings are known to
# agree, go through the SAME capture function first. A control through a different
# path certifies nothing. The first version of this file needed exactly that: it
# word-split driver paths on spaces, spawned twelve nonexistent programs on a box
# with three compilers, captured the two IDENTICAL "No such file" messages, and
# reported `12 driver(s) checked, 0 differing`. Every arm agreed perfectly, which is
# what a broken instrument looks like as well as what a real pattern looks like.
#
# Usage:
#   scripts/check-banner-probe-identity.sh [--self-test]

set -uo pipefail

# Git Bash rewrites an argument that looks like a POSIX path, so `/TP` and `/c`
# reach clang-cl as `C:/Program Files/Git/TP`. Both spellings, always
# (`.agent/rules/build-and-toolchain.md`). It is also why the null input below is
# chosen from the HOST rather than converted: with conversion off, `/dev/null`
# would reach a Windows child as the literal string.
export MSYS2_ARG_CONV_EXCL='*'
export MSYS_NO_PATHCONV=1

readonly SKIP=77

# How long one probe spawn may take. A compiler asked `--version` answers in
# milliseconds; this bounds a WEDGED driver so the check reports rather than
# consuming the suite's timeout and reporting nothing.
readonly ProbeSeconds=60

. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/e2e-common.sh"

workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT
e2e_begin "banner probe identity" "$workdir"

# The empty input a probe compiles, chosen the way `NullInputPath()` chooses it --
# by the HOST, not by testing whether `/dev/null` exists, which is true under Git
# Bash while the Windows child that receives it cannot open it.
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) WindowsShell=1; NullInput="NUL" ;;
    *)                    WindowsShell=0; NullInput="/dev/null" ;;
esac

# ---------------------------------------------------------------------------
# The DECISIONS, split out as pure functions.
#
# This is what `--self-test` drives. Acquisition -- walking PATH, spawning drivers,
# capturing output -- needs a machine with compilers on it and cannot be staged, so
# a check whose verdict logic were reachable only that way would be tested only
# where it was already going to run. `tidy-sweep.sh`'s `CanaryVerdict` is the same
# split for the same reason (#257).

# `NormalizedCompilerName`, in shell: the basename, ASCII-lowercased, with a `.exe`
# suffix removed.
#
# @param 1 A path or a bare name.
# @return The normalized basename.
NormalizedDriverName() {
    local base="${1##*/}"
    base="${base##*\\}"
    base="$(printf '%s' "$base" | tr 'ABCDEFGHIJKLMNOPQRSTUVWXYZ' 'abcdefghijklmnopqrstuvwxyz')"
    case "$base" in
        *.exe) base="${base%.exe}" ;;
    esac
    printf '%s' "$base"
}

# `ClassifyCompilerImpl`'s walk, answering which target-discovery row a NAME lands
# on -- clause 1 of the subject rule.
#
# The stem list and its ORDER are `NamePatterns`', because the first match wins and
# `clang` is a prefix of both `clang-cl` and `clang++`. A reordering here would
# classify `clang-cl` as plain clang and hand it the GNU probe flags.
#
# @param 1 A path or a bare name.
# @return `merged` (ClangDriverLine), `gnu` (GnuTargetLine), `none`, or `unnamed`
#         for a name the table does not match at all.
DriverNameScope() {
    local base pattern rest
    base="$(NormalizedDriverName "$1")"
    for pattern in clang-cl clang++ clang g++ gcc c++ cc cl; do
        case "$base" in
            "$pattern") rest="" ;;
            "$pattern"-*) rest="-" ;;
            *) continue ;;
        esac
        # Anything after the stem must begin with `-`, so `clanger` does not read as
        # clang and `clangd` does not either. `rest` above is already reduced to the
        # only fact that decides it.
        [ -z "$rest" ] || [ "$rest" = "-" ] || continue
        case "$pattern" in
            clang-cl|clang++|clang) printf 'merged' ;;
            g++|gcc|c++|cc)         printf 'gnu' ;;
            cl)                     printf 'none' ;;
        esac
        return
    done
    printf 'unnamed'
}

# Whether this NAME is one we will spawn at all.
#
# THIS FILE'S OWN narrowing, not a model of anything in production -- which is why
# it is a separate function from `DriverNameScope` rather than a tightening of it. A
# reader comparing the two against `ClassifyCompilerImpl` must be able to see which
# one is the faithful copy.
#
# An exact stem, or a stem followed by `-` and a DIGIT. See the header for what it
# costs: 152 candidates on this WSL host become 20, and `clang-cpp` is the real
# thing given up.
#
# @param 1 A normalized driver name.
# @return 0 when it is worth spawning.
IsProbeCandidate() {
    local base="$1" pattern
    for pattern in clang-cl clang++ clang g++ gcc c++ cc cl; do
        case "$base" in
            "$pattern") return 0 ;;
            "$pattern"-[0-9]*) return 0 ;;
        esac
    done
    return 1
}

# The verdict for one driver, over what its two spellings produced.
#
# Takes the exit statuses as well as the lines, because a spawn that did not RUN is
# a different fact from one that ran and printed nothing, and both are different
# from a difference. Collapsing them is what made the first version of this file
# report `ok` for twelve programs that do not exist.
#
# @param 1 `--version`'s exit status.
# @param 2 `--version`'s first line.
# @param 3 `-###`'s exit status.
# @param 4 `-###`'s first line.
# @return One of `ok`, `differ`, `unmerged <why>`, `refused-version <rc>`.
BannerPairVerdict() {
    local versionRc="$1" versionLine="$2" hashesRc="$3" hashesLine="$4"

    # `--version` failing is about the DRIVER, not about the property, and it is the
    # one arm that means this candidate is not a compiler we can say anything about.
    if [ "$versionRc" -ne 0 ]; then
        printf 'refused-version %s' "$versionRc"
        return
    fi
    if [ -z "$versionLine" ]; then
        printf 'refused-version empty'
        return
    fi

    # Clause 2 of the subject rule, and the literal condition in
    # `ProbeDriverIdentity`: a non-zero `-###` or an empty stderr makes production
    # fall back to `--version`, so no merge happens and the property does not apply.
    # NOT a failure -- reported, and excluded from the checked count.
    if [ "$hashesRc" -ne 0 ]; then
        printf 'unmerged -###-exited-%s' "$hashesRc"
        return
    fi
    if [ -z "$hashesLine" ]; then
        printf 'unmerged -###-said-nothing'
        return
    fi

    # Byte comparison, deliberately without trimming. The banner is hashed into a
    # cache key, so a trailing space is a real difference and a comparison that
    # normalised it would pass a pair the key would not.
    if [ "$versionLine" = "$hashesLine" ]; then
        printf 'ok'
    else
        printf 'differ'
    fi
}

# What the run as a whole reports, from its two counts.
#
# A pure function because the SKIP arm is the one nobody exercises: a developer box
# and every CI leg here have clang, so the arm that fires on a machine without one
# would never be observed at all. Skipped, absent and passed are three states and
# this is where they are kept apart.
#
# @param 1 How many subjects were checked.
# @param 2 How many of them differed.
# @return `skip`, `pass`, or `fail`.
RunVerdict() {
    if [ "$1" -eq 0 ]; then
        printf 'skip'
    elif [ "$2" -gt 0 ]; then
        printf 'fail'
    else
        printf 'pass'
    fi
}

# ---------------------------------------------------------------------------
# Acquisition.

# Capture one probe's first line and exit status.
#
# Sets `ProbeRc` and `ProbeLine`. Two modes, because the two production probes read
# two different streams and a check that read the merged stream for both would pass
# under a driver that printed its banner on stdout, which `ProbeDriverIdentity`
# would never see:
#
#   combined  `RunCaptureCombined(...).out`  -- what `--version` is read from
#   stderr    `RunCaptureSplit(...).err`     -- what `-###` is read from
#
# `run_bounded` merges the child's streams into one capture, so the `stderr` mode
# drops stdout in an inner shell before it gets there. Bounded through the shared
# helper rather than `timeout(1)`, which macOS does not have.
#
# @param 1 `combined` or `stderr`.
# @param 2.. The command and its arguments.
ProbeFirstLine() {
    local mode="$1"; shift
    local out rc=0

    if [ "$mode" = "stderr" ]; then
        out="$(run_bounded "$ProbeSeconds" bash -c 'exec "$0" "$@" 2>&1 >/dev/null' "$@")" || rc=$?
    else
        out="$(run_bounded "$ProbeSeconds" "$@")" || rc=$?
    fi

    ProbeRc="$rc"
    ProbeLine="${out%%$'\n'*}"
    # A Windows child writes CRLF and the production reader strips exactly one.
    ProbeLine="${ProbeLine%$'\r'}"
}

# Ask one driver both ways.
#
# Sets `PairVerdict`, `PairVersionLine` and `PairHashesLine`. The clang-cl arm takes
# the MSVC probe flags and everything else the GNU ones, keyed on the driver NAME
# exactly as `DriverSpec` keys them -- never by sniffing a leading `/`, which on
# POSIX starts a path.
#
# **The verdict is a VARIABLE rather than something printed for `$( )` to capture,
# and that is the whole reason this function looks the way it does.** Called in a
# command substitution it runs in a SUBSHELL, so the two evidence lines it sets are
# discarded at the closing paren -- and the only branch that reads them is the
# FAILING one. Written that way, the check exited on `PairVersionLine: unbound
# variable` the first time a planted driver made it refuse: non-zero by accident,
# with neither line printed and the remedy text never reached, which reads as the
# instrument being broken rather than as the finding it is. Found by planting the
# defect, not by review.
#
# @param 1 The driver's path.
# @param 2 Its normalized name.
ProbeOneDriver() {
    local cc="$1" base="$2" versionRc versionLine

    ProbeFirstLine combined "$cc" --version
    versionRc="$ProbeRc"
    versionLine="$ProbeLine"

    case "$base" in
        clang-cl*) ProbeFirstLine stderr "$cc" -### /TP /c "$NullInput" ;;
        *)         ProbeFirstLine stderr "$cc" -### -x c++ -c "$NullInput" ;;
    esac

    PairVersionLine="$versionLine"
    PairHashesLine="$ProbeLine"
    PairVerdict="$(BannerPairVerdict "$versionRc" "$versionLine" "$ProbeRc" "$ProbeLine")"
}

# Every directory on PATH, one per line.
#
# Split on `:`, which is what PATH holds under every shell this runs in -- Git Bash
# and MSYS convert the Windows `;` form before bash ever sees it.
PathDirectories() {
    local rest="$PATH" dir
    while [ -n "$rest" ]; do
        case "$rest" in
            *:*) dir="${rest%%:*}"; rest="${rest#*:}" ;;
            *)   dir="$rest"; rest="" ;;
        esac
        [ -n "$dir" ] || continue
        printf '%s\n' "$dir"
    done
}

# Every driver on PATH worth spawning, one `<scope> <name> <path>` per line, in
# PATH order with duplicates dropped.
#
# Newline-delimited rather than space-joined, because a driver path on Windows
# contains spaces (`C:\Program Files\...`) and the space-joined version of this
# function is the defect described in the header. The NAME travels in the row so
# the caller does not normalize a second time.
#
# **Duplicates are dropped by (name, REAL directory), never by the path string.**
# `/bin` is a symlink to `/usr/bin` on this WSL host and both are on PATH, so a
# string key reported 16 subjects where there are 8, spawning each twice and
# printing a count nobody could reconcile with the machine. The directory is
# resolved ONCE per PATH entry rather than per candidate. The NAME stays part of
# the key on purpose: `clang` and `clang++` are frequently the same inode, and both
# are real subjects, because the launcher is handed a name and classifies on it.
CandidateDrivers() {
    local dir realDir candidate base scope seen=""

    while IFS= read -r dir; do
        realDir="$( cd "$dir" 2>/dev/null && pwd -P )" || realDir="$dir"
        [ -n "$realDir" ] || realDir="$dir"
        # Only the prefixes that can satisfy clause 1 at all, so a large PATH is not
        # walked file by file. `cc*`/`c++*` are deliberately absent: they are `gnu`
        # scope, never subjects, and their observation value is nil since they are
        # symlinks to something the other globs already find.
        for candidate in "$dir"/clang* "$dir"/gcc* "$dir"/g++*; do
            # An unmatched glob is left literal by bash, so the existence test is
            # what removes it rather than `nullglob`, which is not bash 3.2 safe to
            # turn on around someone else's code.
            [ -f "$candidate" ] || continue
            [ -x "$candidate" ] || continue
            # A Windows executable is this host's driver only on a Windows shell.
            # See the header: WSL's PATH reaches `/mnt/c`, and spawning those from a
            # POSIX host produces a false out-of-scope line about a healthy driver.
            case "$candidate" in
                *.exe|*.EXE) [ "$WindowsShell" -eq 1 ] || continue ;;
            esac
            base="$(NormalizedDriverName "$candidate")"
            IsProbeCandidate "$base" || continue
            scope="$(DriverNameScope "$base")"
            [ "$scope" = "merged" ] || [ "$scope" = "gnu" ] || continue
            case "$seen" in
                *"|${realDir}/${base}|"*) continue ;;
            esac
            seen="${seen}|${realDir}/${base}|"
            printf '%s %s %s\n' "$scope" "$base" "$candidate"
        done
    done < <( PathDirectories )
}

# Prove the capture path can see a difference, and can see an agreement, before any
# real driver is judged.
#
# Two arms and both are needed: an instrument that reported `differ` for everything
# would pass the first alone, and one that reported `ok` for everything -- which is
# what the space-splitting version did -- would pass the second alone. They fail in
# opposite directions.
#
# The stand-ins go through `ProbeFirstLine` in the same two modes the real drivers
# use, including the stdout-dropping inner shell, because that is the part
# `--self-test` cannot reach.
#
# @return 0 when both arms behaved; non-zero otherwise, having said which.
RunAcquisitionControl() {
    local standin verdict bad=0

    standin="${workdir}/standin-differs"
    cat > "$standin" <<'STANDIN'
#!/bin/bash
if [ "${1:-}" = "--version" ]; then
    echo "STAND-IN version line A"
else
    echo "STAND-IN driver line B" >&2
fi
STANDIN

    ProbeFirstLine combined bash "$standin" --version
    local vrc="$ProbeRc" vline="$ProbeLine"
    ProbeFirstLine stderr bash "$standin" -###
    verdict="$(BannerPairVerdict "$vrc" "$vline" "$ProbeRc" "$ProbeLine")"
    if [ "$verdict" != "differ" ]; then
        echo "CONTROL FAILED: a stand-in whose two spellings differ was read as [$verdict]" >&2
        echo "                --version[1]: [$vline]" >&2
        echo "                -###[1]     : [$ProbeLine]" >&2
        bad=1
    fi

    standin="${workdir}/standin-agrees"
    cat > "$standin" <<'STANDIN'
#!/bin/bash
if [ "${1:-}" = "--version" ]; then
    echo "STAND-IN shared line"
else
    echo "STAND-IN shared line" >&2
fi
STANDIN

    ProbeFirstLine combined bash "$standin" --version
    vrc="$ProbeRc"; vline="$ProbeLine"
    ProbeFirstLine stderr bash "$standin" -###
    verdict="$(BannerPairVerdict "$vrc" "$vline" "$ProbeRc" "$ProbeLine")"
    if [ "$verdict" != "ok" ]; then
        echo "CONTROL FAILED: a stand-in whose two spellings agree was read as [$verdict]" >&2
        echo "                --version[1]: [$vline]" >&2
        echo "                -###[1]     : [$ProbeLine]" >&2
        bad=1
    fi

    return "$bad"
}

# ---------------------------------------------------------------------------
if [ "${1:-}" = "--self-test" ]; then
    ran=0
    bad=0
    expect() {
        ran=$(( ran + 1 ))
        if [ "$2" != "$3" ]; then
            echo "SELF-TEST FAILED: $1: expected [$2], got [$3]" >&2
            bad=$(( bad + 1 ))
        fi
    }

    echo "== BannerPairVerdict"
    expect "identical lines pass" \
        "ok" "$(BannerPairVerdict 0 "Ubuntu clang version 22.1.8" 0 "Ubuntu clang version 22.1.8")"
    # The arm the whole check exists for. A version bump between the two spellings is
    # the realistic shape, not a wholesale difference.
    expect "a differing version is refused" \
        "differ" "$(BannerPairVerdict 0 "Ubuntu clang version 22.1.8" 0 "Ubuntu clang version 22.1.9")"
    # What a GNU driver produces, measured on g++ 14.2.0. It is out of SCOPE by name
    # so this pair never reaches the comparison in a real run -- pinned anyway,
    # because it is the shape the comparison must be able to see.
    expect "a GNU-shaped -### line is a difference" \
        "differ" "$(BannerPairVerdict 0 "g++ (Ubuntu 14.2.0-4ubuntu2~24.04.1) 14.2.0" 0 "Using built-in specs.")"
    # Trailing whitespace is a real difference in a value that is hashed.
    expect "a trailing space is a difference, because the key hashes bytes" \
        "differ" "$(BannerPairVerdict 0 "clang version 22.1.8" 0 "clang version 22.1.8 ")"
    # `clang-format`, measured: clause 1 calls it Clang and clause 2 excludes it.
    # NOT a failure -- production would fall back to `--version` for it too.
    expect "a -### the driver refused is unmerged, not a difference" \
        "unmerged -###-exited-1" "$(BannerPairVerdict 0 "Ubuntu clang-format version 22.1.8" 1 "clang-format: Unknown command line argument '-###'.")"
    expect "a -### that printed nothing is unmerged" \
        "unmerged -###-said-nothing" "$(BannerPairVerdict 0 "Ubuntu clang version 22.1.8" 0 "")"
    # A candidate that would not run at all. This is the arm whose absence let the
    # first version of this file report `ok` for twelve programs that do not exist:
    # both captures held the same "No such file" text, so a comparison of LINES
    # ALONE said they agreed.
    expect "a --version that failed is refused, never compared" \
        "refused-version 127" "$(BannerPairVerdict 127 "bash: no such file" 127 "bash: no such file")"
    expect "a --version that printed nothing is refused" \
        "refused-version empty" "$(BannerPairVerdict 0 "" 0 "clang version 22.1.8")"

    echo "== DriverNameScope"
    expect "clang is a subject"        "merged" "$(DriverNameScope /usr/bin/clang)"
    expect "clang++ is a subject"      "merged" "$(DriverNameScope /usr/bin/clang++)"
    expect "clang-cl is a subject"     "merged" "$(DriverNameScope clang-cl)"
    # The stem table's ORDER: `clang` is a prefix of both, so a reordering would hand
    # clang-cl the GNU probe flags while still answering `merged`.
    expect "clang-cl.exe normalizes"   "merged" "$(DriverNameScope 'C:\LLVM\bin\Clang-CL.EXE')"
    expect "a version suffix is a subject" "merged" "$(DriverNameScope /usr/bin/clang++-22)"
    # Clause 1 really does accept these. The check does not fail on them because
    # clause 2 excludes them; asserting that here is what stops somebody "fixing"
    # `DriverNameScope` to reject them and diverging from production.
    expect "clang-format satisfies clause 1, as production's rule does" \
        "merged" "$(DriverNameScope /usr/bin/clang-format)"
    expect "clangd does not, having no dash"   "unnamed" "$(DriverNameScope /usr/bin/clangd)"
    expect "clanger does not"                  "unnamed" "$(DriverNameScope clanger)"
    # The carve-out. `cc` on macOS IS clang, and it is still not a subject, because
    # `ProbeDriverIdentity` dispatches on the NAME classification and has no banner
    # to correct it with yet.
    expect "cc is gnu by name, whatever it is really" "gnu" "$(DriverNameScope /usr/bin/cc)"
    expect "c++ is gnu by name"                       "gnu" "$(DriverNameScope /usr/bin/c++)"
    expect "gcc is gnu"                               "gnu" "$(DriverNameScope /usr/bin/gcc)"
    expect "g++-14 is gnu"                            "gnu" "$(DriverNameScope /usr/bin/g++-14)"
    expect "cl discovers no target at all"            "none" "$(DriverNameScope 'C:\VC\bin\cl.exe')"
    expect "an unrelated name matches nothing"        "unnamed" "$(DriverNameScope /usr/bin/ld)"

    echo "== IsProbeCandidate"
    # A helper, because this one answers with a STATUS: `expect` compares strings,
    # and a status silently stringified is how a both-directions test becomes a
    # one-direction test.
    candidate() { if IsProbeCandidate "$1"; then printf 'yes'; else printf 'no'; fi; }

    expect "a bare stem is spawned"          "yes" "$(candidate clang)"
    expect "clang++ is spawned"              "yes" "$(candidate clang++)"
    expect "clang-cl is spawned"             "yes" "$(candidate clang-cl)"
    expect "a version suffix is spawned"     "yes" "$(candidate clang-22)"
    expect "clang++-20 is spawned"           "yes" "$(candidate clang++-20)"
    expect "clang-cl-22 is spawned"          "yes" "$(candidate clang-cl-22)"
    expect "a dotted version is spawned"     "yes" "$(candidate clang-19.1)"
    expect "g++-14 is spawned"               "yes" "$(candidate g++-14)"
    # The narrowing's whole purpose. These satisfy production's clause 1 and are
    # NOT spawned, and the header says what that costs.
    expect "clang-format is not spawned"     "no"  "$(candidate clang-format)"
    expect "clang-tidy is not spawned"       "no"  "$(candidate clang-tidy)"
    expect "clang-format-22 is not spawned"  "no"  "$(candidate clang-format-22)"
    expect "clang-scan-deps is not spawned"  "no"  "$(candidate clang-scan-deps)"
    # Named because it is the real driver the narrowing gives up, so a reader who
    # widens the rule later can see it was a decision.
    expect "clang-cpp is the cost, and is not spawned" "no" "$(candidate clang-cpp)"
    expect "clangd is not spawned"           "no"  "$(candidate clangd)"

    echo "== RunVerdict"
    expect "no subject is a skip, never a pass" "skip" "$(RunVerdict 0 0)"
    expect "subjects, none differing, passes"   "pass" "$(RunVerdict 3 0)"
    expect "one differing fails"                "fail" "$(RunVerdict 3 1)"

    echo "== the acquisition control"
    ran=$(( ran + 1 ))
    if ! RunAcquisitionControl; then
        echo "SELF-TEST FAILED: the acquisition control did not behave" >&2
        bad=$(( bad + 1 ))
    fi

    # Printed on BOTH paths: a run that died half way through -- an unbound variable,
    # a stray exit -- must not read as one where every case passed.
    echo "banner-probe-identity self-test: ${ran} case(s) ran, ${bad} failed"
    [ "$bad" -eq 0 ] || exit 1
    exit 0
fi

# ---------------------------------------------------------------------------
# The run.

if ! RunAcquisitionControl; then
    fail "the acquisition control did not behave, so no verdict about this machine's drivers can be read"
fi
echo "control: the capture path can see a difference and can see an agreement"

checked=0
differing=0
excluded=0
controls=0
excludedNames=""

while IFS= read -r row; do
    [ -n "$row" ] || continue
    scope="${row%% *}"
    row="${row#* }"
    base="${row%% *}"
    cc="${row#* }"

    if [ "$scope" = "gnu" ]; then
        # Not a subject, and spawned anyway: this is the in-experiment control on
        # the check's own SCOPE. A GNU driver's two spellings really do differ
        # (measured, in the header), so where the host has one this run carries the
        # evidence that the scope filter is load-bearing rather than decorative.
        #
        # NOT a refusal in either direction. On macOS `/usr/bin/gcc` is Apple clang
        # and its two spellings AGREE, so a check that demanded a difference here
        # would redden a healthy macOS leg -- and one that demanded agreement would
        # redden every Linux leg. What it can honestly do is report.
        ProbeOneDriver "$cc" "$base"
        controls=$(( controls + 1 ))
        echo "scope control: $base is GnuTargetLine and is never merged -- its two spellings say [$PairVerdict]"
        continue
    fi

    ProbeOneDriver "$cc" "$base"
    case "$PairVerdict" in
        ok)
            checked=$(( checked + 1 ))
            echo "ok:   $cc"
            ;;
        differ)
            checked=$(( checked + 1 ))
            differing=$(( differing + 1 ))
            echo "FAIL: $cc -- the two spellings differ" >&2
            echo "        --version[1]: [$PairVersionLine]" >&2
            echo "        -###[1]     : [$PairHashesLine]" >&2
            ;;
        *)
            # `unmerged` and `refused-version`. Production would not merge for this
            # one, so the property does not apply and it is not counted as checked.
            #
            # Collected rather than printed per line. These are NAMED in the summary
            # below and not merely counted -- a bare count would leave a reader
            # unable to tell an excluded auxiliary tool from an excluded compiler,
            # which is the one thing an exclusion list has to be readable about.
            excluded=$(( excluded + 1 ))
            excludedNames="${excludedNames}${base} (${PairVerdict}), "
            ;;
    esac
done < <( CandidateDrivers )

# The COUNTS are printed on the passing path too. "No failures" is not "it ran": a
# candidate list that silently came back empty would otherwise render identically to
# a clean run, and the skip below is the only place emptiness is supposed to be
# possible.
if [ "$excluded" -gt 0 ]; then
    echo "out of scope: ${excludedNames%, }"
fi
echo "banner-probe-identity: ${checked} merged-path driver(s) checked, ${differing} differing, ${excluded} out of scope, ${controls} scope control(s)"

case "$(RunVerdict "$checked" "$differing")" in
    skip)
        echo "SKIP: this machine has no driver the launcher would merge for."
        echo "      Looked on PATH for names classifying to TargetDiscovery::ClangDriverLine"
        echo "      (clang, clang++, clang-cl, and their -<suffix> forms) that also exit 0"
        echo "      on the -### probe. The property is UNCHECKED here, not confirmed."
        exit "$SKIP"
        ;;
    fail)
        echo "" >&2
        echo "  A driver's -### first line is not its --version first line." >&2
        echo "" >&2
        echo "  DO NOT adjust this check to accommodate it. That disagreement is the" >&2
        echo "  finding: it means the launcher's merged probe puts a different string" >&2
        echo "  in the cache key and the toolchain fingerprint than the one stored" >&2
        echo "  objects and running workers were built with -- silently, with no" >&2
        echo "  counter moving." >&2
        echo "" >&2
        echo "  It is the evidence that argues for the fingerprint bump #1237" >&2
        echo "  declined. Read that ticket's 'bump, or checked property?' section," >&2
        echo "  which states both positions, and take it to whoever owns the fleet." >&2
        fail "${differing} of ${checked} merged-path driver(s) disagree between --version and -###"
        ;;
esac

exit 0
