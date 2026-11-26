// Settings kept in NVS (survive power loss and firmware updates).
#pragma once

#include <Arduino.h>

#include "bwa_bus.h"

struct Settings {
  String wifi_ssid;
  String wifi_pass;
  String hostname = "spa-bridge";
  String admin_pass;  // web settings, firmware upload and OTA; empty = open
  bwa::BusMode mode = bwa::BusMode::kListenOnly;
  uint8_t mac[6] = {0x00, 0x15, 0x27, 0x00, 0x00, 0x01};  // what clients are told
  uint32_t status_interval_ms = 1000;
  int8_t rx_inverted = -1;  // -1 not known yet (auto), 0 normal, 1 inverted

  void load();
  void save() const;
};

bool parse_mac(const String& text, uint8_t out[6]);
String format_mac(const uint8_t mac[6]);
