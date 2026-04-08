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
- keep reducing internal runtime complexity where Rust entities still carry
  more helper logic than their C++ counterparts
- broaden validation from the focused probe and deterministic fuzz harness
  into richer existing IDLs and behaviors
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
  Rust local transport pair, while the initial DLL child zone also uses
  `ChildService` so the C++ root remains the single zone-id allocator in the
  fuzz app
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
- generated Rust interface traits now carry the C++-style static metadata
  surface for `get_id(...)` and `get_function_info(...)`, while the proxy
  construction helpers are kept doc-hidden and routed through
  `canopy_rpc::internal`
- the remaining Rust `Service` helpers now take explicit `InterfaceOrdinal`
  values instead of asking implementation types for interface ids, so local
  lookup and binding follow the C++ `__rpc_query_interface(interface_id)`
  pattern more closely
- the RPC-aware pointer wrappers now live in
  [remote_pointer.rs](/var/home/edward/projects/Canopy/rust/rpc/src/internal/remote_pointer.rs),
  with [bindings.rs](/var/home/edward/projects/Canopy/rust/rpc/src/internal/bindings.rs)
  reduced to binding helpers layered on top
- `LocalProxy`, `CreateLocalProxy`, and `CreateRemoteProxy` are no longer
  re-exported from the normal `canopy_rpc` crate root; app-facing code uses
  `SharedPtr<dyn Interface>` and `OptimisticPtr<dyn Interface>` while the
  generator/runtime use hidden `canopy_rpc::internal` paths
- `OptimisticPtr::from_shared(&Arc<_>)` now models the local optimistic case
  without leaking `LocalProxy` through normal app/test code, which is closer
  to the C++ `remote_pointer.h` split where the local proxy is internal to the
  optimistic pointer design
- hidden incoming optimistic binding now carries `OptimisticPtr<T>` end to end
  through the runtime and generator rather than exposing
  `InterfaceBindResult<LocalProxy<T>>`; null/gone/value transport still uses
  `InterfaceBindResult`, but the conversion back to a pointer wrapper now
  happens through one hidden helper instead of being reimplemented in generated
  code
- `SharedPtr` and `OptimisticPtr` now carry the shared hidden
  [`ObjectProxy`](/var/home/edward/projects/Canopy/rust/rpc/src/internal/object_proxy.rs)
  directly as their remote control block in
  [remote_pointer.rs](/var/home/edward/projects/Canopy/rust/rpc/src/internal/remote_pointer.rs),
  so remote object identity and separate shared/optimistic remote refcount
  state sit on the pointer wrappers rather than the generated interface types
- converting a remote `SharedPtr` into an `OptimisticPtr` now reuses that same
  hidden control block instead of reconstructing remote identity from the
  proxy view, which is closer to the C++ `remote_pointer.h` design
- shared incoming interface binding now stays in `SharedPtr<T>` end to end
  through the runtime and generator, rather than collapsing remote values to a
  bare `Arc<T>` and then rebuilding a shared wrapper later; this keeps the
  hidden control block intact on both the shared and optimistic paths
- generated Rust interfaces now implement direct forwarding for
  `SharedPtr<dyn Interface>` and `OptimisticPtr<dyn Interface>`, so app code
  can call interface methods on those wrappers without unpacking them first
- local optimistic direct calls now follow the same locality-neutral failure
  model as remote optimistic calls: if the target is gone, the hidden local
  proxy path returns `OBJECT_GONE` rather than exposing `Option`/`upgrade`
  mechanics to app-facing code
- generated interface modules now expose remote pointer construction helpers
  like `create_remote_shared(...)` and `create_remote_optimistic(...)`, so
  leaf application/integration code no longer needs to mention
  `ProxySkeleton::with_caller(...)` just to obtain an interface pointer
- hidden remote-interface construction now goes through `ServiceRuntime` and
  the service’s cached zone/service-proxy path rather than ad hoc helper logic
  living in `service_proxy.rs`, so `ServiceProxy` stays closer to the C++
  role of a transport/service channel rather than becoming interface-aware glue
- the Rust fuzz child no longer mentions `ServiceProxy` when turning a
  child-zone exported object into `SharedPtr<dyn IAutonomousNode>`
- the Rust fuzz child now also uses `ServiceRuntime::register_rpc_object(...)`
  for local shared-object registration instead of manually constructing an
  `ObjectStub` in that app-adjacent path
- remote shared and optimistic pointer establishment/adoption is now expressed
  explicitly in `remote_pointer.rs` through hidden pointer-layer constructors,
  so `service.rs` no longer spells out inline “add_ref then wrap” logic when
  binding remote interface pointers
- local `send`/`post`/`try_cast`/`add_ref` dispatch no longer routes through
  `Service::outbound_*`; Rust local object dispatch now goes explicitly through
  `ObjectStub`, keeping `outbound_send` aligned with the C++ meaning of the
  overrideable remote service-proxy-to-transport path
- `IMarshaller` is now explicitly documented as the Rust I/O boundary contract:
  callers must not hold runtime locks across any marshaller call, matching the
  C++ rule that no mutex may be active across blocking, re-entrant, or future
  suspension-capable RPC paths; the only intended nuance is that coroutine-mode
  one-way send-style calls can be relaxed deliberately if they are proven not
  to wait, reply, or suspend while the lock is held
- the fuzz harness now generates autonomous-node instruction streams in the
  C++ test runner and feeds those same operations into both the C++ local-child
  setup and the Rust dynamic-library child setup, so same-seed behavior is
  compared from one deterministic instruction source rather than two separate
  RNG streams
- added a cross-language deterministic parity test in
  [fuzz_test_main.cpp](/var/home/edward/projects/Canopy/integration_tests/fuzz_test/fuzz_test_main.cpp)
  that runs the same seed against both child-zone implementations and compares
  resulting child-zone count, connection count, received-object count, and
  function-call totals
- the Rust fuzz child now creates local factory/cache/worker capability zones
  and follows the same high-level autonomous-node operation flow as the C++
  version for the deterministic parity harness
- the deterministic parity topology currently uses a root fanout of child
  zones rather than a deeper recursive child tree, which keeps the Rust and
  C++ paths aligned while a remaining multi-hop returned-object routing issue
  in the Rust runtime is still being contained

## Verification Baseline

Most recently verified with:

- `cmake --build build_debug --target fuzz_test_gtest`
- `ctest --test-dir build_debug -R 'rust_tests|fuzz_transport_test' --output-on-failure`
- `./build_debug/output/fuzz_test_gtest --gtest_filter=fuzz_transport_cross_language_test.DeterministicSeedParity`
- `cargo test -p canopy-rpc --lib`
- `cargo test -p canopy-transport-local --lib`
- `cargo test -p canopy-transport-dynamic-library-test-child --lib`
- `cargo test -p canopy-protobuf-runtime-probe --lib -- --nocapture`
