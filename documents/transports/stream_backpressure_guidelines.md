<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Stream Backpressure Guidelines

Implementation guidance for stream-backed transports and stream wrappers.

This document is not the source of truth for runtime behaviour. Verify any
claim against the current code in `streaming/`, `transports/`, and the relevant
benchmark or test target before relying on it.

## Purpose

Backpressure exists whenever a producer can generate data faster than the next
layer can accept or drain it.

In Canopy this can happen in several places:

- an SPSC queue becomes full
- a TCP or io_uring send cannot make immediate forward progress
- a wrapper stream such as TLS or WebSocket accumulates framed output faster
  than the underlying stream can flush it
- a receive path has raw bytes available but not yet enough decoded state to
  return a full application message

The common failure mode is not always a true deadlock. It is often one of:

- sustained saturation with slow but valid forward progress
- starvation because the producer does not yield fairly
- message assembly that hides partial progress from the caller
- watchdog logic that measures only high-level completion and misses internal
  progress

## Core Expectations

For `streaming::stream` implementations and wrappers:

- `send()` must tolerate temporary saturation and keep making progress until
  the full span has been accepted or the stream has failed
- `receive()` must tolerate partial delivery and keep assembling protocol state
  until a message is ready, the timeout is genuinely exhausted, or the stream
  closes
- large logical payloads must not require one monolithic flush step if the
  underlying stream can apply backpressure
- progress under saturation must include fair yielding so peer coroutines can
  drain queues and unblock the system
- wrapper streams must treat protocol-generated outbound data as part of normal
  progress, even when it is triggered from a receive path
- if shutdown requires draining or cancelling asynchronous work, expose that as
  an awaitable close operation in normal control flow rather than relying on
  destructors to finish cleanup implicitly

## Send-Side Rules

- Assume one `co_await send(...)` may span many lower-level flush attempts.
- Chunk large framed or encoded output before passing it to the underlying
  stream if that lower layer can saturate.
- If the underlying queue or socket layer cannot currently accept more data,
  yield and retry instead of spin-waiting.
- Do not report success until the whole caller span has been accepted by the
  stream contract for that layer.
- If the stream uses an internal staging buffer, bound its size and drain it in
  increments rather than waiting for one all-or-nothing flush.

Good signs:

- a full queue causes a yield-and-retry loop
- a large WebSocket or TLS frame is flushed in bounded pieces
- watchdog heartbeats can be emitted around repeated flush attempts

Bad signs:

- one very large internal buffer is built and flushed through a single awaited
  send
- send logic assumes the peer will drain without the current coroutine ever
  yielding
- send success is reported after queueing internally even though the stream
  contract promises send-all semantics

## Receive-Side Rules

- Distinguish raw-byte progress from application-message delivery.
- A receive path that consumes bytes, advances decode state, or drains protocol
  control frames has made progress even if it cannot yet return a payload to
  the caller.
- Do not return an empty timeout too eagerly when partial frame or message
  assembly is still active and more raw input is expected within the caller's
  timeout budget.
- Reuse the caller timeout as a deadline, not as permission for only one
  low-level receive attempt.
- Preserve leftover decoded payload when the caller buffer is smaller than the
  logical message.

Good signs:

- `receive()` loops until it can return a decoded message, timeout, or closure
- partial protocol assembly is kept internal and resumed on the next read
- leftover payload is stored explicitly rather than discarded

Bad signs:

- a wrapper reads one raw chunk, decodes part of a frame, then immediately
  returns an empty result to the caller
- a timeout is treated as "no progress happened" even though decode state
  advanced
- control-frame handling queues outbound work but the stream does not flush it

## Fairness And Scheduler Behaviour

- Saturated producer and consumer loops must yield often enough for the peer
  side to run.
- Yield on full queues, transient would-block states, and empty internal queues
  that depend on another coroutine to fill them.
- Avoid tight retry loops that repeatedly inspect local state without allowing
  another coroutine or worker to run.
- When a stream is layered on top of another stream, design both sides so the
  wrapper cannot monopolise execution while waiting for its own staged data to
  flush.

Review question:

Can the blocked side make progress if this coroutine keeps its current control
flow and the scheduler is under load?

If the answer is no, the implementation likely needs an explicit yield or
smaller flush unit.

## Wrapper Stream Rules

These apply to TLS, WebSocket, compression wrappers, and future framed
protocols.

- Wrapper streams must account for both inbound and outbound protocol work.
- A receive call may need to flush protocol-generated outbound bytes before or
  after reading more raw input.
