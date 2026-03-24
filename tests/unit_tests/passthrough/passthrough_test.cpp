/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>

#include <rpc/rpc.h>
#include <common/tests.h>
#include <transport/tests/mock_test/setup.h>

// Additional test macros for GT and LT comparisons
#ifdef CANOPY_BUILD_COROUTINE
#  define CORO_ASSERT_GT(x, y)                                                                                         \
      do                                                                                                               \
      {                                                                                                                \
          auto _coro_temp_x = (x);                                                                                     \
          auto _coro_temp_y = (y);                                                                                     \
          EXPECT_GT(_coro_temp_x, _coro_temp_y);                                                                       \
          if (_coro_temp_x <= _coro_temp_y)                                                                            \
          {                                                                                                            \
              CO_RETURN false;                                                                                         \
          }                                                                                                            \
      } while (0)
#  define CORO_ASSERT_LT(x, y)                                                                                         \
      do                                                                                                               \
      {                                                                                                                \
          auto _coro_temp_x = (x);                                                                                     \
          auto _coro_temp_y = (y);                                                                                     \
          EXPECT_LT(_coro_temp_x, _coro_temp_y);                                                                       \
          if (_coro_temp_x >= _coro_temp_y)                                                                            \
          {                                                                                                            \
              CO_RETURN false;                                                                                         \
          }                                                                                                            \
      } while (0)
#else
#  define CORO_ASSERT_GT(x, y)                                                                                         \
      do                                                                                                               \
      {                                                                                                                \
          auto _coro_temp_x = (x);                                                                                     \
          auto _coro_temp_y = (y);                                                                                     \
          EXPECT_GT(_coro_temp_x, _coro_temp_y);                                                                       \
          if (_coro_temp_x <= _coro_temp_y)                                                                            \
          {                                                                                                            \
              return false;                                                                                            \
          }                                                                                                            \
      } while (0)
#  define CORO_ASSERT_LT(x, y)                                                                                         \
      do                                                                                                               \
      {                                                                                                                \
          auto _coro_temp_x = (x);                                                                                     \
          auto _coro_temp_y = (y);                                                                                     \
          EXPECT_LT(_coro_temp_x, _coro_temp_y);                                                                       \
          if (_coro_temp_x >= _coro_temp_y)                                                                            \
          {                                                                                                            \
              return false;                                                                                            \
          }                                                                                                            \
      } while (0)
#endif

// Test fixture for passthrough tests
template<class T> class passthrough_test : public testing::Test
{
    T lib_;

public:
    T& get_lib() { return lib_; }
    const T& get_lib() const { return lib_; }

    void SetUp() override { this->lib_.set_up(); }
    void TearDown() override { this->lib_.tear_down(); }
};

// Test type list
using passthrough_test_implementations = ::testing::Types<passthrough_setup>;

TYPED_TEST_SUITE(passthrough_test, passthrough_test_implementations);

namespace
{
    rpc::zone_address make_local_zone_address(uint64_t subnet, uint64_t object_id = 0)
    {
        return rpc::zone_address(rpc::zone_address::construction_args(rpc::zone_address::version_3,
            rpc::zone_address::address_type::local,
            0,
            {},
            rpc::zone_address::default_subnet_size_bits,
            subnet,
            rpc::zone_address::default_object_id_size_bits,
            object_id,
            {}));
    }
} // namespace

// Helper function for running coro tests (simplified version of run_coro_test)
template<typename TestFixture, typename CoroFunc>
void run_passthrough_test(TestFixture& test_fixture, CoroFunc&& coro_function)
{
    auto& lib = test_fixture.get_lib();
#ifdef CANOPY_BUILD_COROUTINE
    bool is_ready = false;
    auto wrapper_function = [&]() -> CORO_TASK(bool)
    {
        auto result = CO_AWAIT coro_function(lib);
        is_ready = true;
        CO_RETURN result;
    };

    RPC_ASSERT(lib.get_scheduler()->spawn_detached(lib.check_for_error(wrapper_function())));

    while (!is_ready)
    {
        lib.get_scheduler()->process_events(std::chrono::milliseconds(1));
    }
#else
    coro_function(lib);
#endif
    ASSERT_EQ(lib.error_has_occurred(), false);
}

// ============================================================================
// HAPPY PATH TESTS
// ============================================================================

