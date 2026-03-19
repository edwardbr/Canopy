/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

// Type-test instantiations for the dynamic_library transport.
// Only compiled in non-coroutine builds (the transport is non-coro only).

#ifndef CANOPY_BUILD_COROUTINE

#include <rpc/rpc.h>
#include <common/tests.h>

#include "test_host.h"
#include "test_globals.h"
#include "type_test_fixture.h"
#include <transport/tests/dynamic_library/setup.h>

using namespace marshalled_tests;

template<class T> using dll_type_test = type_test<T>;

using dll_implementations = ::testing::Types<dll_transport_setup<false, false, false>,
    dll_transport_setup<false, false, true>,
    dll_transport_setup<false, true, false>,
    dll_transport_setup<false, true, true>,
    dll_transport_setup<true, false, false>,
    dll_transport_setup<true, false, true>,
    dll_transport_setup<true, true, false>,
    dll_transport_setup<true, true, true>>;

TYPED_TEST_SUITE(dll_type_test, dll_implementations);

TYPED_TEST(dll_type_test, initialisation_test) { }

TYPED_TEST(dll_type_test, standard_tests)
{
    run_coro_test(*this, [](auto& lib) { return coro_standard_tests<TypeParam>(lib); });
}

#endif // !CANOPY_BUILD_COROUTINE