- A send call may need to emit many lower-level writes for one logical message.
- Wrapper logic must preserve the single-executor or non-concurrent contract it
  documents. If concurrent `send()` and `receive()` are not supported, say so
  clearly and implement accordingly.
- Wrapper staging buffers should be bounded and drained incrementally.

Typical hazards:

- ping, pong, close, TLS handshake, or framing output is queued but never
  flushed because only `send()` drives the write pump
- a large encoded frame saturates the underlying stream and the wrapper exposes
  that as a hang instead of incremental progress
- the client and server wrappers use different progress rules, making benchmark
  behaviour asymmetric

## Timeouts And Watchdogs

Timeouts and watchdogs are different tools.

- A receive timeout answers "how long may this operation wait for more input?"
- A watchdog answers "has the overall system stopped making observable
  progress?"

Guidelines:

- keep watchdog thresholds comfortably above legitimate per-receive timeouts
- do not treat a watchdog firing as proof of a deadlock without checking
  whether internal progress was being made but not reported
- emit progress signals around long-running send and receive loops, especially
  when large payloads are fragmented internally
- include stream type, scenario, and blob size in watchdog context when
  benchmarking

## Diagnostic Guidance

When a stream appears hung under load:

- check whether the sender is blocked in one large flush
- check whether the receiver is assembling a large message but returning empty
  receives too early
- check whether full-queue or would-block paths yield fairly
- check whether wrapper-generated outbound data is flushed during receive paths
- check whether the watchdog threshold is below a legitimate operation timeout
- compare raw-byte progress with application-message progress

Useful instrumentation points:

- before and after each awaited lower-level send
- before and after each awaited lower-level receive
- when internal staging buffers grow, shrink, or drain fully
- when a queue-full or would-block path yields
- when partial decode state advances without producing a caller-visible message
- when shutdown begins, when in-flight async operations are cancelled, and when
  close completion becomes observable to the caller

## Shutdown Rules

- Prefer explicit, awaitable shutdown over destructor-driven cleanup when a
  stream owns asynchronous state such as io_uring submissions, worker tasks, or
  wrapper protocol teardown.
- A caller that is responsible for stream lifetime should be able to await
  shutdown completion before dropping the last owning reference.
- `set_closed()` or a future renamed close API should complete only after the
  stream has reached a stable closed state for that implementation.
- Destructors should remain a safety net, not the primary mechanism for
  draining async work.
- Wrapper streams should propagate close to the underlying stream as part of
  the same awaited shutdown path.

## Review Checklist

Use this checklist when reviewing `spsc_queue`, `tcp`, `io_uring`, WebSocket,
TLS, or future stream implementations.

- Does `send()` handle saturation with retry and fair yielding?
- Can large logical payloads be flushed incrementally?
- Does `receive()` use the caller timeout as a deadline rather than a single
  probe?
- Is partial decode progress preserved and resumed correctly?
- Are leftover bytes or message fragments retained safely?
- Can protocol-generated outbound work flush from receive paths when needed?
- Are wrapper staging buffers bounded?
- Is there any path that can spin without yielding while waiting for peer-side
  progress?
- Are watchdog and timeout settings interpreted separately?
- Would the implementation behave symmetrically on both ends of a benchmarked
  stream pair?

## Applying This Guidance

Apply these rules first to:

- `streaming::spsc_queue::stream`
- `streaming::tcp::stream`
- `streaming::io_uring::stream`
- `streaming::tls::stream`
- `streaming::websocket::stream`
- benchmark-local stream wrappers used to exercise those transports

For any concrete fix, verify the final behaviour in code and with the smallest
relevant benchmark or test target rather than relying on this document alone.

## Annex: 2026 io_uring Fullstack Investigation

This annex records a concrete debugging session that affected
`streaming::io_uring::stream`, `rpc::stream_transport::transport`, and the
io_uring fullstack benchmark wiring. It is explanatory history, not a source of
truth. Check the current code before assuming every detail here still applies.

### Observed Symptoms

The original symptom was an intermittent hang in the io_uring fullstack
benchmark under larger payloads. During investigation the behaviour presented in
several different ways:

- fullstack benchmark iterations that stopped making forward progress part way
  through large request/reply traffic
- occasional response deserialisation failures in YAS
- shutdown paths that timed out waiting for the peer to finish closing
- a temporary regression where `type_test/18.initialisation_test` stalled in
  transport teardown

Those symptoms were related, but they did not all have the same immediate
cause.

### Root Cause 1: io_uring Timed Receive Completion

The first real bug was in `streaming::io_uring::stream`.

