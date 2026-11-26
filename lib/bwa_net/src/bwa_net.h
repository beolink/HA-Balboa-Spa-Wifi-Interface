// TCP port 4257 and UDP discovery on 30303, with plain BSD sockets so the
// same code runs on the ESP32 (lwIP) and on a desktop (the simulator).
//
// One task calls poll() in a loop. It owns the sockets and the Hub; frames
// from the bus task arrive through the UpRing, frames for the bus leave
// through the DownRing.
#pragma once

#include <cstddef>
#include <cstdint>

#include "bwa_bus.h"
#include "bwa_hub.h"

namespace bwa {

class NetServer : public HubIo {
 public:
  NetServer(Hub& hub, BusClient::DownRing& down, UpRing& up) : hub_(hub), down_(down), up_(up) {
    hub_.attach(this);
  }
  ~NetServer() override { stop(); }

  // Ports 0 = any free port (the simulator). loopback_only binds 127.0.0.1.
  bool start(uint16_t tcp_port, uint16_t udp_port, bool loopback_only = false);
  void stop();

  // One round: bus frames to the clients, new connections, client bytes,
  // discovery. Waits at most timeout_ms for something to happen.
  void poll(int timeout_ms, bool bus_online, uint32_t now_ms);

  uint16_t tcp_port() const { return tcp_port_; }
  uint16_t udp_port() const { return udp_port_; }
  int clients() const;
  uint32_t refused() const { return refused_; }
  uint32_t slow_drops() const { return slow_drops_; }
  uint32_t discoveries() const { return discoveries_; }

  // HubIo
  void send_to_client(int slot, const uint8_t* data, size_t size) override;
  bool to_bus(const Frame& frame) override { return down_.push(frame); }

 private:
  struct Conn {
    int fd = -1;
    uint8_t out[2048];
    size_t out_len = 0;
  };
  void close_conn(int slot);
  void flush(int slot);
  void accept_one(uint32_t now_ms);
  void answer_discovery();

  Hub& hub_;
  BusClient::DownRing& down_;
  UpRing& up_;
  int listen_fd_ = -1;
  int udp_fd_ = -1;
  uint16_t tcp_port_ = 0;
  uint16_t udp_port_ = 0;
  Conn conns_[Hub::kMaxClients];
  uint32_t refused_ = 0;
  uint32_t slow_drops_ = 0;
  uint32_t discoveries_ = 0;
};

}  // namespace bwa
