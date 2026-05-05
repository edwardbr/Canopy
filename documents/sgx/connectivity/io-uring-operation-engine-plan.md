# SGX io_uring Operation Engine Plan

Status: active plan; the NOP operation-table foundation is implemented, and the
first direct-descriptor TCP smoke path is implemented for loopback self-ping.

This plan refines the current controller direction in
`io-uring-controller-plan.md`. The immediate trigger is the scheduled no-op
stress test: one enclave RPC call can spawn many coroutines that call
`controller::no_op()` concurrently. That is the right stress
shape. The controller must therefore stop treating `no_op()` as a single-caller
helper and grow a real multi-operation completion engine.

## Current Result

The first operation engine stage is in place:

- `controller::no_op()` submits direct `IORING_OP_NOP` SQEs
- each operation has an enclave-owned `user_data` id and operation state
- one operation table attributes CQEs to the correct waiter
- generic SQE submission is used by NOP and the current TCP helpers
- the controller supports 1000 sequential NOPs and 1000 scheduled NOPs in the
  SGX coroutine test harness
- the controller supports a first direct-descriptor TCP loopback smoke test:
  socket, bind, listen, accept, connect, send, recv, and close
- the SGX coroutine test enclave now also creates two independent root services
  inside one enclave and connects them over an io_uring TCP stream; the RPC
  surface between those two services is the separate
  `io_uring_test::i_peer2peer` interface, while `i_test_uring` remains the host
  test-control interface
- TCP streams, acceptors, and connectors hand descriptor lifetime to
  `direct_descriptor`, which provides awaitable deterministic
  close and destructor-triggered best-effort cleanup
- the host controller API remains narrow: `get_iouring_data()` and
  `wake_iouring()`, with descriptor metadata for fixed-file/direct-descriptor
  support

Two wait strategies exist:

- `cooperative_poll`: default; each waiter pumps the CQ once and yields to the
  scheduler when its operation has not completed
- `proactor`: experimental; one detached pump resumes registered waiters

The proactor path works, but the NOP measurements did not justify making it the
default. It reduces CQ pump/yield activity in the scheduled stress test, but it
adds suspend/resume cost for every operation and increases measured
per-operation latency. Keep it configurable until TCP/file operations provide a
better workload for comparison.

The current `total_ticks` and `max_ticks` measurements are diagnostic raw
tick-counter deltas around individual `no_op()` calls. They are not wall-clock
elapsed time and not CPU usage. In scheduled tests `total_ticks` is the sum of
many overlapping waits.

The TCP smoke path intentionally keeps semantics simple. It is enough to prove
that the enclave can be its own io_uring submission agent for a localhost
connection, but it does not yet solve transport framing, TLS/RA-TLS, production
connection policy, caller cancellation tokens, operation-id-specific
cancellation, or multishot accept.

The peer-to-peer RPC smoke test keeps server and client logic in separate
coroutine functions, mirroring the shape of the comprehensive TCP demo. Inside
the SGX enclave it is orchestrated with scheduler spawn and completion events
rather than `coro::when_all`, because the current enclave libc++ build does not
provide the `<ranges>` header required by libcoro's `when_all` implementation.

## Goals

- Support many concurrent enclave coroutines sharing one per-enclave io_uring.
- Support sustained concurrent TCP traffic without descriptor leaks, host-buffer
  leaks, unbounded memory growth, or starvation between connections.
- Keep ring mechanics reusable for NOP, TCP, UDP, file I/O, mapped-file blob
  access, and later time/timer operations.
- Keep the host as an untrusted resource adapter, not a trusted filesystem or
  syscall authority.
- Keep host ring ownership and wakeup narrow: the host creates the ring,
  exposes a descriptor, wakes SQPOLL, and cleans up.
- Keep operation-specific code small: each operation populates an SQE and
  interprets a CQE result; it should not own CQ polling or ring accounting.
- Degrade gracefully when SQ capacity, CQ capacity, fixed-file slots, or
  registered host buffers approach saturation.
- Make cancellation, timeout, and close semantics explicit enough that the same
  controller can safely back Canopy streaming transports.
- Avoid changing core RPC protocol semantics for this stage.

## Non-Goals

- Do not add generic host syscall forwarding.
- Do not make host file credentials trusted by the enclave.
- Do not add generic service-object singleton support for this work.
- Do not share one ring across multiple enclaves.
- Do not make the host consume CQEs for enclave-owned operations unless a later
  design explicitly moves completion dispatch out of the enclave.

