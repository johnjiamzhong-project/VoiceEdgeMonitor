#include <alsa/asoundlib.h>

#include "audio_frame.hpp"
#include "bounded_queue.hpp"
#include "energy_vad.hpp"
#include "voiceedge_protocol.hpp"
#ifdef VOICEEDGE_HAS_ASR
#include "offline_asr.hpp"
#endif
#include "recording_store.hpp"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/beast/websocket.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cerrno>
#include <deque>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
using Clock = std::chrono::steady_clock;

using voiceedge::AudioFrame;
using voiceedge::AudioFramePtr;
using voiceedge::BoundedQueue;
using voiceedge::protocol::encode_audio_packet;
using voiceedge::protocol::json_escape;

constexpr unsigned int kDefaultRate = 16000;
constexpr unsigned int kDefaultChannels = 1;
constexpr snd_pcm_uframes_t kDefaultPeriodFrames = 2000;
constexpr snd_pcm_uframes_t kDefaultBufferFrames = 8000;
constexpr std::uint16_t kDefaultPort = 8765;
constexpr std::uint64_t kDefaultStateMs = 1000;
constexpr std::size_t kDefaultClientQueueCapacity = 128;
constexpr std::size_t kDefaultMaxClients = 4;
constexpr const char* kDefaultDevice = "hw:CARD=U0x46d0x81b,DEV=0";
constexpr const char* kDefaultPath = "/voiceedge";

std::atomic<bool> g_signal_stop{false};

void handle_signal(int) noexcept {
  g_signal_stop.store(true, std::memory_order_relaxed);
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
  std::string bind_address{"0.0.0.0"};
  std::uint16_t port{kDefaultPort};
  std::string path{kDefaultPath};
  std::string device;
  unsigned int rate{kDefaultRate};
  unsigned int channels{kDefaultChannels};
  snd_pcm_uframes_t period_frames{kDefaultPeriodFrames};
  snd_pcm_uframes_t buffer_frames{kDefaultBufferFrames};
  voiceedge::VadConfig vad_config{};
  std::uint64_t run_ms{0};
  std::uint64_t state_ms{kDefaultStateMs};
  std::size_t client_queue_capacity{kDefaultClientQueueCapacity};
  std::size_t max_clients{kDefaultMaxClients};
  std::size_t asr_queue_capacity{4};
  std::uint64_t max_segment_ms{30000};
  std::string asr_model;
  std::string asr_tokens;
  std::string asr_language{"zh"};
  std::string asr_provider{"cpu"};
  int asr_threads{2};
  std::string persist_dir;
  bool persist_audio{false};
  bool persist_transcript{false};
  std::size_t persist_queue_capacity{32};
};

struct AlsaCapture {
  PcmPtr pcm;
  unsigned int rate{0};
  unsigned int channels{0};
  snd_pcm_uframes_t period_frames{0};
  snd_pcm_uframes_t buffer_frames{0};
};

