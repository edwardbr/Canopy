# SGX io_uring Controller Plan V1

Status: deprecated. This document records the first working SGX io_uring
controller direction. The active V2 architecture is now documented in
`io-uring-runtime-scheduler-architecture-v2.md`.

The next-stage multi-operation completion engine and file/blob resource plan is
documented in `io-uring-operation-engine-plan.md`.

This plan replaces the earlier idea of using a generic service-object singleton
for SGX io_uring wakeup and resource discovery. The singleton idea is parked in
`documents/protocol/service-object-singleton-proposal.md`.

The current plan is to pass an explicit per-enclave RPC object through the
normal `connect_to_zone` input path. That object implements the private
io_uring control interface and temporarily encapsulates the ordinary host
interface supplied by application or test code.

## Current Implementation Snapshot

The current implementation keeps the RPC core unchanged and uses a dedicated
SGX coroutine io_uring test host/enclave:

- the host test object implements the ordinary test `i_host` interface
- the SGX coroutine host connection helper creates the `i_io_uring_control`
  adapter internally and transfers the type-erased `i_noop` application
  interface during enclave bootstrap
- `host_controller` creates the ring, optional fixed buffers, and a
  fixed-file table for direct descriptors
- `get_iouring_data()` returns descriptor version 2, including ring mappings,
  host buffer metadata, and fixed-file metadata
- the enclave controller caches the descriptor and uses it for NOP and TCP
  direct-descriptor operations
- `direct_descriptor` owns individual direct descriptor slots
  for the current TCP stream/listener helpers, with awaitable close and
  destructor-triggered best-effort cleanup
- `self_ping_test()` proves the enclave can create a loopback listener and
  connector, send `hello`, receive `world`, and close the direct descriptors

This is still a functional smoke path. It is not yet a production transport
binding, and it does not yet include attested encryption, runtime operation
probes, linked timeout/cancel handling, or file/blob services.

## Goals

- Keep core RPC protocol and object-id semantics unchanged for this work.
- Scope the io_uring wake capability to the enclave connection that owns the
  ring.
- Keep host-owned kernel resources out of the normal data path after bootstrap.
- Let enclave code drive SQ/CQ ring activity directly through validated
  outside-enclave shared memory.
- Keep cleanup automatic through ownership, without requiring application code
  to call an explicit close method.

## Reusable Subcomponent Boundary

The reusable implementation belongs in:

```text
c++/subcomponents/io_uring
```

Test fixtures and SGX transport tests should consume this subcomponent. They
should not own the host/enclave io_uring design.

The subcomponent should provide:

- `host_controller`: host-side kernel resource owner
- `controller`: enclave-side helper that owns the RPC pointer
  to `i_io_uring_control`
- optional thin adapter objects that implement `i_io_uring_control` over a
  `host_controller`

The current CMake boundary is two static libraries:

- `canopy_io_uring`: host-side static library. On Linux it includes the
  `host_controller` implementation and the shared enclave-side
  controller implementation for host/fake-SGX users.
- `canopy_io_uring_enclave`: enclave-side static library compiled with enclave
  flags and linked into SGX enclave shared objects that need direct io_uring
  submission support.

An application or test host that already has a primary interface such as
`i_host` can compose `host_controller` and expose
`i_io_uring_control` as an additional RPC interface. That is the adapter
shape; the io_uring logic itself remains reusable.

The enclave-side API should hide most low-level io_uring detail behind RAII
objects. A TCP connection object should own its direct descriptor, provide
stream-style read/write access, and initiate close/release automatically when
the last owner goes away. An explicit awaitable close remains the deterministic
path for graceful shutdown because C++ destructors cannot themselves await async
cleanup.

## Host Controller

The host-side `host_controller` owns the real host/kernel resources:

- io_uring fd
- mapped SQ/CQ/SQE regions
- optional eventfd
- fixed buffers and fixed files/direct descriptors when enabled
- policy for allowed operations, addresses, file roots, and limits
- any per-enclave wake capability needed for `IORING_ENTER_SQ_WAKEUP`

The host control interface is named `i_io_uring_control` so it does not
become a generic host-control escape hatch.

The host controller should expose a versioned descriptor to the enclave. The
descriptor should include ring mappings, offsets copied from `io_uring_params`,
queue depth, setup flags, SQPOLL policy, fixed buffer metadata, fixed
descriptor metadata, and operation allowlists.

