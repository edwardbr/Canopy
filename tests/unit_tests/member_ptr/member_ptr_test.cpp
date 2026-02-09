/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <memory>
#include <chrono>
#include <atomic>
#include <functional>
#include <iostream>

// Include the actual rpc::shared_ptr implementation first
#include "rpc/rpc.h"
#include "member_ptr_test/member_ptr_test.h"

// Forward declaration for the test interface
int construction_count = 0;
int destruction_count = 0;

extern "C"
{
    void rpc_log(int level, const char* str, size_t sz)
    {
        std::string message(str, sz);
        switch (level)
        {
        case 0:
            printf("[DEBUG] %s\n", message.c_str());
            break;
        case 1:
            printf("[TRACE] %s\n", message.c_str());
            break;
        case 2:
            printf("[INFO] %s\n", message.c_str());
            break;
        case 3:
            printf("[WARN] %s\n", message.c_str());
            break;
        case 4:
            printf("[ERROR] %s\n", message.c_str());
            break;
        case 5:
            printf("[CRITICAL] %s\n", message.c_str());
            break;
        default:
            printf("[LOG %d] %s\n", level, message.c_str());
            break;
        }
    }
}

// Test fixture for rpc::member_ptr tests
class MemberPtrTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        construction_count = 0;
        destruction_count = 0;
    }

    void TearDown() override
    {
        EXPECT_EQ(construction_count, destruction_count)
            << "Memory leak detected: not all test_impl objects were destroyed";
    }
};

class test_impl : public rpc::base<test_impl, member_ptr_test::i_test>
{
    int val_;

public:
    test_impl(int val)
        : val_(val)
    {
        construction_count++;
    }
    virtual ~test_impl() { destruction_count++; }

    CORO_TASK(int) test(int val) override
    {
        std::cout << val;
        CO_RETURN rpc::error::OK();
    }

    CORO_TASK(int) get_value(int& val) override
    {
        val = val_;
        CO_RETURN rpc::error::OK();
    }
};

// Test default constructor for rpc::member_ptr
TEST_F(MemberPtrTest, RpcDefaultConstructor)
{
    rpc::member_ptr<test_impl> ptr;
    auto retrieved = ptr.get_nullable();
    EXPECT_EQ(retrieved, nullptr);
}

// Test constructor from rpc::shared_ptr for rpc::member_ptr
TEST_F(MemberPtrTest, RpcConstructFromSharedPtr)
{
    auto resource = rpc::make_shared<test_impl>(42);
    rpc::member_ptr<test_impl> ptr(resource);

    auto retrieved = ptr.get_nullable();
    ASSERT_NE(retrieved, nullptr);
    int val = 0;
    auto result = retrieved->get_value(val);
    EXPECT_EQ(result, rpc::error::OK());
    EXPECT_EQ(val, 42);
    EXPECT_EQ(resource.use_count(), 3); // Original + copy in member_ptr
}

// Test move constructor from rpc::shared_ptr for rpc::member_ptr
TEST_F(MemberPtrTest, RpcConstructFromMovedSharedPtr)
{
    auto resource = rpc::make_shared<test_impl>(42);
    rpc::member_ptr<test_impl> ptr(std::move(resource));

    auto retrieved = ptr.get_nullable();
    ASSERT_NE(retrieved, nullptr);
    int val = 0;
    auto result = retrieved->get_value(val);
    EXPECT_EQ(result, rpc::error::OK());
    EXPECT_EQ(val, 42);
}

// Test copy constructor for rpc::member_ptr
TEST_F(MemberPtrTest, RpcCopyConstructor)
{
    auto resource = rpc::make_shared<test_impl>(42);
    rpc::member_ptr<test_impl> original(resource);

    rpc::member_ptr<test_impl> copy(original);
    auto original_retrieved = original.get_nullable();
    auto copy_retrieved = copy.get_nullable();

    ASSERT_NE(original_retrieved, nullptr);
    ASSERT_NE(copy_retrieved, nullptr);
    int val1 = 0, val2 = 0;
    auto result = original_retrieved->get_value(val1);
    EXPECT_EQ(result, rpc::error::OK());
    result = copy_retrieved->get_value(val2);
    EXPECT_EQ(result, rpc::error::OK());
    EXPECT_EQ(val1, 42);
    EXPECT_EQ(val2, 42);
    EXPECT_EQ(original_retrieved, copy_retrieved); // Same underlying object
}

