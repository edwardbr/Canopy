/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include <common/foo_impl.h>
#include <gtest/gtest.h>
#include <rpc/rpc.h>

namespace
{
    TEST(
        atomic_smart_ptr_tests,
        std_shared_ptr_load_store_exchange_compare)
    {
        auto first = std::make_shared<int>(1);
        auto second = std::make_shared<int>(2);

        std::atomic<std::shared_ptr<int>> ptr(first);
        EXPECT_EQ(ptr.load(), first);

        ptr.store(second);
        EXPECT_EQ(ptr.load(), second);

        auto old = ptr.exchange(first);
        EXPECT_EQ(old, second);
        EXPECT_EQ(ptr.load(), first);

        std::shared_ptr<int> expected = second;
        EXPECT_FALSE(ptr.compare_exchange_strong(expected, second));
        EXPECT_EQ(expected, first);
        EXPECT_EQ(ptr.load(), first);

        EXPECT_TRUE(ptr.compare_exchange_strong(expected, second));
        EXPECT_EQ(ptr.load(), second);
        EXPECT_FALSE(ptr.is_lock_free());
        static_assert(!std::atomic<std::shared_ptr<int>>::is_always_lock_free);
    }

    TEST(
        atomic_smart_ptr_tests,
        rpc_shared_ptr_load_store_exchange_compare)
    {
        rpc::shared_ptr<xxx::i_foo> first(new marshalled_tests::foo);
        rpc::shared_ptr<xxx::i_foo> second(new marshalled_tests::foo);

        std::atomic<rpc::shared_ptr<xxx::i_foo>> ptr(first);
        EXPECT_EQ(ptr.load().get(), first.get());

        ptr.store(second);
        EXPECT_EQ(ptr.load().get(), second.get());

        auto old = ptr.exchange(first);
        EXPECT_EQ(old.get(), second.get());
        EXPECT_EQ(ptr.load().get(), first.get());

        rpc::shared_ptr<xxx::i_foo> expected = second;
        EXPECT_FALSE(ptr.compare_exchange_strong(expected, second));
        EXPECT_EQ(expected.get(), first.get());
        EXPECT_EQ(ptr.load().get(), first.get());

        EXPECT_TRUE(ptr.compare_exchange_strong(expected, second));
        EXPECT_EQ(ptr.load().get(), second.get());
        EXPECT_FALSE(ptr.is_lock_free());
        static_assert(!std::atomic<rpc::shared_ptr<xxx::i_foo>>::is_always_lock_free);
    }

    TEST(
        atomic_smart_ptr_tests,
        rpc_optimistic_ptr_null_load_store_exchange_compare)
    {
        std::atomic<rpc::optimistic_ptr<xxx::i_foo>> ptr;

        EXPECT_TRUE(ptr.load().is_null());
        ptr.store(nullptr);
        EXPECT_TRUE(ptr.load().is_null());

        auto old = ptr.exchange(nullptr);
        EXPECT_TRUE(old.is_null());

        rpc::optimistic_ptr<xxx::i_foo> expected;
        EXPECT_TRUE(ptr.compare_exchange_strong(expected, nullptr));
        EXPECT_TRUE(ptr.load().is_null());
        EXPECT_FALSE(ptr.is_lock_free());
        static_assert(!std::atomic<rpc::optimistic_ptr<xxx::i_foo>>::is_always_lock_free);
    }

    TEST(
        atomic_smart_ptr_tests,
        concurrent_rpc_shared_ptr_loads_get_stable_copies)
    {
        rpc::shared_ptr<xxx::i_foo> first(new marshalled_tests::foo);
        rpc::shared_ptr<xxx::i_foo> second(new marshalled_tests::foo);
        std::atomic<rpc::shared_ptr<xxx::i_foo>> ptr(first);
        std::atomic<bool> stop{false};

        std::thread writer(
            [&]
            {
                for (int i = 0; i != 2000; ++i)
                    ptr.store((i & 1) ? first : second);
                stop.store(true);
            });

        std::vector<std::thread> readers;
        for (int i = 0; i != 4; ++i)
        {
            readers.emplace_back(
                [&]
                {
                    while (!stop.load())
                    {
                        auto local = ptr.load();
                        ASSERT_TRUE(local.get() == first.get() || local.get() == second.get());
                    }
                });
        }

        writer.join();
        for (auto& reader : readers)
            reader.join();
    }
}
