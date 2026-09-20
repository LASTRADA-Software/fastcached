#!/usr/bin/env bash
#
# Install the apt.llvm.org clang toolchain, and say WHICH KIND of failure it was (#1566).
#
# THE DEFECT. Both `Install Clang` steps in `build.yml` were four bare commands:
#
#     wget https://apt.llvm.org/llvm.sh
#     chmod +x llvm.sh
#     sudo ./llvm.sh $V
#     sudo apt-get install -y clang-$V ...
#
# with no handling of any kind. Master run 35172062484 died 36 s in on `Temporary
# failure in name resolution` and was reported as **"clang-tsan failed"** -- which sends
# whoever reads it to the sanitizer and then to the source tree, for a DNS outage on a
# GitHub runner. The tree was never the subject and nothing in the report said so.
#
# EXPLICITLY NOT A RETRY. A blind retry makes an instrument's own failures disappear
# without recording them: the run goes green on the second attempt, nobody learns the
# acquisition is flaky, and the day it is genuinely broken it looks like a new defect.
# What was missing is not resilience, it is a DIAGNOSIS.
#
# TWO OUTCOMES, and they are different diagnoses with different owners:
#
#   ACQUISITION (exit 75)  the network could not be reached, or apt.llvm.org did not
#                          answer. Nobody's branch did this. `EX_TEMPFAIL`, and the
#                          annotation says `ACQUISITION FAILURE` so a report built from
#                          this run's output names the cause rather than the leg.
#   ABSENT (exit 1)        the network answered and the package is not there -- a
#                          version bumped past what apt.llvm.org carries, a renamed
#                          package. That is a real finding about this tree and stays
#                          LOUD, with no special status and no softening.
#
# A third state exists and is not folded into either: `UNCLASSIFIED`, when the command
# failed in a way neither pattern explains. It exits 1 -- loud, like ABSENT, because an
# unrecognised failure must never be quieter than a known one -- but it SAYS it is
# unclassified, so nobody reads a confident wrong cause off it. "We could not tell" and
# "nothing was wrong" are different answers, and so are "we could not tell" and "the
# package is missing".
#
# The CLASSIFIER is a pure function of the command's output and status, which is what
# `--self-test` drives. The acquisition around it is not testable here and is not the
# part that was wrong.
set -o errexit
set -o nounset
set -o pipefail

EX_ACQUISITION=75

Version=""
Mode="install"
# Repeatable. `coverage` and `clang-tsan` both need `llvm-<major>` beside the
# compiler, and that install is subject to exactly the same two failures -- so it goes
# through the same classifier rather than being left as a bare `apt-get` after the
# script has returned.
ExtraPackages=""
while [ "$#" -gt 0 ]; do
    case "$1" in
        --version)   Version="$2"; shift 2 ;;
        --package)   ExtraPackages="${ExtraPackages} $2"; shift 2 ;;
        --self-test) Mode="selftest"; shift ;;
        *) echo "usage: $0 --version <major> [--package <name>]... | --self-test" >&2; exit 2 ;;
    esac
done

# --- the classifier ---------------------------------------------------------
#
# Rows are `<class>|<pattern>`, matched case-insensitively against the failing
# command's combined output. ACQUISITION rows are listed first and win, because a
# transport failure can drag an apt-shaped message along behind it -- "Unable to locate
# package" is exactly what apt says when its own index could not be fetched, so reading
# that first would classify a network outage as a missing package and send somebody to
# bump a version that is fine.
#
# TOTAL: 13 rows -- 8 acquisition, 5 absent. Asserted against the table in the
# self-test rather than restated as a number nobody re-derives.
FailureClasses=(
    "acquisition|temporary failure in name resolution"
    "acquisition|could not resolve host"
    "acquisition|name or service not known"
    "acquisition|connection timed out"
    "acquisition|connection refused"
    "acquisition|network is unreachable"
    "acquisition|failed to fetch"
    "acquisition|unable to connect to"
    "absent|unable to locate package"
    "absent|has no installation candidate"
    "absent|error 404"
    "absent|no such file or directory"
    "absent|couldn't find any package"
)

