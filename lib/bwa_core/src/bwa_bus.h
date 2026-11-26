// The bridge's own place on the spa's RS-485 bus.
//
// The main board owns the bus. It sends Clear to Send (CTS) to one client
// channel at a time, and a client may write exactly one frame right after a
// CTS for its own channel (or "nothing to send"). New clients get a channel by
// answering a New Client CTS on 0xFE. See docs/protocol.md.
//
// BusClient is the state machine for that, with no I/O of its own: the bus
// task feeds it every valid frame and it answers through BusIo at once, from
// the same call, so a CTS is answered within the time it takes to parse it.
//
// Modes:
//   listen_only  never writes a byte. The safe first step on a new spa.
//   assigned     asks the board for a channel like a top-side panel does and
//                answers CTS on it. Works beside the stock Wi-Fi module.
//   fixed_wifi   takes 0x0A, the stock Wi-Fi module's channel. Only with the
//                stock module unplugged.
#pragma once

#include <atomic>
#include <cstdint>

#include "bwa_frame.h"
#include "bwa_ring.h"

namespace bwa {

enum class BusMode : uint8_t { kListenOnly = 0, kAssigned = 1, kFixedWifi = 2 };

enum class BusState : uint8_t {
  kSilent = 0,        // no valid frame seen yet
  kListening = 1,     // listen-only (by mode, or because writing is blocked)
  kObserving = 2,     // after a reboot: is the board still polling our old channel?
  kRequesting = 3,    // waiting for a New Client CTS to ask for a channel
  kAwaitingChannel = 4,
  kOnline = 5,        // have a channel, answering CTS
  kGaveUp = 6,        // the board would not give a usable channel; listening only
};

const char* mode_name(BusMode mode);
const char* state_name(BusState state);
bool parse_mode(const char* text, BusMode& out);

struct BusConfig {
  BusMode mode = BusMode::kListenOnly;
  // Channel held before a software restart (0 = none). The board keeps
  // polling a channel until it is power-cycled, so taking the old one back
  // saves the board from handing out a new one on every reboot.
  uint8_t previous_channel = 0;
  uint16_t client_hash = 0x5A3C;   // bytes 1-2 of our channel request
  uint8_t device_type = 0x02;      // byte 0; top-side panels send 0x02
  uint8_t existing_response[3] = {0x04, 0x37, 0x00};
  // False blocks every write, e.g. while the receiver has to run inverted
  // (A and B swapped): an inverted transmitter would hold the bus.
  bool tx_allowed = true;
};

// What the bus client needs from the platform.
class BusIo {
 public:
  virtual ~BusIo() = default;
  virtual void transmit(const Frame& frame) = 0;
  // A frame for the TCP side. cache_only: keep it as a known answer but do
  // not push it to the clients (it answered somebody else's request).
  virtual void upstream(const Frame& frame, bool cache_only) = 0;
  // The channel we hold changed (0 = none). Keep it across a restart.
  virtual void channel_changed(uint8_t channel) = 0;
};

// Counters the web page and the simulator read from other tasks.
struct BusStats {
  std::atomic<uint32_t> frames{0};
  std::atomic<uint32_t> cts_seen{0};        // CTS to any channel
  std::atomic<uint32_t> cts_ours{0};
  std::atomic<uint32_t> sent{0};            // frames written for TCP clients
  std::atomic<uint32_t> nothing_to_send{0};
  std::atomic<uint32_t> echoes{0};          // our own writes read back
  std::atomic<uint32_t> dropped_listen{0};  // client writes not sent (listen-only)
  std::atomic<uint32_t> channel_requests{0};
  std::atomic<uint32_t> channel_losses{0};
  std::atomic<uint32_t> wifi_channel_cts{0};    // CTS to 0x0A (is a stock module expected?)
  std::atomic<uint32_t> wifi_channel_answers{0};  // frames from a client on 0x0A
  std::atomic<uint32_t> last_frame_ms{0};
};

class BusClient {
 public:
  using DownRing = SpscRing<Frame, 16>;

  BusClient(BusIo& io, DownRing& down) : io_(io), down_(down) {}

  // Starts over with a new configuration (the channel held is given up).
  void configure(const BusConfig& config);

  // Every valid frame read from the bus, in order.
  void on_frame(const Frame& frame, uint32_t now_ms);
  // Housekeeping; call at least every 100 ms.
  void tick(uint32_t now_ms);

  BusState state() const { return state_.load(); }
  uint8_t channel() const { return channel_.load(); }
  BusMode mode() const { return config_.mode; }
  bool online() const { return state() == BusState::kOnline; }
  const BusStats& stats() const { return stats_; }

  // Limits that keep a misbehaving bridge from using up the board's channels.
  static constexpr uint32_t kObserveMs = 2500;
  static constexpr uint32_t kPreviousChannelCts = 3;
  static constexpr uint32_t kAssignTimeoutMs = 1500;
  static constexpr uint32_t kLostAfterMs = 4000;
  static constexpr uint32_t kMaxChannelRequests = 6;  // per configure()
  static constexpr uint32_t kEchoWindowMs = 30;

 private:
  void enter_start_state(uint32_t now_ms);
  void set_state(BusState s) { state_.store(s); }
  void set_channel_held(uint8_t ch);
  void send(const Frame& frame, uint32_t now_ms);
  void answer_cts(uint32_t now_ms);
  void request_channel(uint32_t now_ms);
  void forward_for_us(const Frame& frame);
  void drain_unsendable();

  BusIo& io_;
  DownRing& down_;
  BusConfig config_;
  std::atomic<BusState> state_{BusState::kSilent};
  std::atomic<uint8_t> channel_{0};
  BusStats stats_;

  uint32_t state_since_ms_ = 0;
  uint32_t previous_cts_ = 0;
  uint32_t last_cts_ours_ms_ = 0;
  uint32_t requests_ = 0;
  uint16_t hash_ = 0;
  Frame last_tx_;
  uint32_t last_tx_ms_ = 0;
  bool expect_echo_ = false;
};

// Types only clients send; a frame of one of these on a client channel is a
// client talking, never the board answering.
bool client_originated(uint8_t type);
// Answers the board gives to a Settings Request (or a filter-cycle write).
bool board_answer(uint8_t type);

}  // namespace bwa
