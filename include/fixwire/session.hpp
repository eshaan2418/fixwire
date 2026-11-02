// FIX session layer (FIX 4.4 session protocol), independent of the transport: feed it bytes and
// clock ticks, and it emits bytes and in-order application messages.
#pragma once

#include <fcntl.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "fixwire/codec.hpp"

namespace fixwire {

// ---------------------------------------------------------------------------------------------
// Message store: sequence numbers and every sent message (for resend), optionally on disk.
class Store {
 public:
  virtual ~Store() = default;
  virtual std::uint64_t next_out() const = 0;
  virtual std::uint64_t next_in() const = 0;
  virtual void set_next_out(std::uint64_t) = 0;
  virtual void set_next_in(std::uint64_t) = 0;
  virtual void save(std::uint64_t seq, std::string_view raw) = 0;
  virtual std::optional<std::string> get(std::uint64_t seq) const = 0;
  virtual void reset() = 0;
};

class MemoryStore : public Store {
 public:
  std::uint64_t next_out() const override { return out_; }
  std::uint64_t next_in() const override { return in_; }
  void set_next_out(std::uint64_t v) override { out_ = v; }
  void set_next_in(std::uint64_t v) override { in_ = v; }
  void save(std::uint64_t seq, std::string_view raw) override { msgs_[seq] = std::string(raw); }
  std::optional<std::string> get(std::uint64_t seq) const override {
    auto it = msgs_.find(seq);
    if (it == msgs_.end()) return std::nullopt;
    return it->second;
  }
  void reset() override {
    out_ = in_ = 1;
    msgs_.clear();
  }

 private:
  std::uint64_t out_ = 1, in_ = 1;
  std::map<std::uint64_t, std::string> msgs_;
};

// Append-only file store: <dir>/seqnums (two decimal numbers) and <dir>/messages
// (records: seq, length, bytes). Survives process restarts.
class FileStore : public Store {
 public:
  explicit FileStore(std::filesystem::path dir, bool fsync_each = false) : dir_(std::move(dir)), fsync_(fsync_each) {
    std::filesystem::create_directories(dir_);
    std::ifstream s(dir_ / "seqnums");
    if (s >> out_ >> in_) {
    } else {
      out_ = in_ = 1;
    }
    std::ifstream m(dir_ / "messages", std::ios::binary);
    std::uint64_t seq;
    std::uint32_t n;
    while (m.read(reinterpret_cast<char*>(&seq), 8) && m.read(reinterpret_cast<char*>(&n), 4)) {
      std::string raw(n, '\0');
      if (!m.read(raw.data(), n)) break;  // torn tail record from a crash: ignore
      msgs_[seq] = std::move(raw);
    }
    log_ = std::fopen((dir_ / "messages").c_str(), "ab");
    if (!log_) throw std::runtime_error("FileStore: cannot open message log");
    seq_fd_ = ::open((dir_ / "seqnums").c_str(), O_RDWR | O_CREAT, 0644);
    if (seq_fd_ < 0) throw std::runtime_error("FileStore: cannot open seqnums");
    write_seq();
  }
  ~FileStore() override {
    if (log_) std::fclose(log_);
    if (seq_fd_ >= 0) ::close(seq_fd_);
  }
  FileStore(const FileStore&) = delete;
  FileStore& operator=(const FileStore&) = delete;
  std::uint64_t next_out() const override { return out_; }
  std::uint64_t next_in() const override { return in_; }
  void set_next_out(std::uint64_t v) override {
    out_ = v;
    write_seq();
  }
  void set_next_in(std::uint64_t v) override {
    in_ = v;
    write_seq();
  }
  void save(std::uint64_t seq, std::string_view raw) override {
    const auto n = static_cast<std::uint32_t>(raw.size());
    std::fwrite(&seq, 8, 1, log_);
    std::fwrite(&n, 4, 1, log_);
    std::fwrite(raw.data(), 1, raw.size(), log_);
    std::fflush(log_);  // into the kernel before the message goes on the wire (resendable after a crash)
    if (fsync_) ::fsync(::fileno(log_));
    msgs_[seq] = std::string(raw);
  }
  std::optional<std::string> get(std::uint64_t seq) const override {
    auto it = msgs_.find(seq);
    if (it == msgs_.end()) return std::nullopt;
    return it->second;
  }
  void reset() override {
    out_ = in_ = 1;
    msgs_.clear();
    std::fclose(log_);
    log_ = std::fopen((dir_ / "messages").c_str(), "wb");
    std::fclose(log_);
    log_ = std::fopen((dir_ / "messages").c_str(), "ab");
    write_seq();
  }

