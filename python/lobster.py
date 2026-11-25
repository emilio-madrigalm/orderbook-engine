"""Realism check: replay a LOBSTER sample day through the C++ book.

LOBSTER (lobsterdata.com) reconstructs Nasdaq order books from ITCH data. A sample
is a pair of CSV files without headers:

  message file    time,type,order_id,size,price,direction       (one row per event)
  orderbook file  ask_px1,ask_sz1,bid_px1,bid_sz1,...           (book AFTER that row)

  time       seconds after midnight, decimal (e.g. 34200.004241176)
  type       1 new limit order        2 partial cancel (size = qty cancelled)
             3 full delete            4 execution of a visible order
             5 execution of a hidden order    6 cross trade    7 trading halt
  price      dollars x 10000
  direction  1 = buy, -1 = sell; for executions, the side of the RESTING order
  empty book level: ask price 9999999999, bid price -9999999999, size 0

What this module does:

1. convert(): turn the messages into our event stream (src/event.hpp). Prices become
   cents (one tick = $0.01). Executions become market orders on the opposite side,
   so the C++ book has to choose which resting order fills: that is the part being
   checked. Hidden executions, crosses and halts do not change the visible book and
   are skipped.
2. Orders resting before the sample starts have no add message. The first book row
   seeds one placeholder order per visible level; any later message for an id we
   never saw is applied to the placeholder at its side and price.
3. check(): after ob_replay runs the events, compare
   - each market order's fill with the order LOBSTER says executed (same resting id,
     same price, single fill), and
   - our top-of-book depth with LOBSTER's book file after every converted message.

Stdlib only. CLI: python3 python/lobster.py --messages M.csv --orderbook O.csv
"""

from __future__ import annotations

import argparse
import csv
import json
import sys
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Dict, Iterable, Iterator, List, Optional, Sequence, Tuple, Union

sys.path.insert(0, str(Path(__file__).resolve().parent))
from replay import run_replay  # noqa: E402

EVENT_HEADER = "ts_ns,type,id,side,price,qty"
EMPTY_ASK = 9999999999
EMPTY_BID = -9999999999
PRICE_SCALE = 100           # LOBSTER units ($ x 10000) per cent
MAX_TICK = 131071           # OrderBook default band upper bound
PLACEHOLDER_BASE = 1 << 40  # ids for seeded pre-open liquidity
MARKET_BASE = 1 << 41       # ids for converted executions

Level = Tuple[int, int]  # (price ticks, qty)


class LobsterError(ValueError):
    pass


@dataclass(frozen=True)
class Message:
    time_ns: int  # nanoseconds after midnight
    type: int
    order_id: int
    size: int
    price: int  # LOBSTER units
    direction: int  # 1 buy, -1 sell


def parse_time_ns(text: str) -> int:
    """'34200.004241176' -> 34200004241176 exactly (no float rounding)."""
    whole, _, frac = text.strip().partition(".")
    if not whole.isdigit() or (frac and not frac.isdigit()):
        raise LobsterError(f"bad time {text!r}")
    return int(whole) * 1_000_000_000 + int((frac + "000000000")[:9])


def parse_message(row: Sequence[str], line: int) -> Message:
    if len(row) < 6:
        raise LobsterError(f"message line {line}: expected 6 columns, got {len(row)}")
    try:
        return Message(parse_time_ns(row[0]), int(row[1]), int(row[2]), int(row[3]),
                       int(row[4]), int(row[5]))
    except (ValueError, LobsterError) as e:
        raise LobsterError(f"message line {line}: {e}") from None


def to_ticks(price: int) -> Optional[int]:
    """LOBSTER price -> cents, or None if it is not a whole cent."""
    return price // PRICE_SCALE if price % PRICE_SCALE == 0 else None


