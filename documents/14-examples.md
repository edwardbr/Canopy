<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Examples

Working examples demonstrating Canopy features and patterns.

## 1. Basic Calculator

### IDL (idl/calculator.idl)

```idl
namespace calculator
{
    [inline] namespace v1
    {
        interface i_calculator
        {
            error_code add(int a, int b, [out] int& result);
            error_code subtract(int a, int b, [out] int& result);
            error_code multiply(int a, int b, [out] int& result);
            error_code divide(int a, int b, [out] int& result);
        };
    }
}
```

### Implementation (calculator_impl.h)

```cpp
#pragma once

#include <calculator/calculator.h>

namespace calculator
{

class calculator_impl : public v1::i_calculator
{
public:
    void* get_address() const override
    {
        return const_cast<calculator_impl*>(this);
    }

    const rpc::casting_interface* query_interface(
        rpc::interface_ordinal interface_id) const override
    {
        if (rpc::match<v1::i_calculator>(interface_id))
            return static_cast<const v1::i_calculator*>(this);
        return nullptr;
    }

    CORO_TASK(error_code) add(int a, int b, int& result) override
    {
        result = a + b;
        CO_RETURN rpc::error::OK();
    }

    CORO_TASK(error_code) subtract(int a, int b, int& result) override
    {
        result = a - b;
        CO_RETURN rpc::error::OK();
    }

    CORO_TASK(error_code) multiply(int a, int b, int& result) override
    {
        result = a * b;
        CO_RETURN rpc::error::OK();
    }

    CORO_TASK(error_code) divide(int a, int b, int& result) override
    {
        if (b == 0)
            CO_RETURN rpc::error::INVALID_DATA();
        result = a / b;
        CO_RETURN rpc::error::OK();
    }
};

inline rpc::shared_ptr<v1::i_calculator> create_calculator()
{
    return rpc::make_shared<calculator_impl>();
}

} // namespace calculator
```

### Main Program (main.cpp)

```cpp
#include "calculator_impl.h"
#include <iostream>

int main()
{
    auto calculator = calculator::create_calculator();

    int result;
    auto error = calculator->add(10, 5, result);
    std::cout << "10 + 5 = " << result << "\n";

    error = calculator->multiply(3, 4, result);
    std::cout << "3 * 4 = " << result << "\n";

    return 0;
}
```

## 2. Cross-Zone Communication

### Server Implementation

```cpp
#include "calculator_impl.h"

class server_app
{
    std::shared_ptr<rpc::service> service_;
    rpc::shared_ptr<calculator::v1::i_calculator> calculator_;

public:
    void start()
    {
        service_ = std::make_shared<rpc::service>(
            "server", rpc::zone{1});

        calculator_ = calculator::create_calculator();

        std::cout << "Server started on zone "
                  << service_->get_zone_id().get_val() << "\n";
    }

    rpc::shared_ptr<calculator::v1::i_calculator> get_calculator()
    {
        return calculator_;
    }

    std::shared_ptr<rpc::service> get_service()
    {
        return service_;
    }
};
```

### Client Implementation

```cpp
#include "calculator_impl.h"

class client_app
{
    std::shared_ptr<rpc::service> service_;
    rpc::shared_ptr<calculator::v1::i_calculator> remote_calculator_;

public:
    void connect_to(server_app& server)
    {
        service_ = std::make_shared<rpc::service>(
            "client", rpc::zone{2});

        auto transport = std::make_shared<rpc::local::child_transport>(
            "server",
            server.get_service(),
            service_->get_zone_id(),
            rpc::local::parent_transport::bind<calculator::v1::i_calculator>(
                service_->get_zone_id(),
                [&](const rpc::shared_ptr<calculator::v1::i_calculator>&,
                    rpc::shared_ptr<calculator::v1::i_calculator>& calc,
                    const std::shared_ptr<rpc::child_service>&) -> CORO_TASK(int)
                {
                    calc = server.get_calculator();
                    CO_RETURN rpc::error::OK();
                }));

        auto error = CO_AWAIT service_->connect_to_zone(
            "server", transport, remote_calculator_);

        std::cout << "Connected to server\n";
    }

    void use_calculator()
    {
        int result;
        auto error = CO_AWAIT remote_calculator_->add(100, 200, result);
        std::cout << "100 + 200 = " << result << "\n";
    }
};
```

## 3. Coroutine Server

