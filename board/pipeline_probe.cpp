#include <alsa/asoundlib.h>

#include "audio_frame.hpp"
#include "bounded_queue.hpp"
#include "energy_vad.hpp"
#ifdef VOICEEDGE_HAS_ASR
#include "offline_asr.hpp"
#endif
#include "recording_store.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cerrno>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using voiceedge::AudioFramePtr;
using voiceedge::BoundedQueue;

constexpr unsigned int kDefaultRate = 16000;
constexpr unsigned int kDefaultChannels = 1;
constexpr snd_pcm_uframes_t kDefaultPeriodFrames = 2000;
constexpr snd_pcm_uframes_t kDefaultBufferFrames = 8000;
constexpr std::size_t kDefaultProcessingCapacity = 32;
constexpr std::size_t kDefaultMonitorCapacity = 64;
constexpr std::uint64_t kDefaultReportMs = 1000;
constexpr const char* kDefaultDevice = "hw:CARD=U0x46d0x81b,DEV=0";

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
  std::string device;
  unsigned int rate{kDefaultRate};
  unsigned int channels{kDefaultChannels};
  snd_pcm_uframes_t period_frames{kDefaultPeriodFrames};
  snd_pcm_uframes_t buffer_frames{kDefaultBufferFrames};
  std::uint64_t run_ms{0};
  std::uint64_t report_ms{kDefaultReportMs};
  std::size_t processing_capacity{kDefaultProcessingCapacity};
  std::size_t monitor_capacity{kDefaultMonitorCapacity};
  std::uint64_t processing_delay_ms{0};
  std::uint64_t monitor_delay_ms{0};
  voiceedge::VadConfig vad_config{};
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

struct PipelineStats {
  std::atomic<std::uint64_t> captured_frames{0};
  std::atomic<std::uint64_t> processed_frames{0};
  std::atomic<std::uint64_t> monitor_frames{0};
  std::atomic<std::uint64_t> processing_drops{0};
  std::atomic<std::uint64_t> monitor_drops{0};
  std::atomic<std::uint64_t> xruns{0};
  std::atomic<std::uint64_t> recoveries{0};
  std::atomic<std::uint64_t> read_errors{0};
  std::atomic<std::uint64_t> vad_started{0};
  std::atomic<std::uint64_t> vad_ended{0};
  std::atomic<std::uint64_t> processing_popped{0};
  std::atomic<std::uint64_t> monitor_popped{0};
  std::atomic<std::uint64_t> worker_errors{0};
  std::atomic<std::uint64_t> asr_segments{0};
  std::atomic<std::uint64_t> asr_drops{0};
  std::atomic<std::uint64_t> asr_failures{0};
  std::atomic<std::uint64_t> asr_overflows{0};
  std::atomic<std::uint64_t> asr_processing_ms{0};
  std::atomic<std::uint64_t> persist_drops{0};
  std::atomic<std::uint64_t> persist_failures{0};
  std::atomic<int> vad_state{static_cast<int>(voiceedge::VadState::Idle)};
  std::atomic<std::int64_t> last_rms_dbfs_x10{-1200};
  std::atomic<std::int64_t> last_peak_dbfs_x10{-1200};
};

struct PipelineContext {
  PipelineContext(const Options& options_value, AlsaCapture& audio_value)
      : options(options_value),
        audio(audio_value),
        processing_queue(options_value.processing_capacity),
        monitor_queue(options_value.monitor_capacity),
        asr_queue(options_value.asr_queue_capacity),
        stats{},
        vad(options_value.vad_config),
        started(Clock::now()) {}