// Tests basic passthrough creation and teardown functionality.
// Verifies that the passthrough object can be created and has correct forward
// and reverse destination zones configured.
template<class T> CORO_TASK(bool) coro_create_and_destroy(T& lib)
{
    auto pt = lib.get_passthrough();
    CORO_ASSERT_NE(pt, nullptr);
    CORO_ASSERT_EQ(pt->get_forward_destination().get_subnet(), lib.get_forward_dest().get_subnet());
    CORO_ASSERT_EQ(pt->get_reverse_destination().get_subnet(), lib.get_reverse_dest().get_subnet());
    CO_RETURN true;
}

TYPED_TEST(passthrough_test, create_and_destroy)
{
    run_passthrough_test(*this, [](auto& lib) { return coro_create_and_destroy<TypeParam>(lib); });
}

// Tests message routing through passthrough from forward to reverse transport.
// Calls inbound_send on forward transport with message destined for reverse zone,
// expects successful routing through passthrough to reverse_transport.
template<class T> CORO_TASK(bool) coro_send_happy_path(T& lib)
{
    auto reverse_transport = lib.get_reverse_transport();

    std::vector<char> in_data = {1, 2, 3, 4};

    // Call inbound_send on reverse_transport: message from zone 100 going to zone 200
    auto result = CO_AWAIT reverse_transport->inbound_send(rpc::send_params{
        .protocol_version = rpc::get_version(),
        .encoding_type = rpc::encoding::yas_binary,
        .tag = 12345,
        .caller_zone_id = lib.get_forward_dest(),                       // caller = zone 100
        .remote_object_id = rpc::remote_object(lib.get_reverse_dest()), // destination = zone 200
        .interface_id = rpc::interface_ordinal{1},
        .method_id = rpc::method{1},
        .in_data = in_data,
        .in_back_channel = {},
    });

    CORO_ASSERT_EQ(result.error_code, rpc::error::OK());
    CORO_ASSERT_EQ(reverse_transport->get_send_count(), 1);
    CORO_ASSERT_EQ(reverse_transport->get_send_count(), 0); // reverse_transport didn't call send itself
    CO_RETURN true;
}

TYPED_TEST(passthrough_test, send_happy_path)
{
    run_passthrough_test(*this, [](auto& lib) { return coro_send_happy_path<TypeParam>(lib); });
}

// Tests direct routing to reverse destination zone.
// Calls inbound_send on forward transport with destination set to reverse zone,
// verifies message is routed correctly to reverse_transport.
template<class T> CORO_TASK(bool) coro_send_to_reverse_destination(T& lib)
{
    auto reverse_transport = lib.get_reverse_transport();

    std::vector<char> in_data = {1, 2, 3, 4};

    auto result = CO_AWAIT reverse_transport->inbound_send(rpc::send_params{
        .protocol_version = rpc::get_version(),
        .encoding_type = rpc::encoding::yas_binary,
        .tag = 12345,
        .caller_zone_id = lib.get_reverse_dest(),                       // caller = zone 200
        .remote_object_id = rpc::remote_object(lib.get_forward_dest()), // destination = zone 100
        .interface_id = rpc::interface_ordinal{1},
        .method_id = rpc::method{1},
        .in_data = in_data,
        .in_back_channel = {},
    });

    CORO_ASSERT_EQ(result.error_code, rpc::error::OK());
    CORO_ASSERT_EQ(lib.get_reverse_transport()->get_send_count(), 0);
    CORO_ASSERT_EQ(lib.get_reverse_transport()->get_send_count(), 1);
    CO_RETURN true;
}

TYPED_TEST(passthrough_test, send_to_reverse_destination)
{
    run_passthrough_test(*this, [](auto& lib) { return coro_send_to_reverse_destination<TypeParam>(lib); });
}