```cpp
#include "calculator_impl.h"
#include <coro/io_scheduler.hpp>
#include <thread>

class coro_server
{
    std::shared_ptr<rpc::service> service_;
    rpc::shared_ptr<calculator::v1::i_calculator> calculator_;
    std::shared_ptr<coro::io_scheduler> scheduler_;

public:
    void start()
    {
        scheduler_ = coro::io_scheduler::make_shared(
            coro::io_scheduler::options{
                .thread_strategy = coro::io_scheduler::thread_strategy_t::spawn,
                .pool = coro::thread_pool::options{
                    .thread_count = std::thread::hardware_concurrency(),
                },
                .execution_strategy = coro::io_scheduler::execution_strategy_t::process_tasks_on_thread_pool
            });

        service_ = std::make_shared<rpc::service>(
            "coro_server", rpc::zone{1}, scheduler_);

        calculator_ = calculator::create_calculator();

        // Spawn background task
        scheduler_->spawn(listen_for_requests());
    }

    auto listen_for_requests() -> CORO_TASK(void)
    {
        // Background task to handle requests
        CO_RETURN;
    }

    void run()
    {
        bool running = true;
        while (running)
        {
            scheduler_->process_events(std::chrono::milliseconds(1));
        }
    }
};
```

## 4. Object Passing

### IDL (idl/service.idl)

```idl
namespace service
{
    interface i_factory
    {
        error_code create_object([out] rpc::shared_ptr<i_data>& obj);
        error_code process_object([in] rpc::shared_ptr<i_data> obj);
    };

    interface i_data
    {
        error_code get_value([out] int& value);
        error_code set_value(int value);
    };
}
```

### Data Object Implementation

```cpp
class data_impl : public i_data
{
    int value_ = 0;

public:
    void* get_address() const override { return const_cast<data_impl*>(this); }

    const rpc::casting_interface* query_interface(
        rpc::interface_ordinal interface_id) const override
    {
        if (rpc::match<i_data>(interface_id))
            return static_cast<const i_data*>(this);
        return nullptr;
    }

    CORO_TASK(error_code) get_value(int& value) override
    {
        value = value_;
        CO_RETURN rpc::error::OK();
    }

    CORO_TASK(error_code) set_value(int value) override
    {
        value_ = value;
        CO_RETURN rpc::error::OK();
    }
};
```

### Factory Implementation

```cpp
class factory_impl : public i_factory
{
public:
    void* get_address() const override { return const_cast<factory_impl*>(this); }

    const rpc::casting_interface* query_interface(
        rpc::interface_ordinal interface_id) const override
    {
        if (rpc::match<i_factory>(interface_id))
            return static_cast<const i_factory*>(this);
        return nullptr;
    }

    CORO_TASK(error_code) create_object(rpc::shared_ptr<i_data>& obj) override
    {
        obj = rpc::make_shared<data_impl>();
        CO_RETURN rpc::error::OK();
    }

    CORO_TASK(error_code) process_object(rpc::shared_ptr<i_data> obj) override
    {
        if (!obj)
            CO_RETURN rpc::error::INVALID_DATA();

        int value;
        auto error = CO_AWAIT obj->get_value(value);
        std::cout << "Received object with value: " << value << "\n";
        CO_RETURN error;
    }
};
```

## 5. Error Handling

```cpp
#include <iostream>

void handle_error(rpc::error_code error)
{
    switch (error)
    {
        case rpc::error::OK():
            std::cout << "Success\n";
            break;
        case rpc::error::INVALID_DATA():
            std::cout << "Invalid data\n";
            break;
        case rpc::error::OBJECT_GONE():
            std::cout << "Object was destroyed\n";
            break;
        case rpc::error::TRANSPORT_ERROR():
            std::cout << "Transport error\n";
            break;
        default:
            std::cout << "Error: " << static_cast<int>(error) << "\n";
            break;
    }
}
```

## 6. Dynamic Interface Casting

```cpp
// Assume foo_ptr is rpc::shared_ptr<xxx::i_foo>
rpc::shared_ptr<xxx::i_foo> foo_ptr = get_foo();

// Try to cast to i_bar
auto bar_ptr = CO_AWAIT rpc::dynamic_pointer_cast<xxx::i_bar>(foo_ptr);

if (bar_ptr)
{
    // Successfully cast to i_bar
    bar_ptr->bar_method();
}
else
{
    // foo_ptr does not implement i_bar
}
```

## 7. Optimistic Pointer

Optimistic pointers are for references to objects with independent lifetimes (e.g., database connections, services managed externally). They don't keep objects alive but also don't return serious errors if the object is gone.

### Use Case 1: Database Connection

