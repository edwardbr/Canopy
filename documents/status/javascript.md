# JavaScript Implementation Status

The JavaScript implementation should currently be treated as a reduced-trust
generated client/transport layer, primarily for WebSocket-oriented scenarios.

It is not a full Canopy runtime equivalent to the C++ implementation.

That means:

- it is useful for generated browser and Node.js client scenarios
- it should not be described as if it had full transport, service, lifetime,
  and runtime parity with C++
- documentation should present it as a limited client-side capability, not as a
  peer implementation of the full Canopy runtime model
