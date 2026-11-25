import csv
import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import lobster  # noqa: E402
from lobster import (  # noqa: E402
    MARKET_BASE,
    PLACEHOLDER_BASE,
    CheckResult,
    LobsterError,
    convert,
    parse_book_row,
    parse_message,
    parse_time_ns,
    to_ticks,
)

P0, P1, P2, P3 = (PLACEHOLDER_BASE + i for i in range(4))

# A hand-built LOBSTER day, 2 levels. Prices in $ x 10000; 1000000 = $100.00 = 10000 ticks.
MESSAGES = [
    "34200.000000001,1,10,100,1000000,1",    # 0 add bid (already in book row 0, not replayed)
    "34200.000000002,1,11,20,1000100,-1",    # 1 add ask 20 @ 10001 behind pre-open 50
    "34200.000000003,4,99,30,1000100,-1",    # 2 pre-open ask executes 30
    "34200.000000004,2,11,5,1000100,-1",     # 3 id 11 cancels 5 -> 15
    "34200.000000005,3,10,100,1000000,1",    # 4 delete id 10 (unseen) -> placeholder 300 -> 200
    "34200.000000006,5,0,7,1000050,1",       # 5 hidden execution: skipped
    "34200.000000007,4,98,20,1000100,-1",    # 6 rest of pre-open ask executes
    "34200.000000008,4,11,15,1000100,-1",    # 7 id 11 executes
    "34200.000000009,7,0,0,-1,-1",           # 8 halt: skipped
]
E = -9999999999
A = 9999999999
BOOK = [
    "1000100,50,1000000,300,1000200,70,999900,40",
    "1000100,70,1000000,300,1000200,70,999900,40",
    "1000100,40,1000000,300,1000200,70,999900,40",
    "1000100,35,1000000,300,1000200,70,999900,40",
    "1000100,35,1000000,200,1000200,70,999900,40",
    "1000100,35,1000000,200,1000200,70,999900,40",
    "1000100,15,1000000,200,1000200,70,999900,40",
    f"1000200,70,1000000,200,{A},0,999900,40",
    f"1000200,70,1000000,200,{A},0,999900,40",
]


def msgs():
    return [parse_message(r.split(","), i + 1) for i, r in enumerate(MESSAGES)]


def first_book():
    return parse_book_row(BOOK[0].split(","), 1)


class ParseTests(unittest.TestCase):
    def test_time_is_exact(self):
        self.assertEqual(parse_time_ns("34200.004241176"), 34200004241176)
        self.assertEqual(parse_time_ns("34200.5"), 34200500000000)
        self.assertEqual(parse_time_ns("34200"), 34200000000000)

    def test_bad_time(self):
        with self.assertRaises(LobsterError):
            parse_time_ns("-1.5")

    def test_ticks(self):
        self.assertEqual(to_ticks(5852100), 58521)
        self.assertIsNone(to_ticks(5852150))

    def test_book_row_drops_empty_levels(self):
        bids, asks = parse_book_row(f"1000100,5,{E},0,{A},0,{E},0".split(","), 1)
        self.assertEqual(asks, ((10001, 5),))
        self.assertEqual(bids, ())

    def test_book_row_bad_width(self):
        with self.assertRaises(LobsterError):
            parse_book_row(["1", "2", "3"], 1)

    def test_message_short_row(self):
        with self.assertRaises(LobsterError):
            parse_message(["34200.1", "1"], 7)


class ConvertTests(unittest.TestCase):
    def setUp(self):
        self.conv = convert(msgs(), first_book())

    def lines(self):
        return [e.csv() for e in self.conv.events]

    def test_seeds_then_events(self):
        self.assertEqual(self.lines(), [
            f"0,A,{P0},B,10000,300",
            f"0,A,{P1},B,9999,40",
            f"0,A,{P2},S,10001,50",
            f"0,A,{P3},S,10002,70",
            "1,A,11,S,10001,20",
            f"2,M,{MARKET_BASE},B,0,30",
            "3,X,11,-,0,15",
            f"4,X,{P0},-,0,200",
            f"6,M,{MARKET_BASE + 1},B,0,20",
            f"7,M,{MARKET_BASE + 2},B,0,15",
        ])

    def test_source_rows(self):
        self.assertEqual(self.conv.source_row, [0, 0, 0, 0, 1, 2, 3, 4, 6, 7])

    def test_expected_fills(self):
        exp = self.conv.expected
        self.assertEqual(exp[MARKET_BASE].resting_id, P2)
        self.assertEqual(exp[MARKET_BASE + 1].resting_id, P2)
        self.assertEqual((exp[MARKET_BASE + 2].resting_id, exp[MARKET_BASE + 2].price), (11, 10001))

    def test_stats(self):
        s = self.conv.stats
        self.assertEqual((s.messages, s.seeded_levels, s.adds, s.executions), (9, 4, 1, 3))
        self.assertEqual((s.partial_cancels, s.cancels, s.skipped_hidden, s.skipped_halt), (1, 1, 1, 1))
        self.assertEqual(s.unmatched_ids, 0)

    def test_unknown_id_without_placeholder_is_counted(self):
        extra = parse_message("34200.1,3,555,10,990000,1".split(","), 10)
        conv = convert(msgs() + [extra], first_book())
        self.assertEqual(conv.stats.unmatched_ids, 1)

    def test_full_delete_of_known_order_cancels(self):
        extra = parse_message("34200.1,1,12,9,999800,1".split(","), 10)
        gone = parse_message("34200.2,3,12,9,999800,1".split(","), 11)
        conv = convert(msgs() + [extra, gone], first_book())
        self.assertEqual(conv.events[-1].csv(), f"{34200200000000 - 34200000000001},C,12,-,0,0")

    def test_price_above_band_raises(self):
        big = parse_message("34200.1,1,13,1,13200000,1".split(","), 10)
        with self.assertRaises(LobsterError):
            convert(msgs() + [big], first_book())

    def test_unknown_type_raises(self):
        bad = parse_message("34200.1,9,1,1,1000000,1".split(","), 10)
        with self.assertRaises(LobsterError):
            convert(msgs() + [bad], first_book())


