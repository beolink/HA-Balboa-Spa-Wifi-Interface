#include "bwa_net.h"

#include <cerrno>
#include <cstring>

#if defined(ESP_PLATFORM)
#include <fcntl.h>
#include <lwip/sockets.h>
#include <unistd.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace bwa {

namespace {

bool set_nonblocking(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

void no_sigpipe(int fd) {
#ifdef SO_NOSIGPIPE
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
  (void)fd;
#endif
}

uint16_t bound_port(int fd) {
  sockaddr_in addr{};
  socklen_t len = sizeof(addr);
  if (getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) return 0;
  return ntohs(addr.sin_port);
}

int open_socket(int kind, uint16_t port, bool loopback_only) {
  const int fd = socket(AF_INET, kind, 0);
  if (fd < 0) return -1;
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(loopback_only ? INADDR_LOOPBACK : INADDR_ANY);
  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || !set_nonblocking(fd)) {
    close(fd);
    return -1;
  }
  return fd;
}

}  // namespace

bool NetServer::start(uint16_t tcp_port, uint16_t udp_port, bool loopback_only) {
  stop();
  listen_fd_ = open_socket(SOCK_STREAM, tcp_port, loopback_only);
  if (listen_fd_ < 0 || listen(listen_fd_, 2) != 0) {
    stop();
    return false;
  }
  udp_fd_ = open_socket(SOCK_DGRAM, udp_port, loopback_only);
  if (udp_fd_ < 0) {
    stop();
    return false;
  }
  tcp_port_ = bound_port(listen_fd_);
  udp_port_ = bound_port(udp_fd_);
  return true;
}

void NetServer::stop() {
  for (int i = 0; i < Hub::kMaxClients; ++i) close_conn(i);
  if (listen_fd_ >= 0) close(listen_fd_);
  if (udp_fd_ >= 0) close(udp_fd_);
  listen_fd_ = udp_fd_ = -1;
}

int NetServer::clients() const {
  int n = 0;
  for (const Conn& c : conns_) n += c.fd >= 0 ? 1 : 0;
  return n;
}

void NetServer::close_conn(int slot) {
  Conn& c = conns_[slot];
  if (c.fd < 0) return;
  close(c.fd);
  c.fd = -1;
  c.out_len = 0;
  hub_.client_closed(slot);
}

void NetServer::send_to_client(int slot, const uint8_t* data, size_t size) {
  if (slot < 0 || slot >= Hub::kMaxClients) return;
  Conn& c = conns_[slot];
  if (c.fd < 0) return;
  size_t sent = 0;
  if (c.out_len == 0) {
    const ssize_t n = send(c.fd, data, size, MSG_NOSIGNAL);
    if (n > 0) {
      sent = static_cast<size_t>(n);
    } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
      close_conn(slot);
      return;
    }
  }
  const size_t rest = size - sent;
  if (rest == 0) return;
  if (c.out_len + rest > sizeof(c.out)) {
    // A client that cannot keep up with ~3 frames a second is gone; let it
    // reconnect rather than hand it half a frame.
    ++slow_drops_;
    close_conn(slot);
    return;
  }
  std::memcpy(c.out + c.out_len, data + sent, rest);
  c.out_len += rest;
}

void NetServer::flush(int slot) {
  Conn& c = conns_[slot];
  if (c.fd < 0 || c.out_len == 0) return;
  const ssize_t n = send(c.fd, c.out, c.out_len, MSG_NOSIGNAL);
  if (n > 0) {
    std::memmove(c.out, c.out + n, c.out_len - static_cast<size_t>(n));
    c.out_len -= static_cast<size_t>(n);
  } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
    close_conn(slot);
  }
}

void NetServer::accept_one(uint32_t /*now_ms*/) {
  sockaddr_in peer{};
  socklen_t len = sizeof(peer);
  const int fd = accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &len);
  if (fd < 0) return;
  int slot = -1;
  for (int i = 0; i < Hub::kMaxClients; ++i) {
    if (conns_[i].fd < 0) {
      slot = i;
      break;
    }
  }
  if (slot < 0 || !set_nonblocking(fd)) {
    ++refused_;
    close(fd);
    return;
  }
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  no_sigpipe(fd);
  conns_[slot].fd = fd;
  conns_[slot].out_len = 0;
  hub_.client_opened(slot);
}

void NetServer::answer_discovery() {
  uint8_t buf[128];
  sockaddr_in peer{};
  socklen_t len = sizeof(peer);
  const ssize_t n = recvfrom(udp_fd_, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&peer), &len);
  if (n < 0) return;
  char reply[64];
  const size_t size = hub_.discovery_reply(reply, sizeof(reply));
  sendto(udp_fd_, reply, size, 0, reinterpret_cast<sockaddr*>(&peer), len);
  ++discoveries_;
}

void NetServer::poll(int timeout_ms, bool bus_online, uint32_t now_ms) {
  UpFrame up;
  while (up_.pop(up)) hub_.bus_frame(up.frame, up.cache_only, now_ms);

  if (listen_fd_ < 0) return;
  fd_set readable, writable;
  FD_ZERO(&readable);
  FD_ZERO(&writable);
  int top = listen_fd_ > udp_fd_ ? listen_fd_ : udp_fd_;
  FD_SET(listen_fd_, &readable);
  FD_SET(udp_fd_, &readable);
  for (const Conn& c : conns_) {
    if (c.fd < 0) continue;
    FD_SET(c.fd, &readable);
    if (c.out_len) FD_SET(c.fd, &writable);
    if (c.fd > top) top = c.fd;
  }
  timeval tv{};
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  const int ready = select(top + 1, &readable, &writable, nullptr, &tv);
  if (ready <= 0) return;

  if (FD_ISSET(listen_fd_, &readable)) accept_one(now_ms);
  if (FD_ISSET(udp_fd_, &readable)) answer_discovery();
  for (int i = 0; i < Hub::kMaxClients; ++i) {
    const int fd = conns_[i].fd;
    if (fd < 0) continue;
    if (FD_ISSET(fd, &writable)) flush(i);
    if (conns_[i].fd < 0 || !FD_ISSET(fd, &readable)) continue;
    uint8_t buf[256];
    const ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n > 0) {
      hub_.client_bytes(i, buf, static_cast<size_t>(n), bus_online, now_ms);
    } else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
      close_conn(i);
    }
  }
}

}  // namespace bwa
