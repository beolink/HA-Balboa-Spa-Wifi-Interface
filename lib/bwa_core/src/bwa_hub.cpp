#include "bwa_hub.h"

#include <cstdio>
#include <cstring>

namespace bwa {

namespace {
constexpr uint8_t kCacheOrder[] = {type::kConfiguration, type::kFilterCycles, type::kInformation,
                                   type::kSetupParameters, type::kPreferences, type::kFaultLog,
                                   0x29};

bool forwardable(uint8_t t) {
  switch (t) {
    case type::kToggleItem:
    case type::kSetTemperature:
    case type::kSetTime:
    case type::kSettingsRequest:
    case type::kFilterCycles:
    case type::kSetPreference:
      return true;
    default:
      return false;
  }
}
}  // namespace

void Hub::configure(const HubConfig& config) {
  std::lock_guard<std::mutex> lock(mutex_);
  config_ = config;
}

int Hub::cache_index(uint8_t t) {
  for (int i = 0; i < kCacheTypes; ++i) {
    if (kCacheOrder[i] == t) return i;
  }
  return -1;
}

uint8_t Hub::answer_type_for(uint8_t code) {
  switch (code) {
    case 0x00: return type::kConfiguration;
    case 0x01: return type::kFilterCycles;
    case 0x02: return type::kInformation;
    case 0x04: return type::kSetupParameters;
    case 0x08: return type::kPreferences;
    case 0x20: return type::kFaultLog;
    case 0x40: return 0x29;
    default: return 0;
  }
}

void Hub::client_opened(int slot) {
  if (slot < 0 || slot >= kMaxClients) return;
  clients_[slot].open = true;
  clients_[slot].parser.reset();
  ++stats_.connects;
  // The stock module starts pushing status as soon as a client connects.
  Frame status;
  bool have = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    have = have_status_;
    status = last_status_;
  }
  if (have) send_frame(slot, status);
}

void Hub::client_closed(int slot) {
  if (slot < 0 || slot >= kMaxClients) return;
  clients_[slot].open = false;
  clients_[slot].parser.reset();
}

void Hub::client_bytes(int slot, const uint8_t* data, size_t size, bool bus_online,
                       uint32_t /*now_ms*/) {
  if (slot < 0 || slot >= kMaxClients || !clients_[slot].open) return;
  clients_[slot].parser.feed(data, size, [&](const Frame& frame) {
    client_frame(slot, frame, bus_online);
  });
}

void Hub::client_frame(int slot, const Frame& frame, bool bus_online) {
  ++stats_.client_frames;
  const uint8_t t = frame.type();

  if (t == type::kExistingClientRequest) {
    Frame answer;
    module_identification(answer);
    send_frame(slot, answer);
    ++stats_.module_answers;
    return;
  }
  if (!forwardable(t)) {
    ++stats_.dropped_type;
    return;
  }
  if (bus_online) {
    Frame out = frame;
    set_channel(out, kChannelWifi);
    if (io_ && io_->to_bus(out)) {
      ++stats_.to_bus;
    } else {
      ++stats_.bus_queue_full;
    }
    return;
  }
  // Not on the bus: answer what we can from what we have heard.
  if (t == type::kSettingsRequest && frame.payload_size() >= 1) {
    const int index = cache_index(answer_type_for(frame.payload()[0]));
    Frame answer;
    bool have = false;
    if (index >= 0) {
      std::lock_guard<std::mutex> lock(mutex_);
      have = cached_[index];
      if (have) answer = cache_[index];
    }
    if (have && answer.type() == type::kFaultLog && frame.payload_size() >= 2 &&
        frame.payload()[1] != 0xFF && answer.payload_size() >= 2 &&
        answer.payload()[1] != frame.payload()[1]) {
      have = false;  // a different fault-log entry than the one asked for
    }
    if (have) {
      send_frame(slot, answer);
      ++stats_.cache_answers;
    } else {
      ++stats_.cache_misses;
    }
    return;
  }
  ++stats_.dropped_listen;
}

void Hub::bus_frame(const Frame& frame, bool cache_only, uint32_t now_ms) {
  const uint8_t t = frame.type();
  if (frame.channel() == kChannelBroadcast && t == type::kStatus) {
    bool push = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const bool changed = !have_status_ || !frame.same_as(last_status_);
      push = changed || config_.status_interval_ms == 0 ||
             now_ms - last_push_ms_ >= config_.status_interval_ms;
      last_status_ = frame;
      have_status_ = true;
      last_status_ms_ = now_ms ? now_ms : 1;
      decode_status(frame, status_);
      if (push) last_push_ms_ = now_ms;
    }
    if (push && !cache_only) push_all(frame);
    return;
  }

  const int index = cache_index(t);
  if (index >= 0) {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_[index] = frame;
    cached_[index] = true;
    if (t == type::kInformation) decode_information(frame, info_);
  }
  if (!cache_only) push_all(frame);
}

void Hub::push_all(const Frame& frame) {
  for (int i = 0; i < kMaxClients; ++i) {
    if (clients_[i].open) send_frame(i, frame);
  }
  ++stats_.pushed;
}

void Hub::send_frame(int slot, const Frame& frame) {
  if (io_) io_->send_to_client(slot, frame.bytes, frame.size());
}

void Hub::module_identification(Frame& out) const {
  uint8_t mac[6];
  {
    std::lock_guard<std::mutex> lock(mutex_);
    std::memcpy(mac, config_.mac, 6);
  }
  // Layout from the wiki's captures of real modules: 3 unknown bytes, the
  // MAC, 8 zero bytes, then an iDigi-style id made of the OUI, FF FF and the
  // MAC's last three bytes.
  uint8_t p[25] = {0x02, 0x14, 0x80};
  std::memcpy(p + 3, mac, 6);
  p[17] = mac[0];
  p[18] = mac[1];
  p[19] = mac[2];
  p[20] = 0xFF;
  p[21] = 0xFF;
  p[22] = mac[3];
  p[23] = mac[4];
  p[24] = mac[5];
  make_frame(out, kChannelWifi, type::kModuleIdentification, p, sizeof(p));
}

size_t Hub::discovery_reply(char* buf, size_t cap) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const uint8_t* m = config_.mac;
  const int n = std::snprintf(buf, cap, "%s\r\n%02X-%02X-%02X-%02X-%02X-%02X\r\n",
                              config_.hostname, m[0], m[1], m[2], m[3], m[4], m[5]);
  return n < 0 ? 0 : (static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : cap - 1);
}

Hub::Snapshot Hub::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  Snapshot s;
  s.status = status_;
  s.info = info_;
  s.status_ms = have_status_ ? last_status_ms_ : 0;
  for (int i = 0; i < kMaxClients; ++i) s.clients += clients_[i].open ? 1 : 0;
  for (int i = 0; i < kCacheTypes; ++i) s.cached |= cached_[i] ? (1u << i) : 0;
  return s;
}

}  // namespace bwa
