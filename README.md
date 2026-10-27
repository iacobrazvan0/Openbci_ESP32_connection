# OpenBCI Ganglion to ESP32 EEG Dashboard

This project connects an OpenBCI Ganglion directly to an ESP32 over Bluetooth
Low Energy, decodes all four EEG channels, and sends every reconstructed sample
to a Python desktop dashboard over USB Serial.

## Quick start: run the complete system

These instructions target Windows 11, VS Code, an ESP32 DevKit
(ESP32-WROOM-32), and an OpenBCI Ganglion.

### 1. Install the required software

Install:

- [Git](https://git-scm.com/download/win)
- [Visual Studio Code](https://code.visualstudio.com/)
- the PlatformIO IDE extension for VS Code
- Python 3.11

Open a new PowerShell terminal after installation and verify:

```powershell
git --version
pio --version
py -3.11 --version
```

### 2. Clone and open the project

```powershell
git clone https://github.com/iacobrazvan0/Openbci_ESP32_connection.git
cd Openbci_ESP32_connection
code .
```

All commands below must be run from the repository root unless another
directory is explicitly shown.

### 3. Connect and identify the ESP32

Connect the ESP32 by USB. In VS Code, use PlatformIO's device list, or run:

```powershell
pio device list
```

The examples use `COM3`. Replace it everywhere if your ESP32 uses another
port.

### 4. Build and upload the ESP32 firmware

Make sure no Serial Monitor or dashboard is using the COM port, then run:

```powershell
pio run -e esp32dev
pio run -e esp32dev -t upload --upload-port COM3
```

Turn on the Ganglion before or immediately after resetting the ESP32. The blue
Ganglion LED should blink while it is advertising.

### 5. Check the connection in Serial Monitor

```powershell
pio device monitor --port COM3 --baud 921600
```

Expected messages include Ganglion discovery, GATT discovery, notification
subscription, `EEG,v=1,...` samples, and `DIAG,v=1,...` diagnostics.

Serial commands:

- `b`: start or restart streaming
- `s`: stop streaming

Close Serial Monitor before starting the dashboard. Only one application can
open COM3 at a time.

### 6. Create the Python environment

```powershell
cd dashboard
py -3.11 -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install -r requirements.txt
```

If PowerShell blocks activation, activation is optional. Use the virtual
environment's Python executable directly:

```powershell
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
```

### 7. Run the synthetic dashboard demo

The demo is clearly labelled and does not contain real EEG data:

```powershell
python -m eeg_dashboard.app --demo
```

Without environment activation:

```powershell
.\.venv\Scripts\python.exe -m eeg_dashboard.app --demo
```

### 8. Run the dashboard with the ESP32

Close PlatformIO Serial Monitor, keep the ESP32 connected, and run:

```powershell
python -m eeg_dashboard.app --port COM3
```

Or:

```powershell
.\.venv\Scripts\python.exe -m eeg_dashboard.app --port COM3
```

The dashboard displays:

- raw or filtered Fp1 and Fp2 time series in microvolts;
- configurable 3, 5, or 10 second windows;
- instantaneous voltage and raw/filtered RMS in microvolts RMS;
- Welch PSD in microvolts squared per hertz;
- theta, alpha, and beta band power;
- sample rate, loss counters, decoder errors, and connection diagnostics.

Use **Start CSV recording** to save raw data, filtered data, timestamps,
discontinuities, and session metadata. Raw values are never overwritten by the
filter.

### 9. Run the automated tests

From the repository root:

```powershell
pio test -e native
cd dashboard
.\.venv\Scripts\python.exe -m pytest tests -q
```

## System architecture

```text
OpenBCI Ganglion
       |
       | BLE notifications
       v
ESP32 + NimBLE
       |
       | packet queue -> decoder -> USB output queue
       v
Versioned USB Serial protocol
       |
       v
Python acquisition -> raw buffer -> filtering -> dashboard / CSV
```

The BLE callback only copies each notification into a FreeRTOS queue. Decoding
runs outside the callback, and a separate FreeRTOS task sends samples over USB.
This prevents plotting or Serial output from blocking BLE notification
handling.

## Ganglion discovery and decoding

The ESP32 scans for:

- names beginning with `Ganglion`, including `Ganglion 1.3 (xxxx)`;
- the legacy `Simblee` name;
- devices advertising service `0xFE84`.

Receive and Send characteristics are searched across all discovered GATT
services because firmware v3 may use a different service layout. The Disconnect
characteristic is optional.

Firmware profiles:

- firmware v2: a 24-bit raw baseline followed by 18-bit or 19-bit compressed
  deltas, with two reconstructed samples per compressed packet;
- firmware v3: 18-bit or 19-bit compressed absolute values, with two samples
  per packet.

ADC counts are converted with:

```text
0.001869917138805 microvolts/count
```

The decoder detects duplicates, invalid packets, missing packets, and
discontinuities. Firmware v2 invalidates the delta chain after packet loss until
a new raw baseline is received. Firmware v3 packets remain independently
decodable because they contain absolute compressed values.

## USB Serial protocol

The firmware uses `921600 baud` so all samples and four channels can be
transmitted without the previous 20 Hz display decimation.

Sample message:

```text
EEG,v=1,seq=42,sample_id=84,ts_us=1230000,rx_us=1234100,ch1_uv=-12.3,ch2_uv=4.5,ch3_uv=0.1,ch4_uv=-0.2,gap_packets=0,discontinuity=0
```

Diagnostic message:

```text
DIAG,v=1,state=streaming,notif_hz=100.0,sample_hz=200.0,lost_packets=0,...
```

`ts_us` is the reconstructed 200 Hz sample timeline. `rx_us` records BLE
notification arrival time and therefore includes transport jitter.

## Signal processing

The Python pipeline provides:

- configurable Butterworth band-pass, default 1-40 Hz;
- configurable 50 Hz notch, default Q=30;
- DC offset removal;
- causal real-time filtering;
- zero-phase offline filtering for validation;
- filter resets at acquisition discontinuities;
- separate raw and filtered RMS, default one-second window;
- Welch PSD in microvolts squared per hertz;
- theta 4-8 Hz, alpha 8-13 Hz, and beta 13-30 Hz power.

The technical quality indicator checks insufficient data, gaps, near-rail
values, flat signals, and unusually high RMS. It does not certify that a signal
is valid physiological EEG.

## Comparing recordings with OpenBCI GUI

The validator compares aggregate sampling rate, RMS, peak-to-peak amplitude,
PSD, and band power:

```powershell
cd dashboard
python -m eeg_dashboard.validator --gui OpenBCI-RAW.txt --esp32 recordings\ganglion.csv --output report.json
```

Recordings from different sessions are not aligned sample by sample. Exact
decoder validation instead uses identical raw BLE packets in the C++ decoder
and the independent Python reference decoder.

OpenBCI GUI stores unfiltered raw recordings. Its current displayed
`uVrms` value is based on the standard deviation of the most recent second of
filtered data. This is equal to mathematical RMS only when the signal mean is
approximately zero, so identical values are not assumed without matching all
processing parameters.

## Diagnosing an incorrect sample rate

The earlier 140.4 Hz result included BLE scanning and connection time in the
first measurement window. The current firmware starts rate measurement only
after the first decoded sample and reports separately:

- `notif_hz`;
- `sample_hz`;
- `conn_interval_ms`;
- callback average and maximum duration;
- BLE queue drops;
- Serial queue drops;
- decoder errors and missing packets.

Do not change BLE timing merely to make the displayed rate approach 200 Hz.
Use these measurements to distinguish a calculation problem from BLE loss or
USB backpressure.

## Hardware and research safety

- Power the Ganglion only as specified by OpenBCI, normally from its battery.
- The Ganglion and ESP32 communicate wirelessly; do not create an unnecessary
  electrical connection between them.
- Verify electrode placement, reference, ground, polarity, and impedance using
  the laboratory protocol.
- Start with an approved test source or disconnected inputs before human
  acquisition.
- Follow the applicable ethical and participant-safety protocol.
- This project is a research prototype, not a medical device.

Hardware still requires validation for the actual Ganglion unit, RF
environment, electrode montage, signal polarity, amplitudes, reconnection, and
long-duration stability.

## Troubleshooting

### COM port is busy or access is denied

Close PlatformIO Serial Monitor, the dashboard, Arduino Serial Monitor, and any
other program using the port. Then retry the upload or dashboard.

### The dashboard cannot import NumPy, SciPy, or PySide6

Run the application with the virtual environment:

```powershell
cd dashboard
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
.\.venv\Scripts\python.exe -m eeg_dashboard.app --demo
```

### PowerShell blocks `run_dashboard.ps1`

Use the direct Python commands above, or:

```powershell
powershell -ExecutionPolicy Bypass -File .\run_dashboard.ps1 -Port COM3
```

## Project layout

```text
include/                 Firmware headers
src/                     ESP32 BLE, decoder, transport, and application code
test/test_decoder/       Native C++ decoder tests
dashboard/eeg_dashboard/ Python acquisition, processing, UI, recording, validation
dashboard/tests/         Python automated tests
dashboard/config.json    Serial, filter, display, and quality configuration
```

More dashboard details are available in
[`dashboard/README.md`](dashboard/README.md).

## References

- [OpenBCI Ganglion data format](https://docs.openbci.com/Ganglion/GanglionDataFormat/)
- [OpenBCI Ganglion specifications](https://docs.openbci.com/Ganglion/GanglionSpecs/)
- [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino)
- [BrainFlow Ganglion driver](https://github.com/brainflow-dev/brainflow/blob/master/src/board_controller/openbci/ganglion_native.cpp)
- [OpenBCI GUI processing](https://github.com/OpenBCI/OpenBCI_GUI/blob/master/OpenBCI_GUI/DataProcessing.pde)
