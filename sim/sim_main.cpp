// The bridge on a desktop, against FakeBoard instead of a spa.
//
// The same BusClient, Hub and NetServer as on the ESP32, in the same two
// tasks (a bus thread and a network thread) with the same rings between
// them. pybalboa, the Balboa integration or the Balboa app can connect to
// the TCP port it prints.
//
//   bridge_sim [--mode listen_only|assigned|fixed_wifi] [--mac 00:15:27:aa:bb:cc]
//              [--stock-module] [--wifi-registered] [--echo] [--tcp-port N]
//              [--udp-port N] [--status-interval MS]
//
// Prints "READY <tcp port> <udp port>", then reads commands on stdin:
//   stats          one line of JSON (board, bus, hub, net)
//   powercycle     the board forgets every channel and starts over
//   reboot         the bridge restarts, keeping its channel (RTC memory)
//   mode <name>    reconfigure the bus client
//   quit
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <random>
#include <string>
#include <thread>

#include "bwa_bus.h"
#include "bwa_hub.h"
#include "bwa_net.h"
#include "fake_board.h"

namespace {

using Clock = std::chrono::steady_clock;
const Clock::time_point kStart = Clock::now();

uint32_t now_ms() {
  return static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - kStart).count());
}

struct Options {
  bwa::BusMode mode = bwa::BusMode::kAssigned;
  uint8_t mac[6] = {0x00, 0x15, 0x27, 0xAA, 0xBB, 0xCC};
  sim::BoardOptions board;
  bool echo = false;
  uint16_t tcp_port = 0, udp_port = 0;
  uint32_t status_interval_ms = 1000;
};

