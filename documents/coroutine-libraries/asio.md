<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Asio

[Asio](https://think-async.com/Asio/) is a cross-platform C++ library for network and low-level I/O programming, available both as part of Boost and as a standalone library.

## Overview

Asio provides a proactor-based async model that has been extended with C++20 coroutine support via `co_await` syntax.

## Adaptation for Canopy

Adapting Canopy to use Asio would require changes to `c++/rpc/include/rpc/internal/coroutine_support.h`:

```cpp
#ifdef CANOPY_BUILD_COROUTINE
#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/use_awaitable_t.hpp>

#define CORO_TASK(x) asio::awaitable<x>
#define CO_RETURN co_return
#define CO_AWAIT co_await
#define SYNC_WAIT(x) \
    asio::io_context ctx; \
    asio::co_spawn(ctx, std::move(x), asio::detached); \
    ctx.run()
#else
#define CORO_TASK(x) x
#define CO_RETURN return
#define CO_AWAIT
#define SYNC_WAIT(x) x
#endif
```

## Key Features

- `asio::awaitable<T>` - Return type for async operations
- `asio::io_context` - Execution context for async operations
- `asio::co_spawn()` - Launch coroutines
- `asio::use_awaitable` - Completion token for async operations
- Rich async I/O primitives (TCP, UDP, timers, etc.)

## TCP Adaptation

Asio provides comprehensive TCP support:

```cpp
auto tcp_echo = []() -> asio::awaitable<void> {
    auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    tcp::acceptor acceptor(executor, {tcp::v4(), 8080});
    auto client = co_await acceptor.async_accept(use_awaitable);
    co_await async_write(client, buffer(data), use_awaitable);
};
```

## Integration Patterns

```cpp
// Using spawn with io_context
asio::io_context ctx;
asio::co_spawn(ctx, server_task, asio::detached);
ctx.run();

// Using use_awaitable with custom executor
auto result = co_await socket.async_read_some(buffer(data), asio::use_awaitable);
```

## Considerations

- Different paradigm (proactor vs. coroutine-first)
- Requires understanding of Asio's executor model
- May need `spawn_with_bind_allocator` for complex scenarios
- Excellent cross-platform support including Windows IOCP
- Can be combined with other coroutine libraries for specific features