# Classify a failure.
# @param 1 The failing command's combined output.
# @return Prints `acquisition`, `absent` or `unclassified`.
ClassifyFailure() {
    local text row class pattern lowered
    lowered="$(printf '%s' "$1" | tr '[:upper:]' '[:lower:]')"
    for row in "${FailureClasses[@]}"; do
        class="${row%%|*}"
        pattern="${row#*|}"
        case "$lowered" in
            *"$pattern"*) echo "$class"; return 0 ;;
        esac
    done
    echo "unclassified"
}

# Report a failure and exit with the status its class earns.
# @param 1 What was being attempted.
# @param 2 The combined output.
Refuse() {
    local what="$1" text="$2" class
    class="$(ClassifyFailure "$text")"
    echo "--- output of the failing step ---" >&2
    printf '%s\n' "$text" >&2
    echo "----------------------------------" >&2
    case "$class" in
        acquisition)
            echo "::error::ACQUISITION FAILURE: ${what} could not reach apt.llvm.org. This is not a defect in this branch -- the toolchain could not be downloaded. Re-run the job; if it persists, apt.llvm.org or the runner's network is down."
            echo "ci-install-clang: ACQUISITION -- exiting ${EX_ACQUISITION}" >&2
            exit "$EX_ACQUISITION"
            ;;
        absent)
            echo "::error::PACKAGE ABSENT: ${what} reached the network and the package is not there. Check CLANG_TOOLS_VERSION against what apt.llvm.org actually carries -- this IS a fact about this tree."
            echo "ci-install-clang: ABSENT -- exiting 1" >&2
            exit 1
            ;;
        *)
            echo "::error::UNCLASSIFIED FAILURE: ${what} failed in a way ci-install-clang does not recognise. It is NOT being reported as a network problem and NOT as a missing package, because it matched neither -- read the output above. If this shape recurs, it wants a row in FailureClasses."
            echo "ci-install-clang: UNCLASSIFIED -- exiting 1" >&2
            exit 1
            ;;
    esac
}

Install() {
    local v="$1" out rc
    [ -n "$v" ] || { echo "ci-install-clang: --version is required" >&2; exit 2; }

    echo "ci-install-clang: fetching the apt.llvm.org installer"
    # `--tries=1`: this is a diagnosis, not a retry. wget's own retrying would turn a
    # clean DNS failure into a slow one and change nothing about the report.
    out="$(wget --tries=1 --timeout=30 -O llvm.sh https://apt.llvm.org/llvm.sh 2>&1)" && rc=0 || rc=$?
    [ "$rc" -eq 0 ] || Refuse "fetching llvm.sh" "$out"

    # A fetch can succeed and still have written something useless -- a captive-portal
    # page, an error document served 200. An empty or non-script file is ABSENT rather
    # than acquisition: the transport worked.
    if [ ! -s llvm.sh ]; then
        Refuse "fetching llvm.sh" "ERROR 404: the download produced an empty file"
    fi

    chmod +x llvm.sh

    echo "ci-install-clang: running the installer for ${v}"
    out="$(sudo ./llvm.sh "$v" 2>&1)" && rc=0 || rc=$?
    [ "$rc" -eq 0 ] || Refuse "running llvm.sh ${v}" "$out"

    # `$ExtraPackages` UNQUOTED and deliberately: it is a space-separated list of
    # package names, and word splitting is how it becomes several arguments. Empty
    # when nothing was asked for, which expands to nothing at all.
    #
    # It rides the SAME `apt-get` as the compiler rather than a second call after it,
    # which is what `--package`'s whole reason is: `llvm-<major>` is subject to the
    # identical two failures, and a bare `apt-get` outside `Refuse`'s reach is one
    # call site back to guessing.
    echo "ci-install-clang: installing clang-${v}, the libc++ headers${ExtraPackages:+ and${ExtraPackages}}"
    # shellcheck disable=SC2086
    out="$(sudo apt-get install -y \
              "clang-${v}" "libc++-${v}-dev" "libc++abi-${v}-dev" ${ExtraPackages} 2>&1)" && rc=0 || rc=$?
    [ "$rc" -eq 0 ] || Refuse "installing clang-${v}${ExtraPackages:+ and${ExtraPackages}}" "$out"

    sudo ln -sf "/usr/bin/clang-${v}" /usr/local/bin/clang
    sudo ln -sf "/usr/bin/clang++-${v}" /usr/local/bin/clang++
    echo "ci-install-clang: clang-${v} installed"
}

