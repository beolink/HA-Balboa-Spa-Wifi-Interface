// Balboa Spa Wifi Interface: an ESP32 on the spa's RS-485 bus that stands
// in for the BWA Wi-Fi module on TCP 4257 (and UDP discovery on 30303), for
// several clients at once.
//
// Tasks:
//   bus  (core 1, high priority)  UART, BusClient: answers CTS at once
//   net  (core 0)                 sockets, Hub
//   loop (core 1, low priority)   web page, OTA, Wi-Fi upkeep, status LED
#include <Arduino.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_task_wdt.h>

#include "bus_task.h"
#include "bwa_hub.h"
#include "bwa_net.h"
#include "ota_guard.h"
#include "settings.h"
#include "version.h"
#include "web.h"

#ifndef BUS_RX_PIN
#error "Set BUS_RX_PIN, BUS_TX_PIN (and BUS_DE_PIN, STATUS_LED_PIN) in platformio.ini"
#endif
#ifndef BUS_DE_PIN
#define BUS_DE_PIN -1
#endif
#ifndef STATUS_LED_PIN
#define STATUS_LED_PIN -1
#endif
#ifndef AP_PASSWORD
#define AP_PASSWORD "spabridge"
#endif

namespace {

Settings settings;
bwa::UpRing up_ring;
bwa::BusClient::DownRing down_ring;
bwa::Hub hub;
bwa::NetServer net(hub, down_ring, up_ring);

uint32_t wifi_lost_since = 0;
bool ap_on = false;
bool net_started = false;

void net_task(void*) {
  esp_task_wdt_add(nullptr);
  for (;;) {
    esp_task_wdt_reset();
    if (!net_started && WiFi.isConnected()) net_started = net.start(4257, 30303);
    if (net_started) {
      net.poll(10, bus_online(), millis());
    } else {
      bwa::UpFrame drop;
      while (up_ring.pop(drop)) {
      }
      vTaskDelay(pdMS_TO_TICKS(50));
    }
  }
}

void start_ap() {
  if (ap_on) return;
  uint8_t m[6];
  WiFi.macAddress(m);
  char name[24];
  snprintf(name, sizeof(name), "SpaBridge-%02X%02X", m[4], m[5]);
  WiFi.mode(settings.wifi_ssid.isEmpty() ? WIFI_AP : WIFI_AP_STA);
  WiFi.softAP(name, AP_PASSWORD);
  ap_on = true;
  Serial.printf("access point %s at %s\n", name, WiFi.softAPIP().toString().c_str());
}

void stop_ap() {
  if (!ap_on) return;
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  ap_on = false;
}

void start_wifi() {
  WiFi.persistent(false);
  WiFi.setHostname(settings.hostname.c_str());
  if (settings.wifi_ssid.isEmpty()) {
    start_ap();
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // no power-save latency on the TCP side
  WiFi.setAutoReconnect(true);
  WiFi.begin(settings.wifi_ssid.c_str(), settings.wifi_pass.c_str());
}

// Without the network for a minute: open the access point so the bridge can
// be reached and set up. Back on the network: close it again.
void keep_wifi() {
  const uint32_t now = millis();
  if (WiFi.isConnected()) {
    wifi_lost_since = 0;
    if (ap_on && WiFi.softAPgetStationNum() == 0) stop_ap();
    return;
  }
  if (wifi_lost_since == 0) wifi_lost_since = now ? now : 1;
  if (now - wifi_lost_since > 60000) start_ap();
  // A long outage: start over (the bus channel is kept across a restart).
  if (!settings.wifi_ssid.isEmpty() && now - wifi_lost_since > 15UL * 60 * 1000 &&
      WiFi.softAPgetStationNum() == 0) {
    ESP.restart();
  }
}

void show_led() {
  if (STATUS_LED_PIN < 0) return;
  static uint32_t last = 0;
  const uint32_t now = millis();
  if (now - last < 250) return;
  last = now;
  const BusReport b = bus_report();
  uint8_t r = 0, g = 0, bl = 0;
  if (b.last_frame_age_ms > 5000) {
    r = (now / 500) % 2 ? 24 : 0;  // blinking red: no bus
  } else if (b.rx_inverted || b.state == bwa::BusState::kGaveUp) {
    r = 24;
  } else if (b.state == bwa::BusState::kOnline) {
    g = 16;
  } else if (b.state == bwa::BusState::kListening) {
    bl = 20;
  } else {
    r = 16;
    g = 10;  // yellow: getting a channel
  }
  neopixelWrite(STATUS_LED_PIN, r, g, bl);
}

void save_found_polarity() {
  const int8_t found = bus_found_polarity();
  if (found >= 0 && settings.rx_inverted < 0) {
    settings.rx_inverted = found;
    settings.save();
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  ota_guard_begin();
  settings.load();
  Serial.printf("\nspa bridge %s on %s, mode %s\n", FIRMWARE_VERSION, BOARD_NAME,
                bwa::mode_name(settings.mode));

  bwa::HubConfig hub_config;
  memcpy(hub_config.mac, settings.mac, 6);
  hub_config.status_interval_ms = settings.status_interval_ms;
  hub.configure(hub_config);

  BusSettings bus_settings;
  bus_settings.mode = settings.mode;
  bus_settings.rx_inverted = settings.rx_inverted;
  bus_settings.client_hash = static_cast<uint16_t>((settings.mac[4] << 8) | settings.mac[5]);
  bus_task_start({BUS_RX_PIN, BUS_TX_PIN, BUS_DE_PIN}, bus_settings, up_ring, down_ring);

  start_wifi();
  xTaskCreatePinnedToCore(net_task, "net", 6144, nullptr, 5, nullptr, 0);
  web_start(settings, hub, net);

  // Starting another update proves this firmware works: keep it first.
  ArduinoOTA.onStart([] { ota_guard_confirm("new update starting"); });
  ArduinoOTA.setHostname(settings.hostname.c_str());
  if (!settings.admin_pass.isEmpty()) ArduinoOTA.setPassword(settings.admin_pass.c_str());
  ArduinoOTA.begin();
  if (MDNS.begin(settings.hostname.c_str())) MDNS.addService("http", "tcp", 80);
}

void loop() {
  web_loop();
  ArduinoOTA.handle();
  keep_wifi();
  // Reachable: on the network, or someone is on the bridge's own access point.
  ota_guard_loop(WiFi.isConnected() || (ap_on && WiFi.softAPgetStationNum() > 0));
  show_led();
  save_found_polarity();
  delay(2);
}
