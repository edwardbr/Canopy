/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

// Standard C++ headers
#include <iostream>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <filesystem>
#include <atomic>

// RPC headers
#include <rpc/rpc.h>
#ifdef CANOPY_USE_TELEMETRY
#include <rpc/telemetry/i_telemetry_service.h>
#include <rpc/telemetry/multiplexing_telemetry_service.h>
#include <rpc/telemetry/console_telemetry_service.h>
#include <rpc/telemetry/sequence_diagram_telemetry_service.h>
#endif

// Other headers
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsuggest-override"
#pragma clang diagnostic ignored "-Wgnu-zero-variadic-macro-arguments"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsuggest-override"
#endif

#include <args.hxx>

#ifdef __clang__
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#ifdef CANOPY_BUILD_COROUTINE
#include <coro/io_scheduler.hpp>
#endif
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include <common/foo_impl.h>
#include <common/tests.h>

#include "rpc_global_logger.h"
#include "test_host.h"
#include <transport/tests/direct/setup.h>
#include <transport/tests/local/setup.h>
#ifdef CANOPY_BUILD_ENCLAVE
#include <transport/tests/sgx/setup.h>
#endif
#ifdef CANOPY_BUILD_COROUTINE
#include <transport/tests/tcp/setup.h>
#include <transport/tests/spsc/setup.h>
#endif
#include "crash_handler.h"

using namespace marshalled_tests;

#include "test_globals.h"
#include "type_test_fixture.h"

extern bool enable_multithreaded_tests;

// Type list for optimistic_ptr test instantiations.
using optimistic_ptr_implementations = ::testing::Types<in_memory_setup<false>,
    in_memory_setup<true>,
    inproc_setup<false, false, false>,
    inproc_setup<false, false, true>,
    inproc_setup<false, true, false>,
    inproc_setup<false, true, true>,
    inproc_setup<true, false, false>,
    inproc_setup<true, false, true>,
    inproc_setup<true, true, false>,
    inproc_setup<true, true, true>
#ifdef CANOPY_BUILD_COROUTINE
    ,
    tcp_setup<false, false, false>,
    tcp_setup<false, false, true>,
    tcp_setup<false, true, false>,
    tcp_setup<false, true, true>,
    tcp_setup<true, false, false>,
    tcp_setup<true, false, true>,
    tcp_setup<true, true, false>,
    tcp_setup<true, true, true>,
    spsc_setup<false, false, false>,
    spsc_setup<false, false, true>,
    spsc_setup<false, true, false>,
    spsc_setup<false, true, true>,
    spsc_setup<true, false, false>,
    spsc_setup<true, false, true>,
    spsc_setup<true, true, false>,
    spsc_setup<true, true, true>
#endif
#ifdef CANOPY_BUILD_ENCLAVE
    ,
    sgx_setup<false, false, false>,
    sgx_setup<false, false, true>,
    sgx_setup<false, true, false>,
    sgx_setup<false, true, true>,
    sgx_setup<true, false, false>,
    sgx_setup<true, false, true>,
    sgx_setup<true, true, false>,
    sgx_setup<true, true, true>
#endif
    >;

template<class T> class optimistic_ptr_test : public type_test<T>
{
};

TYPED_TEST_SUITE(optimistic_ptr_test, optimistic_ptr_implementations);

// ============================================================================
// Optimistic Pointer Tests
// ============================================================================

// Test 1: Basic optimistic_ptr construction and lifecycle
CORO_TASK(bool) optimistic_ptr_basic_lifecycle_test(std::shared_ptr<rpc::service> root_service)
{
    // Create a shared_ptr to a local object
    auto f = rpc::shared_ptr<xxx::i_foo>(new foo());
    CORO_ASSERT_NE(f, nullptr);

    // Create optimistic_ptr from shared_ptr
    rpc::optimistic_ptr<xxx::i_foo> opt_f;
    auto err = CO_AWAIT rpc::make_optimistic(f, opt_f);
    CORO_ASSERT_EQ(err, rpc::error::OK());
    CORO_ASSERT_NE(opt_f.get_unsafe_only_for_testing(), nullptr);
    CORO_ASSERT_EQ(opt_f.get_unsafe_only_for_testing(), f.get());

    // Test copy constructor
    rpc::optimistic_ptr<xxx::i_foo> opt_f_copy(opt_f);
    CORO_ASSERT_NE(opt_f_copy.get_unsafe_only_for_testing(), nullptr);
    CORO_ASSERT_EQ(opt_f_copy.get_unsafe_only_for_testing(), opt_f.get_unsafe_only_for_testing());

    // Test move constructor
    rpc::optimistic_ptr<xxx::i_foo> opt_f_move(std::move(opt_f_copy));
    CORO_ASSERT_NE(opt_f_move.get_unsafe_only_for_testing(), nullptr);
    CORO_ASSERT_EQ(opt_f_move.get_unsafe_only_for_testing(),
        opt_f.get_unsafe_only_for_testing());                          // Should point to same local_proxy
    CORO_ASSERT_EQ(opt_f_copy.get_unsafe_only_for_testing(), nullptr); // Moved-from should be null

    // Test assignment
    rpc::optimistic_ptr<xxx::i_foo> opt_f_assigned;
    CORO_ASSERT_EQ(opt_f_assigned.get_unsafe_only_for_testing(), nullptr);
    opt_f_assigned = opt_f_move;
    CORO_ASSERT_NE(opt_f_assigned.get_unsafe_only_for_testing(), nullptr);
    CORO_ASSERT_EQ(opt_f_assigned.get_unsafe_only_for_testing(), f.get());

    // Test reset
    opt_f_move.reset();
    CORO_ASSERT_EQ(opt_f_move.get_unsafe_only_for_testing(), nullptr);

    CO_RETURN true;
}

