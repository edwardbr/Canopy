<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# libcoro

libcoro is a C++20 coroutine library that provides fundamental primitives for async/await-style programming.

## Current Usage in Canopy

Canopy is currently written against libcoro and uses the following abstractions:

```cpp
#define CORO_TASK(x) coro::task<x>
#define CO_RETURN co_return
#define CO_AWAIT co_await
#define SYNC_WAIT(x) coro::sync_wait(x)
```

## Dependencies

Canopy integrates with libcoro's io_scheduler for async I/O operations:

```cpp
auto scheduler = coro::scheduler::make_unique(
    coro::scheduler::options{
        .thread_strategy = coro::scheduler::thread_strategy_t::spawn,
        .pool = coro::thread_pool::options{
            .thread_count = std::thread::hardware_concurrency(),
        },
        .execution_strategy = coro::scheduler::execution_strategy_t::process_tasks_on_thread_pool
    });
```

## Key Features Used

- `coro::task<T>` - Return type for coroutine functions
- `coro::sync_wait()` - Blocking wait for coroutine completion
- `coro::scheduler` - Async I/O scheduling for TCP and SPSC transports
- `coro::net::tcp::client/server` - TCP networking primitives

## Submodule Location

`submodules/libcoro/`

## CMake Configuration

```cmake
add_subdirectory(libcoro)
target_link_libraries(target PUBLIC libcoro)
```
