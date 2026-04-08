# Rust Port Progress

This file tracks the current active status of the Canopy Rust port.

Completed migration history has been moved to
[`COMPLETED.md`](/var/home/edward/projects/Canopy/rust/COMPLETED.md) to keep
this file focused on the remaining work.

## Current Status

Current focus:

- keep the Rust app-facing API clean and locality-neutral, with internal
  runtime mechanics hidden behind `SharedPtr<dyn Interface>` and
  `OptimisticPtr<dyn Interface>`
- continue comparing Rust `Service`, `Transport`, `ServiceProxy`,
  `ObjectProxy`, and `ObjectStub` responsibilities against the C++ runtime
  where edge-case lifetime behavior still needs scrutiny
- broaden validation from the focused probe and fuzz harness into richer
  existing IDLs and behaviors
- continue hardening shutdown, release, and stale-route cleanup paths to avoid
  leaks in larger topologies

## Recent Milestones

- Rust generated interface-pointer method signatures now use erased app-facing
  shapes like `SharedPtr<dyn IPeer>` and `OptimisticPtr<dyn IPeer>`
- `ObjectStub` and related advanced runtime types are kept under the hidden
  `canopy_rpc::internal` namespace rather than the normal crate-root API
- the Rust dynamic-library child endpoint now forwards into the handwritten
  Rust `Service`/`Transport`/`ObjectStub` path rather than fuzz-specific
  manual dispatch
- added `canopy-transport-local`, a Rust in-process parent/child transport pair
  mirroring the C++ local transport pattern
- the Rust fuzz child now creates recursive child Rust zones using that local
  transport pair after the initial C++ -> Rust dynamic-library connection
- Rust pass-through and remote add-ref routing now match the relevant C++
  third-zone behavior closely enough for the typed fuzz transport suite and
  shuffled stress runs to pass for both the C++ and Rust variants
- added hidden `ServiceRuntime` and `TransportRuntime` trait-object seams so
  the Rust runtime can depend on service/transport behavior through object-safe
  boundaries while keeping the concrete `Service` and `Transport` machinery
  intact for generated local binding and current tests
- the Rust local and dynamic-library transport crates now depend on those
  hidden runtime seams rather than hard-coding only concrete runtime types
- added `RootService` and `ChildService` wrappers over the concrete Rust
  `Service` core, with the core now tracking its owning runtime wrapper so
  service-proxy creation and remote transport handoff stay on the derived
  service path instead of silently bypassing it
- hierarchical Rust fuzz child zone creation now uses `ChildService` plus the
  Rust local transport pair, while the initial DLL child zone uses
  `RootService`
- the Rust local transport tests and probe-side top-level service setup now use
  `RootService` and `ChildService`, so the concrete `Service` is more clearly a
  hidden core implementation rather than the normal app/runtime entrypoint
- the bare crate-root `Service` export is now doc-hidden, while `RootService`
  and `ChildService` remain the intended visible service wrappers
- hidden local-object registration and erased local-interface lookup now sit on
  `ServiceRuntime`, with a hidden helper that recovers typed local interface
  views from that seam, so higher-level Rust code no longer needs
  `concrete_service()` just to get at local generated interface views
- the generated/runtime local interface-binding seam now compiles cleanly
  against `ServiceRuntime`, and the Rust fuzz child no longer reaches through
  `RootService` just to register its root stub with the hidden concrete
  service core
- the Rust transport crates now expose service-oriented construction helpers
  that hide common wiring:
  - `canopy_transport_local::create_child_zone(...)`
  - `canopy_transport_local::create_child_zone_with_exported_object(...)`
  - `canopy_transport_dynamic_library::attach_child_zone(...)`
  - `canopy_transport_dynamic_library::attach_child_zone_with_exported_object(...)`
  so application-side code does not need to manually stitch together child
  services, transports, and endpoint setup for those transport families
- dynamic-library C ABI zone decoding and sample zone/object construction are
  now centralized inside `canopy-transport-dynamic-library`, replacing
  duplicated `zone_from_ffi(...)`-style helpers in test and child crates

## Verification Baseline

Most recently verified with:

- `cmake --build build_debug --target fuzz_test_gtest`
- `ctest --test-dir build_debug -R fuzz_transport_test --output-on-failure`
- `./build_debug/output/fuzz_test_gtest --gtest_filter=fuzz_transport_test/* --gtest_repeat=10 --gtest_shuffle`
- `cargo test -p canopy-rpc --lib`
- `cargo test -p canopy-transport-local --lib`
- `cargo test -p canopy-transport-dynamic-library --lib`
- `cargo test -p canopy-transport-dynamic-library-test-child --lib`
- `cargo test -p canopy-protobuf-runtime-probe --lib -- --nocapture`
- `ctest --test-dir build_debug -R rust_tests --output-on-failure`