TYPED_TEST(optimistic_ptr_test, optimistic_ptr_basic_lifecycle_test)
{
    auto& lib = this->get_lib();
    auto root_service = lib.get_root_service();
    run_coro_test(*this,
        [root_service](auto& lib)
        {
            (void)lib;
            return optimistic_ptr_basic_lifecycle_test(root_service);
        });
}

// Test 2: Optimistic pointer weak semantics for local objects
CORO_TASK(bool) optimistic_ptr_weak_semantics_local_test(std::shared_ptr<rpc::service> root_service)
{
    rpc::optimistic_ptr<xxx::i_foo> opt_f;

    {
        // Create a shared_ptr to a local object
        auto f = rpc::shared_ptr<xxx::i_foo>(new foo());
        CORO_ASSERT_NE(f, nullptr);

        // Create optimistic_ptr from shared_ptr
        auto err = CO_AWAIT rpc::make_optimistic(f, opt_f);
        CORO_ASSERT_EQ(err, rpc::error::OK());
        CORO_ASSERT_NE(opt_f.get_unsafe_only_for_testing(), nullptr);

        // Verify object is accessible
        CORO_ASSERT_EQ(opt_f.get_unsafe_only_for_testing(), f.get());
        // f goes out of scope - object should be deleted
        // (optimistic_ptr has weak semantics for local objects)
    }

    // opt_f still exists and points to local_proxy (which is still alive via shared_ptr)
    // The local_proxy internally has a weak_ptr which will fail to lock
    // This is valid behavior per spec - optimistic_ptr has weak semantics
    CORO_ASSERT_NE(opt_f.get_unsafe_only_for_testing(), nullptr); // local_proxy still exists

    CO_RETURN true;
}

TYPED_TEST(optimistic_ptr_test, optimistic_ptr_weak_semantics_local_test)
{
    auto& lib = this->get_lib();
    auto root_service = lib.get_root_service();
    run_coro_test(*this,
        [root_service](auto& lib)
        {
            (void)lib;
            return optimistic_ptr_weak_semantics_local_test(root_service);
        });
}

// Test 3: optimistic_ptr local_proxy pattern test
CORO_TASK(bool) optimistic_ptr_local_proxy_test(std::shared_ptr<rpc::service> root_service)
{
    rpc::shared_ptr<xxx::i_foo> f;
    rpc::optimistic_ptr<xxx::i_foo> opt_f;

    // Create a shared_ptr to a local object
    f = rpc::shared_ptr<xxx::i_foo>(new foo());
    CORO_ASSERT_NE(f, nullptr);

    // Create optimistic_ptr from shared_ptr
    // optimistic_ptr should automatically create a local_proxy
    auto err = CO_AWAIT rpc::make_optimistic(f, opt_f);
    CORO_ASSERT_EQ(err, rpc::error::OK());
    CORO_ASSERT_NE(opt_f.get_unsafe_only_for_testing(), nullptr);

    // Test calling through the optimistic_ptr while object is alive
    err = CO_AWAIT opt_f->do_something_in_val(42);
    CORO_ASSERT_EQ(err, rpc::error::OK());

    // Clear the shared_ptr - object will be deleted
    f.reset();
    CORO_ASSERT_EQ(f, nullptr);

    // optimistic_ptr still points to the local_proxy
    // but calling through it should return OBJECT_GONE
    err = CO_AWAIT opt_f->do_something_in_val(42);
    CORO_ASSERT_EQ(err, rpc::error::OBJECT_GONE());

    // The local_proxy is still valid (weak_ptr failed to lock)
    CORO_ASSERT_NE(opt_f.get_unsafe_only_for_testing(), nullptr);

    CO_RETURN true;
}

TYPED_TEST(optimistic_ptr_test, optimistic_ptr_local_proxy_test)
{
    auto& lib = this->get_lib();
    auto root_service = lib.get_root_service();
    run_coro_test(*this,
        [root_service](auto& lib)
        {
            (void)lib;
            return optimistic_ptr_local_proxy_test(root_service);
        });
}

