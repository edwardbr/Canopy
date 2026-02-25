<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Zones

A zone is an execution context that represents a boundary between different parts of a distributed system. Zones are the fundamental isolation mechanism in Canopy, separating code running in different processes, machines, or security domains.

## What is a Zone?

Each zone has its own:

- **Zone ID**: A unique identifier (`rpc::zone`)
- **Object Namespace**: Objects are uniquely identified within a zone
- **Service**: Manages object lifecycle and references
- **Transport Connections**: Links to other zones

Think of a zone as a membrane that separates "here" from "there":
- **Inside the zone**: Direct C++ function calls, direct memory access
- **Across zone boundaries**: RPC calls, serialization, transport overhead

## Zone Identity

### Zone IDs Must Be Unique

**Important Rule**: Zone IDs must be unique across the entire distributed system.

### Zone ID Generation Strategies

Canopy does not prescribe a specific zone ID allocation scheme. Common strategies include:

**IPv4-Based Schemes**:
- Use IP address as the zone ID base
- Combine with locally assigned random number or sequential counter
- Example: `zone_id = (ip_address << 32) | local_counter`

**IPv6-Based Schemes**:
- Use IP address with subnet-based allocation
- For larger deployments, extend zone type to 128-bit:
  - 64 bits for IP address
  - 64 bits for local ID
- Alternative: 32 bits for IP, 32 bits for local zone number, 32 bits for object ID, 32 bits reserved

**TCP Transport Zone ID Translation**:
- For environments where IP-based schemes are not feasible
- TCP transport provides zone ID translation/mapping capabilities

**Implementation Details**:
*[Placeholder: Specific zone ID generator implementation and examples to be added]*

## Zone Hierarchy

Zones can form parent/child relationships through hierarchical transports (local, SGX, DLL):

```
Zone 1 (Root)
├── Zone 2 (Child)
│   ├── Zone 4 (Grandchild)
│   └── Zone 5 (Grandchild)
└── Zone 3 (Child)
```

**Key Rules**:
- Children hold strong references to parents (via `child_service`)
- Parent is guaranteed to outlive children
- Zone IDs must be unique across the entire system
- Each zone can only create zones directly adjacent to itself
- Any calls to non-adjacent zones must use passthrough transports.

### Adjacency Rules

```
Zone 1 can create:
  ✓ Zone 2 (direct child)
  ✓ Zone 3 (direct child)

Zone 2 can create:
  ✓ Zone 4 (its direct child)
  ✗ Zone 3 (sibling - must use passthrough)
  ✗ Zone 5 (grandchild via Zone 3 - wrong parent)
```

## Creating Zones

### Root Zone

```cpp
auto root_service = std::make_shared<rpc::service>(
    "root_service",
    rpc::zone{1}  // Zone ID 1
#ifdef CANOPY_BUILD_COROUTINE
    , scheduler   // Optional coroutine scheduler
#endif
);
```

### Child Zone (Hierarchical)

Child zones are created through transport connections:

```cpp

// rpc::local::child_transport is mainly used for testing but is good in this case to demonstrate the code needed in the child zone.

// From parent zone
auto child_transport = std::make_shared<rpc::local::child_transport>("example_zone", this_service, new_zone);

child_transport->set_child_entry_point<yyy::i_host, yyy::i_example>(
    [](const rpc::shared_ptr<yyy::i_host>& host,
        rpc::shared_ptr<yyy::i_example>& new_example,
        const std::shared_ptr<rpc::child_service>& child_service_ptr) -> CORO_TASK(error_code)
    {
        // Create the object in the child zone, to be transferred to the parent zone.
        new_example = rpc::shared_ptr<yyy::i_example>(new marshalled_tests::example(child_service_ptr, host));

        CO_RETURN rpc::error::OK();
    });

auto err_code = CO_AWAIT this_service->connect_to_zone("example_zone", child_transport, host_ptr, target);    
```

### Peer Zone (Network)

Peer zones connect via TCP or other network transports, this example uses coroutines:

```cpp
// Server side
auto server_service = std::make_shared<rpc::service>("server", get_next_zone_id(), io_scheduler_);

// Create the listener for the server side
// The connection handler will be called when a client connects
auto listener = std::make_unique<rpc::tcp::listener>(
    [this](const rpc::interface_descriptor& input_descr,
        rpc::interface_descriptor& output_interface,
        std::shared_ptr<rpc::service> child_service_ptr,
        std::shared_ptr<rpc::tcp::tcp_transport> transport) -> CORO_TASK(int)
    {

        // Use attach_remote_zone to properly manage object lifetime, like SPSC does
        auto ret = CO_AWAIT child_service_ptr->attach_remote_zone<yyy::i_client, yyy::i_example>("service_proxy",
            transport,
            input_descr,
            output_interface,
            [&](const rpc::shared_ptr<yyy::i_client>& client,
                rpc::shared_ptr<yyy::i_example>& new_example,
                const std::shared_ptr<rpc::service>& service_ptr) -> CORO_TASK(int)
            {
                new_example = rpc::make_shared<marshalled_tests::example>(service_ptr, client);

                CO_RETURN rpc::error::OK();
            });
        CO_RETURN ret;
    },

auto server_options = coro::net::tcp::server::options{
    .address = {coro::net::ip_address::from_string("127.0.0.1")}, .port = 8080, .backlog = 128};

if (!listener->start_listening(peer_service_, server_options))
{
    RPC_ERROR("Failed to start TCP listener");
    CO_RETURN false;
}

// Client side

    
// Create the client service
auto client_service = std::make_shared<rpc::service>("client", get_next_zone_id(), io_scheduler_);

coro::net::tcp::client tcp_client(scheduler,
    coro::net::tcp::client::options{
        .address = {coro::net::ip_address::from_string("127.0.0.1")},
        .port = 8080,
    });

auto connection_status = CO_AWAIT tcp_client.connect();
if (connection_status != coro::net::connect_status::connected)
{
    RPC_ERROR("Failed to connect TCP client to server");
    CO_RETURN false;
}

// Create the client transport
auto client_transport = rpc::tcp::tcp_transport::create("client_transport",
    client_service,
    peer_zone_id,
    std::chrono::milliseconds(100000),
    std::move(tcp_client),
    nullptr); // client doesn't need handler

// Start the client coroutine message pump - this must run before we call connect
client_service->spawn(client_transport_->pump_send_and_receive());

// an object to feed to the server if required
rpc::shared_ptr<yyy::i_client> cli(new client());
rpc::shared_ptr<yyy::i_example> example;

// Connect using the client transport
auto ret = CO_AWAIT client_service->connect_to_zone("main child", client_transport, cli, example);

// now we are talking to another zone
CO_AWAIT example->do_something();
    
```

## Zone Types for Routing

Canopy uses specialized zone types for different routing scenarios:

```cpp
struct zone   // The current zone where all this activity is happening
struct destination_zone   // Where the call is going
struct caller_zone        // Where the call came from
struct requesting_zone  // Zone with known calling direction, used in add_ref to deal with zones that do not know about the existance of other zones, the requesting_zone is used to route the add_ref to a zone that can pass on the add_ref to the correct zone
```

### Why Different Types?

These types enable efficient routing in multi-hop scenarios and provide type safety:

```cpp
// Transport routing logic
CORO_TASK(int) transport::inbound_send(
    rpc::caller_zone caller,           // Who's calling
    rpc::destination_zone destination, // Where it's going
    rpc::object object_id,
    rpc::interface_ordinal interface_id,
    rpc::method method_id,
    const rpc::span& in_data,
    std::vector<char>& out_data)
{
    // Route based on destination
    if (destination == get_zone_id()) {
        // Deliver locally
    } else {
        // Forward to passthrough
    }
}
```

### Conversion Between Zone Types

```cpp
rpc::zone zone_id{42};

auto dest = zone_id.as_destination();         // For routing to target
auto caller = zone_id.as_caller();            // For tracking origin
auto known = zone_id.as_requesting_zone(); // For known routing
```

## Zone Boundaries

### What Crosses Zone Boundaries?

**Can Cross**:
- `rpc::shared_ptr<Interface>` - Marshalled to remote reference
- `rpc::optimistic_ptr<Interface>` - Marshalled to optimistic reference
- Serializable types (int, string, vector, map, etc.)
- IDL-defined structures

**Cannot Cross**:
- Raw pointers (`Interface*`)
- `std::shared_ptr` (use `rpc::shared_ptr`)
- Non-serializable types (lambda functions, file handles, etc.)
- Thread-local storage

### Boundary Crossing Example

```cpp
// Zone 1: Client
rpc::shared_ptr<i_calculator> calc;
CO_AWAIT service->get_calculator(calc);  // Returns proxy to Zone 2

int result;
auto error = CO_AWAIT calc->add(5, 3, result);  // RPC call crosses boundary
// Serializes: method_id, parameters (5, 3)
// Deserializes: result (8)
```

## Zone Lifecycle

### Zone Birth

```
1. Zone ID allocated
2. Service created with zone ID
3. Transport connections established
4. Objects registered in service
```

### Zone Life

Zone stays alive as long as:
- Local objects exist in the zone (inbound stubs)
- Remote proxies reference objects in the zone (outbound proxies)
- Passthroughs route through the zone (passthrough objects)

### Zone Death 

When all three kinds of relationships reach zero, the zone enters shutdown:

```
1. When the last relationship is removed:
   - Inbound stubs count = 0
   - Outbound proxies count = 0
   - Passthrough objects count = 0

2. Service signals transport:
   service->set_status(rpc::transport_status::DISCONNECTED)

3. Transport cleanup:
   - Disconnect from remote zones
   - Release parent reference (for child zones)
   - Notify optimistic pointers (returns OBJECT_GONE)

4. Zone destruction:
   - Service destructor runs
   - Transport destructor runs
   - Zone memory released
```

See [Memory Management](04-memory-management.md) for details on the reference counting system.

## Zone Isolation

### Object Visibility

Objects are visible only within their zone and to remote zones via proxy.
When transports connect one or two shared or optimistic pointers are exchanged.  It is through these pointers that other pointers can be obtained according to the api designs of the interfaces.

### Service Isolation

Each zone's service manages only its own objects.

## Zone Communication Patterns

### Point-to-Point

Direct transport between two zones:

```
Zone 1 ←→ Zone 2
```

### Hierarchical

Parent/child relationships:

```
Zone 1 (Root)
├── Zone 2 (Child)
│   └── Zone 4 (Grandchild)
└── Zone 3 (Child)
    └── Zone 5 (Grandchild)
```

### Mesh

Passthroughs enable complex routing:

```
Zone 1 ←→ Zone 2 ←→ Zone 3
           ↕
        Zone 4
```

Zone 1 → Zone 4 communication routes through Zone 2's passthrough.

## Best Practices

### Zone ID Management

For zone ID generation strategies, see [Zone ID Generation Strategies](#zone-id-generation-strategies).

**Do**:
- Use a consistent zone ID generation strategy across your deployment
- Document your zone ID allocation scheme
- Log zone creation/destruction events for troubleshooting
- Consider IP-based schemes for network-distributed systems

**Don't**:
- Hardcode zone IDs in source code
- Reuse zone IDs after a zone dies
- Assume zone IDs are sequential or contiguous

### Zone Lifetime

**Do**:
- Let reference counting manage lifecycle
- Monitor shutdown events in telemetry
- Handle OBJECT_GONE gracefully

**Don't**:
- Force zone destruction
- Assume synchronous cleanup
- Hold references longer than needed

### Zone Design

**Do**:
- Group related objects in same zone
- Minimize cross-zone call frequency
- Use hierarchical zones for isolation

**Don't**:
- Create zones for every object
- Mix security boundaries carelessly
- Cross zones unnecessarily

## Debugging Zones

### Telemetry Events

```javascript
on_service_creation(name, zone_id, parent_zone_id)
 on_service_deletion(zone_id)
 on_service_send(zone_id,
            destination_zone destination_zone_id,
            caller_zone_id,
            object_id,
            interface_id,
            method_id)
 on_service_post(zone_id,
            destination_zone destination_zone_id,
            caller_zone_id,
            object_id,
            interface_id,
            method_id)
 on_service_try_cast(zone_id,
            destination_zone destination_zone_id,
            caller_zone_id,
            object_id,
            interface_id)
 on_service_add_ref(zone_id,
            destination_zone destination_zone_id,
            object_id,
            caller_zone_id,
            requesting_zone_id,
            options)
 on_service_release(zone_id,
            destination_zone destination_zone_id,
            object_id,
            caller_zone_id,
            options)
 on_service_object_released(
            zone_id, destination_zone destination_zone_id, caller_zone_id, object object_id)
 on_service_transport_down(
            zone_id, destination_zone destination_zone_id, caller_zone_id)
```

### Common Issues

**Problem**: Zone dies prematurely
- **Cause**: All references released too early
- **Fix**: Check object lifetimes, ensure references held during work

**Problem**: Zone never dies (leak)
- **Cause**: Circular references or forgotten proxies
- **Fix**: Use `rpc::weak_ptr` to break cycles, audit proxy lifetimes

**Problem**: ZONE_NOT_FOUND error
- **Cause**: Zone died before call completed
- **Fix**: Hold reference to service proxy during operation

## Code References

**Zone Structure**:
- `rpc/interfaces/rpc_types.idl` - Zone definition

**Service Management**:
- `rpc/include/rpc/service.h` - Zone lifecycle management
- `rpc/src/service.cpp` - Zone operations

**Zone Types**:
- `rpc_types.idl:destination_zone` - Routing target
- `rpc_types.idl:caller_zone` - Call origin
- `rpc_types.idl:requesting_zone` - Known direction

## Next Steps

- [Services](03-services.md) - Object lifecycle management within zones
- [Memory Management](04-memory-management.md) - Reference counting across zones
- [Zone Hierarchies](07-zone-hierarchies.md) - Multi-level zone topologies
