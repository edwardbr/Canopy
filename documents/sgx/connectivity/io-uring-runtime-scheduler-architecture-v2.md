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

Implementation note: the first V2 slice has introduced the common
`rpc::io_uring::io_uring_handle` boundary and a transitional
`rpc::io_uring::linux_io_uring_handle` wrapper. The existing enclave adapter
still has compatibility names, but the common controller now depends on the
handle boundary rather than SGX-shaped virtual controller hooks.

Second slice note: the former enclave-named operation table has been renamed to
`rpc::io_uring::detail::direct_ring_operation_engine`. It is still in the same
header for now because the submit functions are templated, but the public
controller surface no longer talks about an enclave operation engine.

Third slice note: `rpc::io_uring::io_uring_scheduler` now exists as the
composition fallback. It owns the existing `rpc::coro::scheduler` pointer plus
the runtime controller, so services still receive the unchanged compile-time
scheduler type while runtime teardown can shut the controller down before the
scheduler.

Fourth slice note: the host-only stream path now has a
`streaming::io_uring::acceptor` adapter around the common
`rpc::io_uring::acceptor`. The typed streaming transport tests, timeout test,
and benchmark io_uring paths can create the chain explicitly:

```text
rpc::coro::scheduler
  -> rpc::io_uring::io_uring_scheduler
      -> rpc::io_uring::controller
          -> linux_io_uring_handle
  -> streaming::io_uring::acceptor / connector
  -> streaming::io_uring::stream
  -> rpc::stream_transport
```

This is intentionally direct construction rather than a factory stack. It is
the first host-only reuse of the common controller path and is the replacement
direction for the benchmark and transport test wiring.

Terminology note: this document uses `controller` for the common operation
controller only. Objects that own or adapt kernel/enclave resources should be
called handles or services, not controllers. The current implementation still
has transitional names such as `host_controller` and
`enclave_io_uring_controller`; those are expected to move toward
`linux_io_uring_handle` and `enclave_io_uring_handle`.

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
- Allow host-only implementations to use native Linux/liburing structures behind
  a thin handle, while keeping those structures out of enclave builds and out of
  the common controller API.
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

Current naming to migrate:

- `rpc::io_uring::host_controller` owns the real Linux ring and associated host
  buffers/fixed-file table. It is conceptually a host Linux io_uring handle, not
  the common operation controller.
- `rpc::sgx::coro::enclave::enclave_io_uring_controller` currently adapts the
  common controller to the enclave host transport. It is conceptually an enclave
  io_uring handle.
- `rpc::sgx::coro::host::detail::enclave_io_uring_control` is a bootstrap RPC
  service for enclaves. It should remain enclave-specific and should not become
  the host non-enclave io_uring API.

### Transfer Buffers And Encryption

The common io_uring controller has two distinct buffer concepts:

- caller buffers: the per-call `rpc::byte_span` or `rpc::mutable_byte_span`
  passed to `send`, `receive`, or `streaming::io_uring::stream`
- host buffers: fixed-size slots in the io_uring handle's host-visible buffer
  pool, configured by `buffer_count` and `buffer_size`

`use_caller_buffers_for_transfers` means the controller submits caller buffer
addresses directly to the kernel in SEND/RECV SQEs. That is a host-only
optimization. In enclave code, caller buffers are enclave-private memory, so
the kernel cannot safely read or write them. Enclave controllers must leave
this option disabled and stage transfers through host buffers.

The API is variable-sized at the stream boundary, but the host staging pool is
fixed-slot. Large transfers are split into chunks by stream and controller
limits. Encryption framing must therefore be sized around host buffer slots:
each ciphertext record needs room for payload bytes plus nonce, tag, and any
padding or record header.

Host-visible buffers may carry encrypted data. They must not carry enclave
plaintext or unauthenticated capability material. The intended secure shape is:

- plaintext is serialized and encrypted inside the enclave
- ciphertext is written to a host buffer slot and submitted to io_uring
- received ciphertext lands in a host buffer slot
- the enclave copies, authenticates, and decrypts it into enclave memory