// Tests local post routing to service.
// Calls inbound_post with local destination (forward transport's zone),
// expects message to be handled locally by service without transport forwarding.
template<class T> CORO_TASK(bool) coro_post_happy_path(T& lib)
{
    auto reverse_transport = lib.get_reverse_transport();

    std::vector<char> in_data = {1, 2, 3, 4};

    CO_AWAIT reverse_transport->inbound_post(rpc::post_params{
        .protocol_version = rpc::get_version(),
        .encoding_type = rpc::encoding::yas_binary,
        .tag = 12345,
        .caller_zone_id = lib.get_reverse_dest(),                                 // caller = zone 200
        .remote_object_id = rpc::remote_object(lib.get_service()->get_zone_id()), // destination = zone 1 (local to service)
        .interface_id = rpc::interface_ordinal{1},
        .method_id = rpc::method{1},
        .in_data = in_data,
        .in_back_channel = {},
    });

    // When using inbound_post with local destination, it routes to service
    // Service handles post locally, doesn't increment transport's post_count
    CORO_ASSERT_EQ(lib.get_reverse_transport()->get_post_count(), 0);
    CORO_ASSERT_EQ(lib.get_reverse_transport()->get_post_count(), 0);
    CO_RETURN true;
}

TYPED_TEST(passthrough_test, post_happy_path)
{
    run_passthrough_test(*this, [](auto& lib) { return coro_post_happy_path<TypeParam>(lib); });
}

// Tests interface casting functionality through passthrough.
// Calls inbound_send with empty data span for try_cast operation,
// verifies the cast request is routed through passthrough to reverse transport.
template<class T> CORO_TASK(bool) coro_try_cast_happy_path(T& lib)
{
    auto reverse_transport = lib.get_reverse_transport();

    // Test casting to reverse destination through forward transport
    // This should route through passthrough to reverse transport
    auto result = CO_AWAIT reverse_transport->inbound_send(rpc::send_params{
        .protocol_version = rpc::get_version(),
        .encoding_type = rpc::encoding::yas_binary,
        .tag = 12345,
        .caller_zone_id = lib.get_reverse_dest(),                       // caller = zone 200
        .remote_object_id = rpc::remote_object(lib.get_forward_dest()), // destination = zone 100
        .interface_id = rpc::interface_ordinal{1},
        .method_id = rpc::method{1},
        .in_data = {}, // empty for try_cast
        .in_back_channel = {},
    });

    // For try_cast functionality test, verify that reverse transport received the call
    CORO_ASSERT_EQ(result.error_code, rpc::error::OK());
    CORO_ASSERT_EQ(lib.get_reverse_transport()->get_send_count(), 1);
    CORO_ASSERT_EQ(lib.get_reverse_transport()->get_send_count(), 0);
    CO_RETURN true;
}

TYPED_TEST(passthrough_test, try_cast_happy_path)
{
    run_passthrough_test(*this, [](auto& lib) { return coro_try_cast_happy_path<TypeParam>(lib); });
}

// Tests reference counting with normal add_ref functionality.
// Calls inbound_add_ref with normal options, expects successful increment
// of shared reference count on the passthrough object.
template<class T> CORO_TASK(bool) coro_add_ref_happy_path(T& lib)
{
    auto reverse_transport = lib.get_reverse_transport();
    auto pt = lib.get_passthrough();

    // Store initial reference counts to verify exact changes
    uint64_t initial_shared = pt->get_shared_count();
    uint64_t initial_optimistic = pt->get_optimistic_count();

    auto result = CO_AWAIT reverse_transport->inbound_add_ref(rpc::add_ref_params{
        .protocol_version = rpc::get_version(),
        .remote_object_id = rpc::remote_object(lib.get_reverse_dest()),
        .caller_zone_id = lib.get_forward_dest(),
        .requesting_zone_id = lib.get_reverse_dest(),
        .build_out_param_channel = rpc::add_ref_options::normal,
        .in_back_channel = {},
    });

    // Verify exact count changes: shared_count should increase by exactly 1, optimistic_count unchanged
    CORO_ASSERT_EQ(result.error_code, rpc::error::OK());
    CORO_ASSERT_EQ(lib.get_forward_transport()->get_add_ref_count(), 1);
    CORO_ASSERT_EQ(pt->get_shared_count(), initial_shared + 1);     // Exact increment by 1
    CORO_ASSERT_EQ(pt->get_optimistic_count(), initial_optimistic); // Unchanged
    CO_RETURN true;
}

TYPED_TEST(passthrough_test, add_ref_happy_path)
{
    run_passthrough_test(*this, [](auto& lib) { return coro_add_ref_happy_path<TypeParam>(lib); });
}