For the first implementation, `i_io_uring_control::get_iouring_data`
returns a flat `rpc::io_uring::data` snapshot copied from
`host_controller::state` while the controller is open. It deliberately
does not expose the state object, does not transfer ownership of liburing
objects, and does not give the enclave authority to close the host ring.
Host/kernel memory addresses are encoded as integer pointer values and are only
valid while the host controller and its RPC lifetime path remain alive. The
descriptor includes both the io_uring mappings (`SQ`, `CQ`, and `SQE` memory)
and the first host-owned payload buffer region. Fixed-buffer registration is a
controller option rather than a baseline requirement at this stage.

Descriptor version 2 also carries `fixed_file_data`. This was added
for direct-descriptor operations, not for NOP. `IORING_OP_SOCKET` and
`IORING_OP_ACCEPT` direct allocation need a registered fixed-file table, and the
enclave needs to know whether that table exists and how many slots it can use
before it submits SQEs with `IORING_FILE_INDEX_ALLOC`.

The wake method must remain narrow. It should wake only the already-owned ring;
it must not become a generic syscall bridge.

### Fixed Files And Direct Descriptors

The host controller should register a sparse fixed-file table once the code
moves beyond NOP. Direct descriptors should be the preferred representation for
sockets and files created through enclave-submitted SQEs.

The descriptor returned by `get_iouring_data` currently includes:

- whether fixed files were registered
- the fixed-file table size

That is intentionally minimal metadata. It is enough for the current direct TCP
smoke path to avoid submitting direct-descriptor SQEs against a ring that has no
registered file table.

The descriptor should eventually include:

- dynamic allocation range, if configured
- reserved host/controller slots
- operation support bits for socket, connect, bind, listen, accept, openat2,
  read, write, send, recv, close, and future mmap support
- buffer regions that kernel operations may read or write

The enclave should treat direct descriptor indexes as opaque resource handles.
They avoid normal process fd installation and per-operation fd-table lookup, but
they do not make sockets, files, or kernel results trusted.

`fixed_file_data` is therefore an ABI/resource-discovery field. It is
not an access-control decision and does not prove that a particular operation is
safe. Future policy still needs operation allowlists, address/file restrictions,
reserved slot ranges, and runtime capability probing.

For kernels that do not support the full descriptorless server path, the host
may pre-create a socket, register it into the fixed-file table, and expose only
the fixed descriptor index to the enclave. Newer kernels can allow the enclave
to submit `SOCKET_DIRECT`, `BIND`, `LISTEN`, and `ACCEPT_DIRECT` itself,
subject to policy.

## Enclave Controller

The enclave-side object should be named something like
`controller`.

It owns enclave-side control of the borrowed ring:

- the RPC pointer to the host io_uring controller
- validation state for outside-enclave ring and buffer mappings
- operation id allocation
- in-flight operation table
- CQ polling and coroutine resumption
- cancellation and shutdown state
- helper factories for streams, acceptors, connectors, and later file services

The enclave controller should provide higher-level helpers such as:

```cpp
connect_tcp(...)
accept_tcp(...)
make_descriptor_stream(...)
open_blob(...)
map_blob(...)
```

Later file services can use the same controller once file policy and
authenticated/encrypted file profiles are defined.

The current stream-shaped helpers are `stream`,
`acceptor`, and `connector`. They are reusable
building blocks for TCP tests and later transport integration, but they still
need stronger timeout, cancellation, partial I/O, and shutdown semantics before
they should carry production RPC traffic.

`map_blob(...)` should initially be host-controller brokered. The host maps the
file and returns an outside-enclave mapping descriptor with an explicit release
handle. The enclave copies and authenticates bounded chunks before parsing.
`IORING_OP_MMAP` should not be assumed until it is present in the target kernel
baseline and covered by runtime probing.

## Stream Ownership

Streams and acceptors created from the enclave controller hold
`std::shared_ptr<controller>`.

This gives the intended lifetime:

```text
descriptor_stream / acceptor / connector
  -> keeps controller alive
  -> keeps host control proxy alive
  -> keeps the RPC transport/service-proxy path alive
  -> allows cleanup to be scheduled when the last stream goes away
```

