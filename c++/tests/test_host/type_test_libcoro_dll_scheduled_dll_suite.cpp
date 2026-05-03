/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

// Type-test instantiations for the libcoro_dll_scheduled_dynamic_library transport.
// Coroutine builds only.

#ifdef CANOPY_BUILD_COROUTINE

#  include <rpc/rpc.h>
#  include <common/tests.h>

#  include "test_host.h"
#  include "test_globals.h"
#  include "type_test_fixture.h"
#  include <transport/tests/libcoro_dll_scheduled_dynamic_library/setup.h>

#  if defined(_WIN32)
#    include <windows.h>
#  else
#    include <dlfcn.h>
#  endif

using namespace marshalled_tests;

template<class T> using libcoro_dll_scheduled_dll_type_test = type_test<T>;

using libcoro_dll_scheduled_dll_implementations = ::testing::Types<
    libcoro_dll_scheduled_transport_setup<false, false, false>,
    libcoro_dll_scheduled_transport_setup<false, false, true>,
    libcoro_dll_scheduled_transport_setup<false, true, false>,
    libcoro_dll_scheduled_transport_setup<false, true, true>,
    libcoro_dll_scheduled_transport_setup<true, false, false>,
    libcoro_dll_scheduled_transport_setup<true, false, true>,
    libcoro_dll_scheduled_transport_setup<true, true, false>,
    libcoro_dll_scheduled_transport_setup<true, true, true>>;

TYPED_TEST_SUITE(
    libcoro_dll_scheduled_dll_type_test,
    libcoro_dll_scheduled_dll_implementations);

TYPED_TEST(
    libcoro_dll_scheduled_dll_type_test,
    initialisation_test)
{
}

TYPED_TEST(
    libcoro_dll_scheduled_dll_type_test,
    standard_tests)
{
    run_coro_test(*this, [](auto& lib) { return coro_standard_tests<TypeParam>(lib); });
}

namespace
{
    using static_probe_fn = int (*)();

    int increment_dll_static_probe()
    {
#  if defined(_WIN32)
        auto handle = LoadLibraryA(CANOPY_TEST_LIBCORO_DLL_SCHEDULED_DLL_PATH);
        EXPECT_NE(handle, nullptr);
        if (!handle)
            return -1;

        auto* symbol = GetProcAddress(handle, "canopy_libcoro_dll_scheduled_dll_test_increment_static_probe");
        EXPECT_NE(symbol, nullptr);
        auto probe = reinterpret_cast<static_probe_fn>(symbol);
        auto result = probe ? probe() : -1;
        FreeLibrary(handle);
        return result;
#  else
        auto* handle = dlopen(CANOPY_TEST_LIBCORO_DLL_SCHEDULED_DLL_PATH, RTLD_NOW | RTLD_LOCAL);
        EXPECT_NE(handle, nullptr);
        if (!handle)
            return -1;

        auto probe = reinterpret_cast<static_probe_fn>(
            dlsym(handle, "canopy_libcoro_dll_scheduled_dll_test_increment_static_probe"));
        EXPECT_NE(probe, nullptr);
        auto result = probe ? probe() : -1;
        dlclose(handle);
        return result;
#  endif
    }
}

TEST(
    libcoro_dll_scheduled_dynamic_library_lifecycle,
    dll_static_state_resets_after_transport_unload)
{
    using setup_type = libcoro_dll_scheduled_transport_setup<false, false, false>;

    for (auto iteration = 0; iteration < 5; ++iteration)
    {
        setup_type setup;
        setup.set_up();
        setup.tear_down();

        EXPECT_EQ(increment_dll_static_probe(), 1) << "iteration " << iteration;
    }
}

#endif // CANOPY_BUILD_COROUTINE
