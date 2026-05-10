<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# io_uring Optimisation Notes

Status: working notes for host and SGX io_uring performance work. These are not
requirements. Treat each item as a hypothesis until a benchmark proves it.

This file intentionally gathers optimisation discussion in one place. The
architecture document should describe ownership and shutdown; this file should
describe performance hypotheses, measurement rules, and TCP-specific tuning
questions.

## Current Baseline

- `streaming::io_uring_new` adapts direct io_uring TCP descriptors into the
  generic `streaming::stream` interface.
- Host-only controllers use `use_caller_buffers_for_transfers=true` by default,
  so SEND/RECV SQEs can point directly at the caller's per-transfer span.
- Enclave controllers must keep `use_caller_buffers_for_transfers=false`
  because caller buffers are enclave-private memory and are not kernel-visible.
- Enclave transfers stage through fixed-size host buffer slots configured by
  `buffer_count` and `buffer_size`.
- The stream API accepts variable-size spans. Large logical transfers may be
  split into smaller controller operations.
- Direct TCP descriptors use the io_uring fixed-file table.
- TCP_NODELAY is enabled for accepted and connected direct TCP descriptors.
- HTML benchmark charts plot latency in milliseconds. Console tables still show
  the native collection units.

## Measurement Rules

- Prefer release builds for performance conclusions. Debug builds are useful
  only for correctness and instrumentation.
- Compare host `tcp`, host `io_uring`, `sgx_io_uring`, and
  `sgx_io_uring_pair` with the same blob sizes, format, warmup, pass count, and
  row ordering.
- Keep send-reply latency and unidirectional throughput separate. They stress
  different parts of the stack.
- Track average, p95, max, standard deviation, failures, and timeout warnings.
  Average alone is not enough.
- Run enough warmup to avoid measuring first-use setup, connection setup,
  buffer-pool cold paths, and SGX startup.
- Use the same scheduler/thread setup when comparing transports. Otherwise the
  benchmark is comparing scheduling policy rather than transport behaviour.
- Keep heavy logging off for performance runs. Timeout-triggered diagnostics are
  preferable to hot-path tracing.
- Treat loopback TCP results as a local-stack measurement. They do not prove
  behaviour on a real network.

## High-Value Optimisation Candidates

### Caller Buffers Versus Host Staging

Host-only io_uring should benchmark both:

- direct caller-buffer transfers
- explicit host-buffer staging

Direct caller-buffer transfer removes a copy and is expected to help host-only
paths. Enclave paths cannot use it for plaintext because enclave memory is not
kernel-visible. If encryption is introduced above the stream, host buffers may
carry ciphertext records, but not enclave plaintext.

### Host Buffer Size And Count

For enclave paths, host buffer slots are the real kernel-visible transfer
buffers. Tune:

- `buffer_size`
- `buffer_count`
- maximum stream transfer size
- encrypted record size, including nonce, tag, padding, and frame headers

Small buffers increase operation count. Oversized buffers increase memory
pressure and cache footprint. The useful size is workload-dependent.

### Proactor Versus Cooperative Completion

Keep proactor/cooperative wait strategy as a benchmark dimension. Proactor mode
can reduce caller-side polling but may add scheduling overhead. Cooperative mode
can be cheaper for short, hot loops but may be less fair.

This should remain configurable per controller. Different priority classes may
eventually use different controllers and therefore different wait strategies.

### Outbound Response Priority

RPC replies and control responses should not be starved behind fresh outbound
requests. A high-priority responder/control lane is still worth investigating.
Candidate messages include:

- `call_receive`
- cast responses
- add-ref/release responses
- connection setup responses
- close acknowledgements

This is a transport-level concern, not an io_uring-only concern.

### Submission Batching

Batching could reduce SQ tail updates and completion wakeups. Useful places to
investigate:

- multiple queued sends becoming multiple SQEs before one publish
- linked operations where ordering is required
- reducing host-control wake calls when SQPOLL does not need waking

