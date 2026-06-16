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

In the current C++ transport stack, the SPSC queue is the stream primitive used
underneath coroutine `rpc::stream_transport::transport` implementations. The
`rpc::spsc_queue` helpers provide direct typed stream and RPC factory functions
for tests and peer-to-peer in-process compositions. The configured
`rpc::connection_factory` API can also create SPSC-backed stream RPC
connections when the queue pair is supplied through
`rpc::connection_factory::context`.

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
- `rpc::spsc_queue::connect_stream` and `rpc::spsc_queue::accept_stream` turn an
  existing queue pair into the two stream endpoints
- `rpc::spsc_queue::connect_rpc` and `rpc::spsc_queue::accept_rpc` attach the
  normal stream transport handshake and generated interfaces
- `rpc::ipc_transport` creates the queue pair, spawns the child process, and
  owns process lifetime
- `rpc::libcoro_spsc_dynamic_dll` consumes an already-created queue pair and
  hosts a DLL zone behind a `stream_transport`
- `canopy_ipc_child_host_process` consumes an already-created queue pair and
  hosts a DLL-backed child service behind a `stream_transport`
- direct child-process bootstrap support exists, but the in-tree
  `canopy_ipc_child_process` executable is currently disabled pending rework and
  still hardcodes the example test interfaces in its source

So the SPSC layer is the message pipe; it is not the process manager, it is not
the DLL loader, and it is not itself a hierarchical transport.

An SPSC queue pair is a single peer-to-peer connection. Unlike TCP and io_uring
acceptors, it does not accept multiple independent connections from one listener.

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

For direct SPSC use, prefer the `rpc::spsc_queue` factory helpers. They take a
shared queue pair plus `rpc::stream_transport::connection_settings`, which is
the direct typed settings object for the stream RPC transport.

```cpp
#include <streaming/spsc_queue/factory.h>
#include <transports/streaming/factory.h>

auto queues = rpc::spsc_queue::queue_pair::create();

rpc::stream_transport::connection_settings options;
options.transport.name = std::string("spsc_transport");
options.transport.service_proxy_name = std::string("peer");
options.transport.encoding = rpc::encoding::yas_binary;
```

Run the two peers concurrently. `accept_rpc` accepts exactly one stream and waits
for the transport handshake, so do not await it to completion before starting
the connecting side.

Peer A:

```cpp
auto accept_result = CO_AWAIT rpc::spsc_queue::accept_rpc<yyy::i_client, yyy::i_server>(
    server_interface,
    queues,
    options,
    server_service);
```

Peer B:

```cpp
auto connect_result = CO_AWAIT rpc::spsc_queue::connect_rpc<yyy::i_client, yyy::i_server>(
    client_interface,
    queues,
    options,
    client_service);
```

When configuration owns the stream topology, use the high-level
`rpc::connection_factory` API instead. The JSON or generated connection
settings name `spsc_queue` as the base stream type, while the actual queue pair
stays in `rpc::connection_factory::context` because it is a runtime dependency,
not serialisable configuration.

```cpp
#include <connection_factory/connection_factory.h>
#include <json/convert.h>
#include <spsc_queue_stream/spsc_queue_stream_config.h>
#include <streaming/spsc_queue/factory.h>
#include <stream_transport/stream_transport_config.h>

auto queues = rpc::spsc_queue::queue_pair::create();

rpc::connection_factory::context context;
context.set_spsc_queues(queues);

rpc::stream_transport::transport_settings transport;
transport.name = std::string("spsc_transport");
transport.service_proxy_name = std::string("peer");
transport.encoding = rpc::encoding::yas_binary;

rpc::connection_factory::connection_settings options;
rpc::connection_factory::typed_settings transport_settings;
transport_settings.type = "stream_rpc";
transport_settings.settings = json::v1::convert::to_json_object(transport);
options.transport = std::move(transport_settings);

rpc::stream_layers::stream_layer_settings spsc_layer;
spsc_layer.type = "spsc_queue";
options.stream_layers.push_back(std::move(spsc_layer));
```

A named queue pair uses the same context and the generated
`rpc::spsc_queue_stream::stream_settings` envelope:

```cpp
context.set_dependency_value(queues, "control");

rpc::spsc_queue_stream::stream_settings spsc_settings;
spsc_settings.queue_pair = std::string("control");

rpc::stream_layers::stream_layer_settings spsc_layer;
spsc_layer.type = "spsc_queue";
spsc_layer.settings = json::v1::convert::to_json_object(spsc_settings);
```

The lower-level pattern is still available when a caller needs to construct
streams or transports manually: wrap each queue pair in
`streaming::spsc_queue::stream` and hand the resulting stream to
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

`streaming::spsc_queue::stream` does not own the coroutine scheduler. It keeps a
weak scheduler reference and locks it only while awaiting a yield or retry. The
owning service or runtime must keep the scheduler alive while stream transports
are active. This prevents a queue stream that is destroyed by a scheduler worker
from becoming the final scheduler owner and trying to shut down the worker pool
from one of its own threads.

This page should therefore be read as streaming-transport guidance for the
current C++ implementation, not as a statement that every Canopy implementation
has an SPSC-backed IPC stack.
