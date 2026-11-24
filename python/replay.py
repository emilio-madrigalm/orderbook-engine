"""Load the C++ replayer's output into Python.

ob_replay (src/replay_main.cpp) turns an event CSV into two files:

    trades CSV:     ts_ns,buy_id,sell_id,price,qty,aggressor      (aggressor = B or S)
    snapshots CSV:  seq,ts_ns,bid_px1,bid_qty1,...,ask_px1,ask_qty1,...
                    seq = events processed so far; an empty level is written as 0,0

This module runs ob_replay, reads both files, and merges them into one
time-ordered stream for the backtester. Standard library only.

Prices stay integer ticks. The mid is kept doubled (bid + ask) so it is an
integer too; divide by 2 only for display.

Usage:
    python3 python/replay.py --events data/sample.csv --out-dir out [--build-dir build]
"""

from __future__ import annotations

import argparse
import csv
import os
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Iterator, NamedTuple, Optional, Sequence, Tuple, Union

TRADES_HEADER = ["ts_ns", "buy_id", "sell_id", "price", "qty", "aggressor"]

Level = Tuple[int, int]  # (price ticks, qty)


class Trade(NamedTuple):
    ts_ns: int
    buy_id: int
    sell_id: int
    price: int
    qty: int
    aggressor: str  # "B" or "S"


@dataclass(frozen=True)
class Snapshot:
    seq: int  # events processed when the snapshot was taken
    ts_ns: int
    bids: Tuple[Level, ...]  # best first, empty levels dropped
    asks: Tuple[Level, ...]
    depth: int = 0  # levels per side in the source file

    @property
    def best_bid(self) -> Optional[int]:
        return self.bids[0][0] if self.bids else None

    @property
    def best_ask(self) -> Optional[int]:
        return self.asks[0][0] if self.asks else None

    @property
    def two_sided(self) -> bool:
        return bool(self.bids) and bool(self.asks)

    @property
    def mid_x2(self) -> Optional[int]:
        """bid + ask in ticks (twice the mid), or None if a side is empty."""
        if not self.two_sided:
            return None
        return self.bids[0][0] + self.asks[0][0]

    @property
    def spread(self) -> Optional[int]:
        if not self.two_sided:
            return None
        return self.asks[0][0] - self.bids[0][0]

    def qty_at(self, side: str, price: int) -> Optional[int]:
        """Resting qty at `price` on side "B" or "S".

        Returns 0 if the price is inside the visible depth but has no level,
        and None if it lies beyond the visible depth (unknown).
        """
        levels = self.bids if side == "B" else self.asks
        for px, qty in levels:
            if px == price:
                return qty
        if not levels:
            return 0
        worst = levels[-1][0]
        beyond = price < worst if side == "B" else price > worst
        return None if beyond and len(levels) == self.depth else 0


class ReplayError(RuntimeError):
    pass


def _int(row: dict, key: str, path: Path, line: int) -> int:
    try:
        return int(row[key])
    except (KeyError, TypeError, ValueError):
        raise ReplayError(f"{path}:{line}: bad {key!r} value {row.get(key)!r}") from None


def read_trades(path: Union[str, Path]) -> list:
    path = Path(path)
    trades = []
    with path.open(newline="") as f:
        reader = csv.DictReader(f)
        if reader.fieldnames != TRADES_HEADER:
            raise ReplayError(f"{path}: unexpected header {reader.fieldnames}")
        for line, row in enumerate(reader, start=2):
            aggressor = row["aggressor"]
            if aggressor not in ("B", "S"):
                raise ReplayError(f"{path}:{line}: bad aggressor {aggressor!r}")
            trades.append(
                Trade(
                    _int(row, "ts_ns", path, line),
                    _int(row, "buy_id", path, line),
                    _int(row, "sell_id", path, line),
                    _int(row, "price", path, line),
                    _int(row, "qty", path, line),
                    aggressor,
                )
            )
    return trades


def _snapshot_depth(header: Sequence[str], path: Path) -> int:
    if len(header) < 2 or header[0] != "seq" or header[1] != "ts_ns" or (len(header) - 2) % 4:
        raise ReplayError(f"{path}: unexpected header {list(header)}")
    depth = (len(header) - 2) // 4
    expected = ["seq", "ts_ns"]
    for side in ("bid", "ask"):
        for i in range(1, depth + 1):
            expected += [f"{side}_px{i}", f"{side}_qty{i}"]
    if list(header) != expected:
        raise ReplayError(f"{path}: unexpected header {list(header)}")
    return depth


