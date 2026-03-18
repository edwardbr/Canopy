<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Best Practices

Guidelines for effective Canopy usage, common pitfalls, and troubleshooting tips.

## 1. Interface Design

### Do

- **Use descriptive names**: `i_payment_processor` not `pay`
- **Keep interfaces focused**: Single responsibility principle
- **Use meaningful parameters**: `[out]` for large return values
- **Document methods**: Add `[description="..."]` attributes
- **Version with namespaces**: Use `[inline] namespace v1` for evolution

### Don't

- **Avoid giant interfaces**: Split into focused interfaces
- **Don't expose internal types**: Use IDs and handles
- **Avoid out parameters for primitives**: Return directly
- **Don't change interface IDs**: Breaks compatibility

### Example

```idl
// Good: Focused interface
interface i_user_repository
{
    error_code get_user(int user_id, [out] user& result);
    error_code create_user([in] const user& data, [out] int& user_id);
    error_code delete_user(int user_id);
};

// Avoid: God interface
interface i_user_service_data_access_repository_cache_auth_session_logging
{
    // Too many responsibilities
};
```

## 2. Memory Management

### Do

- **Use `rpc::make_shared<T>()`** for creation
- **Let reference counting handle cleanup**: Don't manually delete
- **Use `rpc::weak_ptr`** for breaking cycles
- **Use `rpc::optimistic_ptr`** for objects with independent lifetimes (databases, services)
- **Prefer passing by value** for small types

### Don't

- **Never mix `rpc::shared_ptr` with `std::shared_ptr`**
- **Don't store raw pointers** to RPC objects
- **Don't manually call delete** on RPC objects
- **Don't assume synchronous destruction**
- **Don't hold references longer than needed**
- **Don't use optimistic_ptr for objects that should die with last reference**

### Example

```cpp
// Good
auto service = rpc::make_shared<my_service>();
auto error = CO_AWAIT service->process(data);

// Bad: Raw pointer
auto* raw = new my_service();
auto error = CO_AWAIT raw->process(data);  // No refcount!

// Bad: Mixing types
std::shared_ptr<base> std_ptr = std::make_shared<derived>();
rpc::shared_ptr<base> rpc_ptr(std_ptr.get());  // WRONG!
```

## 3. Error Handling

### Do

- **Always check return values** from RPC calls
- **Handle OBJECT_GONE** - expected expiry for `rpc::optimistic_ptr` targets; the object's lifetime is independent of the caller
- **Use logging** for error context
- **Propagate errors** at appropriate layers
- **Distinguish between errors and exceptions**

### Don't

- **Don't ignore errors**
- **Don't swallow errors** without logging
- **Don't use exceptions** for normal error flow
- **Don't assume success** - check explicitly

### Example

```cpp
// Good
auto error = CO_AWAIT calculator_->divide(a, b, result);

if (error == rpc::error::INVALID_DATA())
{
    RPC_ERROR("Division by zero: {} / {}", a, b);
    return error;
}

if (error != rpc::error::OK())
{
    RPC_ERROR("Unexpected error: {}", static_cast<int>(error));
    return error;
}

// Bad
CO_AWAIT calculator_->divide(a, b, result);  // Ignore result!
```

## 4. Coroutine Usage

### Do

- **Keep coroutines short and focused**
- **Use `CO_AWAIT` for async operations**
- **Handle errors at each await point**
- **Use blocking mode for debugging**
- **Spawn background tasks** for parallel operations

### Don't

- **Don't block in coroutines** (use CO_AWAIT instead)
- **Don't mix blocking and coroutine code**
- **Don't create deep call chains** - harder to debug
- **Don't forget to spawn** coroutine tasks

### Example

```cpp
// Good
CORO_TASK(error_code) process_item(int item)
{
    int result;
    auto error = CO_AWAIT calculator_->process(item, result);
    if (error != rpc::error::OK())
        CO_RETURN error;

    CO_RETURN rpc::error::OK();
}

// Spawn task — capture by value, not by reference, since the coroutine
// may outlive the current stack frame
scheduler_->spawn([item = 42]() -> CORO_TASK(void)
{
    CO_AWAIT process_item(item);
    CO_RETURN;
}());

// Bad: Blocking call in coroutine
CORO_TASK(error_code) bad_example(int item)
{
    int result;
    auto error = calculator_->blocking_process(item, result);  // Blocks thread!
    CO_RETURN error;
}
```

