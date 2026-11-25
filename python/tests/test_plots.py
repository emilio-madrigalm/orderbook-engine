"""Tests for python/plots.py. Run: python3 -m unittest discover -s python/tests

Data preparation is tested on small hand-made inputs. Render tests need
matplotlib and are skipped without it; they check that a real PNG is written,
not how it looks. The ob_bench test needs a Release build and is skipped
otherwise.
"""

import math
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import plots  # noqa: E402
import replay  # noqa: E402
from backtest import Point, write_csv  # noqa: E402
from replay import ReplayError, Snapshot  # noqa: E402

try:
    import matplotlib  # noqa: F401

    HAVE_MPL = True
except ImportError:
    HAVE_MPL = False

PNG_MAGIC = b"\x89PNG\r\n\x1a\n"


def snap(ts, bids=((100, 5),), asks=((102, 5),)):
    return Snapshot(seq=ts, ts_ns=ts, bids=tuple(bids), asks=tuple(asks), depth=5)


def expand(hist):
    return [v for v, c in hist for _ in range(c)]


def nearest_rank(samples, p):  # the rule ob_bench uses, on a sorted list
    rank = min(max(math.ceil(p * len(samples)), 1), len(samples))
    return sorted(samples)[rank - 1]


def write_text(path, text):
    Path(path).write_text(text)
    return Path(path)


class PercentileTest(unittest.TestCase):
    HIST = [(10, 50), (12, 30), (40, 15), (300, 4), (9000, 1)]

    def test_matches_nearest_rank_on_expanded_samples(self):
        samples = expand(self.HIST)
        for p in (0.0, 0.01, 0.5, 0.8, 0.81, 0.95, 0.96, 0.99, 0.999, 1.0):
            self.assertEqual(plots.hist_percentile(self.HIST, p), nearest_rank(samples, p), p)

    def test_known_values(self):
        self.assertEqual(plots.hist_percentile(self.HIST, 0.50), 10)
        self.assertEqual(plots.hist_percentile(self.HIST, 0.51), 12)
        self.assertEqual(plots.hist_percentile(self.HIST, 0.99), 300)
        self.assertEqual(plots.hist_percentile(self.HIST, 1.0), 9000)

    def test_single_value(self):
        self.assertEqual(plots.hist_percentile([(7, 3)], 0.999), 7)


class BinTest(unittest.TestCase):
    def test_bins_and_beyond(self):
        hist = [(0, 1), (4, 2), (5, 3), (9, 4), (10, 5), (11, 6)]
        lefts, counts, beyond = plots.bin_hist(hist, width=5, upper=10)
        self.assertEqual(lefts, [0, 5, 10])
        self.assertEqual(counts, [3, 7, 5])
        self.assertEqual(beyond, 6)

    def test_total_is_preserved(self):
        hist = [(v, v % 7 + 1) for v in range(0, 500, 3)]
        _, counts, beyond = plots.bin_hist(hist, width=11, upper=250)
        self.assertEqual(sum(counts) + beyond, plots.hist_total(hist))

    def test_bad_arguments(self):
        with self.assertRaises(ValueError):
            plots.bin_hist([(1, 1)], width=0, upper=10)
        with self.assertRaises(ValueError):
            plots.bin_hist([(1, 1)], width=1, upper=-1)


class SpreadTest(unittest.TestCase):
    def test_counts_two_sided_only_and_sorted(self):
        snaps = [
            snap(1),                                # spread 2
            snap(2, asks=((101, 1),)),              # spread 1
            snap(3, asks=()),                       # one-sided: skipped
            snap(4, bids=()),                       # one-sided: skipped
            snap(5, bids=((98, 1),), asks=((103, 1),)),  # spread 5
            snap(6),                                # spread 2
        ]
        counts = plots.spread_counts(snaps)
        self.assertEqual(counts, {1: 1, 2: 2, 5: 1})
        self.assertEqual(list(counts), [1, 2, 5])

    def test_empty(self):
        self.assertEqual(plots.spread_counts([snap(1, asks=())]), {})


class ReadTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_pnl_round_trip_with_backtest_writer(self):
        pts = [Point(10, 0, 0.0, 202), Point(20, 5, -2.5, 203), Point(35, -3, 7.25, 201)]
        path = self.dir / "pnl.csv"
        write_csv(path, Point._fields, pts)
        self.assertEqual(plots.read_pnl(path), pts)

    def test_pnl_bad_header_and_row(self):
        with self.assertRaises(ReplayError):
            plots.read_pnl(write_text(self.dir / "a.csv", "ts,pos,pnl,mid\n1,0,0,2\n"))
        with self.assertRaises(ReplayError):
            plots.read_pnl(write_text(self.dir / "b.csv", "ts_ns,position,pnl,mid_x2\n1,x,0,2\n"))

    def test_latency_hist_reads(self):
        path = write_text(self.dir / "h.csv", "ns,count\n0,3\n17,1\n250,2\n")
        self.assertEqual(plots.read_latency_hist(path), [(0, 3), (17, 1), (250, 2)])

    def test_latency_hist_rejects_bad_input(self):
        bad = {
            "header": "value,count\n1,1\n",
            "empty": "ns,count\n",
            "not ascending": "ns,count\n5,1\n5,1\n",
            "descending": "ns,count\n5,1\n4,1\n",
            "zero count": "ns,count\n5,0\n",
            "negative": "ns,count\n-1,1\n",
            "text": "ns,count\nfive,1\n",
        }
        for name, text in bad.items():
            with self.subTest(name), self.assertRaises(ReplayError):
                plots.read_latency_hist(write_text(self.dir / "x.csv", text))


@unittest.skipUnless(HAVE_MPL, "matplotlib not installed")
class RenderTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def assertPng(self, path):
        self.assertTrue(path.is_file(), path)
        self.assertEqual(path.read_bytes()[:8], PNG_MAGIC)

    def test_each_plot_writes_a_png(self):
        pts = [Point(i * 1000, (i % 9) - 4, i * 0.5 - (i % 5), 200 + i % 3) for i in range(500)]
        plots.plot_pnl_inventory(pts, self.dir / "a.png")
        plots.plot_spread({1: 40, 2: 25, 3: 5}, self.dir / "b.png")
        plots.plot_latency([(20, 900), (35, 80), (60, 15), (400, 4), (80000, 1)], self.dir / "c.png")
        for name in ("a.png", "b.png", "c.png"):
            self.assertPng(self.dir / name)

    def test_empty_inputs_raise(self):
        with self.assertRaises(ValueError):
            plots.plot_pnl_inventory([], self.dir / "a.png")
        with self.assertRaises(ValueError):
            plots.plot_spread({}, self.dir / "b.png")

    def test_cli_makes_plots_for_present_inputs_only(self):
        write_csv(self.dir / "pnl.csv", Point._fields, [Point(0, 0, 0.0, 200), Point(5, 1, 1.0, 201)])
        write_text(self.dir / "latency_hist.csv", "ns,count\n10,5\n20,1\n")
        self.assertEqual(plots.main(["--out-dir", str(self.dir)]), 0)
        self.assertPng(self.dir / "pnl_inventory.png")
        self.assertPng(self.dir / "latency.png")
        self.assertFalse((self.dir / "spread.png").exists())


class CliErrorTest(unittest.TestCase):
    def test_no_inputs_is_an_error(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(plots.main(["--out-dir", d]), 1)

    def test_bad_input_is_an_error(self):
        with tempfile.TemporaryDirectory() as d:
            write_text(Path(d) / "latency_hist.csv", "nope\n")
            self.assertEqual(plots.main(["--out-dir", d]), 1)


class BenchHistogramTest(unittest.TestCase):
    """ob_bench --latency-hist writes one count per replayed event."""

    def test_histogram_counts_every_event(self):
        try:
            bench = replay.find_binary("ob_bench")
        except ReplayError as e:
            self.skipTest(str(e))
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "h.csv"
            proc = subprocess.run(
                [str(bench), "--events", "20000", "--warmup", "1000", "--repeat", "1",
                 "--latency-hist", str(path)],
                capture_output=True, text=True,
            )
            if proc.returncode == 2 and "non-Release" in proc.stderr:
                self.skipTest("ob_bench is not a Release build")
            self.assertEqual(proc.returncode, 0, proc.stderr)
            hist = plots.read_latency_hist(path)
        self.assertEqual(plots.hist_total(hist), 20000)
        p50 = plots.hist_percentile(hist, 0.5)
        self.assertIn(f"p50 {p50} ", proc.stdout)  # same rule, same samples

    def test_unwritable_path_fails(self):
        try:
            bench = replay.find_binary("ob_bench")
        except ReplayError as e:
            self.skipTest(str(e))
        proc = subprocess.run(
            [str(bench), "--events", "1000", "--warmup", "0", "--repeat", "1",
             "--latency-hist", "/nonexistent-dir/h.csv"],
            capture_output=True, text=True,
        )
        if proc.returncode == 2 and "non-Release" in proc.stderr:
            self.skipTest("ob_bench is not a Release build")
        self.assertEqual(proc.returncode, 3)


if __name__ == "__main__":
    unittest.main()