 private:
  // One fixed-width record rewritten in place: a single small pwrite (no temp file + rename per
  // message, which cost ~0.5 ms each). Process-crash safe; fsync_each adds machine-crash safety.
  void write_seq() {
    char b[48];
    const int n = std::snprintf(b, sizeof b, "%020llu %020llu\n", static_cast<unsigned long long>(out_), static_cast<unsigned long long>(in_));
    if (::pwrite(seq_fd_, b, static_cast<std::size_t>(n), 0) != n) throw std::runtime_error("FileStore: seqnums write failed");
    if (fsync_) ::fsync(seq_fd_);
  }
  std::filesystem::path dir_;
  bool fsync_;
  std::uint64_t out_ = 1, in_ = 1;
  std::map<std::uint64_t, std::string> msgs_;
  std::FILE* log_ = nullptr;
  int seq_fd_ = -1;
};

// ---------------------------------------------------------------------------------------------
struct SessionConfig {
  std::string begin = "FIX.4.4";
  std::string sender;
  std::string target;
  int heartbeat_s = 30;
  bool initiator = true;
  bool reset_on_logon = false;  // 141=Y: both sides restart at sequence 1
};

// Milliseconds since the Unix epoch (injectable for deterministic tests).
using Clock = std::function<std::int64_t()>;

inline std::string fix_time(std::int64_t ms) {
  const std::time_t s = static_cast<std::time_t>(ms / 1000);
  std::tm tm{};
  gmtime_r(&s, &tm);
  char b[32];
  std::snprintf(b, sizeof b, "%04d%02d%02d-%02d:%02d:%02d.%03d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
                tm.tm_sec, static_cast<int>(ms % 1000));
  return b;
}

struct SessionStats {
  std::uint64_t sent = 0, received = 0, app_delivered = 0, resend_requests_sent = 0, resend_requests_served = 0, resent = 0,
                gap_fills_sent = 0, duplicates_ignored = 0, queued_out_of_order = 0, rejects_sent = 0, bad_frames = 0,
                heartbeats_sent = 0, test_requests_sent = 0;
};

class Session {
 public:
  enum class State { Disconnected, AwaitingLogon, Active, LogoutSent, Closed };

  Session(SessionConfig cfg, Store& store, Clock clock)
      : cfg_(std::move(cfg)), store_(store), clock_(std::move(clock)), b_(cfg_.begin), hdr_(cfg_.begin), tmp_(cfg_.begin) {}

  // Transport hooks.
  std::function<void(std::string_view)> send_bytes;
  std::function<void(const Message&)> on_app;
  std::function<void(std::string_view)> on_event;

  State state() const { return state_; }
  bool active() const { return state_ == State::Active; }
  bool closed() const { return state_ == State::Closed; }
  const SessionStats& stats() const { return st_; }
  const SessionConfig& config() const { return cfg_; }
  Store& store() { return store_; }

  // A transport connection is up: the initiator sends Logon.
  void connected() {
    rx_.clear();
    resend_pending_ = false;
    queue_.clear();
    state_ = State::AwaitingLogon;
    last_rx_ = last_tx_ = clock_();
    test_req_out_.reset();
    if (cfg_.initiator) send_logon();
  }

  // The transport connection was lost (sequence numbers persist in the store).
  void disconnected() {
    state_ = State::Disconnected;
    rx_.clear();
    queue_.clear();
  }