// Test move constructor for rpc::member_ptr
TEST_F(MemberPtrTest, RpcMoveConstructor)
{
    auto resource = rpc::make_shared<test_impl>(42);
    rpc::member_ptr<test_impl> original(resource);

    rpc::member_ptr<test_impl> moved(std::move(original));
    auto original_retrieved = original.get_nullable();
    auto moved_retrieved = moved.get_nullable();

    ASSERT_NE(moved_retrieved, nullptr);
    int val = 0;
    auto result = moved_retrieved->get_value(val);
    EXPECT_EQ(result, rpc::error::OK());
    EXPECT_EQ(val, 42);
    // Note: After move, original may have null value, but this depends on implementation
}

// Test copy assignment for rpc::member_ptr
TEST_F(MemberPtrTest, RpcCopyAssignment)
{
    auto resource1 = rpc::make_shared<test_impl>(42);
    auto resource2 = rpc::make_shared<test_impl>(84);

    rpc::member_ptr<test_impl> ptr1(resource1);
    rpc::member_ptr<test_impl> ptr2(resource2);

    ptr1 = ptr2; // Copy assignment

    auto retrieved1 = ptr1.get_nullable();
    auto retrieved2 = ptr2.get_nullable();

    ASSERT_NE(retrieved1, nullptr);
    ASSERT_NE(retrieved2, nullptr);
    int val1 = 0, val2 = 0;
    auto result = retrieved1->get_value(val1);
    EXPECT_EQ(result, rpc::error::OK());
    result = retrieved2->get_value(val2);
    EXPECT_EQ(result, rpc::error::OK());
    EXPECT_EQ(val1, 84);
    EXPECT_EQ(val2, 84);
    EXPECT_EQ(retrieved1, retrieved2); // Same underlying object
}

// Test move assignment for rpc::member_ptr
TEST_F(MemberPtrTest, RpcMoveAssignment)
{
    auto resource1 = rpc::make_shared<test_impl>(42);
    auto resource2 = rpc::make_shared<test_impl>(84);

    rpc::member_ptr<test_impl> ptr1(resource1);
    rpc::member_ptr<test_impl> ptr2(resource2);

    ptr1 = std::move(ptr2); // Move assignment

    auto retrieved1 = ptr1.get_nullable();
    auto retrieved2 = ptr2.get_nullable();

    ASSERT_NE(retrieved1, nullptr);
    int val = 0;
    auto result = retrieved1->get_value(val);
    EXPECT_EQ(result, rpc::error::OK());
    EXPECT_EQ(val, 84);
    // After move, ptr2 may contain null
}

// Test assignment from rpc::shared_ptr for rpc::member_ptr
TEST_F(MemberPtrTest, RpcAssignSharedPtr)
{
    auto resource = rpc::make_shared<test_impl>(42);
    rpc::member_ptr<test_impl> ptr;

    ptr = resource;

    auto retrieved = ptr.get_nullable();
    ASSERT_NE(retrieved, nullptr);
    int val = 0;
    auto result = retrieved->get_value(val);
    EXPECT_EQ(result, rpc::error::OK());
    EXPECT_EQ(val, 42);
}

// Test move assignment from rpc::shared_ptr for rpc::member_ptr
TEST_F(MemberPtrTest, RpcAssignMovedSharedPtr)
{
    auto resource = rpc::make_shared<test_impl>(42);
    rpc::member_ptr<test_impl> ptr;

    ptr = std::move(resource);

    auto retrieved = ptr.get_nullable();
    ASSERT_NE(retrieved, nullptr);
    int val = 0;
    auto result = retrieved->get_value(val);
    EXPECT_EQ(result, rpc::error::OK());
    EXPECT_EQ(val, 42);
}

// Test get_nullable for rpc::member_ptr
TEST_F(MemberPtrTest, RpcGetNullable)
{
    auto resource = rpc::make_shared<test_impl>(42);
    rpc::member_ptr<test_impl> ptr(resource);

    auto retrieved = ptr.get_nullable();
    ASSERT_NE(retrieved, nullptr);
    int val = 0;
    auto result = retrieved->get_value(val);
    EXPECT_EQ(result, rpc::error::OK());
    EXPECT_EQ(val, 42);

    // Test multiple calls return equivalent results
    auto retrieved2 = ptr.get_nullable();
    EXPECT_EQ(retrieved, retrieved2);
}

