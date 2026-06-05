# SPDX-License-Identifier: Apache-2.0
"""Optional chart rendering (PNG + in-memory RGB for Sixel).

Everything here is best-effort: if ``matplotlib`` is not installed, :func:`available`
returns ``False`` and :func:`render_all` returns an empty list, so the suite
still runs and reports — it just skips images.
"""

from __future__ import annotations

import dataclasses
from pathlib import Path

from report import Comparison


def available() -> bool:
    """True if matplotlib can be imported (the only plotting dependency)."""
    try:
        import matplotlib  # noqa: F401
        import numpy  # noqa: F401
    except Exception:
        return False
    return True


@dataclasses.dataclass
class Chart:
    """A rendered chart: a PNG on disk plus an RGB buffer for Sixel."""

    title: str
    path: Path
    rgb: object  # numpy uint8 array HxWx3, or None
    width: int
    height: int


def _figure_to_chart(fig, title: str, path: Path) -> Chart:
    import numpy as np

    fig.savefig(path, dpi=100, bbox_inches="tight")
    fig.canvas.draw()
    buffer = np.asarray(fig.canvas.buffer_rgba())
    rgb = buffer[..., :3].copy()
    height, width = rgb.shape[0], rgb.shape[1]
    return Chart(title=title, path=path, rgb=rgb, width=width, height=height)


def _line_vs_connections(plt, comparisons, storage, op_mix, metric, ylabel, title):
    """One line chart: metric vs connection count, base vs candidate per protocol."""
    protocols = sorted({c.protocol for c in comparisons if c.storage == storage and c.base.op_mix == op_mix})
    if not protocols:
        return None
    fig, axis = plt.subplots(figsize=(7, 4))
    plotted = False
    for protocol in protocols:
        points = sorted(
            (c for c in comparisons if c.protocol == protocol and c.storage == storage and c.base.op_mix == op_mix),
            key=lambda c: c.connections,
        )
        if len(points) < 2:
            continue
        xs = [c.connections for c in points]
        base_ys = [getattr(c.base, metric) for c in points]
        cand_ys = [getattr(c.candidate, metric) for c in points]
        line = axis.plot(xs, cand_ys, marker="o", label=f"{protocol} candidate")[0]
        axis.plot(xs, base_ys, marker="x", linestyle="--", color=line.get_color(), label=f"{protocol} base")
        plotted = True
    if not plotted:
        plt.close(fig)
        return None
    axis.set_xlabel("concurrent connections")
    axis.set_ylabel(ylabel)
    axis.set_title(title)
    axis.set_xscale("log", base=2)
    axis.grid(True, which="both", alpha=0.3)
    axis.legend(fontsize=8)
    return fig


def render_all(comparisons: list[Comparison], out_dir: Path) -> list[Chart]:
    """Render the comparison charts that make sense for this data set."""
    if not available() or not comparisons:
        return []
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    charts: list[Chart] = []

    specs = [
        ("disk", "mixed", "throughput_ops_per_sec", "ops/sec",
         "Throughput vs connections (disk, mixed)", "throughput_vs_connections_disk_mixed.png"),
        ("disk", "mixed", "latency_p99_ms", "p99 latency (ms)",
         "p99 latency vs connections (disk, mixed)", "latency_p99_vs_connections_disk_mixed.png"),
        ("memory", "get", "throughput_ops_per_sec", "ops/sec",
         "Throughput vs connections (memory, get)", "throughput_vs_connections_memory_get.png"),
    ]
    for storage, op_mix, metric, ylabel, title, filename in specs:
        fig = _line_vs_connections(plt, comparisons, storage, op_mix, metric, ylabel, title)
        if fig is not None:
            charts.append(_figure_to_chart(fig, title, out_dir / filename))
            plt.close(fig)

    # Throughput delta overview (sorted bar).
    ranked = sorted(
        (c for c in comparisons if c.throughput_delta_pct is not None),
        key=lambda c: c.throughput_delta_pct,
    )
    if ranked:
        fig, axis = plt.subplots(figsize=(8, max(3, len(ranked) * 0.18)))
        labels = [c.name for c in ranked]
        values = [c.throughput_delta_pct for c in ranked]
        colors = ["#2ca02c" if v >= 0 else "#d62728" for v in values]
        axis.barh(range(len(values)), values, color=colors)
        axis.set_yticks(range(len(values)))
        axis.set_yticklabels(labels, fontsize=6)
        axis.axvline(0, color="black", linewidth=0.8)
        axis.set_xlabel("throughput change candidate vs base (%)")
        axis.set_title("Throughput delta by scenario")
        charts.append(_figure_to_chart(fig, "Throughput delta by scenario", out_dir / "throughput_delta_overview.png"))
        plt.close(fig)

    # Keepalive-storm completion.
    storm = [c for c in comparisons if c.name.startswith("keepalive-storm")]
    if storm:
        fig, axis = plt.subplots(figsize=(6, 4))
        names = [c.name for c in storm]
        x = range(len(names))
        base_done = [c.base.connections_completed for c in storm]
        cand_done = [c.candidate.connections_completed for c in storm]
        totals = [c.connections for c in storm]
        width = 0.35
        axis.bar([i - width / 2 for i in x], base_done, width, label="base", color="#d62728")
        axis.bar([i + width / 2 for i in x], cand_done, width, label="candidate", color="#2ca02c")
        for i, total in enumerate(totals):
            axis.axhline(total, color="gray", linestyle=":", linewidth=0.6)
        axis.set_xticks(list(x))
        axis.set_xticklabels(names, fontsize=7, rotation=15, ha="right")
        axis.set_ylabel("connections completed")
        axis.set_title("Connection-ceiling test: completed connections")
        axis.legend()
        charts.append(_figure_to_chart(fig, "Connection-ceiling test", out_dir / "keepalive_storm.png"))
        plt.close(fig)

    return charts