class CheckFillsTests(unittest.TestCase):
    def run_fills(self, rows):
        conv = convert(msgs(), first_book())
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "t.csv"
            with open(p, "w", newline="") as f:
                w = csv.writer(f)
                w.writerow(["ts_ns", "buy_id", "sell_id", "price", "qty", "aggressor"])
                w.writerows(rows)
            res = CheckResult()
            lobster.check_fills(p, conv.expected, res)
            return res

    def test_all_exact(self):
        res = self.run_fills([
            [2, MARKET_BASE, P2, 10001, 30, "B"],
            [6, MARKET_BASE + 1, P2, 10001, 20, "B"],
            [7, MARKET_BASE + 2, 11, 10001, 15, "B"],
        ])
        self.assertEqual((res.executions, res.exact_fills), (3, 3))

    def test_wrong_order_price_split_and_missing(self):
        res = self.run_fills([
            [2, MARKET_BASE, 11, 10001, 30, "B"],                # wrong resting order
            [6, MARKET_BASE + 1, P3, 10002, 20, "B"],            # wrong price
            [7, MARKET_BASE + 2, 11, 10001, 10, "B"],            # split in two
            [7, MARKET_BASE + 2, P2, 10001, 5, "B"],
        ])
        self.assertEqual((res.wrong_order, res.wrong_price, res.split_fills, res.exact_fills),
                         (1, 1, 1, 0))
        res = self.run_fills([])
        self.assertEqual(res.no_fill, 3)


def build_dir():
    d = os.environ.get("OB_BUILD_DIR")
    return d if d and (Path(d) / "ob_replay").is_file() else None


@unittest.skipUnless(build_dir(), "set OB_BUILD_DIR to a build with ob_replay")
class EndToEndTests(unittest.TestCase):
    def write_day(self, d, book_rows):
        m, b = Path(d) / "m.csv", Path(d) / "b.csv"
        m.write_text("\n".join(MESSAGES) + "\n")
        b.write_text("\n".join(book_rows) + "\n")
        return m, b

    def test_book_reproduces_the_day(self):
        with tempfile.TemporaryDirectory() as d:
            m, b = self.write_day(d, BOOK)
            res = lobster.run_check(m, b, Path(d) / "out", build_dir=build_dir())
        self.assertEqual(res.rejected, 0)
        self.assertEqual((res.executions, res.exact_fills), (3, 3))
        # rows 0,1,2,3,4,6,7 produce events; 5 (hidden) and 8 (halt) do not
        self.assertEqual(res.snapshots_compared, 7)
        self.assertEqual((res.top1_match, res.top5_match), (7, 7))
        self.assertIsNone(res.first_top1_mismatch_row)

    def test_depth_mismatch_is_reported(self):
        book = list(BOOK)
        book[3] = "1000100,36,1000000,300,1000200,70,999900,40"  # exchange says 36, we have 35
        with tempfile.TemporaryDirectory() as d:
            m, b = self.write_day(d, book)
            res = lobster.run_check(m, b, Path(d) / "out", build_dir=build_dir())
        self.assertEqual(res.top1_match, 6)
        self.assertEqual(res.first_top1_mismatch_row, 3)

    def test_cli_writes_report(self):
        with tempfile.TemporaryDirectory() as d:
            m, b = self.write_day(d, BOOK)
            out = Path(d) / "out"
            rc = lobster.main(["--messages", str(m), "--orderbook", str(b), "--out-dir", str(out),
                               "--build-dir", build_dir()])
            self.assertEqual(rc, 0)
            self.assertIn('"exact_fill_pct": 100.0', (out / "lobster_check.json").read_text())


if __name__ == "__main__":
    unittest.main()
