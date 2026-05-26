<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Bi-Modal Execution

Scope note:

- this document describes the C++ bi-modal execution model
- the blocking/coroutine macro surface is a C++ implementation feature, not a
  guarantee of parity across other Canopy implementations
- see [C++ Status](status/cpp.md), [Rust Status](status/rust.md), and
  [JavaScript Status](status/javascript.md) for implementation scope

The primary C++ implementation supports both blocking and coroutine execution
modes using the same codebase. This section explains how bi-modal execution
works and when to use each mode.

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

## 4. Service Dispatch: Executor and Scheduler

Canopy's service uses a thread-of-execution abstraction called `rpc::executor`
to dispatch streaming I/O loops and any other long-running work spawned through
`service->SPAWN(...)`. The executor takes a different concrete form in each
mode but exposes the same call sites, so the same source code drives both.

| Concept | Coroutine build | Blocking build |
|---|---|---|
| Type | `rpc::executor = rpc::coro::scheduler` (alias) | `rpc::executor = rpc::blocking_executor` |
| Underlying tech | libcoro `coro::scheduler` (epoll + cooperative coroutines) | `std::thread` pool with per-worker queues + work-stealing |
| Required for streaming | **Yes** — streaming was never available without one | **Yes** — see §4.4 |
| Required for non-streaming RPC | **Yes** — the scheduler also drives the marshaller resume hops | **No** — the synchronous RPC path runs entirely on the caller's thread |
| `service->SPAWN(expr)` | `expr` is a `CORO_TASK(void)`; `spawn_detached`'d on the scheduler | `expr` is wrapped in a copy-capturing lambda and posted to the pool |
| `service->get_executor()->schedule()` | Cooperative yield awaitable | No-op (use `poll(POLLOUT)` for write-readiness; see §4.3) |
| io_uring | Available on Linux | Not used (see §4.5) |

### 4.1 Coroutine scheduler setup

In coroutine builds the service is always constructed with a scheduler:

```cpp
auto scheduler = coro::scheduler::make_unique(
    coro::scheduler::options{
        .thread_strategy    = coro::scheduler::thread_strategy_t::spawn,
        .pool               = coro::thread_pool::options{
                                  .thread_count = std::thread::hardware_concurrency() },
        .execution_strategy = coro::scheduler::execution_strategy_t::process_tasks_on_thread_pool
    });

auto service = rpc::root_service::create("my_service", rpc::zone{addr}, scheduler);
```

Spawn coroutines through the service (preferred) or directly on the scheduler:

```cpp
service->SPAWN(my_async_work());           // CORO_TASK(void) my_async_work();
scheduler->spawn_detached(my_async_work());// equivalent at this layer
```

### 4.2 Blocking executor setup

In blocking builds the executor is **optional**. Two service-construction
paths are provided:

```cpp
// (A) Synchronous-only, no thread pool. Identical to the pre-executor
//     behaviour: everything runs on the caller's thread. No streaming.
auto service = rpc::root_service::create("my_service", rpc::zone{addr});

// (B) With executor — required for streaming, listener, WS framing, etc.
auto executor = std::make_shared<rpc::blocking_executor>();   // default thread count
auto service  = rpc::root_service::create("my_service", rpc::zone{addr}, executor);
```

The blocking executor's API mirrors the scheduler's where it matters:

```cpp
executor->post(std::function<void()>);     // run a callable on the pool
executor->schedule();                      // documented no-op (see §4.3)
executor->schedule_after(std::chrono::milliseconds{500});   // shutdown-aware timed wait
executor->shutdown();                      // reject new posts, drain queued work, join workers
```

`service->SPAWN(expr)` works identically — in this mode the macro wraps
`expr` in a copy-capturing lambda and posts it to the pool. The same
streaming code that worked in coroutine mode keeps compiling and running.

### 4.3 `schedule()` semantics — important difference

In coroutine mode `executor->schedule()` is the cooperative yield: the
calling coroutine suspends and is resumed on a pool thread. Tight loops
that retry an `EAGAIN` syscall use it to let peers run.

