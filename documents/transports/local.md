<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Local Transport (rpc::local)

Scope note:

- this document describes the current C++ local transport
- the `rpc::local::child_transport` / `parent_transport` model is C++-specific
- see [C++ Status](../status/cpp.md), [Rust Status](../status/rust.md), and
  [JavaScript Status](../status/javascript.md) for implementation scope

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
auto root_service = std::make_shared<rpc::root_service>("root", rpc::zone{1});

// Child will connect to this zone
```

**Child Zone (client)**:
```cpp
auto child_transport = std::make_shared<rpc::local::child_transport>(
    "child_zone",
    root_service);

child_transport->set_child_entry_point<yyy::i_host, yyy::i_example>(
    [&](rpc::shared_ptr<yyy::i_host> host,
        std::shared_ptr<rpc::child_service> child_service_ptr)
        -> CORO_TASK(rpc::service_connect_result<yyy::i_example>)
    {
        // Initialize child zone
        auto new_example = rpc::make_shared<example_impl>(child_service_ptr, host);
        CO_RETURN rpc::service_connect_result<yyy::i_example>{
            rpc::error::OK(),
            std::move(new_example)};
    });

rpc::shared_ptr<yyy::i_host> host_ptr;
auto ret = CO_AWAIT root_service->connect_to_zone<yyy::i_host, yyy::i_example>(
    "child_zone", child_transport, host_ptr);

if (ret.error_code != rpc::error::OK())
{
    CO_RETURN ret.error_code;
}

auto example_ptr = ret.output_interface;
```

## Key Characteristics

- **No handshake**: Status is immediately `CONNECTED`
- **Direct function calls**: Messages pass through inbound handlers
- **Hierarchical transport**: Implements parent/child zone pattern
- **Bidirectional**: parent_transport and child_transport reference each other
- **No stream transport layer**: Calls stay within the in-process hierarchical
  transport pair

Local transport should be read as the simplest concrete C++ implementation of
the hierarchical transport pattern, not as the definition of Canopy transport
behaviour in every implementation.

## Hierarchical Transport Pattern

Local transport implements the standard hierarchical transport pattern used by
the current `child_transport` / `parent_transport` families: local, SGX, and
the in-process DLL transports.

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