  const Options& options;
  AlsaCapture& audio;
  BoundedQueue<AudioFramePtr> processing_queue;
  BoundedQueue<AudioFramePtr> monitor_queue;
  BoundedQueue<voiceedge::SpeechSegmentPtr> asr_queue;
  PipelineStats stats;
  voiceedge::EnergyVAD vad;
#ifdef VOICEEDGE_HAS_ASR
  std::unique_ptr<voiceedge::OfflineASR> asr;
#endif
  std::unique_ptr<voiceedge::RecordingStore> recording_store;
  bool segment_active{false};
  std::uint64_t next_segment_id{0};
  std::uint64_t segment_start_ms{0};
  std::vector<std::int16_t> segment_samples;
  std::atomic<bool> stop_requested{false};
  std::atomic<bool> capture_finished{false};
  Clock::time_point started{Clock::now()};
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

void print_usage(const char* program) {
  std::cout
      << "Usage: " << program << " [options]\n"
      << "  --device <name>              ALSA capture device\n"
      << "                               default: " << kDefaultDevice << "\n"
      << "  --rate <hz>                  capture rate (default: 16000)\n"
      << "  --channels <count>           capture channels (default: 1)\n"
      << "  --period-frames <n>          ALSA period size (default: 2000)\n"
      << "  --buffer-frames <n>          ALSA buffer size (default: 8000)\n"
      << "  --run-ms <ms>                stop after this duration; 0 means Ctrl+C\n"
      << "  --report-ms <ms>             pipeline statistics interval (default: 1000)\n"
      << "  --processing-capacity <n>    processing queue capacity (default: 32)\n"
      << "  --monitor-capacity <n>      monitor queue capacity (default: 64)\n"
      << "  --processing-delay-ms <ms>  test-only processing delay (default: 0)\n"
      << "  --monitor-delay-ms <ms>     test-only monitor delay (default: 0)\n"
      << "  --vad-start-rms <value>    speech-start RMS (default: 0.015)\n"
      << "  --vad-end-rms <value>      speech-end RMS (default: 0.012)\n"
      << "  --vad-min-speech-ms <ms>   minimum speech duration (default: 125)\n"
      << "  --vad-silence-ms <ms>      trailing silence duration (default: 1000)\n"
      << "  --asr-model <path>         optional SenseVoice model.int8.onnx\n"
      << "  --asr-tokens <path>        optional SenseVoice tokens.txt\n"
      << "  --asr-language <name>       ASR language (default: zh)\n"
      << "  --asr-provider <name>       ASR provider (default: cpu)\n"
      << "  --asr-threads <n>           ASR inference threads (default: 2)\n"
      << "  --asr-queue-capacity <n>    ASR segment queue capacity (default: 4)\n"
      << "  --max-segment-ms <ms>       maximum buffered speech segment (default: 30000)\n"
      << "  --persist-dir <path>        local recording/transcript root\n"
      << "  --persist-audio             save speech segments as WAV\n"
      << "  --persist-transcript        save recognition records as JSONL\n"
      << "  --persist-queue-capacity <n> persistence queue capacity (default: 32)\n"
      << "  --help                       show this help\n";
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
    } else if (argument.rfind("--processing-capacity", 0) == 0) {
      options.processing_capacity = static_cast<std::size_t>(
          parse_uint64(option_value(index, argc, argv, "--processing-capacity"), "--processing-capacity"));
      if (options.processing_capacity == 0) {
        throw std::invalid_argument("--processing-capacity must be greater than zero");
      }
    } else if (argument.rfind("--monitor-capacity", 0) == 0) {
      options.monitor_capacity = static_cast<std::size_t>(
          parse_uint64(option_value(index, argc, argv, "--monitor-capacity"), "--monitor-capacity"));
      if (options.monitor_capacity == 0) {
        throw std::invalid_argument("--monitor-capacity must be greater than zero");
      }
    } else if (argument.rfind("--processing-delay-ms", 0) == 0) {
      options.processing_delay_ms = parse_uint64(
          option_value(index, argc, argv, "--processing-delay-ms"), "--processing-delay-ms");
    } else if (argument.rfind("--monitor-delay-ms", 0) == 0) {
      options.monitor_delay_ms = parse_uint64(
          option_value(index, argc, argv, "--monitor-delay-ms"), "--monitor-delay-ms");
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
      options.asr_threads = static_cast<int>(parse_uint64(
          option_value(index, argc, argv, "--asr-threads"), "--asr-threads"));
      if (options.asr_threads <= 0) {
        throw std::invalid_argument("--asr-threads must be greater than zero");
      }
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
    throw std::runtime_error("ALSA accepted parameters different from requested values: actual_rate=" +
                             std::to_string(capture.rate) + " actual_channels=" +
                             std::to_string(capture.channels));
  }
  if (capture.period_frames == 0 || capture.period_frames >
                                      std::numeric_limits<std::size_t>::max() / capture.channels) {
    throw std::runtime_error("ALSA returned an invalid period size");
  }
  check_alsa(snd_pcm_prepare(capture.pcm.get()), "snd_pcm_prepare");
  return capture;
}

