#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# How long does one ctest test take on one CI job, across N runs?
#
#   scripts/probes/ci-test-duration-census.sh                       # the defaults below
#   scripts/probes/ci-test-duration-census.sh --pages 6 --out c.tsv
#   scripts/probes/ci-test-duration-census.sh --test cluster-e2e --job Linux-gcc-release
#
# ## Why a probe rather than a number in a comment
#
# `.agent/rules/` asks that a figure others will refer to live in ONE place they
# point at. A measurement's CONDITIONS are the opposite -- they are the state of
# the world at one instant and must not track their source -- so what travels is
# this script: a citation people can re-run is one they stop re-litigating.
#
# ## The sampling trap this exists to avoid (#1206, #1157)
#
# Every RATE figure the toolchain-walk tickets descend from came from the node's
# own progress lines, and those reach the job output ONLY when the fixture fails
# and dumps the node log. Measured here over 106 runs: **1** carried progress
# lines, and it was the single failing run. Sampling the rate is sampling on the
# dependent variable, and "the walk is slow" was guaranteed by where the data
# lives rather than discovered.
#
# The DURATION is the unbiased instrument: ctest prints it for every run, pass or
# fail. So this samples every completed job and records the outcome BESIDE the
# number rather than filtering on it.
#
# ## What it records, and why each column is there
#
#   duration   the quantity
#   verdict    Passed / ***Failed / ... -- so a reader can see the sample is not
#              conditioned on it
#   ratelines  how many progress lines the log carried, which is the bias check
#   runner     the runner INSTANCE. On GitHub-hosted runners every job gets a
#              fresh VM, so two rows never share a machine -- which is how this
#              answers "within one machine or between machines" structurally
#              rather than by correlation
#   region     the Azure region, from the runner preamble; a between-machine
#              dimension that is free to collect
#   image      the runner image version, the other one
#
# ## What it measured on 2026-09-11 (#1206)
#
# Pinned rather than pointed at. A measurement's CONDITIONS are the state of the
# world at one instant and must not track their source: delete these numbers and
# point at a live query, and the day the fleet changes the sentence still claims
# it was measured under today's conditions.
#
# `node-scratch-isolation-e2e` on `Windows-clangcl-release`, every completed
# `Build` run in a 25.7-hour window (2026-09-09T23:11Z - 2026-09-11T00:50Z),
# 109 jobs, 106 durations. The three without a duration carried no ctest output at
# all -- their logs stop after the runner preamble -- and are counted here rather
# than dropped:
#
#   min 58.97s   p25 81.38s   median 98.68s   p75 185.05s   p90 341.67s
#   max 608.26s  ->  a 10.3x spread, where the three points in #1206 gave 4.8x
#
# **The spread is BETWEEN machines, by construction.** 109 distinct runner
# instances across 109 rows: on GitHub-hosted runners every job gets a fresh VM,
# so no two samples here share a machine and this census cannot say anything about
# variance WITHIN one. Measuring that needs the walk run twice inside ONE job,
# which is a workflow change and not a census.
#
# **The slow tail belongs to one runner image**, which ranks the candidate list
# #1206 deliberately left unranked:
#
#   image 20260824.214.3   n=47   min 63.54s   median  84.73s   max 167.78s
#   image 20260907.229.1   n=59   min 58.97s   median 137.22s   max 608.26s
#
# Every one of the 27 samples at or above 180 s is on `20260907.229.1`; none is on
# `20260824.214.3`. The two images are INTERLEAVED across the whole window -- both
# appear in every six-hour bucket from the first hour -- so this is not the older
# image being the older era. The sharpest pair is 20 seconds apart: 84.91 s on
# `20260824.214.3` at 01:23:53Z against 541.71 s on `20260907.229.1` at 01:24:13Z.
#
# No mechanism is claimed. The image version may itself be a proxy for a hardware
# generation, and a bigger SDK in the newer image would raise the FLOOR rather
# than grow a tail -- its minimum is the lower of the two. What the census does is
# hand the next investigation a dimension that separates the samples, where three
# points could not.
#
# The bias check came out as #1157 predicted, on a larger sample: **1 of 109 logs
# carried progress lines, and it is the single failing run** (58 lines, 608.26 s).
#
# ## REST only
#
# `gh api repos/...` and `.../actions/...` are REST. The GraphQL budget is per
# USER and every lane on a machine shares it, so a probe built on `gh run list`
# stops working exactly when several people are working.
set -uo pipefail

Repo="${FASTCACHED_REPO:-LASTRADA-Software/fastcached}"
TestName="node-scratch-isolation-e2e"
JobName="Windows-clangcl-release"
Pages=3
Out=""

Usage() {
    echo "usage: $(basename "$0") [--test NAME] [--job NAME] [--pages N] [--out FILE]" >&2
    exit 2
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --test)  TestName="${2:-}"; shift 2 ;;
        --job)   JobName="${2:-}"; shift 2 ;;
        --pages) Pages="${2:-}"; shift 2 ;;
        --out)   Out="${2:-}"; shift 2 ;;
        -h|--help) Usage ;;
        *) echo "unknown option: $1" >&2; Usage ;;
    esac
done
[ -n "$TestName" ] && [ -n "$JobName" ] || Usage
case "$Pages" in ''|*[!0-9]*) echo "--pages takes a number" >&2; exit 2 ;; esac

