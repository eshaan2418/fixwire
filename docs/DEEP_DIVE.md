# fixwire: deep-dive interview questions

### 1. How is a FIX message framed and validated?
- **Frame.** `8=FIX.4.4␁9=<n>␁` then the body (starting `35=`), then `10=<ccc>␁`. BodyLength n
  counts the bytes after the `9=` field up to and including the SOH before `10=`. CheckSum is
  the byte sum of everything before `10=`, mod 256, as three digits.
- **Checks.** The parser verifies that the trailer is exactly where BodyLength says, then the
  checksum.
- **Errors.** A bad but well-delimited frame is dropped whole, and the sequence gap it leaves is
  repaired by a resend.

### 2. What happens when a message with too high a MsgSeqNum arrives?
A message was lost (or garbled). Queue it, send ResendRequest(BeginSeqNo = expected,
EndSeqNo = 0, meaning "to the end"), and process the queue in order as the gap fills. Send only
one ResendRequest per gap. If the gap makes no progress for a heartbeat interval (the request or
its replies were lost), ask again. That retry came out of a fault-injection test that otherwise
wedged.

### 3. And too low?
With PossDupFlag=Y it is a replay we already processed, so ignore it. Without PossDup it means
the counterparty has the wrong sequence state (for example it restarted without its store).
Continuing would risk processing orders twice, so the session logs out with "MsgSeqNum too
low" and closes.

### 4. Why are admin messages gap-filled instead of resent?
- **Admin messages.** Replaying a heartbeat, test request or logon has no meaning. A
  SequenceReset-GapFill(NewSeqNo) advances the counterparty's expected number over them, so one
  GapFill covers a whole run.
- **Application messages.** These are replayed with PossDupFlag=Y and OrigSendingTime, so the
  receiver can detect duplicates.

### 5. How does a crash not lose or duplicate fills?
- **Before sending.** Each outgoing message is appended to the file store and flushed to the
  kernel before it is written to the socket. After a process crash, anything the counterparty
  might have seen can be resent.
- **On restart.** The client resumes at the stored sequence numbers. The counterparty's
  messages beyond the stored next_in trigger a ResendRequest, and the fills arrive with
  PossDup. The client dedupes by ClOrdID and fill.
- **Measured.** Interop with QuickFIX: 600 orders, crash, restart, 400 more. Exactly 1,000 fills,
  0 duplicates, and the 50 missed fills recovered.

### 6. Why did the file store start out 22× slower, and what fixed it?
- **Cause.** It rewrote the sequence-number file through a temp file plus rename() on every
  message, a filesystem metadata operation each time: 2,055 orders/s.
- **Fix.** A fixed-width record rewritten in place with one `pwrite`: 46,167 orders/s in the
  first measurement (35,902 in the saved sweep, under load). It is still process-crash safe;
  `fsync_each` adds power-loss safety at a much higher cost.

### 7. Why is liveness "any message received", not "the heartbeat that answers my TestRequest"?
That heartbeat can be lost like any message, and admin messages are not replayed, so waiting
for it specifically can kill a healthy session. That happened in the fault-injection test. As in
QuickFIX: a TestRequest after 1.2 × HeartBtInt of silence, and disconnect after 2.4 × with
nothing received.

### 8. How did you test against an independent implementation?
QuickFIX (C++ engine, Python bindings) with its FIX 4.4 data dictionary validation on, in both
directions:
- fixwire initiator → QuickFIX acceptor: 20,000 orders, 20,000 fills;
- QuickFIX initiator → fixwire acceptor: 5,000 orders, 5,000 fills;
- 0 session-level rejects, so every fixwire message passed QuickFIX's required-field and format
  validation.

### 9. Where does the round-trip time go?
With one order outstanding on loopback the p50 is 20 µs. Most of it is kernel TCP (four
traversals) plus `poll`/`read`/`write` syscalls: about 70 ns to parse and 170 ns to build each
message, times 3 messages per order. Throughput scales with the window: 38k orders/s at 1
outstanding, 191k at 16, which is 574k FIX messages/s counting both execution reports.

### 10. You tried to optimise the builder and it got slower. What happened?
- **The hypothesis.** Accumulating the checksum while appending fields would save a second pass
  in `finish`.
- **Measured.** 222 ns against 173 ns before. One tight byte loop over ~190 bytes in `finish` is
  cheaper than a byte loop interleaved with every `add`.
- **The earlier "422 ns".** It was noise from a loaded machine.

The change was reverted, and the rejected numbers are kept in `bench/results`.

### 11. What would a production engine add?
- Persistence of the incoming message log for audit.
- Scheduled session times and daily resets.
- Pre-built message templates.
- Kernel bypass.
- SSL.
- Throttling and pre-trade risk at the gateway.
- Multi-session event loops.
- Data-dictionary validation of incoming messages.
- FIXT 1.1 / FIX 5.0 SP2.
