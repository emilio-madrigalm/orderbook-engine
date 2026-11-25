"""Plot backtest PnL/inventory, the spread distribution and book latency as PNGs.

Inputs (each optional; a plot is made only when its file exists):
    pnl.csv           backtest.py:                  ts_ns,position,pnl,mid_x2
    snapshots.csv     ob_replay (via replay.py):    seq,ts_ns,bid_px1,...
    latency_hist.csv  ob_bench --latency-hist PATH: ns,count

Outputs in --out-dir:
    pnl_inventory.png  PnL (top) and position (bottom) on a shared time axis.
                       Two panels, not two y-axes on one panel.
    spread.png         share of two-sided snapshots at each spread, in ticks
    latency.png        per-event latency histogram up to p99.9, log count axis,
                       p50/p99/p99.9 marked; samples above p99.9 are counted
                       in a note rather than drawn

Data preparation is plain Python and is tested without matplotlib; matplotlib
is imported only when a plot is rendered (Agg backend, no display needed).

Usage:
    python3 python/plots.py --out-dir out [--pnl P] [--snapshots S] [--latency L]
"""

from __future__ import annotations

import argparse
import csv
import math
import sys
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple, Union

import replay
from backtest import Point, max_drawdown
from replay import ReplayError, Snapshot

LATENCY_HEADER = ["ns", "count"]

# Reference palette (light): one series per panel, so one hue; recessive axes.
SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK_2 = "#52514e"
GRID = "#e4e3df"
SERIES = "#2a78d6"

Hist = List[Tuple[int, int]]  # (value ns, count), ascending by value


# -- reading -----------------------------------------------------------------
def read_pnl(path: Union[str, Path]) -> List[Point]:
    """Read backtest.py's pnl.csv."""
    path = Path(path)
    points = []
    with path.open(newline="") as f:
        reader = csv.reader(f)
        header = next(reader, None)
        if header != list(Point._fields):
            raise ReplayError(f"{path}: unexpected header {header}")
        for line, row in enumerate(reader, start=2):
            try:
                ts, pos, pnl, mid_x2 = row
                points.append(Point(int(ts), int(pos), float(pnl), int(mid_x2)))
            except ValueError:
                raise ReplayError(f"{path}:{line}: bad row {row}") from None
    return points


def read_latency_hist(path: Union[str, Path]) -> Hist:
    """Read ob_bench's histogram: values strictly ascending, counts positive."""
    path = Path(path)
    hist: Hist = []
    with path.open(newline="") as f:
        reader = csv.reader(f)
        header = next(reader, None)
        if header != LATENCY_HEADER:
            raise ReplayError(f"{path}: unexpected header {header}")
        for line, row in enumerate(reader, start=2):
            try:
                ns, count = (int(x) for x in row)
            except ValueError:
                raise ReplayError(f"{path}:{line}: bad row {row}") from None
            if ns < 0 or count <= 0 or (hist and ns <= hist[-1][0]):
                raise ReplayError(f"{path}:{line}: values must ascend, counts must be > 0")
            hist.append((ns, count))
    if not hist:
        raise ReplayError(f"{path}: no samples")
    return hist


# -- data preparation (no matplotlib) ----------------------------------------
def hist_total(hist: Hist) -> int:
    return sum(c for _, c in hist)


def hist_percentile(hist: Hist, p: float) -> int:
    """Nearest-rank percentile, the same rule ob_bench prints."""
    n = hist_total(hist)
    rank = min(max(math.ceil(p * n), 1), n)
    seen = 0
    for value, count in hist:
        seen += count
        if seen >= rank:
            return value
    raise AssertionError("unreachable: rank <= total")


