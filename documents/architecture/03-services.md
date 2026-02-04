<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Services

A service manages the lifecycle of objects within a zone. It's the central authority for object management, acting as the registry, ID generator, and transport coordinator for its zone.

## What is a Service?

Each zone has exactly one service that provides:

- **Zone Identity**: Manages the zone ID
- **Object Registry**: Tracks all object_stubs in the zone
- **Object ID Generation**: Creates unique object IDs
- **Transport Management**: Maintains a registry of connections to other zones
- **Service Proxy Registry**: Maintains a registry of service proxies to remote zones
- **Event Notifications**: Handles lifecycle events

Think of the service as the "central nexus" for a zone — it knows about every object, manages their lifetimes, and coordinates communication with other zones.

## Service Class Hierarchy

```
rpc::service (base class for root zones)
└── rpc::child_service (for hierarchical zones)
```

### When to Use Each

**`rpc::service`**:
- Root zones (no parent)
- Peer-to-peer network connections
- Independent processes

**`rpc::child_service`**:
- Hierarchical topologies (parent/child zones)
- Local transport (in-process child zones)
- SGX enclaves (enclave is child of host)
- DLL boundaries (loaded DLL is child)

**make your own service**:
- When you need custom behavior
- For specialized transport protocols
- To integrate with existing systems
- To implement security policies
- To implement custom memory management

## Service Responsibilities

### 1. Zone Identity Management

```cpp
class service
{
    rpc::zone zone_id_;  // This zone's unique ID

public:
    rpc::zone get_zone_id() const { return zone_id_; }
};
```

Every object registered in the service belongs to this zone.

### 2. Object Registry

The service tracks all objects living in its zone:

```cpp
class service
{
    // Object ID → Object Stub mapping
    std::map<rpc::object, std::shared_ptr<object_stub>> stubs_;

public:
    // Register new object
    void register_object(rpc::object id, std::shared_ptr<object_stub> stub);

    // Lookup object
    std::shared_ptr<object_stub> get_stub(rpc::object id);

    // Deregister object (when refcount reaches 0)
    void deregister_object(rpc::object id);
};
```

### 3. Object ID Generation

```cpp
class service
{
    std::atomic<uint64_t> object_id_counter_{0};

public:
    rpc::object generate_new_object_id()
    {
        return rpc::object{++object_id_counter_};
    }
};
```

Object IDs are unique within a zone. Cross-zone references use `(zone_id, object_id)` pairs.

### 4. Transport Management

Services maintain weak references to transports in their registry:

```cpp
class service
{
    // Zone ID → Transport mapping (registry only, doesn't keep alive)
    std::map<rpc::destination_zone, std::weak_ptr<transport>> transports_;

public:
    // Add transport to another zone
    void add_transport(rpc::destination_zone dest, std::shared_ptr<transport> t);

    // Get transport to zone
    std::shared_ptr<transport> get_transport(rpc::destination_zone dest);
};
```

**Why weak_ptr?** Services don't own transports. The service registry is for lookup only.

**Transports are kept alive by** (as documented in `service.h`):
- **Service proxies** - Hold strong references (`member_ptr<transport>`)
- **Passthroughs** - Hold strong references to both transports and to the service
- **Child services** - Hold strong reference to parent transport
- **Active stubs** - May cause transports to hold references to adjacent transports

**Services are kept alive by**:
- **Local objects** (stubs) - Objects living in this zone
- **Child services** - Hold strong reference to parent service (via parent transport)
- **Passthroughs** - Hold strong reference to intermediary service, allowing zones to function purely as routing hubs

### 5. Service Proxy Registry

Services cache proxies to remote zones:

```cpp
class service
{
public:
    // Get or create service proxy
    std::shared_ptr<service_proxy> get_service_proxy(rpc::destination_zone dest);
};
```

Service proxies represent remote zones and provide access to objects in those zones.

### 6. Event Notifications

```cpp
class service : public i_marshaller
{
public:
...
    // Called an optimistic pointer remote object is released
    virtual void object_released

    // Called when a coonection fails betwee two zones, this will be passed to all interested zones
    virtual void transport_down
};
```

