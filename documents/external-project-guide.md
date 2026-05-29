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

Your project should define its own presets. At minimum, set `CMAKE_BUILD_TYPE`
and the compiler pair. Set Canopy build options before `add_subdirectory()` so
the embedded Canopy build does not enable tests, demos, benchmarks, or language
workspaces that the consuming project does not need.

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
# Set ON for the coroutine TCP examples below. Set OFF, or omit this line, for
# blocking builds that attach an rpc::blocking_executor to stream-backed services.
set(CANOPY_BUILD_COROUTINE  ON  CACHE BOOL "" FORCE)
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
          connection_factory
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
- Use `rpc::optional<T>` and `rpc::variant<Ts...>` for IDL sum types. The generator
  rejects `std::optional` and `std::variant` so JSON schema, YAS, protobuf, and
  Nanopb all use the same wire shape.

Generated header to include in your source: `<subdir/name.h>` — e.g.
`#include <my_service/my_service.h>`.
Do **not** include `_stub.h` or `_proxy.h` directly; `name.h` is the public header.

---

## Server Implementation

```cpp
#include <rpc/rpc.h>
#include <connection_factory/tcp.h>
#include <canopy/network_config/cli_args.h>
#include <canopy/network_config/zone.h>
#include <connection_factory_config/connection_factory_config.h>
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

    auto allocator = canopy::network_config::make_allocator(cfg);
    rpc::zone_address server_zone_addr;
    auto zone_error = allocator.allocate_zone(server_zone_addr);
    if (zone_error != rpc::error::OK())
        CO_RETURN zone_error;
    rpc::zone server_zone{server_zone_addr};

    auto on_shutdown = std::make_shared<rpc::event>();
    auto service = rpc::root_service::create(
        "my_server", server_zone, scheduler);
    service->set_shutdown_event(on_shutdown);

    rpc::connection_factory_config::stream_factory_options options;
    auto& endpoint = options.endpoint.emplace();
    endpoint.host = listen->to_string();
    endpoint.port = listen->port;
    options.service.emplace().name = std::string("my_server");
    options.transport.emplace().name = std::string("server_transport");
    options.listener.emplace().name = std::string("server_listener");
    options.rpc.emplace().encoding = std::string("nanopb");

    auto listener = CO_AWAIT rpc::tcp::accept_rpc<my_app::i_my_service, my_app::i_my_service>(
        rpc::shared_ptr<my_app::i_my_service>(new my_service_impl()),
        options,
        service);
    if (listener.error_code != rpc::error::OK())
        CO_RETURN listener.error_code;

    CO_AWAIT shutdown.wait();

    CO_AWAIT listener.handle->stop();
    CO_AWAIT on_shutdown->wait();
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
#include <connection_factory/tcp.h>
#include <canopy/network_config/cli_args.h>
#include <canopy/network_config/zone.h>
#include <connection_factory_config/connection_factory_config.h>
#include <my_service/my_service.h>

// rpc_log() required here too (see server example above)

CORO_TASK(int) run_client(
    std::shared_ptr<coro::scheduler> scheduler,
    const canopy::network_config::network_config& cfg)
{
    const auto* remote_cfg = cfg.first_connect();
    if (!remote_cfg)
        CO_RETURN 1;

    auto allocator = canopy::network_config::make_allocator(cfg);
    rpc::zone_address client_zone_addr;
    auto zone_error = allocator.allocate_zone(client_zone_addr);
    if (zone_error != rpc::error::OK())
        CO_RETURN zone_error;
    rpc::zone client_zone{client_zone_addr};

    auto client_service = rpc::root_service::create(
        "my_client", client_zone, scheduler);

    rpc::connection_factory_config::stream_factory_options options;
    auto& endpoint = options.endpoint.emplace();
    endpoint.host = remote_cfg->to_string();
    endpoint.port = remote_cfg->port;
    options.service.emplace().name = std::string("my_client");
    options.transport.emplace().name = std::string("client_transport");
    options.connection.emplace().name = std::string("my_server");
    options.rpc.emplace().encoding = std::string("nanopb");

    auto result = CO_AWAIT rpc::tcp::connect_rpc<my_app::i_my_service, my_app::i_my_service>(
        rpc::shared_ptr<my_app::i_my_service>(),
        options,
        client_service);

    if (result.error_code != rpc::error::OK())
        CO_RETURN 1;

    auto remote_service = result.output_interface;

    std::string output;
    CO_AWAIT remote_service->my_method("hello", output);

    remote_service.reset();
    CO_RETURN 0;
}
```

