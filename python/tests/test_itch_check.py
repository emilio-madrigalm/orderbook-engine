import json
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import itch_check  # noqa: E402
from itch_check import MARKET_BASE, CheckResult, ItchCheckError, check_depth, check_fills  # noqa: E402

M0, M1 = MARKET_BASE, MARKET_BASE + 1
TRADES_HEADER = "ts_ns,buy_id,sell_id,price,qty,aggressor\n"


def write(path, text):
    Path(path).write_text(text)
    return path


class CheckFillsTests(unittest.TestCase):
    def run_fills(self, trades, expected):
        with tempfile.TemporaryDirectory() as d:
            t = write(Path(d) / "t.csv", TRADES_HEADER + "".join(r + "\n" for r in trades))
            res = CheckResult()
            check_fills(t, expected, res)
            return res

    def test_exact(self):
        res = self.run_fills([f"1,{M0},11,30015,20,B", f"2,12,{M1},30010,5,S"],
                             {M0: (11, 30015, 20), M1: (12, 30010, 5)})
        self.assertEqual((res.executions, res.exact_fills, res.extra_trades), (2, 2, 0))
        self.assertIsNone(res.first_bad_fill)

    def test_each_failure_kind(self):
        m2, m3 = MARKET_BASE + 2, MARKET_BASE + 3
        res = self.run_fills(
            [f"1,{M0},99,30015,20,B",                           # wrong resting order
             f"2,{M1},11,30016,20,B",                           # wrong price
             f"3,{m2},11,30015,10,B", f"3,{m2},12,30015,10,B",  # split
             "4,5,6,30015,1,B"],                                # extra trade from an add
            {M0: (11, 30015, 20), M1: (11, 30015, 20), m2: (11, 30015, 20), m3: (11, 30015, 1)})
        self.assertEqual((res.wrong_order, res.wrong_price, res.split_fills, res.no_fill, res.extra_trades),
                         (1, 1, 1, 1, 1))
        self.assertEqual(res.exact_fills, 0)
        self.assertEqual(res.first_bad_fill["market_id"], M0)


class CheckDepthTests(unittest.TestCase):
    H = "seq,ts_ns,bid_px1,bid_qty1,ask_px1,ask_qty1\n"

    def test_counts_and_first_mismatch(self):
        with tempfile.TemporaryDirectory() as d:
            a = write(Path(d) / "a.csv", self.H + "1,1,100,5,0,0\n2,2,100,5,101,3\n3,3,100,5,101,1\n")
            b = write(Path(d) / "b.csv", self.H + "1,1,100,5,0,0\n2,2,100,5,101,2\n3,3,100,5,101,1\n")
            res = CheckResult()
            check_depth(a, b, res)
            self.assertEqual((res.snapshots_compared, res.depth_match, res.first_depth_mismatch_seq), (3, 2, 2))

    def test_row_count_mismatch_raises(self):
        with tempfile.TemporaryDirectory() as d:
            a = write(Path(d) / "a.csv", self.H + "1,1,100,5,0,0\n")
            b = write(Path(d) / "b.csv", self.H)
            with self.assertRaises(ItchCheckError):
                check_depth(a, b, CheckResult())


# --- end to end: hand-built ITCH bytes -> ob_itch -> ob_replay -> check -------------

def hdr(t, loc, ts):
    return struct.pack(">cHH", t.encode(), loc, 0) + ts.to_bytes(6, "big")


def stock_dir(loc, sym):
    return (hdr("R", loc, 0) + sym.ljust(8).encode()).ljust(39, b"\0")


def add(loc, ts, ref, side, qty, px):
    return hdr("A", loc, ts) + struct.pack(">QcI8sI", ref, side.encode(), qty, b"AAPL    ", px)


def execute(loc, ts, ref, qty, match):
    return hdr("E", loc, ts) + struct.pack(">QIQ", ref, qty, match)


