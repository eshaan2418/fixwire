// FIX tag=value codec: zero-copy parsing with exact BodyLength / CheckSum validation, and a
// builder that writes BeginString, BodyLength and CheckSum around a body.
//
// Wire format: 8=FIX.4.4<SOH>9=<len><SOH>35=...<SOH>...10=<sum><SOH>, where len counts the bytes
// from the first byte after 9=...<SOH> up to and including the SOH before 10=, and sum is the
// byte sum modulo 256 of everything before 10=, written as three digits.
#pragma once

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace fixwire {

inline constexpr char SOH = '\x01';

struct Field {
  int tag;
  std::string_view value;
};

enum class ParseError { None, Incomplete, BadBeginString, BadBodyLength, BadChecksum, BadField, TooManyFields, Garbled };

inline const char* to_string(ParseError e) {
  switch (e) {
    case ParseError::None: return "none";
    case ParseError::Incomplete: return "incomplete";
    case ParseError::BadBeginString: return "bad_begin_string";
    case ParseError::BadBodyLength: return "bad_body_length";
    case ParseError::BadChecksum: return "bad_checksum";
    case ParseError::BadField: return "bad_field";
    case ParseError::TooManyFields: return "too_many_fields";
    case ParseError::Garbled: return "garbled";
  }
  return "?";
}

// A parsed message: views into the caller's buffer (valid while the buffer is).
class Message {
 public:
  static constexpr std::size_t kMaxFields = 256;
  std::size_t size() const { return n_; }
  const Field& operator[](std::size_t i) const { return f_[i]; }
  std::string_view get(int tag) const {
    for (std::size_t i = 0; i < n_; ++i)
      if (f_[i].tag == tag) return f_[i].value;
    return {};
  }
  bool has(int tag) const {
    for (std::size_t i = 0; i < n_; ++i)
      if (f_[i].tag == tag) return true;
    return false;
  }
  long long get_int(int tag, long long def = -1) const {
    auto v = get(tag);
    long long x = 0;
    auto r = std::from_chars(v.data(), v.data() + v.size(), x);
    return (r.ec == std::errc() && r.ptr == v.data() + v.size() && !v.empty()) ? x : def;
  }
  std::string_view msg_type() const { return get(35); }
  std::string_view raw() const { return raw_; }

 private:
  friend std::size_t parse(const char*, std::size_t, Message&, ParseError&, std::string_view);
  std::array<Field, kMaxFields> f_{};
  std::size_t n_ = 0;
  std::string_view raw_;
};

inline bool parse_uint(std::string_view s, long long& out) {
  if (s.empty() || s.size() > 18) return false;
  long long x = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    x = x * 10 + (c - '0');
  }
  out = x;
  return true;
}

