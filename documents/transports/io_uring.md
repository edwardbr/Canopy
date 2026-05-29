<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# io_uring Stream Factories

Scope note:

- this document describes the current C++ io_uring stream and RPC factory API
- the implementation is Linux and coroutine-only
- the public factory currently targets loopback stream connections

## Requirements

- Linux with io_uring support
- `CANOPY_BUILD_COROUTINE=ON`
- a service with an executor, or permission for the factory to create an
  internal scheduler owner

## Public Headers

Use these headers by concern:

- `<connection_factory/io_uring.h>` for high-level stream and RPC factories
- `<connection_factory/io_uring_types.h>` for typed runtime, controller, and listen defaults
- `<connection_factory/io_uring_options.h>` for JSON-to-typed option conversion

The split is intentional. Core stream and controller code consume typed option
objects. JSON is a process-boundary format for config files, command-line
overrides, and tests.

## Typed Runtime Options

The typed API avoids JSON entirely:

```cpp
#include <connection_factory/io_uring.h>

auto controller_options = rpc::io_uring::default_host_controller_options();
auto stream_options = streaming::io_uring::default_stream_options();

auto runtime = rpc::io_uring::make_runtime(controller_options, service);
if (runtime.error_code != rpc::error::OK())
{
    // Handle setup failure.
}
```

Stream-only connect:

```cpp
auto stream = CO_AWAIT rpc::io_uring::connect_stream(
    uint16_t{26000},
    controller_options,
    stream_options,
    service);
```

Stream-only accept:

```cpp
rpc::io_uring::loopback_listen_options listen_options;
listen_options.port = uint16_t{26000};

auto accepted = CO_AWAIT rpc::io_uring::accept_stream(
    callback,
    listen_options,
    controller_options,
    stream_options,
    service);
```

If `listen_options.port` is zero, the acceptor scans
`listen_options.port_range` and uses the first available loopback port.

## RPC Factories

The RPC helpers attach the normal `rpc::stream_transport` handshake to the
io_uring stream.

```cpp
auto listener = CO_AWAIT rpc::io_uring::accept_rpc<yyy::i_client, yyy::i_server>(
    server_interface,
    listen_options,
    controller_options,
    stream_options,
    server_service);

auto result = CO_AWAIT rpc::io_uring::connect_rpc<yyy::i_client, yyy::i_server>(
    client_interface,
    listener.handle->port(),
    controller_options,
    stream_options,
    client_service);
```

The returned listener owns the acceptor, service lifetime, and any scheduler
owner created by the factory.

## JSON Boundary

JSON is for external configuration. Materialise it once at the boundary, then
pass the generated typed options into the factory.

```cpp
#include <json/config.h>
#include <connection_factory/io_uring_options.h>

auto options = json::v1::parse_file("io_uring.json");
auto materialised = rpc::io_uring::materialise_io_uring_options(options);
if (materialised.error_code != rpc::error::OK())
{
    // Handle invalid configuration.
}

auto typed_options = std::move(materialised.options);
auto listen_options =
    rpc::io_uring::listen_options_from_options(typed_options.io_uring.value());
```

Accepted keys live under `io_uring`, for example:

```json
{
  "io_uring": {
    "queue_depth": 256,
    "use_sqpoll": true,
    "first_port": 26000,
    "last_port": 26064,
    "stream": {
      "max_transfer_size": 4096,
      "timeout_strategy": "linked_timeout"
    }
  }
}
```

Supported stream timeout strategies are `linked_timeout` and
`nonblocking_poll`.

## Relation To TCP And SPSC

TCP and SPSC factories accept `rpc::connection_factory_config::stream_factory_options`
directly as their typed application API. io_uring exposes narrower typed
mechanics because its runtime options are controller and loopback specific.
All three families keep JSON at the edge and use exact schema-backed option
names.
