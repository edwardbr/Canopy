# Rust Development Notes

This file is the Rust-specific companion to the repository-level development
guide.

## Current Supported Scope

The Rust port currently supports:

- Protocol Buffers only
- blocking runtime behavior only
- `local` transport
- `dynamic_library` transport

The Rust port does not currently support:

- coroutine runtime behavior
- streamed transports
- YAS or other non-protobuf serialization formats
- full transport parity with the C++ tree

If you are changing Rust code, assume the supported scope above unless the code
itself clearly shows otherwise.

## Public Rust Surface

Normal Rust application-facing code should stay on:

- generated interface traits
- `SharedPtr<dyn Interface>`
- `OptimisticPtr<dyn Interface>`
- `RootService`
- `ChildService`

Normal Rust application-facing code should not need to know about:

- `ObjectStub`
- `ObjectProxy`
- `ServiceProxy`
- `LocalProxy`
- transport-local pointer/control details

Those concepts belong under `canopy_rpc::internal` or transport internals.

## Structural Intent

The Rust tree should stay close to the C++ structure where practical.

That means:

- keep module and file responsibilities aligned with the corresponding C++ code
- avoid inventing Rust-only runtime layers unless there is a clear technical
  reason
- prefer the same conceptual boundaries for:
  - `service`
  - `service_proxy`
  - `object_proxy`
  - `object_stub`
  - `transport`
  - `pass_through`

If equivalent Rust entities become much larger or more complex than their C++
counterparts, treat that as a design smell and re-check the split.

## Runtime Boundary Rule

Rust follows the same effective rule as the C++ marshaller boundary:

- no runtime mutex or equivalent lock should remain held across RPC I/O or
  re-entrant dispatch paths

Treat the following kinds of calls as I/O/runtime boundaries:

- `send`
- `post`
- `try_cast`
- `add_ref`
- `release`
- `object_released`
- `transport_down`
- `get_new_zone_id`

In blocking mode, no lock should cross these calls.

If Rust coroutine support is added later, one-way send-style calls may only be
relaxed deliberately where they are proven not to wait, suspend, or re-enter
while the lock is held.

## Generated Code Policy

- do not commit machine-generated Rust output
- generated Rust should continue to live in build output or ignored generated
  paths
- generator changes should keep Rust output aligned with C++ naming,
  fingerprints, ordinals, and wire semantics

## Pointer And Interface Guidance

- app-facing Rust code should hide local-vs-remote mechanics behind
  `SharedPtr` and `OptimisticPtr`
- optimistic local failure should behave like optimistic remote failure:
  `OBJECT_GONE`, not local weak-pointer mechanics exposed to the app
- local proxy/control details are runtime internals, not the normal API

## Documentation Files

- [`documents/language-ports/rust/README.md`](/var/home/edward/projects/Canopy/documents/language-ports/rust/README.md)
  is the canonical Rust port overview
- [`documents/language-ports/rust/current-state.md`](/var/home/edward/projects/Canopy/documents/language-ports/rust/current-state.md)
  tracks the current implemented scope
- [`documents/language-ports/rust/progress.md`](/var/home/edward/projects/Canopy/documents/language-ports/rust/progress.md)
  is the canonical active Rust progress file
- [`documents/language-ports/rust/completed.md`](/var/home/edward/projects/Canopy/documents/language-ports/rust/completed.md)
  is the canonical completed Rust migration history
- [`documents/language-ports/rust/original-translation-instructions.md`](/var/home/edward/projects/Canopy/documents/language-ports/rust/original-translation-instructions.md)
  records the original Rust translation plan
- [`documents/language-ports/rust/translation-retrospective.md`](/var/home/edward/projects/Canopy/documents/language-ports/rust/translation-retrospective.md)
  records lessons learned from the Rust migration
- [`documents/language-ports/rust/python-translation-instructions.md`](/var/home/edward/projects/Canopy/documents/language-ports/rust/python-translation-instructions.md)
  records the Python translation guide derived from that work

## Verification Expectations

When changing Rust runtime, generator, or transport code, verify the smallest
relevant checks first and broaden only as needed.

Common checks include:

- `cargo test -p canopy-rpc --lib`
- `cargo test -p canopy-transport-local --lib`
- `cargo test -p canopy-transport-dynamic-library --lib`
- `cargo test -p canopy-protobuf-runtime-probe --lib -- --nocapture`
- `ctest --test-dir build_debug -R 'rust_tests|fuzz_transport_test' --output-on-failure`

If a change affects shared runtime behavior, consider both:

- `build_debug`
- `build_debug_coroutine`

even though the Rust runtime itself remains blocking-only today, because the
overall repository test harness exercises mixed C++/Rust paths.
