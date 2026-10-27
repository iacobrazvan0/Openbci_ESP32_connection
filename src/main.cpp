#include <Arduino.h>

#include "GanglionBLE.h"
#include "GanglionDecoder.h"
#include "SerialTransport.h"

namespace {
constexpr uint32_t kNominalSamplePeriodUs = 5000;
constexpr uint32_t kDiagnosticPeriodMs = 5000;

GanglionBLE ble;
ganglion::GanglionDecoder decoder;
SerialTransport transport;
uint64_t outputSequence = 0;
uint64_t nextSampleTimestampUs = 0;
uint32_t lastDiagnosticMs = 0;
uint32_t intervalSamples = 0;
uint32_t previousNotifications = 0;
bool rateWindowActive = false;

const char* packetKindName(ganglion::PacketKind kind) {
  switch (kind) {
    case ganglion::PacketKind::Raw: return "raw-24";
    case ganglion::PacketKind::Compressed18: return "compressed-18";
    case ganglion::PacketKind::Compressed19: return "compressed-19";
    case ganglion::PacketKind::Impedance: return "impedance";
    case ganglion::PacketKind::Ascii: return "ASCII";
    default: return "unknown";
  }
}

void processSerialCommands() {
  while (Serial.available() > 0) {
    const char command = static_cast<char>(Serial.read());
    if (command == 'b' || command == 'B') ble.startStreaming();
    if (command == 's' || command == 'S') ble.stopStreaming();
  }
}

void anchorSampleClock(const GanglionBLE::Packet& packet, uint8_t sampleCount) {
  const uint64_t packetSpan = sampleCount > 0
                                  ? static_cast<uint64_t>(sampleCount - 1U) * kNominalSamplePeriodUs
                                  : 0;
  nextSampleTimestampUs = packet.receivedAtUs > packetSpan
                              ? packet.receivedAtUs - packetSpan
                              : packet.receivedAtUs;
}

void processPackets() {
  decoder.setFirmware(ble.firmwareVersion() == 3 ? ganglion::Firmware::V3
                                                 : ganglion::Firmware::V2);
  GanglionBLE::Packet packet;
  while (ble.popPacket(packet)) {
    const ganglion::DecodeResult result = decoder.decode(packet.data, packet.length);
    if (result.discontinuity) {
      Serial.printf("EVENT,v=1,type=discontinuity,packet_id=%u,gap_packets=%u\n",
                    result.packetId, result.missingPackets);
      nextSampleTimestampUs = 0;
    }
    if (!result.valid && result.kind != ganglion::PacketKind::Impedance &&
        result.kind != ganglion::PacketKind::Ascii) {
      Serial.printf("EVENT,v=1,type=decoder_reject,kind=%s,packet_id=%u,length=%u\n",
                    packetKindName(result.kind), result.packetId, packet.length);
    }
    if (result.sampleCount == 0) continue;
    if (nextSampleTimestampUs == 0) anchorSampleClock(packet, result.sampleCount);

    for (uint8_t i = 0; i < result.sampleCount; ++i) {
      SerialTransport::SampleRecord record{};
      record.sequence = outputSequence++;
      record.sampleId = result.samples[i].sequence;
      record.timestampUs = nextSampleTimestampUs;
      record.receivedAtUs = packet.receivedAtUs;
      for (uint8_t channel = 0; channel < ganglion::kChannelCount; ++channel) {
        record.microvolts[channel] = result.samples[i].microvolts[channel];
      }
      record.missingPackets = i == 0 ? result.missingPackets : 0;
      record.discontinuity = i == 0 && result.discontinuity;
      transport.enqueue(record);
      nextSampleTimestampUs += kNominalSamplePeriodUs;
      ++intervalSamples;
    }

    if (!rateWindowActive) {
      rateWindowActive = true;
      intervalSamples = 0;
      previousNotifications = ble.notificationCount();
      lastDiagnosticMs = millis();
    }
  }
}

void printDiagnostics() {
  const uint32_t now = millis();
  if (!rateWindowActive || now - lastDiagnosticMs < kDiagnosticPeriodMs) return;
  const float elapsedSeconds = (now - lastDiagnosticMs) / 1000.0F;
  const uint32_t notifications = ble.notificationCount();
  const uint32_t intervalNotifications = notifications - previousNotifications;
  const auto& stats = decoder.stats();
  const float callbackAverageUs = notifications == 0
                                      ? 0.0F
                                      : static_cast<float>(ble.callbackTotalUs()) / notifications;
  Serial.printf(
      "DIAG,v=1,state=%s,notif=%lu,notif_hz=%.2f,packets=%lu,samples=%lu,"
      "sample_hz=%.2f,lost_packets=%lu,decoder_errors=%lu,ble_queue_drops=%lu,"
      "serial_queue_drops=%lu,serial_queue=%lu,reconnects=%lu,conn_interval_ms=%.2f,"
      "callback_avg_us=%.2f,callback_max_us=%lu,runtime_ms=%lu\n",
      ble.isReady() ? "streaming" : "disconnected",
      static_cast<unsigned long>(notifications), intervalNotifications / elapsedSeconds,
      static_cast<unsigned long>(stats.packets), static_cast<unsigned long>(stats.samples),
      intervalSamples / elapsedSeconds, static_cast<unsigned long>(stats.missingPackets),
      static_cast<unsigned long>(stats.invalidPackets),
      static_cast<unsigned long>(ble.queueDrops()),
      static_cast<unsigned long>(transport.drops()),
      static_cast<unsigned long>(transport.queued()),
      static_cast<unsigned long>(ble.reconnectCount()), ble.connectionIntervalMs(),
      callbackAverageUs, static_cast<unsigned long>(ble.callbackMaxUs()),
      static_cast<unsigned long>(now));
  intervalSamples = 0;
  previousNotifications = notifications;
  lastDiagnosticMs = now;
}
}  // namespace

void setup() {
  Serial.begin(921600);
  delay(500);
  Serial.println("STATUS,v=1,state=boot,device=OpenBCI-Ganglion-ESP32,baud=921600");
  Serial.println("STATUS,v=1,state=controls,commands=b:start|s:stop");
  if (!transport.begin()) Serial.println("STATUS,v=1,state=fatal,error=serial_queue");
  if (!ble.begin()) Serial.println("STATUS,v=1,state=fatal,error=ble_init");
}

void loop() {
  ble.loop();
  processSerialCommands();
  processPackets();
  printDiagnostics();
  delay(1);
}
