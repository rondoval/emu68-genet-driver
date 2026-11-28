# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""
speedtest.py - AmiSpeedTest (UNIT=MEGABIT), an internet speed test: recorded,
never judged, since the line and the server decide most of it.

       Average download speed  =  852.8 Mb/s
       Average upload speed  =  71.2 Mb/s
"""
from __future__ import annotations

import re

AVERAGE_RE = re.compile(r"Average (download|upload) speed\s*=\s*(\d+(?:\.\d+)?)\s*Mb/s", re.I)


def parse(text: str) -> dict[str, float]:
    """{'download': Mb/s, 'upload': Mb/s}, as far as the run got."""
    return {kind.lower(): float(value) for kind, value in AVERAGE_RE.findall(text)}
