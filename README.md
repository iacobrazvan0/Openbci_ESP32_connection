# OpenBCI Ganglion pe ESP32 prin BLE

Proiect PlatformIO independent pentru ESP32 DevKit V1 / ESP32-WROOM-32. Detecteaza
un Ganglion clasic, se conecteaza direct prin BLE, porneste fluxul binar si
decodeaza canalele EEG. Nu foloseste dongle USB si nu modifica firmware-ul placii.

## Fluxul implementat

1. ESP32 scaneaza activ dupa nume care incep cu `Ganglion` (inclusiv
   `Ganglion 1.3 (xxxx)`), aliasul vechi `Simblee` sau serviciul `0xFE84`.
2. Dupa conectare, verifica serviciul si caracteristicile Receive, Send si
   Disconnect, apoi se aboneaza la Receive.
3. Comanda ASCII `b` porneste fluxul (`s` il opreste).
4. Callback-ul BLE doar copiaza notificarile intr-o coada FreeRTOS. Decoderul
   ruleaza in `loop()`, iar toate esantioanele sunt trimise de o sarcina USB
   separata printr-o a doua coada, astfel incat Serial nu blocheaza callback-ul.
5. ID `0` furnizeaza baza pe 24 biti. ID-urile `1..100` contin doua esantioane
   delta pe 18 biti, iar `101..200` doua esantioane delta pe 19 biti.
6. Daca lipseste un pachet, lantul delta este invalidat pana la urmatorul ID `0`.
   Pachetele de impedanta (`201..205`) si text (`206..207`) nu sunt tratate ca EEG.
7. Valorile sunt convertite cu factorul oficial `15686 / 8388607`, in `uV/count`.

Codul decodeaza si transmite toate cele patru canale, fara decimarea veche la
20 Hz. Protocolul serial v1 contine secventa globala, ID de esantion, timestamp
reconstruit, timpul receptiei BLE, valorile in µV si discontinuitatile. Separarea
dintre BLE, decoder si transport permite ulterior un consumator Wi-Fi/MQTT.

Pentru placile care se anunta ca `Ganglion 1.3 (...)`, profilul firmware v3 este
selectat automat: caracteristica Disconnect devine optionala, iar pachetele
contin valori EEG comprimate absolute (nu un lant delta). Profilul v2 ramane
disponibil pentru placile vechi `Ganglion-*`/`Simblee`.

Descoperirea Receive/Send se face in toate serviciile GATT, deoarece unele
versiuni v3 nu le grupeaza identic cu profilul Simblee vechi. Inventarul GATT
este afisat la conectare pentru diagnosticarea firmware-urilor diferite.

## Compilare, incarcare si monitorizare

Din acest director:

```powershell
pio run -e esp32dev
pio run -e esp32dev -t upload --upload-port COM3
pio device monitor -b 921600 --port COM3
```

Portul COM poate fi schimbat. Rata de 921600 baud este necesara pentru toate cele
200 esantioane/s si cele patru canale. Biblioteca NimBLE-Arduino 2.x este instalata de
PlatformIO din `lib_deps`. In monitorul serial, `b` reporneste fluxul si `s` il
opreste. La fiecare 5 secunde sunt raportate notificari, pachete, esantioane,
pierderi, pachete invalide, depasiri ale cozii, rata efectiva si reconectari.

## Teste software

```powershell
pio test -e native
```

Testele acopera semnul pe 24 biti, scalarea, vectorul oficial OpenBCI pentru
delta pe 18 biti, delta pe 19 biti inclusiv valori negative si invalidarea dupa
pierderea sau duplicarea unui pachet. Mediul `native` include compilatorul MinGW
prin PlatformIO, deci nu necesita o instalare GCC separata pe Windows. Vectorii
construiti verifica protocolul, nu reprezinta EEG real.

## Test pe hardware

1. Alimenteaza Ganglion exclusiv din bateria sa. Nu exista legaturi electrice
   intre Ganglion si ESP32; comunicarea este numai BLE.
2. Pentru montaj referential, pune comutatoarele canalelor 1 si 2 in pozitia REF,
   conecteaza Fp1 la `1+`, Fp2 la `2+`, REF in spatele urechii stangi si D_G in
   spatele urechii drepte. Urmeaza documentatia si regulile de siguranta OpenBCI.
3. Porneste intai Ganglion, apoi ESP32. Urmareste detectarea, MAC/RSSI, conectarea,
   descoperirea GATT, abonarea si primele notificari.
4. Confirma o rata apropiata de 200 esantioane/s si absenta cresterii contoarelor
`lost_packets`, `decoder_errors`, `ble_queue_drops` si `serial_queue_drops`.
`sample_hz` este calculat numai dupa primul eșantion; vechea valoare de 140,4 Hz
includea scanarea/conectarea in prima fereastra si nu era rata de achizitie pura.
`notif_hz`, `conn_interval_ms`, timpul callback-ului si ambele cozi permit
separarea unei probleme reale BLE de una de masurare sau USB.
5. Verifica initial cu electrozii deconectati sau cu generatorul de test acceptat
   de procedura laboratorului, apoi executa achizitia umana numai conform
   protocolului etic si de siguranta al cercetarii. Sistemul nu este dispozitiv medical.

## Ce necesita validare fizica

- Publicarea serviciului FE84 de catre exemplarul vechi de Ganglion si numele sau.
- Stabilitatea intervalului BLE si rata efectiva de 200 Hz in mediul de test.
- Sensul/polaritatea canalelor si amplitudinile cu montajul concret de electrozi.
- Comportamentul la iesirea din raza radio si reconectarea placii reale.

## Dashboard Python

Codul, configuratia, testele si instructiunile Windows 11 sunt in
[`dashboard/README.md`](dashboard/README.md). Demo-ul este etichetat explicit ca
sintetic; pentru date reale inchide Serial Monitor si deschide acelasi COM din
dashboard.

Referinte: [formatul Ganglion](https://docs.openbci.com/Ganglion/GanglionDataFormat/),
[specificatiile Ganglion](https://docs.openbci.com/Ganglion/GanglionSpecs/),
[NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) si
[driverul BrainFlow Ganglion](https://github.com/brainflow-dev/brainflow/blob/master/src/board_controller/openbci/ganglion_native.cpp),
[procesarea OpenBCI GUI](https://github.com/OpenBCI/OpenBCI_GUI/blob/master/OpenBCI_GUI/DataProcessing.pde).
