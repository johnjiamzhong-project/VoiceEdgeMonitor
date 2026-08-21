#include <alsa/asoundlib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <csignal>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

constexpr unsigned int kDefaultRate = 16000;
constexpr unsigned int kDefaultChannels = 1;
constexpr snd_pcm_uframes_t kDefaultPeriodFrames = 2000;
constexpr snd_pcm_uframes_t kDefaultBufferFrames = 8000;
constexpr std::uint64_t kDefaultReportMs = 1000;
constexpr const char* kDefaultDevice = "hw:CARD=U0x46d0x81b,DEV=0";

std::atomic<bool> g_stop{false};

void handle_signal(int) noexcept {
  g_stop.store(true, std::memory_order_relaxed);
}

struct PcmDeleter {
  void operator()(snd_pcm_t* pcm) const noexcept {
    if (pcm != nullptr) {
      snd_pcm_close(pcm);
    }
  }
};

struct HwParamsDeleter {
  void operator()(snd_pcm_hw_params_t* params) const noexcept {
    if (params != nullptr) {
      snd_pcm_hw_params_free(params);
    }
  }
};

using PcmPtr = std::unique_ptr<snd_pcm_t, PcmDeleter>;
using HwParamsPtr = std::unique_ptr<snd_pcm_hw_params_t, HwParamsDeleter>;

struct Options {
  std::string device;
  unsigned int rate{kDefaultRate};
  unsigned int channels{kDefaultChannels};
  snd_pcm_uframes_t period_frames{kDefaultPeriodFrames};
  snd_pcm_uframes_t buffer_frames{kDefaultBufferFrames};
  std::uint64_t run_ms{0};
  std::uint64_t report_ms{kDefaultReportMs};
  std::string wav_path;
};

std::string default_device() {
  const char* environment_device = std::getenv("VOICEEDGE_CAPTURE_DEVICE");
  if (environment_device != nullptr && *environment_device != '\0') {
    return environment_device;
  }
  return kDefaultDevice;
}

