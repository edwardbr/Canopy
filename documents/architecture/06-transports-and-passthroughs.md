<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Transports and Passthroughs

The Plumbing Between Services

## Overview

Canopy uses two complementary mechanisms for inter-zone communication:

- **Transports**: Direct connections between adjacent zones (e.g., Zone 1 ↔ Zone 2)
- **Passthroughs**: Connections through intermediary zones (e.g., Zone 3 ↔ Zone 4 through Zone 2)

Together, they enable seamless communication across complex zone hierarchies without requiring every zone to connect directly to every other zone.

### Key Principles

1. **Services hold weak references to transports** - Registry only, doesn't keep transports alive
2. **Service proxies hold strong references to transports** - Keep transport alive while proxy exists
3. **Passthroughs hold strong references to transports and service** - Keep routing plumbing and intermediary zone alive, enabling zones to function purely as routing hubs
4. **Child services hold strong reference to parent transport** - Parent must outlive child
5. **Active stubs may hold transport references** - Transports can reference adjacent transports during calls
6. **Transport ref counts track adjacent zones** - External proxy/stub relationships
7. **One Passthrough exists per zone id pair** - Internal proxy/stub relationships

## Part 1: Transports

Transports provide the communication channels between adjacent zones. Each transport implements a specific communication mechanism while adhering to a common interface that enables the Canopy framework to route messages, manage connections, and handle lifecycle events uniformly across different transport types.

### Transport Architecture

All transports inherit from `rpc::transport`, which defines the interface for:

- **Connection establishment** via the `connect()` virtual method
- **Message sending** with `send()` for request-response and `post()` for fire-and-forget operations
- **Reference counting** through `add_ref()` and `release()` for distributed object lifecycle management
- **Interface queries** using `try_cast()` to support dynamic interface resolution

### Transport Ownership Model

As documented in `rpc/include/rpc/internal/service.h`, transports have a distributed ownership model:

```cpp
// From service.h:
// transports owned by:
// - service proxies
// - pass through objects
// - child services to parent transports
std::unordered_map<destination_zone, std::weak_ptr<transport>> transports_;
```

**Strong References (Keep Transport Alive)**:
1. **Service Proxies** - Each proxy holds `stdex::member_ptr<transport>` to route calls
2. **Passthroughs** - Hold `member_ptr` to both forward and reverse transports
3. **Child Services** - Hold `std::shared_ptr<transport>` to parent transport
4. **Active Stubs** - Transports may hold references to adjacent transports during RPC calls

**Weak References (Registry Only)**:
- **Services** - Hold `std::weak_ptr<transport>` for lookup, doesn't keep transport alive

**Lifetime Rule**: Transport stays alive as long as ANY strong reference holder exists.

**Example Flow**:
```
Client creates proxy to remote object
  ↓
Service proxy created with member_ptr<transport>
  ↓
Transport kept alive by service proxy
  ↓
Proxy destroyed (goes out of scope)
  ↓
Service proxy releases member_ptr<transport>
  ↓
If no other strong references exist, transport destroyed
```

### Core Transport Responsibilities

The base `transport` class manages:

1. **Zone identity** - Each transport connects two zones and knows its local zone ID and the adjacent zone ID
2. **Destination routing** - Maintains handlers for zone pairs to route incoming messages to the correct service
3. **Pass-through routing** - For multi-hop zone hierarchies, tracks which transports can reach which destinations
5. **Connection status** - Enum values: `CONNECTING`, `CONNECTED`, `DISCONNECTING`, `DISCONNECTED`

### Transport Status

```cpp
enum class transport_status
{
    CONNECTING,   // Initial state, establishing connection
    CONNECTED,    // Fully operational
    DISCONNECTING, // Beginning to shut down a close signal is being sent or recieved, all active outgoing calls are canceled only incoming releases are processed, all out parameters of incoming calls are not marshalled
    DISCONNECTED  // Terminal state close signal has been acknowleged, or there is a terminal failure, no further traffic allowed
};
```

