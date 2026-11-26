#include "fake_board.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

using bwa::Frame;
using bwa::make_frame;
namespace type = bwa::type;

namespace sim {

namespace {
constexpr uint32_t kCtsEveryMs = 16;
constexpr uint32_t kStatusEveryMs = 300;
constexpr uint8_t kModel[] = {'B', 'P', '2', '1', '0', '0', 'G', '1'};
}  // namespace

FakeBoard::FakeBoard(const BoardOptions& options) : options_(options) { power_cycle(0); }

void FakeBoard::power_cycle(uint32_t now_ms) {
  powered_ms_ = now_ms;
  polled_.clear();
  next_channel_ = 0x10;
  pending_channel_ = 0;
  window_ = Window::kNone;
  next_cts_ms_ = now_ms + 50;
  next_new_client_ms_ = now_ms + 30;
  next_status_ms_ = now_ms + 20;
  next_stock_ms_ = now_ms + 200;
  probe_ = 0;
  next_probe_ms_ = now_ms;
  parser_.reset();
  if (options_.wifi_registered && !options_.stock_module) polled_.push_back(bwa::kChannelWifi);
}

bool FakeBoard::polled(uint8_t channel) const {
  return std::find(polled_.begin(), polled_.end(), channel) != polled_.end();
}

void FakeBoard::send(const Frame& f, std::vector<uint8_t>& out) {
  out.insert(out.end(), f.bytes, f.bytes + f.size());
}

void FakeBoard::status_frame(Frame& f) const {
  uint8_t p[24] = {};
  p[2] = static_cast<uint8_t>(std::lround(water * 2));
  p[3] = hour;
  p[4] = minute;
  p[5] = heat_mode;
  p[9] = 0x01 | 0x02;
  p[10] = (high_range ? 0x04 : 0x00);
  p[11] = static_cast<uint8_t>(pumps[0] | (pumps[1] << 2));
  p[13] = static_cast<uint8_t>(blower << 2);
  p[14] = light ? 0x03 : 0x00;
  p[20] = static_cast<uint8_t>(std::lround(target * 2));
  make_frame(f, bwa::kChannelBroadcast, type::kStatus, p, sizeof(p));
}

void FakeBoard::tick(uint32_t now, std::vector<uint8_t>& out) {
  if (window_ != Window::kNone) {
    if (static_cast<int32_t>(now - window_until_ms_) < 0) return;  // waiting for an answer
    window_ = Window::kNone;
    pending_channel_ = 0;
  }
  Frame f;

  // Power-on sweep: is anyone still there on these channels?
  if (probe_ >= 0) {
    if (static_cast<int32_t>(now - next_probe_ms_) < 0) return;
    static const uint8_t first[] = {0x0A, 0x0C};
    uint8_t ch;
    if (probe_ < 2) ch = first[probe_];
    else ch = static_cast<uint8_t>(0x10 + probe_ - 2);
    if (ch > 0x3F) {
      probe_ = -1;
    } else {
      ++probe_;
      next_probe_ms_ = now + 3;
      make_frame(f, ch, type::kExistingClientRequest);
      send(f, out);
      if (options_.stock_module && ch == bwa::kChannelWifi) {
        const uint8_t r[3] = {0x04, 0x37, 0x00};
        make_frame(f, ch, type::kExistingClientResponse, r, 3);
        send(f, out);
        if (!polled(ch)) polled_.push_back(ch);
        return;
      }
      window_ = Window::kExisting;
      window_channel_ = ch;
      window_until_ms_ = now + 3;
      return;
    }
  }

  if (static_cast<int32_t>(now - next_status_ms_) >= 0) {
    next_status_ms_ = now + kStatusEveryMs;
    status_frame(f);
    send(f, out);
    return;
  }

  if (static_cast<int32_t>(now - next_new_client_ms_) >= 0) {
    next_new_client_ms_ = now + (now - powered_ms_ < 3000 ? 100 : 1000);
    make_frame(f, bwa::kChannelNewClient, type::kNewClientCts);
    send(f, out);
    window_ = Window::kNewClient;
    window_channel_ = bwa::kChannelNewClient;
    window_until_ms_ = now + options_.reply_window_ms;
    return;
  }

  if (!polled_.empty() && static_cast<int32_t>(now - next_cts_ms_) >= 0) {
    next_cts_ms_ = now + kCtsEveryMs;
    const uint8_t ch = polled_[cts_index_++ % polled_.size()];
    make_frame(f, ch, type::kClearToSend);
    send(f, out);
    ++cts_sent;
    if (options_.stock_module && ch == bwa::kChannelWifi) {
      // The stock module answers its own CTS, and now and then asks for the
      // spa's settings, which the board then answers on 0x0A.
      if (static_cast<int32_t>(now - next_stock_ms_) >= 0) {
        next_stock_ms_ = now + 400;
        static const uint8_t codes[] = {0x00, 0x01, 0x02, 0x04, 0x20};
        const uint8_t code = codes[stock_code_++ % 5];
        const uint8_t req[3] = {code, static_cast<uint8_t>(code == 0x20 ? 0xFF : 0x00),
                                static_cast<uint8_t>(code == 0x00 ? 0x01 : 0x00)};
        make_frame(f, ch, type::kSettingsRequest, req, 3);
        send(f, out);
        answer_settings(ch, code, out);
      } else {
        make_frame(f, ch, type::kNothingToSend);
        send(f, out);
      }
      return;
    }
    window_ = Window::kCts;
    window_channel_ = ch;
    window_until_ms_ = now + options_.reply_window_ms;
  }
}

void FakeBoard::receive(const uint8_t* data, size_t size, uint32_t now,
                        std::vector<uint8_t>& out) {
  parser_.feed(data, size, [&](const Frame& f) { frame_from_client(f, now, out); });
}

void FakeBoard::frame_from_client(const Frame& f, uint32_t now, std::vector<uint8_t>& out) {
  ++frames_from_bridge;
  const bool open = window_ != Window::kNone && static_cast<int32_t>(now - window_until_ms_) <= 0;
  if (!open) {
    ++unsolicited;  // on a real bus: a collision
    return;
  }
  if (f.channel() != window_channel_) {
    ++wrong_channel;
    return;
  }
  const Window was = window_;
  window_ = Window::kNone;
  const uint8_t ch = f.channel();
  const uint8_t t = f.type();
  Frame r;

  if (was == Window::kNewClient) {
    if (t != type::kChannelRequest || f.payload_size() < 3) return;
    ++channel_requests;
    uint8_t assigned = next_channel_;
    while (polled(assigned)) ++assigned;
    next_channel_ = static_cast<uint8_t>(assigned + 1);
    const uint8_t p[3] = {assigned, f.payload()[1], f.payload()[2]};
    make_frame(r, bwa::kChannelNewClient, type::kChannelResponse, p, 3);
    send(r, out);
    pending_channel_ = assigned;
    window_ = Window::kCts;  // the acknowledgement comes right after
    window_channel_ = assigned;
    window_until_ms_ = now + options_.reply_window_ms;
    return;
  }
  if (was == Window::kExisting) {
    if (t == type::kExistingClientResponse) {
      ++existing_answers;
      if (!polled(ch)) polled_.push_back(ch);
    }
    return;
  }
  // A CTS window (or the acknowledgement window after an assignment).
  if (t == type::kChannelAck) {
    if (pending_channel_ == ch) {
      ++acks;
      if (!polled(ch)) polled_.push_back(ch);
      pending_channel_ = 0;
    }
    return;
  }
  if (!polled(ch)) return;
  switch (t) {
    case type::kNothingToSend:
      ++nothing_to_send;
      break;
    case type::kToggleItem: {
      ++toggles;
      const uint8_t item = f.payload_size() ? f.payload()[0] : 0;
      if (item == 0x04) pumps[0] = static_cast<uint8_t>((pumps[0] + 1) % 3);
      else if (item == 0x05) pumps[1] = static_cast<uint8_t>((pumps[1] + 1) % 2);
      else if (item == 0x0C) blower = static_cast<uint8_t>((blower + 1) % 2);
      else if (item == 0x11) light = !light;
      else if (item == 0x50) high_range = !high_range;
      else if (item == 0x51) heat_mode = heat_mode ? 0 : 1;
      next_status_ms_ = now;  // the board reports a change quickly
      break;
    }
    case type::kSetTemperature:
      ++set_temperatures;
      if (f.payload_size()) target = f.payload()[0] / 2.0f;
      next_status_ms_ = now;
      break;
    case type::kSetTime:
      if (f.payload_size() >= 2) {
        hour = f.payload()[0] & 0x7F;
        minute = f.payload()[1];
      }
      break;
    case type::kFilterCycles:
      if (f.payload_size() >= 8) std::memcpy(filter_cycles, f.payload(), 8);
      break;
    case type::kSettingsRequest:
      if (f.payload_size() >= 1) answer_settings(ch, f.payload()[0], out);
      break;
    default:
      break;
  }
}

void FakeBoard::answer_settings(uint8_t ch, uint8_t code, std::vector<uint8_t>& out) {
  Frame r;
  switch (code) {
    case 0x00: {
      const uint8_t p[6] = {0x06, 0x00, 0x01, 0x01, 0x00, 0x00};
      make_frame(r, ch, type::kConfiguration, p, sizeof(p));
      break;
    }
    case 0x01:
      make_frame(r, ch, type::kFilterCycles, filter_cycles, 8);
      break;
    case 0x02: {
      uint8_t p[21] = {100, 220, 6, 0};
      std::memcpy(p + 4, kModel, 8);
      const uint8_t tail[9] = {1, 0x4d, 0x1c, 0x4a, 0x0b, 1, 0x0A, 0, 0};
      std::memcpy(p + 12, tail, 9);
      make_frame(r, ch, type::kInformation, p, sizeof(p));
      break;
    }
    case 0x04: {
      const uint8_t p[9] = {0x0C, 0x14, 50, 99, 80, 104, 0, 0b11, 0};
      make_frame(r, ch, type::kSetupParameters, p, sizeof(p));
      break;
    }
    case 0x08: {
      const uint8_t p[18] = {};
      make_frame(r, ch, type::kPreferences, p, sizeof(p));
      break;
    }
    case 0x20: {
      const uint8_t p[10] = {3, 2, 19, 1, 12, 0, 0, 24, 24, 24};
      make_frame(r, ch, type::kFaultLog, p, sizeof(p));
      break;
    }
    default:
      return;
  }
  ++settings_answered;
  send(r, out);
}

std::string FakeBoard::stats_json() const {
  std::string polled_list;
  for (size_t i = 0; i < polled_.size(); ++i) {
    char b[8];
    std::snprintf(b, sizeof(b), "%s%u", i ? "," : "", polled_[i]);
    polled_list += b;
  }
  char buf[1024];
  std::snprintf(buf, sizeof(buf),
                "{\"polled\":[%s],\"frames_from_bridge\":%u,\"unsolicited\":%u,"
                "\"wrong_channel\":%u,\"channel_requests\":%u,\"acks\":%u,"
                "\"nothing_to_send\":%u,\"toggles\":%u,\"set_temperatures\":%u,"
                "\"settings_answered\":%u,\"existing_answers\":%u,\"cts_sent\":%u,"
                "\"target\":%.1f,\"pumps\":[%u,%u],\"light\":%s,\"blower\":%u,"
                "\"hour\":%u,\"minute\":%u}",
                polled_list.c_str(), frames_from_bridge, unsolicited, wrong_channel,
                channel_requests, acks, nothing_to_send, toggles, set_temperatures,
                settings_answered, existing_answers, cts_sent, target, pumps[0], pumps[1],
                light ? "true" : "false", blower, hour, minute);
  return buf;
}

}  // namespace sim