def parse_book_row(row: Sequence[str], line: int) -> Tuple[Tuple[Level, ...], Tuple[Level, ...]]:
    """One orderbook-file row -> (bids, asks), best first, empty levels dropped."""
    if len(row) == 0 or len(row) % 4:
        raise LobsterError(f"orderbook line {line}: column count {len(row)} is not a multiple of 4")
    try:
        v = [int(x) for x in row]
    except ValueError:
        raise LobsterError(f"orderbook line {line}: non-integer field") from None
    bids: List[Level] = []
    asks: List[Level] = []
    for i in range(0, len(v), 4):
        ask_px, ask_sz, bid_px, bid_sz = v[i:i + 4]
        if ask_px != EMPTY_ASK and ask_sz > 0:
            t = to_ticks(ask_px)
            if t is None:
                raise LobsterError(f"orderbook line {line}: sub-cent ask {ask_px}")
            asks.append((t, ask_sz))
        if bid_px != EMPTY_BID and bid_sz > 0:
            t = to_ticks(bid_px)
            if t is None:
                raise LobsterError(f"orderbook line {line}: sub-cent bid {bid_px}")
            bids.append((t, bid_sz))
    return tuple(bids), tuple(asks)


@dataclass
class Event:
    ts_ns: int
    type: str  # A M C X
    id: int
    side: str  # B S -
    price: int
    qty: int

    def csv(self) -> str:
        return f"{self.ts_ns},{self.type},{self.id},{self.side},{self.price},{self.qty}"


@dataclass(frozen=True)
class ExpectedFill:
    market_id: int
    resting_id: int  # LOBSTER id, or the placeholder id standing in for it
    price: int
    qty: int


@dataclass
class ConvertStats:
    messages: int = 0
    seeded_levels: int = 0
    adds: int = 0
    cancels: int = 0
    partial_cancels: int = 0
    executions: int = 0
    skipped_hidden: int = 0
    skipped_cross: int = 0
    skipped_halt: int = 0
    skipped_subcent: int = 0
    unmatched_ids: int = 0  # cancel/execute of an id with no order and no placeholder


@dataclass
class Conversion:
    events: List[Event]
    source_row: List[int]  # 0-based message row behind each event
    expected: Dict[int, ExpectedFill]  # market order id -> what LOBSTER executed
    stats: ConvertStats


def convert(messages: Iterable[Message], first_book: Tuple[Tuple[Level, ...], Tuple[Level, ...]]) -> Conversion:
    """Convert messages[1:] given the book after messages[0] (the first book row).

    Message 0 is already reflected in the first book row, so it is not replayed.
    """
    stats = ConvertStats()
    events: List[Event] = []
    source_row: List[int] = []
    expected: Dict[int, ExpectedFill] = {}

    known: Dict[int, List] = {}  # id -> [side, price ticks, qty]
    placeholders: Dict[Tuple[str, int], List[int]] = {}  # (side, price) -> [id, qty]

    bids, asks = first_book
    next_pid = PLACEHOLDER_BASE
    for side, levels in (("B", bids), ("S", asks)):
        for px, qty in levels:
            placeholders[(side, px)] = [next_pid, qty]
            events.append(Event(0, "A", next_pid, side, px, qty))
            source_row.append(0)
            next_pid += 1
            stats.seeded_levels += 1

    next_mid = MARKET_BASE
    t0: Optional[int] = None

    def reduce(row: int, ts: int, target: List, oid: int, by: int, drop: callable) -> None:
        # target is [.., qty] with qty last; emits X or C for oid
        target[-1] -= by
        if target[-1] > 0:
            events.append(Event(ts, "X", oid, "-", 0, target[-1]))
        else:
            events.append(Event(ts, "C", oid, "-", 0, 0))
            drop()
        source_row.append(row)

    for row, m in enumerate(messages):
        stats.messages += 1
        if t0 is None:
            t0 = m.time_ns
        if row == 0:
            continue
        ts = m.time_ns - t0
        if m.type == 5:
            stats.skipped_hidden += 1
            continue
        if m.type == 6:
            stats.skipped_cross += 1
            continue
        if m.type == 7:
            stats.skipped_halt += 1
            continue
        if m.type not in (1, 2, 3, 4):
            raise LobsterError(f"message row {row}: unknown event type {m.type}")
        px = to_ticks(m.price)
        if px is None:
            stats.skipped_subcent += 1
            continue
        if px > MAX_TICK:
            raise LobsterError(f"message row {row}: price {px} cents is above the book's band ({MAX_TICK})")
        side = "B" if m.direction == 1 else "S"

        if m.type == 1:
            known[m.order_id] = [side, px, m.size]
            events.append(Event(ts, "A", m.order_id, side, px, m.size))
            source_row.append(row)
            stats.adds += 1
            continue

        # 2, 3, 4 act on an existing order: the real one if we saw its add,
        # otherwise the placeholder for pre-open liquidity at that price.
        if m.order_id in known:
            target, oid = known[m.order_id], m.order_id
            drop = lambda k=m.order_id: known.pop(k)
            is_placeholder = False
        elif (side, px) in placeholders:
            target, oid = placeholders[(side, px)], placeholders[(side, px)][0]
            drop = lambda k=(side, px): placeholders.pop(k)
            is_placeholder = True
        else:
            stats.unmatched_ids += 1
            continue

        if m.type == 4:
            mid = next_mid
            next_mid += 1
            aggressor = "S" if side == "B" else "B"
            events.append(Event(ts, "M", mid, aggressor, 0, m.size))
            source_row.append(row)
            expected[mid] = ExpectedFill(mid, oid, px, m.size)
            stats.executions += 1
            # Mirror the exchange's view of the order; the book does its own matching.
            target[-1] -= m.size
            if target[-1] <= 0:
                drop()
            continue

        if m.type == 2:
            stats.partial_cancels += 1
            reduce(row, ts, target, oid, m.size, drop)
        else:  # 3: full delete
            stats.cancels += 1
            # A placeholder stands for several pre-open orders: remove only this one.
            reduce(row, ts, target, oid, m.size if is_placeholder else target[-1], drop)

    return Conversion(events, source_row, expected, stats)


