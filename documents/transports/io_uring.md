<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# io_uring Streams And Controllers

Scope note:

- this document describes the current C++ io_uring controller and stream APIs
- the implementation is Linux and coroutine-only
- configured RPC use normally enters through the TCP coroutine stream adapter

## Requirements

- Linux with io_uring support
- `CANOPY_BUILD_COROUTINE=ON`
- a coroutine scheduler for host-side stream/controller operations

## Public Headers

Use these headers by concern:

- `<io_uring/host_controller.h>` for host-owned ring creation and lifetime
- `<io_uring/controller.h>` and `<io_uring/tcp.h>` for direct descriptor and
  TCP operations on an existing controller
- `<io_uring/io_uring_scheduler.h>` when a scheduler-integrated controller owner
  is needed
- `<io_uring/io_uring_config.h>` for generated controller option types
- `<tcp_coroutine_stream/tcp_coroutine_stream_config.h>` for configured
  io_uring-backed TCP stream endpoint settings

The removed `connection_factory/io_uring*.h` facade is not part of the current
public API. Core stream and controller code consumes typed option objects. JSON
is a process-boundary format for config files, command-line overlays, and tests.

## Typed Controller Options

The typed API avoids JSON entirely:

```cpp
#include <io_uring/host_controller.h>

canopy::io_uring::host_controller_options controller_options;
controller_options.queue_depth = 256;

std::unique_ptr<rpc::io_uring::host_controller> controller;
auto error = rpc::io_uring::host_controller::create(
    controller,
    controller_options,
    scheduler);
if (error != rpc::error::OK())
{
    // Handle setup failure.
}
```

Direct TCP operations are lower-level building blocks. Create or obtain a
controller, then use the operations in `<io_uring/tcp.h>` to create sockets,
listen, connect, accept, and wrap descriptors.

## Configured TCP Coroutine Streams

High-level configured RPC connections use `rpc::connection_factory` with a
`tcp_coroutine` base stream. The endpoint settings include both the io_uring
host-controller options and per-stream TCP coroutine options:

```cpp
#include <connection_factory/connection_factory.h>
#include <json/convert.h>
#include <stream_transport/stream_transport_config.h>
#include <tcp_coroutine_stream/tcp_coroutine_stream_config.h>

rpc::tcp_coroutine_stream::endpoint endpoint;
endpoint.host = "127.0.0.1";
endpoint.port = 26000;
endpoint.controller.queue_depth = 256;
endpoint.stream.timeout_strategy =
    rpc::tcp_coroutine_stream::receive_timeout_strategy::linked_timeout;

rpc::connection_factory::connection_settings options;

rpc::connection_factory::typed_settings transport_settings;
transport_settings.type = "stream_rpc";
transport_settings.settings =
    json::v1::convert::to_json_object(rpc::stream_transport::transport_settings{});
options.transport = std::move(transport_settings);

rpc::stream_layers::stream_layer_settings tcp_layer;
tcp_layer.type = "tcp_coroutine";
tcp_layer.settings = json::v1::convert::to_json_object(endpoint);
options.stream_layers.push_back(std::move(tcp_layer));
```

When `endpoint.port` is zero on a listener, the TCP coroutine adapter scans
`endpoint.first_port..endpoint.last_port` and uses the first available port.

## JSON Boundary

JSON is for external configuration. Materialise it once at the configured
connection-factory boundary, then let the selected stream, layer, or transport
implementation convert its own `typed_settings::settings` object to the
generated IDL type it owns.

Example connection-factory JSON for an io_uring-backed TCP coroutine stream:

```json
{
  "transport": {
    "type": "stream_rpc",
    "settings": {
      "encoding": "yas_binary"
    }
  },
  "stream_layers": [
    {
      "type": "tcp_coroutine",
      "settings": {
        "host": "127.0.0.1",
        "port": 26000,
        "controller": {
          "queue_depth": 256,
          "use_sqpoll": false
        },
        "stream": {
          "max_transfer_size": 4096,
          "timeout_strategy": "linked_timeout"
        }
      }
    }
  ]
}
```

Supported stream timeout strategies are `linked_timeout` and
`nonblocking_poll`.

## Relation To TCP And SPSC

Blocking TCP and SPSC still expose direct typed helpers in their implementation
namespaces. The configured factory surface is
`rpc::connection_factory::connection_settings`: `tcp_blocking`,
`tcp_coroutine`, and `spsc_queue` appear as stream-layer type names, while
`stream_rpc` is the transport type that attaches the Canopy RPC handshake.