void request_stop(PipelineContext& context) {
  const bool was_stopped = context.stop_requested.exchange(true, std::memory_order_relaxed);
  if (!was_stopped) {
    // snd_pcm_drop interrupts a blocking read; the handle remains owned by the
    // main thread and is closed only after the capture worker has joined.
    snd_pcm_drop(context.audio.pcm.get());
  }
  context.processing_queue.close();
  context.monitor_queue.close();
}

void capture_worker(PipelineContext& context) {
  std::vector<std::int16_t> samples(
      static_cast<std::size_t>(context.audio.period_frames) * context.audio.channels);
  std::uint64_t sequence = 0;

  while (!context.stop_requested.load(std::memory_order_relaxed) &&
         !g_signal_stop.load(std::memory_order_relaxed)) {
    const snd_pcm_sframes_t read_frames = snd_pcm_readi(
        context.audio.pcm.get(), samples.data(), context.audio.period_frames);
    if (read_frames < 0) {
      if (context.stop_requested.load(std::memory_order_relaxed) ||
          g_signal_stop.load(std::memory_order_relaxed)) {
        break;
      }
      if (read_frames == -EPIPE) {
        ++context.stats.xruns;
      }
      const int recovered = snd_pcm_recover(context.audio.pcm.get(), static_cast<int>(read_frames), 1);
      if (recovered < 0) {
        ++context.stats.read_errors;
        std::cerr << "pipeline_error stage=capture error="
                  << snd_strerror(static_cast<int>(read_frames))
                  << " recovery=" << snd_strerror(recovered) << '\n';
        break;
      }
      ++context.stats.recoveries;
      continue;
    }
    if (read_frames == 0) {
      continue;
    }

    const std::size_t frame_count = static_cast<std::size_t>(read_frames);
    const std::size_t sample_count = frame_count * context.audio.channels;
    auto frame = std::make_shared<voiceedge::AudioFrame>();
    frame->sequence = sequence++;
    frame->timestamp_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - context.started).count());
    frame->sample_rate = context.audio.rate;
    frame->channels = context.audio.channels;
    frame->samples.assign(samples.begin(), samples.begin() + sample_count);

    context.stats.captured_frames.fetch_add(frame_count, std::memory_order_relaxed);
    if (context.stop_requested.load(std::memory_order_relaxed)) {
      break;
    }
    if (!context.processing_queue.try_push(frame) &&
        !context.stop_requested.load(std::memory_order_relaxed)) {
      ++context.stats.processing_drops;
    }
    if (context.stop_requested.load(std::memory_order_relaxed)) {
      break;
    }
    if (context.monitor_queue.push_drop_oldest(std::move(frame)) &&
        !context.stop_requested.load(std::memory_order_relaxed)) {
      ++context.stats.monitor_drops;
    }
  }

  context.capture_finished.store(true, std::memory_order_relaxed);
  context.processing_queue.close();
  context.monitor_queue.close();
}