In blocking mode `executor->schedule()` is a **no-op**. The worker thread
that is currently running the loop never has anything to yield to. Code
that genuinely needs to wait for I/O readiness — for example, a `send()`
that just got `EAGAIN` — must do its own `poll(POLLOUT, timeout)` or
equivalent. The `streaming::tcp::socket` abstraction encapsulates this
difference so `streaming::tcp::stream::send()` has identical source in both
modes.

### 4.4 The opt-in rule for blocking mode

> `rpc::executor` is **opt-in** in blocking builds. Users who do not need
> streaming continue using the synchronous RPC path unchanged. The cost of
> the thread pool (workers, queues, scheduling) is paid only when its
> benefit (streaming, async dispatch) is consumed.

What this means in practice:

- Existing blocking-mode code that constructs a service without an
  executor continues to work exactly as it did. No behavioural change.
- `service->get_executor()` returns `nullptr` in this configuration.
- `service->SPAWN(...)` returns `false` if no executor is configured —
  callers should surface this as a clean failure rather than crash.
- Streaming features (`streaming::listener`, `streaming::tcp::acceptor`,
  `transport_websocket`, …) require an executor and will fail to start
  if the service was constructed without one. The acceptor's `init()`
  returns `false`; the listener propagates that to `start_listening()`.

Coroutine mode does not have an analogous opt-out — a scheduler has always
been mandatory because the marshaller's resume semantics depend on one.

### 4.5 io_uring is coroutine-only

io_uring's value is its async-completion model — submit many ops, let the
kernel batch them, resume coroutines as completions arrive. In blocking
mode the caller has to park on a cv until its op completes, which
effectively reimplements blocking I/O on top of an async substrate without
gaining anything. The decision recorded in the project is: **io_uring is
reserved for non-blocking / coroutine paths only.** Blocking-mode TCP uses
plain POSIX `::recv`/`::send` + `poll()`.

This is one of the design boundaries where blocking accepts a degraded
experience to keep the code simple, while coroutine retains every
performance advantage it had.

### 4.6 Pros and cons summary

**Coroutine mode (libcoro scheduler):**

Pros
- Single thread can drive thousands of concurrent connections via epoll
- io_uring path available on Linux for the highest-throughput I/O
- `co_await` makes the suspension point obvious in source
- Lock-free task continuation through the scheduler's thread pool
- Lower memory footprint per in-flight call (no per-connection thread)

Cons
- Requires C++20 and a libcoro-compatible toolchain
- Steeper debugging story — coroutine frames are harder to trace
- Forces every dependency on the call path to be coroutine-aware
- The marshaller's `result_listener` race contract is non-trivial; bugs
  manifest as deadlocks or stalls rather than crashes

**Blocking mode (rpc::blocking_executor):**

Pros
- Works on any C++17 compiler — no coroutine support needed
- Plain stack frames in a debugger; standard tooling all applies
- Opt-in: non-streaming users pay nothing for the thread pool
- Simpler primitives (POSIX `::recv`/`::send` + `poll`)
- Per-worker queues with work-stealing keep streaming loops from
  starving each other (a worker blocked in `::recv` won't hold up
  unrelated tasks posted to its queue — peers steal them)

Cons
- One worker thread per long-running streaming loop (receive,
  send) — does not scale to thousands of connections the way the
  coroutine path does
- io_uring is unavailable, so high-throughput Linux I/O is left on
  the table
- `executor->schedule()` is a no-op — calling code can't rely on a
  cooperative yield to give peers a chance; callers must do their own
  blocking wait (poll, cv) where required
- TLS via libcoro is unavailable; secure streams still need their
  own dual-mode treatment (planned)

## 5. Best Practices

### Do

- Use `CORO_TASK`, `CO_RETURN`, `CO_AWAIT` consistently
- Keep coroutines short and focused
- Handle errors at each await point
- Use blocking mode for debugging
- Ensure no runtime locks remain held across marshaller, transport, or other I/O
  boundaries

### Don't

- Mix blocking calls with coroutines casually
- Block in coroutine mode without careful consideration
- Assume synchronous behavior in coroutine mode
- Hold locks across operations that may suspend or perform RPC I/O

## 6. Performance Comparison

See §4.6 for the detailed pros and cons of each execution model. A short
summary:

### Blocking Mode

