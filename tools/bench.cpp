// fixwire-bench: codec throughput (parse + checksum validation, build + framing) on a typical
// NewOrderSingle and ExecutionReport. Prints JSON.
#include <chrono>
#include <cstdio>
#include <string>

#include "fixwire/codec.hpp"

using namespace fixwire;
using clk = std::chrono::steady_clock;

int main() {
  Builder b;
  std::string nos, er;
  b.start("D").add(49, "CLIENT").add(56, "EXCH").add(34, 123456LL).add(52, "20261102-12:00:00.123").add(11, "ord-123456").add(1, "ACCT1").add(55, "AAPL")
      .add(54, "1").add(60, "20261102-12:00:00.123").add(38, 100LL).add(40, "2").add(44, "187.25");
  b.finish(nos);
  b.start("8").add(49, "EXCH").add(56, "CLIENT").add(34, 654321LL).add(52, "20261102-12:00:00.124").add(37, "o-ord-123456").add(11, "ord-123456")
      .add(17, 9988776LL).add(150, "F").add(39, "2").add(55, "AAPL").add(54, "1").add(38, 100LL).add(151, "0").add(14, 100LL).add(6, "187.25").add(31, "187.25").add(32, 100LL);
  b.finish(er);
  const int N = 5'000'000;
  Message m;
  ParseError err;
  long long sink = 0;
  auto bench_parse = [&](const std::string& s) {
    const auto t0 = clk::now();
    for (int i = 0; i < N; ++i) {
      sink += static_cast<long long>(parse(s.data(), s.size(), m, err));
      sink += m.size();
    }
    return std::chrono::duration<double, std::nano>(clk::now() - t0).count() / N;
  };
  const double p_nos = bench_parse(nos), p_er = bench_parse(er);
  std::string out;
  const auto t0 = clk::now();
  for (int i = 0; i < N; ++i) {
    b.start("8").add(49, "EXCH").add(56, "CLIENT").add(34, static_cast<long long>(i)).add(52, "20261102-12:00:00.124").add(37, "o-ord-123456").add(11, "ord-123456")
        .add(17, static_cast<long long>(i)).add(150, "F").add(39, "2").add(55, "AAPL").add(54, "1").add(38, 100LL).add(151, "0").add(14, 100LL).add(6, "187.25")
        .add(31, "187.25").add(32, 100LL);
    sink += static_cast<long long>(b.finish(out).size());
  }
  const double build_er = std::chrono::duration<double, std::nano>(clk::now() - t0).count() / N;
  std::printf("{\"parse_new_order_single_ns\": %.1f, \"parse_execution_report_ns\": %.1f, \"build_execution_report_ns\": %.1f, "
              "\"nos_bytes\": %zu, \"er_bytes\": %zu, \"parse_er_mb_per_s\": %.0f, \"sink\": %lld}\n",
              p_nos, p_er, build_er, nos.size(), er.size(), static_cast<double>(er.size()) / p_er * 1e3, sink % 7);
}