std::uint64_t parse_uint64(const std::string& value, std::string_view option) {
  try {
    std::size_t consumed = 0;
    const unsigned long long parsed = std::stoull(value, &consumed, 10);
    if (consumed != value.size()) {
      throw std::invalid_argument("trailing characters");
    }
    return static_cast<std::uint64_t>(parsed);
  } catch (const std::exception&) {
    throw std::invalid_argument(std::string(option) + " expects a non-negative integer: " + value);
  }
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

void print_usage(const char* program) {
  std::cout
      << "Usage: " << program << " [options]\n"
      << "  --device <name>       ALSA capture device (env: VOICEEDGE_CAPTURE_DEVICE)\n"
      << "                        default: " << kDefaultDevice << "\n"
      << "  --rate <hz>           capture rate (default: 16000)\n"
      << "  --channels <count>    capture channels (default: 1)\n"
      << "  --period-frames <n>   ALSA period size (default: 2000)\n"
      << "  --buffer-frames <n>   ALSA buffer size (default: 8000)\n"
      << "  --run-ms <ms>         stop after this duration; 0 means Ctrl+C (default: 0)\n"
      << "  --report-ms <ms>      statistics interval (default: 1000)\n"
      << "  --wav <path>          optional mono/stereo PCM WAV output\n"
      << "  --help                show this help\n";
}

Options parse_options(int argc, char** argv) {
  Options options;
  options.device = default_device();

  for (int index = 1; index < argc; ++index) {
    const std::string argument(argv[index]);
    if (argument == "--help" || argument == "-h") {
      print_usage(argv[0]);
      std::exit(0);
    }
    if (argument.rfind("--device", 0) == 0) {
      options.device = option_value(index, argc, argv, "--device");
    } else if (argument.rfind("--rate", 0) == 0) {
      const std::uint64_t value = parse_uint64(option_value(index, argc, argv, "--rate"), "--rate");
      if (value == 0 || value > std::numeric_limits<unsigned int>::max()) {
        throw std::invalid_argument("--rate is outside the supported range");
      }
      options.rate = static_cast<unsigned int>(value);
    } else if (argument.rfind("--channels", 0) == 0) {
      const std::uint64_t value = parse_uint64(option_value(index, argc, argv, "--channels"), "--channels");
      if (value == 0 || value > 8) {
        throw std::invalid_argument("--channels must be between 1 and 8");
      }
      options.channels = static_cast<unsigned int>(value);
    } else if (argument.rfind("--period-frames", 0) == 0) {
      const std::uint64_t value = parse_uint64(
          option_value(index, argc, argv, "--period-frames"), "--period-frames");
      if (value == 0 || value > std::numeric_limits<snd_pcm_uframes_t>::max()) {
        throw std::invalid_argument("--period-frames is outside the supported range");
      }
      options.period_frames = static_cast<snd_pcm_uframes_t>(value);
    } else if (argument.rfind("--buffer-frames", 0) == 0) {
      const std::uint64_t value = parse_uint64(
          option_value(index, argc, argv, "--buffer-frames"), "--buffer-frames");
      if (value == 0 || value > std::numeric_limits<snd_pcm_uframes_t>::max()) {
        throw std::invalid_argument("--buffer-frames is outside the supported range");
      }
      options.buffer_frames = static_cast<snd_pcm_uframes_t>(value);
    } else if (argument.rfind("--run-ms", 0) == 0) {
      options.run_ms = parse_uint64(option_value(index, argc, argv, "--run-ms"), "--run-ms");
    } else if (argument.rfind("--report-ms", 0) == 0) {
      options.report_ms = parse_uint64(option_value(index, argc, argv, "--report-ms"), "--report-ms");
      if (options.report_ms == 0) {
        throw std::invalid_argument("--report-ms must be greater than zero");
      }
    } else if (argument.rfind("--wav", 0) == 0) {
      options.wav_path = option_value(index, argc, argv, "--wav");
    } else {
      throw std::invalid_argument("unknown option: " + argument);
    }
  }
  return options;
}

void check_alsa(int result, std::string_view operation) {
  if (result < 0) {
    throw std::runtime_error(std::string(operation) + " failed: " + snd_strerror(result));
  }
}

double dbfs(double amplitude) {
  if (amplitude <= 0.0) {
    return -120.0;
  }
  return 20.0 * std::log10(amplitude / 32768.0);
}

class WavWriter {
 public:
  WavWriter() = default;
  WavWriter(const WavWriter&) = delete;
  WavWriter& operator=(const WavWriter&) = delete;

  void open(const std::string& path, unsigned int rate, unsigned int channels) {
    if (channels == 0 || channels > std::numeric_limits<std::uint16_t>::max()) {
      throw std::invalid_argument("invalid WAV channel count");
    }
    const std::uint64_t bytes_per_frame = static_cast<std::uint64_t>(channels) * sizeof(std::int16_t);
    const std::uint64_t byte_rate = static_cast<std::uint64_t>(rate) * bytes_per_frame;
    if (byte_rate > std::numeric_limits<std::uint32_t>::max()) {
      throw std::invalid_argument("WAV byte rate is too large");
    }

    output_.open(path, std::ios::binary | std::ios::trunc);
    if (!output_) {
      throw std::runtime_error("cannot open WAV output: " + path);
    }

    output_.write("RIFF", 4);
    write_u32(0);
    output_.write("WAVE", 4);
    output_.write("fmt ", 4);
    write_u32(16);
    write_u16(1);
    write_u16(static_cast<std::uint16_t>(channels));
    write_u32(rate);
    write_u32(static_cast<std::uint32_t>(byte_rate));
    write_u16(static_cast<std::uint16_t>(bytes_per_frame));
    write_u16(16);
    output_.write("data", 4);
    write_u32(0);
    if (!output_) {
      throw std::runtime_error("cannot write WAV header: " + path);
    }
  }

  void write(const std::int16_t* samples, std::size_t sample_count) {
    if (!output_.is_open()) {
      return;
    }
    const std::uint64_t bytes = static_cast<std::uint64_t>(sample_count) * sizeof(std::int16_t);
    if (data_bytes_ + bytes > std::numeric_limits<std::uint32_t>::max()) {
      throw std::runtime_error("WAV output exceeds the 4 GiB RIFF limit");
    }
    output_.write(reinterpret_cast<const char*>(samples), static_cast<std::streamsize>(bytes));
    if (!output_) {
      throw std::runtime_error("failed while writing WAV output");
    }
    data_bytes_ += bytes;
  }

  void close() {
    if (!output_.is_open()) {
      return;
    }
    const std::uint32_t riff_size = static_cast<std::uint32_t>(36 + data_bytes_);
    const std::uint32_t data_size = static_cast<std::uint32_t>(data_bytes_);
    output_.seekp(4, std::ios::beg);
    write_u32(riff_size);
    output_.seekp(40, std::ios::beg);
    write_u32(data_size);
    output_.close();
  }

  ~WavWriter() {
    try {
      close();
    } catch (...) {
    }
  }

 private:
  void write_u16(std::uint16_t value) {
    const char bytes[2] = {
        static_cast<char>(value & 0xffU),
        static_cast<char>((value >> 8U) & 0xffU),
    };
    output_.write(bytes, sizeof(bytes));
  }

  void write_u32(std::uint32_t value) {
    const char bytes[4] = {
        static_cast<char>(value & 0xffU),
        static_cast<char>((value >> 8U) & 0xffU),
        static_cast<char>((value >> 16U) & 0xffU),
        static_cast<char>((value >> 24U) & 0xffU),
    };
    output_.write(bytes, sizeof(bytes));
  }

  std::ofstream output_;
  std::uint64_t data_bytes_{0};
};

struct IntervalStats {
  std::uint64_t samples{0};
  long double sum_squared{0.0L};
  int peak{0};

  void add(const std::int16_t* data, std::size_t sample_count) {
    for (std::size_t index = 0; index < sample_count; ++index) {
      const int value = static_cast<int>(data[index]);
      const int magnitude = value < 0 ? -value : value;
      peak = std::max(peak, magnitude);
      sum_squared += static_cast<long double>(value) * static_cast<long double>(value);
    }
    samples += sample_count;
  }

  double rms() const {
    if (samples == 0) {
      return 0.0;
    }
    return std::sqrt(static_cast<double>(sum_squared / static_cast<long double>(samples)));
  }
};

struct CaptureStats {
  std::uint64_t frames{0};
  std::uint64_t xruns{0};
  std::uint64_t recoveries{0};
  std::uint64_t read_errors{0};
};

void print_interval(const IntervalStats& interval, const CaptureStats& stats, std::uint64_t elapsed_ms) {
  const double rms = interval.rms();
  std::cout << "capture_stats elapsed_ms=" << elapsed_ms
            << " samples=" << interval.samples
            << " rms=" << std::fixed << std::setprecision(1) << rms
            << " rms_dbfs=" << dbfs(rms)
            << " peak=" << interval.peak
            << " peak_dbfs=" << dbfs(static_cast<double>(interval.peak))
            << " frames=" << stats.frames
            << " xruns=" << stats.xruns
            << " recoveries=" << stats.recoveries
            << " read_errors=" << stats.read_errors << '\n';
}

int run_capture(const Options& options) {
  PcmPtr pcm;
  snd_pcm_t* raw_pcm = nullptr;
  check_alsa(snd_pcm_open(&raw_pcm, options.device.c_str(), SND_PCM_STREAM_CAPTURE, 0), "snd_pcm_open");
  pcm.reset(raw_pcm);

  snd_pcm_hw_params_t* raw_params = nullptr;
  check_alsa(snd_pcm_hw_params_malloc(&raw_params), "snd_pcm_hw_params_malloc");
  HwParamsPtr params(raw_params);
  check_alsa(snd_pcm_hw_params_any(pcm.get(), params.get()), "snd_pcm_hw_params_any");
  check_alsa(snd_pcm_hw_params_set_access(pcm.get(), params.get(), SND_PCM_ACCESS_RW_INTERLEAVED),
             "snd_pcm_hw_params_set_access");
  check_alsa(snd_pcm_hw_params_set_format(pcm.get(), params.get(), SND_PCM_FORMAT_S16_LE),
             "snd_pcm_hw_params_set_format");
  check_alsa(snd_pcm_hw_params_set_rate(pcm.get(), params.get(), options.rate, 0),
             "snd_pcm_hw_params_set_rate");
  check_alsa(snd_pcm_hw_params_set_channels(pcm.get(), params.get(), options.channels),
             "snd_pcm_hw_params_set_channels");
  snd_pcm_uframes_t requested_period = options.period_frames;
  int period_direction = 0;
  check_alsa(snd_pcm_hw_params_set_period_size_near(
                 pcm.get(), params.get(), &requested_period, &period_direction),
             "snd_pcm_hw_params_set_period_size_near");
  snd_pcm_uframes_t requested_buffer = options.buffer_frames;
  check_alsa(snd_pcm_hw_params_set_buffer_size_near(pcm.get(), params.get(), &requested_buffer),
             "snd_pcm_hw_params_set_buffer_size_near");
  check_alsa(snd_pcm_hw_params(pcm.get(), params.get()), "snd_pcm_hw_params");

  unsigned int actual_rate = 0;
  unsigned int actual_channels = 0;
  snd_pcm_uframes_t period_frames = 0;
  snd_pcm_uframes_t buffer_frames = 0;
  int direction = 0;
  check_alsa(snd_pcm_hw_params_get_rate(params.get(), &actual_rate, &direction),
             "snd_pcm_hw_params_get_rate");
  check_alsa(snd_pcm_hw_params_get_channels(params.get(), &actual_channels),
             "snd_pcm_hw_params_get_channels");
  check_alsa(snd_pcm_hw_params_get_period_size(params.get(), &period_frames, &direction),
             "snd_pcm_hw_params_get_period_size");
  check_alsa(snd_pcm_hw_params_get_buffer_size(params.get(), &buffer_frames),
             "snd_pcm_hw_params_get_buffer_size");

  if (actual_rate != options.rate || actual_channels != options.channels) {
    throw std::runtime_error("ALSA accepted parameters different from requested values: actual_rate=" +
                             std::to_string(actual_rate) + " actual_channels=" +
                             std::to_string(actual_channels));
  }
  if (period_frames == 0 || period_frames > std::numeric_limits<std::size_t>::max() / options.channels) {
    throw std::runtime_error("ALSA returned an invalid period size");
  }

  check_alsa(snd_pcm_prepare(pcm.get()), "snd_pcm_prepare");

  WavWriter wav;
  if (!options.wav_path.empty()) {
    wav.open(options.wav_path, actual_rate, actual_channels);
  }

  const std::size_t sample_capacity = static_cast<std::size_t>(period_frames) * actual_channels;
  std::vector<std::int16_t> samples(sample_capacity);
  CaptureStats stats;
  IntervalStats interval;
  const auto started = Clock::now();
  auto next_report = started + std::chrono::milliseconds(options.report_ms);

  std::cout << "capture_started device=" << options.device
            << " rate=" << actual_rate
            << " channels=" << actual_channels
            << " format=S16_LE"
            << " period_frames=" << period_frames
            << " buffer_frames=" << buffer_frames
            << " report_ms=" << options.report_ms << '\n';

  while (!g_stop.load(std::memory_order_relaxed)) {
    const auto now = Clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - started).count();
    if (options.run_ms != 0 && static_cast<std::uint64_t>(elapsed) >= options.run_ms) {
      break;
    }

    const snd_pcm_sframes_t read_frames = snd_pcm_readi(pcm.get(), samples.data(), period_frames);
    if (read_frames < 0) {
      if (read_frames == -EAGAIN) {
        continue;
      }
      if (read_frames == -EPIPE) {
        ++stats.xruns;
      }
      if (read_frames == -EINTR && g_stop.load(std::memory_order_relaxed)) {
        break;
      }
      const int recovered = snd_pcm_recover(pcm.get(), static_cast<int>(read_frames), 1);
      if (recovered < 0) {
        ++stats.read_errors;
        std::cerr << "capture_error stage=read error=" << snd_strerror(static_cast<int>(read_frames))
                  << " recovery=" << snd_strerror(recovered) << '\n';
        break;
      }
      ++stats.recoveries;
      continue;
    }
    if (read_frames == 0) {
      continue;
    }

    const std::size_t sample_count = static_cast<std::size_t>(read_frames) * actual_channels;
    interval.add(samples.data(), sample_count);
    stats.frames += static_cast<std::uint64_t>(read_frames);
    if (!options.wav_path.empty()) {
      wav.write(samples.data(), sample_count);
    }

    const auto report_time = Clock::now();
    if (report_time >= next_report) {
      const auto report_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(report_time - started).count();
      print_interval(interval, stats, static_cast<std::uint64_t>(report_elapsed));
      interval = IntervalStats{};
      next_report = report_time + std::chrono::milliseconds(options.report_ms);
    }
  }

  const auto finished = Clock::now();
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(finished - started).count();
  if (interval.samples != 0) {
    print_interval(interval, stats, static_cast<std::uint64_t>(elapsed));
  }
  wav.close();
  snd_pcm_drop(pcm.get());

  std::cout << "capture_finished elapsed_ms=" << elapsed
            << " frames=" << stats.frames
            << " xruns=" << stats.xruns
            << " recoveries=" << stats.recoveries
            << " read_errors=" << stats.read_errors
            << " stopped_by_signal=" << (g_stop.load(std::memory_order_relaxed) ? "true" : "false")
            << '\n';
  return stats.read_errors == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  try {
    const Options options = parse_options(argc, argv);
    return run_capture(options);
  } catch (const std::exception& error) {
    std::cerr << "capture_probe_error " << error.what() << '\n';
    return 1;
  }
}