def read_snapshots(path: Union[str, Path]) -> list:
    path = Path(path)
    snaps = []
    with path.open(newline="") as f:
        reader = csv.reader(f)
        header = next(reader, None)
        if header is None:
            raise ReplayError(f"{path}: empty file")
        depth = _snapshot_depth(header, path)
        for line, row in enumerate(reader, start=2):
            if len(row) != len(header):
                raise ReplayError(f"{path}:{line}: expected {len(header)} fields, got {len(row)}")
            try:
                nums = [int(x) for x in row]
            except ValueError:
                raise ReplayError(f"{path}:{line}: non-integer field") from None
            seq, ts = nums[0], nums[1]
            bid_raw, ask_raw = nums[2 : 2 + 2 * depth], nums[2 + 2 * depth :]
            snaps.append(
                Snapshot(seq, ts, _levels(bid_raw), _levels(ask_raw), depth=depth)
            )
    return snaps


def _levels(raw: Sequence[int]) -> Tuple[Level, ...]:
    out = []
    for i in range(0, len(raw), 2):
        px, qty = raw[i], raw[i + 1]
        if qty == 0:
            break  # levels are written best first; the rest are padding
        out.append((px, qty))
    return tuple(out)


def merge(trades: Iterable[Trade], snapshots: Iterable[Snapshot]) -> Iterator[Union[Trade, Snapshot]]:
    """Yield trades and snapshots in time order.

    A snapshot is taken after its event is processed, so on equal timestamps
    trades come first. (Trades carry no event sequence number, so two events
    with the same ts_ns cannot be told apart; the generator's timestamps are
    strictly increasing in practice.)
    """
    t_iter, s_iter = iter(trades), iter(snapshots)
    t, s = next(t_iter, None), next(s_iter, None)
    while t is not None or s is not None:
        if s is None or (t is not None and t.ts_ns <= s.ts_ns):
            yield t
            t = next(t_iter, None)
        else:
            yield s
            s = next(s_iter, None)


def find_binary(name: str, build_dir: Union[str, Path, None] = None) -> Path:
    """Locate a built C++ tool: --build-dir, then $OB_BUILD_DIR, then ./build."""
    candidates = [build_dir, os.environ.get("OB_BUILD_DIR"), Path(__file__).resolve().parent.parent / "build"]
    for d in candidates:
        if d:
            p = Path(d) / name
            if p.is_file() and os.access(p, os.X_OK):
                return p
    raise ReplayError(f"{name} not found; build the project or pass --build-dir / set OB_BUILD_DIR")


def run_replay(
    events_csv: Union[str, Path],
    out_dir: Union[str, Path],
    *,
    build_dir: Union[str, Path, None] = None,
    snapshot_every: int = 1,
    depth: int = 5,
) -> Tuple[Path, Path, str]:
    """Run ob_replay; return (trades_csv, snapshots_csv, ob_replay's summary line)."""
    if snapshot_every < 1 or depth < 1:
        raise ValueError("snapshot_every and depth must be >= 1")
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    trades_csv, snaps_csv = out_dir / "trades.csv", out_dir / "snapshots.csv"
    cmd = [
        str(find_binary("ob_replay", build_dir)),
        "--in", str(events_csv),
        "--trades", str(trades_csv),
        "--snapshots", str(snaps_csv),
        "--snapshot-every", str(snapshot_every),
        "--depth", str(depth),
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        raise ReplayError(f"ob_replay failed ({proc.returncode}): {proc.stderr.strip()}")
    return trades_csv, snaps_csv, proc.stdout.strip()


def summarize(trades: Sequence[Trade], snapshots: Sequence[Snapshot]) -> dict:
    volume = sum(t.qty for t in trades)
    spreads = [s.spread for s in snapshots if s.two_sided]
    return {
        "trades": len(trades),
        "volume": volume,
        "vwap": (sum(t.price * t.qty for t in trades) / volume) if volume else None,
        "buy_aggressor_share": (sum(t.qty for t in trades if t.aggressor == "B") / volume) if volume else None,
        "snapshots": len(snapshots),
        "two_sided_share": (len(spreads) / len(snapshots)) if snapshots else None,
        "mean_spread": (sum(spreads) / len(spreads)) if spreads else None,
    }


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--events", required=True, help="event CSV (ob_gen output)")
    ap.add_argument("--out-dir", default="out", help="where trades.csv and snapshots.csv go")
    ap.add_argument("--build-dir", default=None, help="directory holding ob_replay")
    ap.add_argument("--snapshot-every", type=int, default=1)
    ap.add_argument("--depth", type=int, default=5)
    args = ap.parse_args(argv)
    try:
        trades_csv, snaps_csv, line = run_replay(
            args.events, args.out_dir, build_dir=args.build_dir,
            snapshot_every=args.snapshot_every, depth=args.depth,
        )
        trades, snaps = read_trades(trades_csv), read_snapshots(snaps_csv)
    except (ReplayError, ValueError, OSError) as e:
        print(f"replay.py: {e}", file=sys.stderr)
        return 1
    print(line)
    for k, v in summarize(trades, snaps).items():
        print(f"{k}={v:.4f}" if isinstance(v, float) else f"{k}={v}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
