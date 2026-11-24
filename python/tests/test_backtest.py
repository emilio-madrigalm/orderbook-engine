"""Tests for python/backtest.py. Run: python3 -m unittest discover -s python/tests

The fill model is tested with a scripted strategy (a stub that returns fixed
quotes), so each test controls exactly which orders exist and when. The
end-to-end test needs a built ob_gen/ob_replay and is skipped otherwise.
"""

import math
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import replay  # noqa: E402
from backtest import BacktestConfig, Fill, backtest, max_drawdown  # noqa: E402
from replay import ReplayError, Snapshot, Trade  # noqa: E402
from strategy import MarketMaker, MMConfig, Quote, Quotes  # noqa: E402

NONE = Quotes(None, None)
BIDS = ((100, 30), (99, 20), (98, 10))
ASKS = ((102, 25), (103, 20), (104, 10))


def book(ts, bids=BIDS, asks=ASKS, depth=5):
    return Snapshot(seq=ts, ts_ns=ts, bids=tuple(bids), asks=tuple(asks), depth=depth)


def sell(ts, px, qty):  # sell aggressor: hits bids
    return Trade(ts, 0, 0, px, qty, "S")


def buy(ts, px, qty):  # buy aggressor: lifts asks
    return Trade(ts, 0, 0, px, qty, "B")


def bid(px, qty=10):
    return Quotes(Quote(px, qty), None)


class Scripted:
    """Stub strategy: switches to plan[ts] at that snapshot, else repeats."""

    def __init__(self, plan):
        self.plan = dict(plan)
        self.current = NONE
        self.calls = []

    def quote(self, snapshot, position):
        self.calls.append((snapshot.ts_ns, position))
        self.current = self.plan.get(snapshot.ts_ns, self.current)
        return self.current


def run(stream, plan, **cfg):
    strat = Scripted(plan)
    return backtest(stream, strat, BacktestConfig(**cfg)), strat


class ConfigTest(unittest.TestCase):
    def test_rejects_negative_latency(self):
        with self.assertRaises(ValueError):
            BacktestConfig(latency_ns=-1)


class LatencyTest(unittest.TestCase):
    def test_order_is_not_live_until_latency_passes(self):
        stream = [book(0), sell(5, 99, 50), book(10), sell(11, 99, 50)]
        r, _ = run(stream, {0: bid(100)}, latency_ns=10)
        self.assertEqual(r.fills, [Fill(11, "B", 100, 10)])

    def test_cancel_also_waits_for_latency(self):
        stream = [book(0), book(10), sell(15, 99, 5), sell(25, 99, 5)]
        r, _ = run(stream, {0: bid(100), 10: NONE}, latency_ns=10)
        self.assertEqual(r.fills, [Fill(15, "B", 100, 5)])


class QueueTest(unittest.TestCase):
    def test_trades_at_our_price_consume_queue_ahead_first(self):
        stream = [book(0), sell(1, 100, 20), sell(2, 100, 15)]
        r, _ = run(stream, {0: bid(100)}, latency_ns=0)
        self.assertEqual(r.fills, [Fill(2, "B", 100, 5)])  # 30 ahead, 35 traded

    def test_shrinking_level_shrinks_queue_ahead(self):
        stream = [book(0), book(1, bids=((100, 10),)), sell(2, 100, 12)]
        r, _ = run(stream, {0: bid(100)}, latency_ns=0)
        self.assertEqual(r.fills, [Fill(2, "B", 100, 2)])

    def test_unchanged_quote_keeps_its_place(self):
        # level grows to 40 behind us; an unchanged quote must not re-queue
        stream = [book(0), sell(1, 100, 20), book(2, bids=((100, 40),)), sell(3, 100, 12)]
        r, _ = run(stream, {0: bid(100)}, latency_ns=0)
        self.assertEqual(r.fills, [Fill(3, "B", 100, 2)])

    def test_price_change_requeues_at_new_level(self):
        stream = [book(0), book(2), sell(3, 99, 25)]
        r, _ = run(stream, {0: bid(100), 2: bid(99)}, latency_ns=0)
        self.assertEqual(r.fills, [Fill(3, "B", 99, 5)])  # 20 ahead at 99

    def test_require_trade_through_ignores_trades_at_our_price(self):
        stream = [book(0, bids=((100, 1),)), sell(1, 100, 100), sell(2, 99, 3)]
        r, _ = run(stream, {0: bid(100)}, latency_ns=0, require_trade_through=True)
        self.assertEqual(r.fills, [Fill(2, "B", 100, 3)])


