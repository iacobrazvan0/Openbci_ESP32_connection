#include "GanglionBLE.h"

#include <esp_timer.h>

namespace {
constexpr char kServiceUuid[] = "fe84";
constexpr char kReceiveUuid[] = "2d30c082-f39f-4ce6-923f-3484ea480596";
constexpr char kSendUuid[] = "2d30c083-f39f-4ce6-923f-3484ea480596";
constexpr char kDisconnectUuid[] = "2d30c084-f39f-4ce6-923f-3484ea480596";
constexpr uint32_t kScanDurationMs = 5000;
constexpr uint32_t kReconnectDelayMs = 2000;
}  // namespace

GanglionBLE* GanglionBLE::instance_ = nullptr;

class GanglionBLE::ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* device) override {
    if (instance_ != nullptr) instance_->onAdvertisement(device);
  }
};

class GanglionBLE::ClientCallbacks : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient* client) override {
    if (instance_ != nullptr) {
      instance_->connectionIntervalUnits_ = client->getConnInfo().getConnInterval();
    }
    Serial.println("[BLE] Conexiune GAP stabilita.");
  }

  void onDisconnect(NimBLEClient*, int reason) override {
    if (instance_ != nullptr) instance_->onDisconnected(reason);
  }
};

GanglionBLE::GanglionBLE() { instance_ = this; }

bool GanglionBLE::begin() {
  packetQueue_ = xQueueCreate(32, sizeof(Packet));
  if (packetQueue_ == nullptr) {
    Serial.println("[BLE][EROARE] Nu s-a putut crea coada de pachete.");
    return false;
  }
  Serial.println("[BLE] Initializare NimBLE...");
  NimBLEDevice::init("");
  scanCallbacks_ = new ScanCallbacks();
  clientCallbacks_ = new ClientCallbacks();
  scan_ = NimBLEDevice::getScan();
  scan_->setScanCallbacks(scanCallbacks_, false);
  scan_->setActiveScan(true);
  scan_->setInterval(100);
  scan_->setWindow(80);
  startScan();
  return true;
}

void GanglionBLE::startScan() {
  if (scan_ == nullptr || scan_->isScanning()) return;
  target_ = nullptr;
  state_ = State::Scanning;
  Serial.println("[BLE] Scanare Ganglion (nume Ganglion-* sau serviciu FE84)...");
  scan_->start(kScanDurationMs, false, true);
  nextScanAtMs_ = millis() + kScanDurationMs + kReconnectDelayMs;
}

void GanglionBLE::onAdvertisement(const NimBLEAdvertisedDevice* device) {
  const std::string name = device->haveName() ? device->getName() : "";
  // Legacy/v3 boards may advertise as "Ganglion 1.3 (xxxx)" rather than
  // "Ganglion-xxxx"; early Simblee firmware may use only "Simblee".
  const bool nameMatches = name.rfind("Ganglion", 0) == 0 || name == "Simblee";
  const bool serviceMatches = device->isAdvertisingService(NimBLEUUID(kServiceUuid));
  if (!nameMatches && !serviceMatches) return;

  Serial.printf("[BLE] Ganglion detectat: nume=%s, MAC=%s, RSSI=%d dBm\n",
                name.empty() ? "(neanuntat)" : name.c_str(),
                device->getAddress().toString().c_str(), device->getRSSI());
  firmwareVersion_ = name.rfind("Ganglion 1.3", 0) == 0 ? 3 : 2;
  Serial.printf("[BLE] Profil firmware detectat: v%u.\n", firmwareVersion_);
  target_ = device;
  state_ = State::DeviceFound;
  scan_->stop();
}

