# SPDX-License-Identifier: Apache-2.0
"""fastcached benchmark orchestrator.

Builds a base ref and a candidate ref (each in its own git worktree), runs the
selected workload profile against both, and emits a side-by-side comparison to
the terminal (tables + optional inline Sixel charts) and to disk (``report.md``,
``summary.json``, and PNG charts).

Typical use::

    python bench/fastcached_bench.py                      # master vs HEAD, standard
    python bench/fastcached_bench.py --profile quick
    python bench/fastcached_bench.py --base v1.0 --candidate HEAD
    python bench/fastcached_bench.py --no-build \
        --base-binary <path> --candidate-binary <path>

The build step shells out to CMake, so run it from an environment where
``cmake --build --preset <preset>`` already works for your OS (a Developer
PowerShell on Windows; a shell with the toolchain on PATH elsewhere).
"""

from __future__ import annotations

import argparse
import concurrent.futures
import os
import platform
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import plots
import report
import servers
import termviz
from runner import ScenarioResult, measure_running_server, run_scenario
from workloads import scenarios_for

REPO_ROOT = Path(__file__).resolve().parent.parent
HOST = "127.0.0.1"


# --- Platform-specific build details ------------------------------------------

def default_preset() -> str:
    system = platform.system()
    if system == "Windows":
        return "clangcl-release"
    return "clang-release"  # Linux and macOS


def binary_name() -> str:
    return "fastcached.exe" if platform.system() == "Windows" else "fastcached"


def git_sha(ref: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(REPO_ROOT), "rev-parse", "--short", ref],
        capture_output=True, text=True, check=True,
    )
    return result.stdout.strip()


def build_ref(ref: str, preset: str) -> Path:
    """Build ``ref`` in an isolated worktree and return its fastcached binary path."""
    sha = git_sha(ref)
    worktree = Path(tempfile.gettempdir()) / "fastcached-bench" / sha
    binary = worktree / "out" / "build" / preset / "target" / binary_name()
    if binary.exists():
        print(f"  [{ref} @ {sha}] reusing cached build at {binary}")
        return binary

    if worktree.exists():
        # Stale worktree without a usable binary — drop and recreate.
        subprocess.run(["git", "-C", str(REPO_ROOT), "worktree", "remove", "--force", str(worktree)], check=False)
        shutil.rmtree(worktree, ignore_errors=True)
    worktree.parent.mkdir(parents=True, exist_ok=True)

    print(f"  [{ref} @ {sha}] creating worktree {worktree}")
    subprocess.run(
        ["git", "-C", str(REPO_ROOT), "worktree", "add", "--detach", str(worktree), ref],
        check=True,
    )
    print(f"  [{ref} @ {sha}] configuring ({preset}) ...")
    subprocess.run(["cmake", "--preset", preset], cwd=worktree, check=True)
    print(f"  [{ref} @ {sha}] building ...")
    subprocess.run(["cmake", "--build", "--preset", preset], cwd=worktree, check=True)
    if not binary.exists():
        raise FileNotFoundError(f"build did not produce {binary}")
    return binary


def remove_worktree(ref: str) -> None:
    sha = git_sha(ref)
    worktree = Path(tempfile.gettempdir()) / "fastcached-bench" / sha
    if worktree.exists():
        subprocess.run(["git", "-C", str(REPO_ROOT), "worktree", "remove", "--force", str(worktree)], check=False)
        shutil.rmtree(worktree, ignore_errors=True)


# --- Environment metadata -----------------------------------------------------

def gather_env(args, base_sha: str, candidate_sha: str) -> dict:
    return {
        "timestamp": time.strftime("%Y-%m-%d %H:%M:%S"),
        "host": platform.node(),
        "os": platform.platform(),
        "cpu": platform.processor() or platform.machine(),
        "cores": os.cpu_count(),
        "python": platform.python_version(),
        "preset": args.preset,
        "profile": args.profile,
        "reps": args.reps,
        "base_ref": args.base,
        "base_sha": base_sha,
        "candidate_ref": args.candidate,
        "candidate_sha": candidate_sha,
    }


# --- Suite execution ----------------------------------------------------------

def run_suite(
    label: str,
    executor: concurrent.futures.ProcessPoolExecutor,
    pool_size: int,
    binary: Path,
    profile: str,
    storage_root: Path,
    base_port: int,
    reps: int,
    warmup: int,
    seed: int,
    op_timeout: float,
) -> list[ScenarioResult]:
    scenarios = scenarios_for(profile)
    results: list[ScenarioResult] = []
    for index, scenario in enumerate(scenarios):
        port = base_port + index
        storage_dir = storage_root / f"{label}-{index}"
        print(f"  [{label}] ({index + 1}/{len(scenarios)}) {scenario.name}", flush=True)
        try:
            result = run_scenario(
                executor, pool_size, binary, scenario, HOST, port, storage_dir,
                reps, warmup, seed, op_timeout,
            )
        except Exception as error:  # keep going; record nothing for this scenario
            print(f"      ! scenario failed: {error}", file=sys.stderr)
            continue
        finally:
            shutil.rmtree(storage_dir, ignore_errors=True)
        results.append(result)
    return results


