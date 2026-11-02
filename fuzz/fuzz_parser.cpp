// libFuzzer target: arbitrary bytes through the parser (must never read out of bounds) and
// through a session (must never crash, whatever a counterparty sends).
#include <cstdint>
#include <string>

#include "fixwire/session.hpp"

using namespace fixwire;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const char* p = reinterpret_cast<const char*>(data);
  Message m;
  ParseError err;
  std::size_t off = 0;
  while (off < size) {
    const std::size_t used = parse(p + off, size - off, m, err);
    if (err == ParseError::Incomplete) break;
    if (used == 0) __builtin_trap();  // the parser must always make progress
    off += used;
  }
  MemoryStore store;
  std::int64_t now = 0;
  Session s({"FIX.4.4", "EXCH", "CLIENT", 30, false}, store, [&] { return now; });
  s.send_bytes = [](std::string_view) {};
  s.connected();
  // Prefix a valid logon so the fuzzer explores the active-session state machine too.
  Builder b;
  std::string logon;
  b.start("A").add(49, "CLIENT").add(56, "EXCH").add(34, 1LL).add(52, "20261102-12:00:00.000").add(98, 0LL).add(108, 30LL);
  b.finish(logon);
  s.on_bytes(logon.data(), logon.size());
  s.on_bytes(p, size);
  now += 100000;
  s.on_timer();
  return 0;
}
