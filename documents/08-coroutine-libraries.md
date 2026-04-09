<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Coroutine Libraries

Scope note:

- this document describes the C++ coroutine/backend strategy
- it is not a statement that non-C++ Canopy implementations expose the same
  coroutine abstraction layer
- see [C++ Status](status/cpp.md), [Rust Status](status/rust.md), and
  [JavaScript Status](status/javascript.md) for implementation scope

Canopy is intended to become more coroutine-library agnostic over time. In the
current C++ tree, the macro layer in `coroutine_support.h` isolates the basic
syntax, but the streaming and coroutine transports still depend materially on
the current backend choices.

This document should therefore be read as:

- current C++ state
- target design direction
- migration plan for reducing backend coupling

## Design Philosophy

The coroutine support is encapsulated in a single header file that defines macros for all coroutine primitives. This allows the underlying implementation to be swapped without modifying the rest of the codebase.

## Implementation Location

The syntax-level coroutine abstractions are defined in:

```
rpc/include/rpc/internal/coroutine_support.h
```

## Required Abstractions

To support a new coroutine library, the following abstractions must be provided:

| Macro          | Purpose                             | Blocking mode | Requirements                                    |
|----------------|-------------------------------------|---------------|-------------------------------------------------|
| `CORO_TASK(x)` | Return type for coroutine functions | x             | Must be awaitable, copyable/movable             |
| `CO_RETURN`    | Return from coroutine               | return        | Coroutine return statement                      |
| `CO_AWAIT`     | Suspend until completion            | <empty>       | Must work with `co_await` expression            |
| `SYNC_WAIT(x)` | Blocking wait for coroutine         | <custom>      | Must block current thread/task until completion |

## Current Implementation (libcoro)

```cpp
#ifdef CANOPY_BUILD_COROUTINE
#include <coro/coro.hpp>

#define CORO_TASK(x) coro::task<x>
#define CO_RETURN co_return
#define CO_AWAIT co_await
#define SYNC_WAIT(x) coro::sync_wait(x)
#else
#define CORO_TASK(x) x
#define CO_RETURN return
#define CO_AWAIT
#define SYNC_WAIT(x) x
#endif
```

## Porting Guide

### Step 1: Update coroutine_support.h

Replace the libcoro includes and macros with your chosen library:

```cpp
#ifdef CANOPY_BUILD_COROUTINE
#include <your_library/task.hpp>
#include <your_library/sync_wait.hpp>

#define CORO_TASK(x) your_library::task<x>
#define CO_RETURN co_return
#define CO_AWAIT co_await
#define SYNC_WAIT(x) your_library::sync_wait(x)
#else
#define CORO_TASK(x) x
#define CO_RETURN return
#define CO_AWAIT
#define SYNC_WAIT(x) x
#endif
```

### Step 2: Update CMake Configuration

Modify the CMakeLists.txt to link against your chosen library:

```cmake
# Remove libcoro dependency
# target_link_libraries(target PUBLIC libcoro)

# Add your library
find_package(YourCoroutineLibrary REQUIRED)
target_link_libraries(target PUBLIC YourCoroutineLibrary::YourCoroutineLibrary)
```

### Step 3: Update Transport I/O

For transports that use async I/O (TCP, SPSC), you may need to adapt the networking primitives:

- **libcoro**: Uses `coro::scheduler` and `coro::net::tcp::*`
- **Asio**: Uses `asio::io_context` and `asio::ip::tcp::*`
- **libunifex**: Uses `unifex::single_thread_context` and sender-based operations

## Candidate Libraries

- [libcoro](libcoro.md) - Current implementation, C++20 coroutine library
- [libunifex](libunifex.md) - Facebook's sender/receiver framework
- [cppcoro](cppcoro.md) - Lewis Baker's foundational coroutine library
- [Asio](asio.md) - Cross-platform async I/O library

## Feature Matrix

| Feature            | libcoro | libunifex     | cppcoro            | Asio            |
|--------------------|---------|---------------|--------------------|-----------------|
| task<T>            | Yes     | Via sender    | Yes                | awaitable<T>    |
| sync_wait          | Yes     | Via sync_wait | Yes                | Via io_context  |
| Thread pool        | Yes     | Yes           | static_thread_pool | io_context      |
| TCP I/O            | Yes     | Via libunifex | Limited            | Yes             |
| UDP I/O            | Yes     | Via libunifex | Limited            | Yes             |
| Timers             | Yes     | Via libunifex | Limited            | Yes             |
| Active development | Current backend | Investigate | Investigate | Investigate |

## Current Limitations

Some advanced features still require backend-specific integration work:

- **Scheduler integration** - Currently tied to the active C++ coroutine backend for TCP and streaming transports
- **Network primitives** - TCP client/server abstractions are libcoro-specific
- **Channel/back-channel support** - May require adaptation for sender/receiver models

## Migration Plan

The current macro layer around `CORO_TASK`, `CO_AWAIT`, and `SYNC_WAIT` is enough to swap the coroutine syntax, but it is not yet enough to make Canopy independent from a specific async runtime. The next step is to move the ownership boundary up so that coroutine scheduling, networking, timers, and socket status are Canopy abstractions rather than direct `libcoro` types.

### Target Architecture

Canopy should own the public async surface used by transports and streams:

- `canopy::task<T>`
- `canopy::sync_wait()`
- `canopy::scheduler`
- `canopy::io_status`
- `canopy::tcp_client`
- `canopy::tcp_listener`
- `canopy::stream_socket`
- `canopy::timer` or equivalent timeout primitive

Transport and stream code should depend only on these Canopy abstractions. Backend-specific code should live behind an adapter layer.

### Backend Model

The preferred model is:

1. Canopy defines runtime-facing interfaces and value types.
2. A backend adapter implements those interfaces for a concrete coroutine library.
3. `libcoro` becomes one backend rather than the type system used directly by `streaming`, transports, and demos.

This keeps the rest of Canopy insulated from:

- `coro::scheduler`
- `coro::task`
- `coro::net::tcp::client/server`
- `coro::net::io_status`
- backend-specific timeout and polling mechanics

### Recommended Sequence

#### Create a Canopy Async Facade

Add a Canopy-owned header set that defines:

- coroutine task aliases or wrappers
- scheduler abstraction
- I/O result/status types
- socket and listener interfaces
- timeout and timer abstractions

At this phase the implementation can still delegate entirely to `libcoro`, but direct `libcoro` types should stop appearing in transport-facing public headers.

#### Move Streaming Public Headers to Canopy Types

Refactor:

- `stream`
- `stream_acceptor`
- TCP stream classes
- io_uring stream classes
- listener classes

so their public APIs use Canopy types rather than `coro::*` and `coro::net::*`.

This is the point where `streaming` stops leaking the backend choice into the wider codebase.

#### Isolate Backend Implementations

Move backend-specific code into implementation-specific areas, for example:

- `streaming/backends/libcoro/...`
- `streaming/backends/io_uring/...`

The `libcoro` backend would adapt:

- task execution
- scheduler integration
- TCP connect/accept
- polling and timeouts

The io_uring path can then be treated as a Canopy backend implementation decision rather than a transport class that is hardwired to `libcoro` scheduling semantics.

#### Introduce Canopy-Owned I/O Execution

Once the facade is in place, Canopy can choose a backend-specific execution strategy without changing the transport API:

- shared io_uring ring per runtime or per worker
- dedicated Canopy I/O thread or I/O executor
- backend-specific timeout strategy
- backend-specific accept/connect implementation

This is the point where io_uring can be tuned for Canopy rather than shaped around a generic external scheduler.

#### Add Alternate Backends

After `libcoro` is behind the facade, other backends can be introduced incrementally:

- Asio
- cppcoro
- libunifex
- a Canopy-owned runtime

### Why a Scheduler Wrapper Alone Is Not Enough

Wrapping only `coro::scheduler` does not fully decouple the codebase. The following also need to move behind Canopy abstractions:

- task type
- blocking wait
- I/O status values
- TCP client and listener types
- stream/socket ownership
- timer and timeout support

If these remain as `libcoro` types in public APIs, the dependency still leaks through the entire transport layer.

### io_uring Implications

This separation is especially important for io_uring. A direct io_uring implementation often benefits from a different execution model than a generic TCP scheduler, for example:

- shared rings instead of one ring per connection
- dedicated completion processing
- batching submits and completions
- transport-aware timeout policies

Keeping io_uring behind a Canopy backend makes those choices local to the backend implementation and avoids coupling transport classes to a specific coroutine library.

### Initial Deliverables

The first useful implementation milestone is:

1. Introduce Canopy async facade headers.
2. Convert `stream` and `stream_acceptor` APIs to Canopy-owned async and I/O types.
3. Provide a `libcoro` adapter that preserves current behaviour.
4. Move TCP and io_uring stream implementations behind that adapter boundary.

At that point Canopy remains functional with the current runtime, while the dependency surface is narrow enough to support alternative backends and a Canopy-owned I/O runtime later.

## Testing

After porting, ensure all tests pass in both blocking and coroutine modes:

```bash
# Coroutine mode
cmake --preset Debug_Coroutine
cmake --build build_debug_coroutine --target all
ctest --test-dir build_debug_coroutine

# Blocking mode (default)
cmake --preset Debug
cmake --build build_debug --target all
ctest --test-dir build_debug
```