## Design Influence

The model follows the separation used by libunifex's Linux io_uring context:

- context-level code owns submission capacity and completion dispatch
- operation objects provide operation-specific SQE population and completion
  handling
- `user_data` identifies the operation to complete
- operations that cannot be submitted immediately wait in a pending queue
- higher-level read/write/timer operations do not duplicate ring logic

Canopy should use the same separation, adapted to SGX constraints and Canopy's
coroutine/runtime wrappers.

Reference:
<https://github.com/facebookexperimental/libunifex/blob/main/include/unifex/linux/io_uring_context.hpp>

## Layering

The reusable code remains in:

```text
c++/subcomponents/io_uring
```

The intended layers are:

```text
host_controller
  -> owns host/kernel ring resources and exposes i_host_io_uring_control

controller
  -> owns validated ring descriptor, SQ submission, CQ dispatch, operation table

operation helpers
  -> no_op, socket, connect, bind, listen, accept, recv, send, open, read,
     write, close, timeout, mapped blob descriptors

stream/file abstractions
  -> reusable TCP/UDP/file/blob helpers built on operation helpers
```

Test fixtures should only adapt these components to test interfaces. They
should not own the ring engine design.

## Public API Shape

The public enclave-side API should remain small and approachable. Most
engineers using this layer should not need to understand SQEs, fixed-file
tables, CQ pumping, host buffer slots, or SQPOLL wakeups.

Preferred API shape:

- high-level RAII objects for TCP connections, listeners, file handles, mapped
  blobs, timers, and later UDP endpoints
- narrow construction APIs such as `connect_loopback`, `listen_loopback`,
  `accept`, or their production equivalents
- read/write/send/receive methods that expose Canopy stream semantics rather
  than raw descriptor mechanics
- explicit capability/result objects when an operation is unavailable on the
  current kernel or host policy
- low-level direct-descriptor helpers kept private or treated as advanced
  building blocks for the high-level objects

The controller can still own the low-level implementation functions, but its
day-to-day surface should feel like a connectivity context, not a bag of
syscall wrappers.

### RAII Resource Ownership

Resource ownership should be encapsulated by small RAII objects:

- `direct_descriptor` owns one direct descriptor slot and is
  the low-level cleanup guard used by higher-level TCP objects
- a TCP connection object owns one direct descriptor and provides read/write
  stream access
- a TCP listener owns the listening direct descriptor and returns connection
  objects from accept
- a file object owns an opened direct descriptor or host-brokered file resource
- a mapped blob object owns the mapping descriptor and releases it through the
  host resource interface
- temporary host buffers remain guarded by short-lived objects that release
  slots back to the controller pool

The controller is an enclave-scoped resource. Many child zones may use the same
controller, and zone teardown should release individual streams and descriptors
through that controller. Final enclave teardown registers a runtime cleanup
handler that requests controller shutdown before the last scheduler drain, so
remaining RAII cleanup work can complete while the coroutine runtime is still
available.

Dropping the last owner should initiate cleanup automatically. Because
destructors cannot `co_await`, destructor cleanup is a safety net: it may
schedule or submit best-effort close/release work through the controller. Code
that needs graceful, observable shutdown should still use an awaitable close
path before releasing the last reference.

The intended user model for TCP is:

```text
connection = co_await connector.connect(...)
co_await connection->send(...)
co_await connection->receive(...)
co_await connection->close()   # deterministic cleanup when needed

# If close was not awaited, destruction still initiates best-effort release.
```

The low-level direct descriptor must never leak into normal application code
unless the caller deliberately opts into an advanced API.

## Host Boundary

For the operation table stage, the host implementation should not need to
change.

The current host responsibilities are sufficient for concurrent NOP:

- create a per-enclave `io_uring`
- expose `rpc::io_uring::data`
- wake the SQPOLL thread through `wake_iouring()`
- keep host/kernel resources alive while the host control RPC object is alive
- clean up when the enclave-side controller releases the host control RPC
  object during runtime shutdown

Future host changes are expected for resource brokerage:

- opening files or sockets under host-side policy
- registering fixed files or fixed buffers beyond the current fixed-file table
- mapping files into host memory for enclave-visible blob reads
- returning opaque resource descriptors or fixed-file indexes
- exposing policy metadata and audit/telemetry

