# Remaining Priorities

1. `pending_transmits_` lifetime on disconnect/cancel regression test still open.
   A targeted test needs a stream that can stall the server-side response so callers are genuinely in-flight when disconnect fires. The SPSC setup completes calls too fast for reliable interruption. Requires either a slow-server fixture or direct stream-close access from the test.

2. Lower-priority performance cleanup
   Still worth revisiting later:
   - `queued_send_message` heap-copy overhead in streaming transport (`prefix_data`/`payload_data` are `std::vector<uint8_t>`)
   - any remaining avoidable buffer churn on large payload paths

3. Release coroutine streaming benchmark follow-up
   Reproduce and fix the current release coroutine benchmark failures seen in
   `build_release_coroutine`:
   - `streaming_benchmark --stream io_uring --scenario send_reply --blob-size 1024`
     currently reports `error`
   - `streaming_benchmark --stream tcp --scenario send_reply --blob-size 1024`
     currently hits the watchdog and aborts, indicating a likely send-side
     deadlock

## Component-level todos

- `c++/streaming/spsc_wrapping/todo.md` — cancellation/lifetime, error semantics, receive native errors
- `c++/streaming/io_uring/todo.md` — timed receive timeout/cancel association test
- `c++/streaming/tcp/todo.md` — investigate release coroutine send-reply benchmark watchdog/deadlock
