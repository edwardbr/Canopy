/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#ifdef CANOPY_BUILD_COROUTINE

#  include <common/tests.h>
#  include <rpc/rpc.h>

#  include "type_test_fixture.h"
#  include <transport/tests/libcoro_ipc_dynamic_dll/setup.h>

using namespace marshalled_tests;

template<class T> using libcoro_ipc_dll_type_test = type_test<T>;

using libcoro_ipc_dll_implementations = ::testing::Types<libcoro_ipc_dll_transport_setup<false, false, false>,
    libcoro_ipc_dll_transport_setup<false, true, false>,
    libcoro_ipc_dll_transport_setup<true, false, false>,
    libcoro_ipc_dll_transport_setup<true, true, false>>;

using libcoro_ipc_dll_isolated_implementations
    = ::testing::Types<libcoro_ipc_dll_isolated_transport_setup<false, false, false>,
        libcoro_ipc_dll_isolated_transport_setup<false, true, false>,
        libcoro_ipc_dll_isolated_transport_setup<true, false, false>,
        libcoro_ipc_dll_isolated_transport_setup<true, true, false>>;

using libcoro_ipc_dll_dual_isolated_implementations
    = ::testing::Types<libcoro_ipc_dll_dual_isolated_transport_setup<false, false, false>,
        libcoro_ipc_dll_dual_isolated_transport_setup<false, true, false>,
        libcoro_ipc_dll_dual_isolated_transport_setup<true, false, false>,
        libcoro_ipc_dll_dual_isolated_transport_setup<true, true, false>>;

TYPED_TEST_SUITE(libcoro_ipc_dll_type_test, libcoro_ipc_dll_implementations);

TYPED_TEST(libcoro_ipc_dll_type_test, initialisation_test) { }

TYPED_TEST(libcoro_ipc_dll_type_test, standard_tests)
{
    run_coro_test(*this, [](auto& lib) { return coro_standard_tests<TypeParam>(lib); });
}

template<class T> using libcoro_ipc_dll_isolated_type_test = type_test<T>;

TYPED_TEST_SUITE(libcoro_ipc_dll_isolated_type_test, libcoro_ipc_dll_isolated_implementations);

TYPED_TEST(libcoro_ipc_dll_isolated_type_test, initialisation_test) { }

TYPED_TEST(libcoro_ipc_dll_isolated_type_test, standard_tests)
{
    run_coro_test(*this, [](auto& lib) { return coro_standard_tests<TypeParam>(lib); });
}

template<class T> using libcoro_ipc_dll_dual_isolated_type_test = type_test<T>;

TYPED_TEST_SUITE(libcoro_ipc_dll_dual_isolated_type_test, libcoro_ipc_dll_dual_isolated_implementations);

template<class T> CORO_TASK(bool) coro_call_baz_from_foo(T& lib)
{
    auto example_a = lib.get_example();
    rpc::shared_ptr<xxx::i_baz> baz;
    CORO_ASSERT_EQ(CO_AWAIT example_a->create_baz(baz), rpc::error::OK());
    CORO_ASSERT_NE(baz, nullptr);

    auto example_b = lib.get_peer_example();
    rpc::shared_ptr<xxx::i_foo> foo;
    CORO_ASSERT_EQ(CO_AWAIT example_b->create_foo(foo), rpc::error::OK());
    CORO_ASSERT_NE(foo, nullptr);

    CORO_ASSERT_EQ(CO_AWAIT foo->call_baz_interface(baz), rpc::error::OK());

    CO_RETURN true;
}

TYPED_TEST(libcoro_ipc_dll_dual_isolated_type_test, dll_to_dll_call_baz_from_foo)
{
    run_coro_test(*this, [](auto& lib) { return coro_call_baz_from_foo<TypeParam>(lib); });
}

#endif // CANOPY_BUILD_COROUTINE
