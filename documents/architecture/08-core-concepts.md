<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Core Concepts

Scope note:

- this document explains shared Canopy concepts through the current C++
  implementation
- code examples, class names, coroutine behavior, and transport/runtime details
  should be read as C++-specific unless stated otherwise

This section covers the fundamental building blocks of Canopy: zones, services, smart pointers, and the proxy/stub architecture.

## 1. Zones

A zone is an execution context that represents a boundary between different parts of a distributed system. Each zone has its own:

- **Subnet**: A unique subnet component that identifies the zone within its
  routing prefix
- **Object Namespace**: Objects are uniquely identified within that zone by
  their local/object address component
- **Service**: Manages object lifecycle and references, typically via
  `rpc::root_service` or `rpc::child_service`
- **Transport Connections**: Links to other zones

### Zone Structure

```cpp
// From rpc_types.idl
// zone_address is versioned and stores routing prefix, subnet, and object id
// according to the capability header recorded in the blob representation

struct zone_address
{
private:
    uint64_t routing_prefix_ = 0;  // conceptual routing prefix
    uint32_t subnet_id_ = 0;       // conceptual subnet component
    uint32_t object_id_ = 0;       // conceptual local/object component, 0 for the service object

public:
    // Accessors
    uint64_t get_routing_prefix() const { return routing_prefix_; }
    void set_routing_prefix(uint64_t val) { routing_prefix_ = val; }
    uint64_t get_subnet() const { return subnet_id_; }
    void set_subnet(uint64_t val) { subnet_id_ = val; }
    uint64_t get_object_id() const { return object_id_; }
    void set_object_id(uint64_t val) { object_id_ = val; }

    // Comparison
    bool same_zone(const zone_address& other) const;  // Compare zone portion only
    constexpr bool is_set() const noexcept;

    // Conversion
    std::string to_string() const;  // CIDR-like notation

    // Actual on-wire/in-memory layout is defined by rpc_types.idl and the
    // versioned capability blob representation
};

struct zone
{
    zone_address addr;

    zone() = default;
    explicit zone(uint64_t initial_id) : addr{0, static_cast<uint32_t>(initial_id), 0} {}  // Legacy compat

    const zone_address& get_address() const { return addr; }

    // Conversion helpers
    destination_zone as_destination() const;
    caller_zone as_caller() const;
    requesting_zone as_requesting_zone() const;
};
```

### Zone Hierarchy

Zones can form parent/child relationships:

```
Zone 1 (Root)
├── Zone 2 (Child)
│   ├── Zone 4 (Grandchild)
│   └── Zone 5 (Grandchild)
└── Zone 3 (Child)
```

**Key Rules**:
- Children hold strong references to parents
- Parent is guaranteed to outlive children
- Zone IDs must be unique across the entire system

### Creating a Root Zone

```cpp
#include <canopy/network_config/network_args.h>

// ... in main()
args::ArgumentParser parser("My App");
canopy::network_config::add_network_args(parser);
parser.ParseCLI(argc, argv);

auto cfg = canopy::network_config::get_network_config(parser);
auto allocator = canopy::network_config::make_allocator(cfg);

// Default behavior: If no --routing-prefix is provided, auto-detects best interface
auto root_service = rpc::root_service::create(
    "root_service",
    allocator.allocate_zone(),
#ifdef CANOPY_BUILD_COROUTINE
    scheduler   // Optional coroutine scheduler
#endif
);
```

**Legacy compatibility**: `rpc::zone{42}` still works in local-only mode
(`routing_prefix=0`).

The routing prefix and subnet together identify the zone. The local/object
address identifies a specific object inside that zone. Local address `0` is
reserved for the zone's service object.

The exact field widths are part of the address capabilities in
`interfaces/rpc/rpc_types.idl`. The current default values are 64-bit subnet
and 64-bit object-id fields, but the documentation should treat that as a
default, not a fixed universal layout.

### Creating Child Zones

Child zones are created through transport connections:

```cpp
// From parent zone
auto child_transport = std::make_shared<rpc::local::child_transport>(
    "child_zone",
    root_service_,
    new_zone_id,
    rpc::local::parent_transport::bind<yyy::i_host, yyy::i_example>(
        new_zone_id,
        [&](const rpc::shared_ptr<yyy::i_host>& host,
            rpc::shared_ptr<yyy::i_example>& new_example,
            const std::shared_ptr<rpc::child_service>& child_service_ptr) -> CORO_TASK(int)
        {
            // Initialize child zone
            new_example = rpc::make_shared<example_impl>(child_service_ptr, host);
            CO_RETURN rpc::error::OK();
        }));

auto ret = CO_AWAIT root_service_->connect_to_zone<yyy::i_host, yyy::i_example>(
    "child_zone", child_transport, host_ptr);
```

### Zone Types for Routing

Canopy uses specialized zone types for different routing scenarios, all wrapping `zone_address`:

```cpp
struct zone               // General zone identity (routing_prefix + subnet, object_id=0)
struct remote_object      // Routing target with object identity (includes object_id)
struct destination_zone   // Zone-only routing (object_id=0)
struct caller_zone        // Call origin (object_id=0)
struct requesting_zone    // Known direction (object_id=0)
```

**Key distinction**: `remote_object` carries the full `zone_address` including
the local/object address, while `destination_zone` is zone-only. `i_marshaller`
methods use `remote_object` for object identity.

```cpp
// Access the full address
const zone_address& addr = remote_object.get_address();

// Access components
uint64_t prefix = addr.get_routing_prefix();
uint64_t subnet = addr.get_subnet();
uint64_t obj_id = addr.get_object_id();

// Compare zone portion only (ignoring local/object address)
if (addr1.same_zone(addr2)) { ... }

// Convert between types
destination_zone dest = zone;  // routing_prefix + subnet, local/object address = 0
remote_object obj = dest.with_object(object_id); // adds local/object address
```

## 2. Services

A service manages the lifecycle of objects within a zone. It's the central authority for object management.

### Service Responsibilities

1. **Zone Identity**: Manages the zone ID
2. **Object Registry**: Tracks all objects in the zone
3. **Object ID Generation**: Creates unique object IDs
4. **Transport Management**: Maintains connections to other zones
5. **Service Proxy Registry**: Caches proxies to remote zones
6. **Event Notifications**: Handles lifecycle events

### Service Class Hierarchy

```
rpc::service       (abstract service base)
├── rpc::root_service  (top-level zones)
└── rpc::child_service (for hierarchical zones)
```

### Creating a Root Service

```cpp
class my_service : public rpc::root_service
{
public:
    my_service(const char* name, rpc::zone zone_id)
        : rpc::root_service(name, zone_id)
    {
        // Service initialization
    }
};

// Creation
auto service = std::make_shared<my_service>("my_service", rpc::zone{1});
```

### Child Service

For hierarchical topologies, use `child_service`:

```cpp
class my_child_service : public rpc::child_service
{
public:
    my_child_service(const char* name, rpc::zone zone_id,
                     std::shared_ptr<rpc::transport> parent_transport)
        : rpc::child_service(name, zone_id, std::move(parent_transport))
    {
    }
};
```

**Key Difference**: `child_service` maintains a strong reference to the parent transport, ensuring the parent zone lives as long as any child.

### Key Service Methods

```cpp
// Zone and Object ID Management
rpc::object generate_new_object_id();
rpc::object get_object_id(const rpc::casting_interface* ptr);

// Zone Connection
template<typename InInterface, typename OutInterface>
CORO_TASK(rpc::service_connect_result<OutInterface>)
connect_to_zone(const char* name,
                std::shared_ptr<rpc::transport> transport,
                const rpc::shared_ptr<InInterface>& input_interface);

// Remote Zone Attachment
template<typename InterfaceType, typename... Args>
CORO_TASK(rpc::remote_object_result)
attach_remote_zone(const char* name,
                   std::shared_ptr<rpc::transport> transport,
                   const rpc::interface_descriptor& input_descr,
                   const rpc::interface_descriptor& output_descr,
                   SetupCallback<InterfaceType, Args...> setup);

```

## 3. Smart Pointers

Canopy provides specialized smart pointers for local and remote interface
references. **Critical**: Never mix `rpc::shared_ptr` with `std::shared_ptr`.

### rpc::shared_ptr<T>

Thread-safe reference-counted smart pointer for local or remote interface
objects.

**Characteristics**:
- Custom control block with shared, weak, and optimistic counts in the C++
  runtime
- Requires `casting_interface` inheritance
- Compatible STL API with RPC-specific extensions
- Keeps remote object and transport chain alive

