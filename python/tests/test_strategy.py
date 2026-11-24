"""Tests for python/strategy.py. Run: python3 -m unittest discover -s python/tests"""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from replay import Snapshot  # noqa: E402
from strategy import MarketMaker, MMConfig, Quote, Quotes  # noqa: E402


def snap(bid, ask, bid_qty=10, ask_qty=10):
    bids = ((bid, bid_qty),) if bid is not None else ()
    asks = ((ask, ask_qty),) if ask is not None else ()
    return Snapshot(seq=1, ts_ns=0, bids=bids, asks=asks, depth=1)


class ConfigTest(unittest.TestCase):
    def test_rejects_bad_values(self):
        for kwargs in (
            {"half_spread": 0},
            {"size": 0},
            {"max_position": 0},
            {"skew_step": 0},
            {"size": 20, "max_position": 10},
        ):
            with self.subTest(**kwargs):
                with self.assertRaises(ValueError):
                    MMConfig(**kwargs)


class QuoteTest(unittest.TestCase):
    def setUp(self):
        self.mm = MarketMaker(MMConfig(half_spread=2, size=5, max_position=20, skew_step=5))

    def test_flat_inventory_quotes_symmetric_around_mid(self):
        q = self.mm.quote(snap(98, 102), position=0)  # mid 100
        self.assertEqual(q, Quotes(Quote(98, 5), Quote(102, 5)))

    def test_odd_mid_rounds_away_from_mid(self):
        # mid 100.5: bid floor(98.5)=98, ask ceil(102.5)=103, never inside +-half_spread
        q = self.mm.quote(snap(98, 103), position=0)
        self.assertEqual((q.bid.price, q.ask.price), (98, 103))

    def test_long_inventory_skews_quotes_down(self):
        q = self.mm.quote(snap(90, 110), position=10)  # 10 / skew_step 5 = 2 ticks
        self.assertEqual((q.bid.price, q.ask.price), (96, 100))

    def test_short_inventory_skews_quotes_up(self):
        q = self.mm.quote(snap(90, 110), position=-10)
        self.assertEqual((q.bid.price, q.ask.price), (100, 104))

    def test_skew_truncates_toward_zero(self):
        up = self.mm.quote(snap(90, 110), position=4)
        down = self.mm.quote(snap(90, 110), position=-4)
        self.assertEqual((up.bid.price, down.bid.price), (98, 98))

    def test_never_crosses_the_book(self):
        # tight book: quotes are pulled back to join the touch, not cross it
        q = self.mm.quote(snap(100, 101), position=0)
        self.assertEqual((q.bid.price, q.ask.price), (98, 103))
        q = self.mm.quote(snap(100, 101), position=-20 + 5)  # strong up skew
        self.assertLess(q.bid.price, 101)
        q = self.mm.quote(snap(100, 101), position=20 - 5)  # strong down skew
        self.assertGreater(q.ask.price, 100)

    def test_bid_size_shrinks_near_long_limit(self):
        q = self.mm.quote(snap(90, 110), position=18)
        self.assertEqual(q.bid.qty, 2)
        self.assertEqual(q.ask.qty, 5)

    def test_no_bid_at_long_limit(self):
        q = self.mm.quote(snap(90, 110), position=20)
        self.assertIsNone(q.bid)
        self.assertIsNotNone(q.ask)

    def test_no_ask_at_short_limit(self):
        q = self.mm.quote(snap(90, 110), position=-20)
        self.assertIsNone(q.ask)
        self.assertEqual(q.bid.qty, 5)

    def test_position_never_exceeds_limit_after_full_fill(self):
        for pos in range(-20, 21):
            q = self.mm.quote(snap(90, 110), position=pos)
            if q.bid:
                self.assertLessEqual(pos + q.bid.qty, 20)
            if q.ask:
                self.assertGreaterEqual(pos - q.ask.qty, -20)

    def test_one_sided_book_stands_aside(self):
        self.assertEqual(self.mm.quote(snap(None, 101), 0), Quotes(None, None))
        self.assertEqual(self.mm.quote(snap(99, None), 0), Quotes(None, None))

    def test_prices_stay_positive(self):
        q = self.mm.quote(snap(1, 3), position=20 - 1)  # skew 3 ticks down near the floor
        self.assertGreaterEqual(q.bid.price, 1)
        self.assertGreaterEqual(q.ask.price, 1)
        self.assertLess(q.bid.price, q.ask.price)

    def test_prices_are_ints(self):
        q = self.mm.quote(snap(98, 103), position=3)
        self.assertIsInstance(q.bid.price, int)
        self.assertIsInstance(q.ask.price, int)


if __name__ == "__main__":
    unittest.main()
