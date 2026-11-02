#include <gtest/gtest.h>

#include "fixwire/codec.hpp"

using namespace fixwire;

// A message from the FIX 4.4 spec examples style, framed by hand-computed checksum.
static std::string sample() {
  Builder b;
  b.start("D").add(49, "CLIENT").add(56, "EXCH").add(34, 7LL).add(52, "20261102-12:00:00.000").add(11, "ord-1").add(55, "AAPL").add(54, "1").add(38, 100LL).add(40, "2").add(44, "187.25");
  std::string out;
  b.finish(out);
  return out;
}

TEST(Codec, BuildThenParseRoundTrip) {
  const std::string s = sample();
  Message m;
  ParseError err;
  ASSERT_EQ(parse(s.data(), s.size(), m, err), s.size());
  ASSERT_EQ(err, ParseError::None);
  EXPECT_EQ(m.msg_type(), "D");
  EXPECT_EQ(m.get(55), "AAPL");
  EXPECT_EQ(m.get_int(38), 100);
  EXPECT_EQ(m.get_int(34), 7);
  EXPECT_EQ(m[0].tag, 8);
  EXPECT_EQ(m[m.size() - 1].tag, 10);
  // BodyLength counts from after 9=..| to the SOH before 10=.
  const auto p9 = s.find("\x01" "35=");
  const auto p10 = s.find("\x01" "10=");
  EXPECT_EQ(m.get_int(9), static_cast<long long>(p10 - p9));
}

TEST(Codec, KnownChecksum) {
  // Independently computed: sum of bytes mod 256 for this exact frame.
  const std::string s = from_pretty("8=FIX.4.4|9=5|35=0|10=163|");
  unsigned sum = 0;
  for (std::size_t i = 0; i < s.find("10="); ++i) sum += static_cast<unsigned char>(s[i]);
  ASSERT_EQ(sum % 256, 163u);
  Message m;
  ParseError err;
  EXPECT_EQ(parse(s.data(), s.size(), m, err), s.size());
  EXPECT_EQ(err, ParseError::None);
}

TEST(Codec, IncompleteThenComplete) {
  const std::string s = sample();
  Message m;
  ParseError err;
  for (std::size_t cut = 0; cut < s.size(); ++cut) {
    EXPECT_EQ(parse(s.data(), cut, m, err), 0u) << cut;
    EXPECT_EQ(err, ParseError::Incomplete) << cut;
  }
}

TEST(Codec, RejectsCorruption) {
  const std::string good = sample();
  Message m;
  ParseError err;
  std::string bad = good;
  bad[good.find("AAPL")] = 'B';  // payload flip -> checksum mismatch, frame still delimited
  EXPECT_EQ(parse(bad.data(), bad.size(), m, err), bad.size());
  EXPECT_EQ(err, ParseError::BadChecksum);
  bad = good;
  {  // body length one larger than the real body
    const auto p = bad.find("\x01" "9=") + 3, e = bad.find('\x01', p);
    bad.replace(p, e - p, std::to_string(std::stoi(bad.substr(p, e - p)) + 1));
  }
  parse(bad.data(), bad.size(), m, err);
  EXPECT_NE(err, ParseError::None);
  bad = "garbage" + good;  // resynchronise to the next frame
  std::size_t used = parse(bad.data(), bad.size(), m, err);
  EXPECT_NE(err, ParseError::None);
  EXPECT_GT(used, 0u);
  std::size_t off = used;
  while (err != ParseError::None && off < bad.size()) {
    used = parse(bad.data() + off, bad.size() - off, m, err);
    if (err == ParseError::None) break;
    off += used;
  }
  EXPECT_EQ(err, ParseError::None);
  EXPECT_EQ(m.get(55), "AAPL");
  const std::string wrongver = from_pretty("8=FIX.4.2|9=5|35=0|10=161|");
  parse(wrongver.data(), wrongver.size(), m, err);
  EXPECT_EQ(err, ParseError::BadBeginString);
}

TEST(Codec, TwoMessagesInOneBuffer) {
  const std::string s = sample() + sample();
  Message m;
  ParseError err;
  const std::size_t a = parse(s.data(), s.size(), m, err);
  ASSERT_EQ(err, ParseError::None);
  const std::size_t b = parse(s.data() + a, s.size() - a, m, err);
  ASSERT_EQ(err, ParseError::None);
  EXPECT_EQ(a + b, s.size());
}
