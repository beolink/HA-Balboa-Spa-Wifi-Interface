#include "settings.h"

#include <Preferences.h>
#include <esp_mac.h>

namespace {
constexpr const char* kNamespace = "bridge";
}

bool parse_mac(const String& text, uint8_t out[6]) {
  unsigned v[6];
  if (sscanf(text.c_str(), "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6 &&
      sscanf(text.c_str(), "%x-%x-%x-%x-%x-%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
    return false;
  }
  for (int i = 0; i < 6; ++i) {
    if (v[i] > 0xFF) return false;
    out[i] = static_cast<uint8_t>(v[i]);
  }
  return true;
}

String format_mac(const uint8_t m[6]) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
  return buf;
}

void Settings::load() {
  // Default MAC: Balboa's OUI and this board's own last three bytes, the way
  // other gateways do it, so clients that look for 00:15:27 find it.
  uint8_t own[6];
  esp_read_mac(own, ESP_MAC_WIFI_STA);
  mac[3] = own[3];
  mac[4] = own[4];
  mac[5] = own[5];

  Preferences p;
  if (!p.begin(kNamespace, true)) return;  // nothing saved yet
  wifi_ssid = p.getString("ssid", wifi_ssid);
  wifi_pass = p.getString("pass", wifi_pass);
  hostname = p.getString("host", hostname);
  admin_pass = p.getString("admin", admin_pass);
  bwa::BusMode m;
  if (bwa::parse_mode(p.getString("mode", bwa::mode_name(mode)).c_str(), m)) mode = m;
  uint8_t saved[6];
  if (p.getBytes("mac", saved, 6) == 6) memcpy(mac, saved, 6);
  status_interval_ms = p.getUInt("status_ms", status_interval_ms);
  rx_inverted = p.getChar("rx_inv", rx_inverted);
  p.end();
}

void Settings::save() const {
  Preferences p;
  if (!p.begin(kNamespace, false)) return;
  p.putString("ssid", wifi_ssid);
  p.putString("pass", wifi_pass);
  p.putString("host", hostname);
  p.putString("admin", admin_pass);
  p.putString("mode", bwa::mode_name(mode));
  p.putBytes("mac", mac, 6);
  p.putUInt("status_ms", status_interval_ms);
  p.putChar("rx_inv", rx_inverted);
  p.end();
}