// Test 4: Optimistic pointer semantics (weak for local, shared for remote)
template<class T> CORO_TASK(bool) optimistic_ptr_remote_shared_semantics_test(T& lib)
{
    // Get example object (local or remote depending on setup)
    auto example = lib.get_example();
    CORO_ASSERT_NE(example, nullptr);

    // Create foo through example (will be local or marshalled depending on setup)
    rpc::shared_ptr<xxx::i_foo> f;
    CORO_ASSERT_EQ(CO_AWAIT example->create_foo(f), 0);
    CORO_ASSERT_NE(f, nullptr);

    // Create baz interface (local or marshalled depending on setup)
    rpc::shared_ptr<xxx::i_baz> baz;
    CORO_ASSERT_EQ(CO_AWAIT f->create_baz_interface(baz), 0);
    CORO_ASSERT_NE(baz, nullptr);

    // Release f to ensure it's not holding any references to baz via cached_ member
    f.reset();

    // Store raw pointer for later comparison
    auto* raw_ptr = baz.get();

    // Get object_id directly from the interface (avoids service mutex)
    auto baz_object_id = rpc::casting_interface::get_object_id(*baz);

    // Create optimistic_ptr
    rpc::optimistic_ptr<xxx::i_baz> opt_baz;
    auto err = CO_AWAIT rpc::make_optimistic(baz, opt_baz);
    CORO_ASSERT_EQ(err, rpc::error::OK());
    CORO_ASSERT_NE(opt_baz.get_unsafe_only_for_testing(), nullptr);

    // Can call through optimistic_ptr directly (local_proxy handles weak semantics)
    auto error2 = CO_AWAIT opt_baz->callback(42);
    CORO_ASSERT_EQ(error2, 0);

    // Register for object deletion notification with continuation for verification
    auto waiter = std::make_shared<object_deletion_waiter>(baz_object_id);

    // Schedule verification - handles both local (immediate) and remote (async) cases
    // CRITICAL: Pass opt_baz as argument to ensure it lives in the coroutine frame
    waiter->schedule(
        lib.get_root_service(),
        baz,
        [](auto opt_baz) -> CORO_TASK(void)
        {
            // This runs after the object is deleted
            // The object is deleted when the last shared_ptr goes away
            // local_proxy pointer is still valid but weak_ptr inside will fail to lock
            EXPECT_NE(opt_baz.get_unsafe_only_for_testing(), nullptr); // local_proxy still exists

            // Calling through optimistic_ptr should return OBJECT_GONE (weak_ptr failed to lock)
            auto error = CO_AWAIT opt_baz->callback(42);
            EXPECT_EQ(error, rpc::error::OBJECT_GONE());

            CO_RETURN;
        },
        opt_baz); // Pass as argument instead of capturing

    // Clear the shared_ptr - for remote this triggers async cleanup, for local it's immediate
    baz.reset();

    // For local objects, run verification immediately; for remote, it runs via async callback
    CO_AWAIT waiter->run_if_local();

    CO_RETURN true;
}

TYPED_TEST(optimistic_ptr_test, optimistic_ptr_remote_shared_semantics_test)
{
    GTEST_SKIP() << "skipped for now.";
    run_coro_test(*this, [](auto& lib) { return optimistic_ptr_remote_shared_semantics_test(lib); });
}

// Test 5: Optimistic pointer transparent operator-> for both local and remote
template<class T> CORO_TASK(bool) optimistic_ptr_transparent_access_test(T& lib)
{
    auto example = lib.get_example();
    // Test 1: Local object access
    {
        // Create interface (local or remote depending on setup)
        rpc::shared_ptr<xxx::i_foo> f_local;
        CORO_ASSERT_EQ(CO_AWAIT example->create_foo(f_local), 0);
        CORO_ASSERT_NE(f_local, nullptr);

        rpc::optimistic_ptr<xxx::i_foo> opt_f_local;
        auto err = CO_AWAIT rpc::make_optimistic(f_local, opt_f_local);
        CORO_ASSERT_EQ(err, rpc::error::OK());

        // operator-> works transparently for local object
        CORO_ASSERT_NE(opt_f_local.operator->(), nullptr);
        CORO_ASSERT_NE(opt_f_local.get_unsafe_only_for_testing(), nullptr);
        CORO_ASSERT_EQ(opt_f_local.get_unsafe_only_for_testing(), f_local.get());
    }

    // Test 2: Remote object access
    {
        // Create interface (local or remote depending on setup)
        rpc::shared_ptr<xxx::i_baz> baz;
        CORO_ASSERT_EQ(CO_AWAIT example->create_baz(baz), 0);
        CORO_ASSERT_NE(baz, nullptr);

        rpc::optimistic_ptr<xxx::i_baz> opt_baz;
        auto err = CO_AWAIT rpc::make_optimistic(baz, opt_baz);
        CORO_ASSERT_EQ(err, rpc::error::OK());

        // operator-> works transparently for remote proxy
        CORO_ASSERT_NE(opt_baz.operator->(), nullptr);
        CORO_ASSERT_EQ(opt_baz.get_unsafe_only_for_testing(), baz.get());

        // No bad_local_object exception - works transparently
        auto error = CO_AWAIT opt_baz->callback(45);
        CORO_ASSERT_EQ(error, 0);
    }

    CO_RETURN true;
}