Status transitions are managed through the `set_status()` method, which can be overridden by transport implementations to handle status change events (e.g., propagating disconnection notifications).

### Inbound Message Processing

Transports implement the `i_marshaller` interface for outbound communication—sending requests to the adjacent zone.

These base class methods are called by the specific transport implementation when invoked by messages from the adjacent zone or client to process the request. These methods call the `i_marshaller` interface of either a pass-through object to another transport (for multi-hop routing) or the local service (for direct delivery).

- `outbound/inbound_send()` - Request-response RPC call; returns a response to the caller
- `outbound/inbound_post()` - Fire-and-forget notification; no response expected
- `outbound/inbound_try_cast()` - Interface query to obtain a different interface on an object
- `outbound/inbound_add_ref()` - Increments reference count on a remote object
- `outbound/inbound_release()` - Decrements reference count; may trigger object destruction
- `outbound/inbound_object_released()` - Notifies the transport that an object has been released
- `outbound/inbound_transport_down()` - Notifies the transport that the adjacent transport has failed

Each `inbound_*` method handle calls from the adjacent zone the derived transport implementation calls this function once it has the message.  inbound_* methods handle forward the calls to the service or pass-through.
Each `outbound_*` method is overridden by the derived transport classes and sends the message to the adjacent zone.  These methods are private and are called by the base implementation on receiving an i_marshaller call.

### Transport Types

Canopy provides several transport implementations, each optimized for different use cases:

| Transport | Purpose | Requirements |
|-----------|---------|--------------|
| [Local](../transports/local.md) | In-process parent-child zone communication | None |
| [TCP](../transports/tcp.md) | Network communication between machines | Coroutines |
| [SPSC](../transports/spsc.md) | Lock-free inter-process communication | Coroutines |
| [SGX](../transports/sgx.md) | Secure enclave communication | SGX SDK |
| [Custom](../transports/custom.md) | User-defined transport implementations | Depends on implementation |

Choose the transport that matches your use case: Local for in-process testing, TCP for network communication, SPSC for high-performance IPC, or SGX for secure computation.

### Connection Handshake

Most transports use a two-phase handshake:

**Client sends** `init_client_channel_send`:
```idl
struct init_channel_send
{
    uint64_t caller_zone_id;
    uint64_t caller_object_id;
    uint64_t destination_zone_id;
    // TCP includes: adjacent_zone_id
};
```

**Server responds** `init_client_channel_response`:
```idl
struct init_channel_response
{
    int32_t err_code;
    uint64_t destination_zone_id;
    uint64_t destination_object_id;
    uint64_t caller_zone_id;
};
```

This handshake establishes zone identity, object routing, and confirms the connection is ready for bidirectional communication.

### Transport Reference Counting

Transports track references between **adjacent zones only**:

```cpp
class transport
{
    std::atomic<uint64_t> shared_count_{0};      // Shared references
    std::atomic<uint64_t> optimistic_count_{0};  // Optimistic references

public:
    // Incremented by: Normal add_ref (options=0)
    // Decremented by: Normal release (options=0)
    void add_ref(rpc::object obj, uint64_t count = 1);
    void release(rpc::object obj, uint64_t count = 1);
};
```

**Critical**: Relay operations (options=3) do NOT affect transport ref counts. See Part 2: Passthroughs for details.

### Transport Lifecycle Management

```cpp
// Services hold weak references
class service
{
    std::weak_ptr<transport> transport_;
};

// Passthroughs hold strong references
class pass_through
{
    std::shared_ptr<transport> forward_transport_;
    std::shared_ptr<transport> reverse_transport_;
};
```

#### Lifetime Patterns

**Peer-to-Peer Arrangements** (e.g., TCP transport between standalone services):
- Service manages lifetimes of all objects within its zone
- Service holds **weak references** to transports (registry only)
- **Service proxies** hold **strong references** to transports
- **Active stubs** may cause transports to hold references to adjacent transports
- Transport destroyed when all strong references released

