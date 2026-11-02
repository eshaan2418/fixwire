// fixwire-client: FIX 4.4 initiator that sends N NewOrderSingle (with at most W outstanding) and
// measures order-to-fill round-trip latency; prints a JSON summary.
//   fixwire-client [--port 9878] [--orders 10000] [--window 1] [--store DIR] [--stop-after K]
//                  [--sender CLIENT] [--target EXCH] [--prefix ord] [--reset]
// --reset logs on with ResetSeqNumFlag=Y (both sides restart at sequence 1).
// --stop-after K drops the connection abruptly after K orders are sent (crash simulation; with a
// --store the next run resumes the sequence and recovers via ResendRequest).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <csignal>
#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "fixwire/net.hpp"

using namespace fixwire;

int main(int argc, char** argv) {
  int port = 9878, window = 1;
  long long orders = 10000, stop_after = -1, start_index = 0;
  std::string store_dir, sender = "CLIENT", target = "EXCH", prefix = "ord";
  bool reset = false;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--port") port = std::atoi(argv[++i]);
    else if (a == "--orders") orders = std::atoll(argv[++i]);
    else if (a == "--window") window = std::atoi(argv[++i]);
    else if (a == "--store") store_dir = argv[++i];
    else if (a == "--stop-after") stop_after = std::atoll(argv[++i]);
    else if (a == "--start-index") start_index = std::atoll(argv[++i]);
    else if (a == "--sender") sender = argv[++i];
    else if (a == "--target") target = argv[++i];
    else if (a == "--prefix") prefix = argv[++i];
    else if (a == "--reset") reset = true;
  }
  std::signal(SIGPIPE, SIG_IGN);
  std::unique_ptr<Store> store = store_dir.empty() ? std::unique_ptr<Store>(new MemoryStore) : std::unique_ptr<Store>(new FileStore(store_dir));
  int fd = -1;
  for (int tries = 0; tries < 100 && fd < 0; ++tries) {
    fd = tcp_connect("127.0.0.1", port);
    if (fd < 0) usleep(100000);
  }
  if (fd < 0) {
    std::fprintf(stderr, "fixwire-client: cannot connect\n");
    return 1;
  }
  Session s({"FIX.4.4", sender, target, 30, true, reset}, *store, wall_ms);
  using clk = std::chrono::steady_clock;
  std::unordered_map<std::string, clk::time_point> sent_at;
  std::vector<double> rtt_us;
  rtt_us.reserve(static_cast<std::size_t>(orders));
  std::unordered_set<std::string> filled;
  long long next = start_index, dup_reports = 0, recovered = 0;
  long long outstanding = 0;
  s.on_app = [&](const Message& m) {
    if (m.msg_type() != "8" || m.get(150) != "F") return;
    std::string id(m.get(11));
    if (!filled.insert(id).second) {
      ++dup_reports;
      return;
    }
    auto it = sent_at.find(id);
    if (it == sent_at.end()) {
      ++recovered;  // fill for an order sent by a previous run (recovered via resend)
      return;
    }
    rtt_us.push_back(std::chrono::duration<double, std::micro>(clk::now() - it->second).count());
    sent_at.erase(it);
    --outstanding;
  };
  Builder o;
  bool crashed = false;
  const auto t0 = clk::now();
  run_connection(
      fd, s, [&] { return (next >= start_index + orders && outstanding <= 0) || crashed; },
      [&] {
        while (s.active() && outstanding < window && next < start_index + orders) {
          const std::string id = prefix + std::to_string(next++);
          o.start("D").add(11, id).add(1, "ACCT1").add(55, "AAPL").add(54, "1").add(60, fix_time(wall_ms())).add(38, 100LL).add(40, "2").add(44, "187.25");
          sent_at[id] = clk::now();
          s.send(o);
          ++outstanding;
          if (stop_after >= 0 && next - start_index >= stop_after) {
            crashed = true;
            return;
          }
        }
      });
  const double secs = std::chrono::duration<double>(clk::now() - t0).count();
  std::sort(rtt_us.begin(), rtt_us.end());
  auto q = [&](double p) { return rtt_us.empty() ? 0.0 : rtt_us[std::min(rtt_us.size() - 1, static_cast<std::size_t>(p * static_cast<double>(rtt_us.size())))]; };
  const auto& st = s.stats();
  std::printf("{\"recovered_fills_from_previous_run\": %lld, \"orders_sent\": %lld, \"fills_received\": %zu, \"duplicate_fill_reports\": %lld, \"seconds\": %.3f, \"orders_per_sec\": %.0f, "
              "\"rtt_us\": {\"p50\": %.1f, \"p90\": %.1f, \"p99\": %.1f, \"p999\": %.1f, \"max\": %.1f}, \"window\": %d, \"next_out\": %llu, \"next_in\": %llu, "
              "\"resend_requests_sent\": %llu, \"duplicates_ignored\": %llu, \"crashed\": %s}\n",
              recovered, next - start_index, filled.size(), dup_reports, secs, static_cast<double>(next - start_index) / secs, q(0.5), q(0.9), q(0.99), q(0.999),
              rtt_us.empty() ? 0.0 : rtt_us.back(), window, static_cast<unsigned long long>(store->next_out()), static_cast<unsigned long long>(store->next_in()),
              static_cast<unsigned long long>(st.resend_requests_sent), static_cast<unsigned long long>(st.duplicates_ignored), crashed ? "true" : "false");
  return 0;
}