void handle_vad_segment(PipelineContext& context, const voiceedge::AudioFrame& frame,
                        const voiceedge::VadDecision& decision) {
#ifdef VOICEEDGE_HAS_ASR
  if (!context.asr) {
    return;
  }

  if (!context.segment_active && decision.state != voiceedge::VadState::Idle) {
    context.segment_active = true;
    context.segment_start_ms = frame.timestamp_ms;
    context.segment_samples.clear();
  }
  if (!context.segment_active) {
    return;
  }

  context.segment_samples.insert(context.segment_samples.end(), frame.samples.begin(), frame.samples.end());
  if (frame.timestamp_ms >= context.segment_start_ms &&
      frame.timestamp_ms - context.segment_start_ms > context.options.max_segment_ms) {
    ++context.stats.asr_overflows;
    context.segment_active = false;
    context.segment_samples.clear();
    context.vad.reset();
    std::cerr << "asr_event type=segment_overflow\n";
    return;
  }

  if (decision.event.has_value() && decision.event->type == voiceedge::VadEventType::Ended) {
    auto segment = std::make_shared<voiceedge::SpeechSegment>();
    segment->id = context.next_segment_id++;
    segment->start_timestamp_ms = context.segment_start_ms;
    segment->end_timestamp_ms = decision.event->timestamp_ms;
    segment->sample_rate = frame.sample_rate;
    segment->channels = frame.channels;
    segment->samples = std::move(context.segment_samples);
    context.segment_samples.clear();
    context.segment_active = false;
    ++context.stats.asr_segments;
    if (!context.asr_queue.try_push(std::move(segment))) {
      ++context.stats.asr_drops;
      std::cerr << "asr_event type=segment_dropped\n";
    }
  } else if (decision.state == voiceedge::VadState::Idle && !decision.event.has_value()) {
    // A candidate that never reached min_speech_ms was noise, not an ASR segment.
    context.segment_active = false;
    context.segment_samples.clear();
  }
#else
  (void)context;
  (void)frame;
  (void)decision;
#endif
}

#ifdef VOICEEDGE_HAS_ASR
void asr_worker(PipelineContext& context) {
  voiceedge::SpeechSegmentPtr segment;
  while (true) {
    if (!context.asr_queue.pop_for(segment, std::chrono::milliseconds(100))) {
      if (context.asr_queue.closed()) {
        break;
      }
      continue;
    }
    try {
      std::cout << "asr_event type=asr_started segment_id=" << segment->id
                << " start_timestamp_ms=" << segment->start_timestamp_ms
                << " end_timestamp_ms=" << segment->end_timestamp_ms << '\n';
      const voiceedge::AsrResult result = context.asr->transcribe(*segment);
      context.stats.asr_processing_ms.store(result.processing_ms, std::memory_order_relaxed);
      const double audio_ms = segment->channels == 0 || segment->sample_rate == 0
                                  ? 0.0
                                  : static_cast<double>(segment->samples.size()) * 1000.0 /
                                        (static_cast<double>(segment->channels) * segment->sample_rate);
      const double rtf = audio_ms > 0.0 ? static_cast<double>(result.processing_ms) / audio_ms : 0.0;
      std::cout << "asr_final segment_id=" << segment->id
                << " text=\"" << result.text << "\""
                << " processing_ms=" << result.processing_ms
                << " audio_ms=" << std::fixed << std::setprecision(1) << audio_ms
                << " rtf=" << std::setprecision(4) << rtf << '\n';
      if (context.recording_store) {
        if (!context.recording_store->enqueue(
                voiceedge::RecordingRecord{segment, result.text, "final", result.processing_ms, rtf, ""})) {
          ++context.stats.persist_drops;
        }
      }
    } catch (const std::exception& error) {
      ++context.stats.asr_failures;
      std::cerr << "asr_event type=asr_failed segment_id=" << segment->id
                << " error=" << error.what() << '\n';
      if (context.recording_store) {
        if (!context.recording_store->enqueue(
                voiceedge::RecordingRecord{segment, "", "failed", 0, 0.0, error.what()})) {
          ++context.stats.persist_drops;
        }
      }
    }
  }
}
#endif