**Hierarchical Arrangements** (e.g., local transport with `child_service`):
- Parent-side transport is last to survive
- **Child service** holds **strong reference** to parent transport
- **Service proxies** hold **strong references** to transports
- **Passthroughs** hold **strong references** to transports AND intermediary service
- **Active stubs** may cause transports to maintain references during calls
- Passthroughs keep intermediary zones alive as routing hubs
- Ensures parent-side references remain valid during child zone shutdown
- Maintains zone hierarchy integrity during teardown

#### Transport Cleanup Requirements

**Critical Rule: Transport Disconnection Before Service Destruction**

By the time `service::~service()` is called, all transports must be:
1. **Disconnected** - Status set to `transport_status::DISCONNECTED`
2. **Unregistered** - Removed from service's `transports_` registry via `remove_transport()`

This ensures clean shutdown and prevents:
- Active calls through dead transports
- Circular reference leaks
- Use-after-free in routing logic

**Exception for child_service:**
- The parent_transport is intentionally kept alive DURING `child_service::~child_service()`
- The destructor triggers disconnection by calling `parent_transport->set_status(DISCONNECTED)`
- This propagates to the parent zone's child_transport
- The circular reference is broken safely via the disconnection protocol
- Stack-based shared_ptr protection prevents use-after-free during active calls

**Enforcement:**
- Service proxies must release transport references before service destructs
- Passthroughs must release transport references when ref counts reach zero
- For child_service, parent_transport cleanup is automatic via disconnection protocol

See `03-services.md` for service lifecycle details and `04-memory-management.md` for reference counting patterns.

### Zone Hierarchy and Transport Attachment

Canopy zones form hierarchical structures. Each zone can only create zones directly adjacent to itself:

```
Zone 1 (Root)
├── Zone 2 (created by Zone 1)
│   └── Zone 4 (created by Zone 2)
└── Zone 3 (created by Zone 1)
    └── Zone 5 (created by Zone 3)
```

Rules:
- Zone 1 can directly create Zone 2 and Zone 3
- Zone 2 can directly create Zone 4 (its child)
- Zone 2 cannot directly create Zone 3 (sibling) or Zone 5 (grandchild)

Attach transports to service:
```cpp
// Attach transport to service
service_->add_transport(destination_zone{2}, transport_);

// Get transport for zone
auto transport = service_->get_transport(destination_zone{2});
```

See `02-zones.md` for comprehensive zone architecture details.

### connect_to_zone Signature

The `connect_to_zone` function creates a connection between zones:

```cpp
template<class in_param_type, class out_param_type>
CORO_TASK(int)
connect_to_zone(const char* name,
    std::shared_ptr<transport> child_transport,
    const rpc::shared_ptr<in_param_type>& input_interface,
    rpc::shared_ptr<out_param_type>& output_interface);
```

Parameters:
- `name` - Unique name for the zone connection
- `child_transport` - Transport connecting to the child zone
- `input_interface` - Interface the child can call back to parent (owned by parent)
- `output_interface` - Interface returned from child (owned by child)

### Child Transport Entry Point

When creating a child zone, use `child_transport->set_child_entry_point<i_example_parent, i_example_child>(...)` to provide the callback for initializing the child zone. This callback is invoked when the parent connects to the child zone.

## Part 2: Passthroughs

Passthroughs enable transparent communication between non-adjacent zones by routing through an intermediary zone.

### Purpose

When Zone A wants to share a reference with Zone C, but they're not directly connected and both connect through Zone B, a **passthrough** in Zone B routes the communication.

### Topology Example

```
Zone 1 ←→ Zone 2
           ↓
        ┌──┴──┐
     Zone 3  Zone 4
```

- Zone 1 and Zone 2 are adjacent (direct connection)
- Zone 2 and Zone 3 are adjacent (hierarchical: Zone 3 is child of Zone 2)
- Zone 2 and Zone 4 are adjacent (hierarchical: Zone 4 is child of Zone 2)
- Zone 3 and Zone 4 are NOT adjacent (siblings)
- **Zone 1 and Zone 3** communicate through Zone 2's passthrough
- **Zone 3 and Zone 4** communicate through Zone 2's passthrough

