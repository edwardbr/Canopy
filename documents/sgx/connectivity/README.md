<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# SGX Connectivity And io_uring

This note is a plan for giving coroutine-enabled SGX enclaves direct access to
stream connectivity through io_uring while preserving Canopy's stream and
transport abstractions.

The goal is not to make the host trusted. The host still creates the enclave,
creates or passes kernel objects, maps the io_uring rings, and can always deny
service. The goal is to remove host-owned RPC relay code from the normal data
path and keep application plaintext, transport keys, and capability material
inside the enclave.

## Current Direction

The active SGX io_uring direction is documented in
`io-uring-runtime-scheduler-architecture-v2.md`. The original controller plan
is preserved in `io-uring-controller-plan.md` as deprecated V1 context. The
next-stage operation engine and file/blob resource plan is documented in
`io-uring-operation-engine-plan.md`. Performance hypotheses and TCP-specific
follow-up items are collected in `optimisation.md`.

In short, the V2 direction separates runtime ownership from io_uring ownership.
The host transport has a one-to-one relationship with an enclave runtime owner.
The enclave runtime owns a scheduler. An `io_uring_scheduler` may own one
`rpc::io_uring::controller`, and that controller uses a formal
`io_uring_handle` abstraction. Enclaves use a proxy-backed handle; host-only
users can use a direct host implementation over the same controller API.

The reusable host/enclave controller types live under
`c++/subcomponents/io_uring`. SGX tests may adapt those types onto the existing
test `i_host` interface, but the io_uring resource ownership and enclave helper
logic should not live in test fixtures.

The generic service-object singleton idea is parked separately in
`documents/protocol/service-object-singleton-proposal.md`.

## Current Implementation Status

The current implementation has two smoke-test stages:

- the host owns a per-enclave `host_controller`
- the host sends a bootstrap RPC object that can be cast to
  `i_io_uring_control`
- the enclave uses that object only during connection setup, then transfers the
  user-supplied interface and drops the normal RPC pointer
- the in-enclave `host_transport` retains one explicit host reference to the
  `i_io_uring_control` object, stores the remote object descriptor, and releases
  that reference as part of transport disconnect
- the enclave-side `rpc::io_uring::controller` does not own a shared or
  optimistic RPC pointer to `i_io_uring_control`; it asks `host_transport` to
  perform the narrow serialized `wake_iouring` and `get_iouring_data` calls
- the enclave caches `rpc::io_uring::data` returned through that transport-owned
  path
- `controller::no_op()` writes `IORING_OP_NOP` SQEs directly
  into the shared ring and attributes CQEs by enclave-generated `user_data`
- `controller` has direct-descriptor TCP helpers for socket,
  bind, listen, accept, connect, send, recv, and close
- `stream`, `acceptor`, and
  `connector` provide the first reusable stream-shaped wrapper
  over those helpers
- the SGX coroutine test suite exercises one NOP, 1000 sequential NOPs, and
  1000 scheduled NOPs
- the dedicated SGX coroutine io_uring test enclave also exercises
  `self_ping_test()`, where an enclave acceptor listens on localhost, an
  enclave connector sends `hello`, and the accepted stream replies `world`
- the same harness now includes repeated and multi-stream loopback self-ping
  tests, including a small host-buffer-pool case that verifies temporary
  host-buffer allocation yields and retries under pressure
- the SGX coroutine harness also has a first peer-to-peer RPC test: one enclave
  creates two independent root services on the same scheduler, shares the same
  `rpc::io_uring::controller`, connects them over an io_uring TCP stream, and
  verifies RPC calls in both directions through the dedicated
  `io_uring_test::i_peer2peer` interface
- that test keeps the two endpoint roles readable as separate
  `run_tcp_server` and `run_tcp_client` coroutine functions; the enclave build
  uses SGX-compatible spawn/done events to run them concurrently because the
  current enclave libc++ configuration cannot include libcoro's
  `<coro/when_all.hpp>` dependency on `<ranges>`
- the repeated server/client setup is factored behind `end_2_end_setup`, which
  accepts separate client and server `i_peer2peer` endpoint factories so new
  peer-to-peer tests can reuse the same two-zone io_uring transport harness
