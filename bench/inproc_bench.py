# SPDX-License-Identifier: Apache-2.0
"""In-process cache benchmark: fastcached's storage stack vs jitbit/FastCache.

The rest of ``bench/`` measures the *daemon* — a TCP round trip per operation,
where a single request costs microseconds and the network dominates. That is the
right comparison against memcached and redis, and the wrong one against an
in-process cache library: a dictionary in your own address space has no network
to pay for, so the two differ by roughly three orders of magnitude for reasons
that say nothing about either implementation.

This script measures the one layer where the comparison is meaningful — the
in-process cache map — by running both sides on the same machine:

* ``fastcache-bench``, a Catch2 benchmark over our storage stack, decomposed so
  each production concern (byte budget, sharding, the CacheEngine facade) shows
  up as its own row;
* jitbit/FastCache's own BenchmarkDotNet suite, unmodified, cloned and run
  locally rather than quoted from its README (whose numbers are from unknown
  hardware and so cannot be divided into ours).

Both sides' benchmark bodies issue ``LOOKUPS_PER_ITERATION`` lookups, so both
means are divided by it to reach nanoseconds per single operation.

Typical use::

    python bench/inproc_bench.py                  # build, run both, compare
    python bench/inproc_bench.py --no-jitbit      # ours only (no .NET SDK needed)
    python bench/inproc_bench.py --no-build --samples 200

Requires a working CMake toolchain, as the rest of the suite does. The jitbit
half additionally needs ``git`` and a .NET 10 SDK on PATH; without them the
script reports the baseline as unavailable and still emits our numbers, the same
way ``--vs`` degrades when a real server binary is missing.
"""

from __future__ import annotations

import argparse
import json
import platform
import re
import shutil
import subprocess
import sys
import time
import xml.etree.ElementTree as ElementTree
from pathlib import Path

import termviz

REPO_ROOT = Path(__file__).resolve().parent.parent

#: Lookups performed per benchmark body, on both sides. Must match
#: ``LookupsPerIteration`` in ``src/apps/fastcache-bench/StorageBench.cpp`` and
#: the four ``TryGet`` calls in jitbit's ``FastCache.Benchmarks/Program.cs``.
LOOKUPS_PER_ITERATION = 4

JITBIT_REPO = "https://github.com/jitbit/FastCache"

#: Rows whose bodies are a single operation rather than LOOKUPS_PER_ITERATION,
#: so the normalizer must not divide them.
SINGLE_OP_ROWS = frozenset({"FastCacheAddRemove", "MemoryCacheAddRemove"})

#: How to describe each of our benchmark rows in the report. Adding a row to the
#: C++ table means adding a line here; nothing else in this script changes.
ROW_NOTES = {
    "control-unordered-map": "bare std::unordered_map - the container floor",
    "clock-now": "one injected IClock::Now(), for attribution (not a cache op)",
    "lru-unbounded": "InMemoryLruStorage, eviction off - jitbit-matched semantics",
    "lru-bounded": "+ byte budget and eviction bookkeeping",
    "lru-strict": "+ exact LRU (promotes on every read)",
    "sharded": "+ shard index and the per-shard shared_mutex",
    "engine-steadyclock": "+ CacheEngine facade, reading the OS clock per operation",
    "engine-cachedclock": "the same, with the reactor-refreshed CachedClock (shipped)",
}


def default_preset() -> str:
    """Return the CMake preset this OS builds release binaries with."""
    return "clangcl-release" if platform.system() == "Windows" else "clang-release"


def bench_binary(preset: str) -> Path:
    """Return the path the benchmark executable is built to for ``preset``."""
    name = "fastcache-bench.exe" if platform.system() == "Windows" else "fastcache-bench"
    return REPO_ROOT / "out" / "build" / preset / "target" / name


# --- Our side -----------------------------------------------------------------

def build_bench(preset: str) -> None:
    """Configure with benchmarks enabled and build the benchmark target."""
    subprocess.run(
        ["cmake", "--preset", preset, "-DFASTCACHED_BUILD_BENCHMARKS=ON"],
        cwd=REPO_ROOT, check=True,
    )
    subprocess.run(
        ["cmake", "--build", "--preset", preset, "--target", "fastcache-bench"],
        cwd=REPO_ROOT, check=True,
    )