Those future host changes should use separate resource-control interfaces, for
example `i_host_file_control`, rather than expanding
`i_host_io_uring_control` into a generic host-control interface.

The host should still create and own the ring. The enclave can submit direct
SQEs, but it does not own the ring fd, fixed-file table lifetime, or host-side
cleanup.

The current fixed-file metadata is resource discovery, not a security
capability. It tells the enclave whether a fixed-file table was registered and
how many direct descriptor slots exist, so the enclave can decide whether
`IORING_FILE_INDEX_ALLOC` based socket/accept operations are even valid for the
ring. Policy and operation allowlists still need to be added separately.

## Enclave Operation Engine

`controller` should own the operation engine. It should split
into small internal responsibilities:

- descriptor cache and validation
- SQ coordinator
- CQ dispatcher
- operation table
- pending submission queue
- wakeup adapter
- shutdown/cancellation state

The implementation now keeps the public controller declaration in
`include/io_uring/controller.h` and splits the controller
logic by concern under `c++/subcomponents/io_uring/src`:

- `controller_core.cpp`
- `controller_buffers.cpp`
- `controller_noop.cpp`
- `controller_submission.cpp`
- `controller_tcp.cpp`
- `direct_descriptor.cpp`

Enclave users link this through the `canopy_io_uring_enclave` static library
rather than relying on header-only controller fragments.

### Descriptor Cache

The controller retrieves `rpc::io_uring::data` once, validates it, and caches the
validated derived pointers.

The cached view should include:

- SQ head/tail/flags/array pointers
- SQE array pointer and entry count
- CQ head/tail/CQE pointers
- SQ/CQ masks and entry counts
- setup flags such as SQPOLL and NO_SQARRAY
- buffer region metadata when available

Validation must check:

- descriptor version
- non-zero SQ/CQ entries
- accessible outside-enclave memory ranges
- expected SQE/CQE sizes
- SQPOLL support for enclave direct submission
- no impossible ring sizes or pointer arithmetic overflow

The raw `rpc::io_uring::data` remains untrusted. The cached view only means the
descriptor was syntactically acceptable when checked; the host can still corrupt
the memory later.

### Operation Object

Each in-flight operation should have enclave-owned state:

- unique `user_data`
- completion event or continuation handle
- result code and CQE flags copied from outside-enclave memory
- operation state: pending, submitted, completed, cancelled, failed
- optional cancellation metadata

The operation id should not be a raw enclave pointer for this stage. A monotonic
opaque id plus an enclave-owned table is easier to validate when CQEs arrive
from untrusted memory.

### SQ Coordinator

SQ submission must be serialized or otherwise coordinated so concurrent
coroutines do not publish overlapping SQEs.

Responsibilities:

- allocate an operation id
- reserve an SQ slot
- populate exactly one SQE through an operation-specific callback
- set `sqe.user_data` to the operation id
- update SQ array when NO_SQARRAY is not active
- publish SQ tail with release ordering
- track submitted-but-not-completed count
- request SQPOLL wakeup when needed

The current implementation uses a short controller-level spin mutex for
operation-table and ring-index mutation. It must only protect bounded in-memory
critical sections. Waiting for capacity, completions, or host wakeup must go
through the scheduler path and must not spin inside the lock.

This keeps the synchronisation local to metadata mutation. A later batching path
can reduce lock frequency if measurement shows the mutex is material.

### CQ Dispatcher

There must be one logical CQ consumer for the controller. Individual operations
must not each spin on CQ head/tail expecting their own completion to be next.

Responsibilities:

- acquire CQ head/tail
- copy each CQE into enclave-owned temporaries before acting on it
- validate `user_data`
- find the operation in the table
- store result and flags into the operation
- advance CQ head with release ordering
- signal/resume the operation waiter
- treat unknown operation ids as corruption or fraud

The dispatcher can be called opportunistically by submitting/waiting
coroutines. A small `pump_active` guard should prevent multiple dispatchers from
consuming CQEs concurrently.

### Backpressure

The controller should enforce both SQ and CQ capacity.

An operation cannot be submitted when:

- the SQ ring is full
- the number of in-flight operations would exceed CQ capacity
- the controller is closing
- the descriptor is invalid

The next implementation stage should replace hot retry loops with explicit
backpressure state:

- hard limits for in-flight operations, pending submissions, fixed-file slots,
  and borrowed host buffers
- soft high-water marks that trigger earlier yielding before the ring is full
- per-operation or per-stream accounting so one noisy connection cannot consume
  all queue and buffer capacity
- telemetry counters for submitted, pending, saturated, cancelled, timed out,
  and failed operations

The intended behavior is a bounded pending submission queue. When submission
capacity or host buffers are unavailable, an operation should wait on controller
state and yield through the scheduler. When completions free capacity, the
dispatcher should submit or wake queued work fairly.

Errors should distinguish failure classes:

- permanent controller failure or corrupted ring data -> fail outstanding work
  and mark the controller failed
- stream/connection close -> cancel or fail only operations owned by that stream
- temporary saturation -> wait, yield, and retry until the operation deadline or
  cancellation token expires
- configured queue/buffer limit exceeded -> return a backpressure/error result
  without growing memory unboundedly

The first implementation can map these to existing `rpc::error` values, but the
code should keep the categories distinct internally so more precise errors can
be added later.

### Heavy Concurrent TCP I/O

TCP stream use is expected to involve many concurrent send and receive
operations across multiple connections. The direct-descriptor stream layer must
therefore make ownership and scheduling explicit.

Required stream-side rules:

- each stream owns exactly one direct descriptor until close transfers it to the
  controller cleanup path
- send and receive operations are attributed to the stream that submitted them
- close cancels or drains only that stream's operations, not all controller
  operations
- a stream destructor may schedule best-effort cleanup, but normal code should
  use an awaitable close path when it needs graceful shutdown
- partial sends are retried until the caller's full span is sent, the stream is
  closed, the deadline expires, or cancellation is requested
- receives copy only the completed byte count back into enclave memory and treat
  `0` as orderly peer close
- stream-level high-water marks prevent one connection from monopolising host
  buffers or pending SQEs
- controller-level high-water marks protect the shared ring when all streams
  are busy

This implies a small direct-descriptor owner type should sit below
`stream`. It should make descriptor lifetime visible and avoid
repeating close/release logic in every stream wrapper.

### Cancellation And Timeouts

Timeouts and cancellation should be modelled as operation state, not as a
special case in individual stream functions.

The operation object should grow:

- owner id or stream id
- cancellation state
- deadline or timeout metadata
- optional linked timeout/cancel operation ids
- completion policy for multi-CQE operations

For receive timeouts, a timeout CQE must not by itself make the receive buffer
reusable if the recv SQE can still complete later. The operation should complete
only when the data operation is known to be complete, cancelled, or safely
detached from caller-owned buffers. This mirrors the existing guidance in
`documents/transports/stream_backpressure_guidelines.md`.

For explicit cancellation:

- cancelling a submitted operation should submit `IORING_OP_ASYNC_CANCEL` when
  supported and useful
- cancellation completion and original operation completion must both be
  attributed before caller memory is considered reusable
- cancelling a pending, not-yet-submitted operation can complete it entirely in
  enclave state
- stream close should cancel all operations owned by that stream
- controller shutdown should fail or cancel all operations across all streams

Timeouts should use a deadline model at the stream API boundary. Internally the
controller can decide whether to use linked timeout SQEs, standalone timeout
operations, scheduler timers, or cooperative deadline checks.

Current status: the SGX coroutine io_uring stream uses a linked
`RECV + LINK_TIMEOUT` pair for timed receive. The stream keeps the public API as
a deadline and resubmits only when a retryable native result is returned. The
timeout buffer is retained by the linked operation table entry until its CQE has
been drained.

### Streaming Architecture Integration

The io_uring stream wrappers must follow the same rules as other Canopy streams:

- `send()` should be send-all at the stream contract level, even if the kernel
  completes many partial sends underneath
- `receive(timeout)` should treat the timeout as a deadline over repeated
  low-level receive attempts, not as permission for a single probe
- saturation should yield fairly instead of spinning
- wrapper-generated control work in higher stream layers must still get a chance
  to flush while io_uring operations are pending
- shutdown should be awaitable when a caller needs graceful teardown

Potential streaming bottlenecks to check before wiring io_uring into transports:

- whether stream transports assume only one send and one receive in flight
- whether wrapper streams retain leftover bytes without unbounded staging
- whether large logical messages are chunked before reaching the io_uring stream
- whether watchdog/progress reporting can see partial raw-byte progress under
  heavy load
