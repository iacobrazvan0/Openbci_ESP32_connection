import pytest

from eeg_dashboard.decoder_reference import MICROVOLTS_PER_COUNT, decode_packet


def test_official_v2_packet_independent_reference():
    packet = bytes([1, 0, 0, 0, 0, 32, 0, 40, 0, 4, 128, 0, 188, 0, 7, 0, 40, 192, 10, 14])
    decoded = decode_packet(packet, firmware=2, baseline=[300000] * 4)
    assert decoded[0] == [300000, 299998, 299990, 299996]
    assert decoded[1] == [168926, 54238, 185292, 250834]


def test_v3_packet_and_scale_are_independent():
    packet = bytes.fromhex("000000400020000c00040001400060001c000800")
    decoded = decode_packet(packet, firmware=3)
    assert decoded == [[64, 128, 192, 256], [320, 384, 448, 512]]
    assert decoded[0][0] * MICROVOLTS_PER_COUNT == pytest.approx(0.11967469688352)
