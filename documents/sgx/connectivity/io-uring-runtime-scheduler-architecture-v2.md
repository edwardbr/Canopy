# io_uring Runtime And Scheduler Architecture V2

Status: target architecture for the next SGX coroutine and io_uring refactor.

This document supersedes `io-uring-controller-plan.md`. The V1 controller plan
describes the first working SGX path, where the enclave receives an
`i_io_uring_control` RPC object and builds an enclave-side controller from it.
That model proved the capability transfer and direct ring access path, but it
also left too many responsibilities mixed together:

- SGX host transport owns enclave creation, runtime thread lifetime, RPC
  connection setup, and shutdown signalling.
- Enclave runtime code owns ECALL validation, scheduler creation, service
  creation, host transport creation, worker admission, runtime cleanup, and
  io_uring controller lifetime.
- io_uring currently works first inside enclaves, but the same controller shape
  should also work directly on the host.
- The code has more than one `enclave_owner` concept, which makes ownership hard
  to reason about.

The V2 direction is to separate these concerns so transport/runtime lifetime and
io_uring/scheduler lifetime are explicit and reusable.

## Goals

- Keep the core RPC protocol and object-id semantics unchanged.
- Make the SGX ECALL surface small: validate inputs, locate a runtime, and route
  the call to the appropriate runtime component.
- Keep one host transport paired with one enclave runtime instance.
- Keep one io_uring controller paired with one scheduler.
- Allow host-side users to use the same `rpc::io_uring::controller` API without
  going through the SGX RPC control path.
- Keep enclave io_uring access behind a formal handle abstraction rather than
  treating `i_io_uring_control` as the controller's conceptual dependency.
- Make shutdown deterministic: controller shutdown before scheduler shutdown,
  worker ECALLs returned before enclave destruction, and no late RPC interfaces
  escaping runtime teardown.

## Non-Goals

- Do not redesign the RPC reference-count protocol.
- Do not make `i_io_uring_control` a generic host-control escape hatch.
- Do not change `service::connect_to_zone` to know about io_uring or to switch
  scheduler implementations at runtime.
- Do not add TDX, AMD SEV-SNP, TrustZone, GPU, NIC, or FPGA backends in this
  phase. The design should leave space for them, but SGX and host io_uring are
  the immediate targets.
- Do not require a full Canopy async facade before this refactor can start. The
  design should work with the current libcoro-based coroutine build, while
  narrowing the dependency on direct libcoro types over time.

## Responsibility Boundaries

### Host-Side SGX Transport

The host-side SGX transport is the RPC transport visible to the host service.
It should not own all runtime mechanics directly.

Responsibilities:

- create the SGX enclave
- create the host/enclave queues or streams
- create one host-side runtime owner for that enclave
- connect the RPC stream transport to the enclave entry service
- observe runtime shutdown and expose transport status

Non-responsibilities:

- joining individual worker ECALL threads directly
- owning io_uring controller internals
- containing enclave-specific scheduler cleanup policy
- duplicating runtime owner logic

### Host-Side Connection Helper

The current `connect.h` helper is the host-side convenience entry point for
connecting to an enclave zone with io_uring support.

Today it does three things:

- type-erases the optional application-provided host interface to `i_noop`
- creates the host io_uring control object used for enclave bootstrap
- calls the normal `service::connect_to_zone<i_io_uring_control, Local>(...)`
  path so the existing RPC binding machinery connects to the enclave

That helper is useful, but it should stay thin. The template should contain
only type-specific connection glue. Concrete objects such as the io_uring
control envelope and future host/enclave handle adapters should live in their
own headers and `.cpp` files.

### Host-Side Enclave Runtime Owner

There should be one host-side owner object for one enclave runtime instance.
This should replace duplicate `enclave_owner` implementations.

Responsibilities:

- own the enclave id
- start the master ECALL thread
- start worker ECALL threads requested by the enclave runtime
- ensure the master ECALL and all worker ECALLs have returned before destroying
  the enclave
- provide a small status or callback surface for the host transport