## 5. Transport Selection

### Do

- **Use Local transport** for unit tests
- **Use TCP transport** for network communication
- **Use SPSC transport** for high-performance IPC
- **Use SGX transport** for secure computation

### Don't

- **Don't use TCP** for in-process communication
- **Don't use coroutine transports** without coroutines
- **Don't use SGX** unless you need hardware security
- **Don't assume transport is connected**

### Selection Guide

| Scenario | Transport | Reason |
|----------|-----------|--------|
| Unit test | Local | No overhead |
| Microservices | TCP | Network transparent |
| Same machine, high perf | SPSC | Lock-free, fast |
| Secure computation | SGX | Hardware isolation |

## 6. Zone Hierarchy Design

### Do

- **Plan hierarchy** before implementation
- **Use consistent ID generation**
- **Keep hierarchies shallow**
- **Keep peer root zones independent** — do not force an artificial parent–child relationship between nodes that should be equals
- **Consider pass-through cost** for multi-hop routing

### Don't

- **Don't create deep hierarchies** (performance cost)
- **Don't assume direct communication** between non-adjacent zones
- **Don't use random zone IDs**
- **Don't forget parent references**

### Understanding Zone Relationships

Canopy zones form hierarchical parent-child relationships:

```
Zone 1 (Root)
├── Zone 2 (created by Zone 1)
│   └── Zone 4 (created by Zone 2)
└── Zone 3 (created by Zone 1)
    └── Zone 5 (created by Zone 3)
```

**Key rules**:
- Zone 1 can directly create Zone 2 and Zone 3 (its children)
- Zone 2 can directly create Zone 4 (its child)
- Zone 2 cannot directly create Zone 3 (sibling) or Zone 5 (grandchild)

**Multi-hop routing**: When Zone 1 needs to communicate with Zone 5, messages route through Zone 3 automatically. This is an emergent behavior controlled at a strategic level, not something individual developers configure.

## 7. Serialization

### Do

- **Use binary formats** for production (yas_binary)
- **Use JSON** for debugging/interop
- **Consider compression** for large payloads
- **Test all formats** during development

### Don't

- **Don't use JSON** in production (overhead)
- **Don't serialize sensitive data** in plain binary
- **Don't change serialized formats** without versioning

### Struct Passing Tips

When the caller and callee are in the same zone, Canopy makes a direct function call with no marshalling overhead. When they are in different zones, data is always serialised by the caller and deserialised by the callee — there is no zero-copy reference passing across a zone boundary.

Regardless of whether marshalling occurs, coroutine implementations must take `[in]` parameters **by value**. A coroutine may suspend at any `CO_AWAIT` point; a reference parameter would dangle across that suspension, causing undefined behaviour.

#### Move semantics and rvalue references

The utility of rvalue references depends on which side of the call you are on:

- **Caller / proxy side (cross-zone)**: calling `std::move()` on an argument gives no benefit. The proxy immediately serialises the object; whether it was moved-from or not makes no difference to the serialisation cost, and the object is not used again afterwards.

- **Callee / stub side (cross-zone)**: taking a parameter as `T&&` *does* help. The deserialised value can be moved directly into its destination (e.g. a member variable or container) with no extra copy.

- **Same-zone direct calls**: no serialisation takes place, so rvalue references behave exactly as in ordinary C++ — passing with `std::move()` genuinely avoids a copy on both sides.

```cpp
// Proxy side — std::move() buys nothing for cross-zone calls
service->store(std::move(large_data));  // serialised immediately; move is redundant

// Stub side — T&& allows efficient move from deserialised value
error_code store(large_struct&& data) override
{
    stored_ = std::move(data);  // moved, not copied, from the deserialised buffer
    CO_RETURN rpc::error::OK();
}
```

## 8. Performance

- **Use SPSC transport** for high-throughput in-process IPC
- **Use binary serialization** (`yas_binary`) in production
- **Pass `[in]` parameters by value in coroutines** — references dangle across suspension points regardless of whether the call crosses a zone boundary
- **Use `T&&` on the callee/stub side** for large `[in]` parameters — the deserialised value can be moved rather than copied into its destination; for same-zone direct calls this also avoids the copy on the caller side
- **Do not rely on `std::move()` on the caller/proxy side** for cross-zone performance — the proxy serialises the argument immediately and the move has no effect on cost
- **Keep coroutines non-blocking** — do not call blocking syscalls inside a coroutine
- **Use `rpc::optimistic_ptr`** for objects with independent lifetimes to avoid RAII overhead
- **Batch operations** where possible to reduce round-trip count
- **Use `[post]`** for one-way notifications, events, and logging — fire-and-forget methods eliminate the round-trip latency of a normal call entirely

