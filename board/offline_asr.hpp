#pragma once

#include <sherpa-onnx/c-api/c-api.h>

#include "audio_frame.hpp"

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace voiceedge {

struct AsrResult {
  std::string text;
  std::uint64_t processing_ms{0};
};

class OfflineASR {
 public:
  OfflineASR() = default;

  ~OfflineASR() {
    if (recognizer_ != nullptr) {
      SherpaOnnxDestroyOfflineRecognizer(recognizer_);
    }
  }

  OfflineASR(const OfflineASR&) = delete;
  OfflineASR& operator=(const OfflineASR&) = delete;

  void open(const std::string& model, const std::string& tokens,
            const std::string& language, const std::string& provider,
            int num_threads) {
    if (model.empty() || tokens.empty()) {
      throw std::invalid_argument("ASR model and tokens are required");
    }
    SherpaOnnxOfflineRecognizerConfig config{};
    config.feat_config.sample_rate = 16000;
    config.feat_config.feature_dim = 80;
    config.model_config.sense_voice.model = model.c_str();
    config.model_config.sense_voice.language = language.c_str();
    config.model_config.sense_voice.use_itn = 1;
    config.model_config.tokens = tokens.c_str();
    config.model_config.provider = provider.c_str();
    config.model_config.num_threads = num_threads;
    config.decoding_method = "greedy_search";
    config.max_active_paths = 4;
    recognizer_ = SherpaOnnxCreateOfflineRecognizer(&config);
    if (recognizer_ == nullptr) {
      throw std::runtime_error("SherpaOnnxCreateOfflineRecognizer failed");
    }
  }

  [[nodiscard]] bool enabled() const { return recognizer_ != nullptr; }

  AsrResult transcribe(const SpeechSegment& segment) const {
    if (!enabled() || segment.samples.empty()) {
      return {};
    }
    if (segment.sample_rate != 16000 || segment.channels == 0) {
      throw std::invalid_argument("ASR currently requires 16 kHz audio with valid channels");
    }

    std::vector<float> samples;
    samples.reserve(segment.samples.size() / segment.channels);
    for (std::size_t frame = 0; frame + segment.channels <= segment.samples.size();
         frame += segment.channels) {
      long sum = 0;
      for (unsigned int channel = 0; channel < segment.channels; ++channel) {
        sum += segment.samples[frame + channel];
      }
      samples.push_back(static_cast<float>(sum) /
                        (32768.0F * static_cast<float>(segment.channels)));
    }

    const auto started = std::chrono::steady_clock::now();
    const SherpaOnnxOfflineStream* stream = SherpaOnnxCreateOfflineStream(recognizer_);
    if (stream == nullptr) {
      throw std::runtime_error("SherpaOnnxCreateOfflineStream failed");
    }
    SherpaOnnxAcceptWaveformOffline(stream, 16000, samples.data(),
                                    static_cast<int32_t>(samples.size()));
    SherpaOnnxDecodeOfflineStream(recognizer_, stream);
    const SherpaOnnxOfflineRecognizerResult* result =
        SherpaOnnxGetOfflineStreamResult(stream);
    if (result == nullptr) {
      SherpaOnnxDestroyOfflineStream(stream);
      throw std::runtime_error("SherpaOnnxGetOfflineStreamResult failed");
    }
    AsrResult output;
    output.text = result->text != nullptr ? result->text : "";
    output.processing_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count());
    SherpaOnnxDestroyOfflineRecognizerResult(result);
    SherpaOnnxDestroyOfflineStream(stream);
    return output;
  }

 private:
  const SherpaOnnxOfflineRecognizer* recognizer_{nullptr};
};

}  // namespace voiceedge