def read_messages(path: Union[str, Path]) -> Iterator[Message]:
    with open(path, newline="") as f:
        for line, row in enumerate(csv.reader(f), start=1):
            if row:
                yield parse_message(row, line)


def read_book_rows(path: Union[str, Path]) -> Iterator[Tuple[Tuple[Level, ...], Tuple[Level, ...]]]:
    with open(path, newline="") as f:
        for line, row in enumerate(csv.reader(f), start=1):
            if row:
                yield parse_book_row(row, line)


def write_events(conv: Conversion, path: Union[str, Path]) -> None:
    with open(path, "w", newline="") as f:
        f.write(EVENT_HEADER + "\n")
        for e in conv.events:
            f.write(e.csv() + "\n")


@dataclass
class CheckResult:
    executions: int = 0
    exact_fills: int = 0  # one trade, same resting id, price and qty
    wrong_order: int = 0  # right price, different resting order
    wrong_price: int = 0
    no_fill: int = 0
    split_fills: int = 0
    snapshots_compared: int = 0
    top1_match: int = 0
    top5_match: int = 0
    top10_match: int = 0
    first_top1_mismatch_row: Optional[int] = None
    rejected: int = 0
    replay_summary: str = ""
    convert: dict = field(default_factory=dict)

    def rates(self) -> dict:
        def pct(n: int, d: int) -> Optional[float]:
            return round(100.0 * n / d, 3) if d else None
        return {
            "exact_fill_pct": pct(self.exact_fills, self.executions),
            "top1_match_pct": pct(self.top1_match, self.snapshots_compared),
            "top5_match_pct": pct(self.top5_match, self.snapshots_compared),
            "top10_match_pct": pct(self.top10_match, self.snapshots_compared),
        }


def check_fills(trades_csv: Union[str, Path], expected: Dict[int, ExpectedFill], res: CheckResult) -> None:
    by_market: Dict[int, List[Tuple[int, int, int]]] = {}
    with open(trades_csv, newline="") as f:
        for r in csv.DictReader(f):
            aggr_id = int(r["buy_id"]) if r["aggressor"] == "B" else int(r["sell_id"])
            resting = int(r["sell_id"]) if r["aggressor"] == "B" else int(r["buy_id"])
            if aggr_id in expected:
                by_market.setdefault(aggr_id, []).append((resting, int(r["price"]), int(r["qty"])))
    res.executions = len(expected)
    for mid, exp in expected.items():
        fills = by_market.get(mid, [])
        if not fills:
            res.no_fill += 1
        elif len(fills) > 1:
            res.split_fills += 1
        elif fills[0][1] != exp.price:
            res.wrong_price += 1
        elif fills[0][0] != exp.resting_id:
            res.wrong_order += 1
        elif fills[0][2] == exp.qty:
            res.exact_fills += 1
        else:
            res.split_fills += 1


