<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Dynamic Library and IPC Child Transports

Scope note:

- this document is primarily a C++ transport document
- the transport concepts are still useful as shared Canopy semantics
- exact target names, ABI names, coroutine behavior, and process-hosting
  details should be read as C++ implementation details unless explicitly stated
  otherwise

Canopy now has six closely related transports and transport-adjacent runtime
components for loading child zones from shared objects or child processes:

- `rpc::dynamic_library` for blocking / non-coroutine builds
- `rpc::c_abi` for blocking / non-coroutine builds that need the
  language-neutral dynamic-library ABI
- `rpc::libcoro_host_scheduled_dynamic_library` for coroutine builds where the
  loaded DLL shares the host scheduler
- `rpc::libcoro_dll_scheduled_dynamic_library` for coroutine builds where the
  loaded DLL owns its scheduler
- `rpc::libcoro_spsc_dynamic_dll` for coroutine builds where the DLL is reached
  over an SPSC stream
- `rpc::ipc_transport` for coroutine builds where the host spawns and owns a
  child process and connects to it through an SPSC-backed `stream_transport`

These pieces are intentionally separate:

- `dynamic_library`, `c_abi`, `libcoro_host_scheduled_dynamic_library`, and
  `libcoro_dll_scheduled_dynamic_library` load a DLL into the current process
- `libcoro_spsc_dynamic_dll` runs the DLL runtime behind an SPSC stream
- `ipc_transport` owns a child process and the shared-memory queue pair used to
  talk to it
- `canopy_ipc_child_host_process` is not a transport; it is a small executable
  that maps the shared queue pair and forwards it into
  `libcoro_spsc_dynamic_dll`
- direct child-process bootstrap support exists in the source tree, but the
  `canopy_ipc_child_process` executable is currently disabled in CMake pending
  rework

`rpc::ipc_transport` is not necessarily a hierarchical transport. It is a
process-owning `rpc::stream_transport::transport` that can be combined with the
DLL-hosting child-process runtime. The `direct_service` process kind is still
represented in the API, but the matching in-tree executable is not currently
built.

This page should therefore be read as C++ transport/runtime guidance for the
current tree, not as a statement that every Canopy implementation provides the
same DLL or child-process hosting stack.

## See Also

- [Stream Backpressure Guidelines](stream_backpressure_guidelines.md)
- [SPSC Queues and Streams](spsc_and_ipc.md)
- [Hierarchical Transport Pattern](hierarchical.md)

## How They Fit Together

### In-process DLL loading

Use one of these when you want a zone boundary without process isolation:

- `rpc::dynamic_library`
- `rpc::c_abi`
- `rpc::libcoro_host_scheduled_dynamic_library`
- `rpc::libcoro_dll_scheduled_dynamic_library`

In these cases the host and child zone live in the same operating-system
process, but the child implementation is loaded from a shared object at runtime.

### Out-of-process DLL loading

Use these together when you want process isolation as well as a DLL boundary:

- `rpc::ipc_transport` in the host process
- `canopy_ipc_child_host_process` as the spawned executable
- `rpc::libcoro_spsc_dynamic_dll` inside the child process

The ownership flow is:

1. `ipc_transport` creates a shared-memory SPSC queue pair
2. `ipc_transport` spawns `canopy_ipc_child_host_process`
3. the child process maps the queue pair and loads the DLL
4. `libcoro_spsc_dynamic_dll` hosts the child zone behind a
   `rpc::stream_transport`
5. when the transport disconnects, the child process exits and
   `ipc_transport` reaps it

### Out-of-process direct child service

The API still has a `direct_service` child process kind for process isolation
without a DLL:

- `rpc::ipc_transport` in the host process
- `canopy_ipc_child_process` as the spawned executable

However, the in-tree `canopy_ipc_child_process` target is currently disabled in
`c++/transports/ipc_transport/ipc_child_process/CMakeLists.txt`. Treat this mode
as unavailable until that target is reworked and re-enabled.

## Variants At A Glance

