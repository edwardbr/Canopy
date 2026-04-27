<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Zone Hierarchies

Scope note:

- this document describes the shared zone-topology model through the primary
  C++ implementation
- concrete class names, transport families, and lifecycle examples are
  C++-specific unless explicitly stated otherwise
- see [C++ Status](../status/cpp.md), [Rust Status](../status/rust.md), and
  [JavaScript Status](../status/javascript.md) for implementation scope

The name Canopy reflects the architecture: like a forest of trees, each node in the system grows its own tree of zones — a root zone from which child zones branch outward. Multiple nodes connect as peers across a network or mesh, and objects living at any level of any tree can communicate with objects at any level of any other tree, just as the leaves of a forest canopy interweave above the trunks.

## 1. Per-Node Zone Trees

In the primary C++ implementation, each machine or process hosts its own root
zone. From the root, child zones branch for plugins, enclaves, DLLs, or any
other isolation boundary. A typical single-node hierarchy looks like:

```
Root Zone (Node A)
│
├── Zone 2 (Plugin A)
│   ├── Zone 4 (Grandchild)
│   └── Zone 5 (Grandchild)
│
├── Zone 3 (Plugin B)
│   └── Zone 6 (Grandchild)
│
└── Zone 7 (SGX Enclave)
```

### Key Properties

- **Parent-Child Relationships**: Each zone (except the root) has exactly one parent
- **Strong References**: Children hold strong references to parents
- **Lifecycle Guarantee**: Parent outlives all children
- **Unique IDs**: Zone IDs are unique across the entire mesh

### Child Service Pattern

```cpp
class my_child_service : public rpc::child_service
{
    rpc::shared_ptr<yyy::i_host> host_;

public:
    my_child_service(const char* name, rpc::zone zone_id,
                     std::shared_ptr<rpc::transport> parent_transport,
                     rpc::shared_ptr<yyy::i_host> host)
        : rpc::child_service(name, zone_id, std::move(parent_transport))
        , host_(host)
    {
    }
};
```

## 2. Multi-Node Mesh

Separate nodes connect as peers through streaming transports (TCP, SPSC, WebSocket, and others). There is no global owner: each node's root zone is independent and the connection between two nodes is symmetric. A root zone on Node A connects to the root zone on Node B as a peer, not as a parent–child pair.

```
Node A                            Node B
──────────────────                ──────────────────
Root Zone (A)  ◄── TCP ──────►  Root Zone (B)
├── Zone A2                       ├── Zone B2
└── Zone A3                       └── Zone B3
```

This arrangement scales naturally to a mesh of many nodes:

```
Node A ◄──────► Node B ◄──────► Node C
  │                                 │
  └─────────────────────────────────┘
```

Objects anywhere in the mesh — deep inside Zone A3 or at the root of Node C — reach each other through the same RPC mechanisms. When a call must travel through an intermediate node, that node acts as a relay, forwarding the message onward without the caller needing to know the full path.

### Root Zone Setup

Each node creates its own root service and connects out via transport:

```cpp
// Node A: create root and connect to Node B
auto root_service = rpc::root_service::create("node_a", zone_a_id);
auto transport = rpc::stream_transport::streaming_transport::create(
    root_service,
    tcp_stream,          // established TCP connection to Node B
    connection_handler); // handles inbound calls from Node B

// Retrieve an interface from Node B's root zone
rpc::shared_ptr<i_factory> input_factory;
auto [err, remote_factory] = CO_AWAIT root_service->connect_to_zone<i_factory>(
    "node_b",
    transport,
    input_factory);
```

### Peer Zone Lifetime

Because peer root zones are not in a parent–child relationship, each stays alive as long as local objects (stubs) exist within it or remote peers hold references to those objects. When all references are released the zone becomes eligible for cleanup.

Typical peer scenarios:

- **Peer-to-peer RPC**: two independent processes each create their own root service and connect via TCP
- **Distributed service meshes**: many nodes interconnected, each managing its own object lifetimes independently
- **In-process testing**: two root zones connected via SPSC, exercising network-style RPC without opening a socket

## 3. Multi-Hop Routing

A **transport** links exactly two adjacent zones. In the C++ implementation, the
underlying mechanism may be a physical network connection, a shared-memory
queue between processes, or a direct software call between in-process zones.

When a call must cross more than one transport — for example from Zone A through Zone B to Zone C — Zone B acts as an intermediary, forwarding traffic between its two transport legs transparently. This works equally whether the zones are on the same machine or spread across the network.

