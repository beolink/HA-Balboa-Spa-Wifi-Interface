// When a freshly updated firmware counts as good.
//
// After an update over Wi-Fi the bootloader starts the new firmware "on
// trial". It stays the firmware only once it confirms itself; if it crashes,
// hangs (watchdog) or restarts before that, the bootloader goes back to the
// previous one. The firmware confirms itself once it has been on the network
// for a while, and gives up (goes back) if it never gets there, so a version
// that cannot be reached never needs a visit with a USB cable.
//
// Plain logic, so it can be tested on a desktop; ota_guard.cpp does the rest.
#pragma once

#include <cstdint>

namespace bwa {

enum class OtaAction : uint8_t { kNone = 0, kConfirm = 1, kRollBack = 2 };

struct OtaPolicy {
  uint32_t online_for_ms = 60000;   // on the network this long without a break
  uint32_t min_uptime_ms = 90000;   // and up at least this long
  uint32_t give_up_ms = 600000;     // still not online by then: go back

  // Call regularly with whether the station is on the network.
  OtaAction step(bool online, uint32_t now_ms) {
    if (online) {
      if (online_since_ == 0) online_since_ = now_ms ? now_ms : 1;
      if (now_ms - online_since_ >= online_for_ms && now_ms >= min_uptime_ms) {
        return OtaAction::kConfirm;
      }
      return OtaAction::kNone;  // online: let it finish proving itself
    }
    online_since_ = 0;
    return now_ms >= give_up_ms ? OtaAction::kRollBack : OtaAction::kNone;
  }

 private:
  uint32_t online_since_ = 0;
};

}  // namespace bwa