### Passthrough Structure

```cpp
class pass_through : public i_marshaller
{
    std::shared_ptr<transport> forward_;   // To forward destination
    std::shared_ptr<transport> reverse_;   // To reverse destination
    std::shared_ptr<service> service_;     // Keeps intermediary service alive

    destination_zone forward_destination_;  // Zone 3
    destination_zone reverse_destination_;  // Zone 4

    std::atomic<uint64_t> shared_count_{0};      // Shared references
    std::atomic<uint64_t> optimistic_count_{0};  // Optimistic references
};
```

The `std::shared_ptr<service>` keeps the intermediary zone (Zone 2) alive as long as the passthrough exists. This allows Zone 2 to function purely as a routing intermediary—even if it has no local objects, it remains alive while routing traffic between Zone 1 ↔ Zone 3 or Zone 3 ↔ Zone 4. When all passthroughs are destroyed, Zone 2 can die (if no other references exist).

**Key Design Points**:
1. Passthroughs hold strong references (`std::shared_ptr`) to both transports - keeps the communication paths alive
2. Passthroughs hold strong reference to the intermediary service (`std::shared_ptr<service>`) - keeps the intermediary zone alive
3. This allows zones to function purely as routing hubs, staying alive as long as they're routing traffic between other zones

### Relay Operation (options=3)

#### What is options=3?

In `rpc_types.idl`, `add_ref_options` value 3 is the bitwise OR of:
```cpp
build_destination_route = 0x01  // Bit 0
build_caller_route      = 0x02  // Bit 1
// options = 3: Both bits set
```

This signals a **relay operation**: "Don't create a reference here, route it somewhere else."

#### Relay Sequence

**Scenario**: Zone 1 has a reference to an object in Zone 3, wants to share it with Zone 4

**Step 1: Relay Instruction**
```
Zone 1 → Zone 2: add_ref(object, options=3, caller=Zone4, destination=Zone3)
```
This is a **control message**, NOT a reference operation on Zone 1↔Zone 2 transport.

**Step 2: Zone 2 Processes Relay**

Check: Does passthrough exist for Zone 3 ↔ Zone 4?

**Case A: No Passthrough Exists**
```cpp
// Create new passthrough
auto passthrough = std::make_shared<pass_through>(
    transport_to_zone3,    // forward
    transport_to_zone4,    // reverse
    service,               // Zone 2's service
    Zone{3},              // forward_destination
    Zone{4});             // reverse_destination

passthrough->shared_count_ = 1;  // Initial reference
```

**Case B: Passthrough Already Exists**
```cpp
// Increment existing passthrough
passthrough->shared_count_++;  // 1→2, 2→3, etc.
```

**Step 3: Establish Routes**
```
Zone 2 → Zone 3: add_ref(object, options=build_destination_route)
Zone 2 → Zone 4: add_ref(object, options=build_caller_route)
```

**Step 4: Communication Flows Through Passthrough**
```
Zone 4 → object method call
  ↓ (through Zone 4's transport to Zone 2)
Zone 2's passthrough routes to Zone 3
  ↓ (through passthrough's forward_ transport)
Zone 3 → object in Zone 3 receives call
```

### Passthrough Lifecycle

#### Creation

Passthrough created when:
1. Relay add_ref (options=3) arrives
2. No existing passthrough for that destination/caller pair
3. Initial `shared_count=1`

#### Reference Counting

**Shared References:**
- Normal `rpc::shared_ptr` references
- Tracked in `shared_count_`
- Incremented by: relay add_ref (options=3)
- Decremented by: relay release (options=3)

**Optimistic References:**
- `rpc::optimistic_ptr` references
- Tracked in `optimistic_count_`
- Incremented by: relay add_ref with optimistic flag
- Decremented by: relay release with optimistic flag