The timed receive path used a linked timeout SQE. A timeout CQE could complete
the waiting receive coroutine before the actual recv CQE or its cancel CQE had
arrived. That meant the caller could observe timeout completion, reuse the
buffer, and then still have the kernel-side recv resolve afterwards.

Under load this was enough to corrupt higher-level framing and produce
deserialisation failures.

The fix was:

- timeout CQEs no longer complete the recv operation directly
- timeout only marks the pending recv as expired
- the recv operation is completed only when the recv CQE itself arrives
- if the recv CQE arrives as `-ECANCELED` after timeout expiry, that result is
  translated to `-ETIME`

This keeps ownership of the receive buffer aligned with the actual kernel recv
completion and removes the framing corruption path.

### Root Cause 2: Producer Progress / Wakeup

The second real bug was in the streaming transport send producer.

The transport had one producer coroutine draining an internal outbound queue.
Under stress we captured a case where a large `call_send` request was enqueued
and awaited, but the producer never dequeued it. This left the caller waiting
for a response that could never arrive even though the stream itself had not
failed.

The old design depended on scheduler polling of an empty queue. That was too
weak under load.

The corrected design is:

- enqueueing outbound payloads explicitly signals a producer-ready event
- the producer resets that event only while holding the queue lock and only
  after it has observed the queue empty
- after reset, the producer yields once and re-checks status and readiness
  before actually awaiting the event
- status transitions out of `CONNECTED` also signal the producer so shutdown
  cannot leave it parked forever

This change fixed the benchmark-side producer stall without relying on hot-path
polling.

### Why Priority Still Matters

During debugging it became clear that ordinary outbound requests and responder
traffic currently share the same producer path. That was not the primary cause
of the captured stall above, but it is still an architectural concern.

Responses created while handling inbound RPCs should conceptually have higher
priority than fresh outbound requests. In particular, the following classes of
messages are candidates for a high-priority outbound lane:

- `call_receive`
- `try_cast_receive`
- `addref_receive`
- init / channel setup responses
- close / control acknowledgements such as `close_connection_ack`

The reason is simple: once a request has already been accepted and dispatched,
its response is part of completing existing work, not starting new work.

This priority split was identified as a worthwhile follow-up improvement, but
it was not the fix that resolved the reproduced benchmark stall in this
investigation.

### Benchmark Wiring Lesson

The benchmark initially depended on a listener startup pattern where callers had
to do an extra scheduler yield after `start_listening()` in order to let the
listener run loop enter `accept()`.

That was a poor contract. Listener readiness should be owned by the listener,
not by each benchmark or demo.

The listener API was therefore extended with an awaitable startup path:

- `start_listening_async(service)` now starts the listener and waits until the
  run loop has passed its internal initial yield and is considered ready
- the io_uring benchmark now uses that awaitable startup path rather than doing
  an ad hoc `schedule()` after synchronous startup

This was not the root cause of the hang, but it removed a fragile benchmark
workaround and made startup semantics clearer.

### Regression During Fixes

One intermediate producer-wakeup change caused a regression in
`type_test/18.initialisation_test`.

That regression happened because the first wakeup implementation could still
miss an immediate shutdown transition during a very small handshake/teardown
scenario. The fix was not to revert to blind polling, but to combine explicit
event signalling with a schedule-and-recheck step before sleeping.

This is a useful reminder:

- a fix that is correct under sustained benchmark load can still break
  short-lived teardown-sensitive tests
- small initialisation tests and large stress benchmarks exercise different
  edges of the same concurrency design

### Practical Lessons

- Treat io_uring timeout CQEs and recv CQEs as distinct phases of one logical
  operation. Do not resume user code until the recv itself is resolved.
- If an internal producer queue exists, do not depend on opportunistic polling
  alone for forward progress.
- When using wake events, couple reset and queue observation under the same
  lock or equivalent critical section.
- Shutdown paths must wake any idle producer/consumer tasks that are part of
  completing close.
- Benchmark-specific startup workarounds are often a sign that the underlying
  API contract is too weak.
- Heavy hot-path `std::cerr` tracing can make a healthy benchmark look hung.
  Prefer sparse or timeout-triggered diagnostics once the rough fault location
  is known.

### Resulting Guidance

For stream-backed transports in Canopy, the practical outcome of this
investigation is:

- `stream::set_closed()` should remain the awaited, explicit close path for
  implementations with asynchronous state
- recv timeout logic must not expose partially cancelled kernel operations as if
  they were already complete
- outbound producer loops should use explicit readiness signalling rather than
  assuming scheduler fairness alone will eventually drain queues
- listener readiness should be represented by the listener API itself, not by
  caller-local scheduling folklore