// Parse one message from the front of buf. Returns bytes consumed (0 with Incomplete if more
// data is needed). On a framing error, err is set and the return value is the number of bytes
// to discard to resynchronise (to the next "8=" candidate), never 0.
inline std::size_t parse(const char* buf, std::size_t len, Message& m, ParseError& err, std::string_view begin = "FIX.4.4") {
  err = ParseError::None;
  m.n_ = 0;
  // Resynchronise to the next "8=<begin><SOH>" (after garbage it may follow any byte).
  auto resync = [&](ParseError e) -> std::size_t {
    err = e;
    const std::size_t need = 3 + begin.size();
    for (std::size_t i = 1; i + need <= len; ++i)
      if (buf[i] == '8' && buf[i + 1] == '=' && std::memcmp(buf + i + 2, begin.data(), begin.size()) == 0 && buf[i + 2 + begin.size()] == SOH)
        return i;
    // Keep a possible partial prefix at the end for the next read.
    return len > need ? len - need + 1 : 1;
  };
  // 8=<begin><SOH>
  const std::size_t hb = 2 + begin.size() + 1;
  if (len < hb + 4) {
    if (len >= 2 && (buf[0] != '8' || buf[1] != '=')) return resync(ParseError::BadBeginString);
    err = ParseError::Incomplete;
    return 0;
  }
  if (buf[0] != '8' || buf[1] != '=' || std::memcmp(buf + 2, begin.data(), begin.size()) != 0 || buf[hb - 1] != SOH)
    return resync(ParseError::BadBeginString);
  // 9=<len><SOH>
  if (buf[hb] != '9' || buf[hb + 1] != '=') return resync(ParseError::BadBodyLength);
  std::size_t p = hb + 2, q = p;
  while (q < len && buf[q] != SOH && q - p < 8) ++q;
  if (q >= len) {
    err = ParseError::Incomplete;
    return 0;
  }
  long long body = 0;
  if (buf[q] != SOH || !parse_uint({buf + p, q - p}, body) || body <= 0 || body > (1 << 20)) return resync(ParseError::BadBodyLength);
  const std::size_t body_start = q + 1;
  const std::size_t trailer = body_start + static_cast<std::size_t>(body);  // "10=XXX<SOH>"
  if (len < trailer + 7) {
    err = ParseError::Incomplete;
    return 0;
  }
  if (buf[trailer - 1] != SOH || buf[trailer] != '1' || buf[trailer + 1] != '0' || buf[trailer + 2] != '=' || buf[trailer + 6] != SOH)
    return resync(ParseError::BadBodyLength);
  unsigned sum = 0;
  for (std::size_t i = 0; i < trailer; ++i) sum += static_cast<unsigned char>(buf[i]);
  long long cs = -1;
  if (!parse_uint({buf + trailer + 3, 3}, cs) || static_cast<unsigned>(cs) != (sum & 255u)) {
    err = ParseError::BadChecksum;
    return trailer + 7;  // the frame is well delimited: drop exactly it
  }
  // Fields: header 8 and 9, then the body, then 10.
  m.f_[m.n_++] = {8, {buf + 2, begin.size()}};
  m.f_[m.n_++] = {9, {buf + p, q - p}};
  std::size_t i = body_start;
  while (i < trailer) {
    std::size_t eq = i;
    int tag = 0;
    while (eq < trailer && buf[eq] >= '0' && buf[eq] <= '9' && eq - i < 9) tag = tag * 10 + (buf[eq++] - '0');
    if (eq == i || eq >= trailer || buf[eq] != '=' || tag <= 0) {
      err = ParseError::BadField;
      return trailer + 7;
    }
    std::size_t e = eq + 1;
    while (e < trailer && buf[e] != SOH) ++e;
    if (e >= trailer && buf[e] != SOH) {
      err = ParseError::BadField;
      return trailer + 7;
    }
    if (m.n_ >= Message::kMaxFields - 1) {
      err = ParseError::TooManyFields;
      return trailer + 7;
    }
    m.f_[m.n_++] = {tag, {buf + eq + 1, e - eq - 1}};
    i = e + 1;
  }
  m.f_[m.n_++] = {10, {buf + trailer + 3, 3}};
  if (m.n_ < 4 || m.f_[2].tag != 35) {
    err = ParseError::Garbled;  // MsgType must be the first body field
    return trailer + 7;
  }
  m.raw_ = {buf, trailer + 7};
  return trailer + 7;
}

// Builds a message body, then frames it. Not thread-safe; reuse one per session.
class Builder {
 public:
  explicit Builder(std::string_view begin = "FIX.4.4") : begin_(begin) { body_.reserve(512); }
  Builder& start(std::string_view msg_type) {
    body_.clear();
    return add(35, msg_type);
  }
  Builder& add(int tag, std::string_view v) {
    char t[16];
    auto r = std::to_chars(t, t + sizeof t, tag);
    *r.ptr++ = '=';
    body_.append(t, r.ptr);
    body_.append(v);
    body_.push_back(SOH);
    return *this;
  }
  Builder& add(int tag, long long v) {
    char b[24];
    auto r = std::to_chars(b, b + sizeof b, v);
    return add(tag, std::string_view(b, static_cast<std::size_t>(r.ptr - b)));
  }
  // Frame into out (replaced). Returns out as a view.
  std::string_view finish(std::string& out) const {
    out.clear();
    out.reserve(body_.size() + begin_.size() + 24);
    out.append("8=").append(begin_).push_back(SOH);
    char b[24];
    auto r = std::to_chars(b, b + sizeof b, static_cast<long long>(body_.size()));
    out.append("9=").append(b, r.ptr).push_back(SOH);
    out.append(body_);
    // One tight pass over the framed message (measured faster than summing inside add(); PERF_LOG).
    unsigned sum = 0;
    for (unsigned char c : out) sum += c;
    char cs[8] = {'1', '0', '=', static_cast<char>('0' + (sum & 255) / 100), static_cast<char>('0' + (sum & 255) / 10 % 10),
                  static_cast<char>('0' + (sum & 255) % 10), SOH, 0};
    out.append(cs, 7);
    return out;
  }
  const std::string& body() const { return body_; }
  // Replace the body with already-encoded fields (must start with 35=...).
  Builder& set_raw(std::string_view body) {
    body_.assign(body);
    return *this;
  }

 private:
  std::string begin_;
  std::string body_;
};

// Human-readable form ('|' for SOH), for logs and tests.
inline std::string pretty(std::string_view raw) {
  std::string s(raw);
  for (char& c : s)
    if (c == SOH) c = '|';
  return s;
}

inline std::string from_pretty(std::string_view s) {
  std::string out(s);
  for (char& c : out)
    if (c == '|') c = SOH;
  return out;
}

}  // namespace fixwire
