<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Proxies and Stubs

Scope note:

- this document explains shared proxy/stub semantics through the current C++
  implementation
- concrete class members, control-block details, and code examples should be
  read as C++-specific unless stated otherwise

Proxies and stubs are the client/server marshalling machinery that enables transparent remote procedure calls across zone boundaries. They form the foundation of Canopy's distributed object system. `rpc::shared_ptr`, `rpc::weak_ptr`, and `rpc::optimistic_ptr` may also refer to local objects, in which case no wire marshalling occurs.

## Conceptual Overview

When you call a method on a remote object, you're actually calling through a **proxy** that marshals the call across the transport. On the remote side, a **stub** receives the call, unmarshals the parameters, and dispatches to the actual implementation.

```
Client Zone                        Server Zone
┌──────────────────┐              ┌──────────────────┐
│  Your Code       │              │  Implementation  │
│      │           │              │        ▲         │
│      ▼           │              │        │         │
│  Interface Proxy │─────RPC─────▶│  Interface Stub  │
│  (i_calc_proxy)  │              │  (i_calc_stub)   │
│      │           │              │        ▲         │
│      ▼           │              │        │         │
│  Object Proxy    │              │   Object Stub    │
│      │           │              │        ▲         │
│      ▼           │              │        │         │
│  Service Proxy   │              │     Service      │
│      │           │              │        ▲         │
│      ▼           │              │        │         │
│   Transport      │──────────────┤    Transport     │
└──────────────────┘              └──────────────────┘
```

**Two Levels of Abstraction**:

1. **Object-level** (generic RPC machinery):
   - `object_proxy` - Represents any remote object locally
   - `object_stub` - Represents any local object for remote callers
   - `service_proxy` - Represents a remote zone's service

2. **Interface-level** (machine-generated from IDL):
   - `i_calculator_proxy` - Type-safe client-side proxy for `i_calculator`
     - Serializes parameters in configured format (YAS binary, JSON, Protocol Buffers)
     - Deserializes results
   - `i_calculator_stub` - Type-safe server-side stub for `i_calculator`
     - Deserializes parameters in configured format
     - Serializes results
   - Generated automatically from `.idl` files by the IDL code generator

## Object-Level: Generic RPC Machinery

### object_proxy

Represents a remote object in the local zone. When you hold an
`rpc::shared_ptr<i_interface>` or `rpc::optimistic_ptr<i_interface>` to a
remote object, it is backed by an `object_proxy`.

**Key Members**:
```cpp
class object_proxy : public std::enable_shared_from_this<rpc::object_proxy>
{
    rpc::object object_id_;                          // Remote object ID
    stdex::member_ptr<service_proxy> service_proxy_; // Remote service
    std::unordered_map<interface_ordinal,
        rpc::weak_ptr<casting_interface>> proxy_map; // Cached local weak refs to interface proxies
    std::atomic<int> shared_count_{0};               // Shared references
    std::atomic<int> optimistic_count_{0};           // Optimistic references
};
```

**Responsibilities**:
1. **Object Identity**: Tracks the remote object's ID and zone
2. **Reference Counting**: Manages shared and optimistic reference counts
3. **Interface Caching**: Caches interface-specific proxies (e.g., `i_calculator_proxy`)
4. **RPC Send**: Routes method calls through transport

**Creation**:
```cpp
// Created by service_proxy when marshalling remote reference, not by the api user
auto proxy = std::make_shared<object_proxy>(
    remote_object_id,
    service_proxy_to_remote_zone);
```

**Lifecycle**:
- Created when first reference to remote object arrives
- Destroyed when the last distributed reference path is released
- Destructor sends final `release()` to remote zone

### object_stub

Represents a local object for remote callers. When a remote zone holds an `rpc::shared_ptr<i_interface>` to your local object, it's backed by an `object_stub` in your zone.

**Responsibilities**:
1. **Object Registration**: Registered in service's stub registry
2. **Reference Tracking**: Tracks references per zone (for transport cleanup)
3. **Interface Dispatch**: Holds interface-specific stubs (e.g., `i_calculator_stub`)
4. **RPC Receive**: Receives remote calls and dispatches to implementation

**Lifecycle**:
- Created when first remote reference to local object is established
- Holds strong reference to service (keeps zone alive)
- Destroyed when all remote references are released

### service_proxy

Represents a remote zone's service. Acts as the gateway to all objects in that
remote zone.

**Key Members**:
```cpp
class service_proxy
{
    rpc::zone zone_id_;                      // Local zone
    rpc::destination_zone dest_zone_id_;     // Remote zone (zone-only address)
    std::shared_ptr<service> service_;       // Local service runtime
    stdex::member_ptr<transport> transport_; // Transport to remote zone

};
```

