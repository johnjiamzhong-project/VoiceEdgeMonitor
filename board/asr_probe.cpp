#include <sherpa-onnx/c-api/c-api.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
  std::string model;
  std::string tokens;
  std::string wave;
  std::string language{"zh"};
  std::string provider{"cpu"};
  int num_threads{2};
};

std::uint16_t read_u16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  return static_cast<std::uint16_t>(bytes[offset]) |
         (static_cast<std::uint16_t>(bytes[offset + 1]) << 8U);
}

std::uint32_t read_u32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  return static_cast<std::uint32_t>(bytes[offset]) |
         (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
         (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
         (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
}

std::string option_value(int& index, int argc, char** argv, std::string_view name) {
  const std::string argument(argv[index]);
  const std::string prefix = std::string(name) + "=";
  if (argument.rfind(prefix, 0) == 0) {
    return argument.substr(prefix.size());
  }
  if (argument == name && index + 1 < argc) {
    ++index;
    return argv[index];
  }
  throw std::invalid_argument(std::string(name) + " expects a value");
}

int parse_int(const std::string& value, std::string_view option) {
  try {
    std::size_t consumed = 0;
    const int parsed = std::stoi(value, &consumed, 10);
    if (consumed != value.size() || parsed <= 0) {
      throw std::invalid_argument("invalid integer");
    }
    return parsed;
  } catch (const std::exception&) {
    throw std::invalid_argument(std::string(option) + " expects a positive integer: " + value);
  }
}

Options parse_options(int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string argument(argv[index]);
    if (argument == "--help" || argument == "-h") {
      std::cout
          << "Usage: " << argv[0] << " [options]\n"
          << "  --model <path>       SenseVoice model.int8.onnx\n"
          << "  --tokens <path>      tokens.txt\n"
          << "  --wave <path>        16-bit PCM WAV\n"
          << "  --language <name>    language (default: zh)\n"
          << "  --provider <name>    runtime provider (default: cpu)\n"
          << "  --threads <n>        inference threads (default: 2)\n";
      std::exit(0);
    }
    if (argument.rfind("--model", 0) == 0) {
      options.model = option_value(index, argc, argv, "--model");
    } else if (argument.rfind("--tokens", 0) == 0) {
      options.tokens = option_value(index, argc, argv, "--tokens");
    } else if (argument.rfind("--wave", 0) == 0) {
      options.wave = option_value(index, argc, argv, "--wave");
    } else if (argument.rfind("--language", 0) == 0) {
      options.language = option_value(index, argc, argv, "--language");
    } else if (argument.rfind("--provider", 0) == 0) {
      options.provider = option_value(index, argc, argv, "--provider");
    } else if (argument.rfind("--threads", 0) == 0) {
      options.num_threads = parse_int(option_value(index, argc, argv, "--threads"), "--threads");
    } else {
      throw std::invalid_argument("unknown option: " + argument);
    }
  }
  if (options.model.empty() || options.tokens.empty() || options.wave.empty()) {
    throw std::invalid_argument("--model, --tokens and --wave are required");
  }
  return options;
}

struct WaveData {
  int sample_rate{0};
  std::vector<float> samples;
};

WaveData read_wave(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot open WAV: " + path);
  }
  const std::vector<std::uint8_t> bytes(
      (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  if (bytes.size() < 12 || std::string(bytes.begin(), bytes.begin() + 4) != "RIFF" ||
      std::string(bytes.begin() + 8, bytes.begin() + 12) != "WAVE") {
    throw std::runtime_error("unsupported WAV header: " + path);
  }

  std::uint16_t format = 0;
  std::uint16_t channels = 0;
  std::uint32_t sample_rate = 0;
  std::uint16_t bits = 0;
  std::size_t data_offset = 0;
  std::size_t data_size = 0;
  std::size_t offset = 12;
  while (offset + 8 <= bytes.size()) {
    const std::string chunk(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                            bytes.begin() + static_cast<std::ptrdiff_t>(offset + 4));
    const std::uint32_t chunk_size = read_u32(bytes, offset + 4);
    const std::size_t payload = offset + 8;
    if (payload + chunk_size > bytes.size()) {
      throw std::runtime_error("truncated WAV chunk: " + path);
    }
    if (chunk == "fmt ") {
      if (chunk_size < 16) {
        throw std::runtime_error("invalid WAV fmt chunk: " + path);
      }
      format = read_u16(bytes, payload);
      channels = read_u16(bytes, payload + 2);
      sample_rate = read_u32(bytes, payload + 4);
      bits = read_u16(bytes, payload + 14);
    } else if (chunk == "data") {
      data_offset = payload;
      data_size = chunk_size;
    }
    offset = payload + chunk_size + (chunk_size & 1U);
  }

  if (format != 1 || channels == 0 || sample_rate == 0 || bits != 16 || data_size == 0) {
    throw std::runtime_error("WAV must be PCM 16-bit with a data chunk: " + path);
  }
  const std::size_t total_samples = data_size / sizeof(std::int16_t);
  const std::size_t frames = total_samples / channels;
  WaveData result;
  result.sample_rate = static_cast<int>(sample_rate);
  result.samples.resize(frames);
  for (std::size_t frame = 0; frame < frames; ++frame) {
    long sum = 0;
    for (std::size_t channel = 0; channel < channels; ++channel) {
      const std::size_t sample_offset = data_offset + (frame * channels + channel) * 2;
      const std::uint16_t raw = read_u16(bytes, sample_offset);
      sum += static_cast<std::int16_t>(raw);
    }
    result.samples[frame] = static_cast<float>(sum) /
                           (32768.0F * static_cast<float>(channels));
  }
  return result;
}

class OfflineRecognizer {
 public:
  ~OfflineRecognizer() {
    if (recognizer_ != nullptr) {
      SherpaOnnxDestroyOfflineRecognizer(recognizer_);
    }
  }

  void open(const Options& options) {
    SherpaOnnxOfflineRecognizerConfig config{};
    config.feat_config.sample_rate = 16000;
    config.feat_config.feature_dim = 80;
    config.model_config.sense_voice.model = options.model.c_str();
    config.model_config.sense_voice.language = options.language.c_str();
    config.model_config.sense_voice.use_itn = 1;
    config.model_config.tokens = options.tokens.c_str();
    config.model_config.provider = options.provider.c_str();
    config.model_config.num_threads = options.num_threads;
    config.decoding_method = "greedy_search";
    config.max_active_paths = 4;
    recognizer_ = SherpaOnnxCreateOfflineRecognizer(&config);
    if (recognizer_ == nullptr) {
      throw std::runtime_error("SherpaOnnxCreateOfflineRecognizer failed");
    }
  }

  std::string transcribe(const WaveData& wave, std::uint64_t& processing_ms) const {
    const auto started = Clock::now();
    const SherpaOnnxOfflineStream* stream = SherpaOnnxCreateOfflineStream(recognizer_);
    if (stream == nullptr) {
      throw std::runtime_error("SherpaOnnxCreateOfflineStream failed");
    }
    SherpaOnnxAcceptWaveformOffline(stream, wave.sample_rate, wave.samples.data(),
                                    static_cast<int32_t>(wave.samples.size()));
    SherpaOnnxDecodeOfflineStream(recognizer_, stream);
    const SherpaOnnxOfflineRecognizerResult* result =
        SherpaOnnxGetOfflineStreamResult(stream);
    if (result == nullptr) {
      SherpaOnnxDestroyOfflineStream(stream);
      throw std::runtime_error("SherpaOnnxGetOfflineStreamResult failed");
    }
    const std::string text = result->text != nullptr ? result->text : "";
    SherpaOnnxDestroyOfflineRecognizerResult(result);
    SherpaOnnxDestroyOfflineStream(stream);
    processing_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count());
    return text;
  }

 private:
  const SherpaOnnxOfflineRecognizer* recognizer_{nullptr};
};

}  // namespace

int main(int argc, char** argv) {
  try {
    const Options options = parse_options(argc, argv);
    const WaveData wave = read_wave(options.wave);
    if (wave.sample_rate != 16000) {
      throw std::runtime_error("ASR probe currently requires a 16 kHz WAV");
    }
    OfflineRecognizer recognizer;
    const auto load_started = Clock::now();
    recognizer.open(options);
    const auto load_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - load_started).count());
    std::uint64_t processing_ms = 0;
    const std::string text = recognizer.transcribe(wave, processing_ms);
    const double audio_ms = static_cast<double>(wave.samples.size()) * 1000.0 / wave.sample_rate;
    const double rtf = audio_ms > 0.0 ? static_cast<double>(processing_ms) / audio_ms : 0.0;
    std::cout << "asr_result text=\"" << text << "\""
              << " sample_rate=" << wave.sample_rate
              << " samples=" << wave.samples.size()
              << " audio_ms=" << std::fixed << std::setprecision(1) << audio_ms
              << " load_ms=" << load_ms
              << " processing_ms=" << processing_ms
              << " rtf=" << std::setprecision(4) << rtf << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "asr_probe_error " << error.what() << '\n';
    return 1;
  }
}
