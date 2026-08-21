#include "../board/recording_store.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

}  // namespace

int main() {
  const auto root = std::filesystem::temp_directory_path() / "voiceedge-recording-store-test";
  std::filesystem::remove_all(root);
  try {
    voiceedge::RecordingStore store({root, true, true, 2});
    store.start();
    auto segment = std::make_shared<voiceedge::SpeechSegment>();
    segment->id = 7;
    segment->start_timestamp_ms = 100;
    segment->end_timestamp_ms = 300;
    segment->sample_rate = 16000;
    segment->channels = 1;
    segment->samples.assign(3200, 123);
    require(store.enqueue({segment, "测试文本", "final", 42, 0.21, ""}), "record enqueue failed");
    store.stop();

    require(std::filesystem::exists(root / "transcripts.jsonl"), "transcript file missing");
    std::ifstream transcript(root / "transcripts.jsonl");
    const std::string line((std::istreambuf_iterator<char>(transcript)), std::istreambuf_iterator<char>());
    require(line.find("测试文本") != std::string::npos, "transcript text missing");
    const auto recordings = root / "recordings";
    bool wav_found = false;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(recordings)) {
      if (entry.path().extension() == ".wav") wav_found = true;
    }
    require(wav_found, "WAV recording missing");
    std::filesystem::remove_all(root);
    std::cout << "recording_store_test_passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::filesystem::remove_all(root);
    std::cerr << "recording_store_test_failed " << error.what() << '\n';
    return 1;
  }
}