Application code should not need to call an explicit `close()` on the
controller. When the last stream, acceptor, and connector release the
controller, the host control proxy and ring ownership can naturally unwind.
Individual sockets are owned by direct-descriptor RAII guards, so dropping the
last stream/listener initiates descriptor cleanup even if the caller did not
await an explicit stream close.

## Cleanup Model

Destructors cannot `co_await`, so cleanup should be dispatched through shared
state rather than by awaiting in the destructor.

The direct-descriptor owner currently provides the first per-resource cleanup
guard. The broader enclave controller destructor still needs an explicit shared
cleanup state. It should eventually:

- mark the controller closing
- stop accepting new operations
- capture the scheduler or service needed to spawn cleanup
- capture the host-control RPC pointer
- capture a shared cleanup-state object
- spawn an idempotent detached cleanup coroutine

The cleanup coroutine should cancel outstanding operations, notify the host
controller, and release enclave-side state. It must not capture `this`.

The scheduler is expected to remain usable because the host-control service
proxy keeps the transport path alive until cleanup has been dispatched. The
host controller is still the final owner of kernel resources and must clean
them up if enclave-side cleanup never arrives because the enclave or process
fails.

An awaitable close function may be useful as a private test hook, but it should
not be part of the normal application contract.

## Connection Flow

The intended public entry points for SGX coroutine enclave connectivity are:

```cpp
rpc::sgx::coro::host::connect_to_enclave_zone<Remote, Local>(...);
rpc::sgx::coro::enclave::create_child_enclave_zone<Remote, Local>(...);
```

The host-side helper is the only place application code should need to know
that an enclave connection needs an io_uring control object. It takes the
interface that the host wants to provide to the enclave, creates the host-side
io_uring controller/control adapter internally, wraps the supplied interface in
that adapter, and then calls the normal RPC connection path as
`connect_to_zone<i_io_uring_control, Local>(...)`.

The adapter method that exposes the caller's original interface should be named
`transfer_encapsulated_interface`, not `get_encapsulated_interface`. The method
returns the encapsulated object as `rpc::shared_ptr<rpc::i_noop>` and clears the
adapter's retained copy as part of the same operation. This makes the ownership
intent explicit: the adapter is a bootstrap envelope, not a long-term owner of
the application interface.

The enclave-side helper is the only place that should know how to unwrap the
bootstrap envelope. It receives `i_io_uring_control`, creates or reuses the
per-runtime `rpc::io_uring::controller`, transfers the encapsulated interface,
casts it to the requested `Remote` type, and then calls the existing user
factory unchanged. User factories should continue to receive:

```cpp
rpc::shared_ptr<Remote>
std::shared_ptr<rpc::service>
```

If a factory needs enclave-only functionality, it can cast the service pointer
to the enclave service type explicitly. The common factory shape stays readable
and keeps normal application code insulated from the bootstrap protocol.

`INVALID_INTERFACE_ID` is reserved for handshake/template interface-id
mismatches. If `transfer_encapsulated_interface` succeeds but the returned
object cannot be cast to `Remote`, the enclave helper should return
`INVALID_CAST`.

This keeps all capability exchange inside normal RPC object passing. It avoids
changing the standard connection handshake, avoids changing core RPC object-id
semantics, and avoids using `canopy_coroutine_startup_status*` as a second
capability protocol.

### SGX Coroutine Source Layout

The SGX coroutine transport should be split by responsibility:

```text
c++/transports/sgx_coroutine/host/include/...
c++/transports/sgx_coroutine/host/src/...
c++/transports/sgx_coroutine/enclave/include/...
c++/transports/sgx_coroutine/enclave/src/...
c++/transports/sgx_coroutine/common/include/...
c++/transports/sgx_coroutine/edl/...
```

Host-only APIs belong under `rpc::sgx::coro::host`. Enclave-only APIs belong
under `rpc::sgx::coro::enclave`. Shared protocol and queue helpers stay under
`rpc::sgx::coro::common` or `rpc::sgx::coro::protocol`.

The existing file name `canopy_coroutine_enclave.idl` is too broad and should
be renamed to `coroutine_enclave.idl`. The SGX EDL file can keep whatever name
is easiest for the SGX build, but generated C++ IDL target names should stop
encoding the old `canopy_` prefix once the rename is complete.

The SGX coroutine transport is enclave-only. If `CANOPY_BUILD_ENCLAVE` is
false, neither the blocking SGX transport nor the SGX coroutine transport
should be configured, including fake-SGX builds. The private
`coroutine_enclave.idl` target is also SGX/enclave-only; non-enclave coroutine
builds should not learn about or depend on it.