The master ECALL thread is special: it runs the enclave runtime loop. Worker
ECALL threads provide scheduler execution capacity. Normal worker exit is not a
runtime cleanup request; cleanup is a runtime-level decision made by the master
runtime path.

The owner may hand the master thread handle to the host scheduler for joining,
but it must not destroy the enclave until all ECALLs have actually returned.

### Enclave Runtime

The enclave runtime is the in-enclave owner of runtime state for one enclave
instance.

Responsibilities:

- create and own the scheduler for the runtime
- create the root service and host-facing transport
- admit worker ECALLs
- run the master runtime loop
- coordinate runtime cleanup hooks
- stop worker loops only after service, transport, and object lifetime have
  drained far enough for shutdown
- release io_uring resources before scheduler shutdown

Non-responsibilities:

- implementing io_uring operations
- owning host kernel io_uring resources
- embedding application connection factory logic directly in ECALL handlers
- growing into a general-purpose global singleton

The ECALL entry points should become thin routers:

- validate the input blob and untrusted pointers
- find or create the runtime instance
- route `init` to the runtime master entry
- route `enter_thread` to the runtime worker entry
- return status

### Enclave Host Transport

The in-enclave `host_transport` is the stream transport connected to the host.
It should represent the peer connection, not the whole enclave runtime.

Responsibilities:

- implement the stream transport behaviour for messages to and from the host
- own any transport-specific final release hooks that are inherently tied to the
  host connection
- own the retained host reference for the bootstrap `i_io_uring_control`
  object after enclave setup has dropped the normal RPC pointer
- serialize narrow host io_uring calls such as `wake_iouring` and
  `get_iouring_data` through the generated IDL serializers and `outbound_send`
- notify the runtime when the host-facing transport is destroyed

Non-responsibilities:

- owning the scheduler
- owning all runtime cleanup
- deciding when worker ECALLs stop
- owning io_uring operation state or the enclave operation table

Current implementation note:

- `coroutine_init_enclave` explicitly creates the SPSC stream, then the
  `rpc::enclave_service`, then the in-enclave `host_transport`.
- The enclave connection handler receives the already-created service from the
  host transport. It must not create a second root or child service.
- `create_child_enclave_zone` obtains `i_io_uring_control` only for setup:
  retain one host reference, create or reuse the runtime controller, transfer
  the encapsulated user interface, then drop the normal RPC pointer.
- The enclave controller keeps only a weak route back to `host_transport` for
  the host-control calls. It does not own a `rpc::shared_ptr` or
  `rpc::optimistic_ptr` to `i_io_uring_control`.
- `host_transport::on_disconnecting()` releases the retained host reference
  before stream cleanup makes the outbound release path unavailable.

## Scheduler And io_uring Pairing

The target direction is an `io_uring_scheduler`.

The current service owns `std::shared_ptr<rpc::coro::scheduler>`.
`rpc::coro::scheduler` is an alias for the active scheduler implementation, so
the scheduler type seen by `rpc::service` should remain compile-time
polymorphic. `service::connect_to_zone` should not grow io_uring-specific
branches, scheduler replacement hooks, dynamic casts, or runtime scheduler
selection.

If `io_uring_scheduler` can become the active `rpc::coro::scheduler` alias for
a build, the service can keep owning the same pointer shape and the scheduler
can also carry the io_uring controller. If derivation or alias replacement is
not practical, `io_uring_scheduler` can own the libcoro scheduler internally,
but the runtime must make that ownership explicit and still pass the normal
compile-time scheduler type to `rpc::service`.

Initial shape:

- `io_uring_scheduler` is lifetime-managed by `std::shared_ptr`
- internally it may own a `std::unique_ptr` to a libcoro scheduler
- it owns one `rpc::io_uring::controller`
- the controller owns one `io_uring_handle`

Preferred implementation if practical:

- `io_uring_scheduler` derives from the existing libcoro scheduler type so it
  can be passed anywhere a scheduler is currently expected.

Fallback implementation:

- `io_uring_scheduler` contains a libcoro scheduler and forwards the scheduler
  surface Canopy currently uses.
- This may be the safer direction if libcoro continues moving toward
  `std::unique_ptr` lifetime and non-polymorphic ownership.

