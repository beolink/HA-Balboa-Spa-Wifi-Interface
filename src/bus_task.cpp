#include "bus_task.h"

#include <driver/uart.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>

#include <atomic>

namespace {

constexpr uart_port_t kUart = UART_NUM_1;
constexpr uint32_t kRtcMagic = 0xB5A1C0DEu;
constexpr uint32_t kPolarityWindowMs = 4000;

// Survives a software reset (OTA, watchdog, the reboot button), not a power
// loss. That matches the main board, which forgets its channels only when it
// loses power itself.
RTC_NOINIT_ATTR uint32_t rtc_magic;
RTC_NOINIT_ATTR uint8_t rtc_channel;

bwa::SpscRing<BusLogEntry, 64> log_ring;

void log_frame(const bwa::Frame& f, bool tx) {
  switch (f.type()) {
    case bwa::type::kNewClientCts:
    case bwa::type::kClearToSend:
    case bwa::type::kNothingToSend:
    case bwa::type::kStatus:
      return;
    default:
      break;
  }
  BusLogEntry e;
  e.ms = millis();
  e.tx = tx;
  e.frame = f;
  log_ring.push(e);  // a full log just loses entries
}

class UartBus : public bwa::BusIo {
 public:
  bwa::UpRing* up = nullptr;
  int64_t read_us = 0;
  std::atomic<uint32_t> answer_last{0}, answer_max{0}, up_dropped{0};

  void transmit(const bwa::Frame& f) override {
    uart_write_bytes(kUart, reinterpret_cast<const char*>(f.bytes), f.size());
    const uint32_t us = static_cast<uint32_t>(esp_timer_get_time() - read_us);
    answer_last.store(us);
    if (us > answer_max.load()) answer_max.store(us);
    log_frame(f, true);
  }
  void upstream(const bwa::Frame& f, bool cache_only) override {
    bwa::UpFrame u;
    u.frame = f;
    u.cache_only = cache_only;
    if (!up->push(u)) ++up_dropped;
  }
  void channel_changed(uint8_t ch) override {
    rtc_channel = ch;
    rtc_magic = kRtcMagic;
  }
};

UartBus io;
bwa::BusClient::DownRing* down_ring = nullptr;
bwa::BusClient* client = nullptr;
QueueHandle_t uart_queue = nullptr;
bwa::FrameParser parser;

portMUX_TYPE settings_mux = portMUX_INITIALIZER_UNLOCKED;
BusSettings current{};
BusSettings pending{};
std::atomic<bool> reconfigure_pending{false};

std::atomic<bool> inverted{false};
bool auto_polarity = false;
std::atomic<int8_t> found_polarity{-1};
std::atomic<uint32_t> bytes_read{0}, overflows{0};

uint8_t remembered_channel() {
  if (rtc_magic != kRtcMagic) return 0;
  return rtc_channel;
}

bwa::BusConfig make_config(const BusSettings& s, uint8_t previous) {
  bwa::BusConfig c;
  c.mode = s.mode;
  c.client_hash = s.client_hash;
  c.previous_channel = previous;
  // An inverted receiver means A and B are swapped. Inverting the
  // transmitter too would make an auto-direction transceiver drive the bus
  // all the time, so the bridge only listens until the wires are swapped.
  c.tx_allowed = !inverted.load();
  return c;
}

void set_polarity(bool inv) {
  inverted.store(inv);
  uart_set_line_inverse(kUart, inv ? UART_SIGNAL_RXD_INV : UART_SIGNAL_INV_DISABLE);
  parser.reset();
  client->configure(make_config(current, remembered_channel()));
}

void bus_task(void*) {
  esp_task_wdt_add(nullptr);
  uint8_t buf[256];
  uint32_t window_start = millis();
  uint32_t window_bytes = 0;
  uint32_t frames_before = 0;

  for (;;) {
    esp_task_wdt_reset();

    if (reconfigure_pending.exchange(false)) {
      taskENTER_CRITICAL(&settings_mux);
      const BusSettings next = pending;
      taskEXIT_CRITICAL(&settings_mux);
      const uint8_t keep = client->channel() ? client->channel() : remembered_channel();
      current = next;
      if (next.rx_inverted >= 0 && (next.rx_inverted == 1) != inverted.load()) {
        set_polarity(next.rx_inverted == 1);
      } else {
        client->configure(make_config(current, keep));
      }
      auto_polarity = next.rx_inverted < 0;
    }

    uart_event_t ev;
    if (xQueueReceive(uart_queue, &ev, pdMS_TO_TICKS(20)) == pdTRUE) {
      if (ev.type == UART_DATA) {
        size_t avail = 0;
        uart_get_buffered_data_len(kUart, &avail);
        while (avail > 0) {
          const int n = uart_read_bytes(kUart, buf, avail < sizeof(buf) ? avail : sizeof(buf), 0);
          if (n <= 0) break;
          bytes_read += n;
          io.read_us = esp_timer_get_time();
          const uint32_t now = millis();
          parser.feed(buf, static_cast<size_t>(n),
                      [now](const bwa::Frame& f) {
                        log_frame(f, false);  // before our answer to it
                        client->on_frame(f, now);
                      });
          uart_get_buffered_data_len(kUart, &avail);
        }
      } else if (ev.type == UART_FIFO_OVF || ev.type == UART_BUFFER_FULL) {
        uart_flush_input(kUart);
        xQueueReset(uart_queue);
        parser.reset();
        ++overflows;
      }
    }

    const uint32_t now = millis();
    client->tick(now);

    // Find the receiver's polarity: bytes but no valid frame for a while
    // means A and B are probably swapped.
    if (auto_polarity && found_polarity.load() < 0) {
      if (parser.stats().frames > frames_before) {
        found_polarity.store(inverted.load() ? 1 : 0);
      } else if (now - window_start > kPolarityWindowMs) {
        if (bytes_read.load() - window_bytes > 200) set_polarity(!inverted.load());
        window_start = now;
        window_bytes = bytes_read.load();
        frames_before = parser.stats().frames;
      }
    }
  }
}

}  // namespace

