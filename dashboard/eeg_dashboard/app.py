from __future__ import annotations

import argparse
import json
import queue
import sys
import time
from collections import deque
from datetime import datetime
from pathlib import Path

import numpy as np
import pyqtgraph as pg
from PySide6 import QtCore, QtWidgets

from .acquisition import AcquisitionWorker
from .models import Diagnostic, EEGSample
from .recorder import CSVRecorder
from .signal_processing import FilterConfig, StreamingFilter, band_powers, rms, technical_quality, welch_psd


class Dashboard(QtWidgets.QMainWindow):
    def __init__(self, config: dict, *, demo: bool = False):
        super().__init__()
        self.config = config
        self.demo = demo
        signal_cfg = config["signal"]
        self.sample_rate = float(signal_cfg["sample_rate_hz"])
        self.filter_config = FilterConfig(
            sample_rate_hz=self.sample_rate,
            bandpass_hz=tuple(signal_cfg["bandpass_hz"]),
            bandpass_order=int(signal_cfg["bandpass_order"]),
            notch_hz=float(signal_cfg["notch_hz"]), notch_q=float(signal_cfg["notch_q"]),
        )
        self.filter = StreamingFilter(self.filter_config)
        capacity = int(self.sample_rate * config["display"]["buffer_s"])
        self.samples: deque[EEGSample] = deque(maxlen=capacity)
        self.inbox: queue.Queue = queue.Queue(maxsize=capacity)
        serial_cfg = config["serial"]
        self.worker = AcquisitionWorker(
            self.inbox, port=serial_cfg.get("port"), baud=int(serial_cfg["baud"]), demo=demo
        )
        self.recorder: CSVRecorder | None = None
        self.last_diagnostic = Diagnostic("starting")
        self.last_sample_wall = 0.0
        self.started_wall = time.monotonic()
        self._build_ui()
        self.worker.start()
        self.ui_timer = QtCore.QTimer(self)
        self.ui_timer.timeout.connect(self._update)
        self.ui_timer.start(50)
        self.spectrum_timer = QtCore.QTimer(self)
        self.spectrum_timer.timeout.connect(self._update_spectrum)
        self.spectrum_timer.start(500)

    def _build_ui(self) -> None:
        self.setWindowTitle("Ganglion ESP32 EEG Dashboard — SYNTHETIC DEMO" if self.demo else "Ganglion ESP32 EEG Dashboard")
        self.resize(1400, 900)
        central = QtWidgets.QWidget()
        root = QtWidgets.QVBoxLayout(central)
        if self.demo:
            banner = QtWidgets.QLabel("SYNTHETIC DEMO — generated signals, not human EEG")
            banner.setStyleSheet("background:#8b1e1e;color:white;font-weight:bold;padding:8px")
            root.addWidget(banner)

        controls = QtWidgets.QHBoxLayout()
        self.mode_combo = QtWidgets.QComboBox(); self.mode_combo.addItems(["Filtered", "Raw"])
        self.window_combo = QtWidgets.QComboBox(); self.window_combo.addItems(["3", "5", "10"]); self.window_combo.setCurrentText(str(self.config["display"]["window_s"]))
        self.scale_spin = QtWidgets.QDoubleSpinBox(); self.scale_spin.setRange(10, 20000); self.scale_spin.setValue(self.config["display"]["vertical_uv"]); self.scale_spin.setSuffix(" µV")
        self.rms_spin = QtWidgets.QDoubleSpinBox(); self.rms_spin.setRange(0.25, 10); self.rms_spin.setValue(self.config["signal"]["rms_window_s"]); self.rms_spin.setSuffix(" s")
        self.record_button = QtWidgets.QPushButton("Start CSV recording"); self.record_button.clicked.connect(self._toggle_recording)
        for label, widget in (("Signal", self.mode_combo), ("Window", self.window_combo), ("Vertical", self.scale_spin), ("RMS window", self.rms_spin)):
            controls.addWidget(QtWidgets.QLabel(label)); controls.addWidget(widget)
        controls.addStretch(); controls.addWidget(self.record_button); root.addLayout(controls)

        values = QtWidgets.QGridLayout()
        self.channel_labels: list[QtWidgets.QLabel] = []
        for channel, name in enumerate(("Ch1 / Fp1", "Ch2 / Fp2")):
            label = QtWidgets.QLabel(f"{name}: waiting for samples")
            label.setStyleSheet("font-size:15px;padding:5px")
            values.addWidget(label, 0, channel)
            self.channel_labels.append(label)
        self.diag_label = QtWidgets.QLabel("Connection: starting")
        self.diag_label.setWordWrap(True); values.addWidget(self.diag_label, 1, 0, 1, 2)
        root.addLayout(values)

        plots = QtWidgets.QSplitter(QtCore.Qt.Orientation.Vertical)
        self.time_plots = []
        self.time_curves = []
        for channel, color in enumerate(("#32a8ff", "#ff9f32"), start=1):
            plot = pg.PlotWidget(title=f"Ch{channel} — {'Fp1' if channel == 1 else 'Fp2'}")
            plot.setLabel("left", "Amplitude", units="µV"); plot.setLabel("bottom", "Time", units="s")
            plot.showGrid(x=True, y=True, alpha=0.25)
            self.time_plots.append(plot); self.time_curves.append(plot.plot(pen=pg.mkPen(color, width=1)))
            plots.addWidget(plot)
        self.psd_plot = pg.PlotWidget(title="Welch PSD")
        self.psd_plot.setLabel("left", "PSD", units="µV²/Hz"); self.psd_plot.setLabel("bottom", "Frequency", units="Hz")
        self.psd_plot.setLogMode(y=True); self.psd_plot.showGrid(x=True, y=True, alpha=0.25)
        self.psd_curves = [self.psd_plot.plot(pen=pg.mkPen("#32a8ff", width=2), name="Ch1"), self.psd_plot.plot(pen=pg.mkPen("#ff9f32", width=2), name="Ch2")]
        self.band_label = QtWidgets.QLabel("Band power: waiting")
        psd_container = QtWidgets.QWidget(); psd_layout = QtWidgets.QVBoxLayout(psd_container); psd_layout.setContentsMargins(0, 0, 0, 0); psd_layout.addWidget(self.psd_plot); psd_layout.addWidget(self.band_label)
        plots.addWidget(psd_container); root.addWidget(plots)
        self.setCentralWidget(central)

    def _drain(self) -> None:
        for _ in range(1000):
            try:
                item = self.inbox.get_nowait()
            except queue.Empty:
                break
            if isinstance(item, Diagnostic):
                self.last_diagnostic = item
                continue
            item.filtered_uv = self.filter.process(item.channels_uv[:2], item.discontinuity)
            self.samples.append(item)
            self.last_sample_wall = time.monotonic()
            if self.recorder:
                self.recorder.write(item)

    def _update(self) -> None:
        self._drain()
        if not self.samples:
            self.diag_label.setText(f"Connection: {self.last_diagnostic.state}")
            return
        window_s = float(self.window_combo.currentText())
        latest_ts = self.samples[-1].timestamp_us
        selected = [sample for sample in self.samples if sample.timestamp_us >= latest_ts - window_s * 1_000_000]
        x = np.asarray([(sample.timestamp_us - latest_ts) / 1_000_000.0 for sample in selected])
        use_raw = self.mode_combo.currentText() == "Raw"
        scale = self.scale_spin.value()
        for channel in range(2):
            y = np.asarray([sample.channels_uv[channel] if use_raw else sample.filtered_uv[channel] for sample in selected])
            self.time_curves[channel].setData(x, y)
            self.time_plots[channel].setXRange(-window_s, 0, padding=0)
            self.time_plots[channel].setYRange(-scale, scale, padding=0)

        rms_count = max(1, int(self.rms_spin.value() * self.sample_rate))
        recent = list(self.samples)[-rms_count:]
        quality_cfg = self.config["quality"]
        for channel in range(2):
            raw = np.asarray([sample.channels_uv[channel] for sample in recent])
            filtered = np.asarray([sample.filtered_uv[channel] for sample in recent])
            quality = technical_quality(
                raw, has_gap=any(sample.discontinuity for sample in recent), minimum_samples=rms_count,
                flat_std_uv=quality_cfg["flat_std_uv"], high_rms_uv=quality_cfg["high_rms_uv"], rail_uv=quality_cfg["rail_uv"],
            )
            current = self.samples[-1]
            self.channel_labels[channel].setText(
                f"Ch{channel + 1} / {'Fp1' if channel == 0 else 'Fp2'}: "
                f"instant {current.channels_uv[channel]:.3f} µV | raw RMS {rms(raw):.3f} µVrms | "
                f"filtered RMS {rms(filtered):.3f} µVrms | {quality}"
            )
        connected = "synthetic" if self.demo else ("receiving" if time.monotonic() - self.last_sample_wall < 2 else "no recent samples")
        diag = self.last_diagnostic
        self.diag_label.setText(
            f"Connection: {connected} | sample rate: {diag.values.get('sample_hz', 'calculating')} Hz | "
            f"samples: {diag.values.get('samples', len(self.samples))} | lost packets: {diag.values.get('lost_packets', 0)} | "
            f"decoder errors: {diag.values.get('decoder_errors', 0)} | duration: {time.monotonic() - self.started_wall:.1f} s"
        )

    def _update_spectrum(self) -> None:
        if len(self.samples) < int(self.sample_rate):
            return
        contiguous = list(self.samples)
        last_gap = max((i for i, sample in enumerate(contiguous) if sample.discontinuity), default=-1)
        contiguous = contiguous[last_gap + 1 :]
        lines = []
        for channel in range(2):
            values = np.asarray([sample.filtered_uv[channel] for sample in contiguous])
            frequencies, psd = welch_psd(values, self.sample_rate, self.config["signal"]["welch_window_s"])
            self.psd_curves[channel].setData(frequencies, psd)
            powers = band_powers(frequencies, psd)
            lines.append(f"Ch{channel + 1}: θ={powers['theta']:.2f}, α={powers['alpha']:.2f}, β={powers['beta']:.2f} µV²")
        self.psd_plot.setXRange(0, 40, padding=0)
        self.band_label.setText(" | ".join(lines))

    def _toggle_recording(self) -> None:
        if self.recorder:
            self.recorder.close(); self.recorder = None; self.record_button.setText("Start CSV recording"); return
        root = Path(__file__).resolve().parents[1]
        name = datetime.now().strftime("ganglion_%Y%m%d_%H%M%S.csv")
        metadata = {"source": "synthetic-demo" if self.demo else "ESP32 USB Serial", "filter": {"type": "causal streaming", **self.config["signal"]}}
        self.recorder = CSVRecorder(root / "recordings" / name, metadata)
        self.record_button.setText("Stop CSV recording")

    def closeEvent(self, event) -> None:  # noqa: N802
        self.worker.stop()
        if self.recorder:
            self.recorder.close()
        event.accept()


def load_config(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8-sig"))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", type=Path, default=Path(__file__).resolve().parents[1] / "config.json")
    parser.add_argument("--port", help="override the serial port from config.json")
    parser.add_argument("--demo", action="store_true", help="use explicitly labelled synthetic signals")
    parser.add_argument("--smoke-test", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    config = load_config(args.config)
    if args.port:
        config["serial"]["port"] = args.port
    application = QtWidgets.QApplication(sys.argv[:1])
    window = Dashboard(config, demo=args.demo)
    window.show()
    if args.smoke_test:
        QtCore.QTimer.singleShot(1200, application.quit)
    return application.exec()


if __name__ == "__main__":
    raise SystemExit(main())
