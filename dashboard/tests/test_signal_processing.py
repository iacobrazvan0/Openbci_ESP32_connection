import numpy as np
import pytest

from eeg_dashboard.signal_processing import FilterConfig, band_powers, filter_segments, rms, technical_quality, welch_psd


def test_rms_known_sine_and_dc():
    fs = 200
    t = np.arange(fs) / fs
    sine = 10 * np.sqrt(2) * np.sin(2 * np.pi * 10 * t)
    assert rms(sine) == pytest.approx(10.0, rel=1e-6)
    assert rms(np.full(fs, 3.0)) == pytest.approx(3.0)


def test_filter_preserves_10_hz_and_attenuates_dc_and_50_hz():
    fs = 200
    t = np.arange(fs * 10) / fs
    source = 100 + 20 * np.sin(2 * np.pi * 10 * t) + 20 * np.sin(2 * np.pi * 50 * t)
    filtered = filter_segments(source, FilterConfig())
    frequencies, psd = welch_psd(filtered, fs)
    p10 = psd[np.argmin(np.abs(frequencies - 10))]
    p50 = psd[np.argmin(np.abs(frequencies - 50))]
    assert abs(np.nanmean(filtered)) < 0.1
    assert p10 > p50 * 100
    assert rms(filtered) == pytest.approx(20 / np.sqrt(2), rel=0.08)


def test_filter_does_not_bridge_gap():
    values = np.sin(np.arange(1000) / 10)
    gaps = np.zeros(1000, dtype=bool); gaps[500] = True
    filtered = filter_segments(values, FilterConfig(), gaps)
    assert np.isnan(filtered[500])
    assert np.isfinite(filtered[100:400]).all()
    assert np.isfinite(filtered[600:900]).all()


def test_welch_units_and_band_power():
    fs = 200
    t = np.arange(fs * 4) / fs
    frequencies, psd = welch_psd(10 * np.sin(2 * np.pi * 10 * t), fs)
    powers = band_powers(frequencies, psd)
    assert frequencies[np.argmax(psd)] == pytest.approx(10.0)
    assert powers["alpha"] == pytest.approx(50.0, rel=0.08)
    assert powers["alpha"] > powers["theta"] * 100


def test_quality_never_claims_physiological_validity():
    label = technical_quality(np.ones(200) * 10, has_gap=False, minimum_samples=200)
    assert "EEG" not in label or "not validated" in label