TYPED_TEST(optimistic_ptr_test, optimistic_ptr_transparent_access_test)
{
    run_coro_test(*this, [](auto& lib) { return optimistic_ptr_transparent_access_test(lib); });
}

// Test 7: Circular dependency resolution use case
template<class T> CORO_TASK(bool) optimistic_ptr_circular_dependency_test(T& lib)
{
    // Simulate circular dependency scenario:
    // - Host owns children (shared_ptr)
    // - Children hold optimistic_ptr to host (no RAII ownership)

    // Get example object (local or remote depending on setup)
    auto example = lib.get_example();
    CORO_ASSERT_NE(example, nullptr);

    // Create host (will be local or marshalled depending on setup)
    rpc::shared_ptr<xxx::i_foo> host;
    CORO_ASSERT_EQ(CO_AWAIT example->create_foo(host), 0);
    CORO_ASSERT_NE(host, nullptr);

    // Create child
    rpc::shared_ptr<xxx::i_baz> child_ref;
    CORO_ASSERT_EQ(CO_AWAIT host->create_baz_interface(child_ref), 0);

    // Child could hold optimistic_ptr back to host (breaking circular RAII ownership)
    rpc::optimistic_ptr<xxx::i_foo> opt_host;
    auto err = CO_AWAIT rpc::make_optimistic(host, opt_host);
    CORO_ASSERT_EQ(err, rpc::error::OK());
    CORO_ASSERT_NE(opt_host.get_unsafe_only_for_testing(), nullptr);

    auto host_object_id = rpc::casting_interface::get_object_id(*host);
    auto waiter = std::make_shared<object_deletion_waiter>(host_object_id);

    // Schedule verification - handles both local (immediate) and remote (async) cases
    // CRITICAL: Pass opt_host as argument to ensure it lives in the coroutine frame
    waiter->schedule(
        lib.get_root_service(),
        host,
        [](auto opt_host) -> CORO_TASK(void)
        {
            // opt_host still exists but points to deleted object
            // This is correct behavior - circular dependency is broken
            EXPECT_NE(opt_host.get_unsafe_only_for_testing(), nullptr); // Control block remains

            CO_RETURN;
        },
        opt_host); // Pass as argument instead of capturing

    // If we delete host (last shared_ptr), object is destroyed
    // even though optimistic_ptr exists (weak semantics)
    host.reset();

    // For local objects, run verification immediately; for remote, it runs via async callback
    CO_AWAIT waiter->run_if_local();

    CO_RETURN true;
}

TYPED_TEST(optimistic_ptr_test, optimistic_ptr_circular_dependency_test)
{
    GTEST_SKIP() << "skipped for now.";
    run_coro_test(*this, [](auto& lib) { return optimistic_ptr_circular_dependency_test(lib); });
}

// Test 8: Comparison and nullptr operations
template<class T> CORO_TASK(bool) optimistic_ptr_comparison_test(T& lib)
{
    // Get example object (local or remote depending on setup)
    auto example = lib.get_example();
    CORO_ASSERT_NE(example, nullptr);

    // Create two separate foo objects
    rpc::shared_ptr<xxx::i_foo> f1;
    CORO_ASSERT_EQ(CO_AWAIT example->create_foo(f1), 0);
    CORO_ASSERT_NE(f1, nullptr);

    rpc::shared_ptr<xxx::i_foo> f2;
    CORO_ASSERT_EQ(CO_AWAIT example->create_foo(f2), 0);
    CORO_ASSERT_NE(f2, nullptr);

    rpc::optimistic_ptr<xxx::i_foo> opt_f1;
    auto err = CO_AWAIT rpc::make_optimistic(f1, opt_f1);
    CORO_ASSERT_EQ(err, rpc::error::OK());

    rpc::optimistic_ptr<xxx::i_foo> opt_f2;
    err = CO_AWAIT rpc::make_optimistic(f2, opt_f2);
    CORO_ASSERT_EQ(err, rpc::error::OK());

    rpc::optimistic_ptr<xxx::i_foo> opt_null;

    // Test equality using get_unsafe_only_for_testing()
    CORO_ASSERT_NE(opt_f1.get_unsafe_only_for_testing(), opt_f2.get_unsafe_only_for_testing());
    CORO_ASSERT_EQ(opt_f1.get_unsafe_only_for_testing(), opt_f1.get_unsafe_only_for_testing());

    // Test nullptr comparison
    CORO_ASSERT_EQ(opt_null.get_unsafe_only_for_testing(), nullptr);
    CORO_ASSERT_NE(opt_f1.get_unsafe_only_for_testing(), nullptr);

    // Test bool operator
    CORO_ASSERT_EQ(static_cast<bool>(opt_null), false);
    CORO_ASSERT_EQ(static_cast<bool>(opt_f1), true);

    // Test assignment to nullptr
    opt_f1 = nullptr;
    CORO_ASSERT_EQ(opt_f1.get_unsafe_only_for_testing(), nullptr);

    CO_RETURN true;
}

