# SPDX-License-Identifier: Apache-2.0
"""Terminal presentation: pretty comparison tables and inline Sixel charts.

All terminal features degrade gracefully:
- color is emitted only to a TTY and honours ``NO_COLOR``;
- Sixel is used only when the terminal answers the Primary Device Attributes
  probe with Sixel support (or when forced via ``--charts sixel``); otherwise the
  caller falls back to the PNG files.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

RESET = "\x1b[0m"
BOLD = "\x1b[1m"
GREEN = "\x1b[32m"
RED = "\x1b[31m"
DIM = "\x1b[2m"


# --- Capability detection -----------------------------------------------------

def _enable_windows_vt() -> None:
    """Best-effort: turn on ANSI escape processing on a Windows console."""
    if os.name != "nt":
        return
    try:
        import ctypes

        kernel32 = ctypes.windll.kernel32
        handle = kernel32.GetStdHandle(-11)  # STD_OUTPUT_HANDLE
        mode = ctypes.c_uint32()
        if kernel32.GetConsoleMode(handle, ctypes.byref(mode)):
            kernel32.SetConsoleMode(handle, mode.value | 0x0004)  # ENABLE_VIRTUAL_TERMINAL_PROCESSING
    except Exception:
        pass


def supports_color() -> bool:
    if os.environ.get("NO_COLOR") is not None:
        return False
    if not sys.stdout.isatty():
        return False
    _enable_windows_vt()
    return True


def _query_device_attributes(timeout: float = 0.35) -> str:
    """Send the DA1 query (ESC [ c) and return the raw reply (POSIX only)."""
    if os.name == "nt":
        return ""  # reliable raw stdin reads aren't portable on Windows consoles
    if not (sys.stdin.isatty() and sys.stdout.isatty()):
        return ""
    try:
        import select
        import termios
        import tty
    except Exception:
        return ""

    fd = sys.stdin.fileno()
    old = termios.tcgetattr(fd)
    try:
        tty.setraw(fd)
        sys.stdout.write("\x1b[c")
        sys.stdout.flush()
        response = ""
        deadline = time.time() + timeout
        while time.time() < deadline:
            ready, _, _ = select.select([fd], [], [], 0.05)
            if ready:
                char = os.read(fd, 1).decode("latin-1")
                response += char
                if char == "c":
                    break
        return response
    except Exception:
        return ""
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old)


def detect_sixel(mode: str) -> bool:
    """Decide whether to render Sixels for the given --charts mode."""
    if mode == "none" or mode == "png":
        return False
    if os.environ.get("NO_SIXEL") is not None and mode != "sixel":
        return False
    if mode == "sixel":
        return True  # user forces it
    # mode == "auto": probe the terminal.
    response = _query_device_attributes()
    if not response:
        return False
    # Reply looks like ESC [ ? 62 ; 4 ; ... c — Sixel is attribute "4".
    body = response.lstrip("\x1b[?").rstrip("c")
    return "4" in body.split(";")


# --- Tables -------------------------------------------------------------------

def _colorize(text: str, color: str, enabled: bool) -> str:
    return f"{color}{text}{RESET}" if enabled else text


def _delta_cell(value: float | None, improvement_is_negative: bool, color_enabled: bool) -> str:
    if value is None:
        return "n/a"
    text = f"{value:+.1f}%"
    if abs(value) < 1.0:
        return text
    improved = (value < 0) if improvement_is_negative else (value > 0)
    return _colorize(text, GREEN if improved else RED, color_enabled)


def print_environment(env: dict) -> None:
    color = supports_color()
    print(_colorize("fastcached benchmark - base vs candidate", BOLD, color))
    keys = ("os", "cpu", "cores", "python", "preset", "profile", "reps",
            "base_ref", "base_sha", "candidate_ref", "candidate_sha")
    for key in keys:
        if key in env:
            print(f"  {key:>14}: {env[key]}")
    print()


def print_comparison_table(comparisons) -> None:
    """Render the per-scenario comparison as an aligned, colored table."""
    color = supports_color()
    unicode_ok = bool(sys.stdout.encoding and "utf" in sys.stdout.encoding.lower())
    vbar = "│" if unicode_ok else "|"
    delta = "Δ" if unicode_ok else "d"

    headers = ["Scenario", "base ops/s", "cand ops/s", f"{delta} thrpt", "base p99ms",
               "cand p99ms", f"{delta} p99", "TO b/c"]
    rows: list[list[str]] = []
    plain_rows: list[list[str]] = []  # uncolored, for width calc
    for comparison in comparisons:
        base = comparison.base
        candidate = comparison.candidate
        thr_delta = comparison.throughput_delta_pct
        lat_delta = comparison.latency_delta_pct
        cells = [
            comparison.name,
            f"{base.throughput_ops_per_sec:,.0f}",
            f"{candidate.throughput_ops_per_sec:,.0f}",
            _delta_cell(thr_delta, improvement_is_negative=False, color_enabled=color),
            f"{base.latency_p99_ms:.2f}",
            f"{candidate.latency_p99_ms:.2f}",
            _delta_cell(lat_delta, improvement_is_negative=True, color_enabled=color),
            f"{base.timeouts}/{candidate.timeouts}",
        ]
        plain = [
            comparison.name,
            cells[1], cells[2],
            "n/a" if thr_delta is None else f"{thr_delta:+.1f}%",
            cells[4], cells[5],
            "n/a" if lat_delta is None else f"{lat_delta:+.1f}%",
            cells[7],
        ]
        rows.append(cells)
        plain_rows.append(plain)

    widths = [len(h) for h in headers]
    for plain in plain_rows:
        for i, cell in enumerate(plain):
            widths[i] = max(widths[i], len(cell))

    def render(cells: list[str], plain: list[str]) -> str:
        out = []
        for i, cell in enumerate(cells):
            pad = widths[i] - len(plain[i])
            out.append(("{}" + " " * pad).format(cell) if i == 0 else (" " * pad + cell))
        return f" {vbar} ".join(out)

    header_line = render(headers, headers)
    print(_colorize(header_line, BOLD, color))
    print(_colorize("-" * len(header_line), DIM, color))
    for cells, plain in zip(rows, plain_rows):
        print(render(cells, plain))
    print()


def print_multitarget_table(target_names: list[str], rows: list) -> None:
    """Print fastcached vs real servers: ops/s per target + fastcached speedup."""
    import report

    color = supports_color()
    headers = ["Scenario", *[f"{t} ops/s" for t in target_names], "fc vs best"]
    table: list[list[str]] = []
    plain: list[list[str]] = []
    for name, by_target in rows:
        cells = [name]
        plains = [name]
        for target in target_names:
            result = by_target.get(target)
            text = f"{result.throughput_ops_per_sec:,.0f}" if result is not None else "-"
            cells.append(text)
            plains.append(text)
        speedup = report.fastcached_speedup(by_target)
        if speedup is None:
            cells.append("-")
            plains.append("-")
        else:
            text = f"{speedup:.2f}x"
            plains.append(text)
            cells.append(_colorize(text, GREEN if speedup >= 1.0 else RED, color))
        table.append(cells)
        plain.append(plains)

    widths = [len(h) for h in headers]
    for prow in plain:
        for i, cell in enumerate(prow):
            widths[i] = max(widths[i], len(cell))

    def render(cells: list[str], prow: list[str]) -> str:
        out = []
        for i, cell in enumerate(cells):
            pad = widths[i] - len(prow[i])
            out.append(("{}" + " " * pad).format(cell) if i == 0 else (" " * pad + cell))
        return "  ".join(out)

    print(_colorize(render(headers, headers), BOLD, color))
    print(_colorize("-" * (sum(widths) + 2 * len(widths)), DIM, color))
    for cells, prow in zip(table, plain):
        print(render(cells, prow))
    print()


def print_highlights(lines: list[str]) -> None:
    color = supports_color()
    print(_colorize("Highlights", BOLD, color))
    for line in lines:
        print(f"  {line}")
    print()


# --- Sixel --------------------------------------------------------------------

def _sixel_via_img2sixel(png_path: Path) -> str | None:
    tool = shutil.which("img2sixel")
    if not tool:
        return None
    try:
        result = subprocess.run([tool, str(png_path)], capture_output=True, check=True)
        return result.stdout.decode("latin-1")
    except Exception:
        return None


def _rle(chars) -> str:
    import numpy as np

    if len(chars) == 0:
        return ""
    boundaries = np.nonzero(np.diff(chars))[0] + 1
    starts = np.concatenate(([0], boundaries))
    ends = np.concatenate((boundaries, [len(chars)]))
    parts = []
    for start, end in zip(starts, ends):
        run = int(end - start)
        char = chr(int(chars[start]))
        parts.append(f"!{run}{char}" if run > 3 else char * run)
    return "".join(parts)


def _encode_sixel(rgb) -> str:
    """Encode an RGB array (HxWx3 uint8) as a Sixel string (web-safe 216 palette)."""
    import numpy as np

    height, width, _ = rgb.shape
    max_width = 900
    if width > max_width:
        scale = max_width / width
        new_width = max_width
        new_height = int(height * scale)
        ys = (np.arange(new_height) / scale).astype(int).clip(0, height - 1)
        xs = (np.arange(new_width) / scale).astype(int).clip(0, width - 1)
        rgb = rgb[ys][:, xs]
        height, width = new_height, new_width

    levels = np.round(rgb.astype(np.float32) / 255.0 * 5).astype(int)  # 0..5 per channel
    index = levels[..., 0] * 36 + levels[..., 1] * 6 + levels[..., 2]  # 0..215

    out = ["\x1bPq"]
    for color in np.unique(index):
        red = (int(color) // 36) % 6
        green = (int(color) // 6) % 6
        blue = int(color) % 6
        out.append(f"#{int(color)};2;{red * 20};{green * 20};{blue * 20}")

    for band_start in range(0, height, 6):
        band = index[band_start:band_start + 6]
        rows = band.shape[0]
        segments = []
        for color in np.unique(band):
            bits = np.zeros(width, dtype=int)
            for row in range(rows):
                bits |= (band[row] == color).astype(int) << row
            segments.append(f"#{int(color)}" + _rle(bits + 63))
        out.append("$".join(segments))
        out.append("-")
    out.append("\x1b\\")
    return "".join(out)


def render_charts_to_terminal(charts) -> bool:
    """Render charts inline as Sixels. Returns True if anything was drawn."""
    drawn = False
    for chart in charts:
        sixel = _sixel_via_img2sixel(chart.path)
        if sixel is None and chart.rgb is not None:
            try:
                sixel = _encode_sixel(chart.rgb)
            except Exception:
                sixel = None
        if sixel is None:
            continue
        print(_colorize(chart.title, BOLD, supports_color()))
        sys.stdout.write(sixel)
        sys.stdout.write("\n\n")
        sys.stdout.flush()
        drawn = True
    return drawn