#### Deletion

Passthrough deleted when:
1. `shared_count_ == 0` AND `optimistic_count_ == 0`
2. No more references exist between the zones
3. Passthrough cleans itself up automatically

#### Self-Deletion Logic

```cpp
void pass_through::release(...) {
    uint64_t prev = shared_count_.fetch_sub(1, std::memory_order_acq_rel);

    if (prev == 1 && optimistic_count_.load() == 0) {
        // Last reference - schedule self-deletion
        delete_self();
    }
}
```

### Routing Logic

#### Forward Direction (Reverse → Forward)

When call comes from reverse destination (e.g., Zone 4):
```cpp
if (caller == reverse_destination_) {
    // Route to forward destination (Zone 3)
    return forward_->send(...);
}
```

#### Reverse Direction (Forward → Reverse)

When call comes from forward destination (e.g., Zone 3):
```cpp
if (caller == forward_destination_) {
    // Route to reverse destination (Zone 4)
    return reverse_->send(...);
}
```

The passthrough automatically determines the correct routing direction based on which zone the call originated from.

### Multi-Hop Routing

Passthroughs can chain for multi-hop routing:

```
Zone 1 ↔ Zone 2 ↔ Zone 3 ↔ Zone 4
```

If Zone 1 wants to reach Zone 4:
- Zone 2 has passthrough: Zone 1 ↔ Zone 3
- Zone 3 has passthrough: Zone 2 ↔ Zone 4
- Calls route: Zone 1 → Zone 2 (passthrough) → Zone 3 (passthrough) → Zone 4

Each passthrough maintains its own ref counts.

For complex multi-level hierarchies, messages may route through multiple intermediary zones. This is an emergent behavior controlled at a strategic level—the library handles routing automatically.

## Part 3: How They Work Together

### Transport vs Passthrough Ref Counts

Transports and passthroughs maintain separate reference counts for different purposes:

#### Transport Ref Counts
- Track proxies/stubs between **adjacent zones**
- Direct connections: Zone 1 ↔ Zone 2
- Incremented by: Normal add_ref (options=0)
- Decremented by: Normal release (options=0)

#### Passthrough Ref Counts
- Track references between **non-adjacent zones**
- Routed connections: Zone 3 ↔ Zone 4 (through Zone 2)
- Incremented by: Relay add_ref (options=3)
- Decremented by: Relay release (options=3)

#### Why Relay Operations Don't Affect Transport Counts

```
Zone 1 → Zone 2: add_ref(options=3)
```

This does NOT represent "Zone 1 holds a reference through Zone 2". It represents "Zone 1 is instructing Zone 2 to establish a passthrough between Zone 4 and Zone 3".

The reference exists in the **passthrough**, not on the Zone 1↔Zone 2 transport.

### Ownership and Lifetime Coordination

```
Service (weak_ptr)
    ↓
Transport (shared_ptr) ←─── Passthrough (shared_ptr)
    ↓                            ↓
Adjacent Zone              Non-Adjacent Zone
```

**Key Relationships:**

1. **Service → Transport**: Weak reference
   - Services don't keep transports alive
   - Transports destroyed when no strong references remain

2. **Passthrough → Transport**: Strong reference
   - Passthroughs keep both forward and reverse transports alive
   - Ensures routing plumbing remains valid while references exist

3. **Transport → Object**: Reference counting
   - Tracks proxy/stub relationships for adjacent zones
   - Managed through normal add_ref/release (options=0)

4. **Passthrough → Object**: Reference counting
   - Tracks proxy/stub relationships for non-adjacent zones
   - Managed through relay add_ref/release (options=3)

### Telemetry Tracking

#### Transport Events
```javascript
transport_outbound_add_ref  (options != 3)  // Increment transport ref
transport_inbound_add_ref   (options != 3)  // Increment transport ref
transport_outbound_release  (options != 3)  // Decrement transport ref
transport_inbound_release   (options != 3)  // Decrement transport ref
```

