from pathlib import Path

import pytest

from eeg_dashboard.validator import load_esp32, load_openbci_gui


def test_loads_openbci_percent_prefixed_header(tmp_path: Path):
    path = tmp_path / "OpenBCI-RAW.txt"
    path.write_text(
        "%OpenBCI Raw EEG Data\n%Sample Index,EXG Channel 0,EXG Channel 1,Timestamp\n"
        "0,1.5,-2.5,1000.000\n1,2.0,-2.0,1000.005\n",
        encoding="utf-8",
    )
    channels, timestamps, gaps = load_openbci_gui(path)
    assert channels[0].tolist() == [1.5, -2.5]
    assert timestamps[1] == pytest.approx(0.005)
    assert not gaps.any()


def test_loads_esp32_recording_schema(tmp_path: Path):
    path = tmp_path / "esp.csv"
    path.write_text(
        "sequence,sample_id,timestamp_us,received_at_us,ch1_raw_uv,ch2_raw_uv,ch3_raw_uv,ch4_raw_uv,ch1_filtered_uv,ch2_filtered_uv,gap_packets,discontinuity\n"
        "0,0,0,10,1,2,3,4,0,0,0,0\n1,1,5000,20,2,3,4,5,0,0,1,1\n",
        encoding="utf-8",
    )
    channels, timestamps, gaps = load_esp32(path)
    assert channels.shape == (2, 2)
    assert timestamps[1] == pytest.approx(0.005)
    assert gaps.tolist() == [False, True]
