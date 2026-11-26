// Frames on the Balboa bus and on the BWA module's TCP port.
//
// Both carry the same frame:
//
//   7E  LEN  CHANNEL  KIND  TYPE  PAYLOAD...  CRC  7E
//
// LEN counts itself, CHANNEL, KIND, TYPE, the payload and CRC, so a frame is
// LEN + 2 bytes long. KIND is 0xAF on the broadcast channel 0xFF and 0xBF
// otherwise. CRC is CRC-8 (poly 0x07, init 0x02, final xor 0x02) over LEN up
// to the last payload byte. LEN is always below 0x7E, which is how a reader
// can tell a delimiter from a length.
//
// Portable C++17, no Arduino: the same code runs on the ESP32 and in the host
// simulator and tests.
#pragma once

#include <cstddef>
#include <cstdint>

namespace bwa {

constexpr uint8_t kDelimiter = 0x7E;
constexpr uint8_t kMinLength = 5;      // LEN of a frame without payload
constexpr uint8_t kMaxLength = 0x7D;   // LEN below the delimiter
constexpr size_t kMaxFrame = kMaxLength + 2;

constexpr uint8_t kKindBroadcast = 0xAF;
constexpr uint8_t kKindDirect = 0xBF;

// Channels.
constexpr uint8_t kChannelWifi = 0x0A;       // the BWA Wi-Fi module; what TCP clients use
constexpr uint8_t kChannelFirstAssigned = 0x10;
constexpr uint8_t kChannelLastPolled = 0x2F; // the main board sends CTS up to here
constexpr uint8_t kChannelNewClient = 0xFE;
constexpr uint8_t kChannelBroadcast = 0xFF;

// Message types (ccutrer/balboa_worldwide_app wiki, pybalboa).
namespace type {
constexpr uint8_t kNewClientCts = 0x00;
constexpr uint8_t kChannelRequest = 0x01;
constexpr uint8_t kChannelResponse = 0x02;
constexpr uint8_t kChannelAck = 0x03;
constexpr uint8_t kExistingClientRequest = 0x04;  // also the app's "device present"
constexpr uint8_t kExistingClientResponse = 0x05;
constexpr uint8_t kClearToSend = 0x06;
constexpr uint8_t kNothingToSend = 0x07;
constexpr uint8_t kToggleItem = 0x11;
constexpr uint8_t kStatus = 0x13;
constexpr uint8_t kSetTemperature = 0x20;
constexpr uint8_t kSetTime = 0x21;
constexpr uint8_t kSettingsRequest = 0x22;
constexpr uint8_t kFilterCycles = 0x23;
constexpr uint8_t kInformation = 0x24;
constexpr uint8_t kSetupParameters = 0x25;
constexpr uint8_t kPreferences = 0x26;
constexpr uint8_t kSetPreference = 0x27;
constexpr uint8_t kFaultLog = 0x28;
constexpr uint8_t kConfiguration = 0x2E;
constexpr uint8_t kSetWifi = 0x92;
constexpr uint8_t kModuleIdentification = 0x94;
}  // namespace type

uint8_t crc8(const uint8_t* data, size_t len);

struct Frame {
  uint8_t bytes[kMaxFrame] = {};

  uint8_t length() const { return bytes[1]; }
  size_t size() const { return static_cast<size_t>(bytes[1]) + 2; }
  uint8_t channel() const { return bytes[2]; }
  uint8_t kind() const { return bytes[3]; }
  uint8_t type() const { return bytes[4]; }
  const uint8_t* payload() const { return bytes + 5; }
  size_t payload_size() const { return static_cast<size_t>(bytes[1]) - kMinLength; }
  uint8_t crc() const { return bytes[bytes[1]]; }

  bool is(uint8_t ch, uint8_t t) const { return channel() == ch && type() == t; }
  bool same_as(const Frame& other) const;
};

// Builds a frame. KIND follows the channel (0xAF on 0xFF, else 0xBF).
// Returns false when the payload does not fit.
bool make_frame(Frame& out, uint8_t channel, uint8_t type, const uint8_t* payload = nullptr,
                size_t payload_size = 0);

// Gives a frame another channel and fixes KIND and CRC.
void set_channel(Frame& frame, uint8_t channel);

// Checks delimiters, length and CRC of a complete frame in memory.
bool valid_frame(const uint8_t* data, size_t size);

// Cuts a byte stream into valid frames.
//
// Frames are found from their end: every 0x7E may close a frame, so on each
// one the parser looks back for an opening 0x7E whose length byte matches the
// distance and whose CRC is right. Noise that looks like the start of a long
// frame therefore never holds back a real frame behind it, which matters when
// a Clear to Send has to be answered within a millisecond or so. A closing
// delimiter may double as the next opening one.
class FrameParser {
 public:
  struct Stats {
    uint32_t frames = 0;
    uint32_t skipped_bytes = 0;  // bytes that were not part of any valid frame
  };

  // Feeds bytes; calls on_frame(const Frame&) for every valid frame.
  template <typename F>
  void feed(const uint8_t* data, size_t size, F&& on_frame) {
    for (size_t i = 0; i < size; ++i) {
      if (fill_ == sizeof(buf_)) drop(sizeof(buf_) - kMaxFrame);  // keep the newest frame's worth
      buf_[fill_++] = data[i];
      if (data[i] == kDelimiter && fill_ >= kMinLength + 2) {
        Frame frame;
        if (take_frame(frame)) on_frame(static_cast<const Frame&>(frame));
      }
    }
  }

  void reset() { fill_ = 0; }
  const Stats& stats() const { return stats_; }

 private:
  // Looks for a valid frame that ends with the byte just added.
  bool take_frame(Frame& out);
  void drop(size_t n);

  uint8_t buf_[kMaxFrame * 2] = {};
  size_t fill_ = 0;
  Stats stats_;
};

}  // namespace bwa
