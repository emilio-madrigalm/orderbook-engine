"""Backtest a quoting strategy against the C++ replayer's output.

The strategy's orders are simulated next to the replayed book; they are never
sent into it, so they have no market impact. On every snapshot the strategy
returns the bid and ask it wants; the simulator turns changes into order
requests and decides fills from the replayed trades.

Fill model (all simplifications are deliberate and listed here):
  * Latency. A new, changed or cancelled quote takes effect latency_ns after
    the snapshot that asked for it. Until then the previous order stays live
    and can still fill.
  * Queue position. A new order joins the back of its price level: the queue
    ahead is the visible qty at that price in the latest snapshot. Trades at
    our price use up the queue ahead first; only the overflow fills us. When
    a snapshot shows less qty at our price than our queue ahead (cancels), the
    queue ahead drops to match, as if the cancels were all in front of us.
  * Trade-through. A trade at a worse price than ours (below our bid, above
    our ask) means our level was emptied, so we fill up to the trade's qty
    regardless of queue. With require_trade_through=True this is the only
    way to fill (the most conservative setting).
  * Fills are passive only, at our limit price. An order that would cross the
    book when it arrives is dropped (counted, never filled as a taker). An
    order priced beyond the visible depth has an unknown queue and is dropped
    too (counted).
  * One order per side. If fills arrive while a replacement is in flight, the
    replacement shrinks by the same amount, so the strategy's position limit
    still holds after it lands.
  * PnL is marked to the mid on every two-sided snapshot: cash + position *
    mid - fees. No liquidation cost at the end.

Prices are integer ticks; cash is integer (ticks * qty); fees and PnL are
floats. Standard library only.

Usage:
    python3 python/backtest.py --events data/sample.csv --out-dir out [--build-dir build]
"""

from __future__ import annotations

import argparse
import csv
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable, List, NamedTuple, Optional, Protocol, Sequence, Union

import replay
from replay import ReplayError, Snapshot, Trade
from strategy import MarketMaker, MMConfig, Quote, Quotes


class Strategy(Protocol):
    def quote(self, snapshot: Snapshot, position: int) -> Quotes: ...


@dataclass(frozen=True)
class BacktestConfig:
    latency_ns: int = 5_000  # the generator's default mean gap is 1 us
    fee_per_unit: float = 0.0  # ticks per unit filled; negative = maker rebate
    require_trade_through: bool = False

    def __post_init__(self):
        if self.latency_ns < 0:
            raise ValueError("latency_ns must be >= 0")


class Fill(NamedTuple):
    ts_ns: int
    side: str  # "B" = we bought, "S" = we sold
    price: int
    qty: int


class Point(NamedTuple):
    ts_ns: int
    position: int
    pnl: float
    mid_x2: int


@dataclass
class _Order:
    price: int
    qty: int
    queue_ahead: int


@dataclass
class _Pending:
    active_at: int
    quote: Optional[Quote]  # None = cancel
    position_at_request: int


@dataclass
class _Side:
    side: str  # "B" or "S"
    live: Optional[_Order] = None
    pending: Optional[_Pending] = None

    def intended(self) -> Optional[Quote]:
        """What this side will look like once in-flight requests land."""
        if self.pending is not None:
            return self.pending.quote
        return Quote(self.live.price, self.live.qty) if self.live else None


@dataclass
class BacktestResult:
    fills: List[Fill] = field(default_factory=list)
    series: List[Point] = field(default_factory=list)
    position: int = 0
    cash: int = 0
    fees: float = 0.0
    max_abs_position: int = 0
    dropped_crossing: int = 0
    dropped_unknown_queue: int = 0
    last_mid_x2: Optional[int] = None

    @property
    def pnl(self) -> float:
        mark = self.position * self.last_mid_x2 / 2 if self.last_mid_x2 is not None else 0.0
        return self.cash + mark - self.fees

    @property
    def max_drawdown(self) -> float:
        return max_drawdown([p.pnl for p in self.series])

    @property
    def volume(self) -> int:
        return sum(f.qty for f in self.fills)

    def summary(self) -> dict:
        return {
            "fills": len(self.fills),
            "volume": self.volume,
            "final_position": self.position,
            "max_abs_position": self.max_abs_position,
            "fees": self.fees,
            "pnl": self.pnl,
            "max_drawdown": self.max_drawdown,
            "dropped_crossing": self.dropped_crossing,
            "dropped_unknown_queue": self.dropped_unknown_queue,
        }


def max_drawdown(pnl: Sequence[float]) -> float:
    """Largest peak-to-trough drop in a PnL series (0 if it never falls)."""
    peak, worst = float("-inf"), 0.0
    for v in pnl:
        peak = max(peak, v)
        worst = max(worst, peak - v)
    return worst