- whether close propagation can wait for io_uring cancellation/close completion

The raw io_uring stream helpers should remain transport-compatible, but the
transport integration should be a separate phase. That keeps socket correctness
testable before RPC framing and wrapper streams add their own backpressure.

### Waiting

An operation awaiter should wait on enclave-owned state, not on a particular CQ
slot.

The no-op path becomes:

```text
allocate operation
submit SQE with IORING_OP_NOP
wake SQPOLL if needed
pump CQ until this operation completes or timeout expires
return operation result
```

Multiple no-op operations can complete in any order. The dispatcher routes
them by `user_data`.

The cooperative wait path is a scheduler-friendly polling path, not a blocking
spin. It pumps the CQ and then yields to the scheduler. The local CPU pause path
is only a fallback for a controller without a scheduler and should not be used
by the SGX coroutine implementation.

The proactor wait path registers the awaiting coroutine in the operation object
and starts a single detached CQ pump if no pump is active. Completed operations
are resumed outside the operation-engine lock.

## Direct Descriptor Model

Direct descriptors are the preferred representation for sockets and files that
are created by enclave-submitted SQEs. They live in the io_uring fixed-file
table and are used by setting `IOSQE_FIXED_FILE` on later SQEs.

For raw SQE generation:

```text
specific fixed slot:
  sqe.file_index = slot + 1

dynamic direct descriptor allocation:
  sqe.file_index = IORING_FILE_INDEX_ALLOC

later operation:
  sqe.flags |= IOSQE_FIXED_FILE
  sqe.fd = direct_descriptor_index
```

The direct descriptor index returned by a CQE is an opaque host/kernel resource
handle. It avoids ordinary fd installation and repeated fd-table lookup, but it
does not make the resource trusted. The host kernel can still deny service or
return adversarial network/file data.

The host controller should register a sparse fixed-file table and expose:

- table size
- dynamic allocation range, if configured
- reserved controller-owned slots
- allowed direct-descriptor operations
- whether direct close is available

The enclave should never guess these limits from kernel version alone. It should
combine runtime probing with descriptor metadata supplied by the host
controller.

## Operation Families

The following helpers should be built on the common operation engine.

### NOP

`no_op()` is the smoke test for:

- concurrent operation ids
- SQ publication
- host SQPOLL wakeup
- CQ dispatch
- event/resume behavior

It should not contain bespoke CQ polling after the operation engine exists.

### TCP

The first TCP helper set is implemented for the dedicated SGX coroutine
io_uring tests:

- socket_direct
- connect on a direct descriptor
- bind/listen on a direct descriptor
- accept_direct
- recv
- send
- shutdown/close_direct

Stream classes should hold `std::shared_ptr<controller>` so the
controller stays alive while stream operations are in flight.

The current stream helpers are still a smoke-test layer. Before they are used as
production streaming backends, they need:

- a direct-descriptor owner that closes exactly once
- explicit close/cancel semantics for in-flight send and receive operations
- receive timeout support that cannot race with a later recv CQE
- send-all behavior with partial-send retry and fair yielding
- per-stream capacity accounting for host buffers and pending operations
- controller-level saturation telemetry
- tests with multiple simultaneous streams sending in both directions

The current loopback self-ping covers both directions in one enclave:

```text
server:
  SOCKET_DIRECT -> BIND -> LISTEN -> ACCEPT_DIRECT -> RECV -> SEND

client:
  SOCKET_DIRECT -> CONNECT -> SEND -> RECV
```

The reusable shape remains:

```text
SOCKET_DIRECT -> BIND -> LISTEN -> ACCEPT_DIRECT -> SEND/RECV -> CLOSE_DIRECT
```

If the listening socket is itself a direct descriptor, accept must set
`IOSQE_FIXED_FILE` for the listen descriptor and also set `file_index` for the
accepted direct descriptor allocation.

On older deployment kernels, the host may need to pre-create/bind/listen and
provide a fixed descriptor. On newer kernels, bind/listen can be driven through
io_uring as well. This must be selected by runtime capability probing and
deployment policy.

### UDP

UDP helpers should eventually include:

- recvmsg
- sendmsg
- optional multishot receive if supported and worth the complexity

Each datagram completion should be authenticated or copied before application
parsing when confidentiality/integrity matters.

### File I/O

