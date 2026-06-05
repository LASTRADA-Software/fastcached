# fastcached benchmark suite

A reproducible, cross-platform (Windows / Linux / macOS) benchmark that builds
two git refs of fastcached, drives them across all three wire protocols, and
emits a side-by-side comparison — terminal tables, Markdown + JSON reports, and
PNG / inline-Sixel charts.

The **core suite is dependency-free** (Python 3.11+ standard library only).
Chart rendering is an **optional extra** (`matplotlib`); without it the suite
still runs and reports — it just skips the images.

## Quick start

```sh
# Build master (base) and HEAD (candidate), run the standard profile, compare:
python bench/fastcached_bench.py

# Faster iteration:
python bench/fastcached_bench.py --profile quick

# Compare arbitrary refs:
python bench/fastcached_bench.py --base v1.0 --candidate my-feature-branch

# Skip building; compare two binaries you already have:
python bench/fastcached_bench.py --no-build \
    --base-binary path/to/base/fastcached \
    --candidate-binary path/to/candidate/fastcached

# Charts (PNG + inline Sixel where supported):
python -m pip install -r bench/requirements-plots.txt
```

Run it from an environment where `cmake --build --preset <preset>` already works
for your OS — a *Developer PowerShell for VS* on Windows, or a shell with the
toolchain on `PATH` on Linux/macOS. The default build preset is chosen per OS
(`clangcl-release` on Windows, `clang-release` elsewhere) and can be overridden
with `--preset`.

## What it measures

For each scenario the suite reports **throughput** (ops/sec, median of reps;
higher is better) and **latency** p50/p95/p99/max (ms; lower is better), plus
error and timeout counts. The standard profile sweeps:

- protocols: memcached text, memcached binary, Redis RESP2
- operation mixes: SET-only, GET-only, mixed 1:9 write:read
- storage: in-memory and persistent (`--storage`)
- concurrency: 1, 16, 64, 256 keep-alive connections

plus a headline **connection-ceiling test** (`keepalive-storm`): many
persistent-storage connections opened at once. The candidate (reactor) serves
them all; the base build's per-connection worker pool — once connections exceed
the pool — leaves the rest waiting, which shows up as timeouts / incomplete
connections.

Profiles: `quick` (a few scenarios, ~2 min), `standard` (default), `full`
(adds binary opcodes, INCR/DECR, DELETE, large values, durability sweeps).

## Reproducibility

- Deterministic, fixed-count workloads (fixed RNG seed → identical request
  sequence every repetition).
- A discarded warmup rep, then N measured reps; the median is reported.
- A fresh daemon and a throwaway storage directory per scenario (identical
  initial state).
- `summary.json` and the report's Environment table record CPU, core count, OS,
  Python version, both commit SHAs, preset, and flags.

Absolute numbers depend on the machine and on Python's client-side ceiling; the
**base-vs-candidate deltas** on one machine are what to trust. To reduce
variance: close background apps; on Windows use the High-Performance power plan;
on Linux set the CPU governor to `performance`.

## Output

`bench/results/<timestamp>/` contains `report.md`, `summary.json`, and any
`*.png` charts. `report.md` embeds the charts. The same comparison is printed to
the terminal, with charts drawn inline when the terminal answers the Sixel
capability probe (or with `--charts sixel` to force; `--charts none` to disable).

## Files

| File | Role |
| --- | --- |
| `fastcached_bench.py` | CLI orchestrator: build refs, run, compare, emit |
| `protocols.py` | dependency-free clients for the three wire protocols |
| `workloads.py` | data-driven scenario catalog + profiles |
| `runner.py` | daemon lifecycle, load generation, metric aggregation |
| `report.py` | comparison model + `summary.json` / `report.md` |
| `termviz.py` | terminal tables, color, Sixel detection + encoding |
| `plots.py` | optional matplotlib charts (PNG + RGB for Sixel) |