void processing_worker(PipelineContext& context) {
  try {
    AudioFramePtr frame;
    while (true) {
      if (!context.processing_queue.pop_for(frame, std::chrono::milliseconds(100))) {
        if (context.processing_queue.closed()) {
          break;
        }
        continue;
      }
      ++context.stats.processing_popped;
      if (context.options.processing_delay_ms != 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(context.options.processing_delay_ms));
      }

      long double sum_squared = 0.0L;
      int peak = 0;
      for (const std::int16_t sample : frame->samples) {
        const int value = static_cast<int>(sample);
        const int magnitude = value < 0 ? -value : value;
        peak = std::max(peak, magnitude);
        sum_squared += static_cast<long double>(value) * static_cast<long double>(value);
      }
      const double rms = frame->samples.empty()
                             ? 0.0
                             : std::sqrt(static_cast<double>(sum_squared / frame->samples.size()));
      context.stats.last_rms_dbfs_x10.store(
          static_cast<std::int64_t>(std::llround(dbfs(rms) * 10.0)), std::memory_order_relaxed);
      context.stats.last_peak_dbfs_x10.store(
          static_cast<std::int64_t>(std::llround(dbfs(static_cast<double>(peak)) * 10.0)),
          std::memory_order_relaxed);
      const auto vad_decision = context.vad.process(*frame);
      context.stats.vad_state.store(static_cast<int>(vad_decision.state), std::memory_order_relaxed);
      if (vad_decision.event.has_value()) {
        if (vad_decision.event->type == voiceedge::VadEventType::Started) {
          ++context.stats.vad_started;
        } else {
          ++context.stats.vad_ended;
        }
        std::cout << "vad_event type=" << voiceedge::vad_event_name(vad_decision.event->type)
                  << " timestamp_ms=" << vad_decision.event->timestamp_ms
                  << " duration_ms=" << vad_decision.event->duration_ms
                  << " rms=" << std::fixed << std::setprecision(5) << vad_decision.rms << '\n';
      }
      handle_vad_segment(context, *frame, vad_decision);
      context.stats.processed_frames.fetch_add(
          frame->channels == 0 ? 0 : frame->samples.size() / frame->channels, std::memory_order_relaxed);
    }
  } catch (const std::exception& error) {
    ++context.stats.worker_errors;
    std::cerr << "pipeline_error role=processing error=" << error.what() << '\n';
  }
  context.asr_queue.close();
}

void monitor_worker(PipelineContext& context) {
  try {
    AudioFramePtr frame;
    while (true) {
      if (!context.monitor_queue.pop_for(frame, std::chrono::milliseconds(100))) {
        if (context.monitor_queue.closed()) {
          break;
        }
        continue;
      }
      ++context.stats.monitor_popped;
      if (context.options.monitor_delay_ms != 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(context.options.monitor_delay_ms));
      }
      context.stats.monitor_frames.fetch_add(
          frame->channels == 0 ? 0 : frame->samples.size() / frame->channels, std::memory_order_relaxed);
    }
  } catch (const std::exception& error) {
    ++context.stats.worker_errors;
    std::cerr << "pipeline_error role=monitor error=" << error.what() << '\n';
  }
}

