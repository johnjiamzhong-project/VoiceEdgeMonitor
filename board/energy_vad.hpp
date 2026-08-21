#pragma once

#include "audio_frame.hpp"

#include <cmath>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

namespace voiceedge {

enum class VadState {
  Idle,
  Starting,
  Speaking,
  Ending,
};

enum class VadEventType {
  Started,
  Ended,
};

struct VadConfig {
  double start_rms{0.015};
  double end_rms{0.012};
  std::uint64_t min_speech_ms{125};
  std::uint64_t silence_ms{1000};
};

struct VadEvent {
  VadEventType type{VadEventType::Started};
  std::uint64_t timestamp_ms{0};
  std::uint64_t duration_ms{0};
};

struct VadDecision {
  VadState state{VadState::Idle};
  double rms{0.0};
  std::optional<VadEvent> event;
};

inline const char* vad_state_name(VadState state) {
  switch (state) {
    case VadState::Idle:
      return "idle";
    case VadState::Starting:
      return "starting";
    case VadState::Speaking:
      return "speaking";
    case VadState::Ending:
      return "ending";
  }
  return "unknown";
}

inline const char* vad_event_name(VadEventType type) {
  return type == VadEventType::Started ? "vad_started" : "vad_ended";
}

class EnergyVAD {
 public:
  explicit EnergyVAD(VadConfig config) : config_(config) {
    if (!(config_.start_rms > 0.0 && config_.start_rms <= 1.0) ||
        !(config_.end_rms >= 0.0 && config_.end_rms <= config_.start_rms) ||
        config_.min_speech_ms == 0 || config_.silence_ms == 0) {
      throw std::invalid_argument("invalid energy VAD configuration");
    }
  }

  VadDecision process(const AudioFrame& frame) {
    const double rms = calculate_rms(frame);
    const std::uint64_t timestamp_ms = frame.timestamp_ms;
    std::optional<VadEvent> event;

    switch (state_) {
      case VadState::Idle:
        if (rms >= config_.start_rms) {
          state_ = VadState::Starting;
          candidate_start_ms_ = timestamp_ms;
        }
        break;

      case VadState::Starting:
        if (rms < config_.end_rms) {
          state_ = VadState::Idle;
          candidate_start_ms_ = 0;
        } else if (timestamp_ms >= candidate_start_ms_ + config_.min_speech_ms) {
          state_ = VadState::Speaking;
          speech_start_ms_ = candidate_start_ms_;
          event = VadEvent{VadEventType::Started, speech_start_ms_, 0};
        }
        break;

      case VadState::Speaking:
        if (rms < config_.end_rms) {
          state_ = VadState::Ending;
          silence_start_ms_ = timestamp_ms;
        }
        break;

      case VadState::Ending:
        if (rms >= config_.start_rms) {
          state_ = VadState::Speaking;
        } else if (timestamp_ms >= silence_start_ms_ + config_.silence_ms) {
          state_ = VadState::Idle;
          event = VadEvent{VadEventType::Ended, timestamp_ms, timestamp_ms - speech_start_ms_};
          candidate_start_ms_ = 0;
          speech_start_ms_ = 0;
          silence_start_ms_ = 0;
        }
        break;
    }

    return VadDecision{state_, rms, event};
  }

  std::optional<VadEvent> finish(std::uint64_t timestamp_ms) {
    if (state_ != VadState::Speaking && state_ != VadState::Ending) {
      state_ = VadState::Idle;
      return std::nullopt;
    }
    const VadEvent event{VadEventType::Ended, timestamp_ms, timestamp_ms - speech_start_ms_};
    state_ = VadState::Idle;
    candidate_start_ms_ = 0;
    speech_start_ms_ = 0;
    silence_start_ms_ = 0;
    return event;
  }

  [[nodiscard]] VadState state() const { return state_; }
  [[nodiscard]] const VadConfig& config() const { return config_; }

  void reset() {
    state_ = VadState::Idle;
    candidate_start_ms_ = 0;
    speech_start_ms_ = 0;
    silence_start_ms_ = 0;
  }

 private:
  static double calculate_rms(const AudioFrame& frame) {
    if (frame.samples.empty()) {
      return 0.0;
    }
    long double sum_squared = 0.0L;
    for (const std::int16_t sample : frame.samples) {
      const long double normalized = static_cast<long double>(sample) / 32768.0L;
      sum_squared += normalized * normalized;
    }
    return std::sqrt(static_cast<double>(sum_squared / frame.samples.size()));
  }

  const VadConfig config_;
  VadState state_{VadState::Idle};
  std::uint64_t candidate_start_ms_{0};
  std::uint64_t speech_start_ms_{0};
  std::uint64_t silence_start_ms_{0};
};

}  // namespace voiceedge