| Variant | Namespace / executable | Build mode | Host-side transport | Child-side runtime |
|--------|-------------------------|------------|---------------------|--------------------|
| Blocking DLL | `rpc::dynamic_library` | `CANOPY_BUILD_COROUTINE=OFF` | `transport_dynamic_library` | `transport_dynamic_library_dll` inside the loaded DLL |
| Blocking language-neutral DLL | `rpc::c_abi` | `CANOPY_BUILD_COROUTINE=OFF` | `transport_c_abi` | `transport_c_abi_dll` inside the loaded DLL |
| Host-scheduled coroutine DLL | `rpc::libcoro_host_scheduled_dynamic_library` | `CANOPY_BUILD_COROUTINE=ON` | `transport_libcoro_host_scheduled_dynamic_library` | `transport_libcoro_host_scheduled_dynamic_library_dll` inside the loaded DLL |
| DLL-scheduled coroutine DLL | `rpc::libcoro_dll_scheduled_dynamic_library` | `CANOPY_BUILD_COROUTINE=ON` | `transport_libcoro_dll_scheduled_dynamic_library` | `transport_libcoro_dll_scheduled_dynamic_library_dll` inside the loaded DLL |
| Coroutine SPSC DLL | `rpc::libcoro_spsc_dynamic_dll` | `CANOPY_BUILD_COROUTINE=ON` | usually reached via `rpc::ipc_transport` | `transport_libcoro_spsc_dll_runtime` inside the loaded DLL |
| Process-owned SPSC transport | `rpc::ipc_transport` | `CANOPY_BUILD_COROUTINE=ON` | `transport_ipc_transport` | `canopy_ipc_child_host_process`; direct child executable currently disabled |

## Entry Points At A Glance

| Variant | Export owned by child | User-implemented entry point |
|--------|------------------------|-------------------------------|
| Blocking DLL | `canopy_dll_*` | `canopy_dll_init` |
| Blocking language-neutral DLL | `canopy_dll_*` using `c_abi/dynamic_library/dynamic_library.h` types | `canopy_dll_init` |
| Host-scheduled coroutine DLL | `canopy_libcoro_host_scheduled_dll_create` plus direct coroutine function pointers | `canopy_libcoro_host_scheduled_dll_init` |
| DLL-scheduled coroutine DLL | `canopy_libcoro_dll_scheduled_dll_start` plus begin/complete callbacks | `canopy_libcoro_dll_scheduled_dll_init` |
| Coroutine SPSC DLL | `canopy_libcoro_spsc_dll_start` | `canopy_libcoro_spsc_dll_init` |

## Blocking Transport (`rpc::dynamic_library`)

In-process communication between a host zone and a child zone that lives inside a
dynamically loaded shared object (`.so` / `.dll`).  The child zone is loaded at
runtime via `dlopen` / `LoadLibrary` and communicates with the host through a
plain-C ABI boundary consisting of the `canopy_dll_*` entry points.

## When to Use

- Plugin architectures where child zones are supplied as shared libraries
- Isolating third-party code into its own zone without process separation
- Hot-swapping implementations (unload old DLL, load new one)
- Keeping child zone symbols private from the rest of the process

### Requirements

- Non-coroutine build only (`CANOPY_BUILD_COROUTINE` must be **OFF**)
- Linux: `libdl` (linked automatically by CMake)
- Windows: `Kernel32` (linked automatically)

### Architecture

```
Host Process
┌────────────────────────────────────────────────────────┐
│  Host Zone (zone 1)                                    │
│  ┌──────────────────────────────────────────────────┐  │
│  │  child_transport (transport_dynamic_library)     │  │
│  │  - holds lib_handle_ (dlopen result)             │  │
│  │  - holds dll_ctx_ (opaque DLL state)             │  │
│  │  - holds resolved canopy_dll_* fn pointers       │  │
│  └──────────────┬───────────────────────────────────┘  │
│                 │  canopy_dll_send / canopy_dll_*       │
│  ───────────────┼──────────── DLL boundary ────────    │
│                 │  host_send / host_* callbacks         │
│  ┌──────────────┴───────────────────────────────────┐  │
│  │  libmyplugin.so  (DLL zone, zone 2)              │  │
│  │  ┌────────────────────────────────────────────┐  │  │
│  │  │  parent_transport (transport_dynamic_       │  │  │
│  │  │                    library_dll)             │  │  │
│  │  │  - calls back to host via fn pointers       │  │  │
│  │  └────────────────────────────────────────────┘  │  │
│  │  ┌────────────────────────────────────────────┐  │  │
│  │  │  child_service + user implementation       │  │  │
│  │  └────────────────────────────────────────────┘  │  │
│  └──────────────────────────────────────────────────┘  │
└────────────────────────────────────────────────────────┘
```

The boundary is a set of C function pointers — no C++ name mangling, no vtable
crossing.  Both sides compile against the same `rpc/` headers and share the
same C++ runtime (same process), so passing `rpc::send_params*` across the
boundary is safe by layout compatibility.