// Test reset for rpc::member_ptr
TEST_F(MemberPtrTest, RpcReset)
{
    auto resource = rpc::make_shared<test_impl>(42);
    rpc::member_ptr<test_impl> ptr(resource);

    ptr.reset();

    auto retrieved = ptr.get_nullable();
    EXPECT_EQ(retrieved, nullptr);
}

// Test destructor behavior for rpc::member_ptr
TEST_F(MemberPtrTest, RpcDestructor)
{
    rpc::shared_ptr<test_impl> resource;
    {
        resource = rpc::make_shared<test_impl>(42);
        rpc::member_ptr<test_impl> ptr(resource);
        auto retrieved = ptr.get_nullable();
        ASSERT_NE(retrieved, nullptr);
        int val = 0;
        auto result = retrieved->get_value(val);
        EXPECT_EQ(result, rpc::error::OK());
        EXPECT_EQ(val, 42);
        // ptr goes out of scope here
    }
    // At this point, the member_ptr is destroyed but resource should still be valid
    int val = 0;
    auto result = resource->get_value(val);
    EXPECT_EQ(result, rpc::error::OK());
    EXPECT_EQ(val, 42);
    EXPECT_EQ(resource.use_count(), 1); // Only our local copy remains
}

// Test concurrent access to rpc::member_ptr
TEST_F(MemberPtrTest, RpcConcurrentAccess)
{
    rpc::member_ptr<test_impl> ptr(rpc::make_shared<test_impl>(42));
    std::atomic<int> success_count{0};
    const int num_threads = 10;
    const int operations_per_thread = 100;

    std::vector<std::thread> threads;

    // Launch reader threads
    for (int i = 0; i < num_threads; ++i)
    {
        threads.emplace_back(
            [&ptr, &success_count, operations_per_thread]()
            {
                for (int j = 0; j < operations_per_thread; ++j)
                {
                    auto retrieved = ptr.get_nullable();
                    if (retrieved)
                    {
                        int val = 0;
                        auto result = retrieved->get_value(val);
                        EXPECT_EQ(result, rpc::error::OK());
                        if (val == 42)
                        {
                            success_count++;
                        }
                    }
                }
            });
    }

    // Wait for all threads to complete
    for (auto& t : threads)
    {
        t.join();
    }

    EXPECT_EQ(success_count.load(), num_threads * operations_per_thread);
}

// Test concurrent access with resets for rpc::member_ptr
TEST_F(MemberPtrTest, RpcConcurrentAccessWithReset)
{
    rpc::member_ptr<test_impl> ptr(rpc::make_shared<test_impl>(42));
    std::atomic<int> read_success_count{0};
    std::atomic<bool> should_reset{false};
    const int num_reader_threads = 5;
    const int num_resetter_threads = 2;
    const int operations_per_thread = 50;

    std::vector<std::thread> threads;

    // Launch reader threads
    for (int i = 0; i < num_reader_threads; ++i)
    {
        threads.emplace_back(
            [&ptr, &read_success_count, operations_per_thread, &should_reset]()
            {
                for (int j = 0; j < operations_per_thread; ++j)
                {
                    auto retrieved = ptr.get_nullable();
                    if (retrieved)
                    {
                        int val = 0;
                        auto result = retrieved->get_value(val);
                        EXPECT_EQ(result, rpc::error::OK());
                        if (val == 42)
                        {
                            read_success_count++;
                        }
                    }

                    // Small delay to allow other threads to operate
                    std::this_thread::sleep_for(std::chrono::microseconds(1));
                }
            });
    }

    // Launch resetter threads
    for (int i = 0; i < num_resetter_threads; ++i)
    {
        threads.emplace_back(
            [&ptr, &should_reset, operations_per_thread]()
            {
                for (int j = 0; j < operations_per_thread; ++j)
                {
                    ptr.reset();
                    // Small delay to allow other threads to operate
                    std::this_thread::sleep_for(std::chrono::microseconds(2));
                    // Reassign a new value occasionally
                    if (j % 10 == 0)
                    {
                        ptr = rpc::make_shared<test_impl>(42);
                    }
                }
            });
    }

    // Wait for all threads to complete
    for (auto& t : threads)
    {
        t.join();
    }

    // Just verify no crashes occurred - exact counts may vary due to timing
    SUCCEED() << "Concurrent access with reset completed without crashes";
}