def bin_hist(hist: Hist, width: int, upper: int) -> Tuple[List[int], List[int], int]:
    """Group values <= upper into bins [k*width, (k+1)*width).

    Returns (bin left edges, counts, number of samples above upper).
    """
    if width < 1 or upper < 0:
        raise ValueError("width must be >= 1 and upper >= 0")
    counts = [0] * (upper // width + 1)
    beyond = 0
    for value, count in hist:
        if value > upper:
            beyond += count
        else:
            counts[value // width] += count
    return [k * width for k in range(len(counts))], counts, beyond


def spread_counts(snapshots: Sequence[Snapshot]) -> Dict[int, int]:
    """Number of two-sided snapshots at each spread (ticks), ascending."""
    out: Dict[int, int] = {}
    for s in snapshots:
        if s.two_sided:
            out[s.spread] = out.get(s.spread, 0) + 1
    return dict(sorted(out.items()))


# -- rendering ---------------------------------------------------------------
def _pyplot():
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    return plt


def _style(ax) -> None:
    ax.set_facecolor(SURFACE)
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color(INK_2)
        ax.spines[side].set_linewidth(0.8)
    ax.tick_params(colors=INK_2, labelsize=9)
    ax.yaxis.label.set_color(INK_2)
    ax.xaxis.label.set_color(INK_2)
    ax.grid(axis="y", color=GRID, linewidth=0.6)
    ax.set_axisbelow(True)


def _figure(plt, nrows=1, **kw):
    fig, axes = plt.subplots(nrows, 1, figsize=(9, 5.2), facecolor=SURFACE, **kw)
    for ax in (axes if nrows > 1 else [axes]):
        _style(ax)
    return fig, axes


def _title(fig, title: str, subtitle: str) -> None:
    fig.text(0.06, 0.965, title, fontsize=13, color=INK, weight="bold", va="top")
    fig.text(0.06, 0.915, subtitle, fontsize=9.5, color=INK_2, va="top")


def plot_pnl_inventory(points: Sequence[Point], path: Union[str, Path]) -> None:
    if not points:
        raise ValueError("no PnL points to plot")
    plt = _pyplot()
    t0 = points[0].ts_ns
    t_ms = [(p.ts_ns - t0) / 1e6 for p in points]
    fig, (top, bottom) = _figure(plt, 2, sharex=True, gridspec_kw={"height_ratios": [2, 1]})
    top.axhline(0, color=INK_2, linewidth=0.8)
    top.plot(t_ms, [p.pnl for p in points], color=SERIES, linewidth=1.5)
    top.set_ylabel("PnL (ticks)")
    bottom.axhline(0, color=INK_2, linewidth=0.8)
    bottom.plot(t_ms, [p.position for p in points], color=SERIES, linewidth=1.2,
                drawstyle="steps-post")
    bottom.set_ylabel("Position (units)")
    bottom.set_xlabel("Time since first two-sided snapshot (ms)")
    final, dd = points[-1].pnl, max_drawdown([p.pnl for p in points])
    _title(fig, "Market maker backtest",
           f"Marked to mid. Final PnL {final:,.1f} ticks, max drawdown {dd:,.1f} ticks.")
    fig.subplots_adjust(top=0.85, left=0.1, right=0.97, hspace=0.12)
    fig.savefig(path, dpi=150, facecolor=SURFACE)
    plt.close(fig)


def plot_spread(counts: Dict[int, int], path: Union[str, Path]) -> None:
    total = sum(counts.values())
    if total == 0:
        raise ValueError("no two-sided snapshots to plot")
    plt = _pyplot()
    xs = list(counts)
    shares = [100 * counts[x] / total for x in xs]
    fig, ax = _figure(plt)
    ax.bar(xs, shares, width=0.7, color=SERIES)
    for x, s in zip(xs, shares):  # label only bars big enough to read
        if s >= 1.0:
            ax.text(x, s, f"{s:.1f}%", ha="center", va="bottom", fontsize=8.5, color=INK_2)
    ax.set_xlabel("Spread (ticks)")
    ax.set_ylabel("Share of snapshots (%)")
    ax.set_xticks(xs if len(xs) <= 20 else ax.get_xticks())
    mean = sum(x * c for x, c in counts.items()) / total
    _title(fig, "Bid-ask spread",
           f"{total:,} two-sided snapshots, mean spread {mean:.2f} ticks.")
    fig.subplots_adjust(top=0.84, left=0.1, right=0.97)
    fig.savefig(path, dpi=150, facecolor=SURFACE)
    plt.close(fig)


def plot_latency(hist: Hist, path: Union[str, Path], bins: int = 60) -> None:
    plt = _pyplot()
    p50, p99, p999 = (hist_percentile(hist, p) for p in (0.50, 0.99, 0.999))
    width = max(1, math.ceil((p999 + 1) / bins))
    lefts, counts, beyond = bin_hist(hist, width, p999)
    fig, ax = _figure(plt)
    ax.bar(lefts, counts, width=width * 0.85, align="edge", color=SERIES)
    ax.set_yscale("log")
    right = (p999 // width + 1) * width
    ax.set_xlim(0, right)
    top = max(counts) * 3
    ax.set_ylim(0.7, top)
    for value, name in ((p50, "p50"), (p99, "p99"), (p999, "p99.9")):
        ax.axvline(value, color=INK, linewidth=0.8, linestyle=(0, (3, 3)))
        near_edge = value > 0.8 * right  # put the label left of the line
        ax.text(value, top * 0.85, f" {name} {value} ns ", fontsize=8.5, color=INK, va="top",
                ha="right" if near_edge else "left")
    ax.set_xlabel(f"Latency per event (ns, bins of {width} ns)")
    ax.set_ylabel("Events (log scale)")
    _title(fig, "Order book latency per event",
           f"{hist_total(hist):,} events. {beyond:,} above p99.9 not drawn "
           f"(max {hist[-1][0]:,} ns).")
    fig.subplots_adjust(top=0.84, left=0.1, right=0.97)
    fig.savefig(path, dpi=150, facecolor=SURFACE)
    plt.close(fig)


# -- CLI ---------------------------------------------------------------------
def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--out-dir", default="out", help="where inputs default from and PNGs go")
    ap.add_argument("--pnl", default=None, help="default: OUT_DIR/pnl.csv")
    ap.add_argument("--snapshots", default=None, help="default: OUT_DIR/snapshots.csv")
    ap.add_argument("--latency", default=None, help="default: OUT_DIR/latency_hist.csv")
    args = ap.parse_args(argv)
    out = Path(args.out_dir)
    jobs = [
        (args.pnl or out / "pnl.csv", "pnl_inventory.png",
         lambda p, dst: plot_pnl_inventory(read_pnl(p), dst)),
        (args.snapshots or out / "snapshots.csv", "spread.png",
         lambda p, dst: plot_spread(spread_counts(replay.read_snapshots(p)), dst)),
        (args.latency or out / "latency_hist.csv", "latency.png",
         lambda p, dst: plot_latency(read_latency_hist(p), dst)),
    ]
    made = 0
    try:
        out.mkdir(parents=True, exist_ok=True)
        for src, name, render in jobs:
            if Path(src).is_file():
                render(src, out / name)
                print(out / name)
                made += 1
    except ImportError as e:
        print(f"plots.py: matplotlib is needed to render ({e})", file=sys.stderr)
        return 1
    except (ReplayError, ValueError, OSError) as e:
        print(f"plots.py: {e}", file=sys.stderr)
        return 1
    if made == 0:
        print("plots.py: no input files found; run backtest.py / ob_bench first", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