void print_stats(const PipelineContext& context, std::uint64_t elapsed_ms) {
  const auto& stats = context.stats;
  std::cout << "pipeline_stats elapsed_ms=" << elapsed_ms
            << " captured_frames=" << stats.captured_frames.load(std::memory_order_relaxed)
            << " processed_frames=" << stats.processed_frames.load(std::memory_order_relaxed)
            << " monitor_frames=" << stats.monitor_frames.load(std::memory_order_relaxed)
            << " processing_queue=" << context.processing_queue.size()
            << " monitor_queue=" << context.monitor_queue.size()
            << " asr_queue=" << context.asr_queue.size()
            << " processing_drops=" << stats.processing_drops.load(std::memory_order_relaxed)
            << " monitor_drops=" << stats.monitor_drops.load(std::memory_order_relaxed)
            << " xruns=" << stats.xruns.load(std::memory_order_relaxed)
            << " recoveries=" << stats.recoveries.load(std::memory_order_relaxed)
            << " read_errors=" << stats.read_errors.load(std::memory_order_relaxed)
            << " vad_state=" << voiceedge::vad_state_name(static_cast<voiceedge::VadState>(
                   stats.vad_state.load(std::memory_order_relaxed)))
            << " vad_started=" << stats.vad_started.load(std::memory_order_relaxed)
            << " vad_ended=" << stats.vad_ended.load(std::memory_order_relaxed)
            << " processing_popped=" << stats.processing_popped.load(std::memory_order_relaxed)
            << " monitor_popped=" << stats.monitor_popped.load(std::memory_order_relaxed)
            << " worker_errors=" << stats.worker_errors.load(std::memory_order_relaxed)
            << " asr_segments=" << stats.asr_segments.load(std::memory_order_relaxed)
            << " asr_drops=" << stats.asr_drops.load(std::memory_order_relaxed)
            << " asr_failures=" << stats.asr_failures.load(std::memory_order_relaxed)
            << " asr_overflows=" << stats.asr_overflows.load(std::memory_order_relaxed)
            << " asr_processing_ms=" << stats.asr_processing_ms.load(std::memory_order_relaxed)
            << " persist_drops=" << stats.persist_drops.load(std::memory_order_relaxed)
            << " persist_failures=" << stats.persist_failures.load(std::memory_order_relaxed)
            << " last_rms_dbfs=" << std::fixed << std::setprecision(1)
            << static_cast<double>(stats.last_rms_dbfs_x10.load(std::memory_order_relaxed)) / 10.0
            << " last_peak_dbfs="
            << static_cast<double>(stats.last_peak_dbfs_x10.load(std::memory_order_relaxed)) / 10.0 << '\n';
}

