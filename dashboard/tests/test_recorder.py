import csv
import json
from pathlib import Path

from eeg_dashboard.models import EEGSample
from eeg_dashboard.recorder import CSVRecorder


def test_csv_preserves_raw_filtered_gaps_and_metadata(tmp_path: Path):
    path = tmp_path / "session.csv"
    recorder = CSVRecorder(path, {"source": "test", "filter": {"bandpass_hz": [1, 40]}})
    sample = EEGSample(1, 2, 5000, 5100, (10.0, -20.0, 30.0, -40.0), 1, True, (1.5, -2.5))
    recorder.write(sample)
    recorder.close()
    row = next(csv.DictReader(path.open(encoding="utf-8")))
    assert float(row["ch1_raw_uv"]) == 10.0
    assert float(row["ch1_filtered_uv"]) == 1.5
    assert row["gap_packets"] == "1" and row["discontinuity"] == "1"
    metadata = json.loads(path.with_suffix(".csv.metadata.json").read_text(encoding="utf-8"))
    assert metadata["units"]["eeg"] == "uV"
    assert "never filtered" in metadata["raw_data_policy"]