---

## TCP Link Libraries

Both server and client executables need at minimum:

```cmake
target_link_libraries(my_exe PRIVATE
    my_service_idl
    connection_factory
    rpc
    canopy_network_config
    ${CANOPY_LIBRARIES})
```

For local/in-process-only builds, link `transport_local` instead of
`connection_factory`.

The stream factory overloads accept either the typed
`rpc::connection_factory_config::stream_factory_options` object or a raw
`json::v1::object`. Prefer the typed object inside application code. It is
generated from `connection_factory_config.idl`, so option names and value types
are kept in one place and the compiler catches misspelled fields.

Use raw JSON at configuration boundaries: config files, config blobs, tests, and
command-line overlays. That JSON is validated against the generated schema and
uses exact nested keys: `endpoint.host`, `endpoint.port`, `endpoint.ipv6`,
`endpoint.connect_timeout`, `service.name`, `transport.name`, `listener.name`,
`connection.name`, `rpc.encoding`, `rpc.call_timeout`,
`rpc.call_timeout_sweep`, and `rpc.shutdown_timeout`. Legacy flat aliases such
as `service_name`, `transport_name`, or top-level `port` are rejected.

`json/config_loader.h` provides the usual file/config boundary. Its merge order
is:

```
JSON schema defaults < component defaults < config-file values < CLI overrides
```

```cpp
#include <json/config_loader.h>
#include <connection_factory_config/connection_factory_config.h>
#include <connection_factory_config/connection_factory_config_schema.h>

auto schema = json::v1::parse(
    rpc::connection_factory_config::stream_factory_options::get_schema(rpc::encoding::yas_json));

json::v1::object component_defaults{json::v1::map{
    {"rpc", json::v1::map{{"encoding", "nanopb"}}},
}};

json::v1::object cli_overrides{json::v1::map{
    {"endpoint", json::v1::map{{"port", uint16_t{8080}}}},
}};

auto options =
    json::v1::load_typed_config_file<rpc::connection_factory_config::stream_factory_options>(
        schema, "server.json", component_defaults, cli_overrides);
```

## Blocking TCP Variant

For a blocking external project, keep `CANOPY_BUILD_COROUTINE=OFF` and use C++17.
Plain in-process RPC does not need a thread pool, but stream-backed TCP, TLS, and
WebSocket transports do. Construct the owning service with an
`rpc::blocking_executor` and use the same `rpc::tcp` helper API:

```cpp
auto exec = std::make_shared<rpc::blocking_executor>();
auto service = rpc::root_service::create("my_server", server_zone, exec);

rpc::connection_factory_config::stream_factory_options options;
auto& endpoint = options.endpoint.emplace();
endpoint.host = std::string("127.0.0.1");
endpoint.port = uint16_t{8080};

auto listener = rpc::tcp::accept_rpc<my_app::i_my_service, my_app::i_my_service>(
    rpc::shared_ptr<my_app::i_my_service>(new my_service_impl()),
    options,
    service);
if (listener.error_code != rpc::error::OK())
    return 1;

// On shutdown:
listener.handle->stop();
exec->shutdown();
```

Blocking client code uses the same factory:

```cpp
auto result = rpc::tcp::connect_rpc<my_app::i_my_service, my_app::i_my_service>(
    rpc::shared_ptr<my_app::i_my_service>(),
    options,
    client_service);
```

The blocking TCP socket takes ownership of the descriptor, switches it to
non-blocking mode internally, and uses `poll()`/POSIX `recv`/`send` behind the
same `streaming::stream` interface used by coroutine builds.

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

## Working Example Structure

A complete external application should mirror the structure shown in this
guide: a root build file, presets, an IDL subdirectory, one IDL file, and the
server/client sources. For a TCP greeter-style application, the essential files
are:

- `CMakeLists.txt` — root build
- `CMakePresets.json` — Coroutine and Release_Coroutine presets
- `idl/CMakeLists.txt` — `CanopyGenerate()` call
- `idl/greeting/greeting.idl` — interface definition
- `src/server.cpp` and `src/client.cpp` — implementation
