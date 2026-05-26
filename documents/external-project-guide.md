<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Creating an External Project with Canopy

This guide covers the end-to-end process of building a new C++ application that
uses Canopy as a dependency via `add_subdirectory`.

> **Source-of-truth note:** This document describes patterns validated against the
> live repository. Always verify CMake variable names and API details from the actual
> source files rather than relying solely on this document.

---

## Directory Layout

Place your project adjacent to (or anywhere relative to) the Canopy checkout.
No system install of Canopy is required.

```
projects/
├── Canopy/          ← Canopy checkout
└── my_app/           ← your project
    ├── CMakeLists.txt
    ├── CMakePresets.json
    ├── idl/
    │   ├── CMakeLists.txt
    │   └── my_service/
    │       └── my_service.idl
    └── src/
        ├── server.cpp
        └── client.cpp
```

---

## CMakePresets.json

Your project needs its own presets. The only mandatory cache variable alongside
`CMAKE_BUILD_TYPE` is the compiler pair. 

```json
{
  "version": 3,
  "configurePresets": [
    {
      "name": "Coroutine",
      "displayName": "Debug coroutine build",
      "generator": "Ninja",
      "binaryDir": "${sourceDir}/build_coroutine",
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Debug",
        "CMAKE_C_COMPILER": "clang",
        "CMAKE_CXX_COMPILER": "clang++"
      }
    },
    {
      "name": "Release_Coroutine",
      "displayName": "Release coroutine build",
      "generator": "Ninja",
      "binaryDir": "${sourceDir}/build_release_coroutine",
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Release",
        "CMAKE_C_COMPILER": "clang",
        "CMAKE_CXX_COMPILER": "clang++"
      }
    }
  ]
}
```

---

## Root CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.24)
project(
  my_app
  VERSION 1.0.0
  LANGUAGES C CXX)          # C is required — Canopy submodules need it

# --- Canopy options (set BEFORE add_subdirectory) ---
set(CANOPY_BUILD_COROUTINE  ON  CACHE BOOL "" FORCE)  # TCP examples below use coroutines
set(CANOPY_BUILD_TEST       OFF CACHE BOOL "" FORCE)
set(CANOPY_BUILD_RUST       OFF CACHE BOOL "" FORCE)
set(CANOPY_BUILD_DEMOS      OFF CACHE BOOL "" FORCE)
set(CANOPY_BUILD_BENCHMARKING OFF CACHE BOOL "" FORCE)

# Optional serialization choices. Full protobuf is useful for host interop.
# Nanopb is protobuf-compatible and is the preferred SGX/small-runtime path.
set(CANOPY_BUILD_PROTOCOL_BUFFERS ON CACHE BOOL "" FORCE)
set(CANOPY_BUILD_NANOPB           ON CACHE BOOL "" FORCE)

add_subdirectory(../Canopy canopy_build)

# Output layout
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)

# IDL code generation (must come before executables that use it)
add_subdirectory(idl)