bool GanglionBLE::connectAndDiscover() {
  if (target_ == nullptr) return false;
  state_ = State::Connecting;
  client_ = NimBLEDevice::getDisconnectedClient();
  if (client_ == nullptr) client_ = NimBLEDevice::createClient();
  if (client_ == nullptr) {
    Serial.println("[BLE][EROARE] Nu s-a putut crea clientul BLE.");
    return false;
  }
  client_->setClientCallbacks(clientCallbacks_, false);
  client_->setConnectTimeout(10000);
  client_->setConnectionParams(6, 12, 0, 200);

  Serial.println("[BLE] Conectare...");
  if (!client_->connect(target_)) {
    Serial.printf("[BLE][EROARE] Conectare esuata, cod=%d.\n", client_->getLastError());
    return false;
  }
  connected_ = true;
  connectionIntervalUnits_ = client_->getConnInfo().getConnInterval();

  // Firmware v3 may expose the Ganglion characteristics under a service layout
  // different from the legacy Simblee profile. Discover and search globally,
  // matching the strategy used by the current BrainFlow native driver.
  receive_ = nullptr;
  send_ = nullptr;
  disconnect_ = nullptr;
  bool hasFe84 = false;
  const NimBLEUUID receiveUuid(kReceiveUuid);
  const NimBLEUUID sendUuid(kSendUuid);
  const NimBLEUUID disconnectUuid(kDisconnectUuid);
  const auto& services = client_->getServices(true);
  Serial.printf("[BLE] Inventar GATT: %u servicii.\n",
                static_cast<unsigned>(services.size()));
  for (const auto* service : services) {
    if (service == nullptr) continue;
    const std::string serviceId = service->getUUID().toString();
    if (service->getUUID() == NimBLEUUID(kServiceUuid)) hasFe84 = true;
    const auto& characteristics = service->getCharacteristics(true);
    Serial.printf("[BLE][GATT] serviciu=%s, caracteristici=%u\n", serviceId.c_str(),
                  static_cast<unsigned>(characteristics.size()));
    for (const auto* characteristic : characteristics) {
      if (characteristic == nullptr) continue;
      const std::string characteristicId = characteristic->getUUID().toString();
      Serial.printf("[BLE][GATT]   %s [N=%u W=%u WNR=%u]\n",
                    characteristicId.c_str(), characteristic->canNotify(),
                    characteristic->canWrite(), characteristic->canWriteNoResponse());
      if (characteristic->getUUID() == receiveUuid) receive_ = characteristic;
      if (characteristic->getUUID() == sendUuid) send_ = characteristic;
      if (characteristic->getUUID() == disconnectUuid) disconnect_ = characteristic;
    }
  }
  Serial.printf("[BLE] Serviciul FE84: %s\n", hasFe84 ? "prezent" : "absent");
  Serial.printf("[BLE] Caracteristici: receive=%s, send=%s, disconnect=%s\n",
                receive_ != nullptr ? "OK" : "LIPSA",
                send_ != nullptr ? "OK" : "LIPSA",
                disconnect_ != nullptr ? "OK" : "LIPSA/OPTIONAL");
  if (receive_ == nullptr || send_ == nullptr) {
    Serial.println("[BLE][EROARE] Lipseste caracteristica Receive sau Send.");
    client_->disconnect();
    return false;
  }
  if (!receive_->canNotify() || (!send_->canWrite() && !send_->canWriteNoResponse())) {
    Serial.println("[BLE][EROARE] Proprietatile caracteristicilor sunt incompatibile.");
    client_->disconnect();
    return false;
  }
  if (disconnect_ != nullptr && !disconnect_->canWrite() && !disconnect_->canWriteNoResponse()) {
    Serial.println("[BLE][AVERTISMENT] Caracteristica Disconnect nu permite scrierea.");
  }
  Serial.println("[BLE] Serviciul si caracteristicile de flux au fost verificate.");
  if (!receive_->subscribe(true, notificationCallback, true)) {
    Serial.println("[BLE][EROARE] Abonarea la notificari a esuat.");
    client_->disconnect();
    return false;
  }
  Serial.println("[BLE] Abonare la notificari confirmata.");
  state_ = State::Ready;
  return true;
}

void GanglionBLE::loop() {
  if (state_ == State::DeviceFound) {
    if (connectAndDiscover()) {
      if (!startStreaming()) {
        Serial.println("[BLE][EROARE] Fluxul nu a pornit; inchid conexiunea.");
        client_->disconnect();
      }
    } else {
      connected_ = false;
      state_ = State::Idle;
      nextScanAtMs_ = millis() + kReconnectDelayMs;
    }
  }
  if ((state_ == State::Idle || state_ == State::Scanning) && !scan_->isScanning() &&
      static_cast<int32_t>(millis() - nextScanAtMs_) >= 0) {
    if (state_ == State::Scanning) Serial.println("[BLE] Ganglion negasit; reiau scanarea.");
    startScan();
  }
}

bool GanglionBLE::sendCommand(char command) {
  if (!isReady() || send_ == nullptr) return false;
  const bool response = send_->canWrite();
  const bool ok = send_->writeValue(reinterpret_cast<const uint8_t*>(&command), 1, response);
  Serial.printf("[BLE] Comanda '%c': %s\n", command, ok ? "trimisa" : "ESUATA");
  return ok;
}

bool GanglionBLE::startStreaming() { return sendCommand('b'); }
bool GanglionBLE::stopStreaming() { return sendCommand('s'); }

void GanglionBLE::notificationCallback(NimBLERemoteCharacteristic*, uint8_t* data,
                                       size_t length, bool) {
  if (instance_ == nullptr) return;
  const int64_t startedAt = esp_timer_get_time();
  instance_->enqueue(data, length);
  const uint32_t elapsed = static_cast<uint32_t>(esp_timer_get_time() - startedAt);
  instance_->callbackTotalUs_ += elapsed;
  if (elapsed > instance_->callbackMaxUs_) instance_->callbackMaxUs_ = elapsed;
}

void GanglionBLE::enqueue(const uint8_t* data, size_t length) {
  ++notificationCount_;
  if (length == 0 || length > sizeof(Packet::data)) {
    ++queueDrops_;
    return;
  }
  Packet packet{};
  packet.length = static_cast<uint8_t>(length);
  packet.receivedAtUs = static_cast<uint64_t>(esp_timer_get_time());
  memcpy(packet.data, data, length);
  if (xQueueSend(packetQueue_, &packet, 0) != pdTRUE) ++queueDrops_;
}

bool GanglionBLE::popPacket(Packet& packet) {
  return packetQueue_ != nullptr && xQueueReceive(packetQueue_, &packet, 0) == pdTRUE;
}

void GanglionBLE::onDisconnected(int reason) {
  Serial.printf("[BLE] Deconectat (motiv=%d); reconectare controlata.\n", reason);
  connected_ = false;
  receive_ = nullptr;
  send_ = nullptr;
  disconnect_ = nullptr;
  state_ = State::Idle;
  ++reconnectCount_;
  nextScanAtMs_ = millis() + kReconnectDelayMs;
  if (packetQueue_ != nullptr) xQueueReset(packetQueue_);
}