// Tests reference counting with optimistic add_ref functionality.
// Calls inbound_add_ref with optimistic options, expects successful increment
// of optimistic reference count on the passthrough object.
template<class T> CORO_TASK(bool) coro_add_ref_optimistic(T& lib)
{
    auto reverse_transport = lib.get_reverse_transport();
    auto pt = lib.get_passthrough();

    // Store initial reference counts to verify exact changes
    uint64_t initial_shared = pt->get_shared_count();
    uint64_t initial_optimistic = pt->get_optimistic_count();

    auto result = CO_AWAIT reverse_transport->inbound_add_ref(rpc::add_ref_params{
        .protocol_version = rpc::get_version(),
        .remote_object_id = rpc::remote_object(lib.get_reverse_dest()),
        .caller_zone_id = lib.get_forward_dest(),
        .requesting_zone_id = lib.get_reverse_dest(),
        .build_out_param_channel = rpc::add_ref_options::optimistic,
        .in_back_channel = {},
    });

    // Verify exact count changes: optimistic_count should increase by exactly 1, shared_count unchanged
    CORO_ASSERT_EQ(result.error_code, rpc::error::OK());
    CORO_ASSERT_EQ(lib.get_forward_transport()->get_add_ref_count(), 1);
    CORO_ASSERT_EQ(pt->get_optimistic_count(), initial_optimistic + 1); // Exact increment by 1
    CORO_ASSERT_EQ(pt->get_shared_count(), initial_shared);             // Unchanged
    CO_RETURN true;
}

TYPED_TEST(passthrough_test, add_ref_optimistic)
{
    run_passthrough_test(*this, [](auto& lib) { return coro_add_ref_optimistic<TypeParam>(lib); });
}

// Tests reference release functionality.
// First adds a reference, then calls inbound_release to decrement it,
// expects successful release and exact reference count changes.
template<class T> CORO_TASK(bool) coro_release_happy_path(T& lib)
{
    auto reverse_transport = lib.get_reverse_transport();
    auto pt = lib.get_passthrough();

    // Store initial reference counts to verify exact changes
    uint64_t initial_shared = pt->get_shared_count();
    uint64_t initial_optimistic = pt->get_optimistic_count();

    // First add a reference
    CO_AWAIT reverse_transport->inbound_add_ref(rpc::add_ref_params{
        .protocol_version = rpc::get_version(),
        .remote_object_id = rpc::remote_object(lib.get_reverse_dest()),
        .caller_zone_id = lib.get_forward_dest(),
        .requesting_zone_id = lib.get_reverse_dest(),
        .build_out_param_channel = rpc::add_ref_options::normal,
        .in_back_channel = {},
    });

    // Verify add_ref worked: shared_count should increase by exactly 1
    uint64_t after_add = pt->get_shared_count();
    CORO_ASSERT_EQ(after_add, initial_shared + 1); // Exact increment by 1

    // Now release it
    auto result = CO_AWAIT reverse_transport->inbound_release(rpc::release_params{
        .protocol_version = rpc::get_version(),
        .remote_object_id = rpc::remote_object(lib.get_forward_dest()),
        .caller_zone_id = lib.get_reverse_dest(),
        .options = rpc::release_options::normal,
        .in_back_channel = {},
    });

    // Verify balanced operation: release should decrease by exactly 1, returning to initial state
    CORO_ASSERT_EQ(result.error_code, rpc::error::OK());
    CORO_ASSERT_EQ(lib.get_forward_transport()->get_release_count(), 1);
    CORO_ASSERT_EQ(pt->get_shared_count(), initial_shared);         // Back to original count
    CORO_ASSERT_EQ(pt->get_optimistic_count(), initial_optimistic); // Unchanged
    CO_RETURN true;
}

TYPED_TEST(passthrough_test, release_happy_path)
{
    run_passthrough_test(*this, [](auto& lib) { return coro_release_happy_path<TypeParam>(lib); });
}

// ============================================================================
// UNHAPPY PATH TESTS
// ============================================================================

// Tests transport failure handling during send operations.
// Marks forward transport as down, then calls inbound_send,
// expects TRANSPORT_ERROR due to unavailable transport.
template<class T> CORO_TASK(bool) coro_send_with_forward_transport_down(T& lib)
{
    auto reverse_transport = lib.get_reverse_transport();

    reverse_transport->mark_as_down();

    auto result = CO_AWAIT reverse_transport->inbound_send(rpc::send_params{
        .protocol_version = rpc::get_version(),
        .encoding_type = rpc::encoding::yas_binary,
        .tag = 12345,
        .caller_zone_id = rpc::caller_zone{make_local_zone_address(1)},
        .remote_object_id = rpc::remote_object(lib.get_forward_dest()),
        .interface_id = rpc::interface_ordinal{1},
        .method_id = rpc::method{1},
        .in_data = {1, 2, 3, 4},
        .in_back_channel = {},
    });

    CORO_ASSERT_EQ(result.error_code, rpc::error::TRANSPORT_ERROR());
    CO_RETURN true;
}

