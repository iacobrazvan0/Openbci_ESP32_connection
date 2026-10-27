import pytest

from eeg_dashboard.models import Diagnostic, EEGSample
from eeg_dashboard.protocol import ProtocolError, parse_line


def test_parses_versioned_eeg_sample():
    line = "EEG,v=1,seq=7,sample_id=9,ts_us=10000,rx_us=10100,ch1_uv=-1.25,ch2_uv=2,ch3_uv=0,ch4_uv=0,gap_packets=1,discontinuity=1"
    sample = parse_line(line)
    assert isinstance(sample, EEGSample)
    assert sample.channels_uv[:2] == (-1.25, 2.0)
    assert sample.discontinuity and sample.gap_packets == 1


def test_parses_diagnostics_separately():
    diagnostic = parse_line("DIAG,v=1,state=streaming,sample_hz=199.8,lost_packets=0")
    assert isinstance(diagnostic, Diagnostic)
    assert diagnostic.number("sample_hz") == pytest.approx(199.8)


def test_rejects_incomplete_sample():
    with pytest.raises(ProtocolError):
        parse_line("EEG,v=1,seq=1")