void bus_task_start(const BusPins& pins, const BusSettings& settings, bwa::UpRing& up,
                    bwa::BusClient::DownRing& down) {
  if (esp_reset_reason() == ESP_RST_POWERON) rtc_magic = 0;
  io.up = &up;
  down_ring = &down;
  client = new bwa::BusClient(io, down);
  current = settings;
  auto_polarity = settings.rx_inverted < 0;
  inverted.store(settings.rx_inverted == 1);

  uart_config_t cfg = {};
  cfg.baud_rate = 115200;
  cfg.data_bits = UART_DATA_8_BITS;
  cfg.parity = UART_PARITY_DISABLE;
  cfg.stop_bits = UART_STOP_BITS_1;
  cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  cfg.source_clk = UART_SCLK_APB;
  uart_driver_install(kUart, 2048, 0, 32, &uart_queue, 0);
  uart_param_config(kUart, &cfg);
  uart_set_pin(kUart, pins.tx, pins.rx, pins.de >= 0 ? pins.de : UART_PIN_NO_CHANGE,
               UART_PIN_NO_CHANGE);
  if (pins.de >= 0) uart_set_mode(kUart, UART_MODE_RS485_HALF_DUPLEX);
  // Hand bytes over 2 byte-times after the line goes quiet: a CTS is read
  // ~0.2 ms after its last byte, not when 120 bytes have piled up.
  uart_set_rx_timeout(kUart, 2);
  uart_set_rx_full_threshold(kUart, 64);
  uart_set_line_inverse(kUart, inverted.load() ? UART_SIGNAL_RXD_INV : UART_SIGNAL_INV_DISABLE);

  client->configure(make_config(current, remembered_channel()));
  xTaskCreatePinnedToCore(bus_task, "bus", 4096, nullptr, configMAX_PRIORITIES - 2, nullptr, 1);
}

void bus_task_reconfigure(const BusSettings& settings) {
  taskENTER_CRITICAL(&settings_mux);
  pending = settings;
  taskEXIT_CRITICAL(&settings_mux);
  reconfigure_pending.store(true);
}

bool bus_online() { return client && client->online(); }

const bwa::BusStats& bus_stats() { return client->stats(); }

int8_t bus_found_polarity() { return found_polarity.load(); }

bool bus_log_pop(BusLogEntry& out) { return log_ring.pop(out); }

BusReport bus_report() {
  BusReport r{};
  if (!client) return r;
  const bwa::BusStats& s = client->stats();
  r.state = client->state();
  r.mode = client->mode();
  r.channel = client->channel();
  r.rx_inverted = inverted.load();
  r.tx_blocked = inverted.load();
  r.bytes = bytes_read.load();
  r.frames = s.frames.load();
  r.skipped = parser.stats().skipped_bytes;
  r.overflows = overflows.load();
  r.last_frame_age_ms = r.frames ? millis() - s.last_frame_ms.load() : UINT32_MAX;
  r.answer_us_last = io.answer_last.load();
  r.answer_us_max = io.answer_max.load();
  r.up_dropped = io.up_dropped.load();
  return r;
}