def delete(loc, ts, ref):
    return hdr("D", loc, ts) + struct.pack(">Q", ref)


def frame(msgs):
    return b"".join(struct.pack(">H", len(m)) + m for m in msgs)


def build_dir():
    d = os.environ.get("OB_BUILD_DIR")
    return d if d and (Path(d) / "ob_itch").is_file() else None


@unittest.skipUnless(build_dir(), "set OB_BUILD_DIR to a build with ob_itch and ob_replay")
class EndToEndTests(unittest.TestCase):
    def convert(self, d, msgs):
        raw = Path(d) / "day.itch"
        raw.write_bytes(frame(msgs))
        out = Path(d) / "out"
        p = subprocess.run([str(Path(build_dir()) / "ob_itch"), "--symbol", "AAPL", "--in", str(raw),
                            "--out-dir", str(out), "--depth", "3"], capture_output=True, text=True)
        self.assertEqual(p.returncode, 0, p.stderr)
        return out

    def test_price_time_priority_reproduces_nasdaq(self):
        with tempfile.TemporaryDirectory() as d:
            out = self.convert(d, [
                stock_dir(3, "AAPL"),
                add(3, 1, 11, "S", 50, 3001500),
                add(3, 2, 12, "S", 30, 3001500),  # same price, behind 11
                add(3, 3, 13, "B", 40, 3001000),
                execute(3, 4, 11, 50, 1),         # front of the queue: what our book picks too
                execute(3, 5, 12, 10, 2),
                execute(3, 6, 13, 40, 3),
                delete(3, 7, 12),
            ])
            res = itch_check.run_check(out, build_dir=build_dir())
            self.assertEqual((res.executions, res.exact_fills, res.extra_trades, res.rejected), (3, 3, 0, 0))
            self.assertEqual(res.snapshots_compared, res.depth_match)
            self.assertEqual(res.snapshots_compared, 7)

    def test_out_of_priority_execution_is_caught(self):
        with tempfile.TemporaryDirectory() as d:
            out = self.convert(d, [
                stock_dir(3, "AAPL"),
                add(3, 1, 11, "S", 50, 3001500),
                add(3, 2, 12, "S", 30, 3001500),
                execute(3, 3, 12, 30, 1),  # Nasdaq says the second order filled first
            ])
            res = itch_check.run_check(out, build_dir=build_dir())
            self.assertEqual((res.executions, res.exact_fills, res.wrong_order), (1, 0, 1))
            self.assertEqual(res.first_bad_fill["expected"], [12, 30015, 30])
            self.assertEqual(res.first_bad_fill["got"], [[11, 30015, 30]])
            # Our book still totals 50 at 30015 after the fill, so depth agrees here.
            self.assertEqual(res.depth_match, res.snapshots_compared)

    def test_cli_writes_report(self):
        with tempfile.TemporaryDirectory() as d:
            out = self.convert(d, [stock_dir(3, "AAPL"), add(3, 1, 11, "B", 5, 1000000)])
            self.assertEqual(itch_check.main(["--dir", str(out), "--build-dir", build_dir()]), 0)
            report = json.loads((out / "itch_check.json").read_text())
            self.assertEqual(report["symbol"], "AAPL")
            self.assertEqual(report["depth_match_pct"], 100.0)
            self.assertIsNone(report["exact_fill_pct"])

    def test_missing_symbol_fails(self):
        with tempfile.TemporaryDirectory() as d:
            raw = Path(d) / "day.itch"
            raw.write_bytes(frame([stock_dir(3, "MSFT")]))
            p = subprocess.run([str(Path(build_dir()) / "ob_itch"), "--symbol", "AAPL", "--in", str(raw),
                                "--out-dir", str(Path(d) / "o")], capture_output=True, text=True)
            self.assertEqual(p.returncode, 1)
            self.assertIn("not in the stock directory", p.stderr)


if __name__ == "__main__":
    unittest.main()
