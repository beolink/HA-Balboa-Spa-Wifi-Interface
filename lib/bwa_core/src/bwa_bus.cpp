#include "bwa_bus.h"

#include <cstring>

namespace bwa {

const char* mode_name(BusMode mode) {
  switch (mode) {
    case BusMode::kListenOnly: return "listen_only";
    case BusMode::kAssigned: return "assigned";
    case BusMode::kFixedWifi: return "fixed_wifi";
  }
  return "?";
}

bool parse_mode(const char* text, BusMode& out) {
  if (!text) return false;
  if (std::strcmp(text, "listen_only") == 0) out = BusMode::kListenOnly;
  else if (std::strcmp(text, "assigned") == 0) out = BusMode::kAssigned;
  else if (std::strcmp(text, "fixed_wifi") == 0) out = BusMode::kFixedWifi;
  else return false;
  return true;
}

const char* state_name(BusState state) {
  switch (state) {
    case BusState::kSilent: return "silent";
    case BusState::kListening: return "listening";
    case BusState::kObserving: return "observing";
    case BusState::kRequesting: return "requesting";
    case BusState::kAwaitingChannel: return "awaiting_channel";
    case BusState::kOnline: return "online";
    case BusState::kGaveUp: return "gave_up";
  }
  return "?";
}

bool client_originated(uint8_t t) {
  switch (t) {
    case type::kChannelRequest:
    case type::kChannelAck:
    case type::kExistingClientResponse:
    case type::kNothingToSend:
    case type::kToggleItem:
    case type::kSetTemperature:
    case type::kSetTime:
    case type::kSettingsRequest:
    case type::kSetPreference:
    case 0x2A:  // change setup
    case 0x2D:  // lock
    case 0xE0:  // toggle test setting
    case type::kSetWifi:
      return true;
    default:
      return false;
  }
}

bool board_answer(uint8_t t) {
  switch (t) {
    case type::kFilterCycles:
    case type::kInformation:
    case type::kSetupParameters:
    case type::kPreferences:
    case type::kFaultLog:
    case 0x29:  // settings 0x40
    case type::kConfiguration:
      return true;
    default:
      return false;
  }
}

void BusClient::configure(const BusConfig& config) {
  config_ = config;
  hash_ = config.client_hash;
  requests_ = 0;
  previous_cts_ = 0;
  expect_echo_ = false;
  channel_.store(0);
  set_state(BusState::kSilent);
}

void BusClient::set_channel_held(uint8_t ch) {
  if (channel_.load() == ch) return;
  channel_.store(ch);
  io_.channel_changed(ch);
}

void BusClient::enter_start_state(uint32_t now_ms) {
  state_since_ms_ = now_ms;
  previous_cts_ = 0;
  if (!config_.tx_allowed || config_.mode == BusMode::kListenOnly) {
    set_state(BusState::kListening);
  } else if (config_.mode == BusMode::kFixedWifi) {
    set_state(BusState::kObserving);
  } else if (config_.previous_channel >= kChannelFirstAssigned &&
             config_.previous_channel <= kChannelLastPolled) {
    set_state(BusState::kObserving);
  } else {
    set_state(BusState::kRequesting);
  }
}

void BusClient::send(const Frame& frame, uint32_t now_ms) {
  io_.transmit(frame);
  last_tx_ = frame;
  last_tx_ms_ = now_ms;
  expect_echo_ = true;
}

void BusClient::answer_cts(uint32_t now_ms) {
  ++stats_.cts_ours;
  last_cts_ours_ms_ = now_ms;
  Frame out;
  if (down_.pop(out)) {
    set_channel(out, channel_.load());
    send(out, now_ms);
    ++stats_.sent;
  } else {
    make_frame(out, channel_.load(), type::kNothingToSend);
    send(out, now_ms);
    ++stats_.nothing_to_send;
  }
}

void BusClient::request_channel(uint32_t now_ms) {
  if (requests_ >= kMaxChannelRequests) {
    set_state(BusState::kGaveUp);
    state_since_ms_ = now_ms;
    return;
  }
  ++requests_;
  ++stats_.channel_requests;
  const uint8_t payload[3] = {config_.device_type, static_cast<uint8_t>(hash_ >> 8),
                              static_cast<uint8_t>(hash_ & 0xFF)};
  Frame out;
  make_frame(out, kChannelNewClient, type::kChannelRequest, payload, sizeof(payload));
  send(out, now_ms);
  set_state(BusState::kAwaitingChannel);
  state_since_ms_ = now_ms;
}

void BusClient::forward_for_us(const Frame& frame) {
  Frame up = frame;
  set_channel(up, kChannelWifi);
  io_.upstream(up, false);
}

void BusClient::drain_unsendable() {
  Frame dropped;
  while (down_.pop(dropped)) ++stats_.dropped_listen;
}

void BusClient::on_frame(const Frame& f, uint32_t now_ms) {
  ++stats_.frames;
  stats_.last_frame_ms.store(now_ms);
  if (state() == BusState::kSilent) enter_start_state(now_ms);

  // Our own write read back by the transceiver.
  if (expect_echo_) {
    if (now_ms - last_tx_ms_ <= kEchoWindowMs && f.same_as(last_tx_)) {
      expect_echo_ = false;
      ++stats_.echoes;
      return;
    }
    if (now_ms - last_tx_ms_ > kEchoWindowMs) expect_echo_ = false;
  }

  const uint8_t ch = f.channel();
  const uint8_t t = f.type();
  if (t == type::kClearToSend) ++stats_.cts_seen;
  if (ch == kChannelWifi) {
    if (t == type::kClearToSend) ++stats_.wifi_channel_cts;
    else if (client_originated(t)) ++stats_.wifi_channel_answers;
  }

  // Upstream first: broadcasts to everyone, answers on other channels to the cache.
  const uint8_t ours = channel_.load();
  const bool online_now = state() == BusState::kOnline;
  if (ch == kChannelBroadcast) {
    io_.upstream(f, false);
  } else if (online_now && ch == ours) {
    if (t > type::kNothingToSend && !client_originated(t)) forward_for_us(f);
  } else if (ch != kChannelNewClient && board_answer(t)) {
    Frame up = f;
    set_channel(up, kChannelWifi);
    io_.upstream(up, true);
  }

  switch (state()) {
    case BusState::kSilent:
    case BusState::kListening:
    case BusState::kGaveUp:
      break;

    case BusState::kObserving: {
      const bool fixed = config_.mode == BusMode::kFixedWifi;
      const uint8_t target = fixed ? kChannelWifi : config_.previous_channel;
      if (ch == target && client_originated(t)) {
        // Somebody else answers on that channel.
        if (fixed) {
          set_state(BusState::kGaveUp);
        } else {
          set_state(BusState::kRequesting);
        }
        state_since_ms_ = now_ms;
      } else if (f.is(target, type::kClearToSend) && ++previous_cts_ >= kPreviousChannelCts) {
        set_channel_held(target);
        set_state(BusState::kOnline);
        answer_cts(now_ms);
      }
      break;
    }

    case BusState::kRequesting:
      if (f.is(kChannelNewClient, type::kNewClientCts)) request_channel(now_ms);
      break;

    case BusState::kAwaitingChannel:
      if (f.is(kChannelNewClient, type::kChannelResponse) && f.payload_size() >= 3 &&
          f.payload()[1] == static_cast<uint8_t>(hash_ >> 8)) {
        // The board echoes our hash, though not always its last byte.
        const uint8_t assigned = f.payload()[0];
        if (assigned < kChannelFirstAssigned || assigned > kChannelLastPolled) {
          // A channel the board does not poll is no use; do not ask again.
          set_state(BusState::kGaveUp);
          state_since_ms_ = now_ms;
          break;
        }
        set_channel_held(assigned);
        Frame ack;
        make_frame(ack, assigned, type::kChannelAck);
        send(ack, now_ms);
        last_cts_ours_ms_ = now_ms;
        set_state(BusState::kOnline);
        state_since_ms_ = now_ms;
      }
      break;

    case BusState::kOnline:
      if (ch == ours) {
        if (t == type::kClearToSend) {
          answer_cts(now_ms);
        } else if (t == type::kExistingClientRequest) {
          Frame out;
          make_frame(out, ours, type::kExistingClientResponse, config_.existing_response, 3);
          send(out, now_ms);
        }
      }
      break;
  }
}

void BusClient::tick(uint32_t now_ms) {
  switch (state()) {
    case BusState::kSilent:
    case BusState::kListening:
    case BusState::kGaveUp:
    case BusState::kRequesting:
      drain_unsendable();
      break;

    case BusState::kObserving:
      drain_unsendable();
      if (now_ms - state_since_ms_ > kObserveMs) {
        if (config_.mode == BusMode::kFixedWifi) {
          // Nobody answers on 0x0A. We still only ever write right after a
          // CTS to 0x0A or an Existing Client Request on it.
          set_channel_held(kChannelWifi);
          set_state(BusState::kOnline);
          last_cts_ours_ms_ = now_ms;
        } else {
          set_state(BusState::kRequesting);
        }
        state_since_ms_ = now_ms;
      }
      break;

    case BusState::kAwaitingChannel:
      drain_unsendable();
      if (now_ms - state_since_ms_ > kAssignTimeoutMs) {
        hash_ = static_cast<uint16_t>(hash_ + 1);  // like panels, try another hash
        set_state(requests_ >= kMaxChannelRequests ? BusState::kGaveUp : BusState::kRequesting);
        state_since_ms_ = now_ms;
      }
      break;

    case BusState::kOnline:
      if (config_.mode == BusMode::kAssigned && now_ms - last_cts_ours_ms_ > kLostAfterMs &&
          now_ms - stats_.last_frame_ms.load() < 1000) {
        // The bus is alive but nobody polls us: the board was power-cycled
        // or dropped the channel. Ask again.
        ++stats_.channel_losses;
        set_channel_held(0);
        drain_unsendable();
        set_state(BusState::kRequesting);
        state_since_ms_ = now_ms;
      }
      break;
  }
}

}  // namespace bwa