## Creating Services

### Root Service

```cpp
class my_service : public rpc::service
{
public:
    my_service(const char* name, rpc::zone zone_id)
        : rpc::service(name, zone_id)
    {
        // Service initialization
    }
};

// Creation
std::atomic<uint64_t> zone_gen{0};
auto service = std::make_shared<my_service>("my_service", rpc::zone{++zone_gen});
```

### Destructor Behavior

```cpp
child_service::~child_service()
{
    // Notify parent transport that child is shutting down
    if (parent_transport_) {
        parent_transport_->set_status(rpc::transport_status::DISCONNECTED);
    }

    // This triggers:
    // 1. Status propagation to parent zone
    // 2. child_transport::on_child_disconnected()
    // 3. Breaking of circular reference
    // 4. Clean shutdown
}
```

## Key Service Methods

### Zone and Object ID Management

```cpp
// Generate unique zone ID (for creating child zones)
rpc::zone generate_new_zone_id();

// Generate unique object ID (for registering objects)
rpc::object generate_new_object_id();

// Get object ID for a registered object
rpc::object get_object_id(const rpc::casting_interface* ptr);
```

### Zone Connection

```cpp
template<typename ServiceProxyType, typename... Args>
CORO_TASK(error_code) connect_to_zone(
    const char* name,
    std::shared_ptr<rpc::transport> transport,
    const rpc::shared_ptr<ServiceProxyType>& service_proxy,
    Args&&... args);
```

Establishes a connection to another zone and initializes service proxy.

### Remote Zone Attachment

```cpp
template<typename InterfaceType, typename... Args>
CORO_TASK(error_code) attach_remote_zone(
    const char* name,
    std::shared_ptr<rpc::transport> transport,
    const rpc::interface_descriptor& input_descr,
    const rpc::interface_descriptor& output_descr,
    SetupCallback<InterfaceType, Args...> setup);
```

Attaches to a remote zone with interface negotiation.

### Stub Registration

```cpp
void add_interface_stub_factory(
    rpc::interface_ordinal interface_id,
    i_interface_stub_factory* factory);
```

Registers factory for creating interface stubs (called during code generation).

## Service Lifecycle

### Service Birth

```
1. Service constructor:
   - Initializes zone_id_
   - Sets up object registry
   - Registers telemetry hooks

2. Transport attachment:
   - add_transport() for each connected zone
   - Creates service_proxy for remote zones

3. Object registration:
   - User objects registered via service
   - Stubs created for remote access
```

### Service Life

Service stays alive as long as:
- Local objects exist (stubs registered)
- Remote proxies hold references to local objects
- Passthroughs route through this zone
- Strong references exist (for child_service: parent_transport_)

### Service Death

```
1. Reference counts reach zero:
   - No local stubs
   - No remote proxies to local objects
   - No passthroughs routing through this zone

2. Transport cleanup (MUST complete before destructor):
   - All transports disconnected (status set to DISCONNECTED)
   - All transports unregistered from transports_ map
   - Service proxies release their transport references

3. service destructor called:
   - Verifies all stubs released (check_is_empty())
   - Clears service_proxies_ map
   - Notifies telemetry of service deletion
```

**Critical Requirement: Transport Cleanup Before Service Destruction**

By the time `service::~service()` is called:
- All transports MUST be **disconnected** (status = `DISCONNECTED`)
- All transports MUST be **unregistered** from the `transports_` registry
- All service proxies must be released (enforced at destructor line 104)

**Exception for child_service:**
- The parent_transport is intentionally kept alive DURING `child_service::~child_service()`
- The destructor calls `parent_transport->set_status(DISCONNECTED)`
- This triggers the safe disconnection protocol
- The parent transport propagates the status to the parent zone
- The child_transport in parent zone breaks its circular reference
- Stack-based protection ensures safety during active calls

See `documents/transports/hierarchical.md` for complete hierarchical transport lifecycle details.

## Hidden Service Principle

Each object should only interact with its own service via `get_current_service()` (non-coroutine code only; this function is not suitable for coroutine code).

