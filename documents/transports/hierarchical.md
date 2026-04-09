<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Hierarchical Transport Pattern

Scope note:

- this document describes the current C++ hierarchical transport pattern
- the `child_transport` / `parent_transport` lifetime model is a C++ runtime
  pattern, not a cross-language Canopy guarantee
- see [C++ Status](../status/cpp.md), [Rust Status](../status/rust.md), and
  [JavaScript Status](../status/javascript.md) for implementation scope

This document describes the circular dependency architecture and safe
disconnection protocol used by hierarchical transports in the current C++
implementation.

## Applicable Transports

- **Local Transport** (`rpc::local`) - In-process parent/child zones
- **SGX Enclave Transport** - Host/enclave communication
- **Blocking DLL Transport** (`rpc::dynamic_library`) - In-process DLL child zones in blocking builds
- **Coroutine DLL Transport** (`rpc::libcoro_dynamic_library`) - In-process DLL child zones in coroutine builds
- Any transport where a parent zone creates and manages a child zone

`rpc::ipc_transport` is intentionally not part of this document. It is a
process-owning `rpc::stream_transport::transport`, not necessarily a hierarchical
`child_transport` / `parent_transport` pair.

## Architecture: Circular Dependency by Design

Hierarchical transports implement an intentional circular dependency to manage zone lifetime across boundaries.

### Naming Convention (from zone's perspective)

The naming can be confusing because it's from each zone's perspective:

- **child_transport** - Lives in **parent zone**, gateway TO child zone
- **parent_transport** - Lives in **child zone**, gateway TO parent zone

Think of it as: "I am a transport to reach my parent/child"

### Circular Reference Structure

```
Parent Zone (zone 1):
  └─ child_transport
      └─ child_: stdex::member_ptr<parent_transport>  (points to child zone)

Child Zone (zone 2):
  └─ child_service
      └─ parent_transport_: std::shared_ptr<parent_transport>
  └─ parent_transport
      └─ parent_: stdex::member_ptr<child_transport>  (points back to parent zone)
```

### Ownership Chain

1. `rpc::child_service` (child zone) holds `std::shared_ptr<parent_transport>`
2. `parent_transport` (child zone) holds `stdex::member_ptr<child_transport>` (parent zone)
3. `child_transport` (parent zone) holds `stdex::member_ptr<parent_transport>` (child zone)

This creates a circular reference that keeps both zones alive as long as references exist in either direction.

## Stack-Based Lifetime Protection

The critical safety mechanism: when calls cross zone boundaries, stack-based `shared_ptr` protects transport lifetime.

### Call Flow from Parent to Child

```cpp
// In child_transport (parent zone), calling into child zone:
CORO_TASK(int) child_transport::outbound_send(...) {
    auto child = child_.get_nullable();  // Stack-based shared_ptr<parent_transport>
    if (!child) {
        CO_RETURN rpc::error::ZONE_NOT_FOUND();
    }

    // child shared_ptr on stack keeps parent_transport alive during entire call
    CO_RETURN CO_AWAIT child->inbound_send(...);
    // When stack unwinds, parent_transport can safely destruct
}
```

### Why This Matters

Even if `child_service` releases its last reference to `parent_transport` **during** an active call from the parent zone:
- The stack-based `shared_ptr` keeps `parent_transport` alive
- No use-after-free on the call stack
- Transport destructs naturally when the stack unwinds

This is the **key insight** that makes the pattern safe.

## Safe Disconnection Protocol

When a child zone is being destroyed, the circular references must be broken in a coordinated way.

### Disconnection Sequence

1. **Trigger**: `child_service` destructor runs (child zone shutting down)
2. **Set Status**: Calls `parent_transport->set_status(DISCONNECTED)` on its own transport
3. **Propagate**: `parent_transport::set_status()` override propagates disconnect to parent zone
4. **Parent Breaks**: `child_transport::on_child_disconnected()` breaks its `child_` reference
5. **Child Breaks**: `parent_transport` breaks its `parent_` reference
6. **Cleanup**: Circular dependency resolved, both transports can destruct

### Implementation in parent_transport

```cpp
void parent_transport::set_status(rpc::transport_status status) {
    // Call base class to update status
    rpc::transport::set_status(status);

    // If disconnecting, notify parent zone to break circular reference
    if (status == rpc::transport_status::DISCONNECTED) {
        auto parent = parent_.get_nullable();
        if (parent) {
            // Notify parent zone's child_transport to break its child_ reference
            parent->on_child_disconnected();
        }
        // Break our reference to parent
        parent_.reset();
    }
}
```

### Implementation in child_transport