- timed receive reserves its data buffer and linked-timeout buffer together, so
  linked receive setup does not consume one scarce host buffer while waiting for
  another
- host-buffer pressure now uses FIFO admission, so queued one-buffer and
  two-buffer reservations are granted in order rather than letting later callers
  repeatedly bypass earlier waiters
- SQ/CQ submission pressure also uses FIFO admission for NOP, single-SQE, and
  linked two-SQE operations, and the test harness includes a small-ring
  multi-stream loopback case
- fixed-file/direct-descriptor coverage now includes a small fixed-file table
  test that fills the table, verifies exhaustion, closes all descriptors, and
  repeats to prove reuse; repeated self-ping also exercises stream RAII cleanup
  on the same small table
- direct-descriptor close now submits descriptor-scoped async cancellation
  before close, and the test harness covers close while `accept`, timed
  `receive`, and `send` operations are pending
- controller shutdown now rejects new work, fails queued SQ and host-buffer
  admission waiters, fails the enclave operation table, and resumes registered
  proactor waiters; the test harness covers shutdown with pending accept,
  timed receive, send, and scheduled no-op calls
- SGX coroutine runtime cleanup can now register enclave-scoped cleanup
  handlers, so one shared enclave io_uring controller can be asked to shut down
  before the final scheduler drain even when many child zones have used it

The wait strategy is configurable. `cooperative_poll` is the default because it
currently gives the better latency result in the NOP stress tests. The proactor
path works and remains available for measurement, but it is not the default
until a real stream operation shows that lower CQ pump/yield activity is worth
the additional suspend/resume cost.

The `total_ticks` and `max_ticks` NOP counters are raw local tick-counter
deltas. They are diagnostic latency-sum counters, not wall-clock runtime and not
CPU usage. In scheduled tests `total_ticks` is the sum of many overlapping
operation waits, so it can look much larger than elapsed test time.

The current TCP path is still a smoke path rather than a production transport.
It proves that enclave-submitted direct-descriptor operations can create a
loopback TCP connection and move bytes through `streaming::stream`, but it does
not yet include transport integration, TLS/RA-TLS wrapping, multishot accept,
operation-id-specific cancellation, production backpressure policy, or
transport-level graceful shutdown.

### Build Note: Nanopb Generated Sources

The nanopb build should treat generated `.pb.c` files the same way the
Protocol Buffers path treats generated `.pb.cc` files. The current direction is
therefore to compute `NANOPB_PB_SOURCES` from the generated `manifest.txt` when
it is available, fall back to `generated_nanopb_sources.cmake` during early
configure cases, and compile the generated `.pb.c` files directly.

This avoids the older aggregate-source workaround, where a single generated C
file included all nanopb `.pb.c` files. That aggregate file only changed when
the include list changed, so an IDL edit that changed descriptor contents but
kept the same filenames could leave a stale aggregate object in one-shot
builds.

## Current Baseline

The coroutine SGX transport currently has this shape:

```text
host service
  -> rpc::sgx::coro::enclave::transport
  -> coroutine_init_enclave / coroutine_enter_thread
  -> host-owned SPSC queues
  -> streaming::spsc_queue::stream
  -> rpc::stream_transport::transport inside the enclave
```

`coroutine_init_enclave` receives:

- the encoded init request
- a host-to-enclave queue pointer
- an enclave-to-host queue pointer
- a host-owned startup status pointer
- a response buffer

The enclave validates those pointers as outside-enclave memory and then builds a
normal `rpc::stream_transport::transport` over `streaming::spsc_queue::stream`.
This is good layering for RPC semantics, but the queues are host-controlled and
the host remains the transport relay.

The current io_uring code is split:

- `c++/streaming/io_uring` is header-only and contains a stream plus a simple
  acceptor. It is named generically but still assumes socket-style send/recv.
- `c++/streaming/io_uring_tcp` contains a separate TCP stream and acceptor with
  its own ring management.

