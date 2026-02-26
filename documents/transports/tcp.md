<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# TCP Transport (rpc::tcp)

Network communication between different machines or processes.

## When to Use

- Distributed systems across machines
- Client-server architectures
- Microservices over network

## Requirements

- `CANOPY_BUILD_COROUTINE=ON` (requires libcoro)
- Coroutine-based async I/O

## Components

```cpp
namespace rpc::tcp {
    class tcp_transport : public rpc::transport;
    class listener;
}
```

## Server Setup with Listener

```cpp
auto listener = std::make_unique<rpc::tcp::listener>(
    [](auto& input, auto& output, auto& service, auto& transport)
    {
        // Connection handler for new clients
        // Called when a client connects
    },
    std::chrono::milliseconds(100000)  // Connection timeout
);

auto server_options = coro::net::tcp::server::options{
    .address = coro::net::ip_address::from_string("127.0.0.1"),
    .port = 8080,
    .backlog = 128
};

listener->start_listening(peer_service_, server_options);
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

coro::net::tcp::client client(scheduler,
    coro::net::tcp::client::options{
        .address = coro::net::ip_address::from_string("127.0.0.1"),
        .port = 8080
    });

auto status = CO_AWAIT client.connect();
if (status != coro::net::socket_status::connected)
{
    // Handle connection failure
}

auto client_transport = rpc::tcp::tcp_transport::create(
    "client", root_service_, peer_zone_id,
    std::chrono::seconds(5),  // Timeout
    std::move(client),
    nullptr);  // No handler needed for client

rpc::shared_ptr<i_foo> input_interface;  // Interface to send to peer (optional)
rpc::shared_ptr<i_foo> output_interface;  // Interface received from peer
auto error = CO_AWAIT root_service_->connect_to_zone(
    "client", client_transport, input_interface, output_interface);
```

## TCP Transport Class

```cpp
class tcp_transport : public rpc::transport
{
    coro::net::tcp::client client_;
    std::queue<std::vector<uint8_t>> send_queue_;
    std::unordered_map<uint64_t, result_listener*> pending_transmits_;

public:
    static std::shared_ptr<tcp_transport> create(
        const char* name,
        std::weak_ptr<rpc::service> service,
        rpc::destination_zone adjacent_zone_id,
        std::chrono::milliseconds timeout,
        coro::net::tcp::client client,
        std::function<void(...)> inbound_handler);
};
```
