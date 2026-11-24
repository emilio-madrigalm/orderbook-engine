"""Inventory-skewed market maker.

On each book snapshot the strategy posts one bid and one ask around a
reservation price:

    reservation = mid - skew,   skew = trunc(position / skew_step) ticks
    bid = floor(reservation - half_spread)
    ask = ceil(reservation + half_spread)

Long inventory pushes both quotes down (sell more readily, buy less readily);
short inventory pushes them up. Rounding goes away from the reservation price,
so a quote is never closer than half_spread ticks to it.

Guards:
  * quotes never cross the book (bid < best ask, ask > best bid), so they
    always rest as passive orders instead of taking liquidity;
  * prices stay >= 1 tick (the C++ book's price band starts at 1);
  * order sizes are cut so a full fill cannot push |position| past
    max_position; at the limit that side is not quoted at all;
  * with a one-sided or empty book there is no mid, so nothing is quoted.

All prices are integer ticks. The mid is handled doubled (bid + ask) so the
arithmetic stays in integers. Standard library only.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import NamedTuple, Optional

from replay import Snapshot


class Quote(NamedTuple):
    price: int  # ticks
    qty: int


class Quotes(NamedTuple):
    bid: Optional[Quote]
    ask: Optional[Quote]


@dataclass(frozen=True)
class MMConfig:
    half_spread: int = 1  # ticks from the reservation price to each quote
    size: int = 10  # qty per quote
    max_position: int = 100  # |position| hard limit
    skew_step: int = 10  # inventory units per tick of skew

    def __post_init__(self):
        for name in ("half_spread", "size", "max_position", "skew_step"):
            if getattr(self, name) < 1:
                raise ValueError(f"{name} must be >= 1")
        if self.size > self.max_position:
            raise ValueError("size must be <= max_position")


def _trunc_div(a: int, b: int) -> int:
    """Integer division rounding toward zero (Python's // rounds down)."""
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b > 0) else -q


class MarketMaker:
    def __init__(self, config: MMConfig = MMConfig()):
        self.config = config

    def skew(self, position: int) -> int:
        return _trunc_div(position, self.config.skew_step)

    def quote(self, snapshot: Snapshot, position: int) -> Quotes:
        mid_x2 = snapshot.mid_x2
        if mid_x2 is None:
            return Quotes(None, None)
        c = self.config
        res_x2 = mid_x2 - 2 * self.skew(position)
        bid_px = (res_x2 - 2 * c.half_spread) // 2  # floor
        ask_px = -((-(res_x2 + 2 * c.half_spread)) // 2)  # ceil
        bid_px = max(1, min(bid_px, snapshot.best_ask - 1))
        ask_px = max(ask_px, snapshot.best_bid + 1)

        bid_qty = min(c.size, c.max_position - position)
        ask_qty = min(c.size, c.max_position + position)
        return Quotes(
            Quote(bid_px, bid_qty) if bid_qty > 0 else None,
            Quote(ask_px, ask_qty) if ask_qty > 0 else None,
        )
