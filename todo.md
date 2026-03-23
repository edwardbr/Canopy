# Remaining Priorities

1. `spsc_wrapping` cancellation/lifetime
   The review item about `recv_proxy_loop` holding `shared_ptr<stream>` forever is still real. If the underlying stream never returns and nobody drives `set_closed()`, the wrapper can self-retain indefinitely. This needs an explicit cancellation/stop path, not just atomic flags.

2. Reply/control priority in `stream_transport`
   Replies and control traffic should not compete fairly with outbound requests. Split outbound queues into:
   - high priority: `call_receive`, `try_cast_receive`, `addref_receive`, init responses, close/control replies
   - normal priority: `call_send`, posts, ordinary outbound work

3. Listener shutdown hardening beyond the late-accept fix
   The obvious post-stop spawn race is fixed, but the listener still needs a tighter shutdown model around accept loop ownership and readiness/stop sequencing.

4. `spsc_wrapping` error/close semantics audit
   Timeout handling and send-failure propagation are fixed, but the wrapper still needs a consistency pass for:
   - underlying receive native errors
   - whether close should wake blocked paths more explicitly
   - whether receive-side failure should also be latched

5. Streaming wire validation cleanup
   One bad `assert(prefix.direction)` was replaced, but the transport receive path should be reviewed for release-safe validation of malformed wire input instead of debug-only assumptions.

6. Regression coverage for discovered bugs
   Add targeted tests for:
   - io_uring timed receive timeout/cancel association
   - transport queued-send wakeup/progress
   - `pending_transmits_` lifetime on disconnect/cancel
   - `spsc_wrapping` send failure propagation

7. Diagnostic cleanup review
   Ensure only useful low-overhead diagnostics remain and no debug-only fields are accidentally shaping behavior.

8. Lower-priority performance cleanup
   Still worth revisiting later:
   - `queued_send_item` heap-copy overhead in streaming transport
   - any remaining avoidable buffer churn on large payload paths