The host can provide useful file access, but it is untrusted. The enclave must
not trust host file credentials, file paths, file size, contents, timestamps, or
metadata without application-level verification.

Useful host-brokered file operations:

- open under host policy and return an opaque descriptor or fixed-file index
- openat2_direct submitted by the enclave when policy allows direct path opens
- pread/pwrite using outside-enclave buffers
- close/release resource handles
- optional file metadata for convenience, not trust

The enclave should authenticate file contents using one of:

- expected whole-file digest
- chunk digest table or Merkle tree
- signature over metadata and content digests
- encrypted/authenticated application file format

### Host-Mapped File Blobs

For large read-mostly blobs, host-side `mmap` can be much faster than copying
through repeated RPC calls.

The host file-control interface can return a mapping descriptor:

- outside-enclave address
- length
- page alignment
- optional file size and mapping generation
- optional chunk size for verification
- opaque resource handle for releasing the mapping

The enclave must treat mapped memory as mutable untrusted memory. To avoid
TOCTOU bugs, parsing code should either:

- copy a bounded chunk into enclave memory before verifying and using it, or
- use an authenticated chunk format where each chunk is independently verified
  before use

Confidential blobs should be encrypted outside the enclave and decrypted inside
the enclave.

There is not yet a stable Canopy dependency on `IORING_OP_MMAP`. The local 6.19
headers do not expose it, and current upstream discussion treats it as future
work for mmap through io_uring and fixed files. Until that is in the deployment
baseline, mapped blobs should be brokered by the host controller and represented
by explicit mapping descriptors.

If `IORING_OP_MMAP` becomes available, it should be an optimization of the same
resource model rather than a separate trust model. The mapped memory remains
outside-enclave, mutable, and untrusted.

### Time And Timers

Timer operations can be useful for timeouts and scheduling. The enclave should
treat host/kernel time as an availability and ordering input, not as trusted
wall-clock truth. Trusted time, if needed, requires a separate attestation or
platform-specific design.

## Security Model

The host and all host/kernel memory remain untrusted.

The host can:

- deny service
- corrupt ring memory
- corrupt file mappings
- reorder, omit, or duplicate completions
- close or substitute host resources
- lie about metadata
- observe timing, sizes, and access patterns

The enclave response should be:

- validate every descriptor and pointer before use
- copy CQEs before dispatching
- reject unknown operation ids
- reject impossible ring states
- authenticate data before parsing
- encrypt confidential data before it leaves enclave memory
- fail closed on corruption

This engine improves performance and structure. It does not make host resources
trusted.

## Implementation Phases

### Phase 1: Completion Engine Foundation

- Add an enclave-owned operation table.
- Add a controller-level SQ coordinator.
- Add a single logical CQ dispatcher.
- Rewrite `no_op()` to use the operation engine.
- Keep the host implementation unchanged.
- Make the scheduled 1000-noop stress test pass.
- Keep `cooperative_poll` as the default wait strategy; leave proactor
  configurable.

### Phase 2: Backpressure, Cancellation, And Shutdown

- Add controller high-water/limit settings for in-flight operations, pending
  submissions, borrowed host buffers, and direct descriptors.
- Add a bounded pending submission queue that waits through the scheduler rather
  than retrying in a hot loop.
- Add per-stream/owner ids to operations so close and cancellation affect the
  right connection.
- Add operation timeout/cancellation state.
- Add cancellation mechanics for pending operations and submitted operations.
  Descriptor-scoped async cancellation is implemented for direct-descriptor
  close; controller shutdown now fails queued waiters and operation-table
  waiters. Pending-operation cancellation tokens and operation-id-specific
  cancellation remain open.
- Make controller shutdown fail or cancel outstanding operations. First-stage
  shutdown is implemented by rejecting new work and failing queued/submitted
  enclave-owned operations.
- Ensure destructor-dispatched cleanup does not capture `this`.
- Add telemetry counters for saturation, pending queue length, cancellations,
  timeouts, and cleanup failures.

### Phase 3: Direct Descriptor And Stream Ownership

- Add direct-descriptor metadata to `rpc::io_uring::data`. Implemented for the first
  fixed-file metadata shape.
- Add fixed-file table setup to the host controller. Implemented for the SGX
  coroutine io_uring test host.
- Add outbound TCP helpers first: socket_direct, connect, send, recv,
  close_direct. Implemented as a smoke path.
