# SPDX-License-Identifier: Apache-2.0
"""Build the base-vs-candidate comparison and emit JSON + Markdown reports."""

from __future__ import annotations

import dataclasses
import json
from pathlib import Path

from runner import ScenarioResult

# Higher-is-better for throughput; lower-is-better for latency.
THROUGHPUT_KEY = "throughput_ops_per_sec"
LATENCY_KEY = "latency_p99_ms"


def percent_change(base: float, candidate: float) -> float | None:
    """Signed percent change from base to candidate, or None if base is zero."""
    if base == 0:
        return None
    return (candidate - base) / base * 100.0


@dataclasses.dataclass
class Comparison:
    """One scenario compared across the two builds."""

    name: str
    protocol: str
    storage: str
    connections: int
    base: ScenarioResult
    candidate: ScenarioResult

    @property
    def throughput_delta_pct(self) -> float | None:
        return percent_change(self.base.throughput_ops_per_sec, self.candidate.throughput_ops_per_sec)

    @property
    def latency_delta_pct(self) -> float | None:
        return percent_change(self.base.latency_p99_ms, self.candidate.latency_p99_ms)


def build_comparisons(
    base_results: list[ScenarioResult], candidate_results: list[ScenarioResult]
) -> list[Comparison]:
    """Pair up results by scenario name (intersection, base order preserved)."""
    candidate_by_name = {result.name: result for result in candidate_results}
    comparisons: list[Comparison] = []
    for base in base_results:
        candidate = candidate_by_name.get(base.name)
        if candidate is None:
            continue
        comparisons.append(
            Comparison(
                name=base.name,
                protocol=base.protocol,
                storage=base.storage,
                connections=base.connections,
                base=base,
                candidate=candidate,
            )
        )
    return comparisons


def write_json(path: Path, env: dict, comparisons: list[Comparison]) -> None:
    payload = {
        "environment": env,
        "scenarios": [
            {
                "name": comparison.name,
                "protocol": comparison.protocol,
                "storage": comparison.storage,
                "connections": comparison.connections,
                "throughput_delta_pct": comparison.throughput_delta_pct,
                "latency_p99_delta_pct": comparison.latency_delta_pct,
                "base": comparison.base.as_dict(),
                "candidate": comparison.candidate.as_dict(),
            }
            for comparison in comparisons
        ],
    }
    path.write_text(json.dumps(payload, indent=2), encoding="utf-8")


def _format_delta(value: float | None, improvement_is_negative: bool) -> str:
    if value is None:
        return "n/a"
    arrow = ""
    improved = (value < 0) if improvement_is_negative else (value > 0)
    if abs(value) >= 1.0:
        arrow = " ✓" if improved else " ✗"
    return f"{value:+.1f}%{arrow}"


def _highlights(comparisons: list[Comparison]) -> list[str]:
    lines: list[str] = []
    storm = [c for c in comparisons if c.name.startswith("keepalive-storm")]
    for comparison in storm:
        base = comparison.base
        candidate = comparison.candidate
        lines.append(
            f"- **Connection-ceiling test** (`{comparison.name}`): "
            f"base completed {base.connections_completed}/{base.connections} connections "
            f"(timeouts={base.timeouts}, errors={base.errors}); "
            f"candidate completed {candidate.connections_completed}/{candidate.connections} "
            f"(timeouts={candidate.timeouts}, errors={candidate.errors})."
        )
    ranked = [c for c in comparisons if c.throughput_delta_pct is not None]
    ranked.sort(key=lambda c: c.throughput_delta_pct or 0.0, reverse=True)
    if ranked:
        best = ranked[0]
        worst = ranked[-1]
        lines.append(
            f"- **Largest throughput gain**: `{best.name}` "
            f"{_format_delta(best.throughput_delta_pct, improvement_is_negative=False)}."
        )
        lines.append(
            f"- **Largest throughput regression / smallest gain**: `{worst.name}` "
            f"{_format_delta(worst.throughput_delta_pct, improvement_is_negative=False)}."
        )
    return lines


def write_markdown(
    path: Path,
    env: dict,
    comparisons: list[Comparison],
    png_files: list[Path],
) -> None:
    """Write the human-readable report, embedding any generated PNGs."""
    lines: list[str] = []
    lines.append("# fastcached benchmark — base vs candidate\n")
    lines.append("## Environment\n")
    lines.append("| Field | Value |")
    lines.append("| --- | --- |")
    for key in (
        "timestamp",
        "host",
        "os",
        "cpu",
        "cores",
        "python",
        "preset",
        "profile",
        "reps",
        "base_ref",
        "base_sha",
        "candidate_ref",
        "candidate_sha",
    ):
        if key in env:
            lines.append(f"| {key} | {env[key]} |")
    lines.append("")

    lines.append("## Highlights\n")
    highlights = _highlights(comparisons)
    lines.extend(highlights if highlights else ["- (no highlights)"])
    lines.append("")

    if png_files:
        lines.append("## Charts\n")
        for png in png_files:
            lines.append(f"![{png.stem}]({png.name})\n")

    lines.append("## Per-scenario results\n")
    lines.append(
        "Throughput is ops/sec (median of reps; higher is better). "
        "Latency is p99 in ms (lower is better). Delta is candidate vs base.\n"
    )
    lines.append(
        "| Scenario | Base ops/s | Cand ops/s | Δ thrpt | Base p99 ms | Cand p99 ms | Δ p99 | Timeouts (b/c) |"
    )
    lines.append("| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |")
    for comparison in comparisons:
        base = comparison.base
        candidate = comparison.candidate
        lines.append(
            "| {name} | {b_thr:,.0f} | {c_thr:,.0f} | {d_thr} | "
            "{b_lat:.2f} | {c_lat:.2f} | {d_lat} | {b_to}/{c_to} |".format(
                name=comparison.name,
                b_thr=base.throughput_ops_per_sec,
                c_thr=candidate.throughput_ops_per_sec,
                d_thr=_format_delta(comparison.throughput_delta_pct, improvement_is_negative=False),
                b_lat=base.latency_p99_ms,
                c_lat=candidate.latency_p99_ms,
                d_lat=_format_delta(comparison.latency_delta_pct, improvement_is_negative=True),
                b_to=base.timeouts,
                c_to=candidate.timeouts,
            )
        )
    lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8")