Use authenticated encryption or an equivalent MAC. The host owns the memory and
can modify, reorder, replay, or truncate ciphertext unless the record format
detects it.

Performance follow-up items for buffer sizing, staging, and TCP behaviour are
tracked in `optimisation.md`.

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

Implemented first shape:

- `io_uring_scheduler` is lifetime-managed by `std::shared_ptr`
- it owns the active `std::shared_ptr<rpc::coro::scheduler>` without changing
  the pointer shape passed to `rpc::service`
- it owns one `rpc::io_uring::controller`
- the controller owns one `io_uring_handle`

Preferred implementation if practical:

- `io_uring_scheduler` derives from the existing libcoro scheduler type so it
  can be passed anywhere a scheduler is currently expected.

Current implementation:

- `io_uring_scheduler` is a small composition owner rather than a scheduler
  subtype.
- SGX runtime creation makes the chain explicit: create or reuse the scheduler
  owner, pass the unchanged scheduler pointer to `enclave_service`, and keep the
  controller inside the scheduler owner.
- This keeps `rpc::service` and `service::connect_to_zone` free of io_uring
  special cases while still giving runtime teardown one obvious owner for the
  controller-before-scheduler shutdown order.

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

rpc::io_uring::linux_io_uring_handle
    host-only implementation that owns the real Linux io_uring resources

rpc::io_uring::host_io_uring_handle
    host-side adapter used when serving an enclave
    wraps or references linux_io_uring_handle
    exposes the narrow i_io_uring_control RPC surface

rpc::io_uring::enclave_io_uring_handle
    enclave-side implementation backed by the host_transport narrow call path
    retains the host-side RPC lifetime needed for borrowed ring mappings
```

The controller then becomes common:

```text
rpc::io_uring::controller
    owns or strongly references io_uring_handle
    validates/caches descriptor data
    owns operation ids, operation table, SQ/CQ admission, and stream helpers
```

Host non-enclave users should be able to create:

```text
io_uring_scheduler
  -> controller
      -> linux_io_uring_handle
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
  -> linux_io_uring_handle
  -> i_io_uring_control adapter
```

This makes the RPC interface an implementation detail of the enclave handle,
not the conceptual controller boundary.

The initial handle API should be narrow:

```text
get_iouring_data(data&)
    returns the normalized ring descriptor used by the common direct-ring
    operation engine

notify_submitted(data, sqe_count)
    lets the environment wake or enter the ring after new SQEs are published

close()
    releases environment-specific resources
