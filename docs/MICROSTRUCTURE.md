# Market structure notes: FIX in electronic trading

## Where FIX sits
FIX (Financial Information eXchange) is the lingua franca of order flow between buy side, brokers
and venues: NewOrderSingle, cancels and replaces, and execution reports, carried on long-lived
TCP sessions. Market data mostly moved to binary protocols (ITCH, SBE/MDP3), and many venues
offer binary order entry (OUCH, iLink 3 SBE) for latency. FIX remains the default for
broker/client connectivity, drop copies and most venues' standard order entry.

## Why the session layer matters more than the codec
A FIX session is a *stateful, sequenced* conversation that outlives any single TCP connection:
- **Sequence numbers** persist across reconnects and process restarts, so neither side silently
  loses a message. A disconnect mid-stream is resolved by ResendRequest, not by guessing.
- **PossDup and OrigSendingTime** mark replays, so an order router can tell a duplicate execution
  report from a new fill. Double-counting a fill is a position break.
- **Gap fills.** Administrative messages are gap-filled rather than replayed: a five-minute-old
  heartbeat means nothing, but a five-minute-old order still must not be sent twice.
- **Daily reset.** Sessions usually reset sequence numbers once a day (ResetSeqNumFlag or a
  scheduled reset); within the day they must survive anything.

The crash-and-restart interop test shows exactly this. An initiator dies with 50 orders whose
fills it never saw. It restarts from its file store, logs on at the right sequence number, asks
QuickFIX to resend, and ends with all 1,000 fills, each counted once.

## Latency
- **Here.** A loopback round trip (order → ack → fill) with one order outstanding has a p50 of
  20 µs, dominated by two kernel TCP traversals each way and the `poll()`/`read()` syscalls. The
  codec itself takes ~70 ns to parse and ~170 ns to build a message.
- **Venue-grade.** Production engines go further with kernel-bypass networking (Onload,
  DPDK), pre-built message templates (only the volatile fields rewritten), and busy-polling cores.
  Even then FIX's text encoding costs a few hundred nanoseconds more than binary protocols, which
  is why the latency-critical order-entry paths at many venues are binary.

## Pre-trade risk and drop copies
- **Risk checks.** Brokers must run pre-trade risk checks before orders reach a venue (SEC Rule
  15c3-5 in the US), typically inside or right next to the FIX gateway.
- **Drop copies.** Copies of every execution report go to risk and back-office systems, on FIX
  sessions with the same recovery guarantees.

## Conduct
fixwire talks only to its own toy exchange and to a local QuickFIX instance in tests. It is not
connected to any broker or venue. Educational/research project. Not investment advice. No live
trading.