### Two CMake Targets

| Target | Links into | Purpose |
|--------|-----------|---------|
| `transport_dynamic_library` | Host executable | Provides `child_transport`; resolves and calls `canopy_dll_*` |
| `transport_dynamic_library_dll` | Shared object payload | Provides all `canopy_dll_*` entry points; DLL author only writes `canopy_dll_init` |

### Host Side Setup

Create a `child_transport` with the path to the shared object and call
`connect_to_zone` as with any other hierarchical transport.  No factory lambda
is required — the factory lives inside the DLL.

```cpp
#include <transports/dynamic_library/transport.h>

// root_service already created
rpc::shared_ptr<yyy::i_host> host_ptr(new MyHostImpl());

auto child_transport = std::make_shared<rpc::dynamic_library::child_transport>(
    "plugin",           // transport name
    root_service,       // host service
    "/path/to/libmyplugin.so");  // shared object path

auto result = root_service->connect_to_zone<yyy::i_host, yyy::i_example>(
    "plugin", child_transport, host_ptr);

if (result.error_code != rpc::error::OK())
{
    // load or init failed — DLL was not loaded / canopy_dll_init returned error
}

rpc::shared_ptr<yyy::i_example> plugin = std::move(result.output_interface);
```

`connect_to_zone` calls `dlopen`, resolves all `canopy_dll_*` symbols, and
invokes `canopy_dll_init`.  If any step fails the transport is never marked
`CONNECTED` and the error code is returned.

### DLL Side Setup

Link `transport_dynamic_library_dll` into your shared object and provide one
function: `canopy_dll_init`.  All other entry points are compiled for you.

```cpp
// libmyplugin.cpp
#include <transports/dynamic_library/dll_transport.h>
#include <rpc/rpc.h>

// The user must also provide rpc_log (see "Logging" section below).

extern "C" CANOPY_DLL_EXPORT
int canopy_dll_init(rpc::dynamic_library::dll_init_params* params)
{
    return rpc::dynamic_library::init_child_zone<yyy::i_host, yyy::i_example>(
        params,
        [](rpc::shared_ptr<yyy::i_host> host,
           std::shared_ptr<rpc::child_service> svc)
            -> rpc::service_connect_result<yyy::i_example>
        {
            auto impl = rpc::shared_ptr<yyy::i_example>(
                new MyExampleImpl(svc, host));
            return {rpc::error::OK(), std::move(impl)};
        });
}
```

`init_child_zone<P, C>` creates the `parent_transport`, calls
`child_service::create_child_zone<P, C>`, invokes your factory, and writes the
opaque `dll_ctx` handle and the root object descriptor back into `*params`.

#### CMakeLists.txt for the shared object

```cmake
add_library(myplugin SHARED src/myplugin.cpp src/rpc_log.cpp)

target_compile_definitions(myplugin PRIVATE CANOPY_DLL_BUILDING)

target_link_libraries(myplugin PRIVATE
    transport_dynamic_library_dll
    rpc::rpc
    yas_common)

target_compile_options(myplugin PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-fvisibility=hidden>
    $<$<CXX_COMPILER_ID:GNU,Clang>:-fvisibility-inlines-hidden>)
```

### Symbol Visibility and Isolation

The DLL is opened with `RTLD_NOW | RTLD_LOCAL` on Linux (or `LoadLibraryA` on
Windows):

- **`RTLD_NOW`**: All undefined symbols are resolved at load time; missing
  symbols cause `dlopen` to fail immediately rather than crashing later.
- **`RTLD_LOCAL`**: The DLL's symbols are not added to the global symbol table.
  Host symbols are not visible inside the DLL, and DLL implementation symbols
  are not visible to the host or other loaded libraries.

Only the `canopy_dll_*` entry points carry
`__attribute__((visibility("default")))` (via `CANOPY_DLL_EXPORT`), making them
discoverable by `dlsym` even when the DLL is compiled with `-fvisibility=hidden`.

The practical consequence: the DLL has its **own statically linked copy** of
`librpc.a` and any other static dependencies.  This is intentional — zones are
designed to be separate worlds communicating only through the transport layer.

### Logging

Because `RTLD_LOCAL` hides host symbols, the DLL cannot use the host's
`rpc_log` function.  Every DLL payload must provide its own `rpc_log`:

```cpp
// rpc_log.cpp — compiled into the shared object
extern "C" void rpc_log(int level, const char* str, size_t sz)
{
    // Route to your preferred logging backend.
    // level: 0=trace 1=debug 2=info 3=warn 4=error 5=critical
    if (level >= 3)
        fprintf(stderr, "[plugin] %.*s\n", (int)sz, str);
}
```

### Module Shutdown Hooks

Every dynamic-library ABI now has an explicit module-level shutdown hook.  The
host calls it after the child context has been destroyed and before
`dlclose` / `FreeLibrary`:

- `rpc::dynamic_library`: required exported symbol `canopy_dll_shutdown`
- `rpc::c_abi`: required exported symbol `canopy_dll_shutdown`
- `rpc::libcoro_dll_scheduled_dynamic_library`: shutdown happens before
  `canopy_libcoro_dll_scheduled_dll_start` returns
- `rpc::libcoro_spsc_dynamic_dll`: runtime stop callback performs equivalent
  module shutdown after the worker thread and scheduler have stopped

The hook is intentionally context-free.  By the time it runs, the child service,
transport, and exported object graph should already have gone away.  It is for
module-level runtime cleanup only.

The immediate reason is full Google Protocol Buffers support.  Generated
`*.pb.cc` files and the protobuf runtime keep process/module static state.  If a
DLL or shared object statically links protobuf and is unloaded, the module must
call `google::protobuf::ShutdownProtobufLibrary()` after all protobuf use has
ended and before the shared object is unloaded.  Canopy's DLL helper libraries do
this automatically when compiled with `CANOPY_BUILD_PROTOCOL_BUFFERS`; when full
protobuf is not enabled the shutdown hook is a no-op.

### Lifetime and dlclose Safety

The shared object is kept loaded for as long as the host holds any proxy
reference into the DLL zone.

**When `dlclose` is safe:**  Only after all host-side proxy objects have been
released.  The base class calls `on_destination_count_zero()` at that point,
which is the only place `dlclose` is called.

**Why `set_status(DISCONNECTED)` does not call `dlclose`:**  Disconnect can be
triggered from inside a DLL callback (e.g. the DLL calls a host method which
propagates a transport-down notification back into the DLL).  At that moment
DLL code is still on the call stack.  `set_status` therefore only nulls the
function pointers so that subsequent outbound calls fail gracefully; the actual
unload is deferred until `on_destination_count_zero` (or the destructor if
`child_transport` is destroyed before all proxies are released — e.g. after an
init failure).

```
Sequence for normal shutdown:
  1. Host releases last rpc::shared_ptr<yyy::i_example>
  2. Proxy release propagates to DLL zone via outbound_release
  3. DLL zone ref-count reaches zero → child_service destructs
  4. parent_transport::set_status(DISCONNECTED) fires
     → notify_all_destinations_of_disconnect() called
     → host_ctx_ nulled (no further callbacks)
  5. Host inbound_transport_down handler runs
  6. Host proxy count drops to zero → on_destination_count_zero()
  7. canopy_dll_destroy called → DLL service/transport torn down
  8. canopy_dll_shutdown called → module runtime cleanup
  9. dlclose called → shared object unloaded
```

### Key Characteristics

| Property | Value |
|----------|-------|
| Build mode | Non-coroutine only |
| Symbol isolation | `RTLD_LOCAL` + `-fvisibility=hidden` |
| ABI boundary | Plain-C function pointers (`canopy_dll_*`) |
| Zone type | Hierarchical (parent/child) |
| DLL author writes | `canopy_dll_init` + `rpc_log` |
| dlclose timing | Deferred to `on_destination_count_zero` |
| Module shutdown | Required `canopy_dll_shutdown`, provided by `transport_dynamic_library_dll` |
| `CONNECTED` set | Inside `inner_connect`, after successful `canopy_dll_init` |

### Hierarchical Transport Pattern

The dynamic_library transport implements the same hierarchical parent/child
pattern as the local and SGX transports.  See
`documents/transports/hierarchical.md` for the circular-reference architecture,
stack-based lifetime protection, and safe disconnection protocol.

The key difference from the local transport is the boundary crossing mechanism:
instead of direct C++ function calls, every call crosses via C function pointers
stored during `canopy_dll_init`.

### Differences from Local Transport

| Aspect | Local | Dynamic Library |
|--------|-------|-----------------|
| Child factory | `set_child_entry_point` lambda | `canopy_dll_init` in DLL |
| Boundary | Direct C++ calls | C function pointer table |
| Symbol isolation | None (same address space) | `RTLD_LOCAL` + visibility |
| `child_service` access | Host holds `shared_ptr` | Opaque inside DLL |
| dlclose | N/A | Deferred to proxy count zero |
| Coroutine support | Yes | No |

