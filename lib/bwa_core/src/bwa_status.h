// Just enough decoding for the bridge's own status page. The integration
// (pybalboa) does the real decoding; the bridge passes frames on untouched.
#pragma once

#include <cstddef>
#include <cstdint>

#include "bwa_frame.h"

namespace bwa {

struct SpaStatus {
  bool valid = false;
  bool celsius = true;
  float water = -1000;    // in the spa's unit; -1000 = unknown (sensor not read yet)
  float target = -1000;
  uint8_t hour = 0, minute = 0;
  uint8_t heat_mode = 0;  // 0 ready, 1 rest, 3 ready in rest
  bool high_range = false;
  uint8_t heating = 0;    // 0 off, 1 heating, 2 waiting
  uint8_t pumps[3] = {0, 0, 0};
  uint8_t blower = 0;
  bool light = false;
  bool priming = false;
};

struct SpaInfo {
  bool valid = false;
  char model[9] = {};      // e.g. "BP2100G1"
  char software[16] = {};  // e.g. "M100_220 V20"
};

bool decode_status(const Frame& frame, SpaStatus& out);
bool decode_information(const Frame& frame, SpaInfo& out);

}  // namespace bwa
