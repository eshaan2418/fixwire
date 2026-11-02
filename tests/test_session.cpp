#include <gtest/gtest.h>

#include <deque>
#include <filesystem>
#include <random>

#include "fixwire/session.hpp"

using namespace fixwire;

namespace {

// Two sessions joined by an in-memory link with fault injection on each direction.
struct Link {
  std::int64_t now = 1'793'606'400'000;  // 2026-11-02
  MemoryStore sa, sb;
  Session a;
  Session b;
  std::deque<std::string> ab, ba;               // in flight
  std::function<bool(std::string&)> fault_ab;  // return false to drop; may mutate
  std::function<bool(std::string&)> fault_ba;
  std::vector<std::string> got_a, got_b;        // app payloads (tag 11) delivered
  std::vector<std::string> events;

  explicit Link(bool reset = false)
      : a({"FIX.4.4", "CLIENT", "EXCH", 30, true, reset}, sa, [this] { return now; }),
        b({"FIX.4.4", "EXCH", "CLIENT", 30, false}, sb, [this] { return now; }) {
    wire(a, b, sa, sb);
  }
  void wire(Session& x, Session& y, Store&, Store&) {
    x.send_bytes = [this](std::string_view s) { ab.emplace_back(s); };
    y.send_bytes = [this](std::string_view s) { ba.emplace_back(s); };
    x.on_app = [this](const Message& m) { got_a.emplace_back(m.get(11)); };
    y.on_app = [this](const Message& m) { got_b.emplace_back(m.get(11)); };
    x.on_event = [this](std::string_view e) { events.emplace_back(std::string("a:") + std::string(e)); };
    y.on_event = [this](std::string_view e) { events.emplace_back(std::string("b:") + std::string(e)); };
  }
  void pump() {
    for (int guard = 0; guard < 100000 && (!ab.empty() || !ba.empty()); ++guard) {
      if (!ab.empty()) {
        std::string s = std::move(ab.front());
        ab.pop_front();
        if (!fault_ab || fault_ab(s)) b.on_bytes(s.data(), s.size());
      }
      if (!ba.empty()) {
        std::string s = std::move(ba.front());
        ba.pop_front();
        if (!fault_ba || fault_ba(s)) a.on_bytes(s.data(), s.size());
      }
    }
  }
  void connect() {
    b.connected();
    a.connected();
    pump();
  }
  std::uint64_t order(Session& s, const std::string& id) {
    Builder body;
    body.start("D").add(11, id).add(55, "AAPL").add(54, "1").add(38, 100LL).add(40, "2").add(44, "187.25");
    return s.send(body);
  }
};

std::string type_of(const std::string& raw) {
  Message m;
  ParseError e;
  parse(raw.data(), raw.size(), m, e);
  return std::string(m.msg_type());
}

}  // namespace

TEST(Session, LogonAndInOrderDelivery) {
  Link l;
  l.connect();
  ASSERT_TRUE(l.a.active());
  ASSERT_TRUE(l.b.active());
  for (int i = 0; i < 100; ++i) l.order(l.a, "o" + std::to_string(i));
  l.pump();
  ASSERT_EQ(l.got_b.size(), 100u);
  for (int i = 0; i < 100; ++i) EXPECT_EQ(l.got_b[i], "o" + std::to_string(i));
  EXPECT_EQ(l.sa.next_out(), 102u);  // logon + 100 orders
  EXPECT_EQ(l.sb.next_in(), 102u);
}

TEST(Session, DroppedMessageIsResentWithPossDup) {
  Link l;
  l.connect();
  int n = 0;
  l.fault_ab = [&](std::string& s) { return !(type_of(s) == "D" && ++n == 3); };  // drop the 3rd order
  for (int i = 0; i < 10; ++i) l.order(l.a, "o" + std::to_string(i));
  l.pump();
  ASSERT_EQ(l.got_b.size(), 10u);
  for (int i = 0; i < 10; ++i) EXPECT_EQ(l.got_b[i], "o" + std::to_string(i)) << "order preserved through the gap";
  EXPECT_EQ(l.b.stats().resend_requests_sent, 1u);
  EXPECT_GE(l.a.stats().resent, 1u);
  EXPECT_EQ(l.b.stats().duplicates_ignored, l.a.stats().resent - 1) << "only the missing one is new";
}

TEST(Session, AdminMessagesAreGapFilledOnResend) {
  Link l;
  l.connect();
  // Heartbeats (admin) then an order, all dropped; a later order reveals the gap.
  bool drop = true;
  l.fault_ab = [&](std::string&) { return !drop; };
  l.now += 31'000;
  l.a.on_timer();  // heartbeat (seq 2)
  l.order(l.a, "x1");  // seq 3
  l.pump();
  drop = false;
  l.order(l.a, "x2");  // seq 4: b sees a gap 2..3
  l.pump();
  ASSERT_EQ(l.got_b, (std::vector<std::string>{"x1", "x2"}));
  EXPECT_EQ(l.a.stats().gap_fills_sent, 1u);  // the heartbeat became a SequenceReset-GapFill
  EXPECT_EQ(l.sb.next_in(), 5u);
}

