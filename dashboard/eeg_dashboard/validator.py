from __future__ import annotations

import argparse
import csv
import json
from dataclasses import asdict
from pathlib import Path

import numpy as np

from .signal_processing import FilterConfig, band_powers, filter_segments, rms, welch_psd


def _numeric_rows(path: Path) -> tuple[list[str], np.ndarray]:
    lines: list[str] = []
    for line in path.read_text(encoding="utf-8-sig", errors="replace").splitlines():
        stripped = line.strip()
        if not stripped:
            continue
        if stripped.startswith("%"):
            candidate = stripped[1:].strip()
            if "sample index" in candidate.lower() and any(delimiter in candidate for delimiter in ",;\t"):
                lines.append(candidate)
            continue
        lines.append(stripped)
    if not lines:
        raise ValueError(f"no data in {path}")
    dialect = csv.Sniffer().sniff("\n".join(lines[:5]), delimiters=",;\t")
    rows = list(csv.reader(lines, dialect))
    has_header = any(any(char.isalpha() for char in cell) for cell in rows[0])
    if not has_header:
        raise ValueError("a header row is required to identify EEG and timestamp columns")
    header = [cell.strip() for cell in rows[0]]
    values = np.asarray([[float(cell.strip()) for cell in row[: len(header)]] for row in rows[1:] if len(row) >= len(header)], dtype=float)
    return header, values


def load_esp32(path: Path) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    header, data = _numeric_rows(path)
    index = {name.lower(): i for i, name in enumerate(header)}
    channels = np.column_stack((data[:, index["ch1_raw_uv"]], data[:, index["ch2_raw_uv"]]))
    timestamps = data[:, index["timestamp_us"]] / 1_000_000.0
    gaps = data[:, index["discontinuity"]].astype(bool)
    return channels, timestamps, gaps


def load_openbci_gui(path: Path, channel_names: tuple[str, str] = ("EXG Channel 0", "EXG Channel 1")) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    header, data = _numeric_rows(path)
    index = {name.lower(): i for i, name in enumerate(header)}
    try:
        channels = np.column_stack([data[:, index[name.lower()]] for name in channel_names])
    except KeyError as exc:
        raise ValueError(f"missing OpenBCI GUI channel: {exc}") from exc
    timestamp_key = next((key for key in index if key == "timestamp" or "timestamp" in key and "formatted" not in key), None)
    if timestamp_key:
        timestamps = data[:, index[timestamp_key]]
        timestamps = timestamps - timestamps[0]
    else:
        sample_key = next((key for key in index if "sample index" in key), None)
        if sample_key is None:
            raise ValueError("missing Timestamp or Sample Index")
        timestamps = np.arange(data.shape[0], dtype=float) / 200.0
    return channels, timestamps, np.zeros(data.shape[0], dtype=bool)


def summarize(channels: np.ndarray, timestamps: np.ndarray, gaps: np.ndarray, config: FilterConfig) -> dict:
    duration = float(timestamps[-1] - timestamps[0]) if timestamps.size > 1 else 0.0
    actual_rate = float((timestamps.size - 1) / duration) if duration > 0 else float("nan")
    report: dict = {"samples": int(timestamps.size), "duration_s": duration, "sample_rate_hz": actual_rate, "channels": []}
    for channel in range(min(2, channels.shape[1])):
        raw = channels[:, channel]
        filtered = filter_segments(raw, config, gaps)
        frequencies, psd = welch_psd(filtered, config.sample_rate_hz)
        report["channels"].append({
            "raw_rms_uv": rms(raw),
            "filtered_rms_uv": rms(filtered),
            "raw_peak_to_peak_uv": float(np.nanmax(raw) - np.nanmin(raw)),
            "band_power_uv2": band_powers(frequencies, psd),
        })
    return report


def compare(gui_path: Path, esp_path: Path, config: FilterConfig) -> dict:
    gui = summarize(*load_openbci_gui(gui_path), config)
    esp = summarize(*load_esp32(esp_path), config)
    differences: list[dict] = []
    for channel, (left, right) in enumerate(zip(gui["channels"], esp["channels"]), start=1):
        differences.append({
            "channel": channel,
            "filtered_rms_percent": 100.0 * (right["filtered_rms_uv"] - left["filtered_rms_uv"]) / left["filtered_rms_uv"] if left["filtered_rms_uv"] else float("nan"),
            "peak_to_peak_percent": 100.0 * (right["raw_peak_to_peak_uv"] - left["raw_peak_to_peak_uv"]) / left["raw_peak_to_peak_uv"] if left["raw_peak_to_peak_uv"] else float("nan"),
        })
    return {
        "comparison_scope": "aggregate metrics only; different sessions are not sample-aligned",
        "filter": asdict(config), "openbci_gui": gui, "esp32": esp, "differences": differences,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description="Compare aggregate OpenBCI GUI and ESP32 recordings")
    parser.add_argument("--gui", type=Path, required=True)
    parser.add_argument("--esp32", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=Path("validation_report.json"))
    args = parser.parse_args()
    report = compare(args.gui, args.esp32, FilterConfig())
    args.output.write_text(json.dumps(report, indent=2, allow_nan=True), encoding="utf-8")
    print(f"Report written to {args.output}")


if __name__ == "__main__":
    main()