### Limitations

- Non-coroutine builds only
- Both host and DLL must be built against compatible versions of `librpc.a`
  (same process; ABI must match)
- `RTLD_LOCAL` means the DLL cannot call back into host-side static library
  functions other than through the explicit `host_*` callback pointers
- One DLL instance per `child_transport`; to load the same `.so` multiple
  times create multiple `child_transport` objects

## Blocking C ABI Transport (`rpc::c_abi`)

`rpc::c_abi` is the non-coroutine dynamic-library transport for language-neutral
shared-object boundaries.  It uses the same high-level parent/child-zone shape as
`rpc::dynamic_library`, but the ABI structs and function pointer types are
defined in `c_abi/dynamic_library/dynamic_library.h` instead of passing
C++ RPC structs directly across the boundary.

Use this transport when the child may be implemented by C, Rust, or another
language that can expose a stable C ABI.  The C++ child helper
`transport_c_abi_dll` provides the same convenience pattern as the C++-specific
DLL transport: the child author supplies `canopy_dll_init`; the helper library
supplies the other `canopy_dll_*` entry points, including the required
`canopy_dll_shutdown` module cleanup hook.

The loader treats `canopy_dll_shutdown` as part of the required ABI.  Missing it
is a load failure, just like a missing `canopy_dll_send` or
`canopy_dll_release`.  This is deliberate because the ABI is still internal to
Canopy and shared objects that may link full protobuf need a reliable cleanup
point before unload.

## Host-Scheduled Coroutine Transport (`rpc::libcoro_host_scheduled_dynamic_library`)

This transport is the older coroutine DLL shape. It exchanges direct
`coro::task` function pointers through
`canopy_libcoro_host_scheduled_dll_create`, and both host-to-DLL and
DLL-to-host calls run on the host scheduler.

Because DLL coroutine code can execute on host scheduler worker threads,
`dlclose` is deferred until that scheduler has stopped. This avoids unloading
code that may still be referenced by thread-local destructors on those worker
threads. Use this variant to model host-scheduled plugin behaviour; use the
DLL-scheduled variant when transport teardown itself must stop the scheduler and
reset DLL-local static state immediately.

## DLL-Scheduled Coroutine Transport (`rpc::libcoro_dll_scheduled_dynamic_library`)

This transport provides the same high-level parent/child-zone behaviour, but
for coroutine builds.  It lives under
`transports/libcoro_dll_scheduled_dynamic_library/` and uses a distinct ABI so a coroutine DLL
cannot be mistaken for the blocking variant.

### Requirements

- Coroutine build only (`CANOPY_BUILD_COROUTINE` must be **ON**)
- Linux: `libdl` (linked automatically by CMake)
- Windows: `Kernel32` (linked automatically)

### Architecture

```
Host Process
┌───────────────────────────────────────────────────────────────┐
│  Host Zone                                                    │
│  ┌─────────────────────────────────────────────────────────┐  │
│  │ child_transport (transport_libcoro_dll_scheduled_dynamic_library)     │  │
│  │ - dlopen / LoadLibrary shared object                    │  │
│  │ - starts canopy_libcoro_dll_scheduled_dll_start on an entry thread    │  │
│  │ - stores DLL begin_* function pointers                  │  │
│  │ - awaits the DLL ready callback                         │  │
│  └──────────────────┬──────────────────────────────────────┘  │
│                     │ begin_* functions + completion callbacks│
│  ───────────────────┼──────────── DLL boundary ─────────────  │
│                     │ host begin_* callbacks                   │
│  ┌──────────────────┴──────────────────────────────────────┐  │
│  │ libmyplugin.so                                          │  │
│  │ ┌────────────────────────────────────────────────────┐  │  │
│  │ │ parent_transport                                   │  │  │
│  │ │ - calls host through host begin_* callbacks        │  │  │
│  │ │ - exposes DLL begin_* entry points                 │  │  │
│  │ └────────────────────────────────────────────────────┘  │  │
│  │ ┌────────────────────────────────────────────────────┐  │  │
│  │ │ child_service + user implementation               │  │  │
│  │ └────────────────────────────────────────────────────┘  │  │
│  └─────────────────────────────────────────────────────────┘  │
└───────────────────────────────────────────────────────────────┘
```