# Executables — link against the generated IDL target and Canopy transports
add_executable(server src/server.cpp)
target_compile_definitions(server PRIVATE ${CANOPY_DEFINES})
target_include_directories(server PRIVATE ${CANOPY_INCLUDES})
target_compile_options(server PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(server PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
target_link_libraries(
  server
  PRIVATE my_service_idl         # CanopyGenerate(my_service …) → my_service_idl
          transport_streaming
          streaming_tcp
          rpc
          canopy_network_config
          ${CANOPY_LIBRARIES})

# Repeat for client, etc.
```

### Key CMake variables provided by Canopy

| Variable | Contents |
|---|---|
| `CANOPY_DEFINES` | Compile definitions (`CANOPY_BUILD_COROUTINE`, encoding, etc.) |
| `CANOPY_INCLUDES` | Include paths for all Canopy headers |
| `CANOPY_COMPILE_OPTIONS` | Compiler flags (`-std=gnu++20`, `-fexceptions`, etc.) |
| `CANOPY_WARN_OK` | Warning suppressions for Canopy-generated code |
| `CANOPY_LIBRARIES` | Runtime libraries (libcoro, protobuf, fmt, etc.) |
| `CANOPY_LINK_EXE_OPTIONS` | Linker flags for executables |

### Protobuf and Nanopb

Canopy has two protobuf-compatible C++ backends:

- `CANOPY_BUILD_PROTOCOL_BUFFERS=ON` enables the full Google C++ protobuf runtime.
- `CANOPY_BUILD_NANOPB=ON` enables the Nanopb-backed runtime.

Both use generated `.proto` schemas and protobuf wire bytes. For normal host processes, full protobuf is appropriate when you need the Google generated C++ API or other full-runtime features. For SGX enclaves or other small-runtime deployments, prefer Nanopb so generated code does not link `protobuf::libprotobuf` into generated runtime targets.

The two build options are independent.  `CanopyGenerate(... protocol_buffers
...)` requests protobuf-compatible schema/wire support for that IDL target.  If
full protobuf is enabled, Canopy generates the Google C++ protobuf backend; if
Nanopb is enabled, Canopy generates the Nanopb backend.  Both can be generated
from the same IDL target.

When only one protobuf-compatible backend is enabled, Canopy maps the other
encoding to it.  `rpc::encoding::protocol_buffers` uses Nanopb when full
protobuf is disabled, and `rpc::encoding::nanopb` uses the full protobuf backend
when Nanopb is disabled.

The two build options are independent.  `CanopyGenerate(... protocol_buffers
...)` requests protobuf-compatible schema/wire support for that IDL target.  If
full protobuf is enabled, Canopy generates the Google C++ protobuf backend; if
Nanopb is enabled, Canopy generates the Nanopb backend.  Both can be generated
from the same IDL target.

When only one protobuf-compatible backend is enabled, Canopy maps the other
encoding to it.  `rpc::encoding::protocol_buffers` uses Nanopb when full
protobuf is disabled, and `rpc::encoding::nanopb` uses the full protobuf backend
when Nanopb is disabled.  In SGX enclave targets, full protobuf is stripped from
the enclave compile definitions, so `protocol_buffers` requests are routed
through Nanopb even if the host side of the same build still has full protobuf
enabled.

Nanopb still needs protobuf tooling at build time. That is separate from the runtime dependency of your generated targets.

If an external project builds Canopy-powered DLLs/shared objects and enables full
protobuf, the module must run Canopy's dynamic-library shutdown hook before
unload.  The built-in DLL helper libraries provide this hook:
`canopy_dll_shutdown` for the non-coroutine C++ and language-neutral C ABI
transports, and before the coroutine dynamic-library entry point returns.  Those hooks
call `google::protobuf::ShutdownProtobufLibrary()` when
`CANOPY_BUILD_PROTOCOL_BUFFERS` is enabled.

### Important: `LANGUAGES C CXX`

Always declare both C and C++ in `project()`. Several Canopy submodules (c-ares,
protobuf internals) require the C compiler. Omitting `C` causes a CMake configure
error: *"CMAKE_C_COMPILE_OBJECT not set"*.

---

## IDL Subdirectory

### `idl/CMakeLists.txt`

```cmake
cmake_minimum_required(VERSION 3.24)

CanopyGenerate(
  my_service                          # name → produces my_service_idl target
  my_service/my_service.idl           # path relative to this CMakeLists.txt
  ${CMAKE_CURRENT_SOURCE_DIR}         # base_dir for IDL resolution
  ${CMAKE_BINARY_DIR}/generated       # output root (stays in build tree)
  ""                                  # namespace override (empty = use IDL namespace)
  yas_binary
  yas_compressed_binary
  yas_json
  install_dir ${GENERATED_INSTALL_DIR})
```

`CanopyGenerate(name …)` creates:
- `${name}_idl` — static library target (link this from your executables)
- `${name}_idl_generate` — custom target that runs the generator

Generated headers land at:
```
${CMAKE_BINARY_DIR}/generated/include/<subdir>/<name>.h
${CMAKE_BINARY_DIR}/generated/include/<subdir>/<name>_stub.h
```

Linking `target_link_libraries(my_exe PRIVATE my_service_idl)` is sufficient to
pull in the generated include paths transitively and establish the correct build
ordering.

### IDL syntax

```idl
namespace my_app
{
    interface i_my_service
    {
        [description="Do a thing"]
        int my_method([in] const std::string& input, [out] std::string& output);

        [description="Add two numbers"]
        int add(int a, int b, [out] int& result);
    };
}
```

- Methods always return `int` (the RPC error code).
- Output parameters are marked `[out]` and passed by reference.
- Input parameters are marked `[in]` for non-trivial types; plain value types need no annotation.
- Use `rpc::shared_ptr<i_other>` to pass interface references across zones.

Generated header to include in your source: `<subdir/name.h>` — e.g.
`#include <my_service/my_service.h>`.
Do **not** include `_stub.h` or `_proxy.h` directly; `name.h` is the public header.

---

## Server Implementation

```cpp
#include <rpc/rpc.h>
#include <streaming/listener.h>
#include <streaming/tcp/acceptor.h>
#include <streaming/tcp/stream.h>
#include <transports/streaming/transport.h>
#include <canopy/network_config/network_args.h>
#include <my_service/my_service.h>   // generated header

// Required when telemetry is disabled — Canopy macros call this
void rpc_log(int level, const char* str, size_t sz)
{
    std::string msg(str, sz);
    const char* prefix[] = {"[TRACE]","[DEBUG]","[INFO] ","[WARN] ","[ERROR]"};
    printf("%s %s\n", level < 5 ? prefix[level] : "[LOG]", msg.c_str());
}

// Server-side implementation
class my_service_impl : public rpc::base<my_service_impl, my_app::i_my_service>
{
public:
    CORO_TASK(int) my_method(const std::string& input, std::string& output) override
    {
        output = "Processed: " + input;
        CO_RETURN rpc::error::OK();
    }

    CORO_TASK(int) add(int a, int b, int& result) override
    {
        result = a + b;
        CO_RETURN rpc::error::OK();
    }
};

// Coroutine entry point for the server
CORO_TASK(int) run_server(
    std::shared_ptr<coro::scheduler> scheduler,
    const canopy::network_config::network_config& cfg,
    rpc::event& shutdown)
{
    const auto* listen = cfg.first_listen();
    if (!listen)
        CO_RETURN 1;

    const auto domain = listen->family == canopy::network_config::ip_address_family::ipv6
        ? coro::net::domain_t::ipv6 : coro::net::domain_t::ipv4;
    const coro::net::socket_address endpoint{
        coro::net::ip_address::from_string(listen->to_string(), domain), listen->port};

    auto allocator = canopy::network_config::make_allocator(cfg);
    auto server_zone = allocator.allocate_zone();

    auto on_shutdown = std::make_shared<rpc::event>();
    auto service = rpc::root_service::create(
        "my_server", server_zone, scheduler);
    service->set_shutdown_event(on_shutdown);

    auto listener = std::make_shared<streaming::listener>(
        "server_transport",
        std::make_shared<streaming::tcp::acceptor>(endpoint),
        rpc::stream_transport::make_connection_callback<my_app::i_my_service, my_app::i_my_service>(
            [](const rpc::shared_ptr<my_app::i_my_service>&,
               const std::shared_ptr<rpc::service>&)
               -> CORO_TASK(rpc::service_connect_result<my_app::i_my_service>)
            {
                CO_RETURN rpc::service_connect_result<my_app::i_my_service>{
                    rpc::error::OK(),
                    rpc::shared_ptr<my_app::i_my_service>(new my_service_impl())};
            }));

    if (!listener->start_listening(service))
        CO_RETURN 1;

    service.reset();   // listener holds service alive from here

    co_await shutdown.wait();

    co_await listener->stop_listening();
    listener.reset();
    co_await on_shutdown->wait();
    CO_RETURN 0;
}

int main(int argc, char* argv[])
{
    args::ArgumentParser parser("my server");
    args::HelpFlag help(parser, "help", "Help", {'h', "help"});
    auto cfg = canopy::network_config::parse_network_args(argc, argv, parser);
    cfg.log_values();

    auto scheduler = std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
        coro::scheduler::options{
            .thread_strategy = coro::scheduler::thread_strategy_t::spawn,
            .pool = coro::thread_pool::options{.thread_count = 4},
            .execution_strategy = coro::scheduler::execution_strategy_t::process_tasks_on_thread_pool}));

    rpc::event shutdown;
    // wire up signal handler to call shutdown.set()

    int result = coro::sync_wait(run_server(scheduler, cfg, shutdown));
    scheduler->shutdown();
    return result;
}
```

---

## Client Implementation

```cpp
#include <rpc/rpc.h>
#include <streaming/tcp/stream.h>
#include <transports/streaming/transport.h>
#include <canopy/network_config/network_args.h>
#include <my_service/my_service.h>

// rpc_log() required here too (see server example above)

CORO_TASK(int) run_client(
    std::shared_ptr<coro::scheduler> scheduler,
    const canopy::network_config::network_config& cfg)
{
    const auto* remote = cfg.first_connect();
    if (!remote)
        CO_RETURN 1;

    auto allocator = canopy::network_config::make_allocator(cfg);
    auto client_zone = allocator.allocate_zone();

    auto client_service = rpc::root_service::create(
        "my_client", client_zone, scheduler);

    const auto domain = remote->family == canopy::network_config::ip_address_family::ipv6
        ? coro::net::domain_t::ipv6 : coro::net::domain_t::ipv4;

    coro::net::tcp::client tcp_client(scheduler,
        coro::net::socket_address{
            coro::net::ip_address::from_string(remote->to_string(), domain), remote->port});

    auto status = CO_AWAIT tcp_client.connect(std::chrono::milliseconds(5000));
    if (status != coro::net::connect_status::connected)
        CO_RETURN 1;

    auto tcp_stm = std::make_shared<streaming::tcp::stream>(std::move(tcp_client), scheduler);
    auto transport = rpc::stream_transport::make_client(
        "client_transport", client_service, std::move(tcp_stm));

    auto result = CO_AWAIT client_service->connect_to_zone<
        my_app::i_my_service, my_app::i_my_service>(
            "my_server", transport, rpc::shared_ptr<my_app::i_my_service>());

    if (result.error_code != rpc::error::OK())
        CO_RETURN 1;

    auto remote = result.output_interface;

    std::string output;
    CO_AWAIT remote->my_method("hello", output);

    remote.reset();
    CO_RETURN 0;
}
```

---

## TCP Link Libraries

Both server and client executables need at minimum:

```cmake
target_link_libraries(my_exe PRIVATE
    my_service_idl
    transport_streaming
    streaming_tcp
    rpc
    canopy_network_config
    ${CANOPY_LIBRARIES})
```

For non-TCP (local/in-process) builds replace `transport_streaming streaming_tcp`
with `transport_local`.

---

## Network Configuration CLI

`canopy::network_config::add_network_args(parser)` registers:

| Flag | Default | Meaning |
|---|---|---|
| `--va-name <name>` | first virtual address becomes default | Name of a logical zone identity |
| `--va-type <local\\|ipv4\\|ipv6\\|ipv6_tun>` | `local` | Address kind for the virtual address |
| `--va-prefix <addr>` | auto / explicit | Routing prefix for that virtual address |
| `--va-subnet-bits <n>` | from IDL defaults | Subnet field width |
| `--va-subnet <value>` | `0` | Initial subnet value |
| `--va-object-id-bits <n>` | from IDL defaults | Object-id field width |
| `--va-object-id <value>` | `0` | Initial object-id value |
| `--listen [name:]addr:port` | none | Physical listening endpoint mapped to a virtual address |
| `--connect [name:]addr:port` | none | Physical outbound endpoint mapped to a virtual address |

In practice, most applications should:

- create at least one virtual address
- provide at least one `--listen` endpoint for servers or one `--connect`
  endpoint for clients
- use `cfg.first_listen()` / `cfg.first_connect()` for the simplest single-endpoint setup

---

## Known CMake Pitfalls

### `CMAKE_SOURCE_DIR` vs `CMAKE_CURRENT_LIST_DIR` in included modules

When Canopy's cmake modules are `include()`d or called as functions from a
downstream project, `CMAKE_SOURCE_DIR` points to the downstream project root,
not Canopy's root. Inside a CMake *function*, `CMAKE_CURRENT_LIST_DIR` is the
*caller's* directory, not the file that defines the function.

Canopy works around this by capturing module-relative paths at include time:

```cmake
# At module scope (outside any function), CMAKE_CURRENT_LIST_DIR is reliable:
set(_CANOPY_GENERATE_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}" CACHE INTERNAL "")
```

### Generated header availability

`target_link_libraries(my_exe PRIVATE my_service_idl)` creates a build-level
dependency that ensures the generator runs before the library is compiled. The
transitive include directories from `my_service_idl` make the generated headers
visible to `my_exe`. No additional `add_dependencies()` calls are needed.

---

## Working Example

A complete working example lives at `/var/home/edward/projects/test_app/`.
It implements a `server` and `client` communicating over TCP using the
`greeting_app::i_greeter` interface. The five essential files are:

- `CMakeLists.txt` — root build
- `CMakePresets.json` — Coroutine and Release_Coroutine presets
- `idl/CMakeLists.txt` — `CanopyGenerate()` call
- `idl/greeting/greeting.idl` — interface definition
- `src/server.cpp` and `src/client.cpp` — implementation
