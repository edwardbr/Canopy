<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Bi-Modal Execution

Canopy supports both blocking and coroutine execution modes using the same codebase. This section explains how bi-modal execution works and when to use each mode.

## 1. How Bi-Modal Execution Works

### The Macro System

Canopy uses preprocessor macros to switch between blocking and coroutine modes:

```cpp
// From coroutine_support.h

#ifdef CANOPY_BUILD_COROUTINE
    #define CORO_TASK(x)  coro::task<x>
    #define CO_RETURN     co_return
    #define CO_AWAIT      co_await
    #define SYNC_WAIT(x)  coro::sync_wait(x)
#else
    #define CORO_TASK(x)  x
    #define CO_RETURN     return
    #define CO_AWAIT
    #define SYNC_WAIT(x)  x
#endif
```

### Transformation

**Blocking Mode** (CANOPY_BUILD_COROUTINE=OFF):
```cpp
CORO_TASK(error_code) my_method(int value)
{
    int result;
    CO_AWAIT proxy_->call(value, result);
    CO_RETURN error::OK();
}

// Transforms to:
error_code my_method(int value)
{
    int result;
    proxy_->call(value, result);
    return error::OK();
}
```

**Coroutine Mode** (CANOPY_BUILD_COROUTINE=ON):
```cpp
CORO_TASK(error_code) my_method(int value)
{
    int result;
    CO_AWAIT proxy_->call(value, result);
    CO_RETURN error::OK();
}

// Transforms to:
coro::task<error_code> my_method(int value)
{
    int result;
    co_await proxy_->call(value, result);
    co_return error::OK();
}
```

## 2. Benefits of Bi-Modal Execution

### Development and Debugging

- **Blocking mode** is easier to debug (no coroutine complexity)
- Standard debuggers work without special coroutine support
- Simpler stack traces

### Production Performance

- **Coroutine mode** enables efficient async I/O
- No thread blocking during network calls
- Better resource utilization

### Unified Codebase

- Single source code for both modes
- No code duplication
- Consistent behavior

## 3. Using Bi-Modal Execution

### Writing Compatible Code

```cpp
// Always use macros in interface implementations
CORO_TASK(error_code) my_service::do_work(int input, [out] int& output)
{
    // This works in both modes
    auto error = CO_AWAIT worker_->process(input, output);
    if (error != error::OK())
    {
        CO_RETURN error;
    }
    CO_RETURN error::OK();
}
```

### Switching Modes

**At Build Time**:
```bash
# Blocking mode (default)
cmake --preset Debug

# Coroutine mode
cmake --preset Debug_Coroutine
```

### Handling the Difference

**For synchronous code** (runs in both modes):
```cpp
// Works identically in both modes
auto result = calculate_sync();
```

**For asynchronous code** (requires coroutines):
```cpp
#ifdef CANOPY_BUILD_COROUTINE
    // Coroutine-specific code
    auto task = async_operation();
    CO_AWAIT task;
#else
    // Blocking fallback
    auto result = blocking_operation();
#endif
```

## 4. Coroutine Scheduler Setup

### IO Scheduler

For coroutine mode, set up an IO scheduler:

```cpp
auto scheduler = coro::scheduler::make_unique(
    coro::scheduler::options{
        .thread_strategy = coro::scheduler::thread_strategy_t::spawn,
        .pool = coro::thread_pool::options{
            .thread_count = std::thread::hardware_concurrency(),
        },
        .execution_strategy = coro::scheduler::execution_strategy_t::process_tasks_on_thread_pool
    });

// Pass scheduler to service
auto service = std::make_shared<rpc::service>(
    "my_service",
    rpc::zone{1},
    scheduler);
```

### Spawning Coroutines

```cpp
// Spawn a coroutine task
scheduler->spawn([&]() -> CORO_TASK(void)
{
    int result;
    auto error = CO_AWAIT calculator_->add(10, 20, result);
    std::cout << "Result: " << result << "\n";
    CO_RETURN;
}());

// Process events until complete
bool done = false;
while (!done)
{
    scheduler->process_events(std::chrono::milliseconds(1));
}
```

## 5. Best Practices

### Do

- Use `CORO_TASK`, `CO_RETURN`, `CO_AWAIT` consistently
- Keep coroutines short and focused
- Handle errors at each await point
- Use blocking mode for debugging

### Don't

- Mix blocking calls with coroutines
- Block in coroutine mode without careful consideration
- Assume synchronous behavior in coroutine mode

## 6. Performance Comparison

### Blocking Mode

- **Thread per connection** model
- Simpler mental model
- Higher memory usage
- Good for CPU-bound work

### Coroutine Mode

- **Event-driven** model
- Thousands of concurrent operations
- Lower memory footprint
- Better for I/O-bound work

## 7. Migration Guide

### From Blocking to Coroutine

1. **Add scheduler** to service constructor
2. **Replace `return`** with `CO_RETURN`
3. **Add `CO_AWAIT`** before async operations
4. **Spawn tasks** instead of calling directly
5. **Process events** in main loop

### Complete Example

```cpp
// Blocking version
error_code blocking_work()
{
    return calculator_->add(1, 2, result);
}

// Coroutine version
CORO_TASK(error_code) async_work()
{
    return CO_AWAIT calculator_->add(1, 2, result);
}

// Usage in coroutine mode
scheduler->spawn([&]() -> CORO_TASK(void)
{
    auto error = CO_AWAIT async_work();
    if (error == error::OK())
    {
        std::cout << "Success!\n";
    }
    CO_RETURN;
}());
```

## 8. Common Patterns

### Parallel Operations

```cpp
CORO_TASK(void) parallel_work()
{
    // Run multiple operations concurrently
    auto task1 = calculator_->add(1, 2, result1);
    auto task2 = calculator_->multiply(3, 4, result2);

    // Await both
    CO_AWAIT task1;
    CO_AWAIT task2;

    CO_RETURN;
}
```

### Error Propagation

```cpp
CORO_TASK(error_code) chained_operations()
{
    int result;

    // Each error propagates up
    auto error = CO_AWAIT calculator_->add(10, 20, result);
    if (error != error::OK())
    {
        CO_RETURN error;
    }

    error = CO_AWAIT calculator_->multiply(result, 2, result);
    if (error != error::OK())
    {
        CO_RETURN error;
    }

    CO_RETURN error::OK();
}
```

## 9. Next Steps

- [YAS Serializer](serializers/yas-serializer.md) - Learn about encoding formats
- [Protocol Buffers](serializers/protocol-buffers.md) - Cross-language serialization
- [Error Handling](06-error-handling.md) - Understand error propagation
- [Telemetry](07-telemetry.md) - Debug coroutine execution
