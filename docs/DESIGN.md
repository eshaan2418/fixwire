# fixwire design

## Layers
```
codec.hpp    tag=value parse (zero-copy views) and build; exact BodyLength and CheckSum
session.hpp  FIX 4.4 session protocol: logon, heartbeats, sequence numbers, gap recovery, stores
net.hpp      blocking connect/accept + poll() loop for one session over TCP
tools/       fixwire-exchange (acceptor that acks + fills), fixwire-client (initiator, latency), fixwire-bench
```
The session layer is a pure state machine: it is fed bytes and clock ticks, and emits bytes and
in-order application messages. All of its tests run without sockets, on an injected clock,
through a link that can drop, garble or delay frames.

## Codec
- **Parsing.** Messages are framed by `8=FIX.4.4␁9=<n>␁ … 10=<ccc>␁`.
  - The parser checks the BeginString, reads BodyLength, and verifies that the trailer sits
    exactly where BodyLength says. It then checks the CheckSum (byte sum mod 256 of everything
    before `10=`).
  - Fields are returned as `string_view`s into the receive buffer, so nothing is copied.
- **Error handling.**
  - Incomplete input returns 0 bytes consumed, so the caller waits for more data.
  - A well-delimited frame with a bad checksum or bad field is consumed exactly. The session
    counts it, and the resulting sequence gap triggers a resend.
  - A bad header resynchronises to the next `8=FIX.4.4␁` at any offset. That case was found by
    a test: after garbage, a frame start can follow any byte.
- **Building.** The builder appends `tag=value␁` with `to_chars`. `finish` adds BeginString and
  BodyLength and computes the checksum in one pass.

## Session protocol
| situation | behaviour |
|---|---|
| connect (initiator) | Logon (98=0, 108=HeartBtInt[, 141=Y]) |
| first message is not a Logon | Logout + close |
| CompIDs don't match | Logout + close |
| MsgSeqNum == expected | process, expected += 1, drain any queued messages |
| MsgSeqNum > expected | queue the message, send one ResendRequest(expected, 0); a ResendRequest or Logout is acted on immediately |
| MsgSeqNum < expected, PossDupFlag=Y | ignore (duplicate) |
| MsgSeqNum < expected, no PossDup | Logout "MsgSeqNum too low" + close |
| SequenceReset-GapFill | expected = NewSeqNo (must exceed MsgSeqNum) |
| SequenceReset-Reset | expected = NewSeqNo regardless of MsgSeqNum |
| ResendRequest received | resend stored application messages with 43=Y and 122=original SendingTime; admin messages and holes become SequenceReset-GapFill spans |
| no outgoing traffic for HeartBtInt | Heartbeat |
| nothing received for 1.2 × HeartBtInt | TestRequest |
| nothing received for 2.4 × HeartBtInt | close (peer dead), as QuickFIX does |
| a gap makes no progress for HeartBtInt | re-issue ResendRequest from the current expected number |
| Logon with 141=Y | both sides restart at 1 before any sequence check |

**Liveness.** *Any* valid incoming message proves the peer alive. The Heartbeat that answers a
TestRequest may itself be lost, because admin messages are gap-filled rather than replayed, so
waiting for that specific Heartbeat would kill healthy sessions.

## Stores
- **MemoryStore:** sequence numbers and sent messages in memory.
- **FileStore:** survives restarts.
  - `messages` is an append-only log of (seq, length, bytes), flushed to the kernel before each
    message goes on the wire, so anything the counterparty may have received can be resent
    after a crash.
  - `seqnums` is one fixed-width record rewritten in place with `pwrite`.
  - `fsync_each` adds machine-crash durability.
- **Torn tail.** A torn final record (a crash mid-write) is ignored on reload.

## What is not here
Repeating groups beyond flat fields, FIXT 1.1 / FIX 5.0 application versioning, SSL,
multi-session engines, message validation against a data dictionary (QuickFIX does that, and
the interop tests rely on it to validate fixwire's output), and order-book logic in the exchange
(it fills every order at its limit).
