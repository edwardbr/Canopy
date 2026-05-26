# C++ Implementation Status

The C++ implementation is the primary and most complete Canopy implementation.

Current characteristics:

- supports both blocking and coroutine builds
- supports the full main transport tree currently present in `c++/transports/`
- has dual-mode stream-backed TCP, OpenSSL TLS, WebSocket, and
  `transport_streaming` paths; blocking use of those stream-backed transports
  requires an opt-in `rpc::blocking_executor`
- keeps SPSC, IPC, io_uring, and SGX coroutine stream compositions
  coroutine-only or conditionally built
- supports YAS, full Protocol Buffers, and Nanopb protobuf-compatible serialization
- uses Nanopb as the intended protobuf-compatible runtime for SGX enclave builds
- keeps raw CMake defaults conservative for downstream consumers: tests, demos,
  benchmarks, Rust, and coroutines all default to off unless presets or callers
  enable them
- remains the implementation that repository-wide architecture and transport
  documentation should treat as authoritative unless explicitly stated
  otherwise

When documentation describes Canopy behavior without qualification, it should
normally be read as describing the C++ implementation.
