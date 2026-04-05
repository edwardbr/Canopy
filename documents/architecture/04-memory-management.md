<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Memory Management

Canopy's memory management is built entirely on **smart pointers**. There is no separate garbage collector, no manual memory management—smart pointers ARE the memory management system. Understanding this is fundamental to working with Canopy.  Internally there are reference counts for passthroughs and stubs, however users of this library only need to worry about the shared_ptr and optimistic_ptr.

## Core Principle

**Everything in Canopy is kept alive by `shared_ptr` references**:
- Objects **in** zones (local objects tracked by stubs)
- Objects **from** zones (outbound proxies to remote objects)
- Objects **through** zones (passthroughs routing between zones)

**Note `optimistic_ptr` will not keep a zone alive it will only keep any transports and passthroughs alive to objects kept alive by other shared_ptrs, once those objects are released the passthroughs and transports will be released when needed**:

When all three counts reach zero, the zone dies.

## Smart Pointer Foundation

Canopy provides three smart pointer types, each with distinct lifetime semantics, only works with interfaces defined in the idl:

```cpp
rpc::shared_ptr<T>      // remote and local RAII ownership - object dies when refs = 0
rpc::weak_ptr<T>        // Non-owning - maintains a reference to a the local proxy and not to the remote object, its use is therefore limited
rpc::optimistic_ptr<T>  // Non-RAII callable weak pointer- for independent lifetimes, useful also for callbacks and to avoid circular references.
```

**Critical Rule**: Never mix `rpc::shared_ptr` with `std::shared_ptr`. 

### rpc::shared_ptr<T>

Thread-safe reference-counted smart pointer for remote objects with RAII semantics.

**Characteristics**:
- Custom control block with shared, weak, and optimistic counts
- Requires `casting_interface` inheritance
- Compatible STL API with RPC-specific extensions
- Keeps remote object **AND** transport chain alive
- Object destroyed when shared count reaches zero

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

Non-owning reference to an `rpc::shared_ptr`-managed object.

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

**Use Case**: Breaking circular references in complex hierarchies.

```cpp
class parent_node
{
    std::vector<rpc::shared_ptr<child_node>> children_;  // Owns children
};

class child_node
{
    rpc::weak_ptr<parent_node> parent_;  // Doesn't own parent - breaks cycle
};
```

### rpc::optimistic_ptr<T>

Non-RAII pointer for references to objects with **independent lifetimes**.

**Key Difference from shared_ptr**:
- `rpc::shared_ptr`: RAII semantics - object dies when last shared_ptr is released
- `rpc::optimistic_ptr`: Non-RAII semantics - assumes object has its own lifetime (e.g., database connection, singleton service)

**Important**: `rpc::optimistic_ptr` cannot be created directly. It can only be obtained from:
1. Another `rpc::optimistic_ptr` (copy)
2. `rpc::shared_ptr` via implicit conversion or `.to_optimistic()`

**Error Behavior**:

| Pointer Type | Object Gone Error | Meaning |
|--------------|-------------------|---------|
| `rpc::shared_ptr` | `OBJECT_NOT_FOUND` | Serious - reference was held but object destroyed unexpectedly |
| `rpc::optimistic_ptr` | `OBJECT_GONE` | Expected - object with independent lifetime is gone |

**Use Cases**:

1. **Long-lived services** (databases, message queues)
2. **Callback patterns** - Object A creates Object B and needs callbacks without circular dependency
3. **Preventing circular dependencies in distributed systems**
4. **Objects managed by external lifetime managers**

**Example: Callback Pattern Without Circular Dependency**

