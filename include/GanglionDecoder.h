#pragma once

#include <cstddef>
#include <cstdint>

namespace ganglion {

constexpr std::size_t kChannelCount = 4;
constexpr std::size_t kPacketSize = 20;
// OpenBCI Ganglion scale documented by OpenBCI/BrainFlow, in uV/count.
constexpr double kMicrovoltsPerCount = 0.001869917138805;

enum class PacketKind : uint8_t {
  Raw,
  Compressed18,
  Compressed19,
  Impedance,
  Ascii,
  Unknown
};

enum class Firmware : uint8_t { V2 = 2, V3 = 3 };

struct Sample {
  uint16_t sequence = 0;
  int32_t counts[kChannelCount]{};
  double microvolts[kChannelCount]{};
};

struct DecodeResult {
  PacketKind kind = PacketKind::Unknown;
  Sample samples[2]{};
  uint8_t sampleCount = 0;
  uint8_t packetId = 0;
  uint8_t missingPackets = 0;
  bool valid = false;
  bool discontinuity = false;
};

struct DecoderStats {
  uint32_t packets = 0;
  uint32_t eegPackets = 0;
  uint32_t invalidPackets = 0;
  uint32_t missingPackets = 0;
  uint32_t samples = 0;
  uint32_t impedancePackets = 0;
  uint32_t asciiPackets = 0;
};

class GanglionDecoder {
 public:
  DecodeResult decode(const uint8_t* packet, std::size_t length);
  void reset();
  void setFirmware(Firmware firmware);
  Firmware firmware() const { return firmware_; }
  const DecoderStats& stats() const { return stats_; }
  bool hasBaseline() const { return hasBaseline_; }

  static int32_t signed24(const uint8_t* bytes);
  static void unpackDeltas(const uint8_t* bytes, uint8_t width,
                           int32_t output[2][kChannelCount]);

 private:
  static PacketKind classify(uint8_t id);
  static uint8_t normalizedId(uint8_t id);
  static void unpackFirmware3(const uint8_t* bytes, uint8_t width,
                              int32_t output[2][kChannelCount]);
  DecodeResult decodeFirmware3(const uint8_t* packet);
  uint8_t trackContinuity(uint8_t normalizedId, uint8_t cycleLength);
  void fillSample(Sample& sample, uint16_t sequence);

  int32_t lastCounts_[kChannelCount]{};
  bool hasBaseline_ = false;
  bool hasLastPacketId_ = false;
  uint8_t lastPacketId_ = 0;
  Firmware firmware_ = Firmware::V2;
  DecoderStats stats_{};
};

}  // namespace ganglion