Both are too TCP-shaped for enclave connectivity. They should be refactored
toward a reusable io_uring engine that can back TCP streams, accepted streams,
connected streams, and later file streams.

## Non-Negotiable SGX Constraints

An enclave cannot treat io_uring shared memory as trusted. The submission ring,
completion ring, SQEs, CQEs, registered buffers, socket fds, and any file data
buffers are outside enclave memory.

The kernel also cannot safely read or write enclave-private buffers as normal
I/O buffers. Network and file buffers used by io_uring should therefore be
outside-enclave bounce buffers. If the data is sensitive, the enclave must
encrypt before submitting writes and authenticate/decrypt after receiving reads.

For network RPC, the intended order remains:

```text
typed RPC data
  -> serialise
  -> optional compression
  -> enclave-owned TLS, RA-TLS, or Canopy AEAD record layer
  -> outside-enclave io_uring buffer
  -> kernel/network
```

For inbound data the reverse order applies. RPC deserialisation should not run
until the encrypted record has passed authentication inside the enclave.

The host can still:

- destroy the enclave
- close or corrupt process fds
- stop worker ECALLs
- corrupt the mapped ring memory
- starve or wake the io_uring polling thread at bad times
- observe routing metadata, ciphertext lengths, timing, and queue pressure

These are denial-of-service and side-channel concerns. They are not solved by
io_uring; they must be handled by authenticated framing, quotas, shutdown
policy, and careful logging.

## Desired Connectivity Model

The enclave should build an `io_uring` connectivity context during coroutine
runtime setup, after the normal SGX coroutine RPC connection is established.
Enclave code can then use that context in two ways:

- accept incoming connections and bind each accepted stream either to
  `rpc::stream_transport` or to an application-level stream handler
- connect outbound to another endpoint and either build a Canopy transport over
  the stream or use the stream directly for non-RPC protocols

The core abstraction should remain `streaming::stream`. RPC code should not care
whether the stream came from SPSC, TCP, TLS, WebSocket, io_uring, or enclave
io_uring.

```text
enclave application
  -> streaming::listener or direct stream handler
  -> enclave io_uring acceptor / connector
  -> enclave-owned TLS / RA-TLS / AEAD, optional
  -> streaming::stream
  -> rpc::stream_transport, optional
```

This keeps two separate use cases:

- Canopy transport connectivity: accepted or connected streams are handed to
  `rpc::stream_transport`.
- Non-RPC connectivity: accepted or connected streams are handed to application
  code as plain `streaming::stream`.

## Runtime Initialisation Shape

`coroutine_init_enclave` builds the private host/enclave control channel in an
explicit order:

```text
SPSC stream
  -> rpc::enclave_service
  -> rpc::sgx::coro::enclave::host_transport
```

The runtime creates one `enclave_service` for the host connection rather than a
root service. The registered enclave connection handler receives that service
from the host transport, casts it to `enclave_service`, and completes the
child-zone wiring against the already-existing host transport.

The io_uring capability is still passed as an ordinary RPC object through the
existing `connect_to_zone` input path after the normal SGX coroutine connection
handshake has completed.

That object exposes `i_io_uring_control`, from which the enclave can obtain
a versioned `rpc::io_uring::data` snapshot and request a narrow SQPOLL wakeup.
After setup, however, this object is not retained as a normal
`rpc::shared_ptr` or `rpc::optimistic_ptr` in the enclave. The host transport
owns the retained host reference and performs the generated YAS request and
response serialization directly through `outbound_send`.

This keeps capability exchange inside Canopy's typed RPC path and avoids
turning `canopy_coroutine_startup_status*` into a second capability protocol.

A future raw startup descriptor may still be useful for non-RPC bootstrap, but
it should be treated as a separate ABI proposal. If such a descriptor is added,
it must be versioned and should carry the same information currently returned by
`i_io_uring_control`: ring mappings, queue sizes, copied
`io_uring_params`, fixed-buffer metadata, fixed-file/direct-descriptor metadata,
operation allowlists, and resource policy. A malformed descriptor should fail
startup, not fall back to an insecure host relay.

Whichever route supplies the descriptor, the enclave must validate every pointer
as outside enclave memory, aligned, and within the declared range before it
touches the ring.