int run_pipeline(const Options& options) {
  AlsaCapture audio = open_capture(options);
  PipelineContext context(options, audio);

#ifdef VOICEEDGE_HAS_ASR
  if (!options.asr_model.empty() || !options.asr_tokens.empty()) {
    if (options.asr_model.empty() || options.asr_tokens.empty()) {
      throw std::invalid_argument("--asr-model and --asr-tokens must be supplied together");
    }
    context.asr = std::make_unique<voiceedge::OfflineASR>();
    context.asr->open(options.asr_model, options.asr_tokens, options.asr_language,
                      options.asr_provider, options.asr_threads);
    std::cout << "asr_enabled model=\"" << options.asr_model
              << "\" provider=" << options.asr_provider
              << " threads=" << options.asr_threads << '\n';
  }
#else
  if (!options.asr_model.empty() || !options.asr_tokens.empty()) {
    throw std::invalid_argument("pipeline_probe was built without ASR support; rebuild with VOICEEDGE_BUILD_ASR=ON");
  }
#endif

  if (options.persist_audio || options.persist_transcript) {
    if (options.persist_dir.empty()) {
      throw std::invalid_argument("--persist-dir is required when persistence is enabled");
    }
    context.recording_store = std::make_unique<voiceedge::RecordingStore>(
        voiceedge::RecordingStoreConfig{options.persist_dir, options.persist_audio,
                                        options.persist_transcript, options.persist_queue_capacity});
    context.recording_store->start();
    std::cout << "persistence_enabled root=\"" << options.persist_dir
              << "\" audio=" << (options.persist_audio ? "true" : "false")
              << " transcript=" << (options.persist_transcript ? "true" : "false") << '\n';
  }

  std::cout << "pipeline_started device=" << options.device
            << " rate=" << audio.rate
            << " channels=" << audio.channels
            << " format=S16_LE"
            << " period_frames=" << audio.period_frames
            << " buffer_frames=" << audio.buffer_frames
            << " processing_capacity=" << options.processing_capacity
            << " monitor_capacity=" << options.monitor_capacity << '\n';

  std::thread processing_thread(processing_worker, std::ref(context));
  std::thread monitor_thread(monitor_worker, std::ref(context));
#ifdef VOICEEDGE_HAS_ASR
  std::thread asr_thread;
  if (context.asr) {
    asr_thread = std::thread(asr_worker, std::ref(context));
  }
#endif
  std::thread capture_thread(capture_worker, std::ref(context));

  auto next_report = context.started + std::chrono::milliseconds(options.report_ms);
  while (!context.stop_requested.load(std::memory_order_relaxed) &&
         !g_signal_stop.load(std::memory_order_relaxed)) {
    const auto now = Clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - context.started).count();
    if (options.run_ms != 0 && static_cast<std::uint64_t>(elapsed) >= options.run_ms) {
      break;
    }
    if (context.stats.read_errors.load(std::memory_order_relaxed) != 0) {
      break;
    }
    if (now >= next_report) {
      print_stats(context, static_cast<std::uint64_t>(elapsed));
      next_report = now + std::chrono::milliseconds(options.report_ms);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  request_stop(context);
  capture_thread.join();
  processing_thread.join();
  monitor_thread.join();
#ifdef VOICEEDGE_HAS_ASR
  if (asr_thread.joinable()) {
    asr_thread.join();
  }
#endif
  if (context.recording_store) {
    context.recording_store->stop();
    context.stats.persist_failures.store(context.recording_store->failed_records(),
                                         std::memory_order_relaxed);
  }

  const auto finished = Clock::now();
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(finished - context.started).count();
  print_stats(context, static_cast<std::uint64_t>(elapsed));
  std::cout << "pipeline_finished elapsed_ms=" << elapsed
            << " captured_frames=" << context.stats.captured_frames.load(std::memory_order_relaxed)
            << " processed_frames=" << context.stats.processed_frames.load(std::memory_order_relaxed)
            << " monitor_frames=" << context.stats.monitor_frames.load(std::memory_order_relaxed)
            << " processing_drops=" << context.stats.processing_drops.load(std::memory_order_relaxed)
            << " monitor_drops=" << context.stats.monitor_drops.load(std::memory_order_relaxed)
            << " asr_segments=" << context.stats.asr_segments.load(std::memory_order_relaxed)
            << " asr_drops=" << context.stats.asr_drops.load(std::memory_order_relaxed)
            << " asr_failures=" << context.stats.asr_failures.load(std::memory_order_relaxed)
            << " asr_overflows=" << context.stats.asr_overflows.load(std::memory_order_relaxed)
            << " persist_drops=" << context.stats.persist_drops.load(std::memory_order_relaxed)
            << " persist_failures=" << context.stats.persist_failures.load(std::memory_order_relaxed)
            << " xruns=" << context.stats.xruns.load(std::memory_order_relaxed)
            << " recoveries=" << context.stats.recoveries.load(std::memory_order_relaxed)
            << " read_errors=" << context.stats.read_errors.load(std::memory_order_relaxed)
            << " stopped_by_signal=" << (g_signal_stop.load(std::memory_order_relaxed) ? "true" : "false")
            << '\n';
  return context.stats.read_errors.load(std::memory_order_relaxed) == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  try {
    const Options options = parse_options(argc, argv);
    return run_pipeline(options);
  } catch (const std::exception& error) {
    std::cerr << "pipeline_probe_error " << error.what() << '\n';
    return 1;
  }
}