def run_multitarget(
    fastcached_binary: Path,
    vs_names: list[str],
    profile: str,
    storage_root: Path,
    base_port: int,
    reps: int,
    warmup: int,
    seed: int,
    op_timeout: float,
    redis_server: str | None,
) -> tuple[list[str], list]:
    """Run scenarios against fastcached and each available real server.

    Real servers (in-memory only) run the memory scenarios for the protocols
    they speak; fastcached runs everything. Returns (target_names, rows) where
    each row is (scenario_name, {target_name: ScenarioResult}).
    """
    pool_size = os.cpu_count() or 1
    # Start the requested real servers on distinct high ports (some low ports
    # are blocked); skip any whose dependency is missing.
    started: list = []
    port_offset = 100
    for name in vs_names:
        server = servers.build_baseline(name, HOST, base_port + port_offset, redis_server)
        port_offset += 100
        if server is None:
            print(f"  (baseline '{name}' unavailable — skipping)")
            continue
        print(f"  starting baseline '{name}' on port {server.port} ...")
        try:
            server.start()
            started.append(server)
        except Exception as error:  # noqa: BLE001 - report and continue
            print(f"  ! baseline '{name}' failed to start: {error}", file=sys.stderr)

    target_names = ["fastcached", *[s.name for s in started]]
    scenarios = scenarios_for(profile)
    rows: list = []
    try:
        with concurrent.futures.ProcessPoolExecutor(max_workers=pool_size) as executor:
            for index, scenario in enumerate(scenarios):
                print(f"  ({index + 1}/{len(scenarios)}) {scenario.name}", flush=True)
                by_target: dict = {}
                storage_dir = storage_root / f"fc-{index}"
                try:
                    by_target["fastcached"] = run_scenario(
                        executor, pool_size, fastcached_binary, scenario, HOST, base_port + index,
                        storage_dir, reps, warmup, seed, op_timeout,
                    )
                except Exception as error:  # noqa: BLE001
                    print(f"      ! fastcached scenario failed: {error}", file=sys.stderr)
                finally:
                    shutil.rmtree(storage_dir, ignore_errors=True)
                # Real servers are in-memory only: skip disk scenarios.
                if scenario.storage == "memory":
                    for server in started:
                        if scenario.protocol in server.protocols:
                            try:
                                by_target[server.name] = measure_running_server(
                                    executor, pool_size, scenario, server.host, server.port,
                                    reps, warmup, seed, op_timeout, flush_first=True,
                                )
                            except Exception as error:  # noqa: BLE001
                                print(f"      ! {server.name} scenario failed: {error}", file=sys.stderr)
                rows.append((scenario.name, by_target))
    finally:
        for server in started:
            server.stop()
    return target_names, rows


