<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# TCP Transport

Network communication between different machines or processes.

## When to Use

- Distributed systems across machines
- Client-server architectures
- Microservices over network

## Requirements

- `CANOPY_BUILD_COROUTINE=ON` (requires libcoro)
- Coroutine-based async I/O

## See Also

- [Stream Backpressure Guidelines](stream_backpressure_guidelines.md)
- [SPSC Queues and Streams](spsc_and_ipc.md)
- [Custom Transports](custom.md)

Canopy's current TCP support is provided by:

- `streaming::tcp::acceptor`
- `streaming::tcp::stream`
- `streaming::listener`
- `rpc::stream_transport::transport`

## Server Setup with Listener

```cpp
auto listener = std::make_unique<streaming::listener>(
    "responder_transport",
    std::make_shared<streaming::tcp::acceptor>(coro::net::socket_address{"127.0.0.1", 8080}),
    rpc::stream_transport::make_connection_callback<yyy::i_host, yyy::i_example>(interface_factory));

listener->start_listening(peer_service);
```

## Client Connection

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

## Transport Layering

TCP itself is just the stream layer. The RPC message envelope, handshake,
reference counting, and request routing are all implemented by
`rpc::stream_transport`.