TEST(Session, HeartbeatTestRequestAndDeadPeer) {
  Link l;
  l.connect();
  l.now += 30'000;
  l.a.on_timer();
  l.pump();
  EXPECT_EQ(l.a.stats().heartbeats_sent, 1u);
  // b goes silent: a sends a TestRequest after 1.2 x HeartBtInt without traffic, then gives up.
  l.fault_ba = [](std::string&) { return false; };
  l.now += 37'000;
  l.a.on_timer();
  l.pump();
  EXPECT_EQ(l.a.stats().test_requests_sent, 1u);
  l.now += 36'000;  // 73 s without receiving anything > 2.4 x 30 s
  l.a.on_timer();
  EXPECT_TRUE(l.a.closed());
}

TEST(Session, TestRequestIsAnswered) {
  Link l;
  l.connect();
  l.now += 37'000;
  l.a.on_timer();  // sends heartbeat + test request (no rx for > 1.2 HB)
  l.pump();
  EXPECT_FALSE(l.a.closed());
  l.now += 31'000;
  l.a.on_timer();
  l.pump();
  EXPECT_FALSE(l.a.closed()) << "the heartbeat reply with TestReqID cleared the pending test";
}

TEST(Session, SeqTooLowWithoutPossDupIsFatal) {
  Link l;
  l.connect();
  l.order(l.a, "o1");
  l.pump();
  // Replay the order a second time verbatim (same MsgSeqNum, no PossDup).
  std::string replay;
  l.fault_ab = [&](std::string& s) {
    if (type_of(s) == "D") replay = s;
    return true;
  };
  l.order(l.a, "o2");
  l.pump();
  l.b.on_bytes(replay.data(), replay.size());
  EXPECT_TRUE(l.b.closed());
  EXPECT_EQ(l.got_b.size(), 2u);
}

TEST(Session, PossDupDuplicateIsIgnored) {
  Link l;
  l.connect();
  l.order(l.a, "o1");
  l.pump();
  // Ask a to resend everything: duplicates arrive with 43=Y and are ignored.
  Builder rr;
  rr.start("2").add(7, 1LL).add(16, 0LL);
  l.b.send(rr);
  l.pump();
  EXPECT_EQ(l.got_b.size(), 1u);
  EXPECT_FALSE(l.b.closed());
  EXPECT_GE(l.b.stats().duplicates_ignored, 1u);
}

TEST(Session, GarbledFrameRecoveredByResend) {
  Link l;
  l.connect();
  int n = 0;
  l.fault_ab = [&](std::string& s) {
    if (type_of(s) == "D" && ++n == 2) s[s.size() - 3] ^= 1;  // corrupt the checksum digits
    return true;
  };
  for (int i = 0; i < 5; ++i) l.order(l.a, "o" + std::to_string(i));
  l.pump();
  EXPECT_EQ(l.got_b.size(), 5u);
  EXPECT_EQ(l.b.stats().bad_frames, 1u);
}

TEST(Session, RestartFromFileStoreResumesSequencesAndResends) {
  const auto dir = std::filesystem::temp_directory_path() / ("fixwire-store-" + std::to_string(::getpid()));
  std::filesystem::remove_all(dir);
  std::int64_t now = 1'793'606'400'000;
  MemoryStore sb;
  std::vector<std::string> got;
  std::deque<std::string> ab, ba;
  Session b({"FIX.4.4", "EXCH", "CLIENT", 30, false}, sb, [&] { return now; });
  b.send_bytes = [&](std::string_view s) { ba.emplace_back(s); };
  b.on_app = [&](const Message& m) { got.emplace_back(m.get(11)); };
  auto pump = [&](Session& a) {
    while (!ab.empty() || !ba.empty()) {
      if (!ab.empty()) {
        auto s = ab.front();
        ab.pop_front();
        b.on_bytes(s.data(), s.size());
      }
      if (!ba.empty()) {
        auto s = ba.front();
        ba.pop_front();
        a.on_bytes(s.data(), s.size());
      }
    }
  };
  {
    FileStore fs(dir);
    Session a({"FIX.4.4", "CLIENT", "EXCH", 30, true}, fs, [&] { return now; });
    a.send_bytes = [&](std::string_view s) { ab.emplace_back(s); };
    b.connected();
    a.connected();
    pump(a);
    Builder o;
    for (int i = 0; i < 3; ++i) {
      o.start("D").add(11, "p" + std::to_string(i)).add(55, "MSFT").add(54, "2").add(38, 10LL).add(40, "1");
      a.send(o);
    }
    // Two more orders are written to the store but the connection dies before they arrive.
    pump(a);
    for (int i = 3; i < 5; ++i) {
      o.start("D").add(11, "p" + std::to_string(i)).add(55, "MSFT").add(54, "2").add(38, 10LL).add(40, "1");
      a.send(o);
    }
    ab.clear();
    b.disconnected();
  }
  // Process restart: a new session over the same files continues at the stored sequence.
  FileStore fs2(dir);
  EXPECT_EQ(fs2.next_out(), 7u);  // logon + 5 orders
  Session a2({"FIX.4.4", "CLIENT", "EXCH", 30, true}, fs2, [&] { return now; });
  a2.send_bytes = [&](std::string_view s) { ab.emplace_back(s); };
  b.connected();
  a2.connected();  // Logon(7): b expects 5 -> ResendRequest(5..) -> a2 resends p3, p4 from disk
  pump(a2);
  ASSERT_EQ(got, (std::vector<std::string>{"p0", "p1", "p2", "p3", "p4"}));
  std::filesystem::remove_all(dir);
}

