// Minimal TCP transport for one FIX session: blocking connect/accept, then a poll() loop that
// feeds received bytes to the session, flushes its output and ticks its timers.
#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>

#include "fixwire/session.hpp"

namespace fixwire {

inline std::int64_t wall_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

inline void nodelay(int fd) {
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
}

inline int tcp_connect(const std::string& host, int port) {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(static_cast<uint16_t>(port));
  inet_pton(AF_INET, host.c_str(), &a.sin_addr);
  if (connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
    close(fd);
    return -1;
  }
  nodelay(fd);
  return fd;
}

inline int tcp_listen(int port) {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(static_cast<uint16_t>(port));
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || listen(fd, 4) != 0) throw std::runtime_error("listen failed");
  return fd;
}

inline void write_all(int fd, std::string_view s) {
  while (!s.empty()) {
    const ssize_t w = ::write(fd, s.data(), s.size());
    if (w < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error(std::string("write: ") + std::strerror(errno));
    }
    s.remove_prefix(static_cast<std::size_t>(w));
  }
}

// Run one connection until the session closes, the peer disconnects or stop() returns true.
// idle_hook runs every loop iteration (e.g. to send application messages).
inline void run_connection(int fd, Session& s, const std::function<bool()>& stop, const std::function<void()>& idle_hook = {}) {
  bool broken = false;
  // A write error (peer gone) ends the connection; the session keeps its sequence numbers.
  s.send_bytes = [fd, &broken](std::string_view b) {
    if (broken) return;
    try {
      write_all(fd, b);
    } catch (const std::runtime_error&) {
      broken = true;
    }
  };
  s.connected();
  char buf[1 << 16];
  std::int64_t last_tick = wall_ms();
  while (!s.closed() && !broken && !(stop && stop())) {
    pollfd p{fd, POLLIN, 0};
    const int r = poll(&p, 1, idle_hook ? 0 : 50);
    if (r > 0 && (p.revents & (POLLIN | POLLHUP | POLLERR))) {
      const ssize_t n = ::read(fd, buf, sizeof buf);
      if (n <= 0) break;
      s.on_bytes(buf, static_cast<std::size_t>(n));
    }
    if (idle_hook) idle_hook();
    const std::int64_t now = wall_ms();
    if (now - last_tick >= 200) {
      s.on_timer();
      last_tick = now;
    }
  }
  s.disconnected();
  close(fd);
}

}  // namespace fixwire