```

For SGX, `notify_submitted` serializes the wake request through the host
transport only when the SQPOLL flags require it. For host-only Linux,
`notify_submitted` may call `io_uring_enter`, SQPOLL wakeup, or do nothing
depending on ring setup. The controller should not know which environment it is
using.

Host-only code may use naked Linux/liburing structures inside
`linux_io_uring_handle`. That is the right place for `::io_uring`,
`io_uring_params`, registered buffer vectors, and fixed-file registration. The
common controller should consume the normalized descriptor and handle API so the
same operation code can run in host and enclave builds.

The abstraction should stay thin. The host-only handle is allowed to be little
more than RAII around the Linux ring, registration tables, and the specific
submission notification policy. What it must not do is expose Linux-only types
through the common controller, because that would make enclave builds and future
device-backed handles depend on host kernel details.

## Descriptor And Wire-Type Boundary

Host-only io_uring code should not depend on any SGX EDL/IDL generated type.
The common controller and host-only scheduler path use native C++ descriptor
types in `io_uring/types.h`.

Those native descriptor types are still deliberately plain and fixed-width:

- use `uint32_t`, `int32_t`, and `uint64_t` fields
- represent booleans as `uint32_t` flags
- represent transferred addresses and byte sizes as `uint64_t`
- do not use `bool`, `size_t`, pointers, references, STL containers, or
  platform-dependent integer aliases in the descriptor contract

SGX communication has a separate private IDL wire copy under the coroutine SGX
protocol namespace. The host and enclave convert between native descriptors and
wire descriptors with explicit field-by-field copy helpers at the SGX boundary.
Padding and object layout must never become the marshalling path.

Static assertions should guard the assumptions that matter for this temporary
shared descriptor shape:

- native descriptor sizes match the generated SGX wire descriptor sizes
- descriptor alignment is compatible with the expected 64-bit fields
- `sizeof(size_t)` and `sizeof(std::uintptr_t)` are 64-bit in the supported
  host/enclave build configuration

These assertions catch accidental field drift and host/enclave ABI assumptions,
but they are not permission to serialize by `memcpy`. The field copy is the
wire contract; matching sizes are only a build-time sanity check.

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
- `linux_io_uring_handle` owns real host kernel resources.
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
- Do not convert allocator `std::bad_alloc` into local RPC error codes in this
  code. If allocation fails, log the allocation failure and terminate. Keep
  exception handlers rare and specific; this code should not throw its own
  exceptions as normal control flow.

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
  linux_io_uring_handle.h
  host_io_uring_handle.h
  enclave_io_uring_handle.h
  io_uring_scheduler.h

c++/subcomponents/io_uring/src/
  controller_*.cpp
  linux_io_uring_handle.cpp
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

## Current Host io_uring Stream Notes

The host stream adapter is now expected to use the shared controller/handle
path. Keep the public stream layer small and focused:

- `streaming/io_uring` is already the best-shaped stream adapter. It wraps
  `rpc::io_uring::direct_descriptor` and composes with TLS, websocket, and
  stream transports without knowing whether the descriptor is host or enclave
  backed.
- TCP setup details such as `SO_REUSEADDR`, `FD_CLOEXEC`, and `O_NONBLOCK`
  should live in focused host TCP construction helpers.
- Timeout, cancel, completion wakeup, and shutdown pumping should be tested as
  controller/handle behaviour instead of being hidden inside stream classes.

Do not put stream lifetime, ring ownership, operation tables, socket ownership,
and scheduler interaction into one class. Keep reusable behaviour in focused
components or tests:

- a host timeout/cancel regression test for receive completion racing a linked
  timeout
- a host stream composition test around `streaming::io_uring::stream`
- host-only `linux_io_uring_handle` support for non-SQPOLL `io_uring_enter` and
  completion wakeup, if the first host-direct path does not force SQPOLL

## Migration Plan

1. Mark the V1 SGX io_uring controller plan as deprecated and introduce this
   V2 document as the active architecture.
2. Stabilise the SGX shutdown sequence around the current failing teardown in
   both SGX simulation and fake SGX: worker loops must stop before runtime state
   and enclave memory are destroyed.
3. Introduce `io_uring_handle` and move the common controller to depend on that
   handle instead of virtual SGX-shaped `inner_*` calls. Keep compatibility
   names while this slice is proven by tests.
4. Rename or wrap the current `host_controller` as `linux_io_uring_handle`.
   This object owns the real Linux ring and may keep using native liburing
   structures internally.
5. Replace the current enclave controller subclass with
   `enclave_io_uring_handle`. The current interim implementation already
   removes normal RPC pointer ownership from the controller and places retained
   host-control lifetime in `host_transport`.
6. Rename or split `detail::enclave_io_operation_engine` into a neutral
   direct-ring operation engine. The first rename is now in place as
   `detail::direct_ring_operation_engine`; a later cleanup can move the
   SGX-specific pointer trust checks into a smaller ring-access or memory-policy
   header if that makes the code easier to read.
7. Introduce `io_uring_scheduler` as a scheduler/controller owner.
8. Use `io_uring_scheduler` in SGX runtime creation without changing
   `service::connect_to_zone`; the service should still receive the active
   compile-time `rpc::coro::scheduler` type.
9. Add host non-enclave construction using the same controller over
   `linux_io_uring_handle`.
10. Only after the ownership model is clear, integrate io_uring streams into
    production transport paths.
11. Once the host/enclave common controller is working, split the non-SGX
    io_uring implementation into a branch suitable for merging to `main`, then
    rebase the SGX branch on that shared foundation.

## Benchmark Coverage

The first SGX benchmark should measure enclave-resident io_uring communication,
not host-to-enclave RPC latency. The host benchmark process should create an
enclave service, ask it to run the timed work, and then receive raw timing
samples. Inside the enclave, the benchmark creates a loopback io_uring stream
server and client on the enclave scheduler/controller pair and runs ordinary
RPC calls over that stream.

This gives a direct comparison between host-only `io_uring_*` benchmark rows and
`sgx_io_uring_*` rows while keeping the timed path focused on the io_uring
stream/RPC mechanics.

The `sgx_io_uring_pair_*` rows extend this comparison by launching two benchmark
enclave instances. One enclave owns the server scheduler/controller and the
other owns the client scheduler/controller. This is closer to the host
`io_uring_*` topology than the single-enclave loopback rows, while still using
the enclave io_uring control path.

An enclave-to-enclave benchmark is a separate follow-up. That should exercise
the existing child-to-parent-to-child link so that two enclave child zones
communicate through their parent, with the same benchmark report shape used for
host-only and single-enclave rows.

## Deferred Performance Analysis

The new host-only direct io_uring stream is not inherently slow. The raw stream
tests exercise many direct round trips quickly. The full RPC benchmark can look
hung because release builds run 1000 measured calls and the current
RPC-over-io_uring path is dominated by transport-layer behavior, not by the
basic direct-ring send/receive path.

Items to analyse later, after the lifetime and architecture work is stable:

- `TCP_NODELAY` equivalent for fixed-file direct sockets. Existing TCP streams
  disable Nagle on ordinary sockets. The direct-descriptor path now applies
  `TCP_NODELAY` with the socket `URING_CMD` setsockopt operation after connect
  or accept. Keep this as a measured regression point because the streaming
  transport's small prefix write followed by a small payload write is very
  sensitive to delayed-ACK/Nagle behavior.
- Small-message send coalescing in the streaming transport. This should be
  considered later, not implemented as an immediate generic transport change.
  If used, it should be explicit, easy to read, and limited to small
  prefix+payload messages so large payloads are not copied unnecessarily.
- Direct socket option support in the io_uring controller. `TCP_NODELAY` is the
  first narrow operation. If more options are needed, keep them as explicit
  controller methods unless there is a real repeated pattern worth abstracting.
- Completion pump scheduling cost. The proactor pump currently makes progress,
  but it should be measured for per-operation scheduler churn, especially when
  the streaming transport runs one send and one receive at a time.
- Buffer movement. The current common controller copies into host buffers before
  send and copies out after receive. That is appropriate for enclave safety, but
  host-only paths may eventually use a thinner handle implementation or
  registered buffers once the abstraction is stable.
- Benchmark shape. The debug benchmark intentionally uses very few calls and
  can be distorted by logging. Release benchmark filters are useful for narrow
  checks, but host-only io_uring performance should be analysed with raw stream
  tests, RPC transport tests, and the full benchmark separately.

## Open Design Points

- Whether `io_uring_scheduler` can derive from the current libcoro scheduler, or
  whether composition is required.
- Whether `rpc::service` should continue storing the concrete scheduler pointer
  type, or whether it needs a Canopy-owned scheduler abstraction first.
- Whether `controller` should own `std::unique_ptr<io_uring_handle>` or
  `std::shared_ptr<io_uring_handle>`. Unique ownership is clearer, but shared
  ownership may be needed for stream objects that outlive setup code.
- Whether host-only `linux_io_uring_handle` should initially force SQPOLL so it
  can reuse the direct-ring operation path immediately, or support non-SQPOLL
  `io_uring_enter` from the first host implementation slice.
- The exact API boundary on `io_uring_handle`: normalized descriptor retrieval
  and submit notification are known requirements; future device backends may
  need a more general capability/query surface.
- Whether the final handle abstraction should keep host-control retained
  reference handling in `host_transport`, move it into
  `enclave_io_uring_handle`, or split it so `host_transport` owns wire
  lifetime while the handle owns the typed io_uring operation surface.

These should be resolved as part of the first implementation slice, not by
adding defensive lifetime checks throughout transport teardown.