**Why?** Objects shouldn't know about zone topology. This keeps code portable across different zone configurations.

## Service Isolation

### Each Service Manages Only Its Own Zone

```cpp
// Wrong: Cross-service access
auto stub = service1->get_stub(object_id_from_service2);  // Error!

// Correct: Use proxy for cross-zone access
rpc::shared_ptr<i_interface> input;  // Interface to send to remote zone
rpc::shared_ptr<i_interface> output;  // Interface received from remote zone
CO_AWAIT service1->connect_to_zone("remote", transport, input, output);
auto error = CO_AWAIT output->method();
```

### Service Proxy Represents Remote Service

```cpp
class service_proxy
{
    rpc::zone zone_id_;                      // Local zone
    rpc::destination_zone dest_zone_id_;     // Remote zone
    std::shared_ptr<service> service_;       // Local service
    stdex::member_ptr<transport> transport_; // Transport to remote zone
};
```

Service proxy is the gateway to a remote zone's service.

## Common Patterns

### Pattern 1: Peer-to-Peer Services

```cpp
// Service 1
auto service1 = std::make_shared<rpc::service>("service1", rpc::zone{1});

// Service 2
auto service2 = std::make_shared<rpc::service>("service2", rpc::zone{2});

// Connect via TCP
auto tcp_transport = std::make_shared<rpc::tcp_transport>(...);
service1->add_transport(rpc::destination_zone{2}, tcp_transport);
service2->add_transport(rpc::destination_zone{1}, tcp_transport);
```

### Pattern 2: Hierarchical Services

```cpp
// Parent service
auto parent_service = std::make_shared<rpc::service>("parent", rpc::zone{1});

// Create child zone
auto new_zone_id = parent_service->generate_new_zone_id();

auto child_transport = std::make_shared<rpc::local::child_transport>(
    "child",
    parent_service,
    new_zone_id);

child_transport->set_child_entry_point<i_example_parent, i_example_child>(
    [](const rpc::shared_ptr<i_example_parent>& parent_interface,
       rpc::shared_ptr<i_example_child>& child_interface,
       const std::shared_ptr<rpc::child_service>& child_service) -> CORO_TASK(int) {
        // Register stubs in child zone
        // Create child interface object
        child_interface = rpc::make_shared<example_child_impl>(child_service, parent_interface);
        CO_RETURN rpc::error::OK();
    });

// Child service created inside entry point callback
// child_service holds strong reference to parent_transport
```

### Pattern 3: Service with Multiple Transports

```cpp
auto service = std::make_shared<rpc::service>("hub", rpc::zone{1});

// Connect to multiple zones
service->add_transport(rpc::destination_zone{2}, tcp_transport1);
service->add_transport(rpc::destination_zone{3}, tcp_transport2);
service->add_transport(rpc::destination_zone{4}, local_transport);

// Service acts as hub for communication
```

### Common Issues

**Problem**: Service dies prematurely
- **Cause**: All objects deregistered too early
- **Fix**: Check object lifetimes, ensure stubs registered

**Problem**: Service never dies (leak)
- **Cause**: Forgotten transport references or circular dependencies
- **Fix**: Check transport cleanup, review passthrough lifecycles

**Problem**: Object not found in service
- **Cause**: Object deregistered or wrong zone
- **Fix**: Verify zone_id matches, check registration timing

## Code References

**Service Classes**:
- `rpc/include/rpc/service.h` - Service base class
- `rpc/include/rpc/child_service.h` - Child service class
- `rpc/src/service.cpp` - Service implementation
- `rpc/src/child_service.cpp` - Child service implementation

**Service Proxy**:
- `rpc/include/rpc/internal/service_proxy.h` - Service proxy class
- `rpc/src/service_proxy.cpp` - Service proxy implementation

## Next Steps

- [Memory Management](04-memory-management.md) - Reference counting and smart pointers
- [Proxies and Stubs](05-proxies-and-stubs.md) - RPC marshalling machinery
- [Zone Hierarchies](07-zone-hierarchies.md) - Hierarchical service patterns
