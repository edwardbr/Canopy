<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Zones

A zone is an execution context that represents a boundary between different parts of a distributed system. Zones are the fundamental isolation mechanism in Canopy, separating code running in different processes, machines, or security domains.

## What is a Zone?

Each zone has its own:

- **Zone Address**: A 128-bit unique identifier (`rpc::zone`) structured like an IPv6 address
- **Object Namespace**: Objects are identified within a zone using a 32-bit Object ID, which is now part of the `zone_address`.
- **Service**: Manages object lifecycle and references
- **Transport Connections**: Links to other zones

Think of a zone as a membrane that separates "here" from "there":
- **Inside the zone**: Direct C++ function calls, direct memory access
- **Across zone boundaries**: RPC calls, serialization, transport overhead

## Zone Identity

### Configurable Zone Addressing

Canopy uses a structured addressing model inspired by IPv4/IPv6. This enables network-routable addressing and hierarchical subnet-based allocation.

The `zone_address` struct supports three addressing modes, selectable at build time via CMake:

**Mode: None (local-only)**
No routing prefix. The address is a simple local identifier, equivalent to sequential zone counters. `subnet_id` identifies the zone, `object_id` identifies the resource within it. Suitable for single-process and in-memory transports.

**Mode: IPv4 with local suffix**
A 32-bit IPv4 address is mapped into the 64-bit routing prefix (using 6to4 encoding). The remaining address space is split between subnet and object fields. Suitable for traditional network deployments where IPv4 addressing is used between nodes.

**Mode: IPv6**
Full 128-bit address space usage. The routing prefix occupies the upper 64 bits, with subnet (32 bits) and object (32 bits) sharing the lower 64 bits.

### Field Width Configuration (Future)

Future CMake options may allow configurable field widths:

- **Subnet size**: Currently 32 bits (fixed)
- **Object size**: Currently 32 bits (fixed)

This allows the same `zone_address` type to serve different deployment scenarios while maintaining a consistent 128-bit total size.

### Zone Address Structure

A `zone_address` consists of three main components:

- **Routing Prefix (64 bits)**: Identifies the physical node or network (0 in local-only mode)
- **Subnet ID (32 bits)**: Identifies the specific zone within that node
- **Object ID (32 bits)**: Identifies a specific object within that zone (0 indicates a zone-only address)

**Total size**: 16 bytes = 128 bits (one IPv6 address). The layout is:
```
Bits 0-63:   routing_prefix (network prefix, identifies the physical node)
Bits 64-95:  subnet_id (zone within the node)
Bits 96-127: object_id (specific object, or 0 for zone-only)
```

**CMake-configurable packed representation**: A CMake option controls whether the in-memory representation uses the structured form above or a packed `uint128_t` / `std::array<uint8_t, 16>` for compact storage and wire serialization. The structured form with named fields is always the canonical form for code access.

### Zone Address Types

Canopy uses specialized types for different routing scenarios, all wrapping a `zone_address`:

- `rpc::zone`: Stores a `zone_address` with `object_id = 0`. Used for general zone identity.
- `rpc::remote_object`: Stores a full `zone_address` **including** the `object_id`. This identifies a specific object at a specific zone. Used in i_marshaller methods.
- `rpc::destination_zone`: Stores a `zone_address` with `object_id = 0` (zone-only). Used for zone routing without object identity.
- `rpc::caller_zone`: Stores a `zone_address` with `object_id = 0`. Never needs object identity.
- `rpc::requesting_zone`: Stores a `zone_address` with `object_id = 0`. Used as a routing hint.

### Zone ID Generation Strategies

Zone IDs (subnets) are allocated using a `zone_address_allocator`. This ensures uniqueness within the node's allocated subnet range.

**CLI Configuration**:
Nodes can be configured via CLI arguments to use specific network ranges:
```bash
# Local-only mode (default)
./my_app

# Use IPv4 192.168.1.1 as routing prefix, with a specific subnet range
./my_app -4 --routing-prefix 192.168.1.1 --subnet-base=0x10000 --subnet-range=0xFFFF

# Use IPv6 prefix
./my_app -6 --routing-prefix 2001:db8::1 --subnet-base=0x10000
```

