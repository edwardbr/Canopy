/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

// Type-test instantiations for the blocking_dll transport.
// Only compiled in non-coroutine builds (the transport is non-coro only).

#ifndef CANOPY_BUILD_COROUTINE

#  include <rpc/rpc.h>
#  include <common/tests.h>

#  include "test_host.h"
#  include "test_globals.h"
#  include "type_test_fixture.h"
#  include <transport/tests/blocking_dll/setup.h>

using namespace marshalled_tests;

template<class T> using blocking_dll_type_test = type_test<T>;

using blocking_dll_implementations = ::testing::Types<
    blocking_dll_transport_setup<false, false, false>,
    blocking_dll_transport_setup<false, false, true>,
    blocking_dll_transport_setup<false, true, false>,
    blocking_dll_transport_setup<false, true, true>,
    blocking_dll_transport_setup<true, false, false>,
    blocking_dll_transport_setup<true, false, true>,
    blocking_dll_transport_setup<true, true, false>,
    blocking_dll_transport_setup<true, true, true>>;

TYPED_TEST_SUITE(
    blocking_dll_type_test,
    blocking_dll_implementations);

TYPED_TEST(
    blocking_dll_type_test,
    initialisation_test)
{
}

TYPED_TEST(
    blocking_dll_type_test,
    standard_tests)
{
    run_coro_test(*this, [](auto& lib) { return coro_standard_tests<TypeParam>(lib); });
}

#endif // !CANOPY_BUILD_COROUTINE