  void on_bytes(const char* data, std::size_t n) {
    rx_.append(data, n);
    std::size_t off = 0;
    Message m;
    while (off < rx_.size() && state_ != State::Closed) {
      ParseError err;
      const std::size_t used = parse(rx_.data() + off, rx_.size() - off, m, err, cfg_.begin);
      if (err == ParseError::Incomplete) break;
      off += used;
      if (err != ParseError::None) {
        ++st_.bad_frames;  // garbled frames are dropped; the sequence gap they leave triggers a resend
        event(std::string("bad_frame:") + to_string(err));
        continue;
      }
      last_rx_ = clock_();
      ++st_.received;
      // Any valid message proves the peer is alive (the heartbeat answering our TestRequest may
      // itself be lost: admin messages are gap-filled, not replayed, on resend).
      test_req_out_.reset();
      handle(m);
    }
    rx_.erase(0, off);
  }

  // Heartbeats, test requests and dead-peer detection. Call at least once per second.
  void on_timer() {
    if (state_ != State::Active) return;
    const std::int64_t now = clock_(), hb = cfg_.heartbeat_s * 1000LL;
    // A gap that made no progress for a heartbeat interval: the ResendRequest (or its replies)
    // was lost, so ask again from the current expected sequence number.
    if (resend_pending_ && store_.next_in() == resend_from_ && now - resend_sent_at_ >= hb) {
      ++st_.resend_requests_sent;
      b_.start("2");
      b_.add(7, static_cast<long long>(store_.next_in())).add(16, 0LL);
      admin(b_);
      resend_sent_at_ = now;
    } else if (resend_pending_ && store_.next_in() != resend_from_) {
      resend_from_ = store_.next_in();
      resend_sent_at_ = now;
    }
    if (now - last_tx_ >= hb) {
      ++st_.heartbeats_sent;
      admin(b_.start("0"));
    }
    if (!test_req_out_ && now - last_rx_ >= hb + hb / 5) {
      test_req_out_ = "TEST" + std::to_string(now);
      ++st_.test_requests_sent;
      b_.start("1");
      b_.add(112, *test_req_out_);
      admin(b_);
      test_req_sent_at_ = now;
    } else if (test_req_out_ && now - last_rx_ >= hb * 12 / 5) {
      // As QuickFIX: disconnect after 2.4 x HeartBtInt without receiving anything.
      event("peer_unresponsive");
      state_ = State::Closed;
    }
  }

  // Send an application message (body built with Builder::start(type).add(...)). Returns the
  // sequence number used, or 0 if the session is not active.
  std::uint64_t send(Builder& body) {
    if (state_ != State::Active) return 0;
    return emit(body, false);
  }

  void logout(std::string_view text = {}) {
    if (state_ != State::Active) return;
    b_.start("5");
    if (!text.empty()) b_.add(58, text);
    admin(b_);
    state_ = State::LogoutSent;
  }

 private:
  void event(std::string_view e) {
    if (on_event) on_event(e);
  }

  // Frame body with the standard header and the next outgoing sequence number.
  std::uint64_t emit(Builder& body, bool admin_msg, std::uint64_t seq_override = 0, std::string_view orig_time = {}) {
    const std::uint64_t seq = seq_override ? seq_override : store_.next_out();
    // Rebuild: header fields go right after 35 (body begins with 35=...).
    std::string_view rest = body.body();
    const std::size_t first = rest.find(SOH);
    hdr_.start(rest.substr(3, first - 3));
    hdr_.add(49, cfg_.sender).add(56, cfg_.target).add(34, static_cast<long long>(seq)).add(52, fix_time(clock_()));
    if (!orig_time.empty()) hdr_.add(43, "Y").add(122, orig_time);
    assembled_.assign(hdr_.body());
    assembled_.append(rest.substr(first + 1));
    tmp_.set_raw(assembled_);
    tmp_.finish(out_);
    if (!seq_override) {
      store_.save(seq, out_);
      store_.set_next_out(seq + 1);
    }
    (void)admin_msg;
    last_tx_ = clock_();
    ++st_.sent;
    if (send_bytes) send_bytes(out_);
    return seq;
  }

  void admin(Builder& body) { emit(body, true); }

