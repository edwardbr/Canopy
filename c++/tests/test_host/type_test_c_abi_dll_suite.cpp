/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#ifndef CANOPY_BUILD_COROUTINE

#  include <common/tests.h>
#  include <rpc/rpc.h>

#  include "test_globals.h"
#  include "test_host.h"
#  include "type_test_fixture.h"
#  include <transport/tests/c_abi/setup.h>

using namespace marshalled_tests;

template<class T> using c_abi_dll_type_test = type_test<T>;

using c_abi_dll_implementations = ::testing::Types<
    c_abi_dll_transport_setup<false, false, false>,
    c_abi_dll_transport_setup<false, false, true>,
    c_abi_dll_transport_setup<false, true, false>,
    c_abi_dll_transport_setup<false, true, true>,
    c_abi_dll_transport_setup<true, false, false>,
    c_abi_dll_transport_setup<true, false, true>,
    c_abi_dll_transport_setup<true, true, false>,
    c_abi_dll_transport_setup<true, true, true>>;

TYPED_TEST_SUITE(
    c_abi_dll_type_test,
    c_abi_dll_implementations);

TYPED_TEST(
    c_abi_dll_type_test,
    initialisation_test)
{
}

TYPED_TEST(
    c_abi_dll_type_test,
    standard_tests)
{
    run_coro_test(*this, [](auto& lib) { return coro_standard_tests<TypeParam>(lib); });
}

#endif // !CANOPY_BUILD_COROUTINE