SelfTest() {
    local ran=0 failed=0 got
    Expect() {
        local what="$1" want="$2" text="$3"
        ran=$(( ran + 1 ))
        got="$(ClassifyFailure "$text")"
        if [ "$got" = "$want" ]; then
            echo "ok: ${what} -> ${got}"
        else
            echo "CI INSTALL CLANG SELF-TEST FAILED: ${what}: expected '${want}', got '${got}'" >&2
            failed=$(( failed + 1 ))
        fi
    }

    # The run that opened the ticket, verbatim in shape.
    Expect "the DNS failure from master run 35172062484" acquisition \
        "Resolving apt.llvm.org (apt.llvm.org)... failed: Temporary failure in name resolution.
wget: unable to resolve host address 'apt.llvm.org'"
    Expect "a refused connection" acquisition "Connecting to apt.llvm.org|151.101.0.0|:443... failed: Connection refused."
    Expect "an apt transport failure" acquisition \
        "E: Failed to fetch http://apt.llvm.org/noble/dists/llvm-toolchain/InRelease  Could not resolve host"

    # The other diagnosis: the network worked.
    Expect "a version apt.llvm.org does not carry" absent \
        "E: Unable to locate package clang-99"
    Expect "a package with no candidate" absent \
        "Package libc++-99-dev is not available, but is referred to by another package.
E: Package 'libc++-99-dev' has no installation candidate"
    Expect "a 404 on the installer" absent "ERROR 404: Not Found."

    # PRECEDENCE, and this is the case the row order exists for: apt says "Unable to
    # locate package" when its own index could not be fetched, so an output carrying
    # BOTH must classify as acquisition. Read the other way round, a network outage is
    # reported as a version bump somebody then makes.
    Expect "a network outage that also produced an apt-shaped message" acquisition \
        "E: Failed to fetch http://apt.llvm.org/... Temporary failure in name resolution
E: Unable to locate package clang-22"

    # The third outcome. It must NOT be quietly folded into either neighbour.
    Expect "a failure matching neither pattern" unclassified \
        "sudo: a terminal is required to read the password"
    Expect "an empty output" unclassified ""

    # Case-insensitivity is a property of the classifier, not of the rows.
    Expect "an upper-case message" acquisition "TEMPORARY FAILURE IN NAME RESOLUTION"

    ran=$(( ran + 1 ))
    if [ "${#FailureClasses[@]}" -eq 13 ]; then
        echo "ok: the table has its stated 13 rows"
    else
        echo "CI INSTALL CLANG SELF-TEST FAILED: the table has ${#FailureClasses[@]} rows; the comment above says 13" >&2
        failed=$(( failed + 1 ))
    fi

    # And that acquisition rows really do come first, which is what makes the
    # precedence case above hold for a reason rather than by luck.
    ran=$(( ran + 1 ))
    local firstAbsent=0 i=0 lastAcq=0
    for i in $(seq 0 $(( ${#FailureClasses[@]} - 1 ))); do
        case "${FailureClasses[$i]%%|*}" in
            acquisition) lastAcq=$i ;;
            absent) [ "$firstAbsent" -eq 0 ] && firstAbsent=$i ;;
        esac
    done
    if [ "$lastAcq" -lt "$firstAbsent" ]; then
        echo "ok: every acquisition row precedes every absent row"
    else
        echo "CI INSTALL CLANG SELF-TEST FAILED: an absent row precedes an acquisition row; the precedence case above would then pass by luck" >&2
        failed=$(( failed + 1 ))
    fi

    # THE WIRING, not the decision. `--package` parsed into `ExtraPackages` and
    # `Install` never reading it is a flag that is accepted, echoed in no message and
    # silently dropped -- and the two call sites that pass it (`clang-tsan` and
    # `Coverage`) then fail LATER and elsewhere, on a `test -x` and on a configure
    # refusal about a profile tool. Nothing the classifier can see, because the
    # classifier is only ever handed the output of a command that RAN. `PurgeExpired`
    # is this repository's standing example: correct, tested, and constructed by
    # nobody. So the assertion is that the acquisition reaches the value.
    # Asserted on the `apt-get install` ARGUMENTS and not on the function body: the
    # first spelling of this case grepped the whole of `Install()`, and `ExtraPackages`
    # is named in its echo and in its refusal text as well -- so deleting it from the
    # command line left the case GREEN over exactly the defect it was written for.
    # Watched failing with the expansion removed before being believed.
    local self="${BASH_SOURCE[0]}"
    ran=$(( ran + 1 ))
    local aptLine
    aptLine="$(awk '
        /^Install\(\) \{/ { inFn = 1; next }
        inFn && /^\}/     { exit }
        inFn && /apt-get install/ { grab = 1 }
        grab { print; if ($0 !~ /\\$/) grab = 0 }
    ' "$self")"
    if [ -z "$aptLine" ]; then
        echo "CI INSTALL CLANG SELF-TEST FAILED: no \`apt-get install\` was found inside Install() in ${self}; the wiring assertion could not run, which is not the same as finding nothing" >&2
        failed=$(( failed + 1 ))
    elif grep -q 'ExtraPackages' <<< "$aptLine"; then
        echo "ok: Install()'s apt-get install expands ExtraPackages, so --package reaches apt"
    else
        echo "CI INSTALL CLANG SELF-TEST FAILED: --package is parsed into ExtraPackages and Install()'s apt-get install does not expand it, so every --package argument is silently dropped" >&2
        failed=$(( failed + 1 ))
    fi

    # And that no call site has gone back to a bare `wget`. One is the defect, two is
    # the pattern -- and the handed-over count for this ticket was TWO while the tree
    # carried FIVE, `clang-tsan` among them: the very job whose misreported DNS failure
    # opened it. A diagnosis written at some of the identical call sites reports
    # correctly at some of them, which is indistinguishable from working until the
    # outage lands on one of the others.
    local workflow="${Root:-}/.github/workflows/build.yml"
    [ -f "$workflow" ] || workflow="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/.github/workflows/build.yml"
    ran=$(( ran + 1 ))
    if [ ! -f "$workflow" ]; then
        # A check that cannot find its subject has not passed. Said out loud rather
        # than folded into the tally as a quiet success.
        echo "CI INSTALL CLANG SELF-TEST FAILED: build.yml not found at ${workflow}; the call-site scan could not run, which is not the same as finding nothing" >&2
        failed=$(( failed + 1 ))
    else
        local raw converted
        raw="$(grep -c 'apt\.llvm\.org/llvm\.sh' "$workflow" || true)"
        converted="$(grep -c 'ci-install-clang\.sh' "$workflow" || true)"
        if [ "${raw:-0}" -ne 0 ]; then
            echo "CI INSTALL CLANG SELF-TEST FAILED: ${raw} call site(s) in build.yml still fetch apt.llvm.org/llvm.sh directly. Route them through this script, or a network outage there is reported as whatever the job happens to be called." >&2
            failed=$(( failed + 1 ))
        elif [ "${converted:-0}" -eq 0 ]; then
            # The positive control. Zero raw sites and zero converted ones is what a
            # scan looking at the wrong file also reports, and it reads as clean.
            echo "CI INSTALL CLANG SELF-TEST FAILED: build.yml names this script nowhere AND fetches llvm.sh nowhere. That is what reading the wrong file looks like; it is not a pass." >&2
            failed=$(( failed + 1 ))
        else
            echo "ok: all ${converted} clang-install call site(s) in build.yml route through this script, 0 raw"
        fi
    fi

    echo "ci-install-clang --self-test: ${ran} case(s) ran, ${failed} failed"
    [ "$failed" -eq 0 ]
}

if [ "$Mode" = "selftest" ]; then
    SelfTest
    exit $?
fi

Install "$Version"