```cpp
// Database service - managed externally, has its own lifetime
auto db_service = database_manager->get_connection();

// Create optimistic pointer (non-RAII reference)
rpc::optimistic_ptr<i_database> opt_db;
auto error = CO_AWAIT rpc::make_optimistic(db_service, opt_db);

// Use the database - if it's still running, calls succeed
auto result = CO_AWAIT opt_db->query("SELECT * FROM users");

// If database is shut down externally, call returns OBJECT_GONE (not OBJECT_NOT_FOUND)
db_service.reset();  // Release our reference
auto result2 = CO_AWAIT opt_db->query("SELECT * FROM users");
// Returns OBJECT_GONE - expected, database has its own lifetime

// Contrast with shared_ptr:
// auto error3 = CO_AWAIT db_service->query(...);  // Would return OBJECT_NOT_FOUND
```

### Use Case 2: Callback Pattern (Prevents Circular Dependencies)

```cpp
// Parent creates Child in another zone and needs callbacks without circular reference
//
// Zone 1: Parent (owns Child)
//   └── shared_ptr<Child> (keeps Child alive)
//
// Zone 2: Child (created by Parent)
//   └── optimistic_ptr<Parent> (for callbacks, NOT ownership)

class parent : public i_parent
{
    rpc::shared_ptr<child> child_;  // Ownership

public:
    CORO_TASK(error_code) spawn_child()
    {
        // Create child in another zone
        rpc::shared_ptr<child> new_child;
        auto error = CO_AWAIT zone_->create_object(new_child);

        // Give child an optimistic pointer to us for callbacks
        // This allows child to notify us without creating circular reference
        CO_AWAIT rpc::make_optimistic(
            rpc::shared_ptr<parent>(this),
            new_child->get_callback_handler());

        child_ = new_child;  // Ownership reference
        CO_RETURN error::OK();
    }

    // Callback from child
    CORO_TASK(error_code) on_progress(int percent) override
    {
        RPC_INFO("Child progress: {}%", percent);
        CO_RETURN error::OK();
    }
};

class child : public i_child
{
    rpc::optimistic_ptr<parent> parent_;  // Callbacks only - no refcount
public:
    void set_parent(rpc::optimistic_ptr<parent> p) { parent_ = p; }

    CORO_TASK(error_code) do_work()
    {
        for (int i = 0; i <= 100; i += 10)
        {
            // Notify parent of progress
            // If parent is gone, returns OBJECT_GONE (expected)
            CO_AWAIT parent_->on_progress(i);
        }
        CO_RETURN error::OK();
    }
};
```

**Key Difference**: `OBJECT_GONE` (optimistic) means "object with independent lifetime is gone" while `OBJECT_NOT_FOUND` (shared) means "object was destroyed while reference was held".

## 8. WebSocket Demo Structure

The WebSocket demo shows a complete real-world application:

```
demos/websocket/
├── server/
│   ├── server.cpp           # Main entry point
│   ├── transport.h/cpp      # WebSocket transport
│   ├── websocket_service.h  # RPC service
│   └── demo.cpp             # Calculator implementation
├── client/
│   ├── test_calculator.js   # Node.js RPC client
│   └── websocket_proto.js   # Generated protobuf
└── idl/
    └── websocket_demo.idl   # Interface definitions
```

Key patterns demonstrated:
- Custom transport implementation
- Coroutine-based async I/O
- Protobuf serialization
- Browser and Node.js clients
- JSON schema generation

## 9. Test Patterns

### Template-Based Test Fixture

```cpp
template<class T>
class type_test : public testing::Test
{
protected:
    T lib_;

public:
    T& get_lib() { return lib_; }
    void SetUp() override { lib_.set_up(); }
    void TearDown() override { lib_.tear_down(); }
};

TYPED_TEST(my_test_case, test_name)
{
    auto& lib = this->get_lib();
    // Test implementation
}
```

### Coroutine Test Runner

```cpp
template<typename TestFixture, typename CoroFunc>
void run_coro_test(TestFixture& fixture, CoroFunc&& coro_func)
{
    auto& lib = fixture.get_lib();
#ifdef CANOPY_BUILD_COROUTINE
    bool completed = false;
    auto wrapper = [&]() -> CORO_TASK(bool)
    {
        auto result = CO_AWAIT coro_func(lib);
        completed = true;
        CO_RETURN result;
    };
    lib.get_scheduler()->spawn(wrapper());
    while (!completed)
        lib.get_scheduler()->process_events(std::chrono::milliseconds(1));
#else
    coro_func(lib);
#endif
}
```

## 10. Next Steps

- [Best Practices](15-best-practices.md) - Guidelines and troubleshooting
- [API Reference](12-api-reference.md) - Complete API documentation
- [Building Canopy](05-building.md) - Build configuration
