/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

/*
 *   Shared Pointer Demo
 *   Demonstrates rpc::shared_ptr reference counting and lifecycle management
 *
 *   Concept: rpc::shared_ptr is a thread-safe reference-counted smart pointer
 *   for remote objects. Key characteristics:
 *
 *   - RAII semantics: Object dies when last shared_ptr is released
 *   - Keeps remote object AND transport chain alive
 *   - Returns OBJECT_NOT_FOUND if object is destroyed while reference held
 *   - Never mix with std::shared_ptr
 *
 *   MISCONCEPTION REPORT:
 *   ---------------------
 *   1. rpc::shared_ptr IS NOT std::shared_ptr - they have different control blocks
 *      and memory management. Never cast between them.
 *
 *   2. rpc::shared_ptr keeps the TRANSPORT CHAIN alive, not just the object.
 *      This means if you have a shared_ptr to a remote object, all transports
 *      along the path remain alive.
 *
 *   3. OBJECT_NOT_FOUND is returned when:
 *      - You hold a reference to an object
 *      - The object is destroyed while you hold the reference
 *      - This is a SERIOUS error indicating broken invariants
 *
 *   4. Reference counting is per-zone - remote zones have their own counts.
 *      The total reference count is distributed across zones.
 *
 *   To build and run:
 *   1. cmake --preset Debug (or Coroutine_Debug)
 *   2. cmake --build build --target shared_ptr_demo
 *   3. ./build/output/debug/demos/comprehensive/shared_ptr_demo
 */

#include <demo_impl.h>
#include <rpc/rpc.h>
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <memory>

namespace comprehensive
{
    namespace v1
    {

        void print_separator(const std::string& title)
        {
            std::cout << "\n";
            std::cout << std::string(60, '=') << "\n";
            std::cout << "  " << title << "\n";
            std::cout << std::string(60, '=') << "\n";
        }

        void print_result(const std::string& operation, int error)
        {
            RPC_INFO("{}: ", operation);
            if (error == rpc::error::OK())
            {
                RPC_INFO("OK");
            }
            else
            {
                RPC_INFO("ERROR {}", error);
            }
        }

#ifdef CANOPY_BUILD_COROUTINE
        CORO_TASK(bool) run_shared_ptr_demo(std::shared_ptr<coro::io_scheduler> scheduler)
#else
        bool run_shared_ptr_demo()
#endif
        {
            print_separator("SHARED PTR DEMO");

            std::atomic<uint64_t> zone_gen{0};

            // Create root service
#ifdef CANOPY_BUILD_COROUTINE
            auto service = std::make_shared<rpc::service>("shared_ptr_demo_service", rpc::zone{++zone_gen}, scheduler);
#else
            auto service = std::make_shared<rpc::service>("shared_ptr_demo_service", rpc::zone{++zone_gen});
#endif

            RPC_INFO("Service zone ID: {}", service->get_zone_id().get_val());
            RPC_INFO("");

            // Demo 1: Create and use objects
            {
                print_separator("1. OBJECT CREATION AND USAGE");

                auto factory = create_object_factory();
                RPC_INFO("Created object factory");

                // Create first object
                rpc::shared_ptr<i_managed_object> obj1;
                auto error = CO_AWAIT factory->create_object(obj1);
                print_result("Create object 1", error);

                if (error == rpc::error::OK() && obj1)
                {
                    uint64_t id;
                    error = CO_AWAIT obj1->get_object_id(id);
                    print_result("Get object 1 ID", error);
                    RPC_INFO("  Object 1 ID: {}", id);

                    // Perform operation
                    std::string result;
                    error = CO_AWAIT obj1->perform_operation("test_operation", result);
                    print_result("Perform operation on object 1", error);
                    RPC_INFO("  Result: {}", result);
                }

                // Create second object
                rpc::shared_ptr<i_managed_object> obj2;
                error = CO_AWAIT factory->create_object(obj2);
                print_result("Create object 2", error);

                // Demo shared_ptr behavior
                RPC_INFO("");
                RPC_INFO("--- Shared Pointer Reference Counting ---");
                RPC_INFO("obj1 is valid: {}", (obj1 ? "YES" : "NO"));
                RPC_INFO("obj2 is valid: {}", (obj2 ? "YES" : "NO"));

                // Copy shared_ptr (increases refcount)
                auto obj1_copy = obj1;
                RPC_INFO("After copy: obj1_copy is valid: {}", (obj1_copy ? "YES" : "NO"));

                // Release original (refcount decreases but object stays alive)
                obj1.reset();
                RPC_INFO("After obj1.reset(): obj1 is valid: {}", (obj1 ? "YES" : "NO"));
                RPC_INFO("obj1_copy is valid: {}", (obj1_copy ? "YES" : "NO"));

                // Use copy
                if (obj1_copy)
                {
                    std::string result;
                    error = CO_AWAIT obj1_copy->ping();
                    print_result("Ping via copy", error);
                }

                obj1_copy.reset();
                RPC_INFO("After obj1_copy.reset(): all references released");
            }

            // Demo 2: Object lifecycle
            {
                print_separator("2. OBJECT LIFECYCLE");

                auto factory = create_object_factory();

                // Create object
                rpc::shared_ptr<i_managed_object> obj;
                auto error = CO_AWAIT factory->create_object(obj);
                print_result("Create object", error);

                if (error == rpc::error::OK() && obj)
                {
                    uint64_t id;
                    CO_AWAIT obj->get_object_id(id);
                    RPC_INFO("Created object with ID: {}", id);

                    // Use object multiple times
                    for (int i = 0; i < 3; ++i)
                    {
                        std::string result;
                        error = CO_AWAIT obj->perform_operation("iteration_" + std::to_string(i), result);
                        RPC_INFO("  Iteration {}: {}", i, (error == rpc::error::OK() ? "OK" : "ERROR"));
                    }

                    // Release reference
                    RPC_INFO("");
                    RPC_INFO("Releasing object reference...");
                    obj.reset();
                    RPC_INFO("Object reference released");

                    // Try to get object again (should fail if factory released it)
                    rpc::shared_ptr<i_managed_object> obj2;
                    error = CO_AWAIT factory->get_object(0, obj2);
                    print_result("Try to get released object", error);
                }
            }

            // Demo 3: Error handling
            {
                print_separator("3. ERROR HANDLING");

                RPC_INFO("Testing error conditions:");
                RPC_INFO("");

                auto factory = create_object_factory();

                // Try to get non-existent object
                rpc::shared_ptr<i_managed_object> obj;
                auto error = CO_AWAIT factory->get_object(99999, obj);
                print_result("Get non-existent object (ID=99999)", error);

                if (error == rpc::error::OBJECT_NOT_FOUND())
                {
                    RPC_INFO("  Expected: OBJECT_NOT_FOUND (object ID doesn't exist)");
                }
            }

            print_separator("SHARED PTR DEMO COMPLETED");
            CO_RETURN true;
        }

