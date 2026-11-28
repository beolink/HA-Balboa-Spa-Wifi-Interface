// Host unit tests for the protocol core. `make test`.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "bwa_bus.h"
#include "bwa_frame.h"
#include "bwa_hub.h"
#include "bwa_status.h"
#include "ota_policy.h"

using namespace bwa;

namespace {

int failures = 0;
int checks = 0;

#define CHECK(cond)                                                       \
  do {                                                                    \
    ++checks;                                                             \
    if (!(cond)) {                                                        \
      ++failures;                                                         \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
    }                                                                     \
  } while (0)

std::vector<uint8_t> hex(const char* text) {
  std::vector<uint8_t> out;
  unsigned v;
  int n;
  while (std::sscanf(text, " %2x%n", &v, &n) == 1) {
    out.push_back(static_cast<uint8_t>(v));
    text += n;
  }
  return out;
}

Frame frame_of(uint8_t ch, uint8_t t, std::vector<uint8_t> payload = {}) {
  Frame f;
  make_frame(f, ch, t, payload.data(), payload.size());
  return f;
}

std::vector<Frame> parse_all(const std::vector<uint8_t>& bytes, size_t chunk = 1) {
  std::vector<Frame> out;
  FrameParser p;
  for (size_t at = 0; at < bytes.size(); at += chunk) {
    const size_t n = std::min(chunk, bytes.size() - at);
    p.feed(bytes.data() + at, n, [&](const Frame& f) { out.push_back(f); });
  }
  return out;
}

struct FakeBusIo : BusIo {
  std::vector<Frame> sent;
  std::vector<std::pair<Frame, bool>> up;
  std::vector<uint8_t> channels;
  void transmit(const Frame& f) override { sent.push_back(f); }
  void upstream(const Frame& f, bool cache_only) override { up.push_back({f, cache_only}); }
  void channel_changed(uint8_t ch) override { channels.push_back(ch); }
};

struct FakeHubIo : HubIo {
  std::vector<std::pair<int, std::vector<uint8_t>>> to_client;
  std::vector<Frame> bus;
  bool full = false;
  void send_to_client(int slot, const uint8_t* d, size_t n) override {
    to_client.push_back({slot, std::vector<uint8_t>(d, d + n)});
  }
  bool to_bus(const Frame& f) override {
    if (full) return false;
    bus.push_back(f);
    return true;
  }
};

// ---------------------------------------------------------------- frames

void test_crc_known_frames() {
  // From real captures (wiki, the ESP32 fork, pybalboa traffic).
  const char* known[] = {
      "7e 05 0a bf 04 77 7e",                 // existing client request / device present
      "7e 08 0a bf 22 00 00 01 58 7e",        // configuration request
      "7e 08 0a bf 22 01 00 00 34 7e",        // filter cycles request
      "7e 08 0a bf 22 02 00 00 89 7e",        // information request
      "7e 08 0a bf 22 04 00 00 f4 7e",        // setup parameters request
      "7e 08 0a bf 22 08 00 00 0e 7e",        // preferences request
      "7e 08 0a bf 22 20 ff 00 cb 7e",        // last fault
      "7e 05 0a bf 03 62 7e",                 // channel acknowledgement
  };
  for (const char* k : known) {
    const std::vector<uint8_t> b = hex(k);
    CHECK(valid_frame(b.data(), b.size()));
    Frame f;
    make_frame(f, b[2], b[4], b.data() + 5, b.size() - 7);
    CHECK(f.size() == b.size());
    CHECK(std::memcmp(f.bytes, b.data(), b.size()) == 0);
  }
  // pybalboa's checksum of "08 0a bf 22 00 00 01" is 0x58 as well.
  const std::vector<uint8_t> body = hex("08 0a bf 22 00 00 01");
  CHECK(crc8(body.data(), body.size()) == 0x58);
}

void test_kind_and_set_channel() {
  Frame f = frame_of(0xFF, type::kStatus, std::vector<uint8_t>(24, 0));
  CHECK(f.kind() == kKindBroadcast);
  Frame g = frame_of(0x10, type::kConfiguration, {0x06, 0, 1, 1, 0, 0});
  CHECK(g.kind() == kKindDirect);
  set_channel(g, kChannelWifi);
  CHECK(g.channel() == kChannelWifi);
  CHECK(valid_frame(g.bytes, g.size()));
  CHECK(g.payload()[0] == 0x06);
}

void test_parser_resync() {
  const std::vector<uint8_t> a = hex("7e 05 0a bf 04 77 7e");
  const std::vector<uint8_t> b = hex("7e 08 0a bf 22 00 00 01 58 7e");
  std::vector<uint8_t> s = hex("00 11 7e 7e 33 7e 05");  // noise, a fake start
  s.insert(s.end(), a.begin(), a.end());
  s.insert(s.end(), b.begin(), b.end());
  for (size_t chunk : {1u, 2u, 3u, 7u, 64u}) {
    const std::vector<Frame> got = parse_all(s, chunk);
    CHECK(got.size() == 2);
    if (got.size() == 2) {
      CHECK(got[0].type() == 0x04);
      CHECK(got[1].type() == 0x22);
    }
  }
}

void test_parser_shared_delimiter_and_bad_crc() {
  // Second frame reuses the first one's closing 7E as its opening one.
  std::vector<uint8_t> s = hex("7e 05 0a bf 04 77 7e 08 0a bf 22 00 00 01 58 7e");
  CHECK(parse_all(s).size() == 2);
  // A frame with a broken CRC is skipped, the next one is still read.
  std::vector<uint8_t> t = hex("7e 05 0a bf 04 78 7e 7e 05 0a bf 03 62 7e");
  FrameParser p;
  std::vector<Frame> got;
  p.feed(t.data(), t.size(), [&](const Frame& f) { got.push_back(f); });
  CHECK(got.size() == 1 && got[0].type() == 0x03);
}

void test_parser_not_held_back_by_noise() {
  // "7E 60" claims a 98-byte frame. The CTS right behind it must come out
  // the moment its closing delimiter arrives.
  std::vector<uint8_t> s = hex("7e 60 01 02 03");
  const std::vector<uint8_t> cts = hex("7e 05 10 bf 06");
  s.insert(s.end(), cts.begin(), cts.end());
  Frame f;
  make_frame(f, 0x10, type::kClearToSend);
  s.push_back(f.crc());
  FrameParser p;
  std::vector<Frame> got;
  p.feed(s.data(), s.size(), [&](const Frame& g) { got.push_back(g); });
  CHECK(got.empty());  // not closed yet
  const uint8_t close = 0x7E;
  p.feed(&close, 1, [&](const Frame& g) { got.push_back(g); });
  CHECK(got.size() == 1 && got[0].is(0x10, type::kClearToSend));
  // Long streams of noise do not grow memory or stop the parser.
  std::vector<uint8_t> noise(5000);
  for (size_t i = 0; i < noise.size(); ++i) noise[i] = static_cast<uint8_t>(i * 37 + 0x7E * (i % 9 == 0));
  p.feed(noise.data(), noise.size(), [&](const Frame&) {});
  std::vector<uint8_t> after(f.bytes, f.bytes + f.size());
  got.clear();
  p.feed(after.data(), after.size(), [&](const Frame& g) { got.push_back(g); });
  CHECK(got.size() == 1);
}

void test_parser_delimiter_inside_payload() {
  // 0x7E is 63 °C doubled; it can turn up in a payload unescaped.
  std::vector<uint8_t> payload(24, 0);
  payload[2] = 0x7E;
  payload[20] = 0x7E;
  Frame f = frame_of(0xFF, type::kStatus, payload);
  std::vector<uint8_t> s(f.bytes, f.bytes + f.size());
  const std::vector<Frame> got = parse_all(s);
  CHECK(got.size() == 1 && got[0].payload()[2] == 0x7E);
}

void test_decode_status_and_info() {
  std::vector<uint8_t> p(24, 0);
  p[2] = 24;  // 12 °C
  p[3] = 4;
  p[4] = 8;
  p[9] = 0x03;
  p[10] = 0x14;
  p[11] = 0x02 | (0x01 << 2);
  p[14] = 0x03;
  p[20] = 76;
  SpaStatus s;
  CHECK(decode_status(frame_of(0xFF, type::kStatus, p), s));
  CHECK(s.celsius && s.water == 12.0f && s.target == 38.0f);
  CHECK(s.pumps[0] == 2 && s.pumps[1] == 1 && s.light && s.high_range && s.heating == 1);
  std::vector<uint8_t> info = {100, 220, 6, 0, 'B', 'P', '2', '1', '0', '0', 'G', '1', 1, 0, 0, 0, 0, 1, 10, 0, 0};
  SpaInfo i;
  CHECK(decode_information(frame_of(0x0A, type::kInformation, info), i));
  CHECK(std::string(i.model) == "BP2100G1");
  CHECK(std::string(i.software) == "M100_220 V6");
}

// ------------------------------------------------------------- bus client

BusConfig assigned(uint8_t previous = 0) {
  BusConfig c;
  c.mode = BusMode::kAssigned;
  c.client_hash = 0x4E59;
  c.previous_channel = previous;
  return c;
}

void test_assigned_flow() {
  FakeBusIo io;
  BusClient::DownRing down;
  BusClient bus(io, down);
  bus.configure(assigned());
  uint32_t t = 1000;

  bus.on_frame(frame_of(0xFF, type::kStatus, std::vector<uint8_t>(24, 0)), t);
  CHECK(bus.state() == BusState::kRequesting);
  CHECK(io.up.size() == 1 && !io.up[0].second);
  CHECK(io.sent.empty());

  bus.on_frame(frame_of(0xFE, type::kNewClientCts), ++t);
  CHECK(io.sent.size() == 1);
  CHECK(io.sent[0].is(0xFE, type::kChannelRequest));
  CHECK(io.sent[0].payload()[0] == 0x02 && io.sent[0].payload()[1] == 0x4E &&
        io.sent[0].payload()[2] == 0x59);
  CHECK(bus.state() == BusState::kAwaitingChannel);

  // The board answers with a channel; its last hash byte may differ.
  bus.on_frame(frame_of(0xFE, type::kChannelResponse, {0x11, 0x4E, 0x79}), t += 15);
  CHECK(bus.state() == BusState::kOnline);
  CHECK(bus.channel() == 0x11);
  CHECK(io.channels.size() == 1 && io.channels[0] == 0x11);
  CHECK(io.sent.size() == 2 && io.sent[1].is(0x11, type::kChannelAck));

  // CTS with nothing queued: Nothing to Send.
  bus.on_frame(frame_of(0x11, type::kClearToSend), t += 15);
  CHECK(io.sent.size() == 3 && io.sent[2].is(0x11, type::kNothingToSend));
  // CTS to another channel: silence.
  bus.on_frame(frame_of(0x10, type::kClearToSend), t += 15);
  CHECK(io.sent.size() == 3);

  // A client's request goes out on our channel at our next CTS, one per CTS.
  Frame req = frame_of(0x0A, type::kSettingsRequest, {0x02, 0x00, 0x00});
  Frame toggle = frame_of(0x0A, type::kToggleItem, {0x04, 0x00});
  CHECK(down.push(req));
  CHECK(down.push(toggle));
  bus.on_frame(frame_of(0x11, type::kClearToSend), t += 30);
  CHECK(io.sent.size() == 4 && io.sent[3].is(0x11, type::kSettingsRequest));
  CHECK(valid_frame(io.sent[3].bytes, io.sent[3].size()));
  // Our write echoed back by the transceiver is not passed on.
  const size_t ups = io.up.size();
  bus.on_frame(io.sent[3], t + 1);
  CHECK(io.up.size() == ups);
  CHECK(bus.stats().echoes == 1);
  bus.on_frame(frame_of(0x11, type::kClearToSend), t += 30);
  CHECK(io.sent.size() == 5 && io.sent[4].is(0x11, type::kToggleItem));
  CHECK(bus.stats().sent == 2);

  // The board's answer on our channel goes up as a 0x0A frame for everyone.
  bus.on_frame(frame_of(0x11, type::kInformation, std::vector<uint8_t>(21, 1)), t += 5);
  CHECK(io.up.size() == ups + 1);
  CHECK(io.up.back().first.channel() == kChannelWifi && !io.up.back().second);
  // An answer to somebody else is kept, not pushed.
  bus.on_frame(frame_of(0x10, type::kConfiguration, {6, 0, 1, 1, 0, 0}), t += 5);
  CHECK(io.up.back().second);
  CHECK(io.up.back().first.channel() == kChannelWifi);
  // Another client's request on its channel is neither.
  const size_t before = io.up.size();
  bus.on_frame(frame_of(0x10, type::kToggleItem, {0x11, 0}), t += 5);
  CHECK(io.up.size() == before);

  // Existing Client Request on our channel: we are here.
  bus.on_frame(frame_of(0x11, type::kExistingClientRequest), t += 5);
  CHECK(io.sent.back().is(0x11, type::kExistingClientResponse));
  CHECK(io.sent.back().payload_size() == 3);
}

void test_listen_only_never_writes() {
  FakeBusIo io;
  BusClient::DownRing down;
  BusClient bus(io, down);
  BusConfig c;
  c.mode = BusMode::kListenOnly;
  bus.configure(c);
  uint32_t t = 0;
  for (int i = 0; i < 50; ++i) {
    bus.on_frame(frame_of(0xFE, type::kNewClientCts), t += 10);
    bus.on_frame(frame_of(0x0A, type::kClearToSend), t += 10);
    bus.on_frame(frame_of(0x10, type::kClearToSend), t += 10);
    bus.on_frame(frame_of(0x0A, type::kExistingClientRequest), t += 10);
    down.push(frame_of(0x0A, type::kToggleItem, {0x04, 0}));
    bus.tick(t);
  }
  CHECK(io.sent.empty());
  CHECK(bus.state() == BusState::kListening);
  CHECK(bus.stats().dropped_listen == 50);
}

void test_tx_blocked_is_listening() {
  FakeBusIo io;
  BusClient::DownRing down;
  BusClient bus(io, down);
  BusConfig c = assigned();
  c.tx_allowed = false;
  bus.configure(c);
  bus.on_frame(frame_of(0xFE, type::kNewClientCts), 10);
  CHECK(io.sent.empty() && bus.state() == BusState::kListening);
}

void test_previous_channel_taken_back() {
  FakeBusIo io;
  BusClient::DownRing down;
  BusClient bus(io, down);
  bus.configure(assigned(0x12));
  uint32_t t = 100;
  bus.on_frame(frame_of(0xFF, type::kStatus, std::vector<uint8_t>(24, 0)), t);
  CHECK(bus.state() == BusState::kObserving);
  bus.on_frame(frame_of(0xFE, type::kNewClientCts), t += 10);  // not while observing
  bus.on_frame(frame_of(0x12, type::kClearToSend), t += 30);
  bus.on_frame(frame_of(0x12, type::kClearToSend), t += 30);
  CHECK(io.sent.empty());
  bus.on_frame(frame_of(0x12, type::kClearToSend), t += 30);
  CHECK(bus.state() == BusState::kOnline && bus.channel() == 0x12);
  CHECK(io.sent.size() == 1 && io.sent[0].is(0x12, type::kNothingToSend));
  CHECK(bus.stats().channel_requests == 0);
}

void test_previous_channel_in_use() {
  FakeBusIo io;
  BusClient::DownRing down;
  BusClient bus(io, down);
  bus.configure(assigned(0x12));
  uint32_t t = 100;
  bus.on_frame(frame_of(0x12, type::kClearToSend), t);
  bus.on_frame(frame_of(0x12, type::kNothingToSend), t + 1);  // someone else answers
  CHECK(bus.state() == BusState::kRequesting);
  CHECK(io.sent.empty());
}

void test_previous_channel_timeout() {
  FakeBusIo io;
  BusClient::DownRing down;
  BusClient bus(io, down);
  bus.configure(assigned(0x12));
  bus.on_frame(frame_of(0xFF, type::kStatus, std::vector<uint8_t>(24, 0)), 100);
  bus.tick(100 + BusClient::kObserveMs + 1);
  CHECK(bus.state() == BusState::kRequesting);
}

void test_lost_channel_asks_again() {
  FakeBusIo io;
  BusClient::DownRing down;
  BusClient bus(io, down);
  bus.configure(assigned());
  uint32_t t = 100;
  bus.on_frame(frame_of(0xFE, type::kNewClientCts), t);
  bus.on_frame(frame_of(0xFE, type::kChannelResponse, {0x10, 0x4E, 0x59}), t += 10);
  CHECK(bus.online());
  // Bus traffic continues but CTS for us stops.
  for (int i = 0; i < 50; ++i) {
    bus.on_frame(frame_of(0x11, type::kClearToSend), t += 100);
    bus.tick(t);
  }
  CHECK(bus.state() == BusState::kRequesting);
  CHECK(bus.stats().channel_losses == 1);
  CHECK(io.channels.size() == 2 && io.channels[1] == 0);
}

void test_gives_up_after_too_many_requests() {
  FakeBusIo io;
  BusClient::DownRing down;
  BusClient bus(io, down);
  bus.configure(assigned());
  uint32_t t = 0;
  for (int i = 0; i < 40; ++i) {
    bus.on_frame(frame_of(0xFE, type::kNewClientCts), t += 1000);
    bus.tick(t += BusClient::kAssignTimeoutMs + 1);
  }
  CHECK(bus.state() == BusState::kGaveUp);
  CHECK(bus.stats().channel_requests == BusClient::kMaxChannelRequests);
  // Each try used a new hash, like panels do.
  CHECK(io.sent.size() == BusClient::kMaxChannelRequests);
  CHECK(io.sent[0].payload()[2] != io.sent[1].payload()[2]);
}

void test_unpolled_channel_gives_up() {
  FakeBusIo io;
  BusClient::DownRing down;
  BusClient bus(io, down);
  bus.configure(assigned());
  bus.on_frame(frame_of(0xFE, type::kNewClientCts), 10);
  bus.on_frame(frame_of(0xFE, type::kChannelResponse, {0x31, 0x4E, 0x59}), 20);
  CHECK(bus.state() == BusState::kGaveUp);
  CHECK(io.sent.size() == 1);  // no acknowledgement for a channel we will not use
}

void test_foreign_assignment_ignored() {
  FakeBusIo io;
  BusClient::DownRing down;
  BusClient bus(io, down);
  bus.configure(assigned());
  bus.on_frame(frame_of(0xFE, type::kNewClientCts), 10);
  bus.on_frame(frame_of(0xFE, type::kChannelResponse, {0x10, 0xF2, 0x47}), 20);  // a panel's
  CHECK(bus.state() == BusState::kAwaitingChannel);
}

void test_fixed_wifi() {
  {
    FakeBusIo io;
    BusClient::DownRing down;
    BusClient bus(io, down);
    BusConfig c;
    c.mode = BusMode::kFixedWifi;
    bus.configure(c);
    bus.on_frame(frame_of(0x0A, type::kClearToSend), 10);
    bus.on_frame(frame_of(0x0A, type::kNothingToSend), 11);  // the stock module is there
    CHECK(bus.state() == BusState::kGaveUp);
    bus.on_frame(frame_of(0x0A, type::kClearToSend), 30);
    CHECK(io.sent.empty());
  }
  {
    FakeBusIo io;
    BusClient::DownRing down;
    BusClient bus(io, down);
    BusConfig c;
    c.mode = BusMode::kFixedWifi;
    bus.configure(c);
    uint32_t t = 10;
    for (int i = 0; i < 3; ++i) bus.on_frame(frame_of(0x0A, type::kClearToSend), t += 20);
    CHECK(bus.online() && bus.channel() == 0x0A);
    CHECK(io.sent.size() == 1 && io.sent[0].is(0x0A, type::kNothingToSend));
    down.push(frame_of(0x0A, type::kSetTemperature, {76}));
    bus.on_frame(frame_of(0x0A, type::kClearToSend), t += 20);
    CHECK(io.sent.back().is(0x0A, type::kSetTemperature));
    // The board's answer on 0x0A goes to the clients.
    bus.on_frame(frame_of(0x0A, type::kConfiguration, {6, 0, 1, 1, 0, 0}), t += 5);
    CHECK(!io.up.empty() && !io.up.back().second);
  }
}

// -------------------------------------------------------------------- hub

void test_module_identification_and_discovery() {
  FakeHubIo io;
  Hub hub;
  hub.attach(&io);
  HubConfig c;
  const uint8_t mac[6] = {0x00, 0x15, 0x27, 0x64, 0x4E, 0x59};
  std::memcpy(c.mac, mac, 6);
  hub.configure(c);
  hub.client_opened(0);
  Frame ask = frame_of(0x0A, type::kExistingClientRequest);
  hub.client_bytes(0, ask.bytes, ask.size(), false, 10);
  CHECK(io.to_client.size() == 1);
  const std::vector<Frame> got = parse_all(io.to_client[0].second);
  CHECK(got.size() == 1);
  if (got.size() == 1) {
    const Frame& f = got[0];
    CHECK(f.is(0x0A, type::kModuleIdentification));
    CHECK(f.payload_size() == 25);
    CHECK(std::memcmp(f.payload() + 3, mac, 6) == 0);  // where pybalboa reads the MAC
    CHECK(f.payload()[20] == 0xFF && f.payload()[24] == 0x59);
  }
  char reply[64];
  hub.discovery_reply(reply, sizeof(reply));
  CHECK(std::string(reply) == "BWGSPA\r\n00-15-27-64-4E-59\r\n");
}

void test_hub_forwards_or_answers_from_cache() {
  FakeHubIo io;
  Hub hub;
  hub.attach(&io);
  hub.client_opened(1);
  Frame req = frame_of(0x0A, type::kSettingsRequest, {0x00, 0x00, 0x01});

  // Not on the bus, nothing heard yet: a miss.
  hub.client_bytes(1, req.bytes, req.size(), false, 10);
  CHECK(io.to_client.empty() && io.bus.empty());
  CHECK(hub.stats().cache_misses == 1);

  // Heard on the bus (the stock module asked), then asked by our client.
  Frame answer = frame_of(0x0A, type::kConfiguration, {6, 0, 1, 1, 0, 0});
  hub.bus_frame(answer, true, 20);
  CHECK(io.to_client.empty());  // cache_only: not pushed
  hub.client_bytes(1, req.bytes, req.size(), false, 30);
  CHECK(io.to_client.size() == 1 && io.to_client[0].first == 1);
  CHECK(hub.stats().cache_answers == 1);

  // A toggle while listening goes nowhere.
  Frame toggle = frame_of(0x0A, type::kToggleItem, {0x04, 0});
  hub.client_bytes(1, toggle.bytes, toggle.size(), false, 40);
  CHECK(io.bus.empty() && hub.stats().dropped_listen == 1);

  // On the bus: requests and toggles go to the bus.
  hub.client_bytes(1, req.bytes, req.size(), true, 50);
  hub.client_bytes(1, toggle.bytes, toggle.size(), true, 51);
  CHECK(io.bus.size() == 2 && io.bus[1].type() == type::kToggleItem);

  // Installer and Wi-Fi setup messages are never passed on.
  Frame setup = frame_of(0x0A, 0x2A, {0x01});
  Frame wifi = frame_of(0x0A, type::kSetWifi, std::vector<uint8_t>(10, 0));
  hub.client_bytes(1, setup.bytes, setup.size(), true, 60);
  hub.client_bytes(1, wifi.bytes, wifi.size(), true, 61);
  CHECK(io.bus.size() == 2 && hub.stats().dropped_type == 2);

  // A full bus queue is counted, not hidden.
  io.full = true;
  hub.client_bytes(1, toggle.bytes, toggle.size(), true, 70);
  CHECK(hub.stats().bus_queue_full == 1);
}

void test_hub_status_push_and_throttle() {
  FakeHubIo io;
  Hub hub;
  hub.attach(&io);
  HubConfig c;
  c.status_interval_ms = 1000;
  hub.configure(c);
  hub.client_opened(0);
  hub.client_opened(2);
  std::vector<uint8_t> p(24, 0);
  p[2] = 24;
  p[9] = 0x01;  // Celsius
  Frame s1 = frame_of(0xFF, type::kStatus, p);
  hub.bus_frame(s1, false, 100);
  CHECK(io.to_client.size() == 2);  // both clients
  hub.bus_frame(s1, false, 400);    // unchanged, too soon
  CHECK(io.to_client.size() == 2);
  hub.bus_frame(s1, false, 1200);   // unchanged, interval passed
  CHECK(io.to_client.size() == 4);
  p[2] = 25;
  hub.bus_frame(frame_of(0xFF, type::kStatus, p), false, 1300);  // changed
  CHECK(io.to_client.size() == 6);
  // A client that connects now gets the last status straight away.
  hub.client_opened(1);
  CHECK(io.to_client.size() == 7 && io.to_client.back().first == 1);
  const Hub::Snapshot snap = hub.snapshot();
  CHECK(snap.status.valid && snap.status.water == 12.5f && snap.clients == 3);
}

// ----------------------------------------------------------- update policy

void test_ota_confirms_once_online_long_enough() {
  OtaPolicy p;
  CHECK(p.step(false, 1000) == OtaAction::kNone);
  CHECK(p.step(true, 20000) == OtaAction::kNone);   // online from 20 s
  CHECK(p.step(true, 79000) == OtaAction::kNone);   // 59 s online
  CHECK(p.step(true, 80000) == OtaAction::kNone);   // 60 s online, but up only 80 s
  CHECK(p.step(true, 90000) == OtaAction::kConfirm);
}

void test_ota_online_must_be_unbroken() {
  OtaPolicy p;
  CHECK(p.step(true, 10000) == OtaAction::kNone);
  CHECK(p.step(false, 60000) == OtaAction::kNone);  // dropped off
  CHECK(p.step(true, 65000) == OtaAction::kNone);   // the clock starts again
  CHECK(p.step(true, 120000) == OtaAction::kNone);  // 55 s
  CHECK(p.step(true, 125000) == OtaAction::kConfirm);
}

void test_ota_rolls_back_when_never_online() {
  OtaPolicy p;
  CHECK(p.step(false, 300000) == OtaAction::kNone);
  CHECK(p.step(false, 599999) == OtaAction::kNone);
  CHECK(p.step(false, 600000) == OtaAction::kRollBack);
}

void test_ota_online_at_the_deadline_gets_its_minute() {
  OtaPolicy p;
  CHECK(p.step(true, 590000) == OtaAction::kNone);
  CHECK(p.step(true, 620000) == OtaAction::kNone);  // past the deadline but online
  CHECK(p.step(true, 650000) == OtaAction::kConfirm);
  OtaPolicy q;
  CHECK(q.step(true, 590000) == OtaAction::kNone);
  CHECK(q.step(false, 610000) == OtaAction::kRollBack);  // lost it again
}

}  // namespace

int main() {
  test_crc_known_frames();
  test_kind_and_set_channel();
  test_parser_resync();
  test_parser_shared_delimiter_and_bad_crc();
  test_parser_not_held_back_by_noise();
  test_parser_delimiter_inside_payload();
  test_decode_status_and_info();
  test_assigned_flow();
  test_listen_only_never_writes();
  test_tx_blocked_is_listening();
  test_previous_channel_taken_back();
  test_previous_channel_in_use();
  test_previous_channel_timeout();
  test_lost_channel_asks_again();
  test_gives_up_after_too_many_requests();
  test_unpolled_channel_gives_up();
  test_foreign_assignment_ignored();
  test_fixed_wifi();
  test_module_identification_and_discovery();
  test_hub_forwards_or_answers_from_cache();
  test_hub_status_push_and_throttle();
  test_ota_confirms_once_online_long_enough();
  test_ota_online_must_be_unbroken();
  test_ota_rolls_back_when_never_online();
  test_ota_online_at_the_deadline_gets_its_minute();
  std::printf("%d checks, %d failed\n", checks, failures);
  return failures ? 1 : 0;
}
