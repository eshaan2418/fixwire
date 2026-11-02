# fixwire

**FIX 4.4 engine in C++20.** A zero-copy codec with exact BodyLength/CheckSum validation, and a
full session layer: logon, heartbeats and test requests, sequence-number gap recovery with
ResendRequest/PossDup/GapFill, and a crash-safe file store. Interoperability is verified against
**QuickFIX** with data-dictionary validation on, in both directions and through a
crash-and-restart.

[![CI](https://github.com/eshaan2418/fixwire/actions/workflows/ci.yml/badge.svg)](https://github.com/eshaan2418/fixwire/actions/workflows/ci.yml)

> Educational/research project. Not investment advice. No live trading. fixwire only ever talks
> to its own toy exchange and to a local QuickFIX in tests.

## Headline results
Numbers come from [`interop_quickfix.json`](research/results/interop_quickfix.json),
[`latency.json`](bench/results/latency.json) and [`bench.json`](bench/results/bench.json).

| what | result |
|---|---|
| fixwire initiator → **QuickFIX acceptor** (FIX44 dictionary validation on) | 20,000 orders accepted, **20,000 fills**, **0 session rejects**, 0 duplicates |
| **QuickFIX initiator** → fixwire acceptor | 5,000 orders, 5,000 fills, 0 rejects |
| Crash mid-stream against QuickFIX, restart from file store | 600 orders sent, 550 fills seen, crash; restart recovers the **50 missed fills** via ResendRequest/PossDup, then 400 more orders: **exactly 1,000 fills for 1,000 orders, 0 duplicates** |
| Randomised fault injection (drops, garbling; 20 seeds × 300 orders) | every order delivered **exactly once, in order** |
| Loopback order → ack → fill round trip, 1 outstanding | p50 **20 µs**, p99 71 µs |
| Throughput, 16 outstanding | **191k orders/s** (574k FIX messages/s) |
| Codec | parse + validate 70–86 ns/message (2.2 GB/s); build 170 ns |
| File store fix | 2,055 → **46,167 orders/s** by replacing a rename-per-message with an in-place `pwrite` |

## Session behaviour
| situation | what fixwire does |
|---|---|
| MsgSeqNum too high | queue, one ResendRequest(expected, 0); retry if no progress after HeartBtInt |
| too low with PossDup | ignore the duplicate |
| too low without PossDup | Logout "MsgSeqNum too low", close |
| ResendRequest received | replay app messages with 43=Y + OrigSendingTime; admin messages and holes → SequenceReset-GapFill |
| silence | Heartbeat at HeartBtInt, TestRequest at 1.2×, disconnect at 2.4× with nothing received |
| Logon with 141=Y | both sides restart at 1 |
| restart | FileStore: append-only message log flushed before each send + in-place seqnum record |

## Demo (real output)
```
$ fixwire-client --port 19895 --orders 20000 --window 16
{"orders_sent": 20000, "fills_received": 20000, "duplicate_fill_reports": 0, "orders_per_sec": 78297,
 "rtt_us": {"p50": 138.2, "p99": 3070.7}, ...}
fixwire-exchange: logon
fixwire-exchange: session ended: received 20001, app 20000, sent 40001, ...
```
(See [`demo_output.txt`](research/results/demo_output.txt) for the full run.)

## Run it
```bash
make build test sanitize   # GoogleTest: codec (framing, checksum, resync), 11 session scenarios incl. fault injection + restart
make bench                 # codec micro-benchmarks
make interop               # needs `pip install quickfix`: both directions + crash/restart/resend
make demo                  # exchange + client on loopback
make fuzz                  # libFuzzer: parser + session state machine (clang with fuzzer runtime; runs in CI)
```

```cpp
#include "fixwire/net.hpp"
fixwire::FileStore store("store/");
fixwire::Session s({"FIX.4.4", "CLIENT", "EXCH", 30, /*initiator=*/true}, store, fixwire::wall_ms);
s.on_app = [](const fixwire::Message& m) { /* ExecutionReport: m.get(150), m.get(11) ... */ };
fixwire::Builder o;
o.start("D").add(11, "ord-1").add(55, "AAPL").add(54, "1").add(60, fixwire::fix_time(fixwire::wall_ms())).add(38, 100LL).add(40, "2").add(44, "187.25");
// after logon: s.send(o);
```

## Documentation
- [docs/DESIGN.md](docs/DESIGN.md): codec, the session state table, stores, liveness
- [docs/MICROSTRUCTURE.md](docs/MICROSTRUCTURE.md): where FIX sits, why sequenced sessions
  matter, latency
- [docs/DEEP_DIVE.md](docs/DEEP_DIVE.md): 11 interview-style questions and answers
- [docs/PERF_LOG.md](docs/PERF_LOG.md): latency/throughput, the store fix, a rejected
  optimisation, the bugs found

## Where it fits
fixwire is the order-entry protocol layer of a small research trading stack. An OMS such as
[sorx](https://github.com/eshaan2418/sorx) would speak FIX through it to a venue such as
[matchbox](https://github.com/eshaan2418/matchbox).

## License
MIT. QuickFIX (used only in tests) is under the QuickFIX Software License.