The important invariant is not inheritance. The important invariant is:

```text
io_uring_scheduler
  owns scheduler execution
  owns one io_uring controller
  shuts the controller down before scheduler shutdown
```

The controller may later be folded into the scheduler implementation, but the
first refactor should keep it as a distinct object so the existing operation
engine can be reused.

## Cardinality

Default runtime cardinality:

- one host-side SGX transport per enclave runtime
- one enclave runtime per enclave instance
- one scheduler per enclave runtime
- one io_uring controller per scheduler
- one io_uring handle per controller

Allowed future/test cardinality:

- an enclave may contain additional schedulers for tests or specialised work
- those additional schedulers may or may not have their own io_uring controller
- coroutine DLL runtimes may use the same scheduler/controller pairing pattern
- non-enclave host runtimes may use `io_uring_scheduler` directly

This shape also leaves room for future user/kernel device handles. A high
performance NIC, GPU, FPGA, or similar backend may have its own userspace and
kernel resources. Those resources should be modelled like io_uring handles:
encapsulated behind a backend handle, owned by a controller, and paired with a
scheduler only through a narrow execution interface.

## io_uring Handle Abstraction

The public controller should depend on an abstract handle, not directly on SGX
RPC control.

Target classes:

```text
rpc::io_uring::io_uring_handle
    abstract access to ring metadata, wakeup, and lifecycle hooks

rpc::io_uring::io_uring_handle_impl
    host-only implementation that owns the real Linux io_uring resources

rpc::io_uring::host_io_uring_handle
    host-side adapter used when serving an enclave
    wraps or references io_uring_handle_impl
    exposes the narrow i_io_uring_control RPC surface

rpc::io_uring::enclave_io_uring_handle
    enclave-side implementation backed by the i_io_uring_control proxy
    retains the host-side RPC lifetime needed for borrowed ring mappings
```

The controller then becomes common:

```text
rpc::io_uring::controller
    owns std::unique_ptr<io_uring_handle> or std::shared_ptr<io_uring_handle>
    validates/caches descriptor data
    owns operation ids, operation table, SQ/CQ admission, and stream helpers
```

Host non-enclave users should be able to create:

```text
io_uring_scheduler
  -> controller
      -> io_uring_handle_impl
```

SGX enclave users should create:

```text
io_uring_scheduler
  -> controller
      -> enclave_io_uring_handle
          -> i_io_uring_control proxy
```

The host serving an enclave should create:

```text
host_io_uring_handle
  -> io_uring_handle_impl
  -> i_io_uring_control adapter
```

This makes the RPC interface an implementation detail of the enclave handle,
not the conceptual controller boundary.

## Lifetime Rules

### Scheduler And Controller

- A scheduler may exist without an io_uring controller.
- If a scheduler has an io_uring controller, there is exactly one controller for
  that scheduler.
- The controller must reject new work once shutdown begins.
- The controller must fail or cancel pending operations before the scheduler is
  shut down.
- Async close/cancel cleanup must be allowed to run while the scheduler is still
  alive.

### Controller And Handle

- The controller owns or strongly references its handle.
- The handle remains valid while controller operations are in flight.
- `io_uring_handle_impl` owns real host kernel resources.
- `enclave_io_uring_handle` owns the proxy-backed lifetime needed to keep the
  host implementation alive while enclave operations may still reference mapped
  ring state.
- `host_io_uring_handle` is responsible for adapting host-owned resources to
  the enclave control interface without exposing a generic syscall bridge.

### Runtime And Transport

- The host transport has one runtime owner.
- The runtime owner has one master ECALL thread and zero or more worker ECALL
  threads.
- Worker ECALLs must return before enclave destruction.
- The master ECALL must not return until the runtime has stopped worker loops
  and drained runtime-owned cleanup.
- The host transport may observe runtime state through weak pointers,
  callbacks, or status notifications, but it should not repeatedly probe and
  micromanage runtime internals.

### Runtime And io_uring

