#include "../board/voiceedge_protocol.hpp"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
using Clock = std::chrono::steady_clock;

struct Options {
  std::string host{"127.0.0.1"};
  std::string port{"8765"};
  std::string path{"/voiceedge"};
  std::uint64_t max_packets{0};
};

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

Options parse_options(int argc, char** argv) {
  Options options;
  if (const char* environment_host = std::getenv("VOICEEDGE_WS_HOST");
      environment_host != nullptr && *environment_host != '\0') {
    options.host = environment_host;
  }

  for (int index = 1; index < argc; ++index) {
    const std::string argument(argv[index]);
    if (argument == "--help" || argument == "-h") {
      std::cout
          << "Usage: " << argv[0] << " [options]\n"
          << "  --host <address>       server address (default: 127.0.0.1)\n"
          << "  --port <port>          server port (default: 8765)\n"
          << "  --path <path>          WebSocket path (default: /voiceedge)\n"
          << "  --max-packets <n>      stop after n audio packets; 0 means until close\n";
      std::exit(0);
    }
    if (argument.rfind("--host", 0) == 0) {
      options.host = option_value(index, argc, argv, "--host");
    } else if (argument.rfind("--port", 0) == 0) {
      options.port = option_value(index, argc, argv, "--port");
    } else if (argument.rfind("--path", 0) == 0) {
      options.path = option_value(index, argc, argv, "--path");
      if (options.path.empty() || options.path.front() != '/') {
        throw std::invalid_argument("--path must start with '/'");
      }
    } else if (argument.rfind("--max-packets", 0) == 0) {
      options.max_packets = parse_uint64(option_value(index, argc, argv, "--max-packets"), "--max-packets");
    } else {
      throw std::invalid_argument("unknown option: " + argument);
    }
  }
  return options;
}

std::vector<std::uint8_t> copy_buffer(const beast::flat_buffer& buffer) {
  const std::string bytes = beast::buffers_to_string(buffer.data());
  return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const Options options = parse_options(argc, argv);
    asio::io_context io_context;
    tcp::resolver resolver(io_context);
    const auto endpoints = resolver.resolve(options.host, options.port);
    websocket::stream<tcp::socket> websocket(io_context);
    websocket.set_option(websocket::stream_base::timeout::suggested(
        beast::role_type::client));
    asio::connect(websocket.next_layer(), endpoints);
    websocket.handshake(options.host + ":" + options.port, options.path);
    websocket.text(true);
    const std::string hello = "{\"type\":\"hello\",\"version\":1,\"client\":\"cli\"}";
    websocket.write(asio::buffer(hello));

    std::cout << "ws_client_connected host=" << options.host
              << " port=" << options.port
              << " path=" << options.path << '\n';

    std::uint64_t packets = 0;
    std::uint64_t frames = 0;
    std::uint64_t bytes = 0;
    auto started = Clock::now();
    auto next_report = started + std::chrono::seconds(1);
    beast::flat_buffer buffer;
    while (true) {
      beast::error_code error;
      websocket.read(buffer, error);
      if (error) {
        if (error == websocket::error::closed || error == asio::error::eof ||
            error == asio::error::connection_reset) {
          break;
        }
        throw beast::system_error(error);
      }

      if (websocket.got_text()) {
        std::cout << "ws_text " << beast::buffers_to_string(buffer.data()) << '\n';
      } else {
        const auto packet = copy_buffer(buffer);
        voiceedge::protocol::AudioPacketInfo info;
        if (!voiceedge::protocol::decode_audio_packet(packet, info)) {
          throw std::runtime_error("invalid binary audio packet");
        }
        ++packets;
        frames += info.channels == 0 ? 0 : info.sample_count / info.channels;
        bytes += packet.size();
        const auto now = Clock::now();
        if (now >= next_report) {
          const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - started).count();
          std::cout << "ws_audio elapsed_ms=" << elapsed
                    << " packets=" << packets
                    << " frames=" << frames
                    << " bytes=" << bytes
                    << " last_sequence=" << info.sequence
                    << " sample_rate=" << info.sample_rate
                    << " channels=" << static_cast<unsigned int>(info.channels) << '\n';
          next_report = now + std::chrono::seconds(1);
        }
        if (options.max_packets != 0 && packets >= options.max_packets) {
          beast::error_code close_error;
          websocket.close(websocket::close_code::normal, close_error);
          break;
        }
      }
      buffer.consume(buffer.size());
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
    std::cout << "ws_client_finished elapsed_ms=" << elapsed
              << " packets=" << packets
              << " frames=" << frames
              << " bytes=" << bytes << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "ws_cli_error " << error.what() << '\n';
    return 1;
  }
}
