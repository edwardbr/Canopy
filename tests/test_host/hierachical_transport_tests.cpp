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
#include "crash_handler.h"
#include "type_test_fixture.h"

// This list should be kept sorted.
using testing::_;
using testing::Action;
using testing::ActionInterface;
using testing::Assign;
using testing::ByMove;
using testing::ByRef;
using testing::DoDefault;
using testing::get;
using testing::IgnoreResult;
using testing::Invoke;
using testing::InvokeWithoutArgs;
using testing::make_tuple;
using testing::MakePolymorphicAction;
using testing::Ne;
using testing::PolymorphicAction;
using testing::Return;
using testing::ReturnNull;
using testing::ReturnRef;
using testing::ReturnRefOfCopy;
using ::testing::Sequence;
using testing::SetArgPointee;
using testing::SetArgumentPointee;
using testing::tuple;
using testing::tuple_element;
using testing::Types;

using namespace marshalled_tests;

#include "test_globals.h"

extern bool enable_multithreaded_tests;

// Fixture and type list for host-aware tests.
template<class T> class hierachical_transport_tests : public type_test<T>
{
};

using hierachical_transport_tests_implementations = ::testing::Types<

    // in process marshalled tests
    inproc_setup<true, true>,
    inproc_setup<true, false>,
    inproc_setup<false, true>,
    inproc_setup<false, false>

#ifdef CANOPY_BUILD_ENCLAVE
    ,
    sgx_setup<true, false, false>,
    sgx_setup<true, false, true>,
    sgx_setup<true, true, false>,
    sgx_setup<true, true, true>
#endif
    >;

TYPED_TEST_SUITE(hierachical_transport_tests, hierachical_transport_tests_implementations);

#ifdef CANOPY_BUILD_ENCLAVE
template<class T> CORO_TASK(bool) coro_call_host_create_enclave_and_throw_away(T& lib)
{
    bool run_standard_tests = false;
    CORO_ASSERT_EQ(
        CO_AWAIT lib.get_example()->call_host_create_enclave_and_throw_away(run_standard_tests), rpc::error::OK());
    CO_RETURN true;
}

TYPED_TEST(hierachical_transport_tests, call_host_create_enclave_and_throw_away)
{
    run_coro_test(*this, [](auto& lib) { return coro_call_host_create_enclave_and_throw_away<TypeParam>(lib); });
}

template<class T> CORO_TASK(bool) coro_call_host_create_enclave(T& lib)
{
    bool run_standard_tests = false;
    rpc::shared_ptr<yyy::i_example> target;

    CORO_ASSERT_EQ(CO_AWAIT lib.get_example()->call_host_create_enclave(target, run_standard_tests), rpc::error::OK());
    CORO_ASSERT_NE(target, nullptr);
    CO_RETURN true;
}

TYPED_TEST(hierachical_transport_tests, call_host_create_enclave)
{
    run_coro_test(*this, [](auto& lib) { return coro_call_host_create_enclave<TypeParam>(lib); });
}
#endif

template<class T> CORO_TASK(bool) coro_look_up_app_and_return_with_nothing(T& lib)
{
    bool run_standard_tests = false;
    rpc::shared_ptr<yyy::i_example> target;

    CORO_ASSERT_EQ(
        CO_AWAIT lib.get_example()->call_host_look_up_app("target", target, run_standard_tests), rpc::error::OK());
    CORO_ASSERT_EQ(target, nullptr);
    CO_RETURN true;
}

TYPED_TEST(hierachical_transport_tests, look_up_app_and_return_with_nothing)
{
    run_coro_test(*this, [](auto& lib) { return coro_look_up_app_and_return_with_nothing<TypeParam>(lib); });
}

template<class T> CORO_TASK(bool) coro_call_host_unload_app_not_there(T& lib)
{
    CORO_ASSERT_EQ(CO_AWAIT lib.get_example()->call_host_unload_app("target"), rpc::error::OK());
    CO_RETURN true;
}

TYPED_TEST(hierachical_transport_tests, call_host_unload_app_not_there)
{
    run_coro_test(*this, [](auto& lib) { return coro_call_host_unload_app_not_there<TypeParam>(lib); });
}

#ifdef CANOPY_BUILD_ENCLAVE
template<class T> CORO_TASK(bool) coro_call_host_look_up_app_unload_app(T& lib)
{
    bool run_standard_tests = false;
    rpc::shared_ptr<yyy::i_example> target;

    CORO_ASSERT_EQ(CO_AWAIT lib.get_example()->call_host_create_enclave(target, run_standard_tests), rpc::error::OK());
    CORO_ASSERT_NE(target, nullptr);

    CORO_ASSERT_EQ(CO_AWAIT lib.get_example()->call_host_set_app("target", target, run_standard_tests), rpc::error::OK());
    CORO_ASSERT_EQ(CO_AWAIT lib.get_example()->call_host_unload_app("target"), rpc::error::OK());
    target = nullptr;
    CO_RETURN true;
}

TYPED_TEST(hierachical_transport_tests, call_host_look_up_app_unload_app)
{
    run_coro_test(*this, [](auto& lib) { return coro_call_host_look_up_app_unload_app<TypeParam>(lib); });
}