The host starts one dedicated entry thread for the shared object.  That thread
loads the library, calls `canopy_libcoro_dll_scheduled_dll_start`, and remains blocked there
until the DLL-side transport and service graph have shut down.  The DLL start
function creates the DLL-owned scheduler, runs `canopy_libcoro_dll_scheduled_dll_init`, and
publishes an opaque runtime handle plus a table of DLL `begin_*` functions via a
ready callback.

### Two CMake Targets

| Target | Links into | Purpose |
|--------|-----------|---------|
| `transport_libcoro_dll_scheduled_dynamic_library` | Host executable | Provides coroutine `child_transport`; loads the DLL and routes host-to-DLL calls through DLL `begin_*` functions |
| `transport_libcoro_dll_scheduled_dynamic_library_dll` | Shared object payload | Provides DLL-side transport helpers and ABI types for `canopy_libcoro_dll_scheduled_dll_start` |

### Host Side Setup

Create a `rpc::libcoro_dll_scheduled_dynamic_library::child_transport` and use it with the
normal coroutine `connect_to_zone` flow.

```cpp
#include <transports/libcoro_dll_scheduled_dynamic_library/transport.h>

auto child_transport = std::make_shared<rpc::libcoro_dll_scheduled_dynamic_library::child_transport>(
    "plugin",
    root_service,
    "/path/to/libmyplugin.so");

auto result = CO_AWAIT root_service->connect_to_zone<yyy::i_host, yyy::i_example>(
    "plugin", child_transport, host_ptr);

if (result.error_code != rpc::error::OK())
{
    CO_RETURN result.error_code;
}

rpc::shared_ptr<yyy::i_example> plugin = std::move(result.output_interface);
```

`inner_connect` performs two phases:

1. Start a DLL entry thread and call `canopy_libcoro_dll_scheduled_dll_start`
2. Await the ready callback that carries the child descriptor and DLL `begin_*` table

During setup the host also allocates the child zone ID and passes non-blocking
host `begin_*` callbacks such as `host_send`, `host_post`, and
`host_get_new_zone_id` into the DLL.

### DLL Side Setup

Link `transport_libcoro_dll_scheduled_dynamic_library_dll` into your shared object and provide
`rpc::libcoro_dll_scheduled_dynamic_library::canopy_libcoro_dll_scheduled_dll_init`.  The transport helper
library exports `canopy_libcoro_dll_scheduled_dll_start` for you.  That start function
constructs the DLL-side runtime and `parent_transport`, runs the user init
coroutine on the DLL-owned scheduler, publishes the DLL `begin_*` table to the
host, and returns only after the DLL-side transport has died and module-level
cleanup has run.

```cpp
#include <transports/libcoro_dll_scheduled_dynamic_library/dll_transport.h>

namespace rpc::libcoro_dll_scheduled_dynamic_library
{
CORO_TASK(rpc::connect_result) canopy_libcoro_dll_scheduled_dll_init(
    void* ctx,
    const rpc::connection_settings* settings)
{
    return init_child_zone<yyy::i_host, yyy::i_example>(
        ctx,
        settings,
        [](rpc::shared_ptr<yyy::i_host> host,
           std::shared_ptr<rpc::child_service> svc)
            -> CORO_TASK(rpc::service_connect_result<yyy::i_example>)
        {
            CO_RETURN {rpc::error::OK(), rpc::make_shared<MyExampleImpl>(svc, host)};
        });
}
} // namespace rpc::libcoro_dll_scheduled_dynamic_library
```

### CMakeLists.txt for the shared object

```cmake
add_library(myplugin SHARED src/myplugin.cpp src/rpc_log.cpp)

target_link_libraries(myplugin PRIVATE
    transport_libcoro_dll_scheduled_dynamic_library_dll
    rpc::rpc
    yas_common)

target_compile_options(myplugin PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-fvisibility=hidden>
    $<$<CXX_COMPILER_ID:GNU,Clang>:-fvisibility-inlines-hidden>)
```

### ABI and Symbol Differences

- The coroutine transport exports `canopy_libcoro_dll_scheduled_dll_start`, not
  `canopy_dll_init`
- The DLL author provides `canopy_libcoro_dll_scheduled_dll_init`; the helper library provides
  `canopy_libcoro_dll_scheduled_dll_start`
- Host and DLL exchange non-blocking `begin_*` function pointers and completion
  callbacks
- The DLL owns the scheduler used for its service graph; the host stores only an
  opaque runtime handle and exported `begin_*` function pointers
