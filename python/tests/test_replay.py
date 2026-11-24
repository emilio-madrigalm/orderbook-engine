"""Tests for python/replay.py. Run: python3 -m unittest discover -s python/tests

The end-to-end test needs a built ob_gen/ob_replay (./build or $OB_BUILD_DIR)
and is skipped otherwise.
"""

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import replay  # noqa: E402
from replay import ReplayError, Snapshot, Trade  # noqa: E402

TRADES = """ts_ns,buy_id,sell_id,price,qty,aggressor
100,2,1,10000,5,B
250,3,4,9999,7,S
250,3,5,9998,1,S
"""

SNAPS = """seq,ts_ns,bid_px1,bid_qty1,bid_px2,bid_qty2,ask_px1,ask_qty1,ask_px2,ask_qty2
1,50,9999,10,9998,4,10001,3,0,0
2,100,9999,10,9998,4,10001,3,10002,8
3,250,9997,2,0,0,0,0,0,0
"""


class TempFiles(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self._tmp.name)

    def tearDown(self):
        self._tmp.cleanup()

    def write(self, name, text):
        p = self.dir / name
        p.write_text(text)
        return p


class ReadTrades(TempFiles):
    def test_parses_rows(self):
        trades = replay.read_trades(self.write("t.csv", TRADES))
        self.assertEqual(len(trades), 3)
        self.assertEqual(trades[0], Trade(100, 2, 1, 10000, 5, "B"))
        self.assertEqual(trades[2].aggressor, "S")
        self.assertIsInstance(trades[0].price, int)

    def test_header_only_is_empty(self):
        self.assertEqual(replay.read_trades(self.write("t.csv", TRADES.splitlines()[0] + "\n")), [])

    def test_rejects_wrong_header(self):
        with self.assertRaises(ReplayError):
            replay.read_trades(self.write("t.csv", "ts,buy,sell,px,qty,agg\n1,1,1,1,1,B\n"))

    def test_rejects_bad_aggressor(self):
        with self.assertRaises(ReplayError):
            replay.read_trades(self.write("t.csv", TRADES.splitlines()[0] + "\n1,1,2,100,1,X\n"))

    def test_rejects_non_integer_price(self):
        with self.assertRaisesRegex(ReplayError, ":2: bad 'price'"):
            replay.read_trades(self.write("t.csv", TRADES.splitlines()[0] + "\n1,1,2,100.5,1,B\n"))


class ReadSnapshots(TempFiles):
    def setUp(self):
        super().setUp()
        self.snaps = replay.read_snapshots(self.write("s.csv", SNAPS))

    def test_levels_and_padding(self):
        s0, s1, s2 = self.snaps
        self.assertEqual(s0.depth, 2)
        self.assertEqual(s0.bids, ((9999, 10), (9998, 4)))
        self.assertEqual(s0.asks, ((10001, 3),))  # 0,0 padding dropped
        self.assertEqual(s1.asks, ((10001, 3), (10002, 8)))
        self.assertEqual(s2.asks, ())

    def test_best_mid_spread(self):
        s0, _, s2 = self.snaps
        self.assertEqual((s0.best_bid, s0.best_ask), (9999, 10001))
        self.assertEqual(s0.mid_x2, 20000)
        self.assertEqual(s0.spread, 2)
        self.assertTrue(s0.two_sided)
        self.assertFalse(s2.two_sided)
        self.assertIsNone(s2.mid_x2)
        self.assertIsNone(s2.spread)
        self.assertIsNone(s2.best_ask)

    def test_qty_at(self):
        s0, s1, s2 = self.snaps
        self.assertEqual(s0.qty_at("B", 9999), 10)
        self.assertEqual(s0.qty_at("B", 10000), 0)    # better than best: empty
        self.assertIsNone(s0.qty_at("B", 9990))       # beyond full visible depth: unknown
        self.assertEqual(s0.qty_at("S", 10005), 0)    # ask side not full: book has nothing there
        self.assertIsNone(s1.qty_at("S", 10005))
        self.assertEqual(s2.qty_at("S", 10000), 0)    # empty side

    def test_rejects_ragged_row(self):
        with self.assertRaises(ReplayError):
            replay.read_snapshots(self.write("bad.csv", SNAPS.splitlines()[0] + "\n1,2,3\n"))

    def test_rejects_bad_header(self):
        with self.assertRaises(ReplayError):
            replay.read_snapshots(self.write("bad.csv", "seq,ts_ns,bid_px1,bid_qty1,ask_qty1,ask_px1\n"))
        with self.assertRaises(ReplayError):
            replay.read_snapshots(self.write("empty.csv", ""))


