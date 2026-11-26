#include "bwa_frame.h"

#include <cstring>

namespace bwa {

uint8_t crc8(const uint8_t* data, size_t len) {
  uint8_t crc = 0x02;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x07) : static_cast<uint8_t>(crc << 1);
    }
  }
  return crc ^ 0x02;
}

bool Frame::same_as(const Frame& other) const {
  return length() == other.length() && std::memcmp(bytes, other.bytes, size()) == 0;
}

bool make_frame(Frame& out, uint8_t channel, uint8_t t, const uint8_t* payload,
                size_t payload_size) {
  if (payload_size > kMaxLength - kMinLength) return false;
  const uint8_t len = static_cast<uint8_t>(kMinLength + payload_size);
  out.bytes[0] = kDelimiter;
  out.bytes[1] = len;
  out.bytes[2] = channel;
  out.bytes[3] = channel == kChannelBroadcast ? kKindBroadcast : kKindDirect;
  out.bytes[4] = t;
  if (payload_size) std::memcpy(out.bytes + 5, payload, payload_size);
  out.bytes[len] = crc8(out.bytes + 1, len - 1);
  out.bytes[len + 1] = kDelimiter;
  return true;
}

void set_channel(Frame& frame, uint8_t channel) {
  frame.bytes[2] = channel;
  frame.bytes[3] = channel == kChannelBroadcast ? kKindBroadcast : kKindDirect;
  frame.bytes[frame.length()] = crc8(frame.bytes + 1, frame.length() - 1);
}

bool valid_frame(const uint8_t* data, size_t size) {
  if (size < kMinLength + 2 || data[0] != kDelimiter) return false;
  const uint8_t len = data[1];
  if (len < kMinLength || len > kMaxLength || size != static_cast<size_t>(len) + 2) return false;
  if (data[size - 1] != kDelimiter) return false;
  return crc8(data + 1, len - 1) == data[len];
}

bool FrameParser::take_frame(Frame& out) {
  const size_t end = fill_ - 1;  // index of the closing delimiter
  // Oldest candidate first, so a frame is not mistaken for its own tail.
  const size_t first = end >= kMaxFrame - 1 ? end - (kMaxFrame - 1) : 0;
  for (size_t s = first; s + kMinLength + 1 <= end; ++s) {
    if (buf_[s] != kDelimiter) continue;
    const size_t len = end - s - 1;
    if (buf_[s + 1] != len || len < kMinLength || len > kMaxLength) continue;
    if (crc8(buf_ + s + 1, len - 1) != buf_[s + len]) continue;
    std::memcpy(out.bytes, buf_ + s, len + 2);
    ++stats_.frames;
    stats_.skipped_bytes += static_cast<uint32_t>(s);
    // Keep the closing delimiter: it may open the next frame.
    const size_t consumed = end;  // bytes before the closing delimiter
    std::memmove(buf_, buf_ + consumed, fill_ - consumed);
    fill_ -= consumed;
    return true;
  }
  return false;
}

void FrameParser::drop(size_t n) {
  if (n >= fill_) {
    stats_.skipped_bytes += static_cast<uint32_t>(fill_);
    fill_ = 0;
    return;
  }
  stats_.skipped_bytes += static_cast<uint32_t>(n);
  std::memmove(buf_, buf_ + n, fill_ - n);
  fill_ -= n;
}

}  // namespace bwa
