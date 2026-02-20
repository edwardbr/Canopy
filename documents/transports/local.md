<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Local Transport (rpc::local)

In-process communication between parent and child zones. No network overhead.

## When to Use

- Unit testing
- Microservices in a single process
- High-performance local communication

## Structure

```cpp
namespace rpc::local {
    class parent_transport : public rpc::transport;
    class child_transport : public rpc::transport;
}
```

## Setup

**Parent Zone (server)**:
```cpp
auto root_service = std::make_shared<rpc::service>("root", rpc::zone{1});

// Child will connect to this zone
```

**Child Zone (client)**:
```cpp
uint64_t new_zone_id = 2;

auto child_transport = std::make_shared<rpc::local::child_transport>(
    "child_zone",
    root_service_,
    rpc::zone{new_zone_id});

child_transport->set_child_entry_point<yyy::i_host, yyy::i_example>(
    [&](const rpc::shared_ptr<yyy::i_host>& host,
        rpc::shared_ptr<yyy::i_example>& new_example,
        const std::shared_ptr<rpc::child_service>& child_service_ptr) -> CORO_TASK(int)
    {
        // Initialize child zone
        new_example = rpc::make_shared<example_impl>(child_service_ptr, host);
        CO_RETURN rpc::error::OK();
    });

rpc::shared_ptr<yyy::i_host> host_ptr;
rpc::shared_ptr<yyy::i_example> example_ptr;

auto ret = CO_AWAIT root_service_->connect_to_zone(
    "child_zone", child_transport, host_ptr, example_ptr);
```

## Key Characteristics

- **No handshake**: Status is immediately `CONNECTED`
- **Direct function calls**: Messages pass through inbound handlers
- **Hierarchical transport**: Implements parent/child zone pattern
- **Bidirectional**: parent_transport and child_transport reference each other
- **Zero serialization**: In-process function calls, no marshalling overhead

## Hierarchical Transport Pattern

Local transport implements the standard hierarchical transport pattern used by all parent/child zone transports (local, SGX, DLL).

### Key Features:
- **Circular dependency by design**: Keeps zones alive while references exist
- **Stack-based lifetime protection**: Prevents use-after-free during active calls
- **Safe disconnection protocol**: Coordinated cleanup across zone boundaries
- **Thread-safe with `stdex::member_ptr`**: Concurrent access protected by `shared_mutex`

### Quick Summary:
- `child_transport` lives in parent zone, calls into child via `child->inbound_*()`
- `parent_transport` lives in child zone, calls into parent via `parent->inbound_*()`
- Stack-based `shared_ptr` from `get_nullable()` protects transport during call
- Disconnect propagates via `set_status()` override, breaking circular refs safely

**See `documents/transports/hierarchical.md` for complete architecture details, safety properties, and implementation guide.**

## Local-Specific Details

**Performance:**
- Direct C++ function calls
- No serialization or deserialization
- No context switching overhead
- Ideal for unit tests and high-performance in-process zones

**Limitations:**
- Single process only
- No isolation between zones
- Shared memory space

