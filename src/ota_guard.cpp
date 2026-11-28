#include "ota_guard.h"

#include <esp_ota_ops.h>

#include "ota_policy.h"

// The Arduino core marks a new firmware as good as soon as it starts, unless
// this returns true. Then it is left to us (and to the bootloader's rollback,
// which this core's bootloader has enabled).
extern "C" bool verifyRollbackLater() { return true; }

namespace {

bool on_trial = false;
bool rolled_back = false;
const char* confirmed_by = "";
bwa::OtaPolicy policy;

}  // namespace

void ota_guard_begin() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  on_trial = running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
             state == ESP_OTA_IMG_PENDING_VERIFY;
  rolled_back = esp_ota_get_last_invalid_partition() != nullptr;
  if (on_trial) {
    Serial.println("update on trial: kept once online for a minute, else rolled back");
  }
  if (rolled_back) Serial.println("the last update did not stay; running the one before it");
}

void ota_guard_confirm(const char* why) {
  if (!on_trial) return;
  if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
    on_trial = false;
    confirmed_by = why;
    Serial.printf("update kept (%s)\n", why);
  }
}

void ota_guard_loop(bool online) {
  if (!on_trial) return;
  switch (policy.step(online, millis())) {
    case bwa::OtaAction::kConfirm:
      ota_guard_confirm("online");
      break;
    case bwa::OtaAction::kRollBack:
      if (esp_ota_check_rollback_is_possible()) {
        Serial.println("update never got online: going back to the previous firmware");
        Serial.flush();
        esp_ota_mark_app_invalid_rollback_and_reboot();  // does not return
      }
      // Nothing to go back to: keep this one rather than leave the board without firmware.
      ota_guard_confirm("nothing older to go back to");
      break;
    case bwa::OtaAction::kNone:
      break;
  }
}

OtaReport ota_guard_report() {
  OtaReport r{};
  r.on_trial = on_trial;
  const uint32_t up = millis();
  r.trial_left_s = on_trial && up < policy.give_up_ms ? (policy.give_up_ms - up) / 1000 : 0;
  r.rolled_back = rolled_back;
  r.confirmed_by = confirmed_by;
  return r;
}
