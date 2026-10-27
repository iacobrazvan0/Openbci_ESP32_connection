from __future__ import annotations

from .models import Diagnostic, EEGSample


class ProtocolError(ValueError):
    pass


def _fields(line: str) -> tuple[str, dict[str, str]]:
    parts = [part.strip() for part in line.strip().split(",")]
    if not parts or not parts[0]:
        raise ProtocolError("empty line")
    values: dict[str, str] = {}
    for part in parts[1:]:
        if "=" not in part:
            continue
        key, value = part.split("=", 1)
        values[key] = value
    return parts[0], values


def parse_line(line: str) -> EEGSample | Diagnostic | None:
    kind, data = _fields(line)
    if kind == "EEG":
        try:
            if data.get("v") != "1":
                raise ProtocolError("unsupported EEG protocol version")
            channels = tuple(float(data[f"ch{i}_uv"]) for i in range(1, 5))
            return EEGSample(
                sequence=int(data["seq"]),
                sample_id=int(data["sample_id"]),
                timestamp_us=int(data["ts_us"]),
                received_at_us=int(data["rx_us"]),
                channels_uv=channels,  # type: ignore[arg-type]
                gap_packets=int(data.get("gap_packets", "0")),
                discontinuity=data.get("discontinuity", "0") == "1",
            )
        except (KeyError, TypeError, ValueError) as exc:
            raise ProtocolError(f"invalid EEG line: {exc}") from exc
    if kind == "DIAG":
        return Diagnostic(state=data.get("state", "unknown"), values=data)
    if kind in {"STATUS", "EVENT"}:
        return Diagnostic(state=data.get("state", data.get("type", kind.lower())), values=data)
    return None