**Responsibilities**:
1. **Zone Bridging**: Connects local zone to remote zone
2. **Object Proxy Management**: Creates and caches `object_proxy` instances
3. **Transport Access**: Routes calls through transport
4. **Reference Propagation**: Forwards add_ref/release and related marshaller
   traffic to the remote zone

**Creation**:
```cpp
// Created by service when connecting to remote zone
auto service_proxy = std::make_shared<rpc::service_proxy>(
    local_zone_id,
    remote_zone_id,
    local_service,
    transport_to_remote);
```

## Interface-Level: Generated Code

### Interface Proxy (e.g., i_calculator_proxy)

Type-safe client-side proxy generated from IDL interface definition. Responsible for:
- **Parameter serialization** - Converts C++ types to wire format
- **RPC invocation** - Routes call through object_proxy
- **Result deserialization** - Converts wire format back to C++ types

**Example IDL**:
```cpp
namespace comprehensive
{
    interface i_calculator
    {
        [description="Add two integers"]
        int add(int a, int b, [out] int& sum);

        [description="Subtract two integers"]
        int subtract(int a, int b, [out] int& difference);
    };
}
```

**Generated Proxy**:
```cpp
class i_calculator_proxy : public comprehensive::i_calculator
```

**Key Characteristics**:
- Inherits from IDL interface
- Holds `std::shared_ptr<object_proxy>` in the C++ runtime
- Each method marshals parameters, sends RPC, unmarshals result
- Returns same error codes as defined in IDL

### Parameter Shape vs Marshalling Shape

The generated interface and proxy methods preserve the parameter spelling from the IDL. If an IDL method uses `T&&`, the generated interface-level method also uses `T&&`.

That does not require every internal generated helper to use the same parameter form. In particular, marshalling helpers such as generated serialiser functions may accept a non-consuming view of the same input so they can serialise for retry, version fallback, or encoding fallback without treating the source object as consumed.

The intended split is:
- IDL-facing generated interface: reflects the IDL exactly
- Proxy/stub internals: may use transport-friendly parameter forms
- Stub invocation: reconstructs temporaries and calls the target using the IDL-declared shape

This keeps the public contract faithful to the IDL while allowing the transport layer to avoid fake or misleading ownership transfer during serialisation.


## Serialization Formats

Interface-level proxies and stubs handle parameter and result serialization/deserialization. The serialization format is **configurable via IDL generator options**.

### Supported Formats

```cpp
enum class encoding
{
    enc_default,            // Default encoding (typically yas_binary)
    yas_binary,            // Binary serialization (high performance)
    yas_json,              // JSON serialization (human-readable, debugging)
    yas_compressed_binary, // Compressed binary
    protocol_buffers       // Protocol Buffers format
};
```

### Format Selection

The encoding format is specified when calling the IDL generator:

```cmake
CanopyGenerate(
    calculator                                    # Target name
    ${CMAKE_CURRENT_SOURCE_DIR}/idl/calculator.idl # IDL file
    ${CMAKE_CURRENT_SOURCE_DIR}                   # Source dir
    ${CMAKE_BINARY_DIR}/generated                 # Output dir
    ""                                            # Namespace
    yas_binary                                    # encodings
    yas_json                                      
    protocol_buffers

    dependencies rpc_types_idl
    include_paths ${CMAKE_CURRENT_SOURCE_DIR}/idl
)
```

### Format Characteristics

| Format | Performance | Human-Readable | Use Case |
|--------|-------------|----------------|----------|
| `yas_binary` | Highest | No | Production (default) |
| `yas_compressed_binary` | High (smaller size) | No | Bandwidth-constrained |
| `yas_json` | Lower | Yes | Debugging, interop, fallback |
| `protocol_buffers` | High | No | Cross-language compatibility |

### Format Negotiation

- **Version mismatch**: Falls back to `yas_json` (universal format)
- **Invalid encoding**: Automatically downgrades to `yas_json`
- **Cross-version compatibility**: JSON ensures compatibility across versions

### Multiple Format Support

Interfaces can be generated with multiple formats for different use cases:

```cmake
# Generate with both binary (production) and JSON (debugging)
CanopyGenerate(
    my_service
    idl/my_service.idl
    ...
    yas_binary      # Primary: fast production format
    yas_json        # Secondary: debugging/fallback
)
```

The transport layer automatically negotiates the best available format between client and server.

## RPC Call Flow

### Complete End-to-End Flow