  void send_logon() {
    if (cfg_.reset_on_logon) store_.reset();
    b_.start("A");
    b_.add(98, 0LL).add(108, static_cast<long long>(cfg_.heartbeat_s));
    if (cfg_.reset_on_logon) b_.add(141, "Y");
    admin(b_);
  }

  void reject(const Message& m, int reason, std::string_view text) {
    ++st_.rejects_sent;
    b_.start("3");
    b_.add(45, m.get(34)).add(373, static_cast<long long>(reason)).add(58, text);
    admin(b_);
  }

  void fatal(std::string_view text) {
    event(text);
    b_.start("5");
    b_.add(58, text);
    admin(b_);
    state_ = State::Closed;
  }

  void handle(const Message& m) {
    const auto type = m.msg_type();
    if (m.get(49) != cfg_.target || m.get(56) != cfg_.sender) return fatal("CompID problem");
    const long long seq = m.get_int(34);
    if (seq <= 0) return fatal("MsgSeqNum missing");
    if (state_ == State::AwaitingLogon && type != "A") return fatal("first message not a Logon");
    // ResetSeqNumFlag on Logon: both sides restart at 1 before sequence checks apply.
    if (type == "A" && m.get(141) == "Y") {
      store_.set_next_in(1);
      if (!cfg_.initiator && state_ == State::AwaitingLogon) store_.set_next_out(1);
    }
    // SequenceReset in reset mode (123 != Y) ignores the incoming sequence number entirely.
    if (type == "4" && m.get(123) != "Y") {
      const long long ns = m.get_int(36);
      if (ns < static_cast<long long>(store_.next_in())) return reject(m, 5, "NewSeqNo too low");
      store_.set_next_in(static_cast<std::uint64_t>(ns));
      return drain();
    }
    const auto expected = static_cast<long long>(store_.next_in());
    if (seq < expected) {
      if (m.get(43) == "Y") {
        ++st_.duplicates_ignored;
        return;
      }
      return fatal("MsgSeqNum too low, expecting " + std::to_string(expected) + " but received " + std::to_string(seq));
    }
    if (type == "A") {
      if (!on_logon(m)) return;
      if (seq == expected) {
        store_.set_next_in(store_.next_in() + 1);
        return drain();
      }
    }
    if (seq > expected) {
      // Serve a ResendRequest even when it arrives early; queue everything else in order.
      if (type == "2") serve_resend(m);
      if (type == "5") return on_logout(m);
      ++st_.queued_out_of_order;
      queue_.emplace(seq, std::string(m.raw()));
      if (!resend_pending_) {
        resend_pending_ = true;
        resend_from_ = store_.next_in();
        resend_sent_at_ = clock_();
        ++st_.resend_requests_sent;
        b_.start("2");
        b_.add(7, expected).add(16, 0LL);
        admin(b_);
        event("gap_detected");
      }
      return;
    }
    if (type != "A") process(m);
    drain();
  }

  // Process an in-sequence message and advance next_in (gap fills jump it forward).
  void process(const Message& m) {
    const auto type = m.msg_type();
    if (type == "4") {  // SequenceReset-GapFill
      const long long ns = m.get_int(36);
      if (ns <= m.get_int(34)) return reject(m, 5, "NewSeqNo must exceed MsgSeqNum");
      store_.set_next_in(static_cast<std::uint64_t>(ns));
      return;
    }
    store_.set_next_in(store_.next_in() + 1);
    if (type == "A" || type == "0") {
      // Logon (queued after a gap) and Heartbeat are only sequenced here.
    } else if (type == "1") {
      b_.start("0");
      b_.add(112, m.get(112));
      admin(b_);
    } else if (type == "2") {
      serve_resend(m);
    } else if (type == "3") {
      event("reject_received");
    } else if (type == "5") {
      on_logout(m);
    } else {
      ++st_.app_delivered;
      if (on_app) on_app(m);
    }
  }

