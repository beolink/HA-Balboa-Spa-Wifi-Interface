// The TCP side of the bridge: what the stock BWA Wi-Fi module does for the
// Balboa app and pybalboa on port 4257, for several clients at once.
//
//  * frames from the bus on the broadcast channel, and the board's answers to
//    our channel, go to every client, as channel 0x0A frames
//  * a client's Existing Client Request (pybalboa's "device present") is
//    answered here with a Module Identification (0x94) carrying our MAC,
//    exactly as the stock module answers it itself
//  * toggles, temperature, time, settings requests, filter cycles and
//    preferences go to the bus; other types are dropped (installer
//    functions such as Change Setup, and the module's own Wi-Fi setup)
//  * while the bridge is not on the bus (listen-only), a settings request
//    is answered from the answers heard on the bus, when there is one
//
// Transport-agnostic; NetServer (bwa_net) moves the bytes.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

#include "bwa_frame.h"
#include "bwa_ring.h"
#include "bwa_status.h"

namespace bwa {

struct UpFrame {
  Frame frame;
  bool cache_only = false;
};
using UpRing = SpscRing<UpFrame, 64>;

class HubIo {
 public:
  virtual ~HubIo() = default;
  virtual void send_to_client(int slot, const uint8_t* data, size_t size) = 0;
  // Queues a frame for the bus; false when the queue is full.
  virtual bool to_bus(const Frame& frame) = 0;
};

struct HubConfig {
  uint8_t mac[6] = {0x00, 0x15, 0x27, 0x00, 0x00, 0x01};
  char hostname[16] = "BWGSPA";
  // A status that has not changed is passed on at most this often (the
  // board sends ~3 a second). 0 passes on every one.
  uint32_t status_interval_ms = 1000;
};

struct HubStats {
  std::atomic<uint32_t> client_frames{0};
  std::atomic<uint32_t> module_answers{0};
  std::atomic<uint32_t> to_bus{0};
  std::atomic<uint32_t> bus_queue_full{0};
  std::atomic<uint32_t> cache_answers{0};
  std::atomic<uint32_t> cache_misses{0};
  std::atomic<uint32_t> dropped_listen{0};
  std::atomic<uint32_t> dropped_type{0};
  std::atomic<uint32_t> pushed{0};
  std::atomic<uint32_t> connects{0};
};

class Hub {
 public:
  static constexpr int kMaxClients = 3;

  Hub() = default;
  // The transport (NetServer). Call once before anything else.
  void attach(HubIo* io) { io_ = io; }

  void configure(const HubConfig& config);
  const HubConfig& config() const { return config_; }

  void client_opened(int slot);
  void client_closed(int slot);
  // Bytes from a client. bus_online: the bridge holds a channel and can write.
  void client_bytes(int slot, const uint8_t* data, size_t size, bool bus_online, uint32_t now_ms);
  // A frame from the bus task.
  void bus_frame(const Frame& frame, bool cache_only, uint32_t now_ms);

  void module_identification(Frame& out) const;
  // The answer to a UDP discovery packet: "BWGSPA\r\n00-15-27-XX-XX-XX\r\n".
  size_t discovery_reply(char* buf, size_t cap) const;

  struct Snapshot {
    SpaStatus status;
    SpaInfo info;
    uint32_t status_ms = 0;     // when the last status came (0 = never)
    int clients = 0;
    uint8_t cached = 0;         // bit per cached answer type
  };
  Snapshot snapshot() const;
  const HubStats& stats() const { return stats_; }

  // Settings Request code (payload byte 0) -> the type the board answers with.
  static uint8_t answer_type_for(uint8_t settings_code);

 private:
  void client_frame(int slot, const Frame& frame, bool bus_online);
  void send_frame(int slot, const Frame& frame);
  void push_all(const Frame& frame);
  static int cache_index(uint8_t type);

  static constexpr int kCacheTypes = 7;

  HubIo* io_ = nullptr;
  HubConfig config_;
  HubStats stats_;

  struct Client {
    std::atomic<bool> open{false};  // read by snapshot() from another task
    FrameParser parser;
  } clients_[kMaxClients];

  mutable std::mutex mutex_;  // guards what snapshot() reads
  Frame last_status_;
  bool have_status_ = false;
  uint32_t last_status_ms_ = 0;
  uint32_t last_push_ms_ = 0;
  SpaStatus status_;
  SpaInfo info_;
  Frame cache_[kCacheTypes];
  bool cached_[kCacheTypes] = {};
};

}  // namespace bwa