template<class T> CORO_TASK(bool) coro_call_host_look_up_app_not_return(T& lib)
{
    bool run_standard_tests = false;
    rpc::shared_ptr<yyy::i_example> target;

    CORO_ASSERT_EQ(CO_AWAIT lib.get_example()->call_host_create_enclave(target, run_standard_tests), rpc::error::OK());
    CORO_ASSERT_NE(target, nullptr);

    CORO_ASSERT_EQ(CO_AWAIT lib.get_example()->call_host_set_app("target", target, run_standard_tests), rpc::error::OK());
    CORO_ASSERT_EQ(
        CO_AWAIT lib.get_example()->call_host_look_up_app_not_return("target", run_standard_tests), rpc::error::OK());
    CORO_ASSERT_EQ(CO_AWAIT lib.get_example()->call_host_unload_app("target"), rpc::error::OK());
    target = nullptr;
    CO_RETURN true;
}

TYPED_TEST(hierachical_transport_tests, call_host_look_up_app_not_return)
{
    run_coro_test(*this, [](auto& lib) { return coro_call_host_look_up_app_not_return<TypeParam>(lib); });
}

template<class T> CORO_TASK(bool) coro_create_store_fetch_delete(T& lib)
{
    bool run_standard_tests = false;
    rpc::shared_ptr<yyy::i_example> target;
    rpc::shared_ptr<yyy::i_example> target2;

    CORO_ASSERT_EQ(CO_AWAIT lib.get_example()->call_host_create_enclave(target, run_standard_tests), rpc::error::OK());
    CORO_ASSERT_NE(target, nullptr);

    CORO_ASSERT_EQ(CO_AWAIT lib.get_example()->call_host_set_app("target", target, run_standard_tests), rpc::error::OK());
    CORO_ASSERT_EQ(
        CO_AWAIT lib.get_example()->call_host_look_up_app("target", target2, run_standard_tests), rpc::error::OK());
    CORO_ASSERT_EQ(CO_AWAIT lib.get_example()->call_host_unload_app("target"), rpc::error::OK());
    CORO_ASSERT_EQ(target, target2);
    target = nullptr;
    target2 = nullptr;
    CO_RETURN true;
}

TYPED_TEST(hierachical_transport_tests, create_store_fetch_delete)
{
    run_coro_test(*this, [](auto& lib) { return coro_create_store_fetch_delete<TypeParam>(lib); });
}

template<class T> CORO_TASK(bool) coro_create_store_not_return_delete(T& lib)
{
    bool run_standard_tests = false;
    rpc::shared_ptr<yyy::i_example> target;

    CORO_ASSERT_EQ(CO_AWAIT lib.get_example()->call_host_create_enclave(target, run_standard_tests), rpc::error::OK());
    CORO_ASSERT_NE(target, nullptr);

    CORO_ASSERT_EQ(CO_AWAIT lib.get_example()->call_host_set_app("target", target, run_standard_tests), rpc::error::OK());
    CORO_ASSERT_EQ(CO_AWAIT lib.get_example()->call_host_look_up_app_not_return_and_delete("target", run_standard_tests),
        rpc::error::OK());
    target = nullptr;
    CO_RETURN true;
}

TYPED_TEST(hierachical_transport_tests, create_store_not_return_delete)
{
    run_coro_test(*this, [](auto& lib) { return coro_create_store_not_return_delete<TypeParam>(lib); });
}

template<class T> CORO_TASK(bool) coro_create_store_delete(T& lib)
{
    bool run_standard_tests = false;
    rpc::shared_ptr<yyy::i_example> target;
    rpc::shared_ptr<yyy::i_example> target2;

    CORO_ASSERT_EQ(CO_AWAIT lib.get_example()->call_host_create_enclave(target, run_standard_tests), rpc::error::OK());
    CORO_ASSERT_NE(target, nullptr);

    CORO_ASSERT_EQ(CO_AWAIT lib.get_example()->call_host_set_app("target", target, run_standard_tests), rpc::error::OK());
    CORO_ASSERT_EQ(CO_AWAIT lib.get_example()->call_host_look_up_app_and_delete("target", target2, run_standard_tests),
        rpc::error::OK());
    CORO_ASSERT_EQ(target, target2);
    target = nullptr;
    target2 = nullptr;
    CO_RETURN true;
}

TYPED_TEST(hierachical_transport_tests, create_store_delete)
{
    run_coro_test(*this, [](auto& lib) { return coro_create_store_delete<TypeParam>(lib); });
}
#endif

template<class T> CORO_TASK(bool) coro_create_subordinate_zone(T& lib)
{
    rpc::shared_ptr<yyy::i_example> target;
    CORO_ASSERT_EQ(
        CO_AWAIT lib.get_example()->create_example_in_subordinate_zone(target, lib.get_local_host_ptr(), ++(*zone_gen)),
        rpc::error::OK());
    CO_RETURN true;
}

TYPED_TEST(hierachical_transport_tests, create_subordinate_zone) // TODO: Missing test suite definition
{
    run_coro_test(*this, [](auto& lib) { return coro_create_subordinate_zone<TypeParam>(lib); });
}

template<class T> CORO_TASK(bool) coro_create_subordinate_zone_and_set_in_host(T& lib)
{
    CORO_ASSERT_EQ(CO_AWAIT lib.get_example()->create_example_in_subordinate_zone_and_set_in_host(
                       ++(*zone_gen), "foo", lib.get_local_host_ptr()),
        rpc::error::OK());
    rpc::shared_ptr<yyy::i_example> target;
    CO_AWAIT lib.get_host()->look_up_app("foo", target);
    CO_AWAIT lib.get_host()->unload_app("foo");
    CO_AWAIT target->set_host(nullptr);
    CO_RETURN true;
}

TYPED_TEST(hierachical_transport_tests, create_subordinate_zone_and_set_in_host)
{
    run_coro_test(*this, [](auto& lib) { return coro_create_subordinate_zone_and_set_in_host<TypeParam>(lib); });
}