class FillTest(unittest.TestCase):
    def test_trade_through_fills_up_to_trade_qty_ignoring_queue(self):
        stream = [book(0), sell(1, 99, 4), sell(2, 99, 50), sell(3, 99, 50)]
        r, _ = run(stream, {0: bid(100)}, latency_ns=0)
        self.assertEqual(r.fills, [Fill(1, "B", 100, 4), Fill(2, "B", 100, 6)])
        self.assertEqual((r.position, r.cash), (10, -1000))

    def test_buy_aggressor_fills_ask_only(self):
        both = Quotes(Quote(100, 10), Quote(102, 10))
        stream = [book(0), buy(1, 103, 50)]
        r, _ = run(stream, {0: both}, latency_ns=0)
        self.assertEqual(r.fills, [Fill(1, "S", 102, 10)])
        self.assertEqual((r.position, r.cash), (-10, 1020))

    def test_strategy_sees_updated_position(self):
        stream = [book(0), sell(1, 99, 4), book(2)]
        _, strat = run(stream, {0: bid(100)}, latency_ns=0)
        self.assertEqual(strat.calls, [(0, 0), (2, 4)])

    def test_replacement_is_clipped_by_fills_during_latency(self):
        # bid 100x10 live at pos 0; replacement 99x10 requested at pos 0;
        # the old bid fills before the replacement lands, so it shrinks to 0
        stream = [book(0), book(10), sell(15, 98, 50), book(20), sell(25, 97, 50)]
        r, _ = run(stream, {0: bid(100), 10: bid(99)}, latency_ns=10)
        self.assertEqual(r.fills, [Fill(15, "B", 100, 10)])
        self.assertEqual(r.position, 10)

    def test_order_that_would_cross_on_arrival_is_dropped(self):
        stream = [book(0), sell(1, 101, 5)]
        r, _ = run(stream, {0: bid(102)}, latency_ns=0)
        self.assertEqual(r.fills, [])
        self.assertEqual(r.dropped_crossing, 1)

    def test_order_beyond_visible_depth_is_dropped(self):
        stream = [book(0, depth=3), sell(1, 96, 50)]
        r, _ = run(stream, {0: bid(97)}, latency_ns=0)
        self.assertEqual(r.fills, [])
        self.assertEqual(r.dropped_unknown_queue, 1)


class PnlTest(unittest.TestCase):
    def test_fees_and_mark_to_market(self):
        stream = [book(0), sell(1, 99, 10), book(2, bids=((101, 5),), asks=((103, 5),))]
        r, _ = run(stream, {0: bid(100), 2: NONE}, latency_ns=0, fee_per_unit=0.5)
        self.assertEqual(r.fees, 5.0)
        self.assertEqual(r.series[-1].pnl, -1000 + 10 * 102 - 5.0)
        self.assertEqual(r.pnl, 15.0)

    def test_drawdown_peak_to_trough(self):
        mids = [(101, 103), (96, 98), (98, 100)]  # mid 102, 97, 99
        stream = [book(0), sell(1, 99, 10)] + [
            book(2 + i, bids=((b, 5),), asks=((a, 5),)) for i, (b, a) in enumerate(mids)
        ]
        r, _ = run(stream, {0: bid(100), 2: NONE}, latency_ns=0)
        self.assertEqual([p.pnl for p in r.series], [0.0, 20.0, -30.0, -10.0])
        self.assertEqual(r.max_drawdown, 50.0)

    def test_max_drawdown_helper(self):
        self.assertEqual(max_drawdown([]), 0.0)
        self.assertEqual(max_drawdown([1.0, 2.0, 3.0]), 0.0)
        self.assertEqual(max_drawdown([0.0, 5.0, -1.0, 4.0, -2.0]), 7.0)

    def test_one_sided_snapshot_is_not_marked(self):
        stream = [book(0), book(1, asks=())]
        r, _ = run(stream, {}, latency_ns=0)
        self.assertEqual(len(r.series), 1)


class EndToEndTest(unittest.TestCase):
    def setUp(self):
        try:
            self.gen = replay.find_binary("ob_gen")
            replay.find_binary("ob_replay")
        except ReplayError as e:
            self.skipTest(str(e))

    def _run(self, events, out):
        trades_csv, snaps_csv, _ = replay.run_replay(events, out)
        stream = replay.merge(replay.read_trades(trades_csv), replay.read_snapshots(snaps_csv))
        mm = MarketMaker(MMConfig(half_spread=1, size=5, max_position=50, skew_step=10))
        return backtest(stream, mm, BacktestConfig(latency_ns=5_000, fee_per_unit=0.1))

    def test_market_maker_on_generated_stream(self):
        with tempfile.TemporaryDirectory() as d:
            events = Path(d) / "events.csv"
            subprocess.run(
                [str(self.gen), "--events", "20000", "--seed", "7", "--out", str(events)],
                check=True, capture_output=True,
            )
            r1 = self._run(events, Path(d) / "a")
            r2 = self._run(events, Path(d) / "b")
        self.assertGreater(len(r1.fills), 0)
        self.assertLessEqual(r1.max_abs_position, 50)
        self.assertTrue(math.isfinite(r1.pnl))
        self.assertEqual((r1.fills, r1.pnl), (r2.fills, r2.pnl))  # deterministic


if __name__ == "__main__":
    unittest.main()