TYPED_TEST(optimistic_ptr_test, optimistic_ptr_comparison_test)
{
    run_coro_test(*this, [](auto& lib) { return optimistic_ptr_comparison_test(lib); });
}

// Test 9: Heterogeneous upcast (statically verifiable)
template<class T> CORO_TASK(bool) optimistic_ptr_heterogeneous_upcast_test(T& lib)
{
    // Get example object (local or remote depending on setup)
    auto example = lib.get_example();
    CORO_ASSERT_NE(example, nullptr);

    // Create foo through example (will be local or marshalled depending on setup)
    rpc::shared_ptr<xxx::i_foo> f;
    CORO_ASSERT_EQ(CO_AWAIT example->create_foo(f), 0);
    CORO_ASSERT_NE(f, nullptr);

    // Create a baz object (implements both i_baz and i_bar)
    rpc::shared_ptr<xxx::i_baz> baz;
    CORO_ASSERT_EQ(CO_AWAIT f->create_baz_interface(baz), 0);
    CORO_ASSERT_NE(baz, nullptr);

    // Create optimistic_ptr<i_baz>
    rpc::optimistic_ptr<xxx::i_baz> opt_baz;
    auto err = CO_AWAIT rpc::make_optimistic(baz, opt_baz);
    CORO_ASSERT_EQ(err, rpc::error::OK());
    CORO_ASSERT_NE(opt_baz.get_unsafe_only_for_testing(), nullptr);

    // Upcast to i_bar (statically verifiable - should compile)
    // Note: This requires i_baz to properly derive from i_bar
    // For now, test with same type conversion
    rpc::optimistic_ptr<xxx::i_baz> opt_baz2(opt_baz);
    CORO_ASSERT_NE(opt_baz2.get_unsafe_only_for_testing(), nullptr);
    CORO_ASSERT_EQ(opt_baz2.get_unsafe_only_for_testing(), opt_baz.get_unsafe_only_for_testing());

    CO_RETURN true;
}

TYPED_TEST(optimistic_ptr_test, optimistic_ptr_heterogeneous_upcast_test)
{
    run_coro_test(*this, [](auto& lib) { return optimistic_ptr_heterogeneous_upcast_test(lib); });
}

// Test 10: Multiple optimistic_ptr instances to same object
template<class T> CORO_TASK(bool) optimistic_ptr_multiple_refs_test(T& lib)
{
    // Get example object (local or remote depending on setup)
    auto example = lib.get_example();
    CORO_ASSERT_NE(example, nullptr);

    // Create foo through example (will be local or marshalled depending on setup)
    rpc::shared_ptr<xxx::i_foo> f;
    CORO_ASSERT_EQ(CO_AWAIT example->create_foo(f), 0);
    CORO_ASSERT_NE(f, nullptr);

    // Create multiple optimistic_ptr instances to same object
    rpc::optimistic_ptr<xxx::i_foo> opt_f1;
    auto err = CO_AWAIT rpc::make_optimistic(f, opt_f1);
    CORO_ASSERT_EQ(err, rpc::error::OK());

    rpc::optimistic_ptr<xxx::i_foo> opt_f2;
    err = CO_AWAIT rpc::make_optimistic(f, opt_f2);
    CORO_ASSERT_EQ(err, rpc::error::OK());

    rpc::optimistic_ptr<xxx::i_foo> opt_f3(opt_f1);
    rpc::optimistic_ptr<xxx::i_foo> opt_f4 = opt_f2;

    // All should point to same object
    CORO_ASSERT_EQ(opt_f1.get_unsafe_only_for_testing(), f.get());
    CORO_ASSERT_EQ(opt_f2.get_unsafe_only_for_testing(), f.get());
    CORO_ASSERT_EQ(opt_f3.get_unsafe_only_for_testing(), f.get());
    CORO_ASSERT_EQ(opt_f4.get_unsafe_only_for_testing(), f.get());

    // All should be equal
    CORO_ASSERT_EQ(opt_f1.get_unsafe_only_for_testing(), opt_f2.get_unsafe_only_for_testing());
    CORO_ASSERT_EQ(opt_f2.get_unsafe_only_for_testing(), opt_f3.get_unsafe_only_for_testing());
    CORO_ASSERT_EQ(opt_f3.get_unsafe_only_for_testing(), opt_f4.get_unsafe_only_for_testing());

    CO_RETURN true;
}

TYPED_TEST(optimistic_ptr_test, optimistic_ptr_multiple_refs_test)
{
    run_coro_test(*this, [](auto& lib) { return optimistic_ptr_multiple_refs_test(lib); });
}