```cpp
// Zone 1: Parent (creates child)
//   └── shared_ptr<Child> (ownership - keeps child alive)
//
// Zone 2: Child (created by parent)
//   └── optimistic_ptr<Parent> (callbacks only - no ownership)

class parent_service : public i_parent
{
    rpc::shared_ptr<child_service> child_;  // Ownership

public:
    CORO_TASK(error_code) create_child()
    {
        // Create child in another zone
        rpc::shared_ptr<child_service> new_child;
        auto error = CO_AWAIT factory_->create_child(new_child);

        // Give child an optimistic pointer to us for callbacks
        // This allows child to call us without creating circular reference
        CO_AWAIT rpc::make_optimistic(
            rpc::shared_ptr<parent_service>(this),
            new_child->get_callback_target());

        child_ = new_child;  // Ownership reference
        CO_RETURN error::OK();
    }

    // Callback from child
    CORO_TASK(error_code) on_child_event(int data) override
    {
        // Handle event
        CO_RETURN error::OK();
    }
};

class child_service : public i_child
{
    rpc::optimistic_ptr<parent_service> parent_;  // For callbacks only

public:
    void set_parent_callback(rpc::optimistic_ptr<parent_service> parent)
    {
        parent_ = parent;  // No refcount - no cycle!
    }

    CORO_TASK(error_code) process()
    {
        // Notify parent (if parent is gone, returns OBJECT_GONE - expected)
        CO_AWAIT parent_->on_child_event(progress_);
        CO_RETURN error::OK();
    }
};
```

## Reference Counting Model

### Triple-Count System

Each object maintains three reference counts:

```
Shared Count (shared_count_)
├── Owned by: rpc::shared_ptr
├── Keeps: Remote object AND transport chain alive
└── Returns: OBJECT_NOT_FOUND when count exhausted

Optimistic Count (optimistic_count_)
├── Owned by: rpc::optimistic_ptr
├── Does NOT keep object alive
└── Returns: OBJECT_GONE when count exhausted

Weak Count (weak_count_)
├── Owned by: rpc::weak_ptr
├── Does NOT keep object alive
└── Used for: Breaking circular references
```

### Control Block Structure

Unlike `std::shared_ptr`, `rpc::shared_ptr` has a specialized control block:

```cpp
template<typename T>
class shared_ptr
{
    casting_interface* ptr_;          // Raw pointer
    control_block* block_;            // Custom control block
};

struct control_block
{
    std::atomic<int32_t> shared_count_;     // Strong references
    std::atomic<int32_t> weak_count_;       // Weak references
    std::atomic<int32_t> optimistic_count_; // Optimistic references
    std::atomic<bool> destroyed_;           // Destruction flag
};
```

### Reference Acquisition Flow

```
Creation:
  rpc::make_shared<impl>() → shared_count_ = 1

Add Reference (across zone):
  remote_call_add_ref() → shared_count_++

Release Reference (across zone):
  remote_call_release() → shared_count_--

Destruction:
  shared_count_ == 0 → object deleted
```

### Reference Propagation Across Zones

```
Zone 1: Client creates shared_ptr<i_calc>
  ↓
Service 1: Marshals reference to Zone 2
  ↓
Transport 1→2: Sends add_ref(object_id, zone_id)
  ↓
Transport 2: Receives add_ref, creates object_stub
  ↓
Object Stub: Increments shared_count_
  ↓
Zone 2: Object kept alive by remote reference
```

## Lifetime Management: Services, Transports, Passthroughs

### Transport Ownership Model

As documented in `service.h`, transports are owned by:

```cpp
// From c++/rpc/include/rpc/internal/service.h:
// transports owned by:
// - service proxies
// - pass through objects
// - child services to parent transports
std::unordered_map<destination_zone, std::weak_ptr<transport>> transports_;
```

Services hold **weak_ptr** (registry only), while actual ownership is distributed:

### 1. Service Proxies Hold Strong References

```cpp
class service_proxy
{
    stdex::member_ptr<transport> transport_;  // Strong reference
};
```

**Why strong?** Service proxies need the transport to route calls. As long as proxy exists, transport must remain valid.

### 2. Passthroughs Hold Strong References

