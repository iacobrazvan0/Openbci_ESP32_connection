#include "GanglionDecoder.h"

namespace ganglion {

PacketKind GanglionDecoder::classify(uint8_t id) {
  if (id == 0) return PacketKind::Raw;
  if (id <= 100) return PacketKind::Compressed18;
  if (id <= 200) return PacketKind::Compressed19;
  if (id <= 205) return PacketKind::Impedance;
  if (id <= 207) return PacketKind::Ascii;
  return PacketKind::Unknown;
}

uint8_t GanglionDecoder::normalizedId(uint8_t id) {
  return id > 100 && id <= 200 ? static_cast<uint8_t>(id - 100) : id;
}

int32_t GanglionDecoder::signed24(const uint8_t* bytes) {
  uint32_t value = (static_cast<uint32_t>(bytes[0]) << 16) |
                   (static_cast<uint32_t>(bytes[1]) << 8) | bytes[2];
  if ((value & 0x00800000UL) != 0) value |= 0xFF000000UL;
  return static_cast<int32_t>(value);
}

void GanglionDecoder::unpackDeltas(const uint8_t* bytes, uint8_t width,
                                   int32_t output[2][kChannelCount]) {
  for (uint8_t valueIndex = 0; valueIndex < 8; ++valueIndex) {
    uint32_t packed = 0;
    const uint16_t firstBit = static_cast<uint16_t>(valueIndex) * width;
    for (uint8_t bit = 0; bit < width; ++bit) {
      const uint16_t streamBit = firstBit + bit;
      packed = (packed << 1) |
               ((bytes[streamBit / 8] >> (7 - (streamBit % 8))) & 1U);
    }

    // Ganglion stores the sign in bit 0. Negative encoded values already
    // contain the lower-width two's-complement bits; extend from that flag.
    if ((packed & 1U) != 0) packed |= (UINT32_MAX << width);
    output[valueIndex / 4][valueIndex % 4] = static_cast<int32_t>(packed);
  }
}

void GanglionDecoder::unpackFirmware3(const uint8_t* bytes, uint8_t width,
                                      int32_t output[2][kChannelCount]) {
  const uint32_t mask = (1UL << width) - 1UL;
  const uint32_t signMask = 1UL << (width - 1U);
  const int32_t scale = width == 18 ? 64 : 32;
  for (uint8_t valueIndex = 0; valueIndex < 8; ++valueIndex) {
    uint32_t packed = 0;
    const uint16_t firstBit = static_cast<uint16_t>(valueIndex) * width;
    for (uint8_t bit = 0; bit < width; ++bit) {
      const uint16_t streamBit = firstBit + bit;
      packed = (packed << 1) |
               ((bytes[streamBit / 8] >> (7 - (streamBit % 8))) & 1U);
    }
    // Firmware 3 transmits the sign plus the most-significant ADC bits.
    // Match BrainFlow's Ganglion conversion quirk for negative values.
    const int32_t reduced = (packed & signMask) != 0
                                ? -static_cast<int32_t>((~packed) & mask) - 2
                                : static_cast<int32_t>(packed);
    output[valueIndex / 4][valueIndex % 4] = reduced * scale;
  }
}

uint8_t GanglionDecoder::trackContinuity(uint8_t id, uint8_t cycleLength) {
  if (!hasLastPacketId_) {
    hasLastPacketId_ = true;
    lastPacketId_ = id;
    return 0;
  }
  const uint8_t missing =
      static_cast<uint8_t>((id + cycleLength - lastPacketId_ - 1U) % cycleLength);
  lastPacketId_ = id;
  return missing;
}

void GanglionDecoder::fillSample(Sample& sample, uint16_t sequence) {
  sample.sequence = sequence;
  for (std::size_t channel = 0; channel < kChannelCount; ++channel) {
    sample.counts[channel] = lastCounts_[channel];
    sample.microvolts[channel] = lastCounts_[channel] * kMicrovoltsPerCount;
  }
}

DecodeResult GanglionDecoder::decode(const uint8_t* packet, std::size_t length) {
  DecodeResult result{};
  ++stats_.packets;
  if (packet == nullptr || length == 0) {
    ++stats_.invalidPackets;
    return result;
  }

  result.packetId = packet[0];
  if (firmware_ == Firmware::V3 && length == kPacketSize && result.packetId < 200) {
    return decodeFirmware3(packet);
  }
  result.kind = classify(result.packetId);
  if (result.kind == PacketKind::Impedance) {
    ++stats_.impedancePackets;
    result.valid = length >= 2;
    if (!result.valid) ++stats_.invalidPackets;
    return result;
  }
  if (result.kind == PacketKind::Ascii) {
    ++stats_.asciiPackets;
    result.valid = length >= 2;
    if (!result.valid) ++stats_.invalidPackets;
    return result;
  }
  if (result.kind == PacketKind::Unknown || length != kPacketSize) {
    ++stats_.invalidPackets;
    return result;
  }

  ++stats_.eegPackets;
  const uint8_t packetNumber = normalizedId(result.packetId);
  if (hasLastPacketId_ && packetNumber == lastPacketId_) {
    result.discontinuity = true;
    hasBaseline_ = false;
    ++stats_.invalidPackets;
    return result;
  }
  result.missingPackets = trackContinuity(packetNumber, 101);
  if (result.missingPackets != 0) {
    result.discontinuity = true;
    stats_.missingPackets += result.missingPackets;
    hasBaseline_ = false;
  }

  if (result.kind == PacketKind::Raw) {
    for (std::size_t channel = 0; channel < kChannelCount; ++channel) {
      lastCounts_[channel] = signed24(packet + 1 + channel * 3);
    }
    hasBaseline_ = true;
    fillSample(result.samples[0], 0);
    result.sampleCount = 1;
    result.valid = true;
    ++stats_.samples;
    return result;
  }

  if (!hasBaseline_) {
    ++stats_.invalidPackets;
    return result;
  }

  int32_t deltas[2][kChannelCount]{};
  unpackDeltas(packet + 1,
               result.kind == PacketKind::Compressed18 ? 18 : 19, deltas);
  for (uint8_t sampleIndex = 0; sampleIndex < 2; ++sampleIndex) {
    for (std::size_t channel = 0; channel < kChannelCount; ++channel) {
      lastCounts_[channel] -= deltas[sampleIndex][channel];
    }
    fillSample(result.samples[sampleIndex],
               static_cast<uint16_t>((packetNumber - 1U) * 2U + sampleIndex + 1U));
  }
  result.sampleCount = 2;
  result.valid = true;
  stats_.samples += 2;
  return result;
}

DecodeResult GanglionDecoder::decodeFirmware3(const uint8_t* packet) {
  DecodeResult result{};
  result.packetId = packet[0];
  result.kind = result.packetId < 100 ? PacketKind::Compressed18 : PacketKind::Compressed19;
  ++stats_.eegPackets;

  const uint8_t packetNumber = static_cast<uint8_t>(result.packetId % 100U);
  if (hasLastPacketId_ && packetNumber == lastPacketId_) {
    result.discontinuity = true;
    ++stats_.invalidPackets;
    return result;
  }
  result.missingPackets = trackContinuity(packetNumber, 100);
  if (result.missingPackets != 0) {
    result.discontinuity = true;
    stats_.missingPackets += result.missingPackets;
  }

  int32_t values[2][kChannelCount]{};
  unpackFirmware3(packet + 1,
                  result.kind == PacketKind::Compressed18 ? 18 : 19, values);
  for (uint8_t sampleIndex = 0; sampleIndex < 2; ++sampleIndex) {
    for (std::size_t channel = 0; channel < kChannelCount; ++channel) {
      lastCounts_[channel] = values[sampleIndex][channel];
    }
    fillSample(result.samples[sampleIndex],
               static_cast<uint16_t>(packetNumber * 2U + sampleIndex));
  }
  hasBaseline_ = true;
  result.sampleCount = 2;
  result.valid = true;
  stats_.samples += 2;
  return result;
}

void GanglionDecoder::setFirmware(Firmware firmware) {
  if (firmware_ == firmware) return;
  firmware_ = firmware;
  reset();
}

void GanglionDecoder::reset() {
  for (auto& value : lastCounts_) value = 0;
  hasBaseline_ = false;
  hasLastPacketId_ = false;
  lastPacketId_ = 0;
  stats_ = DecoderStats{};
}

}  // namespace ganglion

