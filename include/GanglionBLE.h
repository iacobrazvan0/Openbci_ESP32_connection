#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

class GanglionBLE {
 public:
  struct Packet {
    uint8_t data[20]{};
    uint8_t length = 0;
    uint64_t receivedAtUs = 0;
  };

  enum class State : uint8_t { Idle, Scanning, DeviceFound, Connecting, Ready };

  GanglionBLE();
  bool begin();
  void loop();
  bool startStreaming();
  bool stopStreaming();
  bool popPacket(Packet& packet);
  bool isReady() const { return state_ == State::Ready && connected_; }
  State state() const { return state_; }
  uint32_t notificationCount() const { return notificationCount_; }
  uint32_t queueDrops() const { return queueDrops_; }
  uint32_t reconnectCount() const { return reconnectCount_; }
  uint8_t firmwareVersion() const { return firmwareVersion_; }
  float connectionIntervalMs() const { return connectionIntervalUnits_ * 1.25F; }
  uint32_t callbackMaxUs() const { return callbackMaxUs_; }
  uint64_t callbackTotalUs() const { return callbackTotalUs_; }

 private:
  class ScanCallbacks;
  class ClientCallbacks;
  friend class ScanCallbacks;
  friend class ClientCallbacks;

  static void notificationCallback(NimBLERemoteCharacteristic*, uint8_t* data,
                                   size_t length, bool);
  void onAdvertisement(const NimBLEAdvertisedDevice* device);
  void onDisconnected(int reason);
  void startScan();
  bool connectAndDiscover();
  bool sendCommand(char command);
  void enqueue(const uint8_t* data, size_t length);

  static GanglionBLE* instance_;
  ScanCallbacks* scanCallbacks_ = nullptr;
  ClientCallbacks* clientCallbacks_ = nullptr;
  NimBLEScan* scan_ = nullptr;
  NimBLEClient* client_ = nullptr;
  const NimBLEAdvertisedDevice* target_ = nullptr;
  const NimBLERemoteCharacteristic* receive_ = nullptr;
  const NimBLERemoteCharacteristic* send_ = nullptr;
  const NimBLERemoteCharacteristic* disconnect_ = nullptr;
  QueueHandle_t packetQueue_ = nullptr;
  volatile State state_ = State::Idle;
  volatile bool connected_ = false;
  volatile uint32_t notificationCount_ = 0;
  volatile uint32_t queueDrops_ = 0;
  volatile uint64_t callbackTotalUs_ = 0;
  volatile uint32_t callbackMaxUs_ = 0;
  uint32_t reconnectCount_ = 0;
  uint32_t nextScanAtMs_ = 0;
  uint8_t firmwareVersion_ = 2;
  uint16_t connectionIntervalUnits_ = 0;
};

