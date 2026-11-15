# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""
limits.py - what the smoke and full tiers accept from a short run: floors, not
baselines (the perf tier judges speed). A floor sits well under what the
2026-11 rounds measured on the PI4, so that only a broken path misses it.
"""
from __future__ import annotations

from .stack import AUTO, NETDEV, SANA2

# 3 s of one TCP stream on the gigabit link, Mb/s. Measured: netdev 926-946 both ways;
# SANA-II 640 send, 446 receive.
TCP_FLOOR_MBPS = {AUTO: 500.0, NETDEV: 500.0, SANA2: 300.0}

# A 100 Mb/s full-duplex link: what TCP and UDP reach there (measured 94-96).
LINK_100_LEAST_MBPS = 80.0
LINK_100_MOST_MBPS = 100.0

# A forced 100 Mb/s half-duplex link: TCP's ACKs share the wire with its data.
HALF_DUPLEX_LEAST_MBPS = 20.0

GIGABIT = 1000
FAST = 100

SHORT = "1 3"  # sockbench: one stream, 3 s