def run_fastcached(binary: Path, samples: int) -> dict[str, float]:
    """Run the Catch2 benchmarks and return {row name: nanoseconds per body}.

    Catch2's XML reporter emits one ``<BenchmarkResults>`` per row with a
    ``<mean value=...>`` child in nanoseconds; parsing that is more robust than
    scraping the console table's aligned columns.
    """
    completed = subprocess.run(
        [str(binary), "[lookup]", "--reporter", "xml", "--benchmark-samples", str(samples)],
        capture_output=True, text=True, check=True,
    )
    root = ElementTree.fromstring(completed.stdout)
    results: dict[str, float] = {}
    for node in root.iter("BenchmarkResults"):
        mean = node.find("mean")
        if mean is not None and mean.get("value"):
            results[node.get("name", "?")] = float(mean.get("value", "0"))
    if not results:
        raise RuntimeError("no benchmark results parsed from fastcache-bench XML output")
    return results


# --- jitbit's side ------------------------------------------------------------

def _parse_bdn_time(cell: str) -> float | None:
    """Parse a BenchmarkDotNet duration cell such as ``20.23 ns`` into nanoseconds.

    Handles thousands separators and the unit BDN picks per column, which is not
    always nanoseconds — a slower row is reported in microseconds and silently
    reading the number alone would be off by a thousand.
    """
    match = re.match(r"^\s*([\d,]+(?:\.\d+)?)\s*(ns|us|µs|ms|s)\s*$", cell)
    if not match:
        return None
    value = float(match.group(1).replace(",", ""))
    scale = {"ns": 1.0, "us": 1e3, "µs": 1e3, "ms": 1e6, "s": 1e9}
    return value * scale[match.group(2)]


def run_jitbit(workdir: Path) -> dict[str, float]:
    """Clone and run jitbit/FastCache's own benchmark; return {method: ns per body}.

    BenchmarkDotNet resolves the built assembly relative to the *working
    directory*, so the run has to happen inside the project directory rather
    than via ``--project`` from elsewhere.
    """
    if shutil.which("dotnet") is None:
        raise RuntimeError("no 'dotnet' on PATH - install a .NET 10 SDK or pass --no-jitbit")

    checkout = workdir / "jitbit-FastCache"
    if not checkout.exists():
        subprocess.run(
            ["git", "clone", "--depth", "1", JITBIT_REPO, str(checkout)], check=True,
        )
    project = checkout / "FastCache.Benchmarks"

    subprocess.run(["dotnet", "run", "-c", "Release"], cwd=project, check=True)

    csv_path = project / "BenchmarkDotNet.Artifacts" / "results" / "BenchMark-report.csv"
    if not csv_path.exists():
        raise RuntimeError(f"BenchmarkDotNet produced no report at {csv_path}")

    import csv as csv_module

    results: dict[str, float] = {}
    with csv_path.open(newline="", encoding="utf-8-sig") as handle:
        for row in csv_module.DictReader(handle):
            method = (row.get("Method") or "").strip()
            nanoseconds = _parse_bdn_time(row.get("Mean") or "")
            if method and nanoseconds is not None:
                results[method] = nanoseconds
    return results


# --- Normalization and output -------------------------------------------------

def per_operation(name: str, body_nanoseconds: float) -> float:
    """Reduce a benchmark body's mean to nanoseconds for one cache operation."""
    if name in SINGLE_OP_ROWS:
        return body_nanoseconds / 2.0  # an add plus a remove
    return body_nanoseconds / LOOKUPS_PER_ITERATION


def collect_environment(preset: str) -> dict[str, str]:
    """Describe the machine and build, so a report is reproducible in hindsight."""
    return {
        "os": f"{platform.system()} {platform.release()} ({platform.version()})",
        "cpu": platform.processor() or "unknown",
        "python": platform.python_version(),
        "preset": preset,
        "commit": subprocess.run(
            ["git", "-C", str(REPO_ROOT), "rev-parse", "--short", "HEAD"],
            capture_output=True, text=True, check=False,
        ).stdout.strip(),
    }


