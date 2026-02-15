// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#pragma once

#include "gtest/gtest.h"
#include <mutex>
#include <set>

// Forward declaration of object_deletion_waiter for run_coro_test
class object_deletion_waiter;

// Generic fixture used to instantiate RPC host tests for multiple setups.
template<class T> class type_test : public testing::Test
{
    T lib_;

public:
    T& get_lib() { return lib_; }
    const T& get_lib() const { return lib_; }

    void SetUp() override { this->lib_.set_up(); }
    void TearDown() override { this->lib_.tear_down(); }
};

// Helper class to wait for object deletion notification and run continuation
class object_deletion_waiter : public rpc::service_event
{
    rpc::object expected_object_id_;
    std::function<CORO_TASK(void)()> continuation_;
    bool continuation_scheduled_ = false;
    bool continuation_completed_ = false;
    bool is_local_ = true;

    // Static tracking of all active waiter instances
    static std::mutex active_waiters_mutex_;
    static std::set<object_deletion_waiter*> active_waiters_;

    void register_active()
    {
        std::lock_guard<std::mutex> lock(active_waiters_mutex_);
        active_waiters_.insert(this);
    }

    void unregister_active()
    {
        std::lock_guard<std::mutex> lock(active_waiters_mutex_);
        active_waiters_.erase(this);
    }

public:
    object_deletion_waiter(rpc::object object_id)
        : expected_object_id_(object_id)
    {
        register_active();
    }

    ~object_deletion_waiter() { unregister_active(); }

    // Check if any waiters are still pending
    static bool any_pending()
    {
        std::lock_guard<std::mutex> lock(active_waiters_mutex_);
        for (auto* waiter : active_waiters_)
        {
            if (waiter->continuation_scheduled_ && !waiter->continuation_completed_)
            {
                return true;
            }
        }
        return false;
    }

    // Schedule the verification to run - either immediately (local) or after async cleanup (remote)
    // Captures are passed as variadic arguments to ensure they live in the coroutine frame
    template<typename Lambda, typename... Args>
    void schedule(std::shared_ptr<rpc::service> service,
        const rpc::shared_ptr<rpc::casting_interface>& obj,
        Lambda&& verification_lambda,
        Args&&... args)
    {
        is_local_ = obj.get()->is_local();
        continuation_scheduled_ = true;

        if (is_local_)
        {
            // For local objects, cleanup is synchronous - set continuation to run immediately
            // Capture args by value to move them into the coroutine frame
            continuation_ = [this, verification_lambda, ... captured = std::forward<Args>(args)]() -> CORO_TASK(void)
            {
                CO_AWAIT verification_lambda(captured...);
                continuation_completed_ = true;
                CO_RETURN;
            };
        }
        else
        {
            // For remote objects, wrap verification in cleanup logic
            // Capture args by value to move them into the coroutine frame
            auto self = shared_from_this();
            continuation_
                = [service, self, verification_lambda, ... captured = std::forward<Args>(args)]() -> CORO_TASK(void)
            {
                CO_AWAIT verification_lambda(captured...);
                ((object_deletion_waiter*)self.get())->continuation_completed_ = true;
                // Remove event listener after verification
                service->remove_service_event(self);
                CO_RETURN;
            };
            // Register for async notification
            service->add_service_event(shared_from_this());
        }
    }

    // Call this after reset() to run local verification immediately
    CORO_TASK(void) run_if_local()
    {
        if (continuation_ && continuation_scheduled_ && is_local_)
        {
            CO_AWAIT continuation_();
        }
        CO_RETURN;
    }

    CORO_TASK(void) on_object_released(rpc::object object_id, rpc::destination_zone destination) override
    {
        if (object_id == expected_object_id_ && continuation_scheduled_ && !is_local_)
        {
            // Run the continuation
            CO_AWAIT continuation_();
        }
        CO_RETURN;
    }

    bool has_continuation_run() const { return continuation_scheduled_; }
    bool is_completed() const { return continuation_completed_; }
};

// Static member definitions
inline std::mutex object_deletion_waiter::active_waiters_mutex_;
inline std::set<object_deletion_waiter*> object_deletion_waiter::active_waiters_;

// Universal template-based coroutine test dispatcher
// All coroutines now return CORO_TASK(bool) and use check_for_error
// The lambda wrapper sets is_ready = true when the coroutine completes
// After the main coroutine completes, keeps processing events until all object_deletion_waiters complete
template<typename TestFixture, typename CoroFunc, typename... Args>
void run_coro_test(TestFixture& test_fixture, CoroFunc&& coro_function, Args&&... args)
{
    auto& lib = test_fixture.get_lib();
#ifdef CANOPY_BUILD_COROUTINE
    bool is_ready = false;
    // Create a lambda that calls the coroutine function and sets is_ready when done
    auto wrapper_function = [&]() -> CORO_TASK(bool)
    {
        auto result = CO_AWAIT coro_function(lib, std::forward<Args>(args)...);
        is_ready = true; // Set is_ready when coroutine completes
        CO_RETURN result;
    };

    RPC_ASSERT(lib.get_scheduler()->spawn(lib.check_for_error(wrapper_function())));

    // Process events until main coroutine completes
    while (!is_ready)
    {
        lib.get_scheduler()->process_events(std::chrono::milliseconds(1));
    }

    // Keep processing events until all object_deletion_waiters complete
    // This ensures async cleanup tasks (like remote object deletion notifications) finish
    while (object_deletion_waiter::any_pending())
    {
        lib.get_scheduler()->process_events(std::chrono::milliseconds(1));
    }
#else
    coro_function(lib, std::forward<Args>(args)...);
#endif
    ASSERT_EQ(lib.error_has_occurred(), false);
}