#### Passthrough Events
```javascript
pass_through_creation       // New passthrough created
pass_through_add_ref        // Passthrough ref count incremented
pass_through_release        // Passthrough ref count decremented
pass_through_deletion       // Passthrough deleted (ref count = 0)
```

#### Relay Activity (options=3)
```javascript
// Transport events with options=3 are relay operations
// They trigger passthrough ref changes, not transport ref changes
if (options === 3) {
    pulseRelayActivity();  // Visual feedback only
    return;  // Don't update transport ref counts
}
```

### Thread Safety

Both transports and passthroughs are thread-safe:
- `std::atomic` for ref counts
- Transport operations are thread-safe
- Multiple threads can route through same transport/passthrough concurrently

### Performance Considerations

**Transport Overhead:**
- Serialization at zone boundaries
- Network/IPC latency (TCP, SPSC)
- Minimal overhead for local transport

**Passthrough Overhead:**
- Additional routing hop through intermediary zone
- No extra serialization (already serialized for zone boundaries)
- Ref count atomic operations (minimal overhead)

**Benefits:**
- Transparent multi-zone communication
- No need for every zone to connect to every other zone
- Simplified topology management
- Automatic routing through complex hierarchies

## Debugging Transports and Passthroughs

### Telemetry Visualization

Enable telemetry to see the complete picture:
- Zones shown as boxes with zone IDs
- Transports shown as connections between adjacent zones
- Passthroughs shown as purple boxes in intermediary zones
- Ref counts shown: `S<shared> O<optimistic>`
- Forwarding routes visualized with arrows

### Common Issues

**Problem**: Transport never deleted (leak)
- **Cause**: Service holds strong reference instead of weak
- **Fix**: Ensure service uses `std::weak_ptr<transport>`

**Problem**: Passthrough never deleted (leak)
- **Cause**: Mismatched relay add_ref/release
- **Fix**: Verify relay operations are balanced (options=3)

**Problem**: Passthrough ref count negative
- **Cause**: Release without corresponding add_ref
- **Fix**: Check relay operation flow, ensure options=3 on both

**Problem**: Object not found through passthrough
- **Cause**: Passthrough routing logic issue
- **Fix**: Verify forward/reverse destinations match caller/destination zones

**Problem**: Zone destroyed while transport active
- **Cause**: Circular dependency or missing stack protection
- **Fix**: See hierarchical transport pattern in `../transports/hierarchical.md`

## Code References

**Transport Implementation:**
- `rpc/include/rpc/internal/transport.h` - Transport base class
- `rpc/src/transport.cpp` - Transport implementation

**Passthrough Implementation:**
- `rpc/include/rpc/internal/pass_through.h` - Passthrough class definition
- `rpc/src/pass_through.cpp` - Passthrough implementation

**Transport Creation:**
- `transport::create_pass_through()` - Passthrough factory method

**Telemetry:**
- `on_transport_created()` - Transport creation event
- `on_transport_status_changed()` - Status change event
- `on_pass_through_creation()` - Passthrough creation event
- `on_pass_through_add_ref()` - Passthrough add reference event
- `on_pass_through_release()` - Passthrough release event
- `on_pass_through_deletion()` - Passthrough deletion event

## See Also

- [Overview](01-overview.md) - Canopy architecture overview
- [Zones](02-zones.md) - Zone architecture and hierarchies
- [Services](03-services.md) - Service lifecycle and responsibilities
- [Memory Management](04-memory-management.md) - Reference counting patterns
- [Proxies and Stubs](05-proxies-and-stubs.md) - Object proxies and stubs
- [Hierarchical Transports](../transports/hierarchical.md) - Parent/child transport pattern
- [Local Transport](../transports/local.md) - In-process transport
- [TCP Transport](../transports/tcp.md) - Network transport
- [SPSC Transport](../transports/spsc.md) - Lock-free IPC transport
- [SGX Transport](../transports/sgx.md) - Secure enclave transport
