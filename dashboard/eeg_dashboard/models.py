from __future__ import annotations

from dataclasses import dataclass, field


@dataclass(slots=True)
class EEGSample:
    sequence: int
    sample_id: int
    timestamp_us: int
    received_at_us: int
    channels_uv: tuple[float, float, float, float]
    gap_packets: int = 0
    discontinuity: bool = False
    filtered_uv: tuple[float, float] | None = None


@dataclass(slots=True)
class Diagnostic:
    state: str = "unknown"
    values: dict[str, str] = field(default_factory=dict)

    def number(self, key: str, default: float = 0.0) -> float:
        try:
            return float(self.values.get(key, default))
        except (TypeError, ValueError):
            return default
