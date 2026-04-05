# Remaining Priorities

1. `pending_transmits_` lifetime on disconnect/cancel regression test still open.
   A targeted test needs a stream that can stall the server-side response so callers are genuinely in-flight when disconnect fires. The SPSC setup completes calls too fast for reliable interruption. Requires either a slow-server fixture or direct stream-close access from the test.

2. Lower-priority performance cleanup
   Still worth revisiting later:
   - `queued_send_message` heap-copy overhead in streaming transport (`prefix_data`/`payload_data` are `std::vector<uint8_t>`)
   - any remaining avoidable buffer churn on large payload paths

## Component-level todos

- `c++/streaming/spsc_wrapping/todo.md` — cancellation/lifetime, error semantics, receive native errors
- `c++/streaming/io_uring/todo.md` — timed receive timeout/cancel association test
