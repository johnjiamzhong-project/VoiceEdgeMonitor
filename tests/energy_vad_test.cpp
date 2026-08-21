#include "../board/audio_frame.hpp"
#include "../board/energy_vad.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

voiceedge::AudioFrame frame(std::uint64_t timestamp_ms, std::int16_t amplitude) {
  voiceedge::AudioFrame value;
  value.timestamp_ms = timestamp_ms;
  value.sample_rate = 16000;
  value.channels = 1;
  value.samples.assign(320, amplitude);  // 20 ms at 16 kHz.
  return value;
}

}  // namespace

int main() {
  try {
    const voiceedge::VadConfig config{0.015, 0.012, 60, 60};

    voiceedge::EnergyVAD noise_vad(config);
    for (std::uint64_t timestamp = 0; timestamp < 400; timestamp += 20) {
      const auto decision = noise_vad.process(frame(timestamp, 100));
      require(!decision.event.has_value(), "noise must not trigger VAD");
    }

    voiceedge::EnergyVAD speech_vad(config);
    std::optional<voiceedge::VadEvent> started;
    for (std::uint64_t timestamp = 0; timestamp <= 60; timestamp += 20) {
      const auto decision = speech_vad.process(frame(timestamp, 1200));
      if (decision.event.has_value()) {
        started = decision.event;
      }
    }
    require(started.has_value(), "speech should trigger vad_started");
    require(started->type == voiceedge::VadEventType::Started, "wrong start event type");
    require(started->timestamp_ms == 0, "speech start timestamp should use candidate start");

    const auto hysteresis = speech_vad.process(frame(80, 450));
    require(hysteresis.state == voiceedge::VadState::Speaking,
            "signal between thresholds should remain speaking");

    std::optional<voiceedge::VadEvent> ended;
    for (std::uint64_t timestamp = 100; timestamp <= 180; timestamp += 20) {
      const auto decision = speech_vad.process(frame(timestamp, 100));
      if (decision.event.has_value()) {
        ended = decision.event;
      }
    }
    require(ended.has_value(), "silence should trigger vad_ended");
    require(ended->type == voiceedge::VadEventType::Ended, "wrong end event type");
    require(ended->duration_ms >= 60, "speech duration should be reported");

    voiceedge::EnergyVAD short_vad(config);
    short_vad.process(frame(0, 1200));
    short_vad.process(frame(20, 1200));
    const auto short_end = short_vad.process(frame(40, 100));
    require(!short_end.event.has_value(), "short noise burst must not trigger speech");

    std::cout << "energy_vad_test_passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "energy_vad_test_failed " << error.what() << '\n';
    return 1;
  }
}
