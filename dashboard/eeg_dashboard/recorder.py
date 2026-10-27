from __future__ import annotations

import csv
import json
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from .models import EEGSample


class CSVRecorder:
    columns = (
        "sequence", "sample_id", "timestamp_us", "received_at_us",
        "ch1_raw_uv", "ch2_raw_uv", "ch3_raw_uv", "ch4_raw_uv",
        "ch1_filtered_uv", "ch2_filtered_uv", "gap_packets", "discontinuity",
    )

    def __init__(self, path: Path, metadata: dict[str, Any]):
        path.parent.mkdir(parents=True, exist_ok=True)
        self.path = path
        self.handle = path.open("w", newline="", encoding="utf-8")
        self.writer = csv.DictWriter(self.handle, fieldnames=self.columns)
        self.writer.writeheader()
        metadata = dict(metadata)
        metadata.update({
            "format_version": 1,
            "started_utc": datetime.now(timezone.utc).isoformat(),
            "units": {"eeg": "uV", "timestamp": "us"},
            "raw_data_policy": "raw columns are never filtered or overwritten",
        })
        path.with_suffix(path.suffix + ".metadata.json").write_text(
            json.dumps(metadata, indent=2), encoding="utf-8"
        )

    def write(self, sample: EEGSample) -> None:
        filtered = sample.filtered_uv or (float("nan"), float("nan"))
        self.writer.writerow({
            "sequence": sample.sequence,
            "sample_id": sample.sample_id,
            "timestamp_us": sample.timestamp_us,
            "received_at_us": sample.received_at_us,
            "ch1_raw_uv": sample.channels_uv[0], "ch2_raw_uv": sample.channels_uv[1],
            "ch3_raw_uv": sample.channels_uv[2], "ch4_raw_uv": sample.channels_uv[3],
            "ch1_filtered_uv": filtered[0], "ch2_filtered_uv": filtered[1],
            "gap_packets": sample.gap_packets,
            "discontinuity": int(sample.discontinuity),
        })

    def close(self) -> None:
        self.handle.flush()
        self.handle.close()
