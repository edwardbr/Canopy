/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <gtest/gtest.h>

#include <rpc/internal/executor/blocking_executor.h>
#include <rpc/internal/polyfill/event.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace
{
    TEST(
        BlockingExecutor,
        ConstructsWithDefaultThreadCount)
    {
        rpc::blocking_executor ex;
        EXPECT_GT(ex.worker_count(), 0u);
        EXPECT_FALSE(ex.is_shutdown());
    }

    TEST(
        BlockingExecutor,
        ConstructsWithExplicitThreadCount)
    {
        rpc::blocking_executor::options opts;
        opts.thread_count = 3;
        rpc::blocking_executor ex(opts);
        EXPECT_EQ(ex.worker_count(), 3u);
    }

    TEST(
        BlockingExecutor,
        PostsCallableAndRunsIt)
    {
        rpc::blocking_executor ex;
        rpc::event done;
        ASSERT_TRUE(ex.post([&] { done.set(); }));
        done.wait();
        SUCCEED();
    }

    TEST(
        BlockingExecutor,
        PostExecutesMultipleCallables)
    {
        rpc::blocking_executor::options opts;
        opts.thread_count = 4;
        rpc::blocking_executor ex(opts);
        std::atomic<int> counter{0};
        constexpr int kJobs = 100;
        for (int i = 0; i < kJobs; ++i)
            ASSERT_TRUE(ex.post([&counter] { ++counter; }));
        // Allow workers to drain. Loop with shutdown so the test is bounded.
        ex.shutdown();
        EXPECT_EQ(counter.load(), kJobs);
    }

    TEST(
        BlockingExecutor,
        PostAfterShutdownReturnsFalse)
    {
        rpc::blocking_executor ex;
        ex.shutdown();
        EXPECT_TRUE(ex.is_shutdown());
        EXPECT_FALSE(ex.post([] { /* never runs */ }));
    }

    TEST(
        BlockingExecutor,
        ScheduleAfterCompletesNormally)
    {
        rpc::blocking_executor ex;
        auto start = std::chrono::steady_clock::now();
        EXPECT_TRUE(ex.schedule_after(std::chrono::milliseconds(50)));
        auto elapsed = std::chrono::steady_clock::now() - start;
        EXPECT_GE(elapsed, std::chrono::milliseconds(40));
    }

    TEST(
        BlockingExecutor,
        ScheduleAfterIsInterruptedByShutdown)
    {
        rpc::blocking_executor ex;
        rpc::event waiting;
        rpc::event finished;
        bool returned = true;
        std::thread sleeper(
            [&]
            {
                waiting.set();
                returned = ex.schedule_after(std::chrono::seconds(30));
                finished.set();
            });
        waiting.wait();
        // Give the sleeper a moment to actually enter the timed wait.
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        ex.shutdown();
        finished.wait();
        sleeper.join();
        EXPECT_FALSE(returned);
    }

    TEST(
        BlockingExecutor,
        ShutdownIsIdempotent)
    {
        rpc::blocking_executor ex;
        ex.shutdown();
        ex.shutdown(); // must not double-join or crash
        SUCCEED();
    }

    TEST(
        BlockingExecutor,
        SchedulerIsNoop)
    {
        rpc::blocking_executor ex;
        // schedule() must compile and return void without parking the caller.
        ex.schedule();
        SUCCEED();
    }
} // namespace
