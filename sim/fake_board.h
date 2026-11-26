// A stand-in for a Balboa main board (modelled on the BP2100G1: two-speed
// pump 1, one-speed pump 2, blower, light, Celsius), at the bus level.
//
// It polls its client channels with Clear to Send, hands out channels on
// 0xFE, answers settings requests, acts on toggles and broadcasts status.
// It also polices the bridge: every frame the bridge writes outside the short
// window after a CTS (or New Client CTS, or Existing Client Request) for it
// counts as unsolicited, which on a real bus would be a collision.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "bwa_frame.h"

namespace sim {

struct BoardOptions {
  bool stock_module = false;     // a stock Wi-Fi module answers on 0x0A
  bool wifi_registered = false;  // the board polls 0x0A though nobody answers
  uint32_t reply_window_ms = 8;
};

class FakeBoard {
 public:
  explicit FakeBoard(const BoardOptions& options);

  // Advances to now_ms; frames the board sends go to out (as bytes).
  void tick(uint32_t now_ms, std::vector<uint8_t>& out);
  // Bytes the bridge wrote.
  void receive(const uint8_t* data, size_t size, uint32_t now_ms, std::vector<uint8_t>& out);
  void power_cycle(uint32_t now_ms);

  std::string stats_json() const;

  // Spa state.
  float water = 12.0f, target = 12.0f;
  uint8_t pumps[2] = {0, 0};
  uint8_t blower = 0;
  bool light = false;
  bool high_range = false;
  uint8_t heat_mode = 0;
  uint8_t hour = 4, minute = 8;
  uint8_t filter_cycles[8] = {0, 0, 0, 45, 4, 0, 0, 30};

 private:
  void send(const bwa::Frame& f, std::vector<uint8_t>& out);
  void frame_from_client(const bwa::Frame& f, uint32_t now_ms, std::vector<uint8_t>& out);
  void answer_settings(uint8_t channel, uint8_t code, std::vector<uint8_t>& out);
  void status_frame(bwa::Frame& f) const;
  bool polled(uint8_t channel) const;

  BoardOptions options_;
  bwa::FrameParser parser_;
  uint32_t powered_ms_ = 0;
  uint32_t next_cts_ms_ = 0, next_new_client_ms_ = 0, next_status_ms_ = 0, next_stock_ms_ = 0;
  size_t cts_index_ = 0;
  std::vector<uint8_t> polled_;      // channels that get CTS
  uint8_t next_channel_ = 0x10;
  // The one client that may answer right now.
  enum class Window { kNone, kCts, kNewClient, kExisting } window_ = Window::kNone;
  uint8_t window_channel_ = 0;
  uint32_t window_until_ms_ = 0;
  uint8_t pending_channel_ = 0;      // assigned, not acknowledged yet
  int probe_ = -1;                   // power-on Existing Client Request sweep
  uint32_t next_probe_ms_ = 0;
  uint8_t stock_code_ = 0;

 public:
  // Counters.
  uint32_t frames_from_bridge = 0;
  uint32_t unsolicited = 0;
  uint32_t wrong_channel = 0;
  uint32_t channel_requests = 0;
  uint32_t acks = 0;
  uint32_t nothing_to_send = 0;
  uint32_t toggles = 0;
  uint32_t settings_answered = 0;
  uint32_t set_temperatures = 0;
  uint32_t existing_answers = 0;
  uint32_t cts_sent = 0;
};

}  // namespace sim