- Host-to-DLL calls enqueue work on the DLL scheduler and complete through a
  host-owned callback.  DLL-to-host calls mirror this with host `begin_*`
  callbacks and DLL-owned completions.
- The DLL entry thread remains blocked in `canopy_libcoro_dll_scheduled_dll_start` until the
  DLL-side transport has died, then shuts down the DLL scheduler and returns.
- The entry thread waits on an internal finish gate rather than scheduler task
  counts; transport lifetime remains driven by the service graph.
- The separate `canopy_libcoro_dll_scheduled_dll_*` naming prevents loading a blocking DLL by
  mistake

### Lifetime and Unload Behaviour

The coroutine variant also uses normal unload semantics.  On Linux it opens the
DLL with `RTLD_NOW | RTLD_LOCAL`, not `RTLD_NODELETE`.  The design goal is that a
shared object or enclave can shut down cleanly, release its service graph, run
module-level cleanup such as protobuf shutdown, and then be unloaded.

As implemented in `transports/libcoro_dll_scheduled_dynamic_library/src/transport.cpp`:

- `on_destination_count_zero()` does not unload the library
- `child_transport` unloads the DLL only after joining the DLL entry thread
- `canopy_libcoro_dll_scheduled_dll_start` returns only after the DLL-side parent transport has
  died
- the DLL scheduler and module-level cleanup run before the entry function
  returns
- ready/finish handshakes use mutex-backed one-shot state so cross-thread
  coroutine resumption is explicit to ThreadSanitizer
- function pointers are nulled before `dlclose` / `FreeLibrary`

### Key Characteristics

| Property | Value |
|----------|-------|
| Build mode | Coroutine only |
| Symbol isolation | `RTLD_LOCAL` + `-fvisibility=hidden` |
| ABI boundary | Plain-C start function plus opaque runtime handle, begin functions, and completion callbacks |
| Zone type | Hierarchical (parent/child) |
| DLL author writes | `canopy_libcoro_dll_scheduled_dll_init` |
| Module shutdown | Natural return from `canopy_libcoro_dll_scheduled_dll_start` after the DLL-side transport dies |
| Linux load flags | `RTLD_NOW | RTLD_LOCAL` |
| Host connect flow | Start entry thread, then await DLL ready callback |

### Differences From Blocking Dynamic Library Transport

| Aspect | Blocking `dynamic_library` | Coroutine `libcoro_dll_scheduled_dynamic_library` |
|--------|----------------------------|-------------------------------------|
| Build mode | Non-coroutine only | Coroutine only |
| Primary entry point | `canopy_dll_init` | `canopy_libcoro_dll_scheduled_dll_start` |
| Cross-boundary calls | Plain C function table | Non-blocking begin functions plus completion callbacks |
| Host setup | Load and init inside `inner_connect` | Start DLL entry thread, then await ready callback |
| DLL unload trigger | Deferred until proxy count reaches zero | Deferred until `child_transport` destruction |
| Linux unload safety | Normal `dlclose` after `canopy_dll_shutdown` | Normal `dlclose` after `canopy_libcoro_dll_scheduled_dll_start` returns |

## SPSC DLL Transport (`rpc::libcoro_spsc_dynamic_dll`)

`rpc::libcoro_spsc_dynamic_dll` is the coroutine DLL runtime used when the DLL
is not loaded by the host directly. Instead, the DLL is given an already-open
SPSC queue pair and exposes its child zone behind a `rpc::stream_transport`.

This transport exists to separate two concerns that used to be coupled:

- how to host a child zone inside a DLL
- how to get bytes to that DLL across a process boundary

`libcoro_spsc_dynamic_dll` owns only the first concern. It assumes the queue
pair already exists and does not spawn processes or create shared-memory files.

### What It Does

- starts a dedicated coroutine scheduler for the loaded DLL
- creates a `streaming::spsc_queue::stream` over the supplied queue pair
- calls the user-provided `canopy_libcoro_spsc_dll_init(...)`
- hosts the DLL child zone behind `rpc::stream_transport`
- notifies the embedding host runtime when the parent transport expires
- stops the worker thread and scheduler before running module-level cleanup such
  as protobuf shutdown

### What It Does Not Do

- it does not create the shared-memory file
- it does not spawn or reap a child process
- it does not parse process-launch policy

Those responsibilities belong to `rpc::ipc_transport` and the small child
executables in `transports/ipc_transport/`.

## IPC Process Transport (`rpc::ipc_transport`)

`rpc::ipc_transport` is a real transport derived from
`rpc::stream_transport::transport`. Its purpose is to encapsulate:

- creation of the shared-memory SPSC queue pair
- spawning of the child process executable
- optional OS-enforced child termination if the parent dies unexpectedly
- reaping the child process when the transport disconnects

It is intentionally separate from the DLL runtime. The child process may do one
of two things:

- run `canopy_ipc_child_host_process`, which maps the queue pair and forwards it
  into a `libcoro_spsc_dynamic_dll` DLL
- run a direct child process that maps the queue pair and hosts a
  `rpc::stream_transport` directly; the in-tree
  `canopy_ipc_child_process` executable for this mode is currently disabled and
  still hardcodes the example test interfaces in its source

### Why This Split Matters

Separating the transport from the child runtime gives three independent
building blocks:

- DLL transport without process isolation: `dynamic_library` /
  `c_abi` / `libcoro_host_scheduled_dynamic_library` /
  `libcoro_dll_scheduled_dynamic_library`
- process isolation plus DLL hosting: `ipc_transport` +
  `canopy_ipc_child_host_process` + `libcoro_spsc_dynamic_dll`
- process isolation plus direct child service hosting: `ipc_transport` +
  `canopy_ipc_child_process` once that executable is reworked and re-enabled

That keeps each component responsible for one boundary:

- DLL ABI boundary
- stream/message boundary
- process lifetime boundary

### Parent-death behaviour

On Linux, `rpc::ipc_transport::options::kill_child_on_parent_death` uses
`PR_SET_PDEATHSIG(SIGKILL)` in the forked child before `execve()`. A readiness
pipe ensures this contract is armed before the parent returns from transport
construction.

Clean child-process shutdown should go through ordinary C++ teardown. The
`canopy_ipc_child_host_process` success path returns normally after the SPSC/DLL
runtime has stopped; immediate process termination is reserved for setup
failures or test harness failure paths where normal unwinding is not possible.

## See Also

- `transports/dynamic_library/include/transports/dynamic_library/dll_abi.h` — C ABI types
- `transports/dynamic_library/include/transports/dynamic_library/transport.h` — `child_transport`
- `transports/dynamic_library/include/transports/dynamic_library/dll_transport.h` — `parent_transport`, `init_child_zone`, `dll_context`
- `c_abi/dynamic_library/dynamic_library.h` — language-neutral dynamic-library ABI
- `transports/c_abi/include/transports/c_abi/transport.h` — C ABI `child_transport`
- `transports/c_abi/include/transports/c_abi/dynamic_library_loader.h` — C ABI loader and required entry-point table
- `transports/libcoro_host_scheduled_dynamic_library/include/transports/libcoro_host_scheduled_dynamic_library/dll_abi.h` — host-scheduled coroutine DLL ABI types
- `transports/libcoro_host_scheduled_dynamic_library/include/transports/libcoro_host_scheduled_dynamic_library/transport.h` — host-scheduled coroutine `child_transport`
- `transports/libcoro_host_scheduled_dynamic_library/include/transports/libcoro_host_scheduled_dynamic_library/dll_transport.h` — host-scheduled coroutine `parent_transport`, `init_child_zone`
- `transports/libcoro_dll_scheduled_dynamic_library/include/transports/libcoro_dll_scheduled_dynamic_library/dll_abi.h` — coroutine DLL ABI types
- `transports/libcoro_dll_scheduled_dynamic_library/include/transports/libcoro_dll_scheduled_dynamic_library/transport.h` — coroutine `child_transport`
- `transports/libcoro_dll_scheduled_dynamic_library/include/transports/libcoro_dll_scheduled_dynamic_library/dll_transport.h` — coroutine `parent_transport`, `init_child_zone`
- `transports/libcoro_spsc_dynamic_dll/include/transports/libcoro_spsc_dynamic_dll/dll_abi.h` — SPSC DLL ABI types and queue-pair definition
- `transports/libcoro_spsc_dynamic_dll/include/transports/libcoro_spsc_dynamic_dll/dll_transport.h` — `canopy_libcoro_spsc_dll_init` contract
- `transports/ipc_transport/include/transports/ipc_transport/transport.h` — process-owning transport
- `transports/ipc_transport/include/transports/ipc_transport/bootstrap.h` — named child-process bootstrap arguments
- `documents/transports/hierarchical.md` — Hierarchical transport pattern
- `documents/transports/local.md` — Local transport (conceptual peer)
- `documents/transports/spsc_and_ipc.md` — SPSC queue and stream background
