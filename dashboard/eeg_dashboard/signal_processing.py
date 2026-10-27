from __future__ import annotations

from dataclasses import dataclass

import numpy as np
from scipy import signal


@dataclass(frozen=True, slots=True)
class FilterConfig:
    sample_rate_hz: float = 200.0
    bandpass_hz: tuple[float, float] = (1.0, 40.0)
    bandpass_order: int = 4
    notch_hz: float = 50.0
    notch_q: float = 30.0

    def validate(self) -> None:
        nyquist = self.sample_rate_hz / 2.0
        low, high = self.bandpass_hz
        if not 0 < low < high < nyquist:
            raise ValueError("bandpass must be inside (0, Nyquist)")
        if not 0 < self.notch_hz < nyquist:
            raise ValueError("notch must be inside (0, Nyquist)")
        if self.bandpass_order < 1 or self.notch_q <= 0:
            raise ValueError("invalid filter order/Q")


def rms(values: np.ndarray) -> float:
    data = np.asarray(values, dtype=float)
    finite = data[np.isfinite(data)]
    return float(np.sqrt(np.mean(np.square(finite)))) if finite.size else float("nan")


def _sections(config: FilterConfig) -> np.ndarray:
    config.validate()
    bandpass = signal.butter(
        config.bandpass_order,
        config.bandpass_hz,
        btype="bandpass",
        fs=config.sample_rate_hz,
        output="sos",
    )
    notch_b, notch_a = signal.iirnotch(
        config.notch_hz, config.notch_q, fs=config.sample_rate_hz
    )
    return np.vstack((signal.tf2sos(notch_b, notch_a), bandpass))


class StreamingFilter:
    """Causal notch + band-pass filter, reset at every acquisition gap."""

    def __init__(self, config: FilterConfig, channels: int = 2):
        self.config = config
        self.sos = _sections(config)
        self.channels = channels
        self.state = [np.zeros((self.sos.shape[0], 2), dtype=float) for _ in range(channels)]
        self.initialized = [False] * channels

    def reset(self) -> None:
        for state in self.state:
            state.fill(0.0)
        self.initialized = [False] * self.channels

    def process(self, values: tuple[float, ...], discontinuity: bool = False) -> tuple[float, ...]:
        if discontinuity:
            self.reset()
        output: list[float] = []
        for channel, value in enumerate(values[: self.channels]):
            if not self.initialized[channel]:
                self.state[channel] = signal.sosfilt_zi(self.sos) * value
                self.initialized[channel] = True
            filtered, self.state[channel] = signal.sosfilt(
                self.sos, np.asarray([value], dtype=float), zi=self.state[channel]
            )
            output.append(float(filtered[0]))
        return tuple(output)


def filter_segments(values: np.ndarray, config: FilterConfig, gaps: np.ndarray | None = None) -> np.ndarray:
    """Zero-phase offline filter; gaps remain NaN and are never bridged."""
    data = np.asarray(values, dtype=float)
    result = np.full(data.shape, np.nan, dtype=float)
    invalid = ~np.isfinite(data)
    if gaps is not None:
        invalid |= np.asarray(gaps, dtype=bool)
    valid_indices = np.flatnonzero(~invalid)
    if not valid_indices.size:
        return result
    split_points = np.where(np.diff(valid_indices) != 1)[0] + 1
    min_length = 3 * (2 * _sections(config).shape[0] + 1)
    for indices in np.split(valid_indices, split_points):
        if indices.size <= min_length:
            continue
        segment = data[indices] - np.mean(data[indices])
        filtered = signal.sosfiltfilt(_sections(config), segment)
        result[indices] = filtered - np.mean(filtered)
    return result


def welch_psd(values: np.ndarray, sample_rate_hz: float, window_s: float = 2.0) -> tuple[np.ndarray, np.ndarray]:
    data = np.asarray(values, dtype=float)
    finite = data[np.isfinite(data)]
    nperseg = min(finite.size, max(8, int(sample_rate_hz * window_s)))
    if nperseg < 8:
        return np.empty(0), np.empty(0)
    return signal.welch(finite - np.mean(finite), fs=sample_rate_hz, nperseg=nperseg, scaling="density")


def band_powers(frequencies: np.ndarray, psd_uv2_hz: np.ndarray) -> dict[str, float]:
    bands = {"theta": (4.0, 8.0), "alpha": (8.0, 13.0), "beta": (13.0, 30.0)}
    powers: dict[str, float] = {}
    for name, (low, high) in bands.items():
        mask = (frequencies >= low) & (frequencies < high)
        powers[name] = float(np.trapezoid(psd_uv2_hz[mask], frequencies[mask])) if np.count_nonzero(mask) > 1 else float("nan")
    return powers


def technical_quality(
    values: np.ndarray,
    *,
    has_gap: bool,
    minimum_samples: int,
    flat_std_uv: float = 0.5,
    high_rms_uv: float = 200.0,
    rail_uv: float = 15000.0,
) -> str:
    data = np.asarray(values, dtype=float)
    finite = data[np.isfinite(data)]
    if finite.size < minimum_samples:
        return "insufficient data"
    if has_gap:
        return "gap detected"
    if np.max(np.abs(finite)) >= rail_uv:
        return "near rail/clipping"
    if np.std(finite) < flat_std_uv:
        return "flat/poor contact suspect"
    if rms(finite) > high_rms_uv:
        return "high amplitude/artifact suspect"
    return "technically usable; EEG not validated"
