# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""
uart.py - the Emu68 debug log from the Pi's UART, captured for the whole
session into the run dir (CLAUDE.md: the PI4's FT232 carries a plain 921600 8N1
UART at /dev/ttyUSB0; reading consumes the data, so one reader at a time).

The profile tier marks where a test starts (position()) and reads what came
after it (text_since()); a perf report arrives every ~2 s.
"""
from __future__ import annotations

import threading
from pathlib import Path

BAUD = 921600
READ_BYTES = 65536
READ_TIMEOUT_S = 0.2


class Capture:
    """A background reader of the serial port into a file."""

    def __init__(self, path: Path, port: str, baud: int = BAUD):
        import serial  # pyserial: only the profile tier needs it

        self.path = Path(path)
        self.port = serial.Serial(port, baud, timeout=READ_TIMEOUT_S)
        self.port.reset_input_buffer()
        self.file = open(self.path, "wb")
        self.written = 0
        self.lock = threading.Lock()
        self.stopping = threading.Event()
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def _run(self) -> None:
        while not self.stopping.is_set():
            data = self.port.read(READ_BYTES)
            if data:
                with self.lock:
                    self.file.write(data)
                    self.file.flush()
                    self.written += len(data)

    def position(self) -> int:
        """How much has been captured so far: a mark for text_since()."""
        with self.lock:
            return self.written

    def text_since(self, position: int) -> str:
        """What arrived after @position, its CRs and NULs dropped."""
        with self.lock:
            self.file.flush()
        data = self.path.read_bytes()[position:]
        return data.replace(b"\r\n", b"\n").replace(b"\r", b"\n").replace(b"\0", b"").decode("latin-1")

    def close(self) -> None:
        """Stop reading and close the port and the file."""
        self.stopping.set()
        self.thread.join()
        self.file.close()
        self.port.close()
