"""Realism check: replay one symbol's Nasdaq ITCH day through the C++ book.

Step 1 (C++, src/itch_main.cpp) turns the raw ITCH 5.0 file into an event stream:

    zcat 01302020.NASDAQ_ITCH50.gz | build/ob_itch --symbol AAPL --out-dir out/itch

Step 2 (this module) runs ob_replay on those events and compares, for every event:

  fills  every Nasdaq execution became a market order; did the C++ book fill it
         from the same resting order, at the same price, in one trade?
  depth  is the C++ book's top-N depth identical to the book built directly from
         the ITCH messages (expected_depth.csv)?
  extra  trades the book made that Nasdaq did not (an add that crossed our book)

    python3 python/itch_check.py --dir out/itch [--build-dir build]

Writes DIR/itch_check.json and prints it. Stdlib only.
"""

from __future__ import annotations

import argparse
import csv
import json
import sys
from dataclasses import asdict, dataclass, field
from itertools import zip_longest
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple, Union

sys.path.insert(0, str(Path(__file__).resolve().parent))
from replay import ReplayError, run_replay  # noqa: E402

MARKET_BASE = 1 << 62  # ob::itch::kMarketIdBase


class ItchCheckError(ValueError):
    pass


@dataclass
class CheckResult:
    symbol: str = ""
    events: int = 0
    executions: int = 0
    exact_fills: int = 0   # one trade, same resting order, price and qty
    wrong_order: int = 0   # right price, a different resting order
    wrong_price: int = 0
    split_fills: int = 0   # more than one trade, or the wrong qty
    no_fill: int = 0
    extra_trades: int = 0  # trades not caused by a converted execution
    snapshots_compared: int = 0
    depth_match: int = 0
    first_depth_mismatch_seq: Optional[int] = None
    first_bad_fill: Optional[dict] = None
    rejected: int = 0
    replay_summary: str = ""
    convert: Dict[str, str] = field(default_factory=dict)

    def rates(self) -> dict:
        def pct(n: int, d: int) -> Optional[float]:
            return round(100.0 * n / d, 4) if d else None
        return {"exact_fill_pct": pct(self.exact_fills, self.executions),
                "depth_match_pct": pct(self.depth_match, self.snapshots_compared)}


def read_meta(path: Union[str, Path]) -> Dict[str, str]:
    try:
        text = Path(path).read_text().split()
    except OSError as e:
        raise ItchCheckError(f"cannot read {path}: {e}") from None
    meta = dict(tok.split("=", 1) for tok in text if "=" in tok)
    for key in ("symbol", "depth", "snapshot_every"):
        if key not in meta:
            raise ItchCheckError(f"{path}: missing {key}")
    return meta


def read_expected(path: Union[str, Path]) -> Dict[int, Tuple[int, int, int]]:
    """market id -> (resting id, price, qty)."""
    out: Dict[int, Tuple[int, int, int]] = {}
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            out[int(r["market_id"])] = (int(r["resting_id"]), int(r["price"]), int(r["qty"]))
    return out


def check_fills(trades_csv: Union[str, Path], expected: Dict[int, Tuple[int, int, int]], res: CheckResult) -> None:
    got: Dict[int, List[Tuple[int, int, int]]] = {}
    with open(trades_csv, newline="") as f:
        for r in csv.DictReader(f):
            buy, sell = int(r["buy_id"]), int(r["sell_id"])
            aggr, resting = (buy, sell) if r["aggressor"] == "B" else (sell, buy)
            if aggr in expected:
                got.setdefault(aggr, []).append((resting, int(r["price"]), int(r["qty"])))
            else:
                res.extra_trades += 1
    res.executions = len(expected)
    for mid in sorted(expected):
        want = expected[mid]
        fills = got.get(mid, [])
        if len(fills) == 1 and fills[0] == want:
            res.exact_fills += 1
            continue
        if not fills:
            res.no_fill += 1
        elif len(fills) > 1 or fills[0][2] != want[2]:
            res.split_fills += 1
        elif fills[0][1] != want[1]:
            res.wrong_price += 1
        else:
            res.wrong_order += 1
        if res.first_bad_fill is None:
            res.first_bad_fill = {"market_id": mid, "expected": list(want), "got": [list(x) for x in fills]}


def check_depth(ours_csv: Union[str, Path], expected_csv: Union[str, Path], res: CheckResult) -> None:
    """Both files have the same header and one row per sampled event: compare row by row."""
    with open(ours_csv) as a, open(expected_csv) as b:
        if a.readline() != b.readline():
            raise ItchCheckError("snapshot headers differ (depth mismatch between runs?)")
        for la, lb in zip_longest(a, b):
            if la is None or lb is None:
                raise ItchCheckError("snapshot files have different row counts")
            res.snapshots_compared += 1
            if la == lb:
                res.depth_match += 1
            elif res.first_depth_mismatch_seq is None:
                res.first_depth_mismatch_seq = int(lb.split(",", 1)[0])


def run_check(dir_: Union[str, Path], *, build_dir: Union[str, Path, None] = None) -> CheckResult:
    d = Path(dir_)
    meta = read_meta(d / "itch_meta.txt")
    depth, every = int(meta["depth"]), int(meta["snapshot_every"])
    if depth < 1 or every < 1:
        raise ItchCheckError("ob_itch was run with depth or snapshot_every = 0; nothing to compare")
    replay_dir = d / "replay"
    trades_csv, snaps_csv, summary = run_replay(d / "events.csv", replay_dir, build_dir=build_dir,
                                                snapshot_every=every, depth=depth)
    res = CheckResult(symbol=meta["symbol"], replay_summary=summary, convert=meta)
    for tok in summary.split():
        k, _, v = tok.partition("=")
        if k == "rejected":
            res.rejected = int(v)
        elif k == "events":
            res.events = int(v)
    check_fills(trades_csv, read_expected(d / "expected_fills.csv"), res)
    check_depth(snaps_csv, d / "expected_depth.csv", res)
    return res


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--dir", required=True, help="ob_itch --out-dir")
    ap.add_argument("--build-dir", default=None)
    a = ap.parse_args(argv)
    try:
        res = run_check(a.dir, build_dir=a.build_dir)
    except (ItchCheckError, ReplayError, OSError) as e:
        print(f"itch_check.py: {e}", file=sys.stderr)
        return 1
    report = {**asdict(res), **res.rates()}
    (Path(a.dir) / "itch_check.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
