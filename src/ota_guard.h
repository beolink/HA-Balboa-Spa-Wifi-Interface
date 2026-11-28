// Safe updates over Wi-Fi: a new firmware stays only once it has shown it can
// be reached; otherwise the bootloader goes back to the previous one. See
// lib/bwa_core/src/ota_policy.h.
#pragma once

#include <Arduino.h>

void ota_guard_begin();
// Call from loop(); online = the station is on the network.
void ota_guard_loop(bool online);
// The firmware has shown it works (someone reached its web page, or a new
// update is starting): keep it, even if the minute online is not up yet.
void ota_guard_confirm(const char* why);

struct OtaReport {
  bool on_trial;          // running a new update that has not confirmed itself yet
  uint32_t trial_left_s;  // until it gives up and goes back (if never online)
  bool rolled_back;       // the last update did not stay; this is the one before it
  const char* confirmed_by;  // why the running firmware was kept ("" = not this boot)
};
OtaReport ota_guard_report();
