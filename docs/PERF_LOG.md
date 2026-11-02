# fixwire performance log

Machine: Apple M5, Apple clang -O3, loopback TCP, shared with another workload (load average
about 4.6 during the saved sweeps).

## Codec (`bench/results/bench.json`)
| operation | time |
|---|---|
| parse + validate NewOrderSingle (163 B) | 70 ns |
| parse + validate ExecutionReport (192 B) | 86 ns (2.2 GB/s) |
| build + frame ExecutionReport | 170 ns |

**Rejected optimisation:**
- **The change.** The checksum was summed inside `add()` instead of one pass in `finish()`.
- **Result.** It measured 222 ns against a 173 ns baseline taken minutes earlier
  (`bench_rejected_incremental_checksum*.json`), so it was reverted.
- **Noise.** An even earlier 422 ns reading for the original code was taken under heavy load.

## Loopback order → ack → fill round trips (`bench/results/latency.json`, 50,000 orders each)
| outstanding orders | store | orders/s | RTT p50 | p99 |
|---|---|---|---|---|
| 1 | memory | 37,843 | 20.0 µs | 71 µs |
| 4 | memory | 140,470 | 25.2 µs | 57 µs |
| 16 | memory | 191,470 | 78.6 µs | 133 µs |
| 64 | memory | 173,984 | 310 µs | 1,427 µs |
| 64 | file | 35,902 | 874 µs | 2,059 µs |

Each order produces 3 FIX messages (order, ack, fill), so 191k orders/s is about 574k
messages/s through two sessions.

## File store: 22× from removing a rename per message
| version | orders/s (window 64, 20,000 orders) | source |
|---|---|---|
| seqnums via temp file + rename() on every message | 2,055 | `research/results/filestore_before.json` |
| fixed-width seqnums record, in-place `pwrite` | 46,167 | `research/results/filestore_after.json` |

## Correctness journey
| issue | found by | fix |
|---|---|---|
| Resync required the frame start to follow an SOH; garbage before a frame lost the whole buffer | codec test | search for `8=FIX.4.4␁` at any offset |
| Logon with 141=Y rejected as "seq too low" before the reset applied | test | apply ResetSeqNumFlag before sequence checks |
| A Logon queued behind a gap was delivered to the application | restart test | sequence-only handling for queued Logon/Heartbeat |
| Lost ResendRequest wedged recovery forever | fault-injection test | re-issue after a heartbeat interval without progress |
| Session declared a live peer dead when the TestRequest's heartbeat was lost | fault-injection test | any received message proves liveness; 2.4 × HeartBtInt rule |
| Exchange crashed (uncaught exception) when the client disconnected mid-write | crash/restart run | write errors end the connection |
| Client stopped early after restart (recovered fills counted against new orders) | crash/restart run | count recovered fills separately |
| Second benchmark session rejected "seq too low" | window sweep | correct FIX behaviour; `--reset` logs on with 141=Y |