## 9. Testing

### Do

- **Use template-based fixtures** for parameterized tests
- **Test both blocking and coroutine modes**
- **Use local transport** for unit tests
- **Enable telemetry** for debugging
- **Test error paths** not just success

### Don't

- **Don't skip tests** - coverage matters
- **Don't test only success paths**
- **Don't use production transports** for unit tests
- **Don't forget to clean up** between tests

### Test Pattern

```cpp
template<class T>
class calculator_test : public testing::Test
{
protected:
    T lib_;

public:
    void SetUp() override { lib_.set_up(); }
    void TearDown() override { lib_.tear_down(); }
};

TYPED_TEST(calculator_test, add)
{
    auto& lib = this->lib_;
    int result;
    auto error = CO_AWAIT lib.get_calculator()->add(1, 2, result);
    EXPECT_EQ(error, rpc::error::OK());
    EXPECT_EQ(result, 3);
}
```

## 10. Debugging

### Enable Telemetry

```cpp
#ifdef CANOPY_USE_TELEMETRY
rpc::console_telemetry_service::create(
    telemetry_service, "test", "debug_test", "/tmp");
#endif
```

### Use Sequence Diagrams

```cpp
rpc::sequence_diagram_telemetry_service::create(
    service, "test", "sequence", "/tmp");
// Generates PlantUML file for visualization
```

### Thread-Local Logging

```bash
cmake --preset Debug -DCANOPY_USE_THREAD_LOCAL_LOGGING=ON
```

## 11. Common Issues and Solutions

### Issue: OBJECT_NOT_FOUND

**Cause**: The object ID supplied to the call does not exist in the remote zone. This is a serious condition: it typically means the caller is holding a stale or fabricated object ID, or the object was never registered. It is not an expected expiry — treat it as a programming error or a security concern.

**Solution**:
```cpp
// Ensure the rpc::shared_ptr stays alive for the duration of the call
auto keep_alive = service_ptr;
auto error = CO_AWAIT service_ptr->call(data);
if (error == rpc::error::OBJECT_NOT_FOUND())
{
    // The object ID is unknown to the remote zone — log and investigate
    RPC_ERROR("Remote object not found — possible stale reference");
    CO_RETURN error;
}
```

### Issue: OBJECT_GONE

**Cause**: The object was valid when the call began but was released by the time it completed. This is the expected expiry behavior for `rpc::optimistic_ptr`: the object's lifetime is independent of the caller, so it may disappear between calls. It is not an error in the same sense as OBJECT_NOT_FOUND — it is a normal lifecycle event.

**Solution**:
```cpp
if (error == rpc::error::OBJECT_GONE())
{
    // The optimistic_ptr target has been released — this is expected behaviour
    // Refresh the reference or skip this operation
    service_ptr = try_get_fresh_reference();
}
```

### Issue: Transport disconnected

**Cause**: A call was made after the transport entered `DISCONNECTING` or `DISCONNECTED` state, or the transport failed before the call completed.

**Solution**: Treat a disconnected transport as a terminal condition. Cancel in-flight work and surface the error to the caller rather than retrying on the same transport:
```cpp
auto error = CO_AWAIT service_ptr->call(data);
if (error == rpc::error::TRANSPORT_ERROR() || error == rpc::error::ZONE_NOT_FOUND())
{
    // Transport is gone — do not retry on this transport
    CO_RETURN error;
}
```

### Issue: Deadlock in pass-through routing

**Cause**: Acquiring transports in wrong order

**Solution**:
```cpp
// Canopy handles this automatically by ordering by zone ID
// Don't manually create pass-throughs
```

### Issue: Memory leak

**Cause**: Circular references between zones

**Solution**:
```cpp
// Use weak_ptr to break cycles
class node
{
    rpc::weak_ptr<node> parent_;  // Not strong reference
    std::vector<rpc::shared_ptr<node>> children_;
};
```