```
Zone A ──[transport]──► Zone B ──[transport]──► Zone C
```

From the caller's perspective the path is invisible: objects in Zone A call objects in Zone C as if they were directly connected. Intermediary routing is handled automatically by Canopy.

**For routing implementation details**, see [Transports and Passthroughs](06-transports-and-passthroughs.md).

## 4. Fork Patterns

### Simple Fork

```
Zone N
    │
    └── fork() ──► Zone N+1 (copy of Zone N)
```

### Y-Topology Fork

```
         Zone 1 (Origin)
         /    \
        /      \
    Zone 2   Zone 3 (forked from Zone 1)
              \
               \
              Zone 4 (forked from Zone 3)
```

### Implementation Note

```idl
interface i_example
{
    error_code create_fork_and_return_object(
        [in] rpc::shared_ptr<i_example> zone_factory,
        [in] const std::vector<uint64_t>& fork_zone_ids,
        [out] rpc::shared_ptr<i_example>& object_from_forked_zone);

    error_code create_y_topology_fork(
        [in] rpc::shared_ptr<i_example> factory_zone,
        [in] const std::vector<uint64_t>& fork_zone_ids);
};
```

## 5. Caching Across Zones

### Object Caching Pattern

Objects created in one zone can be cached and handed to any other zone that has a route to it:

```cpp
// Cache an object so that other zones can retrieve it later
error_code cache_object(
    [in] const std::vector<uint64_t>& zone_ids)
{
    // Create the object locally
    cached_object_ = create_object();

    // Other zones can now retrieve it via retrieve_cached_object()
    CO_RETURN rpc::error::OK();
}

error_code retrieve_cached_object(
    [out] rpc::shared_ptr<i_example>& cached_object)
{
    cached_object = cached_object_;
    CO_RETURN rpc::error::OK();
}
```

## 6. Common Patterns

### Service Discovery

```cpp
// Look up service by name in parent zone
error_code look_up_app(const std::string& name,
                       [out] rpc::shared_ptr<i_example>& app)
{
    auto it = cached_apps_.find(name);
    if (it != cached_apps_.end())
    {
        app = it->second;
        CO_RETURN rpc::error::OK();
    }
    CO_RETURN rpc::error::NOT_FOUND;
}

// Register service in parent zone
error_code set_app(const std::string& name,
                   [in] const rpc::shared_ptr<i_example>& app)
{
    cached_apps_[name] = app;
    CO_RETURN rpc::error::OK();
}
```

### Zone Factory

```cpp
class zone_factory : public i_zone_factory
{
    std::atomic<uint64_t> next_zone_id_{100};

    error_code create_zone([out] rpc::shared_ptr<i_zone>& new_zone,
                           uint64_t parent_zone_id)
    {
        auto zone_id = ++next_zone_id_;
        new_zone = create_zone_instance(zone_id, parent_zone_id);
        CO_RETURN rpc::error::OK();
    }
};
```

## 7. Best Practices

1. **Plan zone hierarchy** before implementation
2. **Use consistent ID generation** to avoid collisions — IDs must be unique across the whole mesh
3. **Keep per-node hierarchies shallow** for better performance; certain transports may add enveloping overhead for each hop
4. **Minimise pass-through hops** - each intermediate zone adds latency and reference counting overhead
5. **Prefer peer root zones** for independent nodes rather than forcing an artificial parent–child relationship across machines
6. **Use templates for test hierarchies** - enables parameterized testing

The fork patterns above are conceptual topology examples. They are not a
statement that every transport or every language implementation currently
supports the same fork setup surface.

## 8. Code References

**Hierarchical Transport Pattern**:
- `documents/transports/hierarchical.md` - Circular dependency pattern details
- `documents/transports/local.md` - Local transport implementation
- `documents/transports/sgx.md` - SGX enclave hierarchies

**Implementation**:
- `c++/rpc/include/rpc/internal/service.h` - `rpc::root_service`, `rpc::child_service`
- `c++/transports/local/include/local/child_transport.h` - Child transport
- `c++/transports/local/include/local/parent_transport.h` - Parent transport

## 9. Next Steps

- [Zones](02-zones.md) - Zone fundamentals
- [Services](03-services.md) - Service and child_service details
- [Memory Management](04-memory-management.md) - Reference counting across hierarchies
- [Transports and Passthroughs](06-transports-and-passthroughs.md) - Multi-hop routing
- [../10-examples.md](../10-examples.md) - Working examples
- [../09-api-reference.md](../09-api-reference.md) - Service and transport APIs
