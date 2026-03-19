/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

// Type-test instantiations for the libcoro_dynamic_library transport.
// Coroutine builds only.

#ifdef CANOPY_BUILD_COROUTINE

#include <rpc/rpc.h>
#include <common/tests.h>

#include "test_host.h"
#include "test_globals.h"
#include "type_test_fixture.h"
#include <transport/tests/libcoro_dynamic_library/setup.h>

using namespace marshalled_tests;

template<class T> using libcoro_dll_type_test = type_test<T>;

using libcoro_dll_implementations
    = ::testing::Types<libcoro_dll_transport_setup<false, false, false>,
        libcoro_dll_transport_setup<false, false, true>,
        libcoro_dll_transport_setup<false, true, false>,
        libcoro_dll_transport_setup<false, true, true>,
        libcoro_dll_transport_setup<true, false, false>,
        libcoro_dll_transport_setup<true, false, true>,
        libcoro_dll_transport_setup<true, true, false>,
        libcoro_dll_transport_setup<true, true, true>>;

TYPED_TEST_SUITE(libcoro_dll_type_test, libcoro_dll_implementations);

TYPED_TEST(libcoro_dll_type_test, initialisation_test) { }

TYPED_TEST(libcoro_dll_type_test, standard_tests)
{
    run_coro_test(*this, [](auto& lib) { return coro_standard_tests<TypeParam>(lib); });
}

#endif // CANOPY_BUILD_COROUTINE