        CORO_TASK(bool)
        demo_task(
#ifdef CANOPY_BUILD_COROUTINE
            std::shared_ptr<coro::io_scheduler> scheduler
#endif
        )
        {
            bool res = CO_AWAIT run_shared_ptr_demo(
#ifdef CANOPY_BUILD_COROUTINE
                scheduler
#endif
            );
            CO_RETURN res;
        }

    }
}

void rpc_log(int level, const char* str, size_t sz)
{
    std::string message(str, sz);
    switch (level)
    {
    case 0:
        std::cout << "[DEBUG] " << message << std::endl;
        break;
    case 1:
        std::cout << "[TRACE] " << message << std::endl;
        break;
    case 2:
        std::cout << "[INFO] " << message << std::endl;
        break;
    case 3:
        std::cout << "[WARN] " << message << std::endl;
        break;
    case 4:
        std::cout << "[ERROR] " << message << std::endl;
        break;
    case 5:
        std::cout << "[CRITICAL] " << message << std::endl;
        break;
    default:
        std::cout << "[LOG " << level << "] " << message << std::endl;
        break;
    }
}

int main()
{
    RPC_INFO("RPC++ Comprehensive Demo - Shared Pointer");
    RPC_INFO("=========================================");
    RPC_INFO("Demonstrates rpc::shared_ptr lifecycle and reference counting");
    RPC_INFO("");

#ifdef CANOPY_BUILD_COROUTINE
    auto scheduler = coro::io_scheduler::make_shared(
        coro::io_scheduler::options{
            .thread_strategy = coro::io_scheduler::thread_strategy_t::spawn,
            .pool = coro::thread_pool::options{
                .thread_count = std::thread::hardware_concurrency(),
            },
            .execution_strategy = coro::io_scheduler::execution_strategy_t::process_tasks_on_thread_pool
        });

    bool result = coro::sync_wait(comprehensive::v1::demo_task(scheduler));

    scheduler->shutdown();

    return result ? 0 : 1;
#else
    return comprehensive::v1::run_shared_ptr_demo() ? 0 : 1;
#endif
}