  void drain() {
    while (!queue_.empty() && state_ != State::Closed) {
      auto it = queue_.begin();
      const auto next = static_cast<long long>(store_.next_in());
      if (it->first < next) {
        queue_.erase(it);  // superseded by a gap fill
        continue;
      }
      if (it->first > next) break;
      std::string raw = std::move(it->second);
      queue_.erase(it);
      Message m;
      ParseError err;
      parse(raw.data(), raw.size(), m, err, cfg_.begin);
      process(m);
    }
    if (queue_.empty()) resend_pending_ = false;
  }

  bool on_logon(const Message& m) {
    if (m.get(141) == "Y") {
      store_.set_next_in(1);
      if (!cfg_.initiator) store_.set_next_out(1);
    }
    if (state_ != State::AwaitingLogon) return true;  // a Logon during an active session is just sequenced
    if (!cfg_.initiator) {
      const long long hb = m.get_int(108);
      if (hb > 0) cfg_.heartbeat_s = static_cast<int>(hb);
      if (m.get(141) == "Y") store_.reset();
      b_.start("A");
      b_.add(98, 0LL).add(108, static_cast<long long>(cfg_.heartbeat_s));
      if (m.get(141) == "Y") b_.add(141, "Y");
      state_ = State::Active;
      admin(b_);
    } else {
      state_ = State::Active;
    }
    event("logon");
    return true;
  }

  void on_logout(const Message& m) {
    if (state_ != State::LogoutSent) {
      b_.start("5");
      admin(b_);
    }
    event(std::string("logout:") + std::string(m.get(58)));
    state_ = State::Closed;
  }

  // Resend [begin, end] (end 0 = everything sent so far): application messages again with
  // PossDupFlag=Y and OrigSendingTime; administrative messages and holes as SequenceReset-GapFill.
  void serve_resend(const Message& m) {
    ++st_.resend_requests_served;
    std::uint64_t lo = static_cast<std::uint64_t>(std::max(1LL, m.get_int(7))), hi = static_cast<std::uint64_t>(m.get_int(16));
    const std::uint64_t last = store_.next_out() - 1;
    if (hi == 0 || hi > last) hi = last;
    std::uint64_t gap_start = 0;
    auto flush_gap = [&](std::uint64_t upto) {
      if (!gap_start) return;
      ++st_.gap_fills_sent;
      Builder g(cfg_.begin);
      g.start("4");
      g.add(123, "Y").add(36, static_cast<long long>(upto));
      emit(g, true, gap_start, fix_time(clock_()));
      gap_start = 0;
    };
    for (std::uint64_t s = lo; s <= hi; ++s) {
      auto raw = store_.get(s);
      Message old;
      ParseError err = ParseError::Incomplete;
      if (raw) parse(raw->data(), raw->size(), old, err, cfg_.begin);
      const auto t = err == ParseError::None ? old.msg_type() : std::string_view{};
      const bool is_admin = t.empty() || t == "0" || t == "1" || t == "2" || t == "3" || t == "4" || t == "5" || t == "A";
      if (is_admin) {
        if (!gap_start) gap_start = s;
        continue;
      }
      flush_gap(s);
      // Resend the original body with PossDup and the original SendingTime.
      Builder body(cfg_.begin);
      bool first = true;
      for (std::size_t i = 0; i < old.size(); ++i) {
        const int tag = old[i].tag;
        if (tag == 8 || tag == 9 || tag == 10 || tag == 49 || tag == 56 || tag == 34 || tag == 52 || tag == 43 || tag == 122) continue;
        if (first) {
          body.start(old[i].value);
          first = false;
        } else {
          body.add(tag, old[i].value);
        }
      }
      ++st_.resent;
      emit(body, false, s, old.get(52));
    }
    flush_gap(hi + 1);
  }

  SessionConfig cfg_;
  Store& store_;
  Clock clock_;
  Builder b_, hdr_, tmp_;
  std::string out_, rx_, assembled_;
  State state_ = State::Disconnected;
  std::int64_t last_rx_ = 0, last_tx_ = 0, test_req_sent_at_ = 0;
  std::optional<std::string> test_req_out_;
  bool resend_pending_ = false;
  std::uint64_t resend_from_ = 0;
  std::int64_t resend_sent_at_ = 0;
  std::map<long long, std::string> queue_;
  SessionStats st_;
};

}  // namespace fixwire