- Runtime cleanup requests controller shutdown before the final scheduler drain.
- Worker loops should not request runtime cleanup on ordinary exit.
- Runtime cleanup is enclave-scoped, not child-zone-scoped.
- Child zones may share the same scheduler/controller pair. Releasing a child
  zone must not shut down the shared controller unless it is the final runtime
  cleanup path.
- The runtime should not repeatedly probe service or transport shared pointers
  to drive shutdown. The host-facing transport destruction signal is the clean
  indication that host RPC activity has ended.
- The io_uring host-control retained reference belongs to the host-facing
  transport, not the service or controller. This keeps core service/proxy
  circular lifetime mechanics unchanged.

## Fake SGX Compatibility

Fake SGX is a required compatibility path for this refactor. It is not a
separate architecture.

The fake backend builds the coroutine SGX transport and enclave runtime as
ordinary shared libraries. It provides the same narrow host-facing SGX shape:
enclave create/destroy, ECALL dispatch, basic enclave/outside memory checks, and
the generated untrusted/trusted headers. Refactors in `sgx_coroutine` should
therefore keep fake SGX and SGX simulation following the same runtime ownership
contract.

Requirements:

- Do not add Intel-SGX-only ownership assumptions to shared SGX coroutine
  runtime code.
- Keep ECALL routing small enough that fake SGX can dispatch through the same
  entry points.
- Keep runtime owner and worker-thread lifecycle independent of whether the
  enclave is backed by Intel SGX simulation or fake SGX shared-library dispatch.
- Verify both `Debug_Coroutine_SGX_Sim` and `Debug_Coroutine_Fake_SGX` after
  changes that touch runtime ownership, ECALL routing, scheduler creation,
  io_uring handles, or shutdown.
- Use fake SGX as the first debugging backend when sanitizer or normal debugger
  visibility is more useful than Intel SGX simulation fidelity.
- Run fake SGX and SGX simulation test binaries serially when investigating
  lifetime bugs. Parallel runs can share temporary/runtime resources and produce
  misleading failures that are not caused by the RPC lifetime path under test.
- Treat "passes alone but fails after many prior enclave tests" as an
  accumulated-state or teardown signal first. A focused reproducer still matters:
  one passing test does not prove full-suite lifetime correctness, and one
  full-suite crash does not prove the focused operation is wrong without a
  stack or a narrowed sequence.

Recent passthrough lifetime debugging found a race that only became obvious in
fake SGX and stream-backed paths: `pass_through::add_ref` must reserve the local
passthrough shared/optimistic count before forwarding to the next transport.
Otherwise a concurrent release can remove the passthrough while the add-ref is
already in flight. This is shared RPC behavior, not SGX-specific behavior, but
SGX and fake SGX are good at exposing it because they widen scheduling windows.

## Readability And Construction Rules

This refactor should prefer easy-to-read code over compact or highly defensive
code. Enclave startup and shutdown are performance-sensitive, but not enough to
justify unclear ownership.

Rules:

- Put each substantial class in its own header and `.cpp` file.
- Keep classes focused on one responsibility.
- Keep ownership comments close to the code that transfers or releases
  ownership. Comments should explain lifetime phases, shutdown ordering, and
  why a reference is retained or released.
- Prefer explicit construction chains over factory layers. A caller should be
  able to read setup code as: create stream, create runtime owner, create
  transport, create scheduler, create controller, connect.
- Avoid factories of factories. Use a factory only when the construction choice
  is genuinely variable and cannot be expressed clearly with direct
  construction or dependency injection.
- Keep templates small. Templates should mainly adapt strongly typed RPC
  interfaces at the boundary; concrete lifetime and resource-owning code should
  live in normal classes.
- Keep scheduler selection out of connection templates. Connection helpers may
  adapt interface types, but they should not choose scheduler implementations.
- Avoid deeply nested template classes and deeply nested lambdas.
- Initialisation and shutdown paths should optimise for readability and
  determinism before micro-performance.
- Use move semantics in performance-sensitive data paths and ownership-transfer
  APIs. In setup and teardown code, prefer clear names and ordinary copies of
  cheap handles such as `std::shared_ptr` when that makes lifetime easier to
  audit.