## 12. Security Considerations

1. **Use SGX** for sensitive data processing
2. **Validate all inputs** - don't trust callers
3. **Use encrypted transports** for network communication
4. **Limit interface exposure** - minimal surface area
5. **Audit object passing** - data leakage prevention

## 13. Common Implementation Mistakes


### Mistake: Missing [in]/[out] Attributes Understanding

**Problem**: Not understanding what parameter attributes mean.

**Explanation**:
- `[in]` - marshal data FROM caller TO remote object (DEFAULT if no attribute)
- `[out]` - marshal data FROM remote object BACK to caller
- `[in, out]` - marshal in BOTH directions

**IDL**:
```idl
interface i_example
{
    int process([in] const std::string& input, [out] std::string& output);
};
```

**C++ Implementation**:
```cpp
// [in] parameters are taken by value — never by reference in a coroutine
CORO_TASK(int) process(std::string input, std::string& output) override
{
    output = "Result: " + input;  // output is [out] - filled by callee
    CO_RETURN rpc::error::OK();
}
```

### Mistake: Expecting std::move() to Improve Cross-Zone Call Performance

**Problem**: Using `std::move()` when passing an argument to a cross-zone proxy call, expecting to avoid a copy. The proxy serialises the argument immediately; the move has no effect on the serialisation cost.

**No benefit**:
```cpp
service->store(std::move(large_data));  // serialised straight away — move is redundant
```

**Where move semantics do help** is on the callee/stub side: taking the deserialised parameter as `T&&` lets you move it into storage rather than copy it.

```cpp
// Stub implementation — T&& avoids copying from the deserialised buffer
error_code store(large_struct&& data) override
{
    stored_ = std::move(data);  // efficient move, no copy
    CO_RETURN rpc::error::OK();
}
```

For same-zone direct calls (no marshalling) `std::move()` on the caller side works as normal C++ and genuinely avoids a copy.

### Mistake: Using [in, out] with Pointer Types

**Problem**: `rpc::shared_ptr` or `rpc::optimistic_ptr` cannot be `[in, out]`.

**Incorrect**:
```idl
interface i_transfer
{
    int transfer([in, out] rpc::shared_ptr<i_data>& data);  // ERROR!
};
```

**Correct** - separate into two parameters:
```idl
interface i_transfer
{
    int transfer([in] const rpc::shared_ptr<i_data>& input, [out] rpc::shared_ptr<i_data>& output);
};
```

### Mistake: Using Raw Pointer Types in IDL

**Problem**: Raw pointers (`T*`, `T*&`) represent memory addresses that are only valid in the local address space.

**Reason**: When marshalling data between processes or machines, pointer values (memory addresses) have no meaning in the remote address space.

**Incorrect**:
```idl
interface i_example
{
    int get_pointer([out] int*& value);  // Address meaningless remotely
    int process_ptr([in] const int* value);  // Address meaningless remotely
};
```

**Correct**:
```idl
interface i_example
{
    int get_value([out] int& value);  // Value copied across
    int get_values([out] std::vector<int>& values);  // Vector contents copied
    int get_service([out] rpc::shared_ptr<i_foo>& service);  // Interface reference
};
```

**Exception**: Raw pointers may be useful only when both objects share the same memory address space (e.g., shared memory regions).

### Mistake: Using Reserved Method Names

**Problem**: `get_id` conflicts with interface ID getter.

**Avoid**:
```idl
interface i_object
{
    int get_id([out] uint64_t& id);  // Conflicts!
};
```

**Use instead**:
```idl
interface i_object
{
    int get_object_id([out] uint64_t& id);  // Unique name
};
```

### Mistake: connect_to_zone with Wrong Arguments

**Problem**: Using 3 arguments instead of 4.

**Incorrect**:
```cpp
rpc::shared_ptr<v1::i_demo_service> child_service;
auto error = root_service->connect_to_zone("child", transport, child_service);
```

**Correct**:
```cpp
rpc::shared_ptr<v1::i_demo_service> input_service(
    new demo_service_impl("input", child_zone, root_service));
rpc::shared_ptr<v1::i_demo_service> output_service;

auto error = CO_AWAIT root_service->connect_to_zone(
    "child", transport, input_service, output_service);
```

### Mistake: Mutex Not Mutable

