# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""
sockbench.py - lwip-amiga's sockbench on the Amiga against the PC's
tcp-bench-peer.py: the test matrix, the run, and the parse of what it prints.

sockbench speaks only bsdsocket.library, so the same binary measures lwip-amiga
(netdev or SANA-II) and Roadshow. Its lines:

    total: <bytes> bytes, <X.Y> Mb/s
    rr resp=<R> req=<Q>: <T> trans/s  lat us avg/min/p50/p90/p99/max a/b/c/d/e/f  X.Y MB/s
    (the stack refused <N> datagrams with ENOBUFS ...)
"""
from __future__ import annotations

import re
import statistics
from dataclasses import dataclass

TOTAL_RE = re.compile(r"^total: (\d+) bytes, (\d+\.\d) Mb/s", re.M)
RR_RE = re.compile(r"^rr(?:sel)? resp=(\S+) req=(\S+): (\d+) trans/s\s+lat us avg/min/p50/p90/p99/max "
                   r"(\d+)/(\d+)/(\d+)/(\d+)/(\d+)/(\d+)", re.M)
REFUSED_RE = re.compile(r"the stack refused (\d+) datagrams with ENOBUFS")

NETDEV = "netdev"
SANA2 = "sana2"
ROADSHOW = "roadshow"
EVERYWHERE = frozenset({NETDEV, SANA2, ROADSHOW})


@dataclass(frozen=True)
class Test:
    """One line of the matrix: sockbench's arguments ({peer} filled in), its deadline, where it runs."""

    name: str
    args: str
    deadline: float
    where: frozenset = EVERYWHERE
    seconds: float = 10.0       # how long sockbench runs it

    @property
    def latency(self) -> bool:
        """Whether this is a request-response test (judged by latency, not Mb/s)."""
        return self.name.startswith("rr")


# SANA-II cannot take 64 KB UDP at 900 Mb/s (the agent starves); Roadshow drops UDP
# datagrams over 32739 bytes (a signed 16-bit ip_len), so it gets 16 KB instead of 64 KB.
MATRIX = (
    Test("tcp_tx", "tx {peer}", 40),
    Test("tcp_rx", "rx {peer}", 40),
    Test("udp_tx_1472", "udptx {peer}", 40),
    Test("udp_tx_64k", "udptx {peer} 1 10 64", 40),
    Test("udp_rx_100", "udprx {peer} 1 10 0 100", 40),
    Test("udp_rx_200", "udprx {peer} 1 10 0 200", 40),
    Test("udp_rx_600", "udprx {peer} 1 10 0 600", 40, frozenset({NETDEV})),
    Test("udp_rx_64k", "udprx {peer} 1 10 64 900", 40, frozenset({NETDEV})),
    Test("udp_rx_64k_300", "udprx {peer} 1 10 64 300", 40, frozenset({NETDEV, SANA2})),
    Test("udp_rx_16k_300", "udprx {peer} 1 10 16 300", 40, frozenset({ROADSHOW})),
    Test("rr_1", "rr {peer} 1 5", 30, seconds=5.0),
    Test("rr_1k", "rr {peer} 1K 5", 30, seconds=5.0),
)


def matrix(where: str) -> list[Test]:
    """The tests that run under @where (netdev, sana2 or roadshow)."""
    return [t for t in MATRIX if where in t.where]


@dataclass
class Throughput:
    """A throughput run: what sockbench's total line said."""

    mbps: float
    bytes: int
    refused: int = 0


@dataclass
class RequestResponse:
    """A request-response run: transactions per second and the latency percentiles, in us."""

    tps: int
    avg_us: int
    min_us: int
    p50_us: int
    p90_us: int
    p99_us: int
    max_us: int


def parse(text: str) -> Throughput | RequestResponse | None:
    """What one sockbench run printed; None when it printed no result."""
    m = RR_RE.search(text)
    if m:
        v = [int(x) for x in m.groups()[2:]]
        return RequestResponse(*v)
    m = TOTAL_RE.search(text)
    if m:
        refused = REFUSED_RE.search(text)
        return Throughput(float(m.group(2)), int(m.group(1)), int(refused.group(1)) if refused else 0)
    return None


@dataclass
class Series:
    """The repeated runs of one test."""

    test: Test
    runs: list[Throughput | RequestResponse]

    def values(self) -> list[float]:
        """The judged figure of each run: Mb/s, or transactions per second for rr."""
        return [float(r.tps) if isinstance(r, RequestResponse) else r.mbps for r in self.runs]

    def latencies(self) -> list[int]:
        """The p50 latency of each rr run, in us."""
        return [r.p50_us for r in self.runs if isinstance(r, RequestResponse)]

    @property
    def median(self) -> float:
        """The median of values()."""
        return statistics.median(self.values())


def script(pri: int) -> bytes:
    """The Execute script one run goes through: sockbench at priority @pri, its arguments as ARGS."""
    return (b".key ARGS/F\n.bra {\n.ket }\n"
            b"ChangeTaskPri %d\n" % pri +
            b"RAM:gt/sockbench {ARGS}\n")