struct object_gone_event : public rpc::service_event
{
    std::weak_ptr<rpc::service> svc_;
    std::function<CORO_TASK(void)()> callback_;
    rpc::object object_id_;
    rpc::destination_zone destination_;
    virtual ~object_gone_event()
    {
        auto svc = svc_.lock();
        if (svc)
        {
            // svc->remove_service_event(rpc::service_event::shared_from_this());
        }
    }

    static std::shared_ptr<object_gone_event> create(std::shared_ptr<rpc::service> svc,
        std::function<CORO_TASK(void)()> callback,
        rpc::object object_id,
        rpc::destination_zone destination)
    {
        auto ret = std::make_shared<object_gone_event>();
        ret->object_id_ = object_id;
        ret->destination_ = destination;
        ret->callback_ = callback;
        svc->add_service_event(ret);
        ret->svc_ = svc; // do it after addin service
        return ret;
    }

    /**
     * @brief Called when an object is released in a remote zone
     * @param object_id The ID of the released object
     * @param destination The zone where the object was released
     */
    virtual CORO_TASK(void) on_object_released(rpc::object object_id, rpc::destination_zone destination) override
    {
        if (object_id_ == object_id && destination_ == destination)
        {
            CO_AWAIT callback_();
        }
        CO_RETURN;
    }
};

// Test 11: optimistic_ptr OBJECT_GONE behavior when remote stub is deleted
template<class T> CORO_TASK(bool) optimistic_ptr_object_gone_test(T& lib)
{
    // Get example object (local or remote depending on setup)
    auto example = lib.get_example();
    CORO_ASSERT_NE(example, nullptr);

    // Create foo through example (will be local or marshalled depending on setup)
    rpc::shared_ptr<xxx::i_foo> f;
    CORO_ASSERT_EQ(CO_AWAIT example->create_foo(f), 0);
    CORO_ASSERT_NE(f, nullptr);

    // Create baz interface (local or marshalled depending on setup)
    rpc::shared_ptr<xxx::i_baz> baz;
    CORO_ASSERT_EQ(CO_AWAIT f->create_baz_interface(baz), 0);
    CORO_ASSERT_NE(baz, nullptr);

    // Get object_id directly from the interface (avoids service mutex)
    auto baz_object_id = rpc::casting_interface::get_object_id(*baz);

    // Test OBJECT_GONE for REMOTE objects only
    // Create optimistic_ptr from shared_ptr
    rpc::optimistic_ptr<xxx::i_baz> opt_baz;
    auto err = CO_AWAIT rpc::make_optimistic(baz, opt_baz);
    CORO_ASSERT_EQ(err, rpc::error::OK());
    CORO_ASSERT_NE(opt_baz.get_unsafe_only_for_testing(), nullptr);

    // First call should work - shared_ptr keeps stub alive
    auto error1 = CO_AWAIT opt_baz->callback(42);
    CORO_ASSERT_EQ(error1, 0);

    // Register for object deletion notification with continuation for verification
    auto waiter = std::make_shared<object_deletion_waiter>(baz_object_id);

    // Schedule verification - handles both local (immediate) and remote (async) cases
    // CRITICAL: Pass opt_baz as argument to ensure it lives in the coroutine frame
    waiter->schedule(
        lib.get_root_service(),
        baz,
        [](auto opt_baz) -> CORO_TASK(void)
        {
            // This runs after the object is deleted
            // Second call through optimistic_ptr should fail with OBJECT_GONE
            // The optimistic_ptr still exists but the remote stub has been deleted
            auto error2 = CO_AWAIT opt_baz->callback(43);
            EXPECT_EQ(error2, rpc::error::OBJECT_GONE());

            // The optimistic_ptr itself remains valid (pointer not null)
            EXPECT_NE(opt_baz.get_unsafe_only_for_testing(), nullptr);

            CO_RETURN;
        },
        opt_baz); // Pass as argument instead of capturing

    // Release the shared_ptr - for remote this triggers async cleanup, for local it's immediate
    baz.reset();
    f.reset();

    // For local objects, run verification immediately; for remote, it runs via async callback
    CO_AWAIT waiter->run_if_local();

    CO_RETURN true;
}

TYPED_TEST(optimistic_ptr_test, optimistic_ptr_object_gone_test)
{
    GTEST_SKIP() << "skipped for now.";
    run_coro_test(*this, [](auto& lib) { return optimistic_ptr_object_gone_test(lib); });
}

