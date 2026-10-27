from __future__ import annotations

import argparse
import json

MICROVOLTS_PER_COUNT = 0.001869917138805


def _fields(payload: bytes, width: int) -> list[int]:
    stream = int.from_bytes(payload, "big")
    total_bits = len(payload) * 8
    mask = (1 << width) - 1
    return [(stream >> (total_bits - (index + 1) * width)) & mask for index in range(8)]


def decode_v2_deltas(payload: bytes, width: int) -> list[list[int]]:
    decoded = [value - (1 << width) if value & 1 else value for value in _fields(payload, width)]
    return [decoded[:4], decoded[4:]]


def decode_v3_absolute(payload: bytes, width: int) -> list[list[int]]:
    mask = (1 << width) - 1
    scale = 64 if width == 18 else 32
    decoded = [(value - mask - 2 if value & (1 << (width - 1)) else value) * scale for value in _fields(payload, width)]
    return [decoded[:4], decoded[4:]]


def decode_packet(packet: bytes, firmware: int, baseline: list[int] | None = None) -> list[list[int]]:
    if len(packet) != 20:
        raise ValueError("Ganglion EEG packets must have 20 bytes")
    packet_id = packet[0]
    if firmware == 3 and packet_id < 200:
        return decode_v3_absolute(packet[1:], 18 if packet_id < 100 else 19)
    if firmware != 2:
        raise ValueError("firmware must be 2 or 3")
    if packet_id == 0:
        return [[int.from_bytes(packet[1 + 3 * i : 4 + 3 * i], "big", signed=True) for i in range(4)]]
    if not 1 <= packet_id <= 200 or baseline is None:
        raise ValueError("compressed firmware v2 packets need a baseline")
    samples: list[list[int]] = []
    current = list(baseline)
    for deltas in decode_v2_deltas(packet[1:], 18 if packet_id <= 100 else 19):
        current = [value - delta for value, delta in zip(current, deltas)]
        samples.append(current)
    return samples


def main() -> None:
    parser = argparse.ArgumentParser(description="Independent Ganglion packet decoder")
    parser.add_argument("packet_hex")
    parser.add_argument("--firmware", type=int, choices=(2, 3), required=True)
    parser.add_argument("--baseline", help="four comma-separated counts for compressed firmware v2")
    args = parser.parse_args()
    baseline = [int(value) for value in args.baseline.split(",")] if args.baseline else None
    counts = decode_packet(bytes.fromhex(args.packet_hex), args.firmware, baseline)
    print(json.dumps({"counts": counts, "microvolts": [[value * MICROVOLTS_PER_COUNT for value in row] for row in counts]}, indent=2))


if __name__ == "__main__":
    main()