// Test assignment during concurrent access for rpc::member_ptr
TEST_F(MemberPtrTest, RpcConcurrentAssignment)
{
    rpc::member_ptr<test_impl> ptr(rpc::make_shared<test_impl>(42));
    std::atomic<int> success_count{0};
    const int num_threads = 8;
    const int operations_per_thread = 50;

    std::vector<std::thread> threads;

    // Launch mixed reader/writer threads
    for (int i = 0; i < num_threads; ++i)
    {
        threads.emplace_back(
            [&ptr, &success_count, operations_per_thread, i]()
            {
                for (int j = 0; j < operations_per_thread; ++j)
                {
                    if (j % 3 == 0)
                    {
                        // Assignment operation
                        ptr = rpc::make_shared<test_impl>(42 + i);
                    }
                    else
                    {
                        // Read operation
                        auto retrieved = ptr.get_nullable();
                        if (retrieved)
                        {
                            success_count++;
                        }
                    }
                }
            });
    }

    // Wait for all threads to complete
    for (auto& t : threads)
    {
        t.join();
    }

    // Just verify no crashes occurred
    SUCCEED() << "Concurrent assignment completed without crashes";
}

// Mock class for testing - inherits from casting_interface for rpc::shared_ptr compatibility
struct TestResource
{
    int value;

    TestResource(int v = 0)
        : value(v)
    {
        construction_count++;
    }
    ~TestResource() { destruction_count++; }

    // Disable copy and move to ensure shared_ptr handles ownership correctly
    TestResource(const TestResource&) = delete;
    TestResource(TestResource&&) = delete;
    TestResource& operator=(const TestResource&) = delete;
    TestResource& operator=(TestResource&&) = delete;
};

// Test default constructor for stdex::member_ptr
TEST_F(MemberPtrTest, StdexDefaultConstructor)
{
    stdex::member_ptr<TestResource> ptr;
    auto retrieved = ptr.get_nullable();
    EXPECT_EQ(retrieved, nullptr);
}

// Test constructor from shared_ptr for stdex::member_ptr
TEST_F(MemberPtrTest, StdexConstructFromSharedPtr)
{
    auto resource = std::make_shared<TestResource>(42);
    stdex::member_ptr<TestResource> ptr(resource);

    auto retrieved = ptr.get_nullable();
    ASSERT_NE(retrieved, nullptr);
    EXPECT_EQ(retrieved->value, 42);
    EXPECT_EQ(resource.use_count(), 3); // Original + copy in member_ptr
}

// Test move constructor from shared_ptr for stdex::member_ptr
TEST_F(MemberPtrTest, StdexConstructFromMovedSharedPtr)
{
    auto resource = std::make_shared<TestResource>(42);
    stdex::member_ptr<TestResource> ptr(std::move(resource));

    auto retrieved = ptr.get_nullable();
    ASSERT_NE(retrieved, nullptr);
    EXPECT_EQ(retrieved->value, 42);
}

// Test copy constructor for stdex::member_ptr
TEST_F(MemberPtrTest, StdexCopyConstructor)
{
    auto resource = std::make_shared<TestResource>(42);
    stdex::member_ptr<TestResource> original(resource);

    stdex::member_ptr<TestResource> copy(original);
    auto original_retrieved = original.get_nullable();
    auto copy_retrieved = copy.get_nullable();

    ASSERT_NE(original_retrieved, nullptr);
    ASSERT_NE(copy_retrieved, nullptr);
    EXPECT_EQ(original_retrieved->value, 42);
    EXPECT_EQ(copy_retrieved->value, 42);
    EXPECT_EQ(original_retrieved, copy_retrieved); // Same underlying object
}

// Test move constructor for stdex::member_ptr
TEST_F(MemberPtrTest, StdexMoveConstructor)
{
    auto resource = std::make_shared<TestResource>(42);
    stdex::member_ptr<TestResource> original(resource);

    stdex::member_ptr<TestResource> moved(std::move(original));
    auto original_retrieved = original.get_nullable();
    auto moved_retrieved = moved.get_nullable();

    ASSERT_NE(moved_retrieved, nullptr);
    EXPECT_EQ(moved_retrieved->value, 42);
    // Note: After move, original may have null value, but this depends on implementation
}

