<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# SPSC Queues and Streams

Scope note:

- this document describes the current C++ SPSC/IPC transport stack
- the SPSC stream and process-hosting transports described here are C++-specific
- see [C++ Status](../status/cpp.md), [Rust Status](../status/rust.md), and
  [JavaScript Status](../status/javascript.md) for implementation scope

Single-Producer Single-Consumer queue-based communication for lock-free IPC.

In the current C++ transport stack, the SPSC queue is a stream primitive, not
a standalone top-level RPC transport API. It is primarily used underneath
coroutine `rpc::stream_transport::transport` implementations.

## Where It Is Used

- High-performance inter-process communication
- Same-machine, different processes
- Lock-free architecture requirements
- `rpc::ipc_transport` shared-memory process-owned connections
- `rpc::libcoro_spsc_dynamic_dll` DLL hosting over process boundaries

## Relationship To The IPC Transports

The current split is:

- `streaming::spsc_queue::stream` turns an SPSC queue pair into a byte stream
- `rpc::stream_transport::transport` turns that stream into an RPC transport
- `rpc::ipc_transport` creates the queue pair, spawns the child process, and
  owns process lifetime
- `rpc::libcoro_spsc_dynamic_dll` consumes an already-created queue pair and
  hosts a DLL zone behind a `stream_transport`
- `ipc_child_process` consumes an already-created queue pair and hosts a direct
  child service behind a `stream_transport`

So the SPSC layer is the message pipe; it is not the process manager, it is not
the DLL loader, and it is not itself a hierarchical transport.

## Requirements

- `CANOPY_BUILD_COROUTINE=ON` (requires libcoro)
- Host-only (no enclave version)

## See Also

- [Stream Backpressure Guidelines](stream_backpressure_guidelines.md)
- [Dynamic Library and IPC Child Transports](dynamic_library.md)
- [TCP Transport](tcp.md)

## Architecture

```
Process A                          Process B
    │                                  │
    │  ┌───────────────────────────┐   │
────┼─►│    send_queue (A → B)     │───┼───►
    │  └───────────────────────────┘   │
    │                                  │
    │  ┌───────────────────────────┐   │
◄───┼──│  receive_queue (B → A)    │◄──┼────
    │  └───────────────────────────┘   │
    │                                  │
```

## Queue Implementation

```cpp
namespace spsc {
    template<typename T, std::size_t Size>
    class queue {
        std::array<T, Size> buffer_;
        std::atomic<size_t> head_{0};
        std::atomic<size_t> tail_{0};
    };

    using message_blob = std::array<uint8_t, 10024>;
    using queue_type = ::spsc::queue<message_blob, 10024>;
}
```

## Queue Pair Setup

The current C++ pattern is to wrap each queue pair in
`streaming::spsc_queue::stream` and then hand the resulting stream to
`rpc::stream_transport`.

```cpp
streaming::spsc_queue::queue_type send_queue;
streaming::spsc_queue::queue_type receive_queue;

auto peer_stream = std::make_shared<streaming::spsc_queue::stream>(
    &receive_queue, &send_queue, io_scheduler);

auto responder_transport = std::static_pointer_cast<rpc::stream_transport::transport>(
    CO_AWAIT peer_service->make_acceptor<yyy::i_host, yyy::i_example>(
        "responder_transport",
        rpc::stream_transport::transport_factory(std::move(peer_stream)),
        interface_factory));

auto client_stream = std::make_shared<streaming::spsc_queue::stream>(
    &send_queue, &receive_queue, io_scheduler);

auto initiator_transport =
    rpc::stream_transport::make_client("initiator_transport", root_service, std::move(client_stream));
```

The envelope and handshake logic lives in `rpc::stream_transport`; the SPSC
layer just moves bytes between the two stream endpoints.

This page should therefore be read as streaming-transport guidance for the
current C++ implementation, not as a statement that every Canopy implementation
has an SPSC-backed IPC stack.
