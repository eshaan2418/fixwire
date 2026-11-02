// fixwire-exchange: FIX 4.4 acceptor that acknowledges every NewOrderSingle with an
// ExecutionReport (New) and fills it immediately at its limit price (a second ExecutionReport).
//   fixwire-exchange [--port 9878] [--sender EXCH] [--target CLIENT] [--store DIR] [--once]
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "fixwire/net.hpp"

using namespace fixwire;

static volatile std::sig_atomic_t stop_flag = 0;

int main(int argc, char** argv) {
  int port = 9878;
  std::string sender = "EXCH", target = "CLIENT", store_dir;
  bool once = false;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--port") port = std::atoi(argv[++i]);
    else if (a == "--sender") sender = argv[++i];
    else if (a == "--target") target = argv[++i];
    else if (a == "--store") store_dir = argv[++i];
    else if (a == "--once") once = true;
  }
  std::signal(SIGTERM, [](int) { stop_flag = 1; });
  std::signal(SIGINT, [](int) { stop_flag = 1; });
  std::signal(SIGPIPE, SIG_IGN);
  std::unique_ptr<Store> store = store_dir.empty() ? std::unique_ptr<Store>(new MemoryStore) : std::unique_ptr<Store>(new FileStore(store_dir));
  const int lfd = tcp_listen(port);
  std::fprintf(stderr, "fixwire-exchange: listening on 127.0.0.1:%d as %s\n", port, sender.c_str());
  long long exec_id = 0;
  while (!stop_flag) {
    pollfd p{lfd, POLLIN, 0};
    if (poll(&p, 1, 200) <= 0) continue;
    const int fd = accept(lfd, nullptr, nullptr);
    if (fd < 0) continue;
    nodelay(fd);
    Session s({"FIX.4.4", sender, target, 30, false}, *store, wall_ms);
    Builder er;
    s.on_app = [&](const Message& m) {
      if (m.msg_type() != "D") return;
      const auto cl = m.get(11), sym = m.get(55), side = m.get(54), qty = m.get(38), px = m.get(44);
      for (int k = 0; k < 2; ++k) {  // ack, then fill
        er.start("8").add(37, "o" + std::string(cl)).add(11, cl).add(17, ++exec_id).add(150, k == 0 ? "0" : "F").add(39, k == 0 ? "0" : "2");
        er.add(55, sym).add(54, side).add(38, qty).add(151, k == 0 ? qty : std::string_view("0")).add(14, k == 0 ? std::string_view("0") : qty);
        er.add(6, k == 0 ? std::string_view("0") : px);
        if (k == 1) er.add(31, px).add(32, qty);
        s.send(er);
      }
    };
    s.on_event = [](std::string_view e) { std::fprintf(stderr, "fixwire-exchange: %.*s\n", static_cast<int>(e.size()), e.data()); };
    run_connection(fd, s, [] { return stop_flag != 0; });
    const auto& st = s.stats();
    std::fprintf(stderr, "fixwire-exchange: session ended: received %llu, app %llu, sent %llu, resend_requests_served %llu, resent %llu\n",
                 static_cast<unsigned long long>(st.received), static_cast<unsigned long long>(st.app_delivered), static_cast<unsigned long long>(st.sent),
                 static_cast<unsigned long long>(st.resend_requests_served), static_cast<unsigned long long>(st.resent));
    if (once) break;
  }
  close(lfd);
}