TYPED_TEST(passthrough_test, send_with_forward_transport_down)
{
    run_passthrough_test(*this, [](auto& lib) { return coro_send_with_forward_transport_down<TypeParam>(lib); });
}

// Tests invalid destination error handling.
// Calls inbound_send with invalid destination zone (999),
// expects ZONE_NOT_FOUND error due to non-existent destination.
template<class T> CORO_TASK(bool) coro_send_with_invalid_destination(T& lib)
{
    auto reverse_transport = lib.get_reverse_transport();

    auto result = CO_AWAIT reverse_transport->inbound_send(rpc::send_params{
        .protocol_version = rpc::get_version(),
        .encoding_type = rpc::encoding::yas_binary,
        .tag = 12345,
        .caller_zone_id = rpc::caller_zone{make_local_zone_address(1)},
        .remote_object_id = rpc::remote_object(rpc::destination_zone{make_local_zone_address(999)}), // Invalid destination
        .interface_id = rpc::interface_ordinal{1},
        .method_id = rpc::method{1},
        .in_data = {1, 2, 3, 4},
        .in_back_channel = {},
    });

    CORO_ASSERT_EQ(result.error_code, rpc::error::ZONE_NOT_FOUND());
    CO_RETURN true;
}

TYPED_TEST(passthrough_test, send_with_invalid_destination)
{
    run_passthrough_test(*this, [](auto& lib) { return coro_send_with_invalid_destination<TypeParam>(lib); });
}

// Tests add_ref functionality with transport down.
// Marks forward transport as down, then calls inbound_add_ref,
// expects TRANSPORT_ERROR and unchanged reference counts.
template<class T> CORO_TASK(bool) coro_add_ref_with_transport_down(T& lib)
{
    auto reverse_transport = lib.get_reverse_transport();
    auto pt = lib.get_passthrough();

    // Store initial reference counts to verify they remain unchanged
    uint64_t initial_shared = pt->get_shared_count();
    uint64_t initial_optimistic = pt->get_optimistic_count();

    reverse_transport->mark_as_down();

    auto result = CO_AWAIT reverse_transport->inbound_add_ref(rpc::add_ref_params{
        .protocol_version = rpc::get_version(),
        .remote_object_id = rpc::remote_object(lib.get_reverse_dest()),
        .caller_zone_id = lib.get_forward_dest(),
        .requesting_zone_id = lib.get_reverse_dest(),
        .build_out_param_channel = rpc::add_ref_options::normal,
        .in_back_channel = {},
    });

    // Verify operation failed AND counts unchanged due to transport being down
    CORO_ASSERT_EQ(result.error_code, rpc::error::TRANSPORT_ERROR());
    CORO_ASSERT_EQ(pt->get_shared_count(), initial_shared);         // Unchanged
    CORO_ASSERT_EQ(pt->get_optimistic_count(), initial_optimistic); // Unchanged
    CO_RETURN true;
}

TYPED_TEST(passthrough_test, add_ref_with_transport_down)
{
    run_passthrough_test(*this, [](auto& lib) { return coro_add_ref_with_transport_down<TypeParam>(lib); });
}

