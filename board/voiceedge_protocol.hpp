#pragma once

#include "audio_frame.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace voiceedge::protocol {

constexpr std::size_t kAudioHeaderSize = 32;
constexpr std::uint8_t kProtocolVersion = 1;
constexpr std::uint8_t kCodecPcmS16Le = 0;

struct AudioPacketInfo {
  std::uint8_t version{0};
  std::uint8_t codec{0};
  std::uint8_t channels{0};
  std::uint32_t sample_rate{0};
  std::uint64_t sequence{0};
  std::uint64_t timestamp_ms{0};
  std::uint32_t sample_count{0};
};

inline void append_u32_le(std::vector<std::uint8_t>& output, std::uint32_t value) {
  output.push_back(static_cast<std::uint8_t>(value & 0xffU));
  output.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
  output.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xffU));
  output.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xffU));
}

inline void append_u64_le(std::vector<std::uint8_t>& output, std::uint64_t value) {
  for (unsigned int shift = 0; shift < 64; shift += 8) {
    output.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
  }
}

inline std::uint32_t read_u32_le(const std::vector<std::uint8_t>& input, std::size_t offset) {
  return static_cast<std::uint32_t>(input[offset]) |
         (static_cast<std::uint32_t>(input[offset + 1]) << 8U) |
         (static_cast<std::uint32_t>(input[offset + 2]) << 16U) |
         (static_cast<std::uint32_t>(input[offset + 3]) << 24U);
}

inline std::uint64_t read_u64_le(const std::vector<std::uint8_t>& input, std::size_t offset) {
  std::uint64_t value = 0;
  for (unsigned int shift = 0; shift < 64; shift += 8) {
    value |= static_cast<std::uint64_t>(input[offset + shift / 8]) << shift;
  }
  return value;
}

inline std::vector<std::uint8_t> encode_audio_packet(const AudioFrame& frame) {
  if (frame.channels == 0 || frame.channels > 255 || frame.sample_rate == 0 ||
      frame.samples.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw std::invalid_argument("audio frame cannot be represented by protocol");
  }

  std::vector<std::uint8_t> packet;
  packet.reserve(kAudioHeaderSize + frame.samples.size() * sizeof(std::int16_t));
  packet.insert(packet.end(), {'V', 'E', 'A', '1'});
  packet.push_back(kProtocolVersion);
  packet.push_back(kCodecPcmS16Le);
  packet.push_back(static_cast<std::uint8_t>(frame.channels));
  packet.push_back(0);
  append_u32_le(packet, frame.sample_rate);
  append_u64_le(packet, frame.sequence);
  append_u64_le(packet, frame.timestamp_ms);
  append_u32_le(packet, static_cast<std::uint32_t>(frame.samples.size()));

  const auto* bytes = reinterpret_cast<const std::uint8_t*>(frame.samples.data());
  packet.insert(packet.end(), bytes, bytes + frame.samples.size() * sizeof(std::int16_t));
  return packet;
}

inline bool decode_audio_packet(const std::vector<std::uint8_t>& packet, AudioPacketInfo& info) {
  if (packet.size() < kAudioHeaderSize || packet[0] != 'V' || packet[1] != 'E' ||
      packet[2] != 'A' || packet[3] != '1') {
    return false;
  }
  info.version = packet[4];
  info.codec = packet[5];
  info.channels = packet[6];
  info.sample_rate = read_u32_le(packet, 8);
  info.sequence = read_u64_le(packet, 12);
  info.timestamp_ms = read_u64_le(packet, 20);
  info.sample_count = read_u32_le(packet, 28);
  const std::size_t payload_bytes = static_cast<std::size_t>(info.sample_count) * sizeof(std::int16_t);
  return info.version == kProtocolVersion && info.codec == kCodecPcmS16Le && info.channels != 0 &&
         packet.size() == kAudioHeaderSize + payload_bytes;
}

inline std::string json_escape(const std::string& value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (const char character : value) {
    if (character == '\\' || character == '"') {
      escaped.push_back('\\');
    }
    escaped.push_back(character);
  }
  return escaped;
}

}  // namespace voiceedge::protocol