```cpp
class pass_through
{
    std::shared_ptr<transport> forward_transport_;   // Strong reference to forward transport
    std::shared_ptr<transport> reverse_transport_;   // Strong reference to reverse transport
    std::shared_ptr<service> service_;               // Strong reference to intermediary service
};
```

**Why strong?** Passthroughs keep entire routing paths alive while references exist between non-adjacent zones. The `std::shared_ptr<service>` ensures the intermediary zone remains alive as long as it's routing traffic.

**Example**: Zone 1 ↔ Zone 2 ↔ Zone 3
- When Zone 1 and Zone 3 communicate through Zone 2
- Passthrough in Zone 2 holds strong references to:
  - Transport to Zone 1
  - Transport to Zone 3
  - Zone 2's service (keeps Zone 2 alive)
- Even if Zone 2 has no local objects, it stays alive as the routing intermediary

### 3. Child Services Hold Strong Reference to Parent Transport

```cpp
class child_service : public service
{
    std::shared_ptr<transport> parent_transport_;  // Strong reference
};
```

**Why strong?** Child depends on parent. Parent must outlive child.

### 4. Active Stubs May Cause Transport References

When stubs are active (calls in progress), transports may hold strong references to adjacent transports to ensure stability during the RPC call.

### Transport Lifetime Pattern

```
Zone A ──► Transport ──► Zone B
  │            ▲         │
  │ weak_ptr   │         │
  ▼            │         ▼
Service    ServiceProxy  PassThrough
           (strong)      (strong to transports + service)
```

**Transports** stay alive as long as ANY of these hold references:
- Service proxies (while proxies exist)
- Passthroughs (while routing non-adjacent zones)
- Child services (to parent transport)
- Active stubs (during RPC calls)

**Services** stay alive as long as ANY of these hold references:
- Local objects (stubs)
- Child services (to parent service via parent transport)
- Passthroughs (to intermediary service, enabling routing zones)

## Zone Death (Amnesia)

### The Three Counts

A zone dies when all three reference categories reach zero:

1. **Objects IN the zone**: Local stubs (objects living in this zone)
2. **Objects FROM the zone**: Outbound proxies (references this zone holds to remote objects)
3. **Objects THROUGH the zone**: Passthroughs (routing through this zone)

Passthroughs keep the zone alive via `std::shared_ptr<service>`. A zone can stay alive purely as a routing intermediary even with no local objects, as long as passthroughs exist routing through it.

```cpp
// Service tracks triple-count
class service
{
    std::atomic<uint64_t> inbound_stub_count_{0};      // Objects IN zone
    std::atomic<uint64_t> outbound_proxy_count_{0};    // Objects FROM zone
    std::atomic<uint64_t> passthrough_count_{0};       // Objects THROUGH zone

public:
    bool is_amnesia() const
    {
        return inbound_stub_count_ == 0
            && outbound_proxy_count_ == 0
            && passthrough_count_ == 0;
    }
};
```

### Amnesia Sequence

```
1. Triple-count reaches zero:
   - Inbound stubs count = 0 (no local objects)
   - Outbound proxies count = 0 (no remote references)
   - Passthrough objects count = 0 (no routing)

2. Service signals transport:
   service->on_amnesia(transport)

3. Transport cleanup:
   - Disconnect from remote zones
   - Release parent reference (for child zones)
   - Notify optimistic pointers (returns OBJECT_GONE)

4. Zone destruction:
   - Service destructor runs
   - Transport destructor runs
   - Zone memory released
```

### State Diagram

```
        ┌─────────────────┐
        │   ALIVE         │◄──────────────────────┐
        │   (refs > 0)    │                        │
        └────────┬────────┘                        │
                 │ all refs released               │
                 ▼                                 │
        ┌─────────────────┐                        │
        │  AMNESIA        │──► transport_down() ───┘
        │  (refs = 0)     │
        └────────┬────────┘
                 │ transport cleanup
                 ▼
        ┌─────────────────┐
        │  DELETED        │
        │  (gone forever) │
        └─────────────────┘
```

