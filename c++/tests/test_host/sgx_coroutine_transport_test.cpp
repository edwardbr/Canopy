/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#ifdef CANOPY_BUILD_ENCLAVE
#  include <rpc/rpc.h>
#  include <common/tests.h>
#  include "gtest/gtest.h"
#  include "type_test_fixture.h"
#  include <transport/tests/sgx_coroutine/setup.h>

template<class T> class sgx_coroutine_transport_test : public type_test<T>
{
};

using sgx_coroutine_transport_test_implementations
    = ::testing::Types<sgx_coroutine_setup<false, false, false>, sgx_coroutine_setup<true, false, false>>;

TYPED_TEST_SUITE(
    sgx_coroutine_transport_test,
    sgx_coroutine_transport_test_implementations);

TYPED_TEST(
    sgx_coroutine_transport_test,
    remote_standard_tests)
{
    run_coro_test(*this, [](auto& lib) { return marshalled_tests::coro_remote_standard_tests(lib); });
}

#endif