// Test 12: [post] attribute with optimistic_ptr - fire-and-forget through optimistic pointer
template<class T> CORO_TASK(bool) coro_post_with_optimistic_ptr(T& lib)
{
    // Create a foo instance to test with
    rpc::shared_ptr<xxx::i_foo> i_foo_ptr;
    auto ret = CO_AWAIT lib.get_example()->create_foo(i_foo_ptr);
    CORO_ASSERT_EQ(ret, rpc::error::OK());
    CORO_ASSERT_NE(i_foo_ptr, nullptr);

    // Convert to optimistic_ptr
    rpc::optimistic_ptr<xxx::i_foo> opt_foo;
    auto opt_ret = CO_AWAIT rpc::make_optimistic(i_foo_ptr, opt_foo);
    CORO_ASSERT_EQ(opt_ret, rpc::error::OK());

    // Clear any existing messages first (through optimistic pointer)
    auto clear_ret = CO_AWAIT opt_foo->clear_recorded_messages();
    CORO_ASSERT_EQ(clear_ret, rpc::error::OK());

    // Test 1: Send multiple [post] messages through optimistic_ptr and verify ordering
    const int num_messages = 10;
    for (int i = 0; i < num_messages; ++i)
    {
        // [post] methods through optimistic_ptr - fire-and-forget
        auto post_ret = CO_AWAIT opt_foo->record_message(i);
        CORO_ASSERT_EQ(post_ret, rpc::error::OK());
    }

    // Give some time for all messages to be processed
#ifdef CANOPY_BUILD_COROUTINE
    for (int i = 0; i < num_messages; ++i)
    {
        lib.get_scheduler()->process_events(std::chrono::milliseconds(1));
    }
#else
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
#endif

    // Retrieve recorded messages and verify ordering (through optimistic pointer)
    std::vector<int> recorded_messages;
    auto get_ret = CO_AWAIT opt_foo->get_recorded_messages(recorded_messages);
    CORO_ASSERT_EQ(get_ret, rpc::error::OK());

    // Verify all messages were received
    CORO_ASSERT_EQ(recorded_messages.size(), num_messages);

    // Verify messages were received in the order they were sent
    for (int i = 0; i < num_messages; ++i)
    {
        CORO_ASSERT_EQ(recorded_messages[i], i);
    }

    // Test 2: Clear and verify
    clear_ret = CO_AWAIT opt_foo->clear_recorded_messages();
    CORO_ASSERT_EQ(clear_ret, rpc::error::OK());

    recorded_messages.clear();
    get_ret = CO_AWAIT opt_foo->get_recorded_messages(recorded_messages);
    CORO_ASSERT_EQ(get_ret, rpc::error::OK());
    CORO_ASSERT_EQ(recorded_messages.size(), 0);

    // Test 3: Larger batch through optimistic_ptr
    const int large_batch = 50;
    for (int i = 0; i < large_batch; ++i)
    {
        auto post_ret = CO_AWAIT opt_foo->record_message(i * 3); // Send multiples of 3
        CORO_ASSERT_EQ(post_ret, rpc::error::OK());
    }

#ifdef CANOPY_BUILD_COROUTINE
    for (int i = 0; i < large_batch; ++i)
    {
        lib.get_scheduler()->process_events(std::chrono::milliseconds(1));
    }
#else
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
#endif

    recorded_messages.clear();
    get_ret = CO_AWAIT opt_foo->get_recorded_messages(recorded_messages);
    CORO_ASSERT_EQ(get_ret, rpc::error::OK());
    CORO_ASSERT_EQ(recorded_messages.size(), large_batch);

    // Verify ordering for large batch
    for (int i = 0; i < large_batch; ++i)
    {
        CORO_ASSERT_EQ(recorded_messages[i], i * 3);
    }

    RPC_INFO("Post with optimistic_ptr test completed - all {} messages received in order through optimistic pointer",
        large_batch);

    CO_RETURN true;
}

TYPED_TEST(optimistic_ptr_test, post_with_optimistic_ptr)
{
    run_coro_test(*this, [](auto& lib) { return coro_post_with_optimistic_ptr<TypeParam>(lib); });
}

// ============================================================================
// Marshalled optimistic_ptr via IDL tests
// ============================================================================

// Test: set and get optimistic_ptr through IDL-marshalled methods
template<class T> CORO_TASK(bool) optimistic_ptr_set_and_get_via_idl_test(T& lib)
{
    auto example = lib.get_example();
    CORO_ASSERT_NE(example, nullptr);

    // Create a foo via the example zone
    rpc::shared_ptr<xxx::i_foo> f;
    CORO_ASSERT_EQ(CO_AWAIT example->create_foo(f), rpc::error::OK());
    CORO_ASSERT_NE(f, nullptr);

    // Create an optimistic_ptr from the shared_ptr
    rpc::optimistic_ptr<xxx::i_foo> opt_f;
    auto err = CO_AWAIT rpc::make_optimistic(f, opt_f);
    CORO_ASSERT_EQ(err, rpc::error::OK());
    CORO_ASSERT_NE(opt_f.get_unsafe_only_for_testing(), nullptr);

    // Send it to the remote zone via IDL-marshalled set_optimistic_ptr
    err = CO_AWAIT example->set_optimistic_ptr(opt_f);
    CORO_ASSERT_EQ(err, rpc::error::OK());

    // Retrieve it back via IDL-marshalled get_optimistic_ptr
    rpc::optimistic_ptr<xxx::i_foo> opt_f_out;
    err = CO_AWAIT example->get_optimistic_ptr(opt_f_out);
    CORO_ASSERT_EQ(err, rpc::error::OK());
    CORO_ASSERT_NE(opt_f_out.get_unsafe_only_for_testing(), nullptr);

    // The shared_ptr f is still alive, so calling through the returned optimistic_ptr should succeed
    err = CO_AWAIT opt_f_out->do_something_in_val(42);
    CORO_ASSERT_EQ(err, rpc::error::OK());

    CO_RETURN true;
}