bool parse_mac(const char* text, uint8_t out[6]) {
  unsigned v[6];
  if (std::sscanf(text, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
    return false;
  for (int i = 0; i < 6; ++i) out[i] = static_cast<uint8_t>(v[i]);
  return true;
}

class SimBus : public bwa::BusIo {
 public:
  SimBus(sim::FakeBoard& board, bwa::UpRing& up, bool echo) : board_(board), up_(up), echo_(echo) {}

  void transmit(const bwa::Frame& frame) override {
    board_.receive(frame.bytes, frame.size(), now_, to_bridge);
    if (echo_) to_bridge.insert(to_bridge.end(), frame.bytes, frame.bytes + frame.size());
  }
  void upstream(const bwa::Frame& frame, bool cache_only) override {
    bwa::UpFrame up;
    up.frame = frame;
    up.cache_only = cache_only;
    up_.push(up);
  }
  void channel_changed(uint8_t channel) override { rtc_channel = channel; }

  std::vector<uint8_t> to_bridge;
  uint8_t rtc_channel = 0;
  uint32_t now_ = 0;

 private:
  sim::FakeBoard& board_;
  bwa::UpRing& up_;
  bool echo_;
};

}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGPIPE, SIG_IGN);
  Options opt;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--mode") {
      if (!bwa::parse_mode(next(), opt.mode)) return 2;
    } else if (a == "--mac") {
      if (!parse_mac(next(), opt.mac)) return 2;
    } else if (a == "--stock-module") {
      opt.board.stock_module = true;
    } else if (a == "--wifi-registered") {
      opt.board.wifi_registered = true;
    } else if (a == "--echo") {
      opt.echo = true;
    } else if (a == "--tcp-port") {
      opt.tcp_port = static_cast<uint16_t>(std::atoi(next()));
    } else if (a == "--udp-port") {
      opt.udp_port = static_cast<uint16_t>(std::atoi(next()));
    } else if (a == "--status-interval") {
      opt.status_interval_ms = static_cast<uint32_t>(std::atoi(next()));
    } else {
      std::fprintf(stderr, "unknown option %s\n", a.c_str());
      return 2;
    }
  }

  sim::FakeBoard board(opt.board);
  bwa::UpRing up;
  bwa::BusClient::DownRing down;
  SimBus io(board, up, opt.echo);
  bwa::BusClient bus(io, down);
  bwa::BusConfig bus_config;
  bus_config.mode = opt.mode;
  bus_config.client_hash = static_cast<uint16_t>((opt.mac[4] << 8) | opt.mac[5]);
  bus.configure(bus_config);

  bwa::Hub hub;
  bwa::HubConfig hub_config;
  std::memcpy(hub_config.mac, opt.mac, 6);
  hub_config.status_interval_ms = opt.status_interval_ms;
  hub.configure(hub_config);
  bwa::NetServer net(hub, down, up);
  if (!net.start(opt.tcp_port, opt.udp_port, true)) {
    std::fprintf(stderr, "cannot open ports\n");
    return 1;
  }

  std::mutex bus_mutex;  // guards board, bus and io against the command thread
  std::atomic<bool> running{true};

  std::thread bus_thread([&] {
    std::mt19937 rng(1234);
    bwa::FrameParser parser;
    std::vector<uint8_t> wire;
    while (running) {
      {
        std::lock_guard<std::mutex> lock(bus_mutex);
        const uint32_t now = now_ms();
        io.now_ = now;
        wire.clear();
        board.tick(now, wire);
        wire.insert(wire.end(), io.to_bridge.begin(), io.to_bridge.end());
        io.to_bridge.clear();
        // Hand the bytes over in odd-sized pieces, as a UART would.
        size_t at = 0;
        while (at < wire.size()) {
          const size_t n = std::min<size_t>(wire.size() - at, 1 + rng() % 17);
          parser.feed(wire.data() + at, n, [&](const bwa::Frame& f) { bus.on_frame(f, now); });
          at += n;
          // Whatever the bridge wrote in answer (and its echo) comes next.
          if (!io.to_bridge.empty()) {
            wire.insert(wire.end(), io.to_bridge.begin(), io.to_bridge.end());
            io.to_bridge.clear();
          }
        }
        bus.tick(now);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });

  std::thread net_thread([&] {
    while (running) net.poll(5, bus.online(), now_ms());
  });

  std::printf("READY %u %u\n", net.tcp_port(), net.udp_port());
  std::fflush(stdout);

  std::string line;
  while (running && std::getline(std::cin, line)) {
    if (line == "quit") break;
    if (line == "stats") {
      std::string board_json;
      uint8_t rtc = 0;
      {
        std::lock_guard<std::mutex> lock(bus_mutex);
        board_json = board.stats_json();
        rtc = io.rtc_channel;
      }
      const bwa::BusStats& b = bus.stats();
      const bwa::HubStats& h = hub.stats();
      const bwa::Hub::Snapshot snap = hub.snapshot();
      std::printf(
          "{\"board\":%s,\"bus\":{\"state\":\"%s\",\"channel\":%u,\"rtc_channel\":%u,"
          "\"frames\":%u,\"cts_ours\":%u,\"sent\":%u,\"nothing_to_send\":%u,\"echoes\":%u,"
          "\"dropped_listen\":%u,\"channel_requests\":%u,\"channel_losses\":%u,"
          "\"wifi_channel_cts\":%u,\"wifi_channel_answers\":%u},"
          "\"hub\":{\"client_frames\":%u,\"module_answers\":%u,\"to_bus\":%u,"
          "\"bus_queue_full\":%u,\"cache_answers\":%u,\"cache_misses\":%u,"
          "\"dropped_listen\":%u,\"dropped_type\":%u,\"pushed\":%u,\"cached\":%u,"
          "\"model\":\"%s\",\"water\":%.1f},"
          "\"net\":{\"clients\":%d,\"refused\":%u,\"discoveries\":%u}}\n",
          board_json.c_str(), bwa::state_name(bus.state()), bus.channel(), rtc,
          b.frames.load(), b.cts_ours.load(), b.sent.load(), b.nothing_to_send.load(),
          b.echoes.load(), b.dropped_listen.load(), b.channel_requests.load(),
          b.channel_losses.load(), b.wifi_channel_cts.load(), b.wifi_channel_answers.load(),
          h.client_frames.load(), h.module_answers.load(), h.to_bus.load(),
          h.bus_queue_full.load(), h.cache_answers.load(), h.cache_misses.load(),
          h.dropped_listen.load(), h.dropped_type.load(), h.pushed.load(), snap.cached,
          snap.info.model, snap.status.water, net.clients(), net.refused(), net.discoveries());
    } else if (line == "powercycle") {
      std::lock_guard<std::mutex> lock(bus_mutex);
      board.power_cycle(now_ms());
      std::printf("OK\n");
    } else if (line == "reboot") {
      std::lock_guard<std::mutex> lock(bus_mutex);
      bwa::BusConfig c = bus_config;
      c.previous_channel = io.rtc_channel;
      bus.configure(c);
      std::printf("OK\n");
    } else if (line.rfind("mode ", 0) == 0) {
      bwa::BusMode m;
      if (!bwa::parse_mode(line.c_str() + 5, m)) {
        std::printf("ERR\n");
      } else {
        std::lock_guard<std::mutex> lock(bus_mutex);
        bus_config.mode = m;
        bus.configure(bus_config);
        std::printf("OK\n");
      }
    } else {
      std::printf("ERR\n");
    }
    std::fflush(stdout);
  }
  running = false;
  bus_thread.join();
  net_thread.join();
  net.stop();
  return 0;
}