struct ServerStats {
  std::atomic<std::uint64_t> captured_frames{0};
  std::atomic<std::uint64_t> xruns{0};
  std::atomic<std::uint64_t> recoveries{0};
  std::atomic<std::uint64_t> read_errors{0};
  std::atomic<std::int64_t> last_rms_dbfs_x10{-1200};
  std::atomic<std::int64_t> last_peak_dbfs_x10{-1200};
  std::atomic<std::uint64_t> sequence{0};
  std::atomic<std::uint64_t> vad_started{0};
  std::atomic<std::uint64_t> vad_ended{0};
  std::atomic<int> vad_state{static_cast<int>(voiceedge::VadState::Idle)};
  std::atomic<std::uint64_t> asr_segments{0};
  std::atomic<std::uint64_t> asr_drops{0};
  std::atomic<std::uint64_t> asr_failures{0};
  std::atomic<std::uint64_t> asr_overflows{0};
  std::atomic<std::uint64_t> asr_processing_ms{0};
  std::atomic<bool> asr_processing{false};
  std::atomic<std::uint64_t> persist_drops{0};
  std::atomic<std::uint64_t> persist_failures{0};
  std::atomic<std::uint64_t> capture_reconnects{0};
  std::atomic<std::uint64_t> capture_reconnect_failures{0};
  std::atomic<int> audio_state{0};  // 0=online, 1=reconnecting
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

double parse_double(const std::string& value, std::string_view option) {
  try {
    std::size_t consumed = 0;
    const double parsed = std::stod(value, &consumed);
    if (consumed != value.size() || !std::isfinite(parsed)) {
      throw std::invalid_argument("invalid floating point value");
    }
    return parsed;
  } catch (const std::exception&) {
    throw std::invalid_argument(std::string(option) + " expects a finite number: " + value);
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

Options parse_options(int argc, char** argv) {
  Options options;
  options.device = default_device();
  for (int index = 1; index < argc; ++index) {
    const std::string argument(argv[index]);
    if (argument == "--help" || argument == "-h") {
      std::cout
          << "Usage: " << argv[0] << " [options]\n"
          << "  --bind <address>              bind address (default: 0.0.0.0)\n"
          << "  --port <port>                 WebSocket port (default: 8765)\n"
          << "  --path <path>                 WebSocket path (default: /voiceedge)\n"
          << "  --device <name>               ALSA capture device\n"
          << "  --rate <hz>                   capture rate (default: 16000)\n"
          << "  --channels <count>            capture channels (default: 1)\n"
          << "  --period-frames <n>           ALSA period size (default: 2000)\n"
          << "  --buffer-frames <n>           ALSA buffer size (default: 8000)\n"
          << "  --vad-start-rms <value>      speech-start RMS (default: 0.015)\n"
          << "  --vad-end-rms <value>        speech-end RMS (default: 0.012)\n"
          << "  --vad-min-speech-ms <ms>     minimum speech duration (default: 125)\n"
          << "  --vad-silence-ms <ms>        trailing silence duration (default: 1000)\n"
          << "  --run-ms <ms>                 stop after duration; 0 means Ctrl+C\n"
          << "  --state-ms <ms>               state broadcast interval (default: 1000)\n"
          << "  --client-queue-capacity <n>   per-client bounded queue (default: 128)\n"
          << "  --max-clients <n>             maximum clients (default: 4)\n"
          << "  --vad-start-rms <value>       speech-start RMS (default: 0.015)\n"
          << "  --vad-end-rms <value>         speech-end RMS (default: 0.012)\n"
          << "  --vad-min-speech-ms <ms>      minimum speech duration (default: 125)\n"
          << "  --vad-silence-ms <ms>         trailing silence duration (default: 1000)\n"
          << "  --asr-model <path>            optional SenseVoice model.int8.onnx\n"
          << "  --asr-tokens <path>           optional SenseVoice tokens.txt\n"
          << "  --asr-language <name>         ASR language (default: zh)\n"
          << "  --asr-provider <name>         ASR provider (default: cpu)\n"
          << "  --asr-threads <n>             ASR inference threads (default: 2)\n"
          << "  --asr-queue-capacity <n>      ASR segment queue capacity (default: 4)\n"
          << "  --max-segment-ms <ms>         maximum speech segment (default: 30000)\n"
          << "  --persist-dir <path>          local recording/transcript root\n"
          << "  --persist-audio               save speech segments as WAV\n"
          << "  --persist-transcript          save recognition records as JSONL\n"
          << "  --persist-queue-capacity <n>  persistence queue capacity (default: 32)\n";
      std::exit(0);
    }
    if (argument.rfind("--bind", 0) == 0) {
      options.bind_address = option_value(index, argc, argv, "--bind");
    } else if (argument.rfind("--port", 0) == 0) {
      const auto value = parse_uint64(option_value(index, argc, argv, "--port"), "--port");
      if (value == 0 || value > std::numeric_limits<std::uint16_t>::max()) {
        throw std::invalid_argument("--port is outside the valid range");
      }
      options.port = static_cast<std::uint16_t>(value);
    } else if (argument.rfind("--path", 0) == 0) {
      options.path = option_value(index, argc, argv, "--path");
      if (options.path.empty() || options.path.front() != '/') {
        throw std::invalid_argument("--path must start with '/'");
      }
    } else if (argument.rfind("--device", 0) == 0) {
      options.device = option_value(index, argc, argv, "--device");
    } else if (argument.rfind("--rate", 0) == 0) {
      const auto value = parse_uint64(option_value(index, argc, argv, "--rate"), "--rate");
      if (value == 0 || value > std::numeric_limits<unsigned int>::max()) {
        throw std::invalid_argument("--rate is outside the supported range");
      }
      options.rate = static_cast<unsigned int>(value);
    } else if (argument.rfind("--channels", 0) == 0) {
      const auto value = parse_uint64(option_value(index, argc, argv, "--channels"), "--channels");
      if (value == 0 || value > 8) {
        throw std::invalid_argument("--channels must be between 1 and 8");
      }
      options.channels = static_cast<unsigned int>(value);
    } else if (argument.rfind("--period-frames", 0) == 0) {
      const auto value = parse_uint64(option_value(index, argc, argv, "--period-frames"), "--period-frames");
      if (value == 0 || value > std::numeric_limits<snd_pcm_uframes_t>::max()) {
        throw std::invalid_argument("--period-frames is outside the supported range");
      }
      options.period_frames = static_cast<snd_pcm_uframes_t>(value);
    } else if (argument.rfind("--buffer-frames", 0) == 0) {
      const auto value = parse_uint64(option_value(index, argc, argv, "--buffer-frames"), "--buffer-frames");
      if (value == 0 || value > std::numeric_limits<snd_pcm_uframes_t>::max()) {
        throw std::invalid_argument("--buffer-frames is outside the supported range");
      }
      options.buffer_frames = static_cast<snd_pcm_uframes_t>(value);
    } else if (argument.rfind("--vad-start-rms", 0) == 0) {
      options.vad_config.start_rms = parse_double(
          option_value(index, argc, argv, "--vad-start-rms"), "--vad-start-rms");
    } else if (argument.rfind("--vad-end-rms", 0) == 0) {
      options.vad_config.end_rms = parse_double(
          option_value(index, argc, argv, "--vad-end-rms"), "--vad-end-rms");
    } else if (argument.rfind("--vad-min-speech-ms", 0) == 0) {
      options.vad_config.min_speech_ms = parse_uint64(
          option_value(index, argc, argv, "--vad-min-speech-ms"), "--vad-min-speech-ms");
    } else if (argument.rfind("--vad-silence-ms", 0) == 0) {
      options.vad_config.silence_ms = parse_uint64(
          option_value(index, argc, argv, "--vad-silence-ms"), "--vad-silence-ms");
    } else if (argument.rfind("--run-ms", 0) == 0) {
      options.run_ms = parse_uint64(option_value(index, argc, argv, "--run-ms"), "--run-ms");
    } else if (argument.rfind("--state-ms", 0) == 0) {
      options.state_ms = parse_uint64(option_value(index, argc, argv, "--state-ms"), "--state-ms");
      if (options.state_ms == 0) {
        throw std::invalid_argument("--state-ms must be greater than zero");
      }
    } else if (argument.rfind("--client-queue-capacity", 0) == 0) {
      options.client_queue_capacity = static_cast<std::size_t>(parse_uint64(
          option_value(index, argc, argv, "--client-queue-capacity"), "--client-queue-capacity"));
      if (options.client_queue_capacity == 0) {
        throw std::invalid_argument("--client-queue-capacity must be greater than zero");
      }
    } else if (argument.rfind("--max-clients", 0) == 0) {
      options.max_clients = static_cast<std::size_t>(
          parse_uint64(option_value(index, argc, argv, "--max-clients"), "--max-clients"));
      if (options.max_clients == 0) {
        throw std::invalid_argument("--max-clients must be greater than zero");
      }
    } else if (argument.rfind("--vad-start-rms", 0) == 0) {
      options.vad_config.start_rms = parse_double(
          option_value(index, argc, argv, "--vad-start-rms"), "--vad-start-rms");
    } else if (argument.rfind("--vad-end-rms", 0) == 0) {
      options.vad_config.end_rms = parse_double(
          option_value(index, argc, argv, "--vad-end-rms"), "--vad-end-rms");
    } else if (argument.rfind("--vad-min-speech-ms", 0) == 0) {
      options.vad_config.min_speech_ms = parse_uint64(
          option_value(index, argc, argv, "--vad-min-speech-ms"), "--vad-min-speech-ms");
    } else if (argument.rfind("--vad-silence-ms", 0) == 0) {
      options.vad_config.silence_ms = parse_uint64(
          option_value(index, argc, argv, "--vad-silence-ms"), "--vad-silence-ms");
    } else if (argument.rfind("--asr-model", 0) == 0) {
      options.asr_model = option_value(index, argc, argv, "--asr-model");
    } else if (argument.rfind("--asr-tokens", 0) == 0) {
      options.asr_tokens = option_value(index, argc, argv, "--asr-tokens");
    } else if (argument.rfind("--asr-language", 0) == 0) {
      options.asr_language = option_value(index, argc, argv, "--asr-language");
    } else if (argument.rfind("--asr-provider", 0) == 0) {
      options.asr_provider = option_value(index, argc, argv, "--asr-provider");
    } else if (argument.rfind("--asr-threads", 0) == 0) {
      const auto value = parse_uint64(option_value(index, argc, argv, "--asr-threads"), "--asr-threads");
      if (value == 0 || value > std::numeric_limits<int>::max()) {
        throw std::invalid_argument("--asr-threads is outside the supported range");
      }
      options.asr_threads = static_cast<int>(value);
    } else if (argument.rfind("--asr-queue-capacity", 0) == 0) {
      options.asr_queue_capacity = static_cast<std::size_t>(parse_uint64(
          option_value(index, argc, argv, "--asr-queue-capacity"), "--asr-queue-capacity"));
      if (options.asr_queue_capacity == 0) {
        throw std::invalid_argument("--asr-queue-capacity must be greater than zero");
      }
    } else if (argument.rfind("--max-segment-ms", 0) == 0) {
      options.max_segment_ms = parse_uint64(
          option_value(index, argc, argv, "--max-segment-ms"), "--max-segment-ms");
      if (options.max_segment_ms == 0) {
        throw std::invalid_argument("--max-segment-ms must be greater than zero");
      }
    } else if (argument.rfind("--persist-dir", 0) == 0) {
      options.persist_dir = option_value(index, argc, argv, "--persist-dir");
    } else if (argument == "--persist-audio") {
      options.persist_audio = true;
    } else if (argument == "--persist-transcript") {
      options.persist_transcript = true;
    } else if (argument.rfind("--persist-queue-capacity", 0) == 0) {
      options.persist_queue_capacity = static_cast<std::size_t>(parse_uint64(
          option_value(index, argc, argv, "--persist-queue-capacity"), "--persist-queue-capacity"));
      if (options.persist_queue_capacity == 0) {
        throw std::invalid_argument("--persist-queue-capacity must be greater than zero");
      }
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

AlsaCapture open_capture(const Options& options) {
  AlsaCapture capture;
  snd_pcm_t* raw_pcm = nullptr;
  check_alsa(snd_pcm_open(&raw_pcm, options.device.c_str(), SND_PCM_STREAM_CAPTURE, 0), "snd_pcm_open");
  capture.pcm.reset(raw_pcm);

  snd_pcm_hw_params_t* raw_params = nullptr;
  check_alsa(snd_pcm_hw_params_malloc(&raw_params), "snd_pcm_hw_params_malloc");
  HwParamsPtr params(raw_params);
  check_alsa(snd_pcm_hw_params_any(capture.pcm.get(), params.get()), "snd_pcm_hw_params_any");
  check_alsa(snd_pcm_hw_params_set_access(capture.pcm.get(), params.get(), SND_PCM_ACCESS_RW_INTERLEAVED),
             "snd_pcm_hw_params_set_access");
  check_alsa(snd_pcm_hw_params_set_format(capture.pcm.get(), params.get(), SND_PCM_FORMAT_S16_LE),
             "snd_pcm_hw_params_set_format");
  check_alsa(snd_pcm_hw_params_set_rate(capture.pcm.get(), params.get(), options.rate, 0),
             "snd_pcm_hw_params_set_rate");
  check_alsa(snd_pcm_hw_params_set_channels(capture.pcm.get(), params.get(), options.channels),
             "snd_pcm_hw_params_set_channels");
  snd_pcm_uframes_t requested_period = options.period_frames;
  int period_direction = 0;
  check_alsa(snd_pcm_hw_params_set_period_size_near(
                 capture.pcm.get(), params.get(), &requested_period, &period_direction),
             "snd_pcm_hw_params_set_period_size_near");
  snd_pcm_uframes_t requested_buffer = options.buffer_frames;
  check_alsa(snd_pcm_hw_params_set_buffer_size_near(
                 capture.pcm.get(), params.get(), &requested_buffer),
             "snd_pcm_hw_params_set_buffer_size_near");
  check_alsa(snd_pcm_hw_params(capture.pcm.get(), params.get()), "snd_pcm_hw_params");

  int direction = 0;
  check_alsa(snd_pcm_hw_params_get_rate(params.get(), &capture.rate, &direction),
             "snd_pcm_hw_params_get_rate");
  check_alsa(snd_pcm_hw_params_get_channels(params.get(), &capture.channels),
             "snd_pcm_hw_params_get_channels");
  check_alsa(snd_pcm_hw_params_get_period_size(params.get(), &capture.period_frames, &direction),
             "snd_pcm_hw_params_get_period_size");
  check_alsa(snd_pcm_hw_params_get_buffer_size(params.get(), &capture.buffer_frames),
             "snd_pcm_hw_params_get_buffer_size");
  if (capture.rate != options.rate || capture.channels != options.channels) {
    throw std::runtime_error("ALSA accepted parameters different from requested values");
  }
  check_alsa(snd_pcm_prepare(capture.pcm.get()), "snd_pcm_prepare");
  return capture;
}

class ClientSession : public std::enable_shared_from_this<ClientSession> {
 public:
  struct Message {
    std::shared_ptr<std::vector<std::uint8_t>> bytes;
    bool binary{false};
  };

  ClientSession(tcp::socket socket, std::string path, std::size_t queue_capacity)
      : websocket_(std::move(socket)), path_(std::move(path)), queue_capacity_(queue_capacity) {}

  ~ClientSession() {
    stop();
    join();
  }

  ClientSession(const ClientSession&) = delete;
  ClientSession& operator=(const ClientSession&) = delete;

  void start() {
    thread_ = std::thread([self = shared_from_this()] { self->run(); });
  }

  void join() {
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  void stop() {
    const bool was_stopped = stopped_.exchange(true, std::memory_order_relaxed);
    if (!was_stopped) {
      queue_cv_.notify_all();
      beast::error_code error;
      websocket_.next_layer().cancel(error);
      websocket_.next_layer().shutdown(tcp::socket::shutdown_both, error);
      websocket_.next_layer().close(error);
    }
  }

  [[nodiscard]] bool active() const { return !stopped_.load(std::memory_order_relaxed); }

  [[nodiscard]] std::uint64_t dropped_messages() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return dropped_messages_;
  }

  void enqueue(std::shared_ptr<std::vector<std::uint8_t>> bytes, bool binary) {
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      if (stopped_) {
        return;
      }
      if (queue_.size() >= queue_capacity_) {
        queue_.pop_front();
        ++dropped_messages_;
      }
      queue_.push_back(Message{std::move(bytes), binary});
    }
    queue_cv_.notify_one();
  }

 private:
  void run() {
    try {
      websocket_.set_option(websocket::stream_base::timeout::suggested(
          beast::role_type::server));
      beast::flat_buffer handshake_buffer;
      http::request<http::empty_body> request;
      beast::error_code error;
      http::read(websocket_.next_layer(), handshake_buffer, request, error);
      if (error) {
        std::cerr << "ws_client_error stage=http_read error=" << error.message() << '\n';
        stop();
        return;
      }
      if (std::string(request.target()) != path_) {
        http::response<http::string_body> response(http::status::not_found, request.version());
        response.set(http::field::content_type, "text/plain");
        response.body() = "WebSocket path not found\n";
        response.prepare_payload();
        http::write(websocket_.next_layer(), response, error);
        stop();
        return;
      }
      websocket_.accept(request, error);
      if (error) {
        std::cerr << "ws_client_error stage=handshake error=" << error.message() << '\n';
        stop();
        return;
      }

      writer_thread_ = std::thread([self = shared_from_this()] { self->writer_loop(); });
      beast::flat_buffer buffer;
      while (!stopped_.load(std::memory_order_relaxed)) {
        websocket_.read(buffer, error);
        if (error) {
          if (error != websocket::error::closed && error != asio::error::operation_aborted &&
              !stopped_.load(std::memory_order_relaxed)) {
            std::cerr << "ws_client_error stage=read error=" << error.message() << '\n';
          }
          break;
        }
        buffer.consume(buffer.size());
      }
    } catch (const std::exception& error) {
      std::cerr << "ws_client_error stage=session error=" << error.what() << '\n';
    }
    stop();
    if (writer_thread_.joinable()) {
      writer_thread_.join();
    }
  }

  void writer_loop() {
    while (true) {
      Message message;
      {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        queue_cv_.wait(lock, [this] { return stopped_ || !queue_.empty(); });
        if (queue_.empty()) {
          return;
        }
        message = std::move(queue_.front());
        queue_.pop_front();
      }

      websocket_.binary(message.binary);
      beast::error_code error;
      websocket_.write(asio::buffer(*message.bytes), error);
      if (error) {
        if (error != asio::error::operation_aborted) {
          std::cerr << "ws_client_error stage=write error=" << error.message() << '\n';
        }
        stop();
        return;
      }
    }
  }

  websocket::stream<tcp::socket> websocket_;
  const std::string path_;
  const std::size_t queue_capacity_;
  mutable std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  std::deque<Message> queue_;
  std::uint64_t dropped_messages_{0};
  std::atomic<bool> stopped_{false};
  std::thread thread_;
  std::thread writer_thread_;
};

class ServerRuntime {
 public:
  ServerRuntime(const Options& options, AlsaCapture audio)
      : options_(options),
        audio_(std::move(audio)),
        io_context_(),
        acceptor_(io_context_),
        started_(Clock::now()),
        stats_{},
        vad_(options.vad_config),
        asr_queue_(options.asr_queue_capacity) {}

  ServerRuntime(const ServerRuntime&) = delete;
  ServerRuntime& operator=(const ServerRuntime&) = delete;

  void start() {
#ifdef VOICEEDGE_HAS_ASR
    if (!options_.asr_model.empty() || !options_.asr_tokens.empty()) {
      if (options_.asr_model.empty() || options_.asr_tokens.empty()) {
        throw std::invalid_argument("--asr-model and --asr-tokens must be supplied together");
      }
      asr_ = std::make_unique<voiceedge::OfflineASR>();
      asr_->open(options_.asr_model, options_.asr_tokens, options_.asr_language,
                 options_.asr_provider, options_.asr_threads);
      std::cout << "ws_asr_enabled model=\"" << options_.asr_model
                << "\" provider=" << options_.asr_provider
                << " threads=" << options_.asr_threads << '\n';
    }
#else
    if (!options_.asr_model.empty() || !options_.asr_tokens.empty()) {
      throw std::invalid_argument("ws_server was built without ASR support; rebuild with VOICEEDGE_BUILD_ASR=ON");
    }
#endif
    if (options_.persist_audio || options_.persist_transcript) {
      if (options_.persist_dir.empty()) {
        throw std::invalid_argument("--persist-dir is required when persistence is enabled");
      }
      recording_store_ = std::make_unique<voiceedge::RecordingStore>(
          voiceedge::RecordingStoreConfig{options_.persist_dir, options_.persist_audio,
                                          options_.persist_transcript, options_.persist_queue_capacity});
      recording_store_->start();
      std::cout << "ws_persistence_enabled root=\"" << options_.persist_dir
                << "\" audio=" << (options_.persist_audio ? "true" : "false")
                << " transcript=" << (options_.persist_transcript ? "true" : "false") << '\n';
    }
    const auto address = asio::ip::make_address(options_.bind_address);
    tcp::endpoint endpoint(address, options_.port);
    beast::error_code error;
    acceptor_.open(endpoint.protocol(), error);
    if (error) {
      throw std::runtime_error("acceptor open failed: " + error.message());
    }
    acceptor_.set_option(asio::socket_base::reuse_address(true), error);
    if (error) {
      throw std::runtime_error("acceptor set_option failed: " + error.message());
    }
    acceptor_.bind(endpoint, error);
    if (error) {
      throw std::runtime_error("acceptor bind failed: " + error.message());
    }
    acceptor_.listen(asio::socket_base::max_listen_connections, error);
    if (error) {
      throw std::runtime_error("acceptor listen failed: " + error.message());
    }
    acceptor_.non_blocking(true, error);
    if (error) {
      throw std::runtime_error("acceptor non_blocking failed: " + error.message());
    }

    accept_thread_ = std::thread([this] { accept_loop(); });
    capture_thread_ = std::thread([this] { capture_loop(); });
#ifdef VOICEEDGE_HAS_ASR
    if (asr_) {
      asr_thread_ = std::thread([this] { asr_loop(); });
    }
#endif
    std::cout << "ws_server_started bind=" << options_.bind_address
              << " port=" << options_.port
              << " path=" << options_.path
              << " device=" << options_.device
              << " rate=" << audio_.rate
              << " channels=" << audio_.channels
              << " period_frames=" << audio_.period_frames
              << " buffer_frames=" << audio_.buffer_frames << '\n';
  }

  void run() {
    auto next_state = Clock::now();
    while (!stopping_.load(std::memory_order_relaxed) &&
           !g_signal_stop.load(std::memory_order_relaxed)) {
      const auto now = Clock::now();
      const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - started_).count();
      if (options_.run_ms != 0 && static_cast<std::uint64_t>(elapsed) >= options_.run_ms) {
        break;
      }
      if (stats_.read_errors.load(std::memory_order_relaxed) != 0) {
        break;
      }
      if (now >= next_state) {
        broadcast_text(make_state_json(static_cast<std::uint64_t>(elapsed)));
        next_state = now + std::chrono::milliseconds(options_.state_ms);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }

  void stop() {
    const bool was_stopping = stopping_.exchange(true, std::memory_order_relaxed);
    if (was_stopping) {
      return;
    }
    std::cerr << "ws_server_shutdown stage=begin\n";
    beast::error_code error;
    acceptor_.cancel(error);
    acceptor_.close(error);
    snd_pcm_t* pcm = pcm_for_stop_.load(std::memory_order_acquire);
    if (pcm != nullptr) {
      snd_pcm_drop(pcm);
    }

    std::vector<std::shared_ptr<ClientSession>> clients;
    {
      std::lock_guard<std::mutex> lock(clients_mutex_);
      clients = clients_;
    }
    for (const auto& client : clients) {
      client->stop();
    }
    if (accept_thread_.joinable()) {
      accept_thread_.join();
    }
    std::cerr << "ws_server_shutdown stage=accept_joined\n";
    if (capture_thread_.joinable()) {
      capture_thread_.join();
    }
    std::cerr << "ws_server_shutdown stage=capture_joined\n";
    asr_queue_.close();
#ifdef VOICEEDGE_HAS_ASR
    if (asr_thread_.joinable()) {
      asr_thread_.join();
    }
#endif
    std::cerr << "ws_server_shutdown stage=asr_joined\n";
    if (recording_store_) {
      recording_store_->stop();
      stats_.persist_failures.store(recording_store_->failed_records(), std::memory_order_relaxed);
    }
    std::cerr << "ws_server_shutdown stage=persistence_joined\n";
    for (const auto& client : clients) {
      client->join();
    }
    std::cerr << "ws_server_shutdown stage=clients_joined\n";
  }

  ~ServerRuntime() { stop(); }

 private:
  void accept_loop() {
    while (!stopping_.load(std::memory_order_relaxed)) {
      beast::error_code error;
      tcp::socket socket(io_context_);
      acceptor_.accept(socket, error);
      if (error) {
        if (error == asio::error::would_block || error == asio::error::try_again) {
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
          continue;
        }
        if (!stopping_.load(std::memory_order_relaxed) &&
            error != asio::error::operation_aborted) {
          std::cerr << "ws_server_error stage=accept error=" << error.message() << '\n';
        }
        continue;
      }

      std::shared_ptr<ClientSession> client;
      {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        const std::size_t active_clients = static_cast<std::size_t>(std::count_if(
            clients_.begin(), clients_.end(), [](const auto& item) { return item->active(); }));
        if (active_clients >= options_.max_clients) {
          beast::error_code close_error;
          socket.close(close_error);
          continue;
        }
        client = std::make_shared<ClientSession>(std::move(socket), options_.path,
                                                 options_.client_queue_capacity);
        clients_.push_back(client);
      }
      client->start();
      client->enqueue(make_text("{\"type\":\"hello\",\"version\":1,\"server\":\"VoiceEdgeMonitor\"}"),
                      false);
    }
  }

  void handle_vad_segment(const AudioFrame& frame, const voiceedge::VadDecision& decision) {
#ifdef VOICEEDGE_HAS_ASR
    if (!asr_) {
      return;
    }
    if (!segment_active_ && decision.state != voiceedge::VadState::Idle) {
      segment_active_ = true;
      segment_start_ms_ = frame.timestamp_ms;
      segment_samples_.clear();
    }
    if (!segment_active_) {
      return;
    }

    segment_samples_.insert(segment_samples_.end(), frame.samples.begin(), frame.samples.end());
    if (frame.timestamp_ms >= segment_start_ms_ &&
        frame.timestamp_ms - segment_start_ms_ > options_.max_segment_ms) {
      ++stats_.asr_overflows;
      segment_active_ = false;
      segment_samples_.clear();
      vad_.reset();
      broadcast_text("{\"type\":\"asr_event\",\"event\":\"segment_overflow\"}");
      return;
    }

    if (decision.event.has_value() && decision.event->type == voiceedge::VadEventType::Ended) {
      auto segment = std::make_shared<voiceedge::SpeechSegment>();
      segment->id = next_segment_id_++;
      segment->start_timestamp_ms = segment_start_ms_;
      segment->end_timestamp_ms = decision.event->timestamp_ms;
      segment->sample_rate = frame.sample_rate;
      segment->channels = frame.channels;
      segment->samples = std::move(segment_samples_);
      segment_samples_.clear();
      segment_active_ = false;
      ++stats_.asr_segments;
      if (!asr_queue_.try_push(std::move(segment))) {
        ++stats_.asr_drops;
        broadcast_text("{\"type\":\"asr_event\",\"event\":\"segment_dropped\"}");
      }
    } else if (decision.state == voiceedge::VadState::Idle && !decision.event.has_value()) {
      segment_active_ = false;
      segment_samples_.clear();
    }
#else
    (void)frame;
    (void)decision;
#endif
  }

  void capture_loop() {
    std::vector<std::int16_t> samples(
        static_cast<std::size_t>(audio_.period_frames) * audio_.channels);
    pcm_for_stop_.store(audio_.pcm.get(), std::memory_order_release);
    while (!stopping_.load(std::memory_order_relaxed) &&
           !g_signal_stop.load(std::memory_order_relaxed)) {
      const snd_pcm_sframes_t read_frames =
          snd_pcm_readi(audio_.pcm.get(), samples.data(), audio_.period_frames);
      if (read_frames < 0) {
        if (stopping_.load(std::memory_order_relaxed) ||
            g_signal_stop.load(std::memory_order_relaxed)) {
          break;
        }
        if (read_frames == -EPIPE) {
          ++stats_.xruns;
        }
        const int recovered = snd_pcm_recover(audio_.pcm.get(), static_cast<int>(read_frames), 1);
        if (recovered < 0) {
          ++stats_.read_errors;
          if (!reopen_capture()) {
            break;
          }
          samples.resize(static_cast<std::size_t>(audio_.period_frames) * audio_.channels);
          continue;
        }
        ++stats_.recoveries;
        continue;
      }
      if (read_frames == 0) {
        continue;
      }

      const std::size_t frame_count = static_cast<std::size_t>(read_frames);
      const std::size_t sample_count = frame_count * audio_.channels;
      auto frame = std::make_shared<AudioFrame>();
      frame->sequence = stats_.sequence.fetch_add(1, std::memory_order_relaxed);
      frame->timestamp_ms = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started_).count());
      frame->sample_rate = audio_.rate;
      frame->channels = audio_.channels;
      frame->samples.assign(samples.begin(), samples.begin() + sample_count);
      stats_.captured_frames.fetch_add(frame_count, std::memory_order_relaxed);
      update_level_stats(*frame);

      const auto vad_decision = vad_.process(*frame);
      stats_.vad_state.store(static_cast<int>(vad_decision.state), std::memory_order_relaxed);
      if (vad_decision.event.has_value()) {
        if (vad_decision.event->type == voiceedge::VadEventType::Started) {
          ++stats_.vad_started;
        } else {
          ++stats_.vad_ended;
        }
        broadcast_text(make_vad_event_json(*vad_decision.event, vad_decision.rms));
      }
      handle_vad_segment(*frame, vad_decision);

      const auto packet = std::make_shared<std::vector<std::uint8_t>>(encode_audio_packet(*frame));
      broadcast(packet, true);
    }
    pcm_for_stop_.store(nullptr, std::memory_order_release);
    asr_queue_.close();
  }

  bool reopen_capture() {
    stats_.audio_state.store(1, std::memory_order_relaxed);
    broadcast_text("{\"type\":\"audio_event\",\"event\":\"capture_reconnecting\"}");
    pcm_for_stop_.store(nullptr, std::memory_order_release);
    audio_.pcm.reset();

    std::uint64_t backoff_ms = 100;
    while (!stopping_.load(std::memory_order_relaxed) &&
           !g_signal_stop.load(std::memory_order_relaxed)) {
      for (std::uint64_t elapsed = 0; elapsed < backoff_ms &&
                                      !stopping_.load(std::memory_order_relaxed) &&
                                      !g_signal_stop.load(std::memory_order_relaxed);
           elapsed += 50) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      if (stopping_.load(std::memory_order_relaxed) || g_signal_stop.load(std::memory_order_relaxed)) {
        return false;
      }
      try {
        audio_ = open_capture(options_);
        pcm_for_stop_.store(audio_.pcm.get(), std::memory_order_release);
        stats_.capture_reconnects.fetch_add(1, std::memory_order_relaxed);
        stats_.audio_state.store(0, std::memory_order_relaxed);
        broadcast_text("{\"type\":\"audio_event\",\"event\":\"capture_reconnected\"}");
        return true;
      } catch (const std::exception& error) {
        stats_.capture_reconnect_failures.fetch_add(1, std::memory_order_relaxed);
        std::cerr << "ws_server_error stage=capture_reopen error=" << error.what() << '\n';
        backoff_ms = std::min<std::uint64_t>(backoff_ms * 2, 5000);
      }
    }
    return false;
  }

  void update_level_stats(const AudioFrame& frame) {
    long double sum_squared = 0.0L;
    int peak = 0;
    for (const std::int16_t sample : frame.samples) {
      const int value = static_cast<int>(sample);
      const int magnitude = value < 0 ? -value : value;
      peak = std::max(peak, magnitude);
      sum_squared += static_cast<long double>(value) * static_cast<long double>(value);
    }
    const double rms = frame.samples.empty()
                           ? 0.0
                           : std::sqrt(static_cast<double>(sum_squared / frame.samples.size()));
    stats_.last_rms_dbfs_x10.store(static_cast<std::int64_t>(std::llround(dbfs(rms) * 10.0)),
                                   std::memory_order_relaxed);
    stats_.last_peak_dbfs_x10.store(
        static_cast<std::int64_t>(std::llround(dbfs(static_cast<double>(peak)) * 10.0)),
        std::memory_order_relaxed);
  }

#ifdef VOICEEDGE_HAS_ASR
  void asr_loop() {
    voiceedge::SpeechSegmentPtr segment;
    while (true) {
      if (!asr_queue_.pop_for(segment, std::chrono::milliseconds(100))) {
        if (asr_queue_.closed()) {
          break;
        }
        continue;
      }
      stats_.asr_processing.store(true, std::memory_order_relaxed);
      {
        std::ostringstream json;
        json << "{\"type\":\"asr_started\",\"segment_id\":" << segment->id
             << ",\"start_timestamp_ms\":" << segment->start_timestamp_ms
             << ",\"end_timestamp_ms\":" << segment->end_timestamp_ms << "}";
        broadcast_text(json.str());
      }
      try {
        const voiceedge::AsrResult result = asr_->transcribe(*segment);
        stats_.asr_processing_ms.store(result.processing_ms, std::memory_order_relaxed);
        const double audio_ms = segment->channels == 0 || segment->sample_rate == 0
                                    ? 0.0
                                    : static_cast<double>(segment->samples.size()) * 1000.0 /
                                          (static_cast<double>(segment->channels) * segment->sample_rate);
        const double rtf = audio_ms > 0.0 ? static_cast<double>(result.processing_ms) / audio_ms : 0.0;
        std::ostringstream json;
        json << "{\"type\":\"asr_final\",\"segment_id\":" << segment->id
             << ",\"text\":\"" << json_escape(result.text) << "\""
             << ",\"processing_ms\":" << result.processing_ms
             << ",\"audio_ms\":" << std::fixed << std::setprecision(1) << audio_ms
             << ",\"rtf\":" << std::setprecision(4) << rtf << "}";
        broadcast_text(json.str());
        if (recording_store_ && !recording_store_->enqueue(
                voiceedge::RecordingRecord{segment, result.text, "final", result.processing_ms, rtf, ""})) {
          ++stats_.persist_drops;
        }
      } catch (const std::exception& error) {
        ++stats_.asr_failures;
        std::ostringstream json;
        json << "{\"type\":\"asr_failed\",\"segment_id\":" << segment->id
             << ",\"reason\":\"" << json_escape(error.what()) << "\"}";
        broadcast_text(json.str());
        if (recording_store_ && !recording_store_->enqueue(
                voiceedge::RecordingRecord{segment, "", "failed", 0, 0.0, error.what()})) {
          ++stats_.persist_drops;
        }
      }
      stats_.asr_processing.store(false, std::memory_order_relaxed);
    }
  }
#endif

  static std::shared_ptr<std::vector<std::uint8_t>> make_text(const std::string& text) {
    return std::make_shared<std::vector<std::uint8_t>>(text.begin(), text.end());
  }

  void broadcast(const std::shared_ptr<std::vector<std::uint8_t>>& bytes, bool binary) {
    std::vector<std::shared_ptr<ClientSession>> clients;
    {
      std::lock_guard<std::mutex> lock(clients_mutex_);
      clients = clients_;
    }
    for (const auto& client : clients) {
      client->enqueue(bytes, binary);
    }
  }

  void broadcast_text(const std::string& text) {
    broadcast(make_text(text), false);
  }

  static std::string make_vad_event_json(const voiceedge::VadEvent& event, double rms) {
    std::ostringstream json;
    json << "{\"type\":\"vad_event\",\"event\":\""
         << voiceedge::vad_event_name(event.type)
         << "\",\"timestamp_ms\":" << event.timestamp_ms
         << ",\"duration_ms\":" << event.duration_ms
         << ",\"rms\":" << std::fixed << std::setprecision(6) << rms << "}";
    return json.str();
  }

  std::string make_state_json(std::uint64_t elapsed_ms) const {
    std::ostringstream json;
    json << "{\"type\":\"state\",\"version\":1"
         << ",\"timestamp_ms\":" << elapsed_ms
         << ",\"device\":\"" << json_escape(options_.device) << "\""
         << ",\"sample_rate\":" << audio_.rate
         << ",\"channels\":" << audio_.channels
         << ",\"format\":\"S16_LE\""
         << ",\"captured_frames\":" << stats_.captured_frames.load(std::memory_order_relaxed)
         << ",\"xruns\":" << stats_.xruns.load(std::memory_order_relaxed)
         << ",\"recoveries\":" << stats_.recoveries.load(std::memory_order_relaxed)
         << ",\"read_errors\":" << stats_.read_errors.load(std::memory_order_relaxed)
         << ",\"clients\":" << active_client_count()
         << ",\"vad\":\""
         << voiceedge::vad_state_name(static_cast<voiceedge::VadState>(
                stats_.vad_state.load(std::memory_order_relaxed)))
         << "\",\"vad_started\":" << stats_.vad_started.load(std::memory_order_relaxed)
         << ",\"vad_ended\":" << stats_.vad_ended.load(std::memory_order_relaxed)
         << ",\"asr\":\""
         << (stats_.asr_processing.load(std::memory_order_relaxed) ? "processing" : "idle")
         << "\",\"asr_segments\":" << stats_.asr_segments.load(std::memory_order_relaxed)
         << ",\"asr_drops\":" << stats_.asr_drops.load(std::memory_order_relaxed)
         << ",\"asr_failures\":" << stats_.asr_failures.load(std::memory_order_relaxed)
         << ",\"asr_processing_ms\":" << stats_.asr_processing_ms.load(std::memory_order_relaxed)
         << ",\"audio_state\":\""
         << (stats_.audio_state.load(std::memory_order_relaxed) == 0 ? "online" : "reconnecting")
         << "\",\"capture_reconnects\":"
         << stats_.capture_reconnects.load(std::memory_order_relaxed)
         << ",\"capture_reconnect_failures\":"
         << stats_.capture_reconnect_failures.load(std::memory_order_relaxed)
         << ",\"persist_drops\":" << stats_.persist_drops.load(std::memory_order_relaxed)
         << ",\"persist_failures\":" << stats_.persist_failures.load(std::memory_order_relaxed)
         << "}";
    return json.str();
  }

  std::size_t active_client_count() const {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    return static_cast<std::size_t>(std::count_if(
        clients_.begin(), clients_.end(), [](const auto& client) { return client->active(); }));
  }

  const Options& options_;
  AlsaCapture audio_;
  asio::io_context io_context_;
  tcp::acceptor acceptor_;
  const Clock::time_point started_;
  ServerStats stats_;
  voiceedge::EnergyVAD vad_;
  BoundedQueue<voiceedge::SpeechSegmentPtr> asr_queue_;
#ifdef VOICEEDGE_HAS_ASR
  std::unique_ptr<voiceedge::OfflineASR> asr_;
#endif
  std::unique_ptr<voiceedge::RecordingStore> recording_store_;
  bool segment_active_{false};
  std::uint64_t next_segment_id_{0};
  std::uint64_t segment_start_ms_{0};
  std::vector<std::int16_t> segment_samples_;
  std::atomic<bool> stopping_{false};
  mutable std::mutex clients_mutex_;
  std::vector<std::shared_ptr<ClientSession>> clients_;
  std::thread accept_thread_;
  std::thread capture_thread_;
#ifdef VOICEEDGE_HAS_ASR
  std::thread asr_thread_;
#endif
  std::atomic<snd_pcm_t*> pcm_for_stop_{nullptr};
};

}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);
  try {
    const Options options = parse_options(argc, argv);
    ServerRuntime server(options, open_capture(options));
    server.start();
    server.run();
    server.stop();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "ws_server_error " << error.what() << '\n';
    return 1;
  }
}
