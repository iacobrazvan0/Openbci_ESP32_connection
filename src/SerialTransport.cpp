#include "SerialTransport.h"

namespace {
constexpr UBaseType_t kQueueLength = 256;
}

bool SerialTransport::begin() {
  queue_ = xQueueCreate(kQueueLength, sizeof(SampleRecord));
  if (queue_ == nullptr) return false;
  return xTaskCreatePinnedToCore(taskEntry, "eeg-usb", 4096, this, 1, nullptr, 1) == pdPASS;
}

bool SerialTransport::enqueue(const SampleRecord& record) {
  if (queue_ != nullptr && xQueueSend(queue_, &record, 0) == pdTRUE) return true;
  ++drops_;
  return false;
}

uint32_t SerialTransport::queued() const {
  return queue_ == nullptr ? 0 : static_cast<uint32_t>(uxQueueMessagesWaiting(queue_));
}

void SerialTransport::taskEntry(void* context) {
  static_cast<SerialTransport*>(context)->run();
}

void SerialTransport::run() {
  SampleRecord record;
  for (;;) {
    if (xQueueReceive(queue_, &record, portMAX_DELAY) != pdTRUE) continue;
    Serial.printf(
        "EEG,v=1,seq=%llu,sample_id=%u,ts_us=%llu,rx_us=%llu,"
        "ch1_uv=%.6f,ch2_uv=%.6f,ch3_uv=%.6f,ch4_uv=%.6f,"
        "gap_packets=%u,discontinuity=%u\n",
        static_cast<unsigned long long>(record.sequence), record.sampleId,
        static_cast<unsigned long long>(record.timestampUs),
        static_cast<unsigned long long>(record.receivedAtUs), record.microvolts[0],
        record.microvolts[1], record.microvolts[2], record.microvolts[3],
        record.missingPackets, record.discontinuity ? 1U : 0U);
  }
}
