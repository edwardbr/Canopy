<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# TCP Transport

Scope note:

- this document describes the current C++ TCP transport stack
- the TCP transport described here is built on the C++ streaming transport
  layer; it supports either the coroutine scheduler or the blocking executor
- see [C++ Status](../status/cpp.md), [Rust Status](../status/rust.md), and
  [JavaScript Status](../status/javascript.md) for implementation scope

Network communication between different machines or processes.

## When to Use

- Distributed systems across machines
- Client-server architectures
- Microservices over network

## Requirements

- Coroutine build with `rpc::coro::scheduler`, or blocking build with an
  `rpc::blocking_executor` attached to the service
- TCP stream transports require an executor on the owning service in both
  modes; plain synchronous non-streaming RPC does not

## See Also

- [Stream Backpressure Guidelines](stream_backpressure_guidelines.md)
- [SPSC Queues and Streams](spsc_and_ipc.md)
- [Custom Transports](custom.md)

In the current C++ implementation, TCP support is provided by:

- `streaming::tcp::acceptor`
- `streaming::tcp::stream`
- `streaming::listener`
- `rpc::stream_transport::transport`

## Server Setup with Listener

The examples below use coroutine syntax. Blocking builds use the same listener
and transport layering but construct a `streaming::tcp::endpoint` acceptor and
pass an `rpc::blocking_executor` to the owning service.

```cpp
auto listener = std::make_unique<streaming::listener>(
    "responder_transport",
    std::make_shared<streaming::tcp::acceptor>(coro::net::socket_address{"127.0.0.1", 8080}),
    rpc::stream_transport::make_connection_callback<yyy::i_host, yyy::i_example>(interface_factory));

listener->start_listening(peer_service);
```

The listener and stream objects are streaming-layer components. The RPC
transport is `rpc::stream_transport::transport`.

## Coroutine Client Connection

```cpp
auto scheduler = coro::scheduler::make_unique(
    coro::scheduler::options{
        .thread_strategy = coro::scheduler::thread_strategy_t::spawn,
        .pool = coro::thread_pool::options{
            .thread_count = std::thread::hardware_concurrency(),
        },
        .execution_strategy = coro::scheduler::execution_strategy_t::process_tasks_on_thread_pool
    });

coro::net::tcp::client client(scheduler, coro::net::socket_address{"127.0.0.1", 8080});

auto status = CO_AWAIT client.connect(std::chrono::milliseconds(5000));
if (status != coro::net::connect_status::connected)
{
    // Handle connection failure
}

auto client_stream = std::make_shared<streaming::tcp::stream>(std::move(client), scheduler);
auto client_transport =
    rpc::stream_transport::make_client("client", root_service, std::move(client_stream));

rpc::shared_ptr<i_foo> input_interface;
auto connect_result = CO_AWAIT root_service->connect_to_zone<i_foo, i_foo>(
    "client", client_transport, input_interface);
auto output_interface = std::move(connect_result.output_interface);
auto error = connect_result.error_code;
```

## Blocking Setup Notes

Blocking TCP uses the same `streaming::listener`, `streaming::tcp::acceptor`,
`streaming::tcp::stream`, and `rpc::stream_transport` layers. The differences
are at the I/O boundary:

- construct the service with an `rpc::blocking_executor` when a listener or
  stream transport will spawn receive/send loops
- construct listeners with `streaming::tcp::endpoint`, not libcoro socket types
- wrap a connected POSIX file descriptor with `streaming::tcp::socket(fd)` for
  client streams
- the blocking TCP socket owns the descriptor, switches it to non-blocking mode,
  and waits with `poll()` around POSIX `recv`/`send`

Server-side setup:

```cpp
auto exec = std::make_shared<rpc::blocking_executor>();
auto service = rpc::root_service::create("server", server_zone, exec);

streaming::tcp::endpoint endpoint;
endpoint.host = "127.0.0.1";
endpoint.port = 8080;

auto listener = std::make_shared<streaming::listener>(
    "responder_transport",
    std::make_shared<streaming::tcp::acceptor>(endpoint),
    rpc::stream_transport::make_connection_callback<yyy::i_host, yyy::i_example>(
        interface_factory));

if (!listener->start_listening(service))
{
    // Handle missing executor, bind failure, or spawn failure.
}
```

Client-side stream creation after a successful `::connect()`:

```cpp
auto client_stream = std::make_shared<streaming::tcp::stream>(
    streaming::tcp::socket(connected_fd));
auto client_transport =
    rpc::stream_transport::make_client("client", root_service, client_stream);

auto connect_result = root_service->connect_to_zone<i_foo, i_foo>(
    "server", client_transport, rpc::shared_ptr<i_foo>());
```

## Transport Layering

TCP itself is just the stream layer. The RPC message envelope, handshake,
reference counting, and request routing are all implemented by
`rpc::stream_transport`.

This page should therefore be read as transport-stack guidance for the current
C++ implementation, not as a statement that every Canopy implementation
provides a TCP transport.
