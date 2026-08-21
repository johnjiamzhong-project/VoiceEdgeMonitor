#include "../board/audio_frame.hpp"
#include "../board/voiceedge_protocol.hpp"

#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

}  // namespace

int main() {
  try {
    voiceedge::AudioFrame frame;
    frame.sequence = 42;
    frame.timestamp_ms = 123456;
    frame.sample_rate = 16000;
    frame.channels = 1;
    frame.samples = {1, -2, 300, -400};

    const auto packet = voiceedge::protocol::encode_audio_packet(frame);
    require(packet.size() == voiceedge::protocol::kAudioHeaderSize + frame.samples.size() * 2,
            "encoded packet size is incorrect");

    voiceedge::protocol::AudioPacketInfo info;
    require(voiceedge::protocol::decode_audio_packet(packet, info), "packet should decode");
    require(info.version == 1, "protocol version mismatch");
    require(info.codec == voiceedge::protocol::kCodecPcmS16Le, "codec mismatch");
    require(info.channels == 1, "channel count mismatch");
    require(info.sample_rate == 16000, "sample rate mismatch");
    require(info.sequence == 42, "sequence mismatch");
    require(info.timestamp_ms == 123456, "timestamp mismatch");
    require(info.sample_count == frame.samples.size(), "sample count mismatch");

    auto invalid_packet = packet;
    invalid_packet[0] = 'X';
    require(!voiceedge::protocol::decode_audio_packet(invalid_packet, info),
            "invalid magic must be rejected");

    std::cout << "protocol_test_passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "protocol_test_failed " << error.what() << '\n';
    return 1;
  }
}