- **Thread-pool dispatch** (`rpc::blocking_executor`) when an executor is
  configured; **pure synchronous on the caller's thread** when one is not
- Plain stack frames; standard debuggers; C++17 toolchain
- One worker per streaming receive/send loop — adequate for tens of
  connections, not thousands
- Per-worker queues + work-stealing keep posted handlers running even when
  a worker is parked inside `::recv`
- No io_uring; POSIX `::recv`/`::send` + `poll()` carry the I/O

### Coroutine Mode

- **Event-driven** (libcoro `coro::scheduler`) — single epoll loop drives
  many connections
- Lower memory per in-flight call
- io_uring available on Linux for the highest-throughput paths
- Requires C++20 and the libcoro toolchain
- Coroutine frames harder to debug; suspension-point bugs surface as
  stalls rather than crashes

## 7. Migration Guide

### From Blocking to Coroutine

1. **Add scheduler** to a concrete service such as `rpc::root_service`
2. **Replace `return`** with `CO_RETURN`
3. **Add `CO_AWAIT`** before async operations
4. **Spawn tasks** instead of calling directly — prefer `service->SPAWN(...)`
   over poking the scheduler directly, so the same call site works in both
   modes
5. **Process events** in main loop

### From Coroutine to Blocking (Adding Streaming)

Existing synchronous code keeps working unchanged — it does not need a
thread pool. To add streaming features, opt in:

1. **Construct an `rpc::blocking_executor`** (default options work for most
   cases — hardware concurrency with a minimum of four workers)
2. **Pass it to the service** via the `(name, zone, executor)`
   constructor / `root_service::create` overload
3. Streaming uses (`streaming::listener`, `streaming::tcp::acceptor`,
   `transport_streaming`, `transport_websocket`) work the same as they do
   in coroutine mode, written against the same macros
4. **Do not call `executor->schedule()` as a write-readiness wait** —
   it is a no-op in blocking mode; use `poll(POLLOUT)` (or the streaming
   primitive that wraps it) instead

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
    CO_RETURN CO_AWAIT calculator_->add(1, 2, result);
}

// Usage in coroutine mode
scheduler->spawn([&]() -> CORO_TASK(void)
{
    auto error = CO_AWAIT async_work();
    if (error == rpc::error::OK())
    {
        std::cout << "Success!\n";
    }
    CO_RETURN;
}());
```

## 8. Common Patterns

### Sequential Operations

```cpp
CORO_TASK(void) sequential_work()
{
    // Run operations one after the other
    auto task1 = calculator_->add(1, 2, result1);
    auto task2 = calculator_->multiply(3, 4, result2);

    // Await each in turn
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
    if (error != rpc::error::OK())
    {
        CO_RETURN error;
    }

    error = CO_AWAIT calculator_->multiply(result, 2, result);
    if (error != rpc::error::OK())
    {
        CO_RETURN error;
    }

    CO_RETURN rpc::error::OK();
}
```

## 9. Current Scope

- C++ supports both blocking and coroutine builds.
- The abstract `streaming::stream` interface, `streaming::tcp::stream`,
  `streaming::tcp::acceptor`, `streaming::listener`, `transport_streaming`,
  `streaming::websocket::stream`, and `transport_websocket` are all
  dual-mode and compile from the same source in both builds.
- In blocking builds, `rpc::executor` (an `rpc::blocking_executor` thread
  pool) is opt-in: non-streaming users continue to use the synchronous
  RPC path with no thread pool. See §4.4.
- OpenSSL TLS secure streams, the HTTP server, and the file-system manager
  now have blocking and coroutine paths. mbedtls secure streams, io_uring,
  the WebSocket demo's video pipeline, and SGX enclave variants remain
  coroutine-only or conditionally built.
- The experimental Rust implementation is currently blocking-only.
- The current JavaScript implementation does not mirror the C++ coroutine
  runtime model.

## 10. Next Steps

- [YAS Serializer](serializers/yas-serializer.md) - Learn about encoding formats
- [Protocol Buffers](serializers/protocol-buffers.md) - Cross-language serialization
- [Error Handling](06-error-handling.md) - Understand error propagation
- [Telemetry](07-telemetry.md) - Debug coroutine execution