command -v gh >/dev/null 2>&1 || { echo "gh is not on PATH, so nothing can be asked" >&2; exit 77; }

if [ -z "$Out" ]; then
    Out="$(mktemp)" || { echo "cannot create a scratch file" >&2; exit 2; }
fi
: > "$Out"

echo "census of '${TestName}' on '${JobName}', ${Pages} page(s) of runs, repo ${Repo}" >&2

runs=""
p=1
while [ "$p" -le "$Pages" ]; do
    page="$(gh api "repos/${Repo}/actions/runs?per_page=100&page=${p}&status=completed" \
        --jq '.workflow_runs[] | select(.name=="Build") | "\(.id)|\(.head_branch)"' 2>/dev/null)"
    if [ -n "$page" ]; then
        runs="${runs}${page}"$'\n'
    fi
    p=$(( p + 1 ))
done

# A census returning zero gets a positive control: state what was searched and
# whether the search could have found it, before concluding "nothing there".
candidates="$(grep -c . <<< "$runs" || true)"
if [ "$candidates" -eq 0 ]; then
    echo "REFUSING: no completed 'Build' runs came back at all, so this is a statement about the QUERY and not about the fleet" >&2
    exit 2
fi
echo "candidate Build runs: ${candidates}" >&2

sampled=0
noline=0
while IFS='|' read -r rid branch; do
    [ -n "$rid" ] || continue
    job="$(gh api "repos/${Repo}/actions/runs/${rid}/jobs?per_page=100" \
        --jq ".jobs[] | select(.name==\"${JobName}\") | \"\(.id)|\(.runner_name)|\(.conclusion)|\(.started_at)\"" 2>/dev/null)"
    [ -n "$job" ] || continue
    IFS='|' read -r jid runner concl jstart <<< "$job"
    [ "$concl" = "success" ] || [ "$concl" = "failure" ] || continue

    log="$(gh api "repos/${Repo}/actions/jobs/${jid}/logs" --allow-escape-sequences 2>/dev/null)"
    if [ -z "$log" ]; then
        echo "${rid}|${branch}|${jid}|${runner}|${concl}|LOGFAIL|||${jstart}|?|?" >> "$Out"
        continue
    fi
    # A HERESTRING, never `producer | grep`: under `set -o pipefail` a consumer
    # that leaves early kills the producer and the pipeline reports its status.
    line="$(grep -a "${TestName} \." <<< "$log" | grep -a -v "Start " | tail -1)"
    dur="$(sed -n 's/.*  *\([0-9][0-9]*\.[0-9][0-9]*\) sec.*/\1/p' <<< "$line" | tail -1)"
    verdict="$(sed -n 's/.*\(Passed\|\*\*\*Failed\|\*\*\*Timeout\|\*\*\*Exception\|Skipped\|\*\*\*Not Run\).*/\1/p' <<< "$line" | tail -1)"
    rate="$(grep -a -c "hashing toolchain files:" <<< "$log" || true)"
    # Captured then trimmed, never `| head`: a pipe into an early-exiting consumer
    # kills the producer with SIGPIPE and `pipefail` then reports the producer's
    # status. `check-e2e-helpers.sh` scans every script for that shape and caught
    # the first version of this line, which is the scan doing its job on its author.
    regions="$(sed -n 's/.*Azure Region: \(.*\)/\1/p' <<< "$log" | tr -d '\r')"
    region="${regions%%$'\n'*}"
    image="$(grep -a -m2 -oE "Version: [0-9][0-9.]*" <<< "$log" | tail -1 | sed 's/Version: //')"

    if [ -z "$verdict" ]; then
        noline=$(( noline + 1 ))
        verdict="NOLINE"
    fi
    echo "${rid}|${branch}|${jid}|${runner}|${concl}|${verdict}|${dur}|${rate}|${jstart}|${region:-?}|${image:-?}" >> "$Out"
    sampled=$(( sampled + 1 ))
done <<< "$runs"

echo >&2
echo "sampled ${sampled} job(s); ${noline} carried no '${TestName}' line at all and are counted, not dropped" >&2
echo "rows: ${Out}" >&2
echo >&2

# The summary. Every figure states its N, because a spread without one invites an
# inference it cannot support.
awk -F'|' -v test="$TestName" -v job="$JobName" '
    $7 != "" { d[n++] = $7 + 0 }
    { runners[$4] = 1; rows++ }
    $8 > 0   { withrate++ }
    END {
        if (n == 0) { print "no durations parsed -- the extraction found no test line in any log"; exit 1 }
        for (i = 0; i < n; i++)
            for (j = i + 1; j < n; j++)
                if (d[j] < d[i]) { t = d[i]; d[i] = d[j]; d[j] = t }
        printf "%s on %s\n", test, job
        printf "  N=%d durations from %d job(s)\n", n, rows
        printf "  min %.2fs   p25 %.2fs   median %.2fs   p75 %.2fs   p90 %.2fs   max %.2fs\n",
               d[0], d[int(n * 0.25)], d[int(n * 0.5)], d[int(n * 0.75)], d[int(n * 0.9)], d[n - 1]
        printf "  spread max/min = %.1fx\n", d[n - 1] / d[0]
        printf "  distinct runner instances: %d across %d row(s)%s\n", length(runners), rows,
               (length(runners) == rows ? "  -- so no two samples share a machine" : "")
        printf "  logs carrying progress lines: %d of %d\n", withrate + 0, rows
    }
' "$Out"