- Add inbound TCP helpers after that: bind, listen, accept_direct. Implemented
  as a smoke path.
- Build reusable enclave io_uring stream classes. First stream, acceptor, and
  connector wrappers are implemented.
- Add a direct-descriptor owner/guard used by stream, acceptor, and connector
  code.
- Make stream close awaitable and ensure it cancels/drains in-flight operations
  before releasing the direct descriptor.
- Make stream destructors best-effort cleanup only, not the primary graceful
  shutdown mechanism.
- Keep RPC stream transport integration separate from the raw stream helpers.

### Phase 4: TCP Timeout, Backpressure, And Stress Tests

- Implement send-all with partial-send retry and scheduler-friendly
  backpressure. Partial-send retry is implemented; saturation policy still needs
  targeted tests.
- Implement `receive(timeout)` as deadline-based repeated receive attempts.
  Implemented for timed TCP receive.
- Add linked timeout/cancel support or an equivalent scheduler deadline path
  that prevents buffer reuse before the recv operation is safe. Linked timeout is
  implemented for receive; descriptor-scoped async cancellation is implemented
  for direct-descriptor close.
- Add multi-stream stress tests with concurrent bidirectional traffic.
  Implemented for the SGX coroutine io_uring harness with multiple accepted
  loopback streams sharing one controller. The test claims stream ids from the
  first payload rather than assuming `accept()` order matches client spawn order.
- Add a first host-buffer pressure test. Implemented with a deliberately small
  host buffer pool and concurrent loopback streams. Single-buffer allocation now
  yields to the scheduler and retries while all slots are borrowed, so bursty
  send/receive traffic does not fail on the first full-pool observation. Timed
  receive also reserves its data buffer and linked-timeout buffer together, so a
  coroutine cannot hold half of a linked receive while waiting for the other
  half.
- Add host-buffer admission fairness. Implemented as a FIFO reservation queue:
  once pressure exists, later single-buffer or paired-buffer requests do not
  bypass the waiter at the front of the queue. The wait path still yields through
  the scheduler and remains bounded by the existing allocation retry limit.
- Add SQ/CQ submission admission fairness. Implemented as a controller-level
  FIFO submitter queue for NOP, single-SQE operations, and linked two-SQE
  operations. Only the waiter at the front of the queue calls into the operation
  engine, so linked operations cannot be starved by later single-SQE callers
  repeatedly racing through `try_submit`.
- Add saturation tests for SQ capacity, CQ capacity, host buffer slots, and
  fixed-file slots. Small host-buffer-pool and small submission-ring loopback
  stress tests are implemented. Fixed-file coverage now includes a small-table
  direct socket test that fills the direct descriptor table, verifies the next
  allocation fails promptly, closes the descriptors, and repeats the cycle to
  prove slots are reusable. A small-table self-ping loop also verifies the
  stream/acceptor/connector RAII close path returns direct descriptor slots.
- Add close-during-send, close-during-receive, close-during-accept, timeout,
  peer-close, and controller-shutdown tests. The close-during accept, receive,
  and send cases are covered in the SGX coroutine io_uring harness. Controller
  shutdown is covered for post-shutdown rejection, scheduled no-op callers, and
  pending accept, timed receive, and send operations.
- Compare cooperative and proactor strategies under TCP load before changing
  defaults.

### Phase 5: Streaming Transport Integration

- Audit `streaming::stream` callers for assumptions about in-flight send/receive
  concurrency.
- Integrate the io_uring stream with transport code only after raw stream
  stress tests pass.
- Verify wrapper streams propagate backpressure and close into the underlying
  io_uring stream.
- Add transport-level tests with large payloads, small buffers, timeouts, and
  graceful shutdown.

### Phase 6: Host Resource Interfaces

- Add host-brokered file resource interfaces.
- Add direct `OPENAT2` support where policy allows it.
- Add opaque descriptors or fixed-file registration metadata.
- Keep policy separate from ring mechanics.

### Phase 7: Mapped Blob Reads

- Add host file mapping descriptors.
- Add enclave chunk-copy and authentication helpers.
- Add tests for host mutation/corruption during reads.

### Phase 8: Broader Operation Set

- Add UDP helpers.
- Add timer/time helpers where useful.
- Consider batching and lock-free SQ submission only after the correctness
  model is stable.

## Immediate Acceptance Criteria

