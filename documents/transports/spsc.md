<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# SPSC Transport (rpc::spsc)

Single-Producer Single-Consumer queue-based communication for lock-free IPC.

## When to Use

- High-performance inter-process communication
- Same-machine, different processes
- Lock-free architecture requirements

## Requirements

- `CANOPY_BUILD_COROUTINE=ON` (requires libcoro)
- Host-only (no enclave version)

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

## Setup

**Server Side (receiver)**:
```cpp
spsc::queue_type receive_queue;
spsc::queue_type send_queue;

auto server_transport = rpc::spsc::spsc_transport::create(
    "server",
    peer_service_,
    rpc::zone{peer_zone_id},
    &receive_queue,     // Queue to receive on
    &send_queue,        // Queue to send on (reversed)
    handler_lambda);    // Inbound message handler
```

**Client Side (initiator)**:
```cpp
spsc::queue_type send_queue;
spsc::queue_type receive_queue;

auto client_transport = rpc::spsc::spsc_transport::create(
    "client",
    root_service_,
    rpc::zone{peer_zone_id},
    &send_queue,       // Queue to send on
    &receive_queue,    // Queue to receive on (reversed)
    nullptr);          // Client doesn't need handler
```

## Message Protocol

Messages include metadata and payload:

```idl
struct envelope_prefix
{
    uint64_t version;
    message_direction direction;      // send, receive, one_way
    uint64_t sequence_number;
    uint64_t payload_size;
};

struct envelope_payload
{
    uint64_t payload_fingerprint;
    std::vector<uint8_t> payload;
};
```

**Message Types**:
- `init_client_channel_send` / `init_client_channel_response` - Connection handshake
- `call_send` / `call_receive` - Request-response RPC calls
- `post_send` - Fire-and-forget notifications
- `try_cast_send` / `try_cast_receive` - Interface queries
- `addref_send` / `addref_receive` - Reference count increments
- `release_send` / `release_receive` - Reference count decrements
- `object_released_send` - Object lifecycle notification
- `transport_down_send` - Transport disconnection notification
- `close_connection_send` / `close_connection_ack` - Graceful shutdown
