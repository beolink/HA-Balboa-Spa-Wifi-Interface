#include "bwa_status.h"

#include <cstdio>

namespace bwa {

bool decode_status(const Frame& f, SpaStatus& s) {
  if (f.type() != type::kStatus || f.payload_size() < 21) return false;
  const uint8_t* p = f.payload();
  s.valid = true;
  s.priming = (p[1] & 0x01) != 0;
  s.celsius = (p[9] & 0x01) != 0;
  const float scale = s.celsius ? 2.0f : 1.0f;
  s.water = p[2] == 0xFF ? -1000 : p[2] / scale;
  s.target = p[20] / scale;
  s.hour = p[3];
  s.minute = p[4];
  s.heat_mode = p[5] & 0x03;
  s.high_range = (p[10] & 0x04) != 0;
  s.heating = (p[10] >> 4) & 0x03;
  for (int i = 0; i < 3; ++i) s.pumps[i] = (p[11] >> (2 * i)) & 0x03;
  s.blower = (p[13] >> 2) & 0x03;
  s.light = (p[14] & 0x03) == 0x03;
  return true;
}

bool decode_information(const Frame& f, SpaInfo& info) {
  if (f.type() != type::kInformation || f.payload_size() < 12) return false;
  const uint8_t* p = f.payload();
  for (int i = 0; i < 8; ++i) {
    const char c = static_cast<char>(p[4 + i]);
    info.model[i] = (c >= 0x20 && c < 0x7F) ? c : '?';
  }
  info.model[8] = '\0';
  for (int i = 7; i >= 0 && info.model[i] == ' '; --i) info.model[i] = '\0';
  std::snprintf(info.software, sizeof(info.software), "M%u_%u V%u", p[0], p[1], p[2]);
  info.valid = true;
  return true;
}

}  // namespace bwa