Do not add batching until correctness and shutdown behaviour are stable. It can
make cancellation and ownership harder to read.

### Registered Buffers

`register_buffers=true` currently registers the host buffer pool with the
kernel, but registration alone does not guarantee faster TCP send/receive. It
becomes interesting only if the TCP operation path actually uses fixed-buffer
or selected-buffer operations and manages buffer indices correctly.

Until that path exists, registered buffers are a measurement item, not an
assumed optimisation.

### Non-SQPOLL Host Path

SGX needs SQPOLL because the enclave cannot directly perform normal
`io_uring_enter()` submission. Host-only code does not have that restriction.

Benchmarking a non-SQPOLL host path may be worthwhile, especially for workloads
where SQPOLL thread wake/sleep behaviour is more expensive than direct enter.
This is host-only and should not complicate the enclave path.

## TCP-Specific Items To Discuss

### Nagle And TCP_NODELAY

TCP_NODELAY is already enabled for direct io_uring TCP descriptors. It should
remain the default for latency-sensitive RPC. A Nagle-enabled variant can be
benchmarked for bulk throughput, but it is not the expected default.

### Socket Buffer Sizes

Expose optional socket buffer tuning for direct TCP descriptors:

- `SO_SNDBUF`
- `SO_RCVBUF`

This is mostly useful for larger payloads, real network links, or high
bandwidth-delay products. Loopback may hide or distort the effect.

### Small Writes, Framing, And Coalescing

For small RPC messages, per-operation overhead can dominate. Possible
directions:

- coalesce header and payload before sending
- use vectored or message-style submission where available
- batch adjacent outbound frames when latency requirements allow it

This must respect RPC message boundaries and shutdown semantics. It should not
be mixed into the current lifetime work.

### TCP_CORK, MSG_MORE, And Latency

`TCP_CORK` or `MSG_MORE` can help bulk throughput by delaying packet emission
until a full frame is ready. They can hurt p95 latency if used too broadly.

These should be explicit throughput-mode experiments, not defaults.

### MSG_ZEROCOPY

Host-only large-payload TCP might benefit from Linux zero-copy send support.
This is unlikely to help small RPC calls and is awkward for enclave paths where
plaintext cannot live in host memory.

If investigated, measure completion overhead and lifetime complexity, not just
copy avoidance.

### Multishot Accept Or Receive

Multishot accept may reduce server accept overhead. Multishot receive is more
complicated because buffer ownership, framing, cancellation, and timeout rules
must remain clear.

Treat multishot receive as a later experiment.

### Busy Polling

Linux busy-poll options may improve latency on some host deployments and harm
CPU efficiency. They are not an enclave-specific mechanism and should remain a
host-only tuning experiment.

### Loopback Versus Real Network

The current benchmarks are mostly loopback. Loopback can make TCP look unusually
fast and can make io_uring overhead more visible than it would be on a real
network. Before drawing broad conclusions, run at least one non-loopback TCP
benchmark.

## Deferred Or Lower-Priority Items

- Do not refactor the core streaming transport purely for performance until the
  current io_uring and SGX lifetime work is settled.
- Do not add broad send-queue coalescing before responder/control priority is
  understood.
- Do not assume registered buffers help until the operation path uses them.
- Do not optimise around debug logging behaviour.
- Do not make enclave code depend on host-only Linux TCP features that cannot
  map cleanly to future IOCP or other backend implementations.

## Current Best Next Step

Use the benchmark matrix to isolate costs before changing transport behaviour:

1. Host io_uring direct caller buffers, multiple buffer sizes, proactor and
   cooperative modes.
2. Host io_uring forced host-buffer staging, same matrix.
3. SGX io_uring, same buffer sizes and wait strategies.
4. SGX pair benchmark with two enclaves and separate schedulers/controllers.
5. Compare against TCP using the same payloads and formats.

Only after that should we choose whether the next code change is buffer sizing,
priority lanes, batching, or TCP-specific tuning.