**Problem**: `lock_guard` fails in const method because mutex is not mutable.

**Incorrect**:
```cpp
class my_impl : public v1::i_interface
{
    std::mutex mutex_;  // Not mutable
    std::vector<int> data_;

    int get_count() const  // const method
    {
        std::lock_guard<std::mutex> lock(mutex_);  // Error!
        return data_.size();
    }
};
```

**Correct**:
```cpp
class my_impl : public v1::i_interface
{
    mutable std::mutex mutex_;  // mutable for const methods
    std::vector<int> data_;

    int get_count() const  // const method
    {
        std::lock_guard<std::mutex> lock(mutex_);  // OK
        return data_.size();
    }
};
```

### Mistake: Local Transport Include Order

**Problem**: `rpc::local::` namespace not visible.

**Incorrect**:
```cpp
#include <demo_impl.h>      // Uses rpc::local::child_transport
#include <rpc/rpc.h>
#include <transports/local/transport.h>  // Too late!
```

**Correct**:
```cpp
#include <transports/local/transport.h>  // Include first
#include <demo_impl.h>
#include <rpc/rpc.h>
```

### Mistake: Wrong Library Name in CMake

**Problem**: Linker can't find transport symbols.

**Incorrect**:
```cmake
target_link_libraries(my_demo PRIVATE rpc_transport_local)  # Wrong name!
```

**Correct**:
```cmake
target_link_libraries(my_demo PRIVATE transport_local_host)  # Correct name
```

### Mistake: Return Type Mismatch in Coroutines

**Problem**: Returning `bool` from `CORO_TASK(bool)`.

**Incorrect**:
```cpp
CORO_TASK(bool) run_demo()
{
    if (failed)
        return false;  // Type mismatch!
    return true;
}
```

**Correct**:
```cpp
CORO_TASK(bool) run_demo()
{
    if (failed)
        CO_RETURN false;  // Use CO_RETURN
    CO_RETURN true;
}
```

Or for blocking mode:
```cpp
bool run_demo()  // No CORO_TASK
{
    if (failed)
        return false;
    return true;
}
```

### Mistake: Using make_shared with Non-IDL Types

**Problem**: `create_interface_stub` not generated for types not in IDL.

**Incorrect**:
```cpp
// demo_service_impl is NOT in IDL - no create_interface_stub generated
auto service = rpc::make_shared<demo_service_impl>(name, zone, service_ptr);
auto error = CO_AWAIT root_service->connect_to_zone(
    "child", transport, service, output);
```

**Correct**:
```cpp
// Use direct construction with rpc::shared_ptr
rpc::shared_ptr<v1::i_demo_service> input_service(
    new demo_service_impl(name, zone, service_ptr));
auto error = CO_AWAIT root_service->connect_to_zone(
    "child", transport, input_service, output);
```

### Mistake: Passing [in] Parameters by Reference in Coroutines

**Problem**: A coroutine may suspend at any `CO_AWAIT` point. If an `[in]` parameter is taken by const reference, the object it refers to may have been destroyed by the time the coroutine resumes, causing undefined behaviour.

**Incorrect**:
```cpp
CORO_TASK(error_code) process(const large_struct& data) override
{
    CO_AWAIT some_async_operation();  // data reference may dangle here!
    use(data);
    CO_RETURN rpc::error::OK();
}
```

**Correct** — take `[in]` parameters by value:
```cpp
CORO_TASK(error_code) process(large_struct data) override
{
    CO_AWAIT some_async_operation();  // data is owned by the coroutine frame, safe
    use(data);
    CO_RETURN rpc::error::OK();
}
```

`[out]` parameters are references by necessity (they are written back by the callee), and Canopy's generated code ensures they remain valid. Only `[in]` parameters need to move from reference to value in coroutine implementations.

### Mistake: Thinking Duplicate Parameter Names Are Invalid

**Explanation**: Multiple methods in the same interface can use the same parameter names (e.g., `result`) without issue. The IDL generator should handle this correctly.

```idl
// This is valid IDL - parameter names can repeat across methods
interface i_calculator
{
    int add(int a, int b, [out] int& result);
    int multiply(int a, int b, [out] int& result);  // 'result' is fine here
    int divide(int a, int b, [out] int& result);    // 'result' is fine here
};
```

If you encounter conflicts with duplicate parameter names, this indicates a bug in the code generator.