**Creation**:

```cpp
// From implementation
auto obj = rpc::make_shared<my_implementation>();

// From existing pointer
rpc::shared_ptr<xxx::i_foo> foo_ptr(new foo_impl());

// Via factory function
rpc::shared_ptr<xxx::i_foo> foo;
auto error = CO_AWAIT service_->create_foo(foo);
```

**Usage**:

```cpp
// Transparent like std::shared_ptr
auto ptr = rpc::make_shared<my_impl>();
ptr->do_something();

// Check validity
if (ptr) {
    // Object is alive
}

// Reset (decrement refcount)
ptr.reset();

// Get underlying object
auto raw = ptr.get_nullable();
if (raw) {
    raw->method();
}
```

### rpc::weak_ptr<T>

Non-owning local weak reference to an `rpc::shared_ptr`-managed proxy/control
block.

```cpp
rpc::shared_ptr<xxx::i_foo> shared_ptr = rpc::make_shared<foo_impl>();
rpc::weak_ptr<xxx::i_foo> weak_ptr = shared_ptr;

// Check if still alive
if (auto locked = weak_ptr.lock()) {
    // Object is alive
    locked->method();
}
// else: object has been destroyed
```

**Use Case**: Breaking local circular references or holding non-owning local
references to proxy/control-block state.

### rpc::optimistic_ptr<T>

Callable remote weak pointer for references to objects with independent
lifetimes.

**Key Difference from shared_ptr**:
- `rpc::shared_ptr`: RAII semantics - object dies when last shared_ptr is released
- `rpc::optimistic_ptr`: Distributed weak-lifetime semantics - assumes the
  object has its own lifetime (for example database connections, services, or
  callback targets)

**Important**: `rpc::optimistic_ptr` cannot be created directly. It can only be obtained from:
1. Another `rpc::optimistic_ptr` (copy)
2. `rpc::shared_ptr` via implicit conversion or `.to_optimistic()`

**Error Behavior**:
| Pointer Type | Object Gone Error | Meaning |
|--------------|-------------------|---------|
| `rpc::shared_ptr` | `OBJECT_NOT_FOUND` | Serious - reference was held but object destroyed |
| `rpc::optimistic_ptr` | `OBJECT_GONE` | Expected - the remote weak target was checked at call time and is gone |

**Callable accessor pattern** (race mitigation for local objects — see
`documents/architecture/cpp/optimistic-ptr-race-conditions.md`):

```cpp
auto acc = opt_db->get_callable();  // pins local / captures remote
if (!acc)
    return;  // null or local-gone
auto result = CO_AWAIT acc->query("SELECT ...");
```

`callable_accessor::operator bool()` is `true` only when the underlying object
is alive.  For local-gone the accessor still provides a valid dispatch pointer
that returns `OBJECT_GONE`.

**Use Cases**:
1. References to long-lived services (databases, message queues)
2. Preventing circular dependencies in cross-zone communication
3. Callback patterns where one zone creates another and needs to receive events
4. Objects managed by external lifetime managers

```cpp
// Get optimistic_ptr from shared_ptr (implicit conversion)
rpc::shared_ptr<idatabase> db_connection = get_database_connection();
rpc::optimistic_ptr<idatabase> opt_db = db_connection;  // Implicit conversion

// Use the optimistic pointer
auto result = CO_AWAIT opt_db->query("SELECT * FROM users");

// Use Case 2: Callback pattern - Object A creates Object B in another zone
// Object A needs to receive callbacks from Object B without creating circular dependency
//
// Zone 1: Object A (creator)
//   └── shared_ptr<ObjB> (owns Object B)
//
// Zone 2: Object B (created)
//   └── optimistic_ptr<ObjA> (for callbacks, NOT ownership)

class object_a : public i_object_a
{
    rpc::shared_ptr<object_b> child_b_;  // Ownership - keeps B alive

public:
    CORO_TASK(error_code) create_child()
    {
        // Create Object B in another zone
        rpc::shared_ptr<object_b> new_b;
        auto error = CO_AWAIT zone_service_->create_object(new_b);

        // Give Object B an optimistic pointer to ourselves for callbacks
        // This allows B to call A without creating a circular reference
        CO_AWAIT rpc::make_optimistic(
            rpc::shared_ptr<object_a>(this),
            new_b->get_callback_handler());

        child_b_ = new_b;  // Ownership reference
        CO_RETURN error::OK();
    }

    // Callback from Object B
    CORO_TASK(error_code) on_event(int event_data) override
    {
        // Handle event from child
        CO_RETURN error::OK();
    }
};

class object_b : public i_object_b
{
    rpc::optimistic_ptr<object_a> parent_a_;  // For callbacks only

public:
    void set_parent_callback(rpc::optimistic_ptr<object_a> parent)
    {
        parent_a_ = parent;  // No ownership cycle across zones
    }

    CORO_TASK(error_code) do_work()
    {
        // ... do work ...

        // Notify parent of progress (using optimistic_ptr)
        // If parent is gone, returns OBJECT_GONE - expected for remote weak lifetime
        CO_AWAIT parent_a_->on_event(progress);

        CO_RETURN error::OK();
    }
};
```