class _Sim:
    def __init__(self, strategy: Strategy, config: BacktestConfig):
        self.strategy = strategy
        self.config = config
        self.sides = {"B": _Side("B"), "S": _Side("S")}
        self.last_snap: Optional[Snapshot] = None
        self.r = BacktestResult()

    # -- order requests ------------------------------------------------------
    def activate(self, ts: int) -> None:
        for s in self.sides.values():
            p = s.pending
            if p is None or p.active_at > ts:
                continue
            s.pending = None
            s.live = None
            if p.quote is None:
                continue
            # fills since the request shrink the replacement (keeps the limit)
            moved = self.r.position - p.position_at_request
            toward = moved if s.side == "B" else -moved
            qty = p.quote.qty - max(0, toward)
            if qty <= 0:
                continue
            snap = self.last_snap
            if s.side == "B" and snap.best_ask is not None and p.quote.price >= snap.best_ask:
                self.r.dropped_crossing += 1
                continue
            if s.side == "S" and snap.best_bid is not None and p.quote.price <= snap.best_bid:
                self.r.dropped_crossing += 1
                continue
            ahead = snap.qty_at(s.side, p.quote.price)
            if ahead is None:
                self.r.dropped_unknown_queue += 1
                continue
            s.live = _Order(p.quote.price, qty, ahead)

    def request(self, ts: int, quotes: Quotes) -> None:
        for s, want in ((self.sides["B"], quotes.bid), (self.sides["S"], quotes.ask)):
            if want != s.intended():
                s.pending = _Pending(ts + self.config.latency_ns, want, self.r.position)

    # -- market data ---------------------------------------------------------
    def on_trade(self, t: Trade) -> None:
        s = self.sides["B" if t.aggressor == "S" else "S"]  # side the aggressor hits
        o = s.live
        if o is None:
            return
        through = t.price < o.price if s.side == "B" else t.price > o.price
        if through:
            qty = min(o.qty, t.qty)
        elif t.price == o.price and not self.config.require_trade_through:
            used = min(o.queue_ahead, t.qty)
            o.queue_ahead -= used
            qty = min(o.qty, t.qty - used)
        else:
            return
        if qty > 0:
            self.fill(t.ts_ns, s, qty)

    def fill(self, ts: int, s: _Side, qty: int) -> None:
        o = s.live
        sign = 1 if s.side == "B" else -1
        self.r.position += sign * qty
        self.r.cash -= sign * o.price * qty
        self.r.fees += self.config.fee_per_unit * qty
        self.r.max_abs_position = max(self.r.max_abs_position, abs(self.r.position))
        self.r.fills.append(Fill(ts, s.side, o.price, qty))
        o.qty -= qty
        if o.qty == 0:
            s.live = None

    def on_snapshot(self, snap: Snapshot) -> None:
        self.last_snap = snap
        for s in self.sides.values():
            if s.live is not None:
                level = snap.qty_at(s.side, s.live.price)
                if level is not None:
                    s.live.queue_ahead = min(s.live.queue_ahead, level)
        mid_x2 = snap.mid_x2
        if mid_x2 is not None:
            self.r.last_mid_x2 = mid_x2
            self.r.series.append(Point(snap.ts_ns, self.r.position, self.r.pnl, mid_x2))
        self.request(snap.ts_ns, self.strategy.quote(snap, self.r.position))


def backtest(
    stream: Iterable[Union[Trade, Snapshot]],
    strategy: Strategy,
    config: BacktestConfig = BacktestConfig(),
) -> BacktestResult:
    """Run `strategy` over a time-ordered stream (see replay.merge)."""
    sim = _Sim(strategy, config)
    for item in stream:
        if sim.last_snap is not None:
            sim.activate(item.ts_ns)
        if isinstance(item, Snapshot):
            sim.on_snapshot(item)
        else:
            sim.on_trade(item)
    return sim.r


def write_csv(path: Path, header: Sequence[str], rows: Iterable[Sequence]) -> None:
    with path.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(header)
        w.writerows(rows)


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--events", required=True, help="event CSV (ob_gen output)")
    ap.add_argument("--out-dir", default="out")
    ap.add_argument("--build-dir", default=None, help="directory holding ob_replay")
    ap.add_argument("--depth", type=int, default=5, help="snapshot depth per side")
    ap.add_argument("--half-spread", type=int, default=1)
    ap.add_argument("--size", type=int, default=10)
    ap.add_argument("--max-position", type=int, default=100)
    ap.add_argument("--skew-step", type=int, default=10)
    ap.add_argument("--latency-ns", type=int, default=5_000)
    ap.add_argument("--fee", type=float, default=0.0, help="ticks per unit filled")
    ap.add_argument("--trade-through-only", action="store_true")
    args = ap.parse_args(argv)
    try:
        mm = MarketMaker(MMConfig(args.half_spread, args.size, args.max_position, args.skew_step))
        cfg = BacktestConfig(args.latency_ns, args.fee, args.trade_through_only)
        trades_csv, snaps_csv, _ = replay.run_replay(
            args.events, args.out_dir, build_dir=args.build_dir, depth=args.depth
        )
        stream = replay.merge(replay.read_trades(trades_csv), replay.read_snapshots(snaps_csv))
        r = backtest(stream, mm, cfg)
    except (ReplayError, ValueError, OSError) as e:
        print(f"backtest.py: {e}", file=sys.stderr)
        return 1
    out = Path(args.out_dir)
    write_csv(out / "fills.csv", Fill._fields, r.fills)
    write_csv(out / "pnl.csv", Point._fields, r.series)
    for k, v in r.summary().items():
        print(f"{k}={v:.2f}" if isinstance(v, float) else f"{k}={v}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