## Kernel Operation Direction

The preferred long-term model is to use io_uring direct descriptors whenever the
kernel supports them. A direct descriptor lives in the ring's fixed-file table
and is addressed by index with `IOSQE_FIXED_FILE`; it is not installed in the
normal process fd table.

The host controller should register a fixed-file table up front. The enclave
then treats returned descriptor indexes as opaque host/kernel resource handles.
They are useful for performance and for avoiding ordinary host-userspace fd
creation, but they are not security capabilities. The host kernel can still deny
service, substitute data, or observe metadata.

Current target operations:

```text
outbound TCP:
  IORING_OP_SOCKET with direct descriptor allocation
  IORING_OP_CONNECT using IOSQE_FIXED_FILE
  IORING_OP_SEND / IORING_OP_RECV using IOSQE_FIXED_FILE
  IORING_OP_CLOSE for the direct descriptor

inbound TCP:
  IORING_OP_SOCKET with direct descriptor allocation
  IORING_OP_BIND using IOSQE_FIXED_FILE
  IORING_OP_LISTEN using IOSQE_FIXED_FILE
  IORING_OP_ACCEPT using IOSQE_FIXED_FILE for the listen descriptor and direct
    descriptor allocation for the accepted socket
  IORING_OP_SEND / IORING_OP_RECV using IOSQE_FIXED_FILE
  IORING_OP_CLOSE for accepted direct descriptors

file reads/writes:
  IORING_OP_OPENAT2 with direct descriptor allocation
  IORING_OP_READ / IORING_OP_WRITE using IOSQE_FIXED_FILE
  IORING_OP_CLOSE for the direct descriptor
```

For raw SQE generation, `file_index` must be handled explicitly. A specific
direct descriptor slot is encoded as `slot + 1`; dynamic allocation uses
`IORING_FILE_INDEX_ALLOC`. Subsequent operations set `IOSQE_FIXED_FILE` and put
the direct descriptor index in `sqe.fd`.

All pointer arguments supplied to kernel operations, such as `sockaddr`,
`open_how`, iovecs, paths, and data buffers, must point at host/kernel-readable
outside-enclave memory that remains stable until submission is complete. The
enclave should use controller-managed request buffers rather than pointing the
kernel at enclave-private memory.

Kernel support varies and should be feature-probed at runtime rather than
selected only by version number. As a working reference, direct descriptors for
open/accept are in the Linux 5.15 era, `IORING_OP_SOCKET` is documented from
5.19, multishot accept from 5.19, and `IORING_OP_BIND`/`IORING_OP_LISTEN` from
6.11. `IORING_OP_PIPE` direct-descriptor creation is newer and documented from
6.16. The local 6.19 headers expose socket, bind, listen, accept, openat2,
close, and pipe direct-descriptor building blocks.

## Submission And Wakeup Model

The preferred steady-state mode is SQPOLL:

```text
enclave writes SQE into shared SQ ring
  -> enclave advances SQ tail with release ordering
  -> kernel SQPOLL thread observes new work
  -> kernel completes into CQ ring
  -> enclave polls CQ ring and resumes coroutine
```

This is the closest shape to "no host communication" after bootstrap. It still
depends on the kernel polling thread and outside-enclave ring memory.

The difficult case is `IORING_SQ_NEED_WAKEUP`. If the SQPOLL thread goes idle,
Linux requires a userspace `io_uring_enter(..., IORING_ENTER_SQ_WAKEUP)` call to
wake it. Normal SGX enclaves should not issue syscalls directly. The design
therefore needs one of these policies:

- configure SQPOLL with an idle timeout high enough that production enclave I/O
  does not normally require wakeups
- provide a narrow wakeup OCALL that only calls `io_uring_enter` with
  `IORING_ENTER_SQ_WAKEUP` for the already-registered ring
- treat wakeup as untrusted availability help and never pass payload data through
  it
- expose telemetry for wakeup frequency so accidental host dependence is visible