**Note**: Fire-and-forget is possible with both shared_ptr and optimistic_ptr. The key distinction is lifetime semantics, not call pattern.

### No casting between rpc::shared_ptr and std::shared_ptr

**Critical Rule**: Never cast between `rpc::shared_ptr` and `std::shared_ptr`.

**Wrong**:
```cpp
auto rpc_ptr = rpc::make_shared<impl>();
std::shared_ptr<base> std_ptr = std::dynamic_pointer_cast<base>(rpc_ptr); // WRONG
```

**Correct**:
```cpp
// Use rpc::shared_ptr throughout for RPC-managed objects
rpc::shared_ptr<interface> rpc_ptr = rpc::make_shared<impl>();

// If you need std::shared_ptr for non-RPC code, create separately
std::shared_ptr<non_rpc_impl> std_ptr = std::make_shared<non_rpc_impl>();
```

## 4. Proxies and Stubs

### Proxy Architecture

Proxies represent remote objects locally and handle marshalling.

```
Client Code
    │
    ▼
┌───────────────────────┐
│   Interface Proxy     │  (Generated, e.g., i_calculator_proxy)
│   (inherits interface)│
└───────────┬───────────┘
            │ holds
            ▼
┌───────────────────────┐
│    Object Proxy       │  (rpc::object_proxy)
│   (network client)    │
└───────────┬───────────┘
            │ uses
            ▼
┌───────────────────────┐
│   Service Proxy       │  (rpc::service_proxy)
│   (remote zone)       │
└───────────┬───────────┘
            │ uses
            ▼
┌───────────────────────┐
│     Transport         │  (rpc::transport)
│   (TCP/SPSC/etc)      │
└───────────────────────┘
```

### Object Proxy (`object_proxy`)

Represents a remote object in the local zone.

**Key Members**:
```cpp
class object_proxy : public rpc::casting_interface
{
    rpc::object object_id_;                          // Remote object ID
    std::shared_ptr<service_proxy> service_proxy_;  // Remote service
    std::shared_ptr<proxy_map> proxy_map_;          // Interface proxies
    shared_count shared_count_;                     // Reference count
    optimistic_count optimistic_count_;              // Optimistic count
};
```

### Service Proxy (`service_proxy`)

Represents a remote zone's service.

**Key Members**:
```cpp
class service_proxy
{
    rpc::zone zone_id_;                      // Local zone
    rpc::destination_zone dest_zone_id_;     // Remote zone
    std::shared_ptr<service> service_;       // Local service
    stdex::member_ptr<transport> transport_; // Transport to remote zone
};
```

### Stub Architecture

Stubs receive remote calls and dispatch to local implementations.

```
Transport (TCP/SPSC/etc)
        │
        ▼
┌───────────────────────┐
│   Transport Handler   │  (Deserializes message)
└───────────┬───────────┘
            │
            ▼
┌───────────────────────┐
│    Object Stub        │  (rpc::object_stub)
│   (dispatcher)        │
└───────────┬───────────┘
            │ holds
            ▼
┌───────────────────────┐
│   Interface Stub      │  (Generated, e.g., i_calculator_stub)
│   (method dispatch)   │
└───────────┬───────────┘
            │
            ▼
┌───────────────────────┐
│   Implementation      │  (User code)
│   (concrete class)    │
└───────────────────────┘
```

### Object Stub (`object_stub`)

Server-side representative of a local object for remote callers.