def print_table(rows: list[tuple[str, float, str]], reference: float | None) -> None:
    """Print the ns/op comparison, styled like the rest of the suite's tables."""
    color = termviz.supports_color()
    headers = ["Row", "ns/op", "vs jitbit", "What it adds"]
    body: list[list[str]] = []
    for name, nanoseconds, note in rows:
        ratio = "-" if not reference else f"{nanoseconds / reference:.2f}x"
        body.append([name, f"{nanoseconds:.2f}", ratio, note])

    widths = [len(h) for h in headers]
    for cells in body:
        for index, cell in enumerate(cells):
            widths[index] = max(widths[index], len(cell))

    def render(cells: list[str]) -> str:
        out = []
        for index, cell in enumerate(cells):
            out.append(cell.ljust(widths[index]) if index != 1 else cell.rjust(widths[index]))
        return " | ".join(out)

    header_line = render(headers)
    print(termviz._colorize(header_line, termviz.BOLD, color))
    print(termviz._colorize("-" * len(header_line), termviz.DIM, color))
    for cells in body:
        print(render(cells))
    print()


def write_report(outdir: Path, environment: dict, ours: dict, theirs: dict) -> None:
    """Write ``report.md`` and ``summary.json`` next to the suite's other results."""
    outdir.mkdir(parents=True, exist_ok=True)
    (outdir / "summary.json").write_text(
        json.dumps(
            {
                "environment": environment,
                "lookups_per_iteration": LOOKUPS_PER_ITERATION,
                "fastcached_ns_per_op": {k: per_operation(k, v) for k, v in ours.items()},
                "jitbit_ns_per_op": {k: per_operation(k, v) for k, v in theirs.items()},
            },
            indent=2,
        ),
        encoding="utf-8",
    )

    lines = [
        "# In-process cache comparison: fastcached vs jitbit/FastCache",
        "",
        "Both sides measured on this machine. Times are nanoseconds per single",
        "cache operation; each benchmark body performs "
        f"{LOOKUPS_PER_ITERATION} lookups and is divided accordingly.",
        "",
        "## Environment",
        "",
        "| Key | Value |",
        "| --- | --- |",
    ]
    lines += [f"| {key} | {value} |" for key, value in environment.items()]

    reference = per_operation("FastCacheLookup", theirs["FastCacheLookup"]) if "FastCacheLookup" in theirs else None

    lines += ["", "## fastcached storage stack", "", "| Row | ns/op | vs jitbit | What it adds |", "| --- | ---: | ---: | --- |"]
    for name, body_ns in ours.items():
        nanoseconds = per_operation(name, body_ns)
        ratio = "-" if not reference else f"{nanoseconds / reference:.2f}x"
        lines.append(f"| {name} | {nanoseconds:.2f} | {ratio} | {ROW_NOTES.get(name, '')} |")

    if theirs:
        lines += ["", "## jitbit/FastCache (its own BenchmarkDotNet suite)", "", "| Method | ns/op |", "| --- | ---: |"]
        for name, body_ns in theirs.items():
            lines.append(f"| {name} | {per_operation(name, body_ns):.2f} |")

    (outdir / "report.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--preset", default=default_preset(), help="CMake build preset")
    parser.add_argument("--no-build", action="store_true", help="skip configure/build")
    parser.add_argument("--samples", type=int, default=100, help="Catch2 samples per row")
    parser.add_argument("--no-jitbit", action="store_true", help="skip the jitbit baseline")
    parser.add_argument("--out", default=str(REPO_ROOT / "bench" / "results"), help="results directory")
    args = parser.parse_args()

    if not args.no_build:
        build_bench(args.preset)

    binary = bench_binary(args.preset)
    if not binary.exists():
        print(f"benchmark binary not found: {binary}", file=sys.stderr)
        return 1

    ours = run_fastcached(binary, args.samples)

    theirs: dict[str, float] = {}
    if not args.no_jitbit:
        try:
            theirs = run_jitbit(Path(args.out).parent / "third-party")
        except (RuntimeError, subprocess.CalledProcessError) as error:
            print(f"(jitbit baseline unavailable - skipping: {error})\n", file=sys.stderr)

    environment = collect_environment(args.preset)
    termviz.print_environment(environment)

    reference = per_operation("FastCacheLookup", theirs["FastCacheLookup"]) if "FastCacheLookup" in theirs else None
    if reference:
        print(f"jitbit FastCache lookup: {reference:.2f} ns/op (this machine)\n")

    print_table(
        [(name, per_operation(name, value), ROW_NOTES.get(name, "")) for name, value in ours.items()],
        reference,
    )

    outdir = Path(args.out) / time.strftime("inproc-%Y%m%d-%H%M%S")
    write_report(outdir, environment, ours, theirs)
    print(f"wrote {outdir / 'report.md'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