```cpp
void child_transport::on_child_disconnected() {
    // Break circular reference when child zone disconnects
    // Safe because stack-based shared_ptr in outbound_* methods keeps parent_transport alive
    child_.reset();
}
```

## Critical Safety Properties

1. **Zone Boundaries Respected**: `child_service` only touches its own `parent_transport`
2. **Status Propagation**: Disconnect notification crosses zone boundary via override
3. **Stack Protection**: Active calls protected by stack-based `shared_ptr`
4. **Natural Cleanup**: Transport destructs when stack unwinds and refs drop to zero
5. **Thread Safe**: `stdex::member_ptr` uses `shared_mutex` for concurrent access

## Thread Safety with stdex::member_ptr

`stdex::member_ptr` provides thread-safe access to the circular references:

- **`get_nullable()`**: Acquires `shared_lock` (concurrent reads allowed)
- **`reset()`**: Acquires `unique_lock` (exclusive write)

Multiple threads can safely:
- Call `outbound_*` methods (concurrent reads via `get_nullable()`)
- Break references during shutdown (exclusive write via `reset()`)

## Transport-Specific Implementations

Each hierarchical transport implements this pattern:

### Local Transport (rpc::local)
- Direct in-process function calls
- No serialization overhead
- Immediate CONNECTED status
- See `documents/transports/local.md`

### SGX Enclave Transport
- Crosses SGX enclave boundary
- Uses ECALL/OCALL mechanisms
- Serialization required for boundary crossing
- See `documents/transports/sgx.md`

### Blocking DLL Transport (`rpc::dynamic_library`)
- Loads a shared object at runtime via `dlopen` / `LoadLibrary`
- Boundary crossed via C function pointers (`canopy_dll_*` entry points)
- `RTLD_LOCAL` keeps DLL symbols isolated from the host symbol table
- `dlclose` deferred to `on_destination_count_zero` — never called while DLL code is on the stack
- Non-coroutine builds only
- See `documents/transports/dynamic_library.md`

### Coroutine DLL Transport (`rpc::libcoro_dynamic_library`)
- Loads a shared object into the current process in coroutine builds
- Uses coroutine-oriented DLL entry points
- Preserves the same parent/child lifetime pattern as other hierarchical transports
- See `documents/transports/dynamic_library.md`

## Common Patterns

### Creating a Child Zone

```cpp
auto child_transport = std::make_shared<rpc::local::child_transport>(
    "child_name",
    parent_service,
    rpc::zone{new_zone_id});

child_transport->set_child_entry_point<i_example_parent, i_example_child>(
    [](const rpc::shared_ptr<i_example_parent>& parent_interface,
       rpc::shared_ptr<i_example_child>& child_interface,
       const std::shared_ptr<rpc::child_service>& child_service) -> CORO_TASK(int) {
        // Initialize child zone
        child_interface = rpc::make_shared<example_child_impl>(child_service, parent_interface);
        CO_RETURN rpc::error::OK();
    });

rpc::shared_ptr<i_example_parent> parent_ptr;
auto ret = CO_AWAIT parent_service->connect_to_zone<i_example_parent, i_example_child>(
    "child_name", child_transport, parent_ptr);

if (ret.error_code != rpc::error::OK())
{
    CO_RETURN ret.error_code;
}

auto child_ptr = ret.output_interface;
```

### Destroying a Child Zone

The child zone destructs naturally when all references are released:
1. Release all proxies to child zone objects
2. `child_service` destructs (last reference holder)
3. Disconnection protocol runs automatically
4. Circular references broken
5. Transports destruct

## Debugging

### Telemetry Visualization

Enable telemetry to visualize the circular dependency lifecycle:
- Green highlighting shows deletion/destruction events
- Transport ref counts tracked per zone pair
- Status changes visible in timeline

### Common Issues

**Problem**: Transport destructing during active call
- **Cause**: Circular reference broken too early
- **Fix**: Ensure stack-based `shared_ptr` in all `outbound_*` methods

**Problem**: Transport never destructs (leak)
- **Cause**: Circular reference not broken on disconnect
- **Fix**: Verify `set_status()` override and `on_child_disconnected()` called

**Problem**: Negative ref counts in telemetry
- **Cause**: Mismatched add_ref/release (often relay operations)
- **Fix**: Ensure relay operations (options=3) skip ref counting

## Related Issues

- **canopy-gj2**: Implementation of circular dependency fix
- **canopy-w6l**: Telemetry ref counting for relay operations

## See Also

- `c++/rpc/include/rpc/internal/member_ptr.h` - Thread-safe pointer wrapper
- `c++/rpc/include/rpc/internal/transport.h` - Base transport class
- `c++/rpc/include/rpc/internal/service.h` - `rpc::root_service` and `rpc::child_service`