The wakeup OCALL must not be a generic syscall escape hatch. It should not allow
the host to inspect buffers, choose operations, rewrite SQEs, or learn more than
the fact that the enclave asked for the ring to be woken.

## Completion Model

The enclave runtime should own the CQ polling loop. A scheduler task or worker
loop can poll CQ head/tail in outside-enclave memory, copy CQEs into enclave
temporaries, validate the user data, and resume the waiting coroutine.

Important details:

- user data should be an enclave-generated opaque operation id
- completions for unknown ids are fraud or corruption, not harmless noise
- result codes should be copied into enclave-owned operation state before
  resuming user code
- timeout and cancel CQEs must be associated with the exact operation they
  belong to
- CQ ring corruption should fail the connectivity context and close all streams
  using it

The first SGX coroutine test implementation now associates timed receives with a
linked `IORING_OP_LINK_TIMEOUT` SQE. The timeout SQE has its own enclave-created
operation id so the completion table can drain both CQEs without accepting
unknown completions. This covers the normal `receive(timeout)` stream contract.
Direct-descriptor close now uses `IORING_OP_ASYNC_CANCEL` scoped to the direct
descriptor before submitting `IORING_OP_CLOSE`, so close wakes pending accept,
receive, and send waiters in the current TCP stream layer. It does not yet solve
operation-id-specific cancellation or caller cancellation tokens.

Controller shutdown is deliberately coarse at this stage. It marks the
controller stopped, rejects new work, fails queued admission waiters, and fails
all enclave-owned operation records so controller-owned coroutine waiters
complete. It does not yet maintain a central direct-descriptor registry or
submit descriptor-specific async cancellation for every live stream during
shutdown; host ring teardown remains the final cleanup boundary for that case.
Code that is about to release a large fan-out should call the synchronous
`request_shutdown()` transition before any cooperative yield; the awaitable
`shutdown()` wrapper does the same transition before it can suspend.

In SGX coroutine enclaves the io_uring controller is intended to be
enclave-scoped, not zone-scoped. Many child zones may create transports and
streams backed by the same controller. Normal stream and descriptor destructors
therefore hand cleanup work to the controller; the final enclave runtime
cleanup requests controller shutdown before draining the scheduler one last
time. By then the only remaining transport should be the special parent-zone
transport, which is not backed by the io_uring stream.

The host-control lifetime is now owned by that parent-zone `host_transport`.
During setup it sends an extra add-ref for the remote `i_io_uring_control`
object and stores the release parameters. When the stream transport enters
disconnecting, `host_transport::on_disconnecting()` enqueues the final release
before the stream is torn down. This avoids keeping the controller alive through
a normal RPC proxy cycle while still keeping the host-side io_uring control
object alive for controller calls.

The dedicated SGX coroutine io_uring test harness also exercises several
simultaneous loopback TCP streams over one enclave-side controller. The server
side does not assume that concurrent `accept()` completions match client spawn
order; it identifies the stream from the first payload and rejects duplicate
stream ids.

## Stream And Acceptor Refactor

The stream layer should be refactored into a reusable io_uring core:

```text
streaming::io_uring::ring
  owns or borrows ring mappings, operation ids, CQ polling, cancellation

streaming::io_uring::descriptor
  represents a kernel fd or direct descriptor produced by accept/connect/open

streaming::io_uring::descriptor_stream
  implements streaming::stream using recv/send or read/write on a descriptor

streaming::io_uring::acceptor
  submits accept/multishot accept and returns descriptor_stream

streaming::io_uring::connector
  submits socket/connect and returns descriptor_stream

streaming::io_uring::file
  later: submits open/read/write/close or returns random-access file operations
```

TCP-specific code should become thin policy and address conversion:

```text
streaming::io_uring_tcp::acceptor
  -> builds sockaddr policy
  -> uses streaming::io_uring::acceptor

streaming::io_uring_tcp::connector
  -> builds socket/connect SQEs
  -> returns streaming::io_uring::descriptor_stream
```

That avoids having `io_uring` exclusively bound to TCP while still letting TCP
remain the first production user.

## Accepting Connections In The Enclave

The enclave should support two accept paths over the same acceptor:

```text
accepted stream
  -> security transform, optional
  -> rpc::stream_transport connection callback
```

and:

```text
accepted stream
  -> security transform, optional
  -> application stream handler
```

This can reuse `streaming::listener` for the RPC case. For non-RPC endpoints,
add a sibling listener or helper that accepts streams and invokes an application
coroutine directly.

The dispatch decision should be made by enclave policy, not by the host. For
example:

- port 443: RA-TLS then HTTP/WebSocket gateway
- port 7000: RA-TLS then Canopy `stream_transport`
- unix or internal endpoint: Canopy-native AEAD then application protocol

If public clients are accepted, they should still go through a flat gateway
interface. They should not receive the full Canopy object-reference protocol.

## Outbound Connections From The Enclave

Applications inside the enclave need a direct way to connect out:

```text
auto stream = co_await connectivity.connect_tcp(endpoint, policy);
auto protected_stream = co_await make_ra_tls_client_stream(stream, peer_policy);
```

From there:

- for Canopy, call `rpc::stream_transport::make_client(...)` and
  `service->connect_to_zone(...)`
- for non-RPC protocols, pass the stream to the application protocol parser

Outbound connect policy should be explicit. An enclave should not be able to
connect to arbitrary hosts unless the deployment intentionally grants that
authority. The policy belongs in the connectivity descriptor or in an
attested/sealed configuration loaded by the enclave.

## File Access

io_uring file operations can use the same ring engine later, but file access
needs stricter policy than sockets:

- paths and open flags are visible to the host kernel
- file contents are untrusted unless authenticated or encrypted
- writes are not durable or private merely because they were requested by an
  enclave
- filenames may themselves be sensitive and should be treated as routing
  metadata that the OS can observe

The first safe file profile should be "encrypted and authenticated blob access"
rather than arbitrary POSIX filesystem access. The enclave can request reads and
writes through io_uring, but it should verify file MACs, decrypt contents inside
the enclave, and avoid trusting host-provided directory contents.

For direct file operations, the first enclave-driven path should use
`OPENAT2_DIRECT`, fixed-descriptor `READ`/`WRITE`, and direct close. This avoids
ordinary fd installation, but it still treats the filesystem as untrusted.

For memory-mapped blobs, the current practical path is host-controller managed:
the host maps the file, returns an outside-enclave address/length/resource
descriptor, and owns `munmap` lifetime. The enclave treats the mapping as
mutable untrusted memory and copies/verifies chunks before parsing them.

Do not depend on `IORING_OP_MMAP` yet. It has been proposed upstream to support
mmap through io_uring and fixed files, but it is not present in the local 6.19
headers. If it becomes stable and widely available, it can replace some
host-controller mapping work, but the same untrusted-memory and lifetime rules
will still apply.

## Security Requirements

The io_uring connectivity context should enforce:

- one-shot initialisation tied to the enclave runtime instance
- outside-enclave pointer validation for all ring and buffer mappings
- operation allowlists
- address and file policy allowlists
- maximum SQ/CQ depth and maximum in-flight operations
- maximum accepted connections and outbound connects
- per-stream byte and frame limits
- cancellation and shutdown for all in-flight operations when fraud is detected
- encrypted payloads for untrusted network or file data
- no generic host syscall bridge

The enclave must assume the host can corrupt ring memory. Ring corruption should
close the connectivity context and fail existing streams. It should not be
treated as an ordinary parse error.

## Near-Term io_uring Considerations

The current io_uring work is good enough to continue into the next functional
area, but these points should remain visible while TCP, file, and transport
integration are added:

- add a multi-zone lifecycle test where several child zones in one enclave use
  the shared controller, release their streams, and then final enclave shutdown
  proves all controller cleanup has drained before only the parent-zone
  transport remains; the current peer-to-peer test proves two independent
  enclave root services can share the controller, but it is not yet a broad
  lifecycle fan-out test
- add a live direct-descriptor registry so controller shutdown can issue
  descriptor-scoped cancellation/close for every still-live descriptor instead
  of relying on host ring teardown as the final boundary