- `shared_from_this()` works for the SGX test object layout.
- Concurrent scheduled no-op calls no longer crash or hang.
- CQ completions can arrive out of order and still resume the correct waiter.
- Unknown `user_data` is detected and reported.
- A stream can be closed while send/receive operations are pending without
  leaking a direct descriptor or registered host buffer.
- Host-buffer and ring saturation cause bounded waiting, fair yielding, or a
  clear error; they do not grow memory unboundedly.
- Multiple TCP streams can perform bidirectional I/O concurrently without one
  stream monopolising the controller.
- Receive timeouts do not permit caller buffer reuse while a kernel recv may
  still write to the host buffer.
- Transport integration preserves Canopy stream backpressure and shutdown
  expectations.
- The host controller API remains narrow; NOP still only needs descriptor
  discovery and optional SQPOLL wakeup.
- The new engine code has comments only around non-obvious concurrency and
  untrusted-memory handling.
- The first direct-descriptor TCP self-ping proves the stream-shaped helpers can
  move bytes before they are wired into RPC transports.

## Known Weaknesses And Improvements

- SQ/CQ submission now has FIFO admission, but it is still cooperative. The
  front waiter owns retry attempts and yields through the scheduler; there is no
  detached submission pump that wakes queued submitters directly when CQ
  completions free capacity.
- Host-buffer pressure handling now has FIFO admission for one- and two-buffer
  reservations. Future operations that need larger buffer sets should use the
  same all-or-nothing reservation principle rather than borrowing slots one at a
  time.
- Fixed-file table exhaustion is covered for direct socket allocation and
  normal stream close/reuse. Close racing against in-flight accept, timed
  receive, and send operations is also covered for the current stream wrappers.
- Timed TCP receive uses a linked timeout operation with its own operation id.
  Because both real timeout and explicit close can surface as a cancelled
  primary receive, the stream wrapper currently uses its closed/descriptor state
  to distinguish explicit close from deadline expiry. Future operation-level
  cancellation should carry a clearer cancellation source.
- Controller shutdown is coarse-grained. It satisfies controller-owned
  coroutine waiters by failing queues and the operation table, but it does not
  yet track every live direct descriptor or submit async cancellation for every
  descriptor during shutdown.
- Runtime cleanup is enclave-scoped. The final SGX coroutine runtime shutdown
  requests the shared controller shutdown before the last scheduler drain, which
  lets RAII cleanup from all child zones complete before only the parent-zone
  transport remains.
- This enclave-scoped model still needs a multi-zone lifecycle test. The test
  should create several child zones in one enclave, run stream work through the
  shared controller, release the zones, and prove the final runtime cleanup does
  not hang and does not leave io_uring-backed transports alive. The first
  peer-to-peer RPC test now covers two independent enclave root services sharing
  one controller and scheduler, but it is intentionally narrower than the final
  lifecycle fan-out test.
- Shutdown must be requested before yielding into a large fan-out. The
  synchronous `request_shutdown()` transition exists for that path, and the
  scheduled no-op shutdown test now requests shutdown before yielding so all
  spawned callers complete by observing the stopped controller.
- The controller does not yet have a live direct-descriptor registry. Adding one
  would let shutdown issue descriptor-scoped cancellation/close for every live
  descriptor and make shutdown less dependent on host ring teardown.
- The current acceptor is single-accept oriented. Multishot accept may reduce
  submission cost later, but it needs careful cancellation and ownership rules.
- Fixed-buffer allocation is deliberately simple. Larger stream/file workloads
  need buffer pools, scatter/gather support, and explicit bounds on pinned host
  memory.
- Backpressure observability is still limited. The controller should expose
  counters for descriptor allocation/release, close/cancel attempts, queued SQ
  waiters, queued host-buffer waiters, host wakeups, and shutdown fan-out before
  being used for heavy concurrent TCP or file I/O.
- Runtime capability probing is still too shallow. The enclave should not rely
  on kernel version or host claims alone for direct socket, bind, listen, accept,
  close, openat2, and future mmap support.
- Cancellation source attribution is still coarse. Timeout, explicit stream
  close, controller shutdown, transport shutdown, and caller cancellation should
  become distinct observable outcomes where the public stream contract needs to
  tell them apart.
- The self-ping test uses localhost and plaintext. Real enclave connectivity
  still needs TLS/RA-TLS or an enclave-owned authenticated record layer before
  application data is treated as protected.
