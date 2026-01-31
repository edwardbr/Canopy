/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

/*
 *   Optimistic Pointer Demo
 *   Demonstrates rpc::optimistic_ptr for non-RAII references
 *
 *   Concept: optimistic_ptr is for references to objects with independent lifetimes.
 *   Key characteristics:
 *
 *   - Non-RAII: Does NOT keep remote object alive
 *   - Returns OBJECT_GONE if object with independent lifetime is gone
 *   - Prevents circular dependencies in distributed systems
 *   - Use for: database connections, callbacks, singleton services
 *
 *   IMPORTANT: optimistic_ptr cannot be used in IDL interfaces for cross-zone
 *   communication because it cannot be serialized. It is only for SAME-ZONE
 *   references where both sides understand RPC++ types.
 *
 *   USE CASE: Same-zone callback pattern
 *   ------------------------------------
 *   Object A and Object B are in the same zone/service. A creates B and needs
 *   callbacks from B without creating a circular dependency:
 *
 *   Zone 1: Object A (creator)
 *     └── shared_ptr<ObjectB> (ownership - keeps B alive)
 *
 *   Zone 1: Object B (created by A, same zone)
 *     └── optimistic_ptr<ObjectA> (callbacks only - NO ownership)
 *
 *   This allows B to call A without creating a reference cycle. When A is
 *   destroyed, B's optimistic_ptr returns OBJECT_GONE.
 *
 *   MISCONCEPTION REPORT:
 *   ---------------------
 *   1. optimistic_ptr is NOT for fire-and-forget - both shared_ptr and
 *      optimistic_ptr can be used for fire-and-forget calls.
 *
 *   2. The key difference is ERROR SEMANTICS:
 *      - shared_ptr + object gone = OBJECT_NOT_FOUND (serious error)
 *      - optimistic_ptr + object gone = OBJECT_GONE (expected!)
 *
 *   3. optimistic_ptr does NOT add to reference count - it assumes the
 *      object has its own lifetime management.
 *
 *   4. CANNOT be used in IDL interfaces for serialization - only for
 *      same-zone, in-memory references.
 *
 *   5. Use optimistic_ptr for:
 *      - Same-zone callbacks (parent/child in same service)
 *      - Singleton services (global lifetime)
 *      - Database connection handles (connection pool managed)
 *      - Any object that should NOT die when last reference is released
 *
 *   To build and run:
 *   1. cmake --preset Debug (or Coroutine_Debug)
 *   2. cmake --build build --target optimistic_ptr_demo
 *   3. ./build/output/debug/demos/comprehensive/optimistic_ptr_demo
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
            else if (error == rpc::error::OBJECT_GONE())
            {
                RPC_INFO("OBJECT_GONE (expected for optimistic_ptr)");
            }
            else if (error == rpc::error::OBJECT_NOT_FOUND())
            {
                RPC_INFO("OBJECT_NOT_FOUND (unexpected for optimistic_ptr)");
            }
            else
            {
                RPC_INFO("ERROR {} - {}", static_cast<int>(error), rpc::error::to_string(error));
            }
        }

        CORO_TASK(bool)
        run_optimistic_ptr_demo(
#ifdef CANOPY_BUILD_COROUTINE
            std::shared_ptr<coro::io_scheduler> scheduler
#endif
        )
        {
            print_separator("OPTIMISTIC PTR DEMO");

            std::atomic<uint64_t> zone_gen{0};

            // Create root service
            auto service = std::make_shared<rpc::service>("optimistic_ptr_demo_service",
                rpc::zone{++zone_gen}
#ifdef CANOPY_BUILD_COROUTINE
                ,
                scheduler
#endif
            );

            RPC_INFO("Service zone ID: {}", service->get_zone_id().get_val());
            RPC_INFO("");
            RPC_INFO("NOTE: optimistic_ptr is for SAME-ZONE references only.");
            RPC_INFO("It cannot be serialized across zone boundaries.");
            RPC_INFO("");

            // Demo 1: Same-Zone Callback Pattern (Preventing Circular Dependencies)
            {
                print_separator("1. SAME-ZONE CALLBACK PATTERN (Preventing Circular Dependencies)");

                RPC_INFO("Scenario: Parent and Child in same zone/service");
                RPC_INFO("  Parent holds: shared_ptr<Child> (ownership)");
                RPC_INFO("  Child holds: optimistic_ptr<Parent> (callbacks only, same zone)");
                RPC_INFO("");
                RPC_INFO("Both objects are in the same service/zone - no serialization needed.");
                RPC_INFO("");

                // Create parent (callback receiver)
                auto parent = create_callback_receiver();
                RPC_INFO("Created callback receiver (parent)");

                // Create child (worker)
                auto child = create_worker();
                RPC_INFO("Created worker (child)");

                // Give child a shared_ptr to parent for callbacks (this IS serializable)
                // This keeps parent alive as long as child holds the reference
                auto error = CO_AWAIT child->set_callback_receiver(parent);
                print_result("Set callback receiver (shared_ptr)", error);

                // Start work - child will send progress updates to parent
                RPC_INFO("");
                RPC_INFO("Starting child work (5 iterations)...");
                error = CO_AWAIT child->start_work(5);
                print_result("Start work", error);

                // Check received callbacks
                RPC_INFO("Child sent progress updates to parent");
                // Note: Parent received callbacks via on_progress

                // Note: With shared_ptr, releasing parent would destroy it
                // and child would get OBJECT_NOT_FOUND (serious error)
                RPC_INFO("");
                RPC_INFO("With shared_ptr: releasing parent would destroy it");
                RPC_INFO("Child would get OBJECT_NOT_FOUND (serious error)");
            }

            // Demo 2: Database Connection Pattern (Same-Zone optimistic_ptr)
            {
                print_separator("2. SAME-ZONE OPTIMISTIC PTR PATTERN");

                RPC_INFO("Scenario: Object uses same-zone service with independent lifetime");
                RPC_INFO("  Service has its own lifetime (singleton pattern)");
                RPC_INFO("  Object uses optimistic_ptr to reference it");
                RPC_INFO("");

                // Create a service that has independent lifetime
                auto independent_service = create_data_processor();
                RPC_INFO("Created independent service (simulates singleton)");

                // Create optimistic reference to the service using make_optimistic
                // This does NOT keep the service alive
                rpc::optimistic_ptr<i_data_processor> opt_service;
                auto error = CO_AWAIT rpc::make_optimistic(independent_service, opt_service);
                print_result("Create optimistic_ptr to service", error);

                // Use service (works while service is alive)
                std::vector<int> input{1, 2, 3};
                std::vector<int> output;
                error = CO_AWAIT opt_service->process_vector(input, output);
                print_result("Use service via optimistic_ptr", error);

                // Release our reference to the service
                // The service continues to exist (independent lifetime)
                RPC_INFO("");
                RPC_INFO("Releasing direct reference to service...");
                RPC_INFO("Service continues to exist (independent lifetime)");
                independent_service.reset();

                // Try to use optimistic_ptr after direct reference released
                // The service is still alive (independent lifetime), so this works!
                std::vector<int> input2{4, 5, 6};
                std::vector<int> output2;
                error = CO_AWAIT opt_service->process_vector(input2, output2);
                print_result("Use service after releasing direct reference", error);

                if (error == rpc::error::OK())
                {
                    RPC_INFO("  Service still accessible via optimistic_ptr");
                    RPC_INFO("  (Service has independent lifetime)");
                }
            }

            // Demo 3: Error Semantics Comparison
            {
                print_separator("3. ERROR SEMANTICS COMPARISON");

                RPC_INFO("Demonstrating the difference between shared_ptr and optimistic_ptr:");
                RPC_INFO("");

                // Create object with shared_ptr
                auto obj = create_data_processor();
                RPC_INFO("Created object with shared_ptr");

                // Create optimistic reference using make_optimistic
                rpc::optimistic_ptr<i_data_processor> opt_obj;
                auto error = CO_AWAIT rpc::make_optimistic(obj, opt_obj);
                print_result("Create optimistic_ptr to object", error);

                // Release shared_ptr - object is destroyed
                RPC_INFO("");
                RPC_INFO("Releasing shared_ptr (object destroyed)...");
                obj.reset();

                // Try optimistic_ptr (should return OBJECT_GONE - EXPECTED)
                std::vector<int> test{1};
                std::vector<int> result;
                auto error2 = CO_AWAIT opt_obj->process_vector(test, result);
                print_result("Optimistic ptr after shared_ptr released", error2);

                RPC_INFO("");
                RPC_INFO("  KEY INSIGHT:");
                RPC_INFO("  - OBJECT_GONE from optimistic_ptr = EXPECTED");
                RPC_INFO("    (Object managed by shared_ptr, not independent lifetime)");
                RPC_INFO("  - OBJECT_NOT_FOUND from shared_ptr = SERIOUS ERROR");
                RPC_INFO("    (Reference was held but object was destroyed)");
            }

            // Demo 4: Preventing Circular Dependencies (Same-Zone)
            {
                print_separator("4. PREVENTING CIRCULAR DEPENDENCIES (SAME-ZONE)");

                RPC_INFO("Scenario: Two objects in same zone that need to reference each other");
                RPC_INFO("  Object A: shared_ptr<ObjectB> (ownership)");
                RPC_INFO("  Object B: optimistic_ptr<ObjectA> (callbacks only, same zone)");
                RPC_INFO("");
                RPC_INFO("This pattern prevents circular reference counts.");
                RPC_INFO("");
                RPC_INFO("Pattern demonstrated in Demo 1 (Same-Zone Callback Pattern)");
                RPC_INFO("See demo_impl.h for worker_impl and callback_receiver_impl");
            }

            print_separator("OPTIMISTIC PTR DEMO COMPLETED");
            CO_RETURN true;
        }

        CORO_TASK(void)
        demo_task(
#ifdef CANOPY_BUILD_COROUTINE
            std::shared_ptr<coro::io_scheduler> scheduler,
#endif
            bool* result_flag,
            bool* completed_flag)
        {
            bool res = CO_AWAIT run_optimistic_ptr_demo(
#ifdef CANOPY_BUILD_COROUTINE
                scheduler
#endif
            );
            *result_flag = res;
            *completed_flag = true;
            CO_RETURN;
        }

    }
}

#include <iostream>

// RPC logging function - called by RPC library for logging
extern "C" void rpc_log(int level, const char* str, size_t sz)
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
    RPC_INFO("RPC++ Comprehensive Demo - Optimistic Pointer");
    RPC_INFO("=============================================");
    RPC_INFO("Demonstrates rpc::optimistic_ptr for non-RAII references");
    RPC_INFO("");
    RPC_INFO("Key Points:");
    RPC_INFO("  1. optimistic_ptr does NOT keep object alive");
    RPC_INFO("  2. Returns OBJECT_GONE (expected) vs OBJECT_NOT_FOUND (serious)");
    RPC_INFO("  3. Prevents circular dependencies in same-zone scenarios");
    RPC_INFO("  4. Use for callbacks, singleton services, independent lifetime objects");
    RPC_INFO("  5. CANNOT be used in IDL for cross-zone communication");
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

    bool result = false;
    bool completed = false;

    scheduler->spawn(comprehensive::v1::demo_task(scheduler, &result, &completed));

    while (!completed)
    {
        scheduler->process_events(std::chrono::milliseconds(1));
    }

    return result ? 0 : 1;
#else
    return comprehensive::v1::run_optimistic_ptr_demo() ? 0 : 1;
#endif
}