- separate cancellation sources so timeout, explicit stream close, controller
  shutdown, transport shutdown, and caller cancellation are reported
  deterministically
- keep backpressure policy explicit for SQ slots, CQ pumping, host buffers,
  direct descriptor slots, and fixed files; queue saturation should cause fair
  waiting, bounded retry, or a clear error, not memory growth or busy spinning
- expand runtime capability probing so the enclave proves socket, bind, listen,
  accept-direct, close, openat2, mmap, and fixed-resource support rather than
  trusting kernel-version claims or host metadata alone
- keep all ring data, CQEs, result codes, fixed-file indexes, and host buffer
  descriptors treated as untrusted inputs; unknown `user_data` remains a
  connectivity-context failure, not harmless noise
- add counters for descriptor allocation/release, close/cancel paths, queued
  admission waiters, shutdown fan-out, host wakeups, and runtime cleanup
  requests before relying on the implementation under heavy concurrent I/O

## Suggested Implementation Phases

1. Keep the NOP operation engine stable.
   The 1000 scheduled NOP test is the baseline for operation ids, concurrent
   waits, CQ attribution, and SQPOLL wakeup behavior.

2. Add direct-descriptor support to the host controller.
   A first fixed-file table and `fixed_file_data` descriptor field are
   implemented for the SGX coroutine io_uring test host. Runtime operation
   probing and richer ranges/policy metadata are still needed.

3. Add outbound TCP first.
   The first smoke path implements `SOCKET_DIRECT`, `CONNECT`,
   fixed-descriptor `SEND`/`RECV`, and direct close for localhost self-ping.
   It still needs production connection policy, TLS/RA-TLS wrapping, and more
   complete stream error handling.

4. Add inbound TCP.
   The first smoke path implements `SOCKET_DIRECT`, `BIND`, `LISTEN`,
   `ACCEPT_DIRECT`, and accepted stream ownership for the same self-ping test.
   RPC transport binding and non-RPC handler integration remain separate work.

5. Add enclave-owned TLS or RA-TLS stream transforms.
   Only after this should accepted or connected network streams be considered
   suitable for protected enclave application traffic.

6. Add file operations as a separate capability.
   Start with `OPENAT2_DIRECT` plus authenticated/encrypted blob reads and
   writes, not general file-system authority.

7. Add host-mapped blob descriptors.
   Use host-side mmap for large read-mostly data, with enclave chunk copy,
   verification, and explicit release handles.

8. Revisit `IORING_OP_MMAP`.
   Only adopt it after it is in the target kernel/liburing baseline or covered
   by reliable runtime probing and fallback to host-controller mappings.

## Open Questions

- Which minimum Linux kernel version should Canopy officially support, given
  direct descriptors, socket direct, bind/listen, and future mmap support arrive
  in different kernel releases?
- Should deployments below the bind/listen baseline pre-create listen sockets
  and pass fixed descriptors, while newer kernels let the enclave submit
  socket/bind/listen/accept operations through the ring?
- How much fd identity can be hidden from the host in practice, given process
  fd tables are host-visible?
- Should each enclave have one global ring or separate rings for network, file,
  and diagnostics?
- What should the default SQPOLL idle timeout be, and is a wakeup OCALL
  acceptable in production?
- How should io_uring connectivity interact with remote attestation policy?
- Which file access profile is safe enough to expose first?

## Kernel References

- `io_uring_enter(2)` documents operation availability, direct descriptor
  semantics, socket support, bind/listen support, and fixed-file use:
  <https://man7.org/linux/man-pages/man2/io_uring_enter.2.html>
- `io_uring_prep_accept_direct(3)` documents accept into the fixed-file table
  and dynamic direct descriptor allocation:
  <https://man7.org/linux/man-pages/man3/io_uring_prep_accept_direct.3.html>
- `io_uring_registered_files(7)` summarises registered files and direct
  descriptor usage:
  <https://manpages.debian.org/unstable/liburing-dev/io_uring_registered_files.7.en.html>
- `IORING_OP_MMAP` is still future-facing for this project; the current design
  should not depend on it:
  <https://lwn.net/Articles/1056726/>