```
1. Client Code Calls Method
   │
   ▼
2. Interface Proxy (i_calculator_proxy)
   - Serializes parameters (a=5, b=3)
   - Calls object_proxy->send()
   │
   ▼
3. Object Proxy
   - Routes through service_proxy
   - Adds object_id, interface_id, method_id
   │
   ▼
4. Service Proxy
   - Routes through transport
   - Adds caller_zone, destination_zone
   │
   ▼
5. Transport (TCP/SPSC/etc)
   - Sends serialized message across zone boundary
   │
   ▼
6. Transport (Server Side)
   - Receives message
   - Routes to service->deliver()
   │
   ▼
7. Service
   - Looks up object_stub by object_id
   - Calls stub->call()
   │
   ▼
8. Object Stub
   - Looks up interface_stub by interface_id
   - Calls stub->call()
   │
   ▼
9. Interface Stub (i_calculator_stub)
   - Deserializes parameters (a=5, b=3)
   - Dispatches to implementation based on method_id
   │
   ▼
10. Implementation (calculator_impl)
    - Executes: sum = 5 + 3 = 8
    - Returns error::OK()
    │
    ▼
11. Interface Stub
    - Serializes result (sum=8)
    - Returns to object_stub
    │
    ▼
12. Service, Transport, Service Proxy, Object Proxy
    - Route response back to client zone
    │
    ▼
13. Interface Proxy
    - Deserializes result (sum=8)
    - Returns to client code
    │
    ▼
14. Client Code
    - Receives: error=OK, sum=8
```

### Reference Acquisition Flow

When passing an `rpc::shared_ptr` across zones:

```
Zone 1: Client Code
   rpc::shared_ptr<i_calc> calc = ...;  // Local object
   CO_AWAIT remote_service->use_calculator(calc);
   │
   ▼
Zone 1: Proxy Marshalling
   - Checks if calc is local or remote
   - If local: Creates object_stub if not exists
   - Sends add_ref(object_id, zone_id) to Zone 2
   │
   ▼
Zone 2: Stub Unmarshalling
   - Receives add_ref
   - Creates object_proxy for (object_id, zone_1)
   - Increments shared_count_
   - Returns proxy to application
   │
   ▼
Zone 2: Application Code
   rpc::shared_ptr<i_calc> calc;  // Now points to Zone 1's object
   CO_AWAIT calc->add(5, 3, sum);  // Calls back to Zone 1
```

## Code Generation Process

### IDL to C++ Pipeline

```
1. Write .idl file
   └─ comprehensive.idl

2. CMake generates target
   └─ comprehensive_idl

3. generator parses IDL
   └─ Uses idlparser library
   └─ Extracts interfaces, methods, attributes

4. Generate C++ headers
   ├─ comprehensive.h           (Interface definitions)
   ├─ comprehensive_proxy.h     (Proxy implementations)
   ├─ comprehensive_stub.h      (Stub implementations)
   └─ comprehensive_adaptor.h   (Helper code)

5. Compile into project
   └─ Headers included in both client and server zones
```

### Generated Code Structure

For each interface in the IDL:

**Interface Definition** (`comprehensive.h`):
```cpp
namespace comprehensive
{
    template<typename Derived>
    class interface : public rpc::casting_interface { ... };

    class calculator : public rpc::base<calculator, i_calculator>
    {
    public:
        static constexpr uint64_t get_id(uint64_t rpc_version);

        virtual CORO_TASK(int) add(int a, int b, int& sum) = 0;
        virtual CORO_TASK(int) subtract(int a, int b, int& difference) = 0;
    };
}
```


### Interface Versioning

Each interface has a SHA3-based fingerprint based on the namespace name and shape of the interface:

```cpp
static constexpr uint64_t get_id(uint64_t rpc_version)
{
    // Based on:
    // - Interface name
    // - Method signatures
    // - Parameter types
    // Hash changes if interface definition changes

    return 0x1234567890ABCDEF;  // Example hash
}
```



## Working Together: Proxies, Stubs, and Memory Management

### Object Lifetime Through Proxies and Stubs

```
Zone 1 (Client)                    Zone 2 (Server)
┌──────────────────┐              ┌──────────────────┐
│ rpc::shared_ptr  │              │                  │
│  <i_calculator>  │              │                  │
│        │         │              │                  │
│        ▼         │              │                  │
│ i_calculator_    │              │                  │
│     proxy        │              │                  │
│        │         │              │                  │
│        ▼         │              │                  │
│ object_proxy     │──add_ref()─▶│  object_stub     │
│  shared_count=1  │              │  shared_count=1  │
│        │         │              │        │         │
│        │         │              │        ▼         │
│        │         │              │ i_calculator_    │
│        │         │              │     stub         │
│        │         │              │        │         │
│        │         │              │        ▼         │
│        │         │              │ calculator_impl  │
│        │         │──RPC calls─▶│  (your code)     │
└──────────────────┘              └──────────────────┘

When Zone 1's shared_ptr is destroyed:
1. object_proxy->release() sends release() to Zone 2
2. object_stub->shared_count_ decrements
3. If shared_count == 0: object_stub destroyed
4. calculator_impl refcount decrements
5. If impl refcount == 0: implementation destroyed
```

