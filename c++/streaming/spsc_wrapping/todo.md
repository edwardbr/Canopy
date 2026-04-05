# spsc_wrapping Remaining Work

1. Cancellation/lifetime
   `recv_proxy_loop` holds `shared_ptr<proxy_state>` (and thus the underlying stream) alive for as long as it runs. If the underlying stream never returns and nobody drives `set_closed()`, the wrapper can self-retain indefinitely. This needs an explicit cancellation/stop path, not just atomic flags.

2. Error/close semantics audit
   Timeout handling and send-failure propagation are fixed, but the wrapper still needs a consistency pass for:
   - underlying receive native errors: a non-closed, non-timeout status with empty data currently falls into the timeout branch and spins indefinitely rather than being treated as fatal
   - whether close should wake blocked paths more explicitly
   - whether receive-side failure should also be latched

3. Regression test
   Add targeted test for `spsc_wrapping` send failure propagation through the wrapper's receive path under native errors.