- Do not add mutexes, status polling, or weak-pointer probing unless the
  ownership boundary requires them. Prefer a clear owner or a single explicit
  callback when one component must notify another.

## File And Class Direction

The refactor should move classes toward one responsibility per file.

Suggested SGX host layout:

```text
c++/transports/sgx_coroutine/host/include/transports/sgx_coroutine/host/
  transport.h
  enclave_runtime_owner.h
  connect.h

c++/transports/sgx_coroutine/host/src/
  transport.cpp
  enclave_runtime_owner.cpp
```

Suggested SGX enclave layout:

```text
c++/transports/sgx_coroutine/enclave/include/transports/sgx_coroutine/enclave/
  runtime.h
  runtime_instance.h
  host_transport.h
  service.h

c++/transports/sgx_coroutine/enclave/src/
  runtime.cpp              # ECALL routing and C ABI boundary only
  runtime_instance.cpp     # scheduler/service/worker/runtime lifecycle
  host_transport.cpp
```

Suggested io_uring layout:

```text
c++/subcomponents/io_uring/include/io_uring/
  controller.h
  io_uring_handle.h
  io_uring_handle_impl.h
  host_io_uring_handle.h
  enclave_io_uring_handle.h
  io_uring_scheduler.h

c++/subcomponents/io_uring/src/
  controller_*.cpp
  io_uring_handle_impl.cpp
  host_io_uring_handle.cpp
  enclave_io_uring_handle.cpp
  io_uring_scheduler.cpp
```

The exact file names can change, but the responsibility split should not:

- runtime owner starts/stops ECALL threads
- runtime instance owns enclave scheduler/service lifecycle
- host transport owns RPC transport behaviour
- scheduler owns execution and controller lifetime
- controller owns operations
- handle owns or adapts ring resources

## Migration Plan

1. Mark the V1 SGX io_uring controller plan as deprecated and introduce this
   V2 document as the active architecture.
2. Stabilise the SGX shutdown sequence around the current failing teardown in
   both SGX simulation and fake SGX: worker loops must stop before runtime state
   and enclave memory are destroyed.
3. Extract a single host-side `enclave_runtime_owner` and remove duplicate
   `enclave_owner` logic.
4. Split enclave runtime implementation into ECALL routing and runtime-instance
   lifecycle code.
5. Introduce `io_uring_handle` and move SGX `i_io_uring_control` usage behind
   `enclave_io_uring_handle`. The current interim implementation already
   removes normal RPC pointer ownership from the controller and places retained
   host-control lifetime in `host_transport`.
6. Move host kernel ownership into `io_uring_handle_impl`.
7. Adapt the existing controller to depend on `io_uring_handle`.
8. Introduce `io_uring_scheduler` as a scheduler/controller owner.
9. Use `io_uring_scheduler` in SGX runtime creation without changing
   `service::connect_to_zone`; the service should still receive the active
   compile-time `rpc::coro::scheduler` type.
10. Add host non-enclave construction using the same controller over
    `io_uring_handle_impl`.
11. Only after the ownership model is clear, integrate io_uring streams into
    production transport paths.

## Open Design Points

- Whether `io_uring_scheduler` can derive from the current libcoro scheduler, or
  whether composition is required.
- Whether `rpc::service` should continue storing the concrete scheduler pointer
  type, or whether it needs a Canopy-owned scheduler abstraction first.
- Whether `controller` should own `std::unique_ptr<io_uring_handle>` or
  `std::shared_ptr<io_uring_handle>`. Unique ownership is clearer, but shared
  ownership may be needed for stream objects that outlive setup code.
- The exact API boundary on `io_uring_handle`: raw descriptor retrieval and
  wakeup are known requirements; future device backends may need a more general
  capability/query surface.
- Whether the final handle abstraction should keep host-control retained
  reference handling in `host_transport`, move it into
  `enclave_io_uring_handle`, or split it so `host_transport` owns wire
  lifetime while the handle owns the typed io_uring operation surface.

These should be resolved as part of the first implementation slice, not by
adding defensive lifetime checks throughout transport teardown.
