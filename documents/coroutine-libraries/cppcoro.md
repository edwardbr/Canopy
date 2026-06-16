<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# cppcoro

[cppcoro](https://github.com/lewissbaker/cppcoro) is a C++20 coroutine library by Lewis Baker, providing low-level primitives for async/await programming.

## Overview

cppcoro was one of the first widely-used C++20 coroutine libraries and provides the foundational abstractions that influenced many later designs, including libcoro.

## Adaptation for Canopy

Adapting Canopy to use cppcoro would require changes to `c++/rpc/include/rpc/internal/coroutine_support.h`:

```cpp
#ifdef CANOPY_BUILD_COROUTINE
#include <cppcoro/task.hpp>
#include <cppcoro/sync_wait.hpp>
#include <cppcoro/when_all.hpp>
#include <cppcoro/static_thread_pool.hpp>

#define CORO_TASK(x) cppcoro::task<x>
#define CO_RETURN co_return
#define CO_AWAIT co_await
#define SYNC_WAIT(x) cppcoro::sync_wait(std::move(x))
#else
#define CORO_TASK(x) x
#define CO_RETURN return
#define CO_AWAIT
#define SYNC_WAIT(x) x
#endif
```

## Key Features

- `cppcoro::task<T>` - Basic coroutine task type
- `cppcoro::shared_task<T>` - Reference-counted tasks
- `cppcoro::sync_wait()` - Blocking await for single task
- `cppcoro::when_all()` - Join multiple tasks
- `cppcoro::static_thread_pool` - Thread pool for scheduling

## TCP and I/O

cppcoro provides async I/O primitives (platform-dependent):

```cpp
auto task = [&]() -> cppcoro::task<std::string> {
    auto file = co_await cppcoro::async_file_read("path", 0, 1024);
    co_return file;
};
```

Note: cppcoro's I/O primitives are less mature than libcoro's io_scheduler and may require platform-specific extensions.

## Differences from libcoro

- No built-in io_scheduler (requires custom implementation)
- Missing some higher-level networking primitives
- Lower-level primitives require more boilerplate
- No built-in async TCP client/server types

## Considerations

- May require external library for async I/O (e.g., libunifex for networking)
- Thread pool integration requires additional code
- Active development has slowed in favor of libunifex