**Address-family flags** (mutually exclusive):
- `-4`: `--routing-prefix` is an IPv4 address in dotted-decimal notation
- `-6`: `--routing-prefix` is an IPv6 address in colon-hex notation
- *(neither)*: Local-only mode: `routing_prefix=0`, inter-node routing disabled

**IPv4 → routing_prefix conversion** (6to4-inspired, RFC 3056):
```
routing_prefix = 0x2002 << 48 | (uint64_t)ipv4_addr << 16
```
For example `192.168.1.1` (`0xC0A80101`): `routing_prefix = 0x2002C0A801010000`

**IPv6 → routing_prefix**: The first 64 bits of the address are taken directly as the routing prefix.

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

The root zone is typically created using a `zone_address_allocator` configured via network arguments.

```cpp
#include <canopy/network_config/network_args.h>

// ... in main()
args::ArgumentParser parser("My App");
canopy::network_config::add_network_args(parser);
parser.ParseCLI(argc, argv);

auto cfg = canopy::network_config::get_network_config(parser);
auto allocator = canopy::network_config::make_allocator(cfg);

auto root_service = std::make_shared<rpc::service>(
    "root_service",
    allocator.allocate_zone(),
    scheduler
);
```

**Default behavior**: If no `--routing-prefix` is provided, the library auto-detects the best routing prefix from the host's network interfaces (globally-routable IPv6 > public IPv4 > private IPv4 > 0 for local-only).

### Child Zone (Hierarchical)

Child zones are created through transport connections:

```cpp
// rpc::local::child_transport is mainly used for testing, but it shows the
// same connect_to_zone pattern used by the current demos.

// From parent zone
auto child_transport = std::make_shared<rpc::local::child_transport>("example_zone", this_service, new_zone);

child_transport->set_child_entry_point<yyy::i_host, yyy::i_example>(
    [](const rpc::shared_ptr<yyy::i_host>& host,
        const std::shared_ptr<rpc::child_service>& child_service_ptr)
        -> CORO_TASK(rpc::service_connect_result<yyy::i_example>)
    {
        CO_RETURN rpc::service_connect_result<yyy::i_example>{
            rpc::error::OK(),
            rpc::shared_ptr<yyy::i_example>(new marshalled_tests::example(child_service_ptr, host))};
    });

auto connect_result = CO_AWAIT this_service->connect_to_zone<yyy::i_host, yyy::i_example>(
    "example_zone", child_transport, host_ptr);
auto target = connect_result.output_interface;
auto err_code = connect_result.error_code;
```

### Peer Zone (Network)

Peer zones connect via TCP or other network transports, this example uses coroutines:

```cpp
// Server side
auto server_service = std::make_shared<rpc::service>("server", get_next_zone_id(), io_scheduler_);

// Create a streaming listener; the connection callback is invoked for each new client
const coro::net::socket_address endpoint{
    coro::net::ip_address::from_string("127.0.0.1"), static_cast<uint16_t>(8080)};

auto listener = std::make_shared<streaming::listener>("server_transport",
    std::make_shared<streaming::tcp::acceptor>(endpoint),
    rpc::stream_transport::make_connection_callback<yyy::i_client, yyy::i_example>(
        [](const rpc::shared_ptr<yyy::i_client>& client,
            const std::shared_ptr<rpc::service>& svc)
            -> CORO_TASK(rpc::service_connect_result<yyy::i_example>)
        {
            CO_RETURN rpc::service_connect_result<yyy::i_example>{
                rpc::error::OK(),
                rpc::shared_ptr<yyy::i_example>(new example_impl(svc, client))};
        }));

if (!listener->start_listening(server_service))
{
    RPC_ERROR("Failed to start TCP listener");
    CO_RETURN false;
}

// Client side

// Create the client service
auto client_service = std::make_shared<rpc::service>("client", get_next_zone_id(), io_scheduler_);

coro::net::tcp::client tcp_client(io_scheduler_,
    coro::net::socket_address{
        coro::net::ip_address::from_string("127.0.0.1"), static_cast<uint16_t>(8080)});

auto connection_status = CO_AWAIT tcp_client.connect(std::chrono::milliseconds(5000));
if (connection_status != coro::net::connect_status::connected)
{
    RPC_ERROR("Failed to connect TCP client to server");
    CO_RETURN false;
}

// Wrap the connected socket in a stream and create the streaming transport
auto tcp_stm = std::make_shared<streaming::tcp::stream>(std::move(tcp_client), io_scheduler_);
auto client_transport = rpc::stream_transport::make_client("client_transport",
    client_service, std::move(tcp_stm));

// an object to feed to the server if required
rpc::shared_ptr<yyy::i_client> cli(new client_impl());

// Connect using the client transport; result carries error_code and the remote interface
auto connect_result = CO_AWAIT client_service->connect_to_zone<yyy::i_client, yyy::i_example>(
    "main child", client_transport, cli);
if (connect_result.error_code != rpc::error::OK())
{
    CO_RETURN false;
}
auto example = connect_result.output_interface;

// now we are talking to another zone
CO_AWAIT example->do_something();

```