TEST(Session, ResetSeqNumFlagRestartsAtOne) {
  Link l;
  l.connect();
  for (int i = 0; i < 5; ++i) l.order(l.a, "o" + std::to_string(i));
  l.pump();
  Link l2(true);
  l2.sa.set_next_out(50);
  l2.sb.set_next_in(77);
  l2.connect();
  ASSERT_TRUE(l2.a.active());
  EXPECT_EQ(l2.sa.next_out(), 2u);
  EXPECT_EQ(l2.sb.next_in(), 2u);
}

// Random drops and garbling of orders and resends: every order is delivered exactly once and in
// order once the link heals.
TEST(Session, RandomFaultsNoLossNoDuplicates) {
  for (int seed = 0; seed < 20; ++seed) {
    Link l;
    l.connect();
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> u(0, 1);
    bool faulty = true;
    // Faults hit application and resend traffic; heartbeats/test requests are spared because
    // liveness under loss is a separate property (HeartbeatTestRequestAndDeadPeer).
    auto fault = [&](std::string& s) {
      if (!faulty) return true;
      const std::string t = type_of(s);
      if (t == "0" || t == "1") return true;
      const double x = u(rng);
      if (x < 0.05) return false;              // drop
      if (x < 0.08) s[s.size() / 2] ^= 0x20;   // garble
      return true;
    };
    // Faults on the order direction (initiator -> acceptor); the acceptor's requests always get
    // through. Silently dropping frames inside a live TCP stream is already harsher than reality
    // (TCP delivers or disconnects); dropping both directions mostly tests liveness timeouts.
    l.fault_ab = fault;
    for (int i = 0; i < 300; ++i) {
      l.order(l.a, "o" + std::to_string(i));
      if (i % 5 == 0) {  // time passes in 10 s steps; both sides run their timers
        l.now += 10'000;
        l.a.on_timer();
        l.b.on_timer();
      }
      l.pump();
    }
    faulty = false;
    // After healing: a final order reveals any trailing gap; timers re-request lost resends.
    l.order(l.a, "end");
    l.pump();
    for (int r = 0; r < 12; ++r) {
      l.now += 10'000;
      l.a.on_timer();
      l.b.on_timer();
      l.pump();
    }
    if (l.a.closed() || l.b.closed()) {
      std::string ev;
      for (auto& e : l.events) ev += e + " ";
      const auto& sa = l.a.stats();
      const auto& sb = l.b.stats();
      ADD_FAILURE() << "seed " << seed << " inflight ab=" << l.ab.size() << " ba=" << l.ba.size() << " a.sent=" << sa.sent << " a.recv=" << sa.received
                    << " b.sent=" << sb.sent << " b.recv=" << sb.received << " b.rr=" << sb.resend_requests_sent << " a.resent=" << sa.resent
                    << " b.queued=" << sb.queued_out_of_order << " b.dups=" << sb.duplicates_ignored << " got=" << l.got_b.size() << " nextin_b=" << l.sb.next_in()
                    << " nextout_a=" << l.sa.next_out() << " events: " << ev.substr(0, 200);
    }
    ASSERT_FALSE(l.a.closed()) << seed;
    ASSERT_FALSE(l.b.closed()) << seed;
    ASSERT_EQ(l.got_b.size(), 301u) << seed;
    for (int i = 0; i < 300; ++i) ASSERT_EQ(l.got_b[i], "o" + std::to_string(i)) << seed;
  }
}