// Test copy assignment for stdex::member_ptr
TEST_F(MemberPtrTest, StdexCopyAssignment)
{
    auto resource1 = std::make_shared<TestResource>(42);
    auto resource2 = std::make_shared<TestResource>(84);

    stdex::member_ptr<TestResource> ptr1(resource1);
    stdex::member_ptr<TestResource> ptr2(resource2);

    ptr1 = ptr2; // Copy assignment

    auto retrieved1 = ptr1.get_nullable();
    auto retrieved2 = ptr2.get_nullable();

    ASSERT_NE(retrieved1, nullptr);
    ASSERT_NE(retrieved2, nullptr);
    EXPECT_EQ(retrieved1->value, 84);
    EXPECT_EQ(retrieved2->value, 84);
    EXPECT_EQ(retrieved1, retrieved2); // Same underlying object
}

// Test move assignment for stdex::member_ptr
TEST_F(MemberPtrTest, StdexMoveAssignment)
{
    auto resource1 = std::make_shared<TestResource>(42);
    auto resource2 = std::make_shared<TestResource>(84);

    stdex::member_ptr<TestResource> ptr1(resource1);
    stdex::member_ptr<TestResource> ptr2(resource2);

    ptr1 = std::move(ptr2); // Move assignment

    auto retrieved1 = ptr1.get_nullable();
    auto retrieved2 = ptr2.get_nullable();

    ASSERT_NE(retrieved1, nullptr);
    EXPECT_EQ(retrieved1->value, 84);
    // After move, ptr2 may contain null
}

// Test assignment from shared_ptr for stdex::member_ptr
TEST_F(MemberPtrTest, StdexAssignSharedPtr)
{
    auto resource = std::make_shared<TestResource>(42);
    stdex::member_ptr<TestResource> ptr;

    ptr = resource;

    auto retrieved = ptr.get_nullable();
    ASSERT_NE(retrieved, nullptr);
    EXPECT_EQ(retrieved->value, 42);
}

// Test move assignment from shared_ptr for stdex::member_ptr
TEST_F(MemberPtrTest, StdexAssignMovedSharedPtr)
{
    auto resource = std::make_shared<TestResource>(42);
    stdex::member_ptr<TestResource> ptr;

    ptr = std::move(resource);

    auto retrieved = ptr.get_nullable();
    ASSERT_NE(retrieved, nullptr);
    EXPECT_EQ(retrieved->value, 42);
}

// Test get_nullable for stdex::member_ptr
TEST_F(MemberPtrTest, StdexGetNullable)
{
    auto resource = std::make_shared<TestResource>(42);
    stdex::member_ptr<TestResource> ptr(resource);

    auto retrieved = ptr.get_nullable();
    ASSERT_NE(retrieved, nullptr);
    EXPECT_EQ(retrieved->value, 42);

    // Test multiple calls return equivalent results
    auto retrieved2 = ptr.get_nullable();
    EXPECT_EQ(retrieved, retrieved2);
}

// Test reset for stdex::member_ptr
TEST_F(MemberPtrTest, StdexReset)
{
    auto resource = std::make_shared<TestResource>(42);
    stdex::member_ptr<TestResource> ptr(resource);

    ptr.reset();

    auto retrieved = ptr.get_nullable();
    EXPECT_EQ(retrieved, nullptr);
}

// Test destructor behavior for stdex::member_ptr
TEST_F(MemberPtrTest, StdexDestructor)
{
    std::shared_ptr<TestResource> resource;
    {
        resource = std::make_shared<TestResource>(42);
        stdex::member_ptr<TestResource> ptr(resource);
        auto retrieved = ptr.get_nullable();
        ASSERT_NE(retrieved, nullptr);
        EXPECT_EQ(retrieved->value, 42);
        // ptr goes out of scope here
    }
    // At this point, the member_ptr is destroyed but resource should still be valid
    EXPECT_EQ(resource->value, 42);
    EXPECT_EQ(resource.use_count(), 1); // Only our local copy remains
}

// Test concurrent access to stdex::member_ptr
TEST_F(MemberPtrTest, StdexConcurrentAccess)
{
    stdex::member_ptr<TestResource> ptr(std::make_shared<TestResource>(42));
    std::atomic<int> success_count{0};
    const int num_threads = 10;
    const int operations_per_thread = 100;

    std::vector<std::thread> threads;

    // Launch reader threads
    for (int i = 0; i < num_threads; ++i)
    {
        threads.emplace_back(
            [&ptr, &success_count, operations_per_thread]()
            {
                for (int j = 0; j < operations_per_thread; ++j)
                {
                    auto retrieved = ptr.get_nullable();
                    if (retrieved && retrieved->value == 42)
                    {
                        success_count++;
                    }
                }
            });
    }

    // Wait for all threads to complete
    for (auto& t : threads)
    {
        t.join();
    }

    EXPECT_EQ(success_count.load(), num_threads * operations_per_thread);
}

