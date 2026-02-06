<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Canopy Comprehensive Demos

This directory contains comprehensive demonstrations of all major Canopy features.

## Directory Structure

```
comprehensive/
├── CMakeLists.txt              # Build configuration
├── README.md                   # This file
├── idl/
│   └── demo.idl               # IDL definitions for all demos
├── include/
│   └── demo_impl.h            # Implementation of demo interfaces
└── src/
    ├── transport/
    │   ├── local_transport_demo.cpp   # Local (in-process) transport
    │   ├── tcp_transport_demo.cpp     # TCP network transport
    │   ├── spsc_transport_demo.cpp    # SPSC queue-based IPC
    │   └── benchmark.cpp              # Transport/encoding/blob benchmark
    ├── serialisation/
    │   └── serialisation_demo.cpp     # All serialization formats
    └── pointers/
        ├── shared_ptr_demo.cpp       # rpc::shared_ptr lifecycle
        └── optimistic_ptr_demo.cpp   # rpc::optimistic_ptr pattern
```

## Quick Start

### Prerequisites

```bash
# Configure with coroutines (required for TCP and SPSC transports)
cmake --preset Coroutine_Debug

# Build the core library and generator
cmake --build build --target rpc
cmake --build build --target rpc_generator

# Generate IDL code (after creating the demo.idl generation target)
cmake --build build --target demo_idl
```

### Building the Demos

1. **Uncomment the demo executables** in `CMakeLists.txt`
2. **Build**:
```bash
cmake --build build --target comprehensive_demo
```

3. **Run**:
```bash
./build/output/debug/demos/comprehensive/comprehensive_demo
```

## Demo Categories

### 1. Transport Demos

#### Local Transport (`local_transport_demo.cpp`)

**Concept**: In-process communication between zones using local transport.

**Features Demonstrated**:
- Zone creation (root and child zones)
- Local transport setup
- Cross-zone RPC calls
- Service proxies

**Requirements**: None (works without coroutines)

**Key Code Pattern**:
```cpp
auto child_transport = std::make_shared<rpc::local::child_transport>(
    "child_zone",
    parent_service,
    child_zone_id);
child_transport->set_child_entry_point<InterfaceType, InterfaceType>(
    child_zone_id,
    setup_callback);
```

---

#### TCP Transport (`tcp_transport_demo.cpp`)

**Concept**: Network communication between different machines/processes.

**Features Demonstrated**:
- TCP server with listener
- TCP client connection
- Async I/O with coroutines
- Network address handling

**Requirements**: `CANOPY_BUILD_COROUTINE=ON` (uses libcoro)

**MISCONCEPTION REPORT**:
- TCP transport requires coroutines for async I/O
- Without coroutines, you need a synchronous TCP wrapper
- The rpc::tcp::listener and tcp_transport classes are coroutine-based

---

#### SPSC Transport (`spsc_transport_demo.cpp`)

**Concept**: Single-Producer Single-Consumer queue-based IPC for same-machine, different-process communication.

**Features Demonstrated**:
- Lock-free ring buffer queues
- Message pump coroutines
- Sequence number correlation
- Producer/consumer pattern

**Requirements**: `CANOPY_BUILD_COROUTINE=ON` (uses coroutine message pump)

**MISCONCEPTION REPORT**:
- SPSC is for different processes, not same-process (use local transport)
- Single-producer/single-consumer constraint is enforced
- Message pump requires coroutines even for same-process use

---

#### Benchmark (`benchmark.cpp`)

**Concept**: Performance matrix for transfer between two zones across multiple transports, encodings, and blob sizes.

**Features Demonstrated**:
- Local, SPSC, and TCP transports
- Per-service default encoding via `service::set_default_encoding`
- Middle 80% statistics over 1000 RPC calls

**Requirements**: `CANOPY_BUILD_COROUTINE=ON` (uses coroutine-based transports)

---

### 2. Serialization Demo (`serialisation_demo.cpp`)

**Concept**: All supported serialization formats and data types.

**Features Demonstrated**:
- `yas_binary`: High-performance binary
- `yas_compressed_binary`: Compressed binary
- `yas_json`: Human-readable JSON
- `protocol_buffers`: Google's protobuf
- Complex types: vectors, maps, structs
- Large binary data (1MB)

**Requirements**: None

**Key Code Pattern**:
```cpp
// Format is negotiated automatically
auto error = CO_AWAIT proxy->process_data(input, output);

// Direct serialization API
auto serialized = rpc::serialise<data_type, rpc::encoding::yas_binary>(obj);
auto error = rpc::deserialise<rpc::encoding::yas_binary>(serialized, restored);
```