See [Memory Management](04-memory-management.md) for the complete reference counting system.

### Transport Lifetime and Proxies

Proxies hold the transport chain alive through service_proxy:

```cpp
class service_proxy
{
    stdex::member_ptr<transport> transport_;  // Strong reference via member_ptr
};

class object_proxy
{
    stdex::member_ptr<service_proxy> service_proxy_;  // Strong reference
};
```

**Stack-Based Protection During Calls**:
```cpp
CORO_TASK(int) object_proxy::send(...)
{
    auto svc_proxy = service_proxy_.get_nullable();  // Stack shared_ptr
    if (!svc_proxy)
        CO_RETURN rpc::error::ZONE_NOT_FOUND();

    // svc_proxy on stack keeps service_proxy alive
    // service_proxy keeps transport alive
    // Safe during entire async operation
    CO_RETURN CO_AWAIT svc_proxy->send(...);
}
```

Even if another thread releases the last external reference to the service_proxy during the call, the stack-based `shared_ptr` keeps it alive until the call completes.

See [Transports and Passthroughs](06-transports-and-passthroughs.md) for transport architecture.

## Best Practices

### Do

- Let code generation handle proxy/stub creation
- Use type-safe interface methods (not raw object_proxy->send())
- Hold `rpc::shared_ptr<Interface>` for ownership
- Use `rpc::optimistic_ptr<Interface>` to break cycles
- Let reference counting manage cleanup
- Monitor proxy/stub counts in telemetry

### Don't

- Don't manually create proxies or stubs
- Don't bypass interface-level proxies
- Don't store raw pointers to proxies/stubs
- Don't assume synchronous destruction
- Don't hold references longer than needed

## Debugging Proxies and Stubs

### Telemetry Events

```javascript
// Proxy lifecycle
object_proxy_created       // New remote reference
object_proxy_add_ref       // Reference count increased
object_proxy_release       // Reference count decreased
object_proxy_destroyed     // Proxy destroyed

// Stub lifecycle
object_stub_created        // Object exposed remotely
object_stub_add_ref        // Remote reference added
object_stub_release        // Remote reference released
object_stub_destroyed      // Stub destroyed

// Interface queries
interface_query_remote     // Remote interface check
interface_cache_hit        // Cached interface proxy
interface_cache_miss       // New interface proxy created
```

### Common Issues

**Problem**: Proxy destroyed during active call
- **Cause**: Last reference released before async operation completed
- **Fix**: Hold reference during operation, check for stack-based protection

**Problem**: Stub refcount doesn't reach zero (leak)
- **Cause**: Remote zone forgot to release reference
- **Fix**: Check for balanced add_ref/release, review proxy lifetimes

**Problem**: INTERFACE_NOT_SUPPORTED error
- **Cause**: Server doesn't implement interface version
- **Fix**: Rebuild both zones with same IDL, check interface fingerprint

**Problem**: METHOD_NOT_FOUND error
- **Cause**: Method ID mismatch between proxy and stub
- **Fix**: Regenerate code from IDL, ensure both zones use same version

## Code References

**Object-Level Machinery**:
- `c++/rpc/include/rpc/internal/object_proxy.h` - Object proxy class
- `c++/rpc/include/rpc/internal/stub.h` - Object stub and interface stub classes
- `c++/rpc/include/rpc/internal/service_proxy.h` - Service proxy class
- `c++/rpc/src/object_proxy.cpp` - Object proxy implementation
- `c++/rpc/src/object_stub.cpp` - Object stub implementation
- `c++/rpc/src/service_proxy.cpp` - Service proxy implementation

**Code Generation**:
- `generator/src/synchronous_generator.cpp` - IDL to C++ generator
- `submodules/idlparser/` - IDL parsing library
- `cmake/CanopyGenerate.cmake` - Code generation CMake macros

**Examples**:
- `c++/demos/comprehensive/idl/comprehensive/comprehensive.idl` - Example IDL
- `c++/demos/comprehensive/` - Complete working demo

## Next Steps

- [Zones](02-zones.md) - Execution contexts and boundaries
- [Services](03-services.md) - Object lifecycle management
- [Memory Management](04-memory-management.md) - Smart pointer reference counting
- [Transports and Passthroughs](06-transports-and-passthroughs.md) - Communication infrastructure
