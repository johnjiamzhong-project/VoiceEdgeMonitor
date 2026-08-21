#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace voiceedge {

struct AudioFrame {
  std::uint64_t sequence{0};
  std::uint64_t timestamp_ms{0};
  unsigned int sample_rate{0};
  unsigned int channels{0};
  std::vector<std::int16_t> samples;
};

using AudioFramePtr = std::shared_ptr<AudioFrame>;

struct SpeechSegment {
  std::uint64_t id{0};
  std::uint64_t start_timestamp_ms{0};
  std::uint64_t end_timestamp_ms{0};
  unsigned int sample_rate{0};
  unsigned int channels{0};
  std::vector<std::int16_t> samples;
};

using SpeechSegmentPtr = std::shared_ptr<SpeechSegment>;

}  // namespace voiceedge