---

### 3. Pointer Demos

#### Shared Pointer (`shared_ptr_demo.cpp`)

**Concept**: `rpc::shared_ptr` lifecycle and reference counting.

**Features Demonstrated**:
- Object creation with `rpc::make_shared<T>()`
- Reference counting behavior
- Copy semantics (increases refcount)
- Reset semantics (decreases refcount)
- Cross-zone object passing
- Error handling (OBJECT_NOT_FOUND)

**Requirements**: None

**Key Code Pattern**:
```cpp
auto obj = rpc::make_shared<my_impl>();
auto copy = obj;  // refcount increases
obj.reset();      // refcount decreases, object may be deleted
```

**MISCONCEPTION REPORT**:
- `rpc::shared_ptr` IS NOT `std::shared_ptr` - different control blocks
- Never mix pointer types 
- Keeps transport chain alive, not just the object
- OBJECT_NOT_FOUND is serious (reference held but object destroyed)

---

#### Optimistic Pointer (`optimistic_ptr_demo.cpp`)

**Concept**: `rpc::optimistic_ptr` for non-RAII references.

**Features Demonstrated**:
- Creating optimistic references with `rpc::make_optimistic()`
- Callback pattern (parent/child without circular dependency)
- Database connection pattern (independent lifetime)
- Error semantics (OBJECT_GONE vs OBJECT_NOT_FOUND)

**Requirements**: None

**Key Use Case - Callback Pattern**:
```cpp
// Zone 1: Parent (creates child, needs callbacks)
auto child = rpc::make_shared<worker_impl>();
CO_AWAIT child->set_callback_receiver(
    rpc::optimistic_ptr<callback_receiver>(parent));

// Zone 2: Child (calls parent via optimistic_ptr)
CO_AWAIT parent_callback_->on_progress(progress);
// Returns OBJECT_GONE if parent is gone (EXPECTED!)
```

**MISCONCEPTION REPORT**:
- NOT for fire-and-forget (both pointer types support this)
- Key difference is ERROR SEMANTICS:
  - shared_ptr + gone = OBJECT_NOT_FOUND (serious)
  - optimistic_ptr + gone = OBJECT_GONE (expected!)
- Does NOT keep object alive
- Use for: database connections, singletons, callbacks

---

## Error Code Reference

| Code | Meaning | When Returned |
|------|---------|---------------|
| `OK` | Success | All successful operations |
| `OBJECT_NOT_FOUND` | Serious error | shared_ptr reference held, object destroyed |
| `OBJECT_GONE` | Expected | optimistic_ptr to independent-lifetime object |
| `INVALID_DATA` | Bad input | Division by zero, null pointers, etc. |
| `TRANSPORT_ERROR` | Connection failed | Network issues, timeouts |

## Common Patterns

### Zone Setup
```cpp
auto service = std::make_shared<rpc::service>("name", rpc::zone{id}, scheduler);
```

### Interface Implementation
```cpp
class my_impl : public i_my_interface
{
    void* get_address() const override { return const_cast<my_impl*>(this); }
    const rpc::casting_interface* query_interface(rpc::interface_ordinal id) const override
    {
        return match<i_my_interface>(id) ? this : nullptr;
    }
    CORO_TASK(error_code) my_method(...) override { ... }
};
```

### Coroutine Pattern
```cpp
CORO_TASK(error_code) my_operation()
{
    auto result = CO_AWAIT other_service->call(...);
    if (result != error::OK()) CO_RETURN result;
    CO_RETURN error::OK();
}
```

## Troubleshooting

### Build Errors
- **"demo.h not found"**: Generate IDL code first with `cmake --build build --target demo_idl`
- **"TCP/SPSC requires coroutines"**: Use `Coroutine_Debug` preset

### Runtime Errors
- **OBJECT_NOT_FOUND**: Check if object was released while reference held
- **OBJECT_GONE**: Expected for optimistic_ptr - object has independent lifetime
- **Connection failed**: Check network settings, ports, firewall

## Next Steps

1. Review the code in each demo
2. Modify parameters to experiment
3. Combine patterns (e.g., optimistic_ptr over TCP transport)
4. Integrate into your own application

## Additional Resources

- [Canopy Documentation](../../docs_new2/)
- [API Reference](../../docs_new2/08-api-reference.md)
- [Best Practices](../../docs_new2/10-best-practices.md)
- [Examples](../../docs_new2/09-examples.md)