// Test concurrent access with resets for stdex::member_ptr
TEST_F(MemberPtrTest, StdexConcurrentAccessWithReset)
{
    stdex::member_ptr<TestResource> ptr(std::make_shared<TestResource>(42));
    std::atomic<int> read_success_count{0};
    std::atomic<bool> should_reset{false};
    const int num_reader_threads = 5;
    const int num_resetter_threads = 2;
    const int operations_per_thread = 50;

    std::vector<std::thread> threads;

    // Launch reader threads
    for (int i = 0; i < num_reader_threads; ++i)
    {
        threads.emplace_back(
            [&ptr, &read_success_count, operations_per_thread, &should_reset]()
            {
                for (int j = 0; j < operations_per_thread; ++j)
                {
                    auto retrieved = ptr.get_nullable();
                    if (retrieved && retrieved->value == 42)
                    {
                        read_success_count++;
                    }

                    // Small delay to allow other threads to operate
                    std::this_thread::sleep_for(std::chrono::microseconds(1));
                }
            });
    }

    // Launch resetter threads
    for (int i = 0; i < num_resetter_threads; ++i)
    {
        threads.emplace_back(
            [&ptr, &should_reset, operations_per_thread]()
            {
                for (int j = 0; j < operations_per_thread; ++j)
                {
                    ptr.reset();
                    // Small delay to allow other threads to operate
                    std::this_thread::sleep_for(std::chrono::microseconds(2));
                    // Reassign a new value occasionally
                    if (j % 10 == 0)
                    {
                        ptr = std::make_shared<TestResource>(42);
                    }
                }
            });
    }

    // Wait for all threads to complete
    for (auto& t : threads)
    {
        t.join();
    }

    // Just verify no crashes occurred - exact counts may vary due to timing
    SUCCEED() << "Concurrent access with reset completed without crashes";
}

// Test assignment during concurrent access for stdex::member_ptr
TEST_F(MemberPtrTest, StdexConcurrentAssignment)
{
    stdex::member_ptr<TestResource> ptr(std::make_shared<TestResource>(42));
    std::atomic<int> success_count{0};
    const int num_threads = 8;
    const int operations_per_thread = 50;

    std::vector<std::thread> threads;

    // Launch mixed reader/writer threads
    for (int i = 0; i < num_threads; ++i)
    {
        threads.emplace_back(
            [&ptr, &success_count, operations_per_thread, i]()
            {
                for (int j = 0; j < operations_per_thread; ++j)
                {
                    if (j % 3 == 0)
                    {
                        // Assignment operation
                        ptr = std::make_shared<TestResource>(42 + i);
                    }
                    else
                    {
                        // Read operation
                        auto retrieved = ptr.get_nullable();
                        if (retrieved)
                        {
                            success_count++;
                        }
                    }
                }
            });
    }

    // Wait for all threads to complete
    for (auto& t : threads)
    {
        t.join();
    }

    // Just verify no crashes occurred
    SUCCEED() << "Concurrent assignment completed without crashes";
}

// Test self-assignment for stdex::member_ptr
TEST_F(MemberPtrTest, StdexSelfAssignment)
{
    stdex::member_ptr<TestResource> ptr(std::make_shared<TestResource>(42));

    // Self-assignment should be safe
    ptr = ptr;

    auto retrieved = ptr.get_nullable();
    ASSERT_NE(retrieved, nullptr);
    EXPECT_EQ(retrieved->value, 42);
}

// Test equality comparison between member_ptr instances
TEST_F(MemberPtrTest, StdexEqualityComparison)
{
    auto resource1 = std::make_shared<TestResource>(42);
    auto resource2 = std::make_shared<TestResource>(84);

    stdex::member_ptr<TestResource> ptr1(resource1);
    stdex::member_ptr<TestResource> ptr2(resource1); // Same resource
    stdex::member_ptr<TestResource> ptr3(resource2); // Different resource

    auto retrieved1 = ptr1.get_nullable();
    auto retrieved2 = ptr2.get_nullable();
    auto retrieved3 = ptr3.get_nullable();

    EXPECT_EQ(retrieved1, retrieved2); // Same resource
    EXPECT_NE(retrieved1, retrieved3); // Different resources
}