## member_ptr: Thread-Safe Transport References

Canopy uses `stdex::member_ptr` for thread-safe transport references:

```cpp
// member_ptr is a thread-safe wrapper around shared_ptr
class member_ptr<T>
{
    std::shared_ptr<T> ptr_;
    mutable std::shared_mutex mutex_;

public:
    std::shared_ptr<T> get_nullable() const
    {
        std::shared_lock lock(mutex_);
        return ptr_;
    }

    void reset(std::shared_ptr<T> ptr)
    {
        std::unique_lock lock(mutex_);
        ptr_ = std::move(ptr);
    }
};
```

### Why member_ptr?

- Safe access during concurrent RPC calls
- Safe access during object destruction
- No raw pointer lifetime issues
- Provides stack-based protection during calls

### Stack-Based Lifetime Protection

When calls cross zone boundaries, stack-based `shared_ptr` prevents transport destruction during active calls:

```cpp
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

Even if `child_service` releases its last reference to `parent_transport` during an active call, the stack-based `shared_ptr` keeps it alive until the call returns, preventing use-after-free.

## No casting between rpc::shared_ptr and std::shared_ptr

Never cast between `rpc::shared_ptr` and `std::shared_ptr`.

### Pattern 3: Break Cycles with weak_ptr

```cpp
class node
{
    rpc::optimistic_ptr<node> parent_;             // Doesn't own parent
    std::vector<rpc::shared_ptr<node>> children_;  // Owns children
};
```

### Pattern 4: Callback Without Circular Dependency

## Memory Best Practices

### Do

- Use `rpc::make_shared<T>()` for creation
- Use `rpc::shared_ptr` for all RPC references
- Use `rpc::weak_ptr` for local proxies
- Use `rpc::optimistic_ptr` for independent lifetimes, callbacks and circular dependency protection
- Let reference counting handle cleanup
- Monitor amnesia events in telemetry

### Don't

- Don't store raw pointers to RPC objects
- Don't mix with `std::shared_ptr`
- Don't manually delete RPC objects
- Don't assume synchronous destruction
- Don't hold references longer than needed
- Don't create circular dependencies with shared_ptr

## Debugging Memory Issues

### Enable Telemetry

```cpp
#ifdef CANOPY_USE_TELEMETRY
rpc::console_telemetry_service::create(telemetry, "test", "memory_test", "/tmp");
#endif
```

### Watch For

```
Warning: shared_count_ was 0 before decrement
  → Double-release or use-after-free

Warning: stub zone_id X has been released but not deregistered
  → Orphaned stub reference

## Thread Safety

All smart pointer operations are thread-safe:

- **Atomic reference counts**: `std::atomic<int32_t>` for shared/weak/optimistic counts
- **member_ptr locking**: `shared_mutex` for concurrent access to transports
- **Control block operations**: Lock-free atomic operations

Multiple threads can:
- Hold `shared_ptr` to same object
- Increment/decrement reference counts
- Route through same passthrough
- Access same transport (via member_ptr)

## Code References

**Smart Pointers**:
- `c++/rpc/include/rpc/shared_ptr.h` - shared_ptr implementation
- `c++/rpc/include/rpc/weak_ptr.h` - weak_ptr implementation
- `c++/rpc/include/rpc/optimistic_ptr.h` - optimistic_ptr implementation

**member_ptr**:
- `c++/rpc/include/rpc/internal/member_ptr.h` - Thread-safe wrapper

**Reference Counting**:
- `c++/rpc/src/object_stub.cpp` - Server-side refcounting
- `c++/rpc/src/object_proxy.cpp` - Client-side refcounting

## Next Steps

- [Proxies and Stubs](05-proxies-and-stubs.md) - How references are marshalled
- [Transports and Passthroughs](06-transports-and-passthroughs.md) - How transports are kept alive
- [Zone Hierarchies](07-zone-hierarchies.md) - Hierarchical lifetime patterns