**Key Members**:
```cpp
class object_stub
{
    rpc::object id_;                               // Local object ID
    stdex::member_ptr<service> zone_;              // Owning service
    stub_map stub_map_;                            // Interface stubs
    shared_count shared_count_;                    // Reference count
    optimistic_count optimistic_count_;            // Optimistic count
};
```


## 5. Interface Pattern

All IDL interfaces must inherit from `casting_interface` and implement required methods.

### Base Interface Requirements

```cpp
class app : public rpc::base<app, i_app>
{
public:
    virtual error_code do_something(int value) = 0;

};
```

### Generated Interface Base

The IDL generator creates a template base class:

```cpp
template<typename Derived>
class interface : public rpc::casting_interface
{
public:
    static constexpr uint64_t get_id(uint64_t rpc_version)
    {
        // SHA3-based fingerprint
    }
};
```

### Interface Versioning

Each interface has a version-independent ID based on its definition. The ID is derived from the interface's hash so that changes to method signatures produce a new ID, allowing the runtime to detect version mismatches at connection time:

```cpp
// Generated stub carries the interface fingerprint
template<>
struct interface_descriptor<i_calculator>
{
    static constexpr uint64_t get_id(uint64_t rpc_version)
    {
        // SHA3-based fingerprint of the interface definition
        return 0xA1B2C3D4E5F60718ULL;
    }
};
```

When `connect_to_zone` is called, the runtime exchanges these fingerprints with the remote zone. If the IDs do not match, the connection is rejected with a version error, preventing silent protocol mismatches across process or machine boundaries.

To evolve an interface without breaking existing clients, place the new version in a versioned namespace:

```idl
namespace my_service {
    [inline] namespace v1 {
        interface i_calculator {
            error_code add(int a, int b, [out] int& result);
        };
    }
    [inline] namespace v2 {
        interface i_calculator {
            error_code add(int a, int b, [out] int& result);
            error_code subtract(int a, int b, [out] int& result);
        };
    }
}
```

Old clients continue to connect against `v1::i_calculator` while new clients use `v2::i_calculator`, each with its own fingerprint.

## 6. Lifecycle Management

### Object Lifetime

Objects are managed via `rpc::shared_ptr` with automatic reference counting:

1. **Creation**: Object created with `rpc::make_shared<impl>()` or in factory
2. **Marshalling**: Object reference passed across zones via proxy
3. **Reference Counting**: Each proxy adds a reference
4. **Release**: When last proxy is destroyed, object is deleted

### Zone Lifecycle

Zones are kept alive through `shared_ptr` references:

```
Zone Death Sequence (Amnesia):
1. Triple-count reaches zero (Inbound stubs, Outbound proxies, Passthroughs)
2. Service sends async message to Edge Transport
3. Transport cleans up local resources
4. Child releases strong reference to parent
5. Optimistic pointers notified via object_released
6. Failed calls return OBJECT_GONE or OBJECT_NOT_FOUND
```

### Transport Lifetime

Transports are kept alive through service references and pass-through references:

```cpp
// Services hold weak references to transports
std::weak_ptr<transport> transport_;

// Pass-throughs hold strong references
stdex::member_ptr<transport> forward_transport_;
stdex::member_ptr<transport> reverse_transport_;
```

## Summary

Understanding these core concepts is essential for effective Canopy usage:

| Concept | Purpose | Key Class |
|---------|---------|-----------|
| Zone | Execution context boundary | `rpc::zone` |
| Service | Object lifecycle management | `rpc::service`, `rpc::child_service` |
| shared_ptr | Remote object reference | `rpc::shared_ptr<T>` |
| weak_ptr | Non-owning reference | `rpc::weak_ptr<T>` |
| optimistic_ptr | Independent lifetime | `rpc::optimistic_ptr<T>` |
| Object Proxy | Client-side remote object | `rpc::object_proxy` |
| Service Proxy | Remote zone access | `rpc::service_proxy` |
| Object Stub | Server-side representative | `rpc::object_stub` |
| Interface | Base for IDL interfaces | `rpc::interface<T>` |

## Next Steps

- [IDL Guide](../03-idl-guide.md) - Learn to define interfaces
- [Transports and Passthroughs](06-transports-and-passthroughs.md) - Understand communication channels
- [Getting Started](../02-getting-started.md) - Follow a tutorial