TYPED_TEST(optimistic_ptr_test, optimistic_ptr_set_and_get_via_idl)
{
    run_coro_test(*this, [](auto& lib) { return optimistic_ptr_set_and_get_via_idl_test(lib); });
}

// Test: get returns OBJECT_GONE when shared_ptr is released
template<class T> CORO_TASK(bool) optimistic_ptr_get_returns_object_gone_when_shared_released_test(T& lib)
{
    auto example = lib.get_example();
    CORO_ASSERT_NE(example, nullptr);

    // Create a foo via the example zone
    rpc::shared_ptr<xxx::i_foo> f;
    CORO_ASSERT_EQ(CO_AWAIT example->create_foo(f), rpc::error::OK());
    CORO_ASSERT_NE(f, nullptr);

    // Create an optimistic_ptr and send to remote
    rpc::optimistic_ptr<xxx::i_foo> opt_f;
    auto err = CO_AWAIT rpc::make_optimistic(f, opt_f);
    CORO_ASSERT_EQ(err, rpc::error::OK());

    err = CO_AWAIT example->set_optimistic_ptr(opt_f);
    CORO_ASSERT_EQ(err, rpc::error::OK());

#ifdef CANOPY_BUILD_COROUTINE
    rpc::event ev;
    auto cb = object_gone_event::create(
        lib.get_root_service(),
        [&]() -> CORO_TASK(void)
        {
            ev.set();
            CO_RETURN;
        },
        rpc::casting_interface::get_object_id(*f),
        rpc::casting_interface::get_destination_zone(*f));

#endif
    // Release the shared_ptr - the underlying object should be destroyed
    f.reset();

#ifdef CANOPY_BUILD_COROUTINE
    CO_AWAIT ev.wait();
    lib.get_root_service()->remove_service_event(cb);
    cb.reset();
#endif

    // Calling through it should return OBJECT_GONE since the shared_ptr is released
    err = CO_AWAIT opt_f->do_something_in_val(42);
    CORO_ASSERT_EQ(err, rpc::error::OBJECT_GONE());

    opt_f.reset();

    // Retrieve via get_optimistic_ptr
    rpc::optimistic_ptr<xxx::i_foo> opt_f_out;
    err = CO_AWAIT example->get_optimistic_ptr(opt_f_out);
    CORO_ASSERT_EQ(err, rpc::error::OK());

    CORO_ASSERT_EQ(nullptr, opt_f_out.get());

    // clean up example so that it does not trigger an unclean service error
    err = CO_AWAIT example->set_optimistic_ptr(nullptr);
    CORO_ASSERT_EQ(err, rpc::error::OK());

    CO_RETURN true;
}

TYPED_TEST(optimistic_ptr_test, optimistic_ptr_get_returns_object_gone_when_shared_released)
{
    run_coro_test(*this, [](auto& lib) { return optimistic_ptr_get_returns_object_gone_when_shared_released_test(lib); });
}

// Test: null optimistic_ptr roundtrip
template<class T> CORO_TASK(bool) optimistic_ptr_null_roundtrip_test(T& lib)
{
    auto example = lib.get_example();
    CORO_ASSERT_NE(example, nullptr);

    // Create a default (null) optimistic_ptr
    rpc::optimistic_ptr<xxx::i_foo> opt_null;
    CORO_ASSERT_EQ(static_cast<bool>(opt_null), false);

    // Send it via set_optimistic_ptr
    auto err = CO_AWAIT example->set_optimistic_ptr(opt_null);
    CORO_ASSERT_EQ(err, rpc::error::OK());

    // Retrieve it via get_optimistic_ptr
    rpc::optimistic_ptr<xxx::i_foo> opt_null_out;
    err = CO_AWAIT example->get_optimistic_ptr(opt_null_out);
    CORO_ASSERT_EQ(err, rpc::error::OK());

    // The retrieved pointer should be null
    CORO_ASSERT_EQ(static_cast<bool>(opt_null_out), false);
    CORO_ASSERT_EQ(opt_null_out.get_unsafe_only_for_testing(), nullptr);

    CO_RETURN true;
}

TYPED_TEST(optimistic_ptr_test, optimistic_ptr_null_roundtrip)
{
    run_coro_test(*this, [](auto& lib) { return optimistic_ptr_null_roundtrip_test(lib); });
}
