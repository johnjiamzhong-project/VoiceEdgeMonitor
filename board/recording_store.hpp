#pragma once

#include "audio_frame.hpp"
#include "bounded_queue.hpp"

#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace voiceedge {

struct RecordingStoreConfig {
  std::filesystem::path root;
  bool save_audio{false};
  bool save_transcript{false};
  std::size_t queue_capacity{32};
};

struct RecordingRecord {
  SpeechSegmentPtr segment;
  std::string text;
  std::string status{"final"};
  std::uint64_t processing_ms{0};
  double rtf{0.0};
  std::string error;
};

class RecordingStore {
 public:
  explicit RecordingStore(RecordingStoreConfig config)
      : config_(std::move(config)), queue_(config_.queue_capacity) {}

  RecordingStore(const RecordingStore&) = delete;
  RecordingStore& operator=(const RecordingStore&) = delete;

  ~RecordingStore() { stop(); }

  [[nodiscard]] bool enabled() const {
    return !config_.root.empty() && (config_.save_audio || config_.save_transcript);
  }

  void start() {
    if (!enabled() || worker_.joinable()) {
      return;
    }
    std::filesystem::create_directories(config_.root);
    if (config_.save_transcript) {
      transcript_.open(config_.root / "transcripts.jsonl", std::ios::app);
      if (!transcript_) {
        throw std::runtime_error("cannot open transcript store");
      }
    }
    worker_ = std::thread([this] { worker_loop(); });
  }

  bool enqueue(RecordingRecord record) {
    if (!enabled()) {
      return true;
    }
    if (!worker_.joinable()) {
      throw std::logic_error("recording store must be started before enqueue");
    }
    auto item = std::make_shared<RecordingRecord>(std::move(record));
    if (!queue_.try_push(std::move(item))) {
      ++dropped_records_;
      return false;
    }
    return true;
  }

  void stop() {
    if (!worker_.joinable()) {
      return;
    }
    queue_.close();
    worker_.join();
    if (transcript_.is_open()) {
      transcript_.flush();
      transcript_.close();
    }
  }

  [[nodiscard]] std::uint64_t dropped_records() const { return dropped_records_; }
  [[nodiscard]] std::uint64_t failed_records() const { return failed_records_; }

 private:
  static std::string json_escape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char character : value) {
      switch (character) {
        case '\\': escaped += "\\\\"; break;
        case '"': escaped += "\\\""; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default: escaped.push_back(character); break;
      }
    }
    return escaped;
  }

  static void write_u16(std::ofstream& output, std::uint16_t value) {
    const char bytes[2] = {static_cast<char>(value & 0xffU),
                           static_cast<char>((value >> 8U) & 0xffU)};
    output.write(bytes, sizeof(bytes));
  }

  static void write_u32(std::ofstream& output, std::uint32_t value) {
    const char bytes[4] = {static_cast<char>(value & 0xffU),
                           static_cast<char>((value >> 8U) & 0xffU),
                           static_cast<char>((value >> 16U) & 0xffU),
                           static_cast<char>((value >> 24U) & 0xffU)};
    output.write(bytes, sizeof(bytes));
  }

  static void write_wav(const std::filesystem::path& path, const SpeechSegment& segment) {
    if (segment.sample_rate == 0 || segment.channels == 0 || segment.samples.empty()) {
      throw std::invalid_argument("cannot save empty audio segment");
    }
    const std::uint64_t data_bytes =
        static_cast<std::uint64_t>(segment.samples.size()) * sizeof(std::int16_t);
    if (data_bytes > std::numeric_limits<std::uint32_t>::max()) {
      throw std::runtime_error("audio segment exceeds WAV RIFF size limit");
    }
    const std::filesystem::path temporary = path.string() + ".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
      throw std::runtime_error("cannot open WAV output");
    }
    const std::uint32_t byte_rate = segment.sample_rate * segment.channels * 2U;
    const std::uint16_t block_align = static_cast<std::uint16_t>(segment.channels * 2U);
    output.write("RIFF", 4);
    write_u32(output, static_cast<std::uint32_t>(36U + data_bytes));
    output.write("WAVEfmt ", 8);
    write_u32(output, 16);
    write_u16(output, 1);
    write_u16(output, static_cast<std::uint16_t>(segment.channels));
    write_u32(output, segment.sample_rate);
    write_u32(output, byte_rate);
    write_u16(output, block_align);
    write_u16(output, 16);
    output.write("data", 4);
    write_u32(output, static_cast<std::uint32_t>(data_bytes));
    output.write(reinterpret_cast<const char*>(segment.samples.data()),
                 static_cast<std::streamsize>(data_bytes));
    output.close();
    if (!output) {
      std::filesystem::remove(temporary);
      throw std::runtime_error("failed writing WAV output");
    }
    std::filesystem::rename(temporary, path);
  }

  static std::string date_directory() {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    std::ostringstream value;
    value << std::put_time(&local, "%Y-%m-%d");
    return value.str();
  }

  void persist(const RecordingRecord& record) {
    if (!record.segment) {
      throw std::invalid_argument("recording record has no segment");
    }
    std::string audio_path;
    if (config_.save_audio) {
      const std::filesystem::path directory = config_.root / "recordings" / date_directory();
      std::filesystem::create_directories(directory);
      std::ostringstream filename;
      filename << "segment-" << std::setw(8) << std::setfill('0') << record.segment->id << ".wav";
      const auto path = directory / filename.str();
      write_wav(path, *record.segment);
      audio_path = std::filesystem::relative(path, config_.root).generic_string();
    }
    if (config_.save_transcript) {
      const double audio_ms = record.segment->channels == 0 || record.segment->sample_rate == 0
                                  ? 0.0
                                  : static_cast<double>(record.segment->samples.size()) * 1000.0 /
                                        (record.segment->channels * record.segment->sample_rate);
      transcript_ << "{\"segment_id\":" << record.segment->id
                  << ",\"start_timestamp_ms\":" << record.segment->start_timestamp_ms
                  << ",\"end_timestamp_ms\":" << record.segment->end_timestamp_ms
                  << ",\"audio_path\":\"" << json_escape(audio_path) << "\""
                  << ",\"text\":\"" << json_escape(record.text) << "\""
                  << ",\"status\":\"" << json_escape(record.status) << "\""
                  << ",\"processing_ms\":" << record.processing_ms
                  << ",\"audio_ms\":" << std::fixed << std::setprecision(1) << audio_ms
                  << ",\"rtf\":" << std::setprecision(4) << record.rtf
                  << ",\"error\":\"" << json_escape(record.error) << "\"}\n";
      transcript_.flush();
    }
  }

  void worker_loop() {
    std::shared_ptr<RecordingRecord> record;
    while (true) {
      if (!queue_.pop_for(record, std::chrono::milliseconds(100))) {
        if (queue_.closed()) {
          break;
        }
        continue;
      }
      try {
        persist(*record);
      } catch (const std::exception& error) {
        ++failed_records_;
        // Persistence must not take down capture, VAD, ASR or networking.
        (void)error;
      }
    }
  }

  const RecordingStoreConfig config_;
  BoundedQueue<std::shared_ptr<RecordingRecord>> queue_;
  std::ofstream transcript_;
  std::thread worker_;
  std::uint64_t dropped_records_{0};
  std::uint64_t failed_records_{0};
};

}  // namespace voiceedge
