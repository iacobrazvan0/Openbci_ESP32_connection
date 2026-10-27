#include <unity.h>

#include <cstdint>
#include <cstring>

#include "GanglionDecoder.h"

using ganglion::GanglionDecoder;

extern "C" void setUp() {}
extern "C" void tearDown() {}

namespace {
void put24(uint8_t* destination, int32_t value) {
  const uint32_t packed = static_cast<uint32_t>(value) & 0x00FFFFFFUL;
  destination[0] = static_cast<uint8_t>(packed >> 16);
  destination[1] = static_cast<uint8_t>(packed >> 8);
  destination[2] = static_cast<uint8_t>(packed);
}

void makeRaw(uint8_t packet[20], const int32_t values[4]) {
  std::memset(packet, 0, 20);
  for (int channel = 0; channel < 4; ++channel) put24(packet + 1 + channel * 3, values[channel]);
}

void encodeDeltas(const int32_t deltas[8], uint8_t width, uint8_t* bytes) {
  std::memset(bytes, 0, width);
  const uint32_t mask = (1UL << width) - 1UL;
  for (uint8_t valueIndex = 0; valueIndex < 8; ++valueIndex) {
    uint32_t encoded = static_cast<uint32_t>(deltas[valueIndex]) & mask;
    encoded = (encoded & ~1UL) | (deltas[valueIndex] < 0 ? 1UL : 0UL);
    for (uint8_t bit = 0; bit < width; ++bit) {
      const uint16_t streamBit = valueIndex * width + bit;
      const uint8_t valueBit = static_cast<uint8_t>((encoded >> (width - 1 - bit)) & 1U);
      bytes[streamBit / 8] |= valueBit << (7 - (streamBit % 8));
    }
  }
}

void encodePacked(const uint32_t values[8], uint8_t width, uint8_t* bytes) {
  std::memset(bytes, 0, width);
  for (uint8_t valueIndex = 0; valueIndex < 8; ++valueIndex) {
    for (uint8_t bit = 0; bit < width; ++bit) {
      const uint16_t streamBit = valueIndex * width + bit;
      const uint8_t valueBit =
          static_cast<uint8_t>((values[valueIndex] >> (width - 1 - bit)) & 1U);
      bytes[streamBit / 8] |= valueBit << (7 - (streamBit % 8));
    }
  }
}

void test_signed_24_bit() {
  const uint8_t positive[] = {0x7F, 0xFF, 0xFF};
  const uint8_t negative[] = {0xFF, 0xFF, 0xFE};
  TEST_ASSERT_EQUAL_INT32(8388607, GanglionDecoder::signed24(positive));
  TEST_ASSERT_EQUAL_INT32(-2, GanglionDecoder::signed24(negative));
}

void test_raw_seeds_decoder_and_scales_to_microvolts() {
  GanglionDecoder decoder;
  uint8_t packet[20];
  const int32_t values[] = {1000, -1000, 8388607, -8388608};
  makeRaw(packet, values);
  const auto result = decoder.decode(packet, sizeof(packet));
  TEST_ASSERT_TRUE(result.valid);
  TEST_ASSERT_EQUAL_UINT8(1, result.sampleCount);
  TEST_ASSERT_EQUAL_INT32(-1000, result.samples[0].counts[1]);
  TEST_ASSERT_DOUBLE_WITHIN(0.000001, 15686.0, result.samples[0].microvolts[2]);
  TEST_ASSERT_DOUBLE_WITHIN(1e-15, 0.001869917138805,
                            ganglion::kMicrovoltsPerCount);
}

void test_official_18_bit_reference_packet() {
  GanglionDecoder decoder;
  uint8_t raw[20];
  const int32_t baseline[] = {300000, 300000, 300000, 300000};
  makeRaw(raw, baseline);
  TEST_ASSERT_TRUE(decoder.decode(raw, sizeof(raw)).valid);

  const uint8_t packet[20] = {
      1, 0, 0, 0, 0, 32, 0, 40, 0, 4,
      128, 0, 188, 0, 7, 0, 40, 192, 10, 14};
  const auto result = decoder.decode(packet, sizeof(packet));
  TEST_ASSERT_TRUE(result.valid);
  const int32_t expectedFirst[] = {0, 2, 10, 4};
  const int32_t expectedSecond[] = {131074, 245760, 114698, 49162};
  for (int channel = 0; channel < 4; ++channel) {
    TEST_ASSERT_EQUAL_INT32(baseline[channel] - expectedFirst[channel],
                            result.samples[0].counts[channel]);
    TEST_ASSERT_EQUAL_INT32(baseline[channel] - expectedFirst[channel] - expectedSecond[channel],
                            result.samples[1].counts[channel]);
  }
}

void test_19_bit_delta_reconstruction_with_negative_values() {
  GanglionDecoder decoder;
  uint8_t raw[20];
  const int32_t baseline[] = {100, 200, 300, 400};
  makeRaw(raw, baseline);
  decoder.decode(raw, sizeof(raw));
  uint8_t packet[20]{};
  packet[0] = 101;
  // The Ganglion overwrites bit 0 with the sign, so decoded negative deltas
  // are odd values (the original least-significant bit is not transmitted).
  const int32_t deltas[] = {2, -3, 6, -7, 10, -11, 14, -15};
  encodeDeltas(deltas, 19, packet + 1);
  const auto result = decoder.decode(packet, sizeof(packet));
  TEST_ASSERT_TRUE(result.valid);
  TEST_ASSERT_EQUAL_INT32(98, result.samples[0].counts[0]);
  TEST_ASSERT_EQUAL_INT32(203, result.samples[0].counts[1]);
  TEST_ASSERT_EQUAL_INT32(88, result.samples[1].counts[0]);
  TEST_ASSERT_EQUAL_INT32(214, result.samples[1].counts[1]);
}

void test_packet_loss_invalidates_delta_chain_until_raw() {
  GanglionDecoder decoder;
  uint8_t raw[20];
  const int32_t baseline[] = {0, 0, 0, 0};
  makeRaw(raw, baseline);
  decoder.decode(raw, sizeof(raw));
  uint8_t packet[20]{};
  packet[0] = 2;  // ID 1 is missing.
  const auto lost = decoder.decode(packet, sizeof(packet));
  TEST_ASSERT_FALSE(lost.valid);
  TEST_ASSERT_TRUE(lost.discontinuity);
  TEST_ASSERT_EQUAL_UINT8(1, lost.missingPackets);
  makeRaw(raw, baseline);
  TEST_ASSERT_TRUE(decoder.decode(raw, sizeof(raw)).valid);
  TEST_ASSERT_TRUE(decoder.hasBaseline());
}

void test_duplicate_packet_is_rejected() {
  GanglionDecoder decoder;
  uint8_t raw[20];
  const int32_t baseline[] = {10, 20, 30, 40};
  makeRaw(raw, baseline);
  decoder.decode(raw, sizeof(raw));
  uint8_t packet[20]{};
  packet[0] = 1;
  TEST_ASSERT_TRUE(decoder.decode(packet, sizeof(packet)).valid);
  const auto duplicate = decoder.decode(packet, sizeof(packet));
  TEST_ASSERT_FALSE(duplicate.valid);
  TEST_ASSERT_TRUE(duplicate.discontinuity);
  TEST_ASSERT_FALSE(decoder.hasBaseline());
}

void test_firmware_3_decodes_absolute_samples_without_raw_baseline() {
  GanglionDecoder decoder;
  decoder.setFirmware(ganglion::Firmware::V3);
  uint8_t packet[20]{};
  packet[0] = 0;
  const uint32_t values[] = {1, 2, 3, 4, 5, 6, 7, 8};
  encodePacked(values, 18, packet + 1);
  const auto result = decoder.decode(packet, sizeof(packet));
  TEST_ASSERT_TRUE(result.valid);
  TEST_ASSERT_EQUAL_UINT8(2, result.sampleCount);
  TEST_ASSERT_EQUAL_INT32(64, result.samples[0].counts[0]);
  TEST_ASSERT_EQUAL_INT32(256, result.samples[0].counts[3]);
  TEST_ASSERT_EQUAL_INT32(320, result.samples[1].counts[0]);
}

void test_firmware_3_negative_conversion_matches_brainflow() {
  GanglionDecoder decoder;
  decoder.setFirmware(ganglion::Firmware::V3);
  uint8_t packet[20]{};
  packet[0] = 100;
  const uint32_t negativeTwo = (1UL << 19) - 1UL;
  const uint32_t values[] = {negativeTwo, 0, 0, 0, 0, 0, 0, 0};
  encodePacked(values, 19, packet + 1);
  const auto result = decoder.decode(packet, sizeof(packet));
  TEST_ASSERT_TRUE(result.valid);
  TEST_ASSERT_EQUAL_INT32(-64, result.samples[0].counts[0]);
}

void test_firmware_3_loss_is_reported_but_absolute_packet_remains_valid() {
  GanglionDecoder decoder;
  decoder.setFirmware(ganglion::Firmware::V3);
  uint8_t packet[20]{};
  packet[0] = 1;
  TEST_ASSERT_TRUE(decoder.decode(packet, sizeof(packet)).valid);
  packet[0] = 3;
  const auto result = decoder.decode(packet, sizeof(packet));
  TEST_ASSERT_TRUE(result.valid);
  TEST_ASSERT_TRUE(result.discontinuity);
  TEST_ASSERT_EQUAL_UINT8(1, result.missingPackets);
  TEST_ASSERT_EQUAL_UINT32(1, decoder.stats().missingPackets);
}
}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_signed_24_bit);
  RUN_TEST(test_raw_seeds_decoder_and_scales_to_microvolts);
  RUN_TEST(test_official_18_bit_reference_packet);
  RUN_TEST(test_19_bit_delta_reconstruction_with_negative_values);
  RUN_TEST(test_packet_loss_invalidates_delta_chain_until_raw);
  RUN_TEST(test_duplicate_packet_is_rejected);
  RUN_TEST(test_firmware_3_decodes_absolute_samples_without_raw_baseline);
  RUN_TEST(test_firmware_3_negative_conversion_matches_brainflow);
  RUN_TEST(test_firmware_3_loss_is_reported_but_absolute_packet_remains_valid);
  return UNITY_END();
}

