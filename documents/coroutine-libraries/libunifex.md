<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# libunifex

[libunifex](https://github.com/facebookexperimental/libunifex) is Facebook's C++20 sender/receiver framework for asynchronous operations.

## Overview

libunifex provides a novel approach to async programming based on the "sender/receiver" model rather than the traditional coroutine-based approach. It separates the description of asynchronous work (senders) from the mechanism of executing it (receivers).

## Adaptation for Canopy

Adapting Canopy to use libunifex would require changes to `c++/rpc/include/rpc/internal/coroutine_support.h`:

```cpp
#ifdef CANOPY_BUILD_COROUTINE
#include <unifex/single_thread_context.hpp>
#include <unifex/task.hpp>
#include <unifex/sync_wait.hpp>
#include <unifex/when_all.hpp>
#include <unifex/upon_stopped.hpp>

#define CORO_TASK(x) unifex::task<x>
#define CO_RETURN co_return
#define CO_AWAIT co_await
#define SYNC_WAIT(x) unifex::sync_wait(std::move(x))
#else
#define CORO_TASK(x) x
#define CO_RETURN return
#define CO_AWAIT
#define SYNC_WAIT(x) x
#endif
```

## Key Differences from libcoro

- **Sender/Receiver Model**: Operations return "senders" rather than "tasks"
- **Scheduler Abstraction**: Uses `single_thread_context` or custom schedulers
- **No sync_wait by default**: Requires explicit receiver context
- **Range-based Operations**: Strong integration with ranges-v3 concepts

## TCP Adaptation

libunifex provides async TCP operations via `unifex::ip::tcp::socket`:

```cpp
auto context = unifex::single_thread_context{};
auto socket = unifex::ip::tcp::socket{context};
co_await socket.async_connect(endpoint);
```

## Considerations

- Requires understanding of sender/receiver protocol
- Different cancellation semantics
- May require custom schedulers for transport I/O
- Strong integration with Range-v3 required
