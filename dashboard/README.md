# Dashboard EEG

Aplicatie desktop PySide6/PyQtGraph pentru fluxul serial `Ganglion -> ESP32`.
Semnalul brut ramane in memorie si in CSV; filtrul nu suprascrie coloanele raw.

## Instalare pe Windows 11 / VS Code

Din terminalul VS Code, in directorul `dashboard`:

```powershell
py -3.11 -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install -r requirements.txt
$env:PYTHONPATH = (Get-Location).Path
pytest -q
python -m eeg_dashboard.app --demo
python -m eeg_dashboard.app --port COM3
```

Alternativ, scriptul poate fi pornit chiar daca politica PowerShell blocheaza
scripturile locale:

```powershell
powershell -ExecutionPolicy Bypass -File .\run_dashboard.ps1 -Port COM3
```

Inainte de rularea reala, inchide PlatformIO Serial Monitor: un port COM poate fi
deschis de o singura aplicatie. Portul si parametrii se pot schimba in
`config.json`. Modul demo are banner rosu si genereaza determinist sinusuri la
10 Hz si 6 Hz plus zgomot; nu reprezinta EEG uman.

## Protocol serial v1

Baud: `921600`, o linie UTF-8 per mesaj:

```text
EEG,v=1,seq=42,sample_id=84,ts_us=1230000,rx_us=1234100,ch1_uv=-12.3,ch2_uv=4.5,ch3_uv=0.1,ch4_uv=-0.2,gap_packets=0,discontinuity=0
DIAG,v=1,state=streaming,notif_hz=100.0,sample_hz=200.0,...
```

`ts_us` este grila nominala reconstruita la 200 Hz, raportata la timpul de boot
ESP32. `rx_us` este timpul de sosire BLE si include jitter. `seq` este monoton pe
eșantioanele efectiv transmise, iar `sample_id` provine din ID-ul Ganglion.
O discontinuitate este pastrata explicit; nu se interpoleaza automat.

## Procesare si unitati

- band-pass Butterworth ordin 4, implicit 1-40 Hz;
- notch IIR 50 Hz, Q=30;
- componenta DC este eliminata de treapta high-pass si explicit prin scaderea
  mediei in analiza offline;
- filtrul live este cauzal si se reseteaza la gap; filtrul validatorului este
  zero-phase si proceseaza separat fiecare segment continuu;
- RMS: `sqrt(mean(x^2))`, fereastra implicita 1 s, separat raw/filtered, in µVrms;
- PSD Welch in µV²/Hz; puterea theta 4-8 Hz, alpha 8-13 Hz si beta 13-30 Hz
  este integrala PSD, in µV².

Calitatea tehnica raporteaza: date insuficiente, gap, apropiere de rail/clipping
(15 000 µV), semnal aproape plat (`std < 0.5 µV`) sau amplitudine mare
(`RMS > 200 µV`). Pragurile sunt configurabile si nu confirma ca semnalul este
EEG fiziologic valid. Contactul, impedanta, montajul, artefactele si protocolul
experimental trebuie verificate separat.

## CSV si validare

Butonul de inregistrare creeaza CSV cu raw, filtered, timestamp, timp de receptie,
secvente si gap-uri, plus un fisier `.metadata.json` cu unitati si filtre.

Comparatie agregata cu un export RAW OpenBCI GUI:

```powershell
python -m eeg_dashboard.validator --gui OpenBCI-RAW.txt --esp32 recordings\ganglion.csv --output report.json
```

Validatorul compara rata, peak-to-peak, RMS si puteri de banda. Nu aliniaza
eșantioane din sesiuni diferite. Pentru validarea exacta a decodorului, aceeasi
linie BLE hex poate fi trecuta prin implementarea Python independenta:

```powershell
python -m eeg_dashboard.decoder_reference <40-hex-digits> --firmware 3
```

## Relatia cu OpenBCI GUI

Codul OpenBCI GUI curent copiaza raw intr-un buffer filtrat, aplica filtrele
BrainFlow configurate si calculeaza pe ultima secunda `std(filtered)`, afisand-o
ca `uVrms`. Aceasta coincide cu RMS doar pentru semnal cu medie zero. GUI salveaza
datele RAW nefiltrate. FFT-ul GUI este un spectru de amplitudine cu fereastra
Hamming si smoothing; dashboard-ul foloseste deliberat Welch PSD. Pentru o
comparatie valida trebuie fixate aceleasi limite, ordine, tipuri de filtru si
fereastra; valorile nu sunt declarate identice implicit.