def _our_levels(row: Sequence[str], depth: int) -> Tuple[Tuple[Level, ...], Tuple[Level, ...]]:
    v = [int(x) for x in row[2:]]
    bids = tuple((v[2 * i], v[2 * i + 1]) for i in range(depth) if v[2 * i + 1] > 0)
    off = 2 * depth
    asks = tuple((v[off + 2 * i], v[off + 2 * i + 1]) for i in range(depth) if v[off + 2 * i + 1] > 0)
    return bids, asks


def check_depth(snapshots_csv: Union[str, Path], conv: Conversion, book_rows: Iterable, depth: int,
                res: CheckResult) -> None:
    """Compare our book after each event with LOBSTER's row for the same message.

    Several events can come from one message (never more than one today, but the
    seeding events all map to row 0), so only the LAST event of each source row is
    compared, which is when our book should equal the file.
    """
    last_event_of_row: Dict[int, int] = {}
    for i, r in enumerate(conv.source_row, start=1):  # snapshot seq is 1-based
        last_event_of_row[r] = i
    wanted = {seq: r for r, seq in last_event_of_row.items()}

    book_iter = enumerate(book_rows)
    cur_row, cur_book = -1, None

    def book_at(row: int):
        nonlocal cur_row, cur_book
        while cur_row < row:
            cur_row, cur_book = next(book_iter)
        return cur_book

    def same(a: Tuple[Level, ...], b: Tuple[Level, ...], k: int) -> bool:
        return a[:k] == b[:k]

    with open(snapshots_csv, newline="") as f:
        rd = csv.reader(f)
        next(rd)
        for row in rd:
            seq = int(row[0])
            if seq not in wanted:
                continue
            src = wanted[seq]
            lb, la = book_at(src)
            ob, oa = _our_levels(row, depth)
            res.snapshots_compared += 1
            if same(ob, lb, 1) and same(oa, la, 1):
                res.top1_match += 1
            elif res.first_top1_mismatch_row is None:
                res.first_top1_mismatch_row = src
            if same(ob, lb, 5) and same(oa, la, 5):
                res.top5_match += 1
            if same(ob, lb, 10) and same(oa, la, 10):
                res.top10_match += 1


def run_check(messages_csv: Union[str, Path], orderbook_csv: Union[str, Path], out_dir: Union[str, Path],
              *, build_dir: Union[str, Path, None] = None) -> CheckResult:
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    rows = read_book_rows(orderbook_csv)
    first = next(rows, None)
    if first is None:
        raise LobsterError("orderbook file is empty")
    with open(orderbook_csv, newline="") as f:
        first_line = f.readline()
    depth = len(first_line.strip().split(",")) // 4

    conv = convert(read_messages(messages_csv), first)
    events_csv = out_dir / "lobster_events.csv"
    write_events(conv, events_csv)
    trades_csv, snaps_csv, summary = run_replay(events_csv, out_dir, build_dir=build_dir,
                                                snapshot_every=1, depth=depth)
    res = CheckResult(replay_summary=summary, convert=asdict(conv.stats))
    for tok in summary.split():
        if tok.startswith("rejected="):
            res.rejected = int(tok.split("=", 1)[1])
    check_fills(trades_csv, conv.expected, res)
    check_depth(snaps_csv, conv, read_book_rows(orderbook_csv), depth, res)
    return res


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--messages", required=True, help="LOBSTER message file")
    ap.add_argument("--orderbook", required=True, help="LOBSTER orderbook file (same day)")
    ap.add_argument("--out-dir", default="out/lobster")
    ap.add_argument("--build-dir", default=None)
    a = ap.parse_args(argv)
    try:
        res = run_check(a.messages, a.orderbook, a.out_dir, build_dir=a.build_dir)
    except (LobsterError, OSError) as e:
        print(f"lobster.py: {e}", file=sys.stderr)
        return 1
    report = {**asdict(res), **res.rates()}
    (Path(a.out_dir) / "lobster_check.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