Prefer target usage requirements over repeated include-directory plumbing.
When a target publicly links another target, it should inherit the generated
headers, transport headers, and RPC/interface headers through that dependency
chain. Repeating the same target dependencies or generated include directories
in consumers is a sign that the provider target is missing public usage
requirements. Concrete SGX/enclave targets may still need
`CANOPY_ENCLAVE_LIBCXX_INCLUDES` privately because enclave libc++ headers are
toolchain inputs rather than Canopy target usage requirements.

### Enclave Service And Runtime Controller

The enclave helper needs a service object that can own or reference
enclave-only runtime state. That type should be a small
`rpc::enclave_service` derived from `rpc::child_service`.

Only the SGX coroutine enclave entry-point path should create
`rpc::enclave_service` initially. Ordinary child zones should keep using the
existing child-service path until there is a concrete reason to generalize the
core RPC API.

The first `create_child_enclave_zone` call should create and register the
per-runtime `rpc::io_uring::controller`. Later child-zone connections in the
same enclave runtime should reuse that controller while an enclave object still
owns it. The controller should call the host-control interface through
`rpc::optimistic_ptr<i_io_uring_control>` so it remains callable without making
the controller itself an ordinary outgoing shared interface owner.

Because the host-control object still owns the host io_uring resources, the
enclave service must retain one explicit normal host reference when the
host-control shared pointer is first received. This is done by sending a normal
`outbound_add_ref` for that remote object, then converting the controller to
the optimistic pointer path and letting the original shared pointer release
normally. When the enclave service/runtime shuts down, the service sends the
matching `outbound_release` before its local lifetime bookkeeping disappears.
This keeps the host object alive without changing generic RPC reference-count
mechanics.

Avoid broad changes in `rpc/` for this work. If the enclave helper needs a
different child-service construction path, it should keep that logic local to
the SGX coroutine transport rather than adding a new generic hook to
`rpc::child_service::create_child_zone`.

### Implementation Priorities

The connection path is performance-sensitive because it is on the way to
enclave I/O, but it is still setup-time code. Prefer easy-to-read code over
cleverness:

- use small named helper types instead of dense lambdas where ownership matters
- comment the lifetime boundaries around the host adapter, transferred
  interface, runtime controller, and enclave service
- keep the user-facing factory signature unchanged
- do not hide important error choices behind generic catch-all helpers
- keep core RPC changes narrow and mechanical, or avoid them entirely

## Startup Status Scope

`canopy_coroutine_startup_status*` should remain a narrow startup-status
mechanism. It is suitable for fixed runtime states such as worker admission,
readiness, shutdown, and failure.

It should not be used to carry general io_uring descriptors or RPC object
capabilities. That would create a second control protocol beside Canopy and
would make future capability expansion harder to type, authorize, and test.

## Security Notes

The enclave must treat all io_uring shared memory, registered buffers, SQEs,
CQEs, and descriptors as untrusted host/kernel-controlled state.

The host can still deny service, corrupt ring memory, close fds, or stop
waking the SQPOLL thread. The controller design reduces host data-path relay
logic; it does not make the host trusted.

Sensitive data must be encrypted/authenticated inside the enclave before being
placed in outside-enclave buffers. Inbound data must be authenticated before RPC
deserialization or application parsing.

Unknown completion ids, invalid CQEs, ring pointer corruption, and impossible
state transitions should fail the controller and close dependent streams.

## Out Of Scope For This Stage

- generic service-object singleton support in core RPC
- caller-zone-specific interface filtering
- general file-system authority
- generic host syscall forwarding
- shared rings across enclaves
- production TLS/RA-TLS or Canopy AEAD stream wrapping
- broad file/blob services and mmap resource management

## Build Model: Nanopb Generated Sources

The nanopb build should mirror the Protocol Buffers generated-source model.
Generated `.pb.c` files are listed in `NANOPB_PB_SOURCES` and compiled directly
rather than being pulled through one aggregate C file.

This matters for one-shot builds after IDL edits. If only descriptor contents
change, the generated `.pb.c` file changes and its object should rebuild
directly. The older aggregate-source model could keep the aggregate file
unchanged when the include list stayed the same, leaving stale descriptor
objects until a clean build or manual touch forced recompilation.
