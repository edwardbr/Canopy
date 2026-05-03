<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Streaming Result Listener

This note describes the pending-call listener used by the C++ streaming
transport. It is design background only; the source of truth is
`c++/transports/streaming/include/transports/streaming/transport.h`.

## Current Shape

Each outbound streaming RPC call creates a `result_listener` and inserts it into
the transport's `pending_transmits_` map before sending the encoded request. The
listener is completed by one of three paths:

- the receive loop finds a matching response sequence number
- the timeout sweep expires the pending call
- disconnect cleanup cancels outstanding calls

The caller then awaits the listener. The listener mutex protects the handoff
between `await_ready()`, `await_suspend()`, and `complete()`. Without that
handoff, a response can arrive after `await_ready()` observes an incomplete
listener but before `await_suspend()` stores the coroutine handle. The completer
would see no continuation to resume, and the caller could suspend forever.

The scheduler is not part of that race. It is construction-time state captured
before the listener is published in `pending_transmits_`.

## Lock-Free Candidate

A lock-free version should use an explicit atomic state machine rather than an
atomic `done` flag.

One possible model:

- `initial`: no continuation is registered and no result is ready
- `waiting`: a continuation has been registered by `await_suspend()`
- `complete`: the result has been published

The result storage must be written before the state is changed to `complete`
with release semantics. `await_resume()` must observe `complete` with acquire
semantics before moving the result payload out.

The transitions would be:

- `await_ready()` acquires the state and returns true only for `complete`
- `await_suspend(handle)` CASes `initial -> waiting`; if the state is already
  `complete`, it returns false so the caller does not suspend
- `complete(result)` writes the result, then CASes either `initial -> complete`
  or `waiting -> complete`
- if `complete()` observes `waiting`, it resumes the stored continuation after
  publishing the result
- if another completion path observes `complete`, it must no-op; timeout,
  disconnect cleanup, and a late response must not be able to publish a second
  result

This needs careful treatment of the coroutine handle storage. The handle must be
visible to the completer before `waiting` is published, and the result must be
visible to the awaiter before the continuation is resumed. A packed atomic state
plus separate handle/result storage may be enough, but it should be stress
tested under TSAN and repeated timeout/disconnect races before replacing the
mutex-backed implementation.
