#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

class SerialTransport {
 public:
  struct SampleRecord {
    uint64_t sequence = 0;
    uint16_t sampleId = 0;
    uint64_t timestampUs = 0;
    uint64_t receivedAtUs = 0;
    double microvolts[4]{};
    uint8_t missingPackets = 0;
    bool discontinuity = false;
  };

  bool begin();
  bool enqueue(const SampleRecord& record);
  uint32_t drops() const { return drops_; }
  uint32_t queued() const;

 private:
  static void taskEntry(void* context);
  void run();

  QueueHandle_t queue_ = nullptr;
  volatile uint32_t drops_ = 0;
};