// Tests balanced add_ref/release operations returning to initial state.
// Verifies that paired reference counting operations are properly balanced.
template<class T> CORO_TASK(bool) coro_reference_count_balance(T& lib)
{
    auto reverse_transport = lib.get_reverse_transport();
    auto pt = lib.get_passthrough();

    // Store initial reference counts
    uint64_t initial_shared = pt->get_shared_count();
    uint64_t initial_optimistic = pt->get_optimistic_count();

    // Add normal reference
    CO_AWAIT reverse_transport->inbound_add_ref(rpc::add_ref_params{
        .protocol_version = rpc::get_version(),
        .remote_object_id = rpc::remote_object(lib.get_reverse_dest()),
        .caller_zone_id = lib.get_forward_dest(),
        .requesting_zone_id = lib.get_reverse_dest(),
        .build_out_param_channel = rpc::add_ref_options::normal,
        .in_back_channel = {},
    });

    // Add optimistic reference
    CO_AWAIT reverse_transport->inbound_add_ref(rpc::add_ref_params{
        .protocol_version = rpc::get_version(),
        .remote_object_id = rpc::remote_object(lib.get_reverse_dest()),
        .caller_zone_id = lib.get_forward_dest(),
        .requesting_zone_id = lib.get_reverse_dest(),
        .build_out_param_channel = rpc::add_ref_options::optimistic,
        .in_back_channel = {},
    });

    // Verify both increments
    uint64_t after_add_refs = pt->get_shared_count();
    CORO_ASSERT_EQ(after_add_refs, initial_shared + 2); // Two normal refs added
    CORO_ASSERT_EQ(pt->get_optimistic_count(), 1);      // One optimistic ref added

    // Release both references
    CO_AWAIT reverse_transport->inbound_release(rpc::release_params{
        .protocol_version = rpc::get_version(),
        .remote_object_id = rpc::remote_object(lib.get_forward_dest()),
        .caller_zone_id = rpc::caller_zone{make_local_zone_address(1)},
        .options = rpc::release_options::normal,
        .in_back_channel = {},
    });

    CO_AWAIT reverse_transport->inbound_release(rpc::release_params{
        .protocol_version = rpc::get_version(),
        .remote_object_id = rpc::remote_object(lib.get_forward_dest()),
        .caller_zone_id = rpc::caller_zone{make_local_zone_address(1)},
        .options = rpc::release_options::normal,
        .in_back_channel = {},
    });

    // Verify everything returned to initial state (balanced operations)
    uint64_t final_shared = pt->get_shared_count();
    uint64_t final_optimistic = pt->get_optimistic_count();

    CORO_ASSERT_EQ(final_shared, initial_shared); // Back to start
    CORO_ASSERT_EQ(final_optimistic, 0);          // Back to start
    CO_RETURN true;
}

TYPED_TEST(passthrough_test, reference_count_balance)
{
    run_passthrough_test(*this, [](auto& lib) { return coro_reference_count_balance<TypeParam>(lib); });
}

// Tests cleanup verification at test completion.
// Verifies that reference counts return to expected values after test operations.
template<class T> CORO_TASK(bool) coro_cleanup_verification(T& lib)
{
    auto reverse_transport = lib.get_reverse_transport();
    auto pt = lib.get_passthrough();

    // Store initial reference counts
    uint64_t initial_shared = pt->get_shared_count();
    uint64_t initial_optimistic = pt->get_optimistic_count();

    // Perform balanced add_ref/release cycle
    CO_AWAIT reverse_transport->inbound_add_ref(rpc::add_ref_params{
        .protocol_version = rpc::get_version(),
        .remote_object_id = rpc::remote_object(lib.get_reverse_dest()),
        .caller_zone_id = lib.get_forward_dest(),
        .requesting_zone_id = lib.get_reverse_dest(),
        .build_out_param_channel = rpc::add_ref_options::normal,
        .in_back_channel = {},
    });

    CO_AWAIT reverse_transport->inbound_release(rpc::release_params{
        .protocol_version = rpc::get_version(),
        .remote_object_id = rpc::remote_object(lib.get_forward_dest()),
        .caller_zone_id = rpc::caller_zone{make_local_zone_address(1)},
        .options = rpc::release_options::normal,
        .in_back_channel = {},
    });

    // Verify balanced operation returned to initial state
    uint64_t final_shared = pt->get_shared_count();
    uint64_t final_optimistic = pt->get_optimistic_count();

    // Note: May not be 0 due to test fixture holding reference,
    // but should return to initial state from start of this test
    CORO_ASSERT_EQ(final_shared, initial_shared);         // Back to start
    CORO_ASSERT_EQ(final_optimistic, initial_optimistic); // Back to start
    CO_RETURN true;
}

TYPED_TEST(passthrough_test, cleanup_verification)
{
    run_passthrough_test(*this, [](auto& lib) { return coro_cleanup_verification<TypeParam>(lib); });
}

// Note: Multithreaded and stress tests are more complex and would need additional
// infrastructure to work properly in the coroutine test framework. They can be
// implemented as separate non-typed tests or with additional helper utilities.
