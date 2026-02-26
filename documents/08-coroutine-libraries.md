<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Coroutine Libraries

Canopy is designed to be coroutine-library agnostic. While the library is currently written against libcoro, the core abstractions have been isolated to enable porting to other coroutine libraries with minimal changes.

## Design Philosophy

The coroutine support is encapsulated in a single header file that defines macros for all coroutine primitives. This allows the underlying implementation to be swapped without modifying the rest of the codebase.

## Implementation Location

All coroutine abstractions are defined in:

```
rpc/include/rpc/internal/coroutine_support.h
```

## Required Abstractions

To support a new coroutine library, the following abstractions must be provided:

| Macro | Purpose | Requirements |
|-------|---------|--------------|
| `CORO_TASK(x)` | Return type for coroutine functions | Must be awaitable, copyable/movable |
| `CO_RETURN` | Return from coroutine | Coroutine return statement |
| `CO_AWAIT` | Suspend until completion | Must work with `co_await` expression |
| `SYNC_WAIT(x)` | Blocking wait for coroutine | Must block current thread until completion |

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

## Supported Libraries

- [libcoro](libcoro.md) - Current implementation, C++20 coroutine library
- [libunifex](libunifex.md) - Facebook's sender/receiver framework
- [cppcoro](cppcoro.md) - Lewis Baker's foundational coroutine library
- [Asio](asio.md) - Cross-platform async I/O library

## Feature Matrix

| Feature | libcoro | libunifex | cppcoro | Asio |
|---------|---------|-----------|---------|------|
| task<T> | Yes | Via sender | Yes | awaitable<T> |
| sync_wait | Yes | Via sync_wait | Yes | Via io_context |
| Thread pool | Yes | Yes | static_thread_pool | io_context |
| TCP I/O | Yes | Via libunifex | Limited | Yes |
| UDP I/O | Yes | Via libunifex | Limited | Yes |
| Timers | Yes | Via libunifex | Limited | Yes |
| Active development | Yes | Yes | Minimal | Yes |

## Limitations

Some advanced features may require library-specific extensions:

- **io_scheduler integration** - Currently tied to libcoro's io_scheduler for TCP and SPSC transports
- **Network primitives** - TCP client/server abstractions are libcoro-specific
- **Channel/back-channel support** - May require adaptation for sender/receiver models

## Testing

After porting, ensure all tests pass in both blocking and coroutine modes:

```bash
# Coroutine mode
cmake --preset Coroutine_Debug
cmake --build build --target all
ctest --test-dir build

# Blocking mode (default)
cmake --preset Debug
cmake --build build --target all
ctest --test-dir build
```
