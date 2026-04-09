# Rust Current State

This document intentionally keeps the original Rust port plan in
[`original-translation-instructions.md`](/var/home/edward/projects/Canopy/documents/language-ports/rust/original-translation-instructions.md)
so drift from the initial assumptions remains visible.

Current implemented scope:

- Protocol Buffers only
- blocking runtime only
- Rust `local` transport
- Rust `dynamic_library` transport

Current non-goals or deferred areas:

- coroutine runtime support in Rust
- streamed transports
- YAS or other non-protobuf serialization formats
- full transport-family parity with the C++ tree

Current API direction:

- normal Rust application-facing code should stay on generated interface traits
- interface values should be expressed as:
  - `SharedPtr<dyn Interface>`
  - `OptimisticPtr<dyn Interface>`
- local-vs-remote mechanics should remain hidden behind the runtime and pointer
  wrappers

Current maintenance intent:

- keep the Rust runtime structure as close as practical to the equivalent C++
  entities
- prefer matching C++ responsibilities for `service`, `service_proxy`,
  `object_proxy`, `object_stub`, `transport`, and `pass_through`
- avoid Rust-only architectural layers unless there is a clear technical
  reason

Current validation baseline:

- Rust/C++ deterministic parity exists in the fuzz harness for the current
  constrained topology
- deeper recursive returned-object routing is still unfinished, so parity has
  not yet been expanded to the full recursive shape
- streamed transports are expected to fit better with a future coroutine effort
  than with the current blocking Rust runtime
