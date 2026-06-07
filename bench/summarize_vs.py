# SPDX-License-Identifier: Apache-2.0
"""Print a small/large value performance summary from a `--vs` summary.json.

Usage: python bench/summarize_vs.py <results-dir>/summary.json
Reports fastcached throughput vs redis and memcached, split by value size
(small = 64 B, large = 64 KiB), with the geomean speedup (>1 = fastcached
faster) and the per-concurrency breakdown for the GET hot path.
"""
from __future__ import annotations

import json
import sys


def geomean(xs: list[float]) -> float | None:
    xs = [x for x in xs if x]
    if not xs:
        return None
    p = 1.0
    for x in xs:
        p *= x
    return p ** (1.0 / len(xs))


def main() -> int:
    path = sys.argv[1]
    d = json.load(open(path))
    scn = d["scenarios"]
    env = d.get("environment", {})

    def speedup(s: dict, comp: str) -> float | None:
        t = s["targets"]
        if comp not in t:
            return None
        return t["fastcached"]["throughput_ops_per_sec"] / t[comp]["throughput_ops_per_sec"]

    small = [s for s in scn if s["targets"]["fastcached"]["value_bytes"] == 64]
    large = [s for s in scn if s["targets"]["fastcached"]["value_bytes"] > 1000]

    print(f"Environment: {env.get('cpu','?')} x{env.get('cores','?')} cores, "
          f"{env.get('os','?')}, preset={env.get('preset','?')}, reps={env.get('reps','?')}")
    print(f"Candidate: {env.get('candidate_ref','?')}  (redis = Valkey fork; memcached native)\n")

    print(f"{'='*72}")
    print(f"{'GEOMEAN throughput speedup (fastcached / competitor; >1 = we win)':^72}")
    print(f"{'='*72}")
    print(f"{'':16}{'vs memcached':>18}{'vs redis/Valkey':>20}")
    for label, group in (("small (64 B)", small), ("large (64 KiB)", large)):
        mc = geomean([speedup(s, "memcached") for s in group if "memcached" in s["targets"]])
        rd = geomean([speedup(s, "redis") for s in group if "redis" in s["targets"]])
        mcs = f"{mc:.2f}x" if mc else "-"
        rds = f"{rd:.2f}x" if rd else "-"
        print(f"{label:16}{mcs:>18}{rds:>20}")

    for comp, title in (("memcached", "vs memcached"), ("redis", "vs redis/Valkey")):
        print(f"\n{title} — GET throughput by value size x concurrency (ops/sec):")
        print(f"  {'scenario':44}{'fastcached':>12}{comp:>12}{'ratio':>8}")
        rows = [s for s in scn if s["targets"]["fastcached"]["op_mix"] == "get"
                and s["targets"]["fastcached"]["storage"] == "memory" and comp in s["targets"]]
        rows.sort(key=lambda s: (s["targets"]["fastcached"]["value_bytes"],
                                 s["targets"]["fastcached"]["connections"]))
        for s in rows:
            fc = s["targets"]["fastcached"]
            other = s["targets"][comp]["throughput_ops_per_sec"]
            r = speedup(s, comp)
            print(f"  {s['name']:44}{fc['throughput_ops_per_sec']:>12,.0f}{other:>12,.0f}{r:>7.2f}x")

    errors = sum(tv.get("errors", 0) for s in scn for tv in s["targets"].values())
    timeouts = sum(tv.get("timeouts", 0) for s in scn for tv in s["targets"].values())
    print(f"\nCorrectness: errors={errors} timeouts={timeouts} across all scenarios/targets.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
