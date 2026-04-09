# Rust Port Progress

This file tracks the current active status of the Canopy Rust port.

Completed migration history has been moved to
[`completed.md`](/var/home/edward/projects/Canopy/documents/language-ports/rust/completed.md) to keep
this file focused on the remaining work.

## Current Status

Current focus:

- keep reducing internal runtime complexity where Rust entities still carry
  more helper logic than their C++ counterparts
- broaden validation beyond the probe crate and deterministic fuzz parity into
  richer existing IDLs and runtime behaviors
- audit Rust `OptimisticPtr` semantics against C++ `rpc::optimistic_ptr`,
  specifically:
  - remote weak-vs-local weak behaviour
  - use in breaking circular dependencies
  - remote object-deletion notification / callback support for cleanup and
    reconnection logic
- resolve the remaining Rust multi-hop returned-object routing issue so the
  deterministic parity harness can move from root fanout to deeper recursive
  topologies
- harden release/shutdown/stale-route cleanup in larger hierarchies and
  coroutine-adjacent designs, even though the Rust runtime itself remains
  blocking-only today

## Active Gaps

- only Protocol Buffers are implemented on the Rust side
- only the local and dynamic-library transports have Rust implementations
- coroutine support is not implemented for Rust transports or runtime
- streamed transports have not been ported, and should probably be tackled as a
  coroutine-oriented effort rather than forced into the current blocking Rust
  runtime
- the deterministic C++/Rust parity harness still uses a constrained topology
  while deeper recursive returned-object routing remains unfinished

## Verification Baseline

Most recently verified with:

- `cmake --build build_debug --target fuzz_test_gtest`
- `ctest --test-dir build_debug -R 'rust_tests|fuzz_transport_test' --output-on-failure`
- `./build_debug/output/fuzz_test_gtest --gtest_filter=fuzz_transport_cross_language_test.DeterministicSeedParity`
- `cargo test -p canopy-rpc --lib`
- `cargo test -p canopy-transport-local --lib`
- `cargo test -p canopy-transport-dynamic-library-test-child --lib`
- `cargo test -p canopy-protobuf-runtime-probe --lib -- --nocapture`