def main() -> int:
    parser = argparse.ArgumentParser(description="Benchmark fastcached: base vs candidate.")
    parser.add_argument("--base", default="master", help="base git ref (default: master)")
    parser.add_argument("--candidate", default="HEAD", help="candidate git ref (default: HEAD)")
    parser.add_argument("--profile", default="standard", choices=("quick", "standard", "full"))
    parser.add_argument("--preset", default=default_preset(), help="CMake build preset")
    parser.add_argument("--reps", type=int, default=3, help="measured repetitions per scenario")
    parser.add_argument("--warmup", type=int, default=1, help="warmup repetitions (discarded)")
    parser.add_argument("--seed", type=int, default=1234, help="RNG seed for deterministic workloads")
    parser.add_argument("--port", type=int, default=21211, help="base TCP port (incremented per scenario)")
    parser.add_argument("--op-timeout", type=float, default=5.0, help="per-operation client timeout (s)")
    parser.add_argument("--out", default=str(REPO_ROOT / "bench" / "results"), help="results directory")
    parser.add_argument("--charts", default="auto", choices=("auto", "sixel", "png", "none"))
    parser.add_argument("--no-build", action="store_true", help="skip building; use --base-binary/--candidate-binary")
    parser.add_argument("--base-binary", help="prebuilt base binary (with --no-build)")
    parser.add_argument("--candidate-binary", help="prebuilt candidate binary (with --no-build)")
    parser.add_argument("--vs", default="", help="comma list of real baselines to compare against: redis,memcached")
    parser.add_argument("--redis-server", help="path to redis-server (else discovered on PATH)")
    args = parser.parse_args()

    # Never let a non-UTF-8 stdout (e.g. a redirected cp1252 console on Windows)
    # crash the run on a stray glyph; degrade those characters instead.
    try:
        sys.stdout.reconfigure(errors="replace")  # type: ignore[union-attr]
    except (AttributeError, ValueError):
        pass

    # Resolve binaries (build or provided). --vs only needs the candidate.
    if args.no_build:
        if not args.candidate_binary:
            parser.error("--no-build requires --candidate-binary")
        if not args.vs and not args.base_binary:
            parser.error("--no-build (base-vs-candidate) requires --base-binary")
        base_binary = Path(args.base_binary) if args.base_binary else None
        candidate_binary = Path(args.candidate_binary)
        base_sha = candidate_sha = "provided"
    else:
        candidate_binary = build_ref(args.candidate, args.preset)
        candidate_sha = git_sha(args.candidate)
        if args.vs:
            base_binary, base_sha = None, ""
        else:
            print("Building base ...")
            base_binary = build_ref(args.base, args.preset)
            base_sha = git_sha(args.base)

    pool_size = os.cpu_count() or 1
    timestamp = time.strftime("%Y%m%d-%H%M%S")
    out_dir = Path(args.out) / timestamp
    out_dir.mkdir(parents=True, exist_ok=True)

    # --vs mode: compare the candidate fastcached against real redis/memcached.
    if args.vs:
        vs_names = [name.strip() for name in args.vs.split(",") if name.strip()]
        env = gather_env(args, base_sha, candidate_sha)
        with tempfile.TemporaryDirectory(prefix="fastcached-bench-storage-") as storage_root_str:
            target_names, rows = run_multitarget(
                candidate_binary, vs_names, args.profile, Path(storage_root_str),
                args.port, args.reps, args.warmup, args.seed, args.op_timeout, args.redis_server,
            )
        chart_objects = [] if args.charts == "none" else plots.render_multitarget(target_names, rows, out_dir)
        png_files = [chart.path for chart in chart_objects]
        report.write_multitarget_json(out_dir / "summary.json", env, target_names, rows)
        report.write_multitarget_markdown(out_dir / "report.md", env, target_names, rows, png_files)
        print()
        termviz.print_environment(env)
        termviz.print_multitarget_table(target_names, rows)
        if chart_objects and termviz.detect_sixel(args.charts):
            termviz.render_charts_to_terminal(chart_objects)
        elif png_files:
            print(f"Charts written to {out_dir}.")
        print(f"\nReport:  {out_dir / 'report.md'}")
        print(f"Summary: {out_dir / 'summary.json'}")
        return 0

    with tempfile.TemporaryDirectory(prefix="fastcached-bench-storage-") as storage_root_str:
        storage_root = Path(storage_root_str)
        with concurrent.futures.ProcessPoolExecutor(max_workers=pool_size) as executor:
            print("\nRunning base suite ...")
            base_results = run_suite(
                "base", executor, pool_size, base_binary, args.profile, storage_root,
                args.port, args.reps, args.warmup, args.seed, args.op_timeout,
            )
            print("\nRunning candidate suite ...")
            candidate_results = run_suite(
                "candidate", executor, pool_size, candidate_binary, args.profile, storage_root,
                args.port + 1000, args.reps, args.warmup, args.seed, args.op_timeout,
            )

    env = gather_env(args, base_sha, candidate_sha)
    comparisons = report.build_comparisons(base_results, candidate_results)

    # Charts (optional; degrade gracefully).
    chart_objects = [] if args.charts == "none" else plots.render_all(comparisons, out_dir)
    png_files = [chart.path for chart in chart_objects]
    if args.charts != "none" and not plots.available():
        print("(matplotlib not installed — skipping PNG/Sixel charts; install with "
              "`pip install -r bench/requirements-plots.txt`)\n")

    # Reports on disk.
    report.write_json(out_dir / "summary.json", env, comparisons)
    report.write_markdown(out_dir / "report.md", env, comparisons, png_files)

    # Terminal output.
    print()
    termviz.print_environment(env)
    termviz.print_comparison_table(comparisons)
    from report import _highlights  # internal helper reuse
    termviz.print_highlights(_highlights(comparisons))

    if chart_objects and termviz.detect_sixel(args.charts):
        termviz.render_charts_to_terminal(chart_objects)
    elif png_files:
        print(f"Charts written to {out_dir} (terminal is not Sixel-capable or --charts disabled inline rendering).")

    print(f"\nReport:  {out_dir / 'report.md'}")
    print(f"Summary: {out_dir / 'summary.json'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