class Merge(unittest.TestCase):
    def test_time_order_trades_first_on_ties(self):
        trades = [Trade(100, 0, 0, 1, 1, "B"), Trade(250, 0, 0, 1, 1, "S")]
        snaps = [Snapshot(1, 50, (), ()), Snapshot(2, 100, (), ()), Snapshot(3, 300, (), ())]
        out = list(replay.merge(trades, snaps))
        self.assertEqual([x.ts_ns for x in out], [50, 100, 100, 250, 300])
        self.assertIsInstance(out[1], Trade)
        self.assertIsInstance(out[2], Snapshot)

    def test_empty_inputs(self):
        self.assertEqual(list(replay.merge([], [])), [])
        self.assertEqual(len(list(replay.merge([], [Snapshot(1, 1, (), ())]))), 1)


class Summarize(unittest.TestCase):
    def test_numbers(self):
        trades = [Trade(1, 0, 0, 100, 1, "B"), Trade(2, 0, 0, 103, 3, "S")]
        snaps = [Snapshot(1, 1, ((99, 1),), ((101, 1),)), Snapshot(2, 2, (), ((101, 1),))]
        s = replay.summarize(trades, snaps)
        self.assertEqual(s["volume"], 4)
        self.assertAlmostEqual(s["vwap"], 102.25)
        self.assertAlmostEqual(s["buy_aggressor_share"], 0.25)
        self.assertAlmostEqual(s["two_sided_share"], 0.5)
        self.assertAlmostEqual(s["mean_spread"], 2.0)

    def test_empty(self):
        s = replay.summarize([], [])
        self.assertIsNone(s["vwap"])
        self.assertIsNone(s["mean_spread"])


def _build_dir():
    try:
        replay.find_binary("ob_replay")
        replay.find_binary("ob_gen")
        return replay.find_binary("ob_replay").parent
    except ReplayError:
        return None


@unittest.skipIf(_build_dir() is None, "ob_gen/ob_replay not built")
class EndToEnd(TempFiles):
    def test_generate_replay_load(self):
        build = _build_dir()
        events = self.dir / "events.csv"
        subprocess.run([str(build / "ob_gen"), "--events", "20000", "--seed", "7", "--out", str(events)],
                       check=True, capture_output=True)
        trades_csv, snaps_csv, line = replay.run_replay(events, self.dir / "out", build_dir=build,
                                                        snapshot_every=10, depth=3)
        trades = replay.read_trades(trades_csv)
        snaps = replay.read_snapshots(snaps_csv)

        stats = dict(kv.split("=") for kv in line.split())
        self.assertEqual(len(trades), int(stats["trades"]))
        self.assertEqual(sum(t.qty for t in trades), int(stats["traded_qty"]))
        self.assertEqual(len(snaps), int(stats["events"]) // 10)
        self.assertEqual([s.seq for s in snaps[:3]], [10, 20, 30])

        for s in snaps:
            if s.two_sided:
                self.assertLess(s.best_bid, s.best_ask)  # book never stays crossed
            self.assertEqual(list(s.bids), sorted(s.bids, reverse=True))
            self.assertEqual(list(s.asks), sorted(s.asks))
        ts = [x.ts_ns for x in replay.merge(trades, snaps)]
        self.assertEqual(ts, sorted(ts))

    def test_cli(self):
        build = _build_dir()
        events = self.dir / "events.csv"
        subprocess.run([str(build / "ob_gen"), "--events", "2000", "--seed", "1", "--out", str(events)],
                       check=True, capture_output=True)
        rc = replay.main(["--events", str(events), "--out-dir", str(self.dir / "o"), "--build-dir", str(build)])
        self.assertEqual(rc, 0)

    def test_missing_input_is_an_error(self):
        with self.assertRaises(ReplayError):
            replay.run_replay(self.dir / "nope.csv", self.dir / "o", build_dir=_build_dir())


if __name__ == "__main__":
    unittest.main()
