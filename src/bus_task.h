// The UART side: a high-priority task that reads the RS-485 bus, runs the
// BusClient and answers a Clear to Send from inside the same call.
#pragma once

#include <Arduino.h>

#include "bwa_bus.h"
#include "bwa_hub.h"

struct BusPins {
  int rx;
  int tx;
  int de;  // driver enable for transceivers without auto direction; -1 = none
};

struct BusSettings {
  bwa::BusMode mode;
  int8_t rx_inverted;  // -1 auto, 0 normal, 1 inverted
  uint16_t client_hash;
};

struct BusReport {
  bwa::BusState state;
  bwa::BusMode mode;
  uint8_t channel;
  bool rx_inverted;
  bool tx_blocked;        // writing blocked (inverted receiver)
  uint32_t bytes;         // raw bytes read
  uint32_t frames;
  uint32_t skipped;       // bytes outside any valid frame
  uint32_t overflows;     // UART buffer overruns
  uint32_t last_frame_age_ms;  // UINT32_MAX = never
  uint32_t answer_us_last;     // read of a CTS -> our answer queued, microseconds
  uint32_t answer_us_max;
  uint32_t up_dropped;
};

void bus_task_start(const BusPins& pins, const BusSettings& settings, bwa::UpRing& up,
                    bwa::BusClient::DownRing& down);
// Applied by the bus task itself at its next turn.
void bus_task_reconfigure(const BusSettings& settings);
bool bus_online();
BusReport bus_report();
const bwa::BusStats& bus_stats();
// Set once the receiver's polarity has been found (to be saved); -1 = not yet.
int8_t bus_found_polarity();

// Bus traffic for the web page's log, without the frequent flow-control
// frames (CTS, Nothing to Send, New Client CTS) and status updates.
struct BusLogEntry {
  uint32_t ms;
  bool tx;  // written by the bridge
  bwa::Frame frame;
};
// One consumer only (the web task).
bool bus_log_pop(BusLogEntry& out);
