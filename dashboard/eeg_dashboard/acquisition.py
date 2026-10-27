from __future__ import annotations

import math
import queue
import threading
import time
from collections.abc import Iterator

import numpy as np
import serial

from .models import Diagnostic, EEGSample
from .protocol import ProtocolError, parse_line


class AcquisitionWorker(threading.Thread):
    def __init__(self, output: queue.Queue, *, port: str | None, baud: int, demo: bool = False):
        super().__init__(daemon=True, name="eeg-acquisition")
        self.output = output
        self.port = port
        self.baud = baud
        self.demo = demo
        self.stop_event = threading.Event()

    def stop(self) -> None:
        self.stop_event.set()

    def run(self) -> None:
        if self.demo:
            self._run_demo()
        else:
            self._run_serial()

    def _put(self, item: EEGSample | Diagnostic) -> None:
        try:
            self.output.put_nowait(item)
        except queue.Full:
            try:
                self.output.get_nowait()
                self.output.put_nowait(item)
            except queue.Empty:
                pass

    def _run_serial(self) -> None:
        if not self.port:
            self._put(Diagnostic("error", {"error": "serial port is missing"}))
            return
        try:
            with serial.Serial(self.port, self.baud, timeout=0.2) as connection:
                self._put(Diagnostic("serial-open", {"port": self.port, "baud": str(self.baud)}))
                while not self.stop_event.is_set():
                    raw = connection.readline()
                    if not raw:
                        continue
                    try:
                        parsed = parse_line(raw.decode("utf-8", errors="replace"))
                        if parsed is not None:
                            self._put(parsed)
                    except ProtocolError as exc:
                        self._put(Diagnostic("protocol-error", {"error": str(exc)}))
        except serial.SerialException as exc:
            self._put(Diagnostic("serial-error", {"error": str(exc)}))

    def _run_demo(self) -> None:
        """Deterministic synthetic alpha + theta; never presented as real EEG."""
        sample_rate = 200.0
        rng = np.random.default_rng(20261027)
        started = time.perf_counter()
        sequence = 0
        self._put(Diagnostic("synthetic", {"state": "synthetic", "sample_hz": "200"}))
        while not self.stop_event.is_set():
            target = started + sequence / sample_rate
            remaining = target - time.perf_counter()
            if remaining > 0:
                time.sleep(min(remaining, 0.005))
                continue
            t = sequence / sample_rate
            ch1 = 25.0 * math.sin(2 * math.pi * 10.0 * t) + 4.0 * rng.normal()
            ch2 = 18.0 * math.sin(2 * math.pi * 6.0 * t + 0.4) + 4.0 * rng.normal()
            timestamp = int(t * 1_000_000)
            self._put(EEGSample(sequence, sequence % 200, timestamp, timestamp, (ch1, ch2, 0.0, 0.0)))
            sequence += 1