## Zone Types for Routing

Canopy uses specialized zone types for different routing scenarios:

```cpp
struct zone   // The current zone where all this activity is happening
struct destination_zone   // Where the call is going (includes object_id)
struct caller_zone        // Where the call came from
struct requesting_zone  // Zone with known calling direction, used in add_ref to deal with zones that do not know about the existance of other zones, the requesting_zone is used to route the add_ref to a zone that can pass on the add_ref to the correct zone
```

### Why Different Types?

These types enable efficient routing in multi-hop scenarios and provide type safety:

```cpp
// Transport routing logic
CORO_TASK(int) transport::inbound_send(
    rpc::caller_zone caller,           // Who's calling
    rpc::destination_zone destination, // Where it's going (includes object_id)
    rpc::interface_ordinal interface_id,
    rpc::method method_id,
    const rpc::span& in_data,
    std::vector<char>& out_data)
{
    // Route based on destination zone portion (ignoring object_id)
    if (destination.get_address().same_zone(get_zone_id())) {
        // Deliver locally to the object specified in destination.get_address().object_id
    } else {
        // Forward to passthrough
    }
}
```

### Conversion Between Zone Types

```cpp
rpc::zone zone_id{42};

auto dest = zone_id;         // For routing to target (object_id=0)
auto caller = zone_id;            // For tracking origin
auto known = zone_id;    // For known routing
auto obj = dest.with_object(object_id);       // For specific object (remote_object)
```

### Accessing the Full Address

```cpp
rpc::remote_object remote_obj = ...;

// Get the full zone_address (routing_prefix, subnet_id, object_id)
const zone_address& addr = remote_obj.get_address();

// Access individual components
uint64_t prefix = addr.get_routing_prefix();
uint64_t subnet = addr.get_subnet();
uint64_t obj_id = addr.get_object_id();

// Check if two addresses are in the same zone (ignoring object_id)
if (addr1.same_zone(addr2)) { ... }
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
            remote_object remote_object_id,  // includes zone and object_id
            caller_zone_id,
            interface_id,
            method_id)
on_service_post(zone_id,
            remote_object remote_object_id,  // includes zone and object_id
            caller_zone_id,
            interface_id,
            method_id)
on_service_try_cast(zone_id,
            remote_object remote_object_id,  // includes zone and object_id
            caller_zone_id,
            interface_id)
on_service_add_ref(zone_id,
            remote_object remote_object_id,  // includes zone and object_id
            caller_zone_id,
            requesting_zone_id,
            options)
on_service_release(zone_id,
            remote_object remote_object_id,  // includes zone and object_id
            caller_zone_id,
            options)
on_service_object_released(
            zone_id, remote_object remote_object_id, caller_zone_id)
on_service_transport_down(
            zone_id, destination_zone destination_zone_id, caller_zone_id)  // zone-only, no object_id
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
- `interfaces/rpc/rpc_types.idl` - Zone definition

**Service Management**:
- `c++/rpc/include/rpc/service.h` - Zone lifecycle management
- `c++/rpc/src/service.cpp` - Zone operations

**Zone Types**:
- `rpc_types.idl:destination_zone` - Routing target
- `rpc_types.idl:caller_zone` - Call origin
- `rpc_types.idl:requesting_zone` - Known direction

## Next Steps

- [Services](03-services.md) - Object lifecycle management within zones
- [Memory Management](04-memory-management.md) - Reference counting across zones
- [Zone Hierarchies](07-zone-hierarchies.md) - Multi-level zone topologies
