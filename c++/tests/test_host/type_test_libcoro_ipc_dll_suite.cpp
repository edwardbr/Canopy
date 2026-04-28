/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#ifdef CANOPY_BUILD_COROUTINE

#  include <csignal>
#  include <sys/prctl.h>
#  include <sys/wait.h>
#  include <thread>

#  include <common/tests.h>
#  include <rpc/rpc.h>

#  include "type_test_fixture.h"
#  include <transport/tests/ipc_transport/setup.h>
#  include <transport/tests/libcoro_spsc_dynamic_dll/setup.h>

using namespace marshalled_tests;

template<class T> using libcoro_ipc_dll_type_test = type_test<T>;

using libcoro_ipc_dll_implementations = ::testing::Types<
    libcoro_spsc_dll_transport_setup<false, false, false>,
    libcoro_spsc_dll_transport_setup<false, true, false>,
    libcoro_spsc_dll_transport_setup<true, false, false>,
    libcoro_spsc_dll_transport_setup<true, true, false>>;

using libcoro_ipc_dll_isolated_implementations = ::testing::Types<
    ipc_child_host_process_setup<false, false, false>,
    ipc_child_host_process_setup<false, true, false>,
    ipc_child_host_process_setup<true, false, false>,
    ipc_child_host_process_setup<true, true, false>>;

// using libcoro_ipc_process_isolated_implementations = ::testing::Types<
//     ipc_child_process_setup<false, false, false>,
//     ipc_child_process_setup<false, true, false>,
//     ipc_child_process_setup<true, false, false>,
//     ipc_child_process_setup<true, true, false>>;

using libcoro_ipc_dll_dual_isolated_implementations = ::testing::Types<
    ipc_child_host_process_dual_setup<false, false, false>,
    ipc_child_host_process_dual_setup<false, true, false>,
    ipc_child_host_process_dual_setup<true, false, false>,
    ipc_child_host_process_dual_setup<true, true, false>>;

TYPED_TEST_SUITE(
    libcoro_ipc_dll_type_test,
    libcoro_ipc_dll_implementations);

TYPED_TEST(
    libcoro_ipc_dll_type_test,
    initialisation_test)
{
}

TYPED_TEST(
    libcoro_ipc_dll_type_test,
    standard_tests)
{
    run_coro_test(*this, [](auto& lib) { return coro_standard_tests<TypeParam>(lib); });
}

template<class T> using libcoro_ipc_dll_isolated_type_test = type_test<T>;

TYPED_TEST_SUITE(
    libcoro_ipc_dll_isolated_type_test,
    libcoro_ipc_dll_isolated_implementations);

TYPED_TEST(
    libcoro_ipc_dll_isolated_type_test,
    initialisation_test)
{
}

TYPED_TEST(
    libcoro_ipc_dll_isolated_type_test,
    standard_tests)
{
    run_coro_test(*this, [](auto& lib) { return coro_standard_tests<TypeParam>(lib); });
}

// template<class T> using libcoro_ipc_process_isolated_type_test = type_test<T>;

// TYPED_TEST_SUITE(
//     libcoro_ipc_process_isolated_type_test,
//     libcoro_ipc_process_isolated_implementations);

// TYPED_TEST(
//     libcoro_ipc_process_isolated_type_test,
//     initialisation_test)
// {
// }

// TYPED_TEST(
//     libcoro_ipc_process_isolated_type_test,
//     standard_tests)
// {
//     run_coro_test(*this, [](auto& lib) { return coro_standard_tests<TypeParam>(lib); });
// }

template<class T> using libcoro_ipc_dll_dual_isolated_type_test = type_test<T>;

TYPED_TEST_SUITE(
    libcoro_ipc_dll_dual_isolated_type_test,
    libcoro_ipc_dll_dual_isolated_implementations);

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

TYPED_TEST(
    libcoro_ipc_dll_dual_isolated_type_test,
    dll_to_dll_call_baz_from_foo)
{
    run_coro_test(*this, [](auto& lib) { return coro_call_baz_from_foo<TypeParam>(lib); });
}

#  if defined(__linux__)
namespace
{
    struct harness_result
    {
        pid_t harness_pid = -1;
        pid_t child_pid = -1;
    };

    harness_result spawn_pdeathsig_harness()
    {
        // The test process and harness process use this pipe to pass back the spawned IPC child pid.
        // Parent reads once; harness writes once after ipc_transport has fully created the child.
        int pipe_fds[2] = {-1, -1};
        EXPECT_EQ(::pipe(pipe_fds), 0);

        auto harness_pid = ::fork();
        EXPECT_GE(harness_pid, 0);

        if (harness_pid == 0)
        {
            // Harness only writes to the pipe, so it closes the read end immediately.
            ::close(pipe_fds[0]);

            auto scheduler = std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
                coro::scheduler::options{.thread_strategy = coro::scheduler::thread_strategy_t::manual,
                    .pool = coro::thread_pool::options{.thread_count = 1}}));
            auto service = rpc::root_service::create("pdeathsig_host", rpc::DEFAULT_PREFIX, scheduler);
            auto dll_zone = rpc::DEFAULT_PREFIX;
            [[maybe_unused]] auto ok = dll_zone.set_subnet(dll_zone.get_subnet() + 10);
            RPC_ASSERT(ok);

            auto transport = rpc::ipc_transport::make_client(
                "pdeathsig child",
                service,
                rpc::ipc_transport::options{
                    .process_executable = CANOPY_TEST_IPC_CHILD_HOST_PROCESS_PATH,
                    .dll_path = CANOPY_TEST_LIBCORO_SPSC_DLL_PATH,
                    .dll_zone = dll_zone,
                    .kill_child_on_parent_death = true,
                });

            auto child_pid = static_cast<pid_t>(transport->child_pid_for_test());
            // Report the spawned child pid to the real test process before waiting to be killed.
            auto bytes_written = ::write(pipe_fds[1], &child_pid, sizeof(child_pid));
            ::close(pipe_fds[1]);
            if (bytes_written != sizeof(child_pid))
                _exit(120);

            for (;;)
                ::pause();
        }

        // Test process only reads from the pipe, so it closes the write end immediately.
        ::close(pipe_fds[1]);
        pid_t child_pid = -1;
        auto bytes_read = ::read(pipe_fds[0], &child_pid, sizeof(child_pid));
        ::close(pipe_fds[0]);
        EXPECT_EQ(bytes_read, sizeof(child_pid));
        EXPECT_GT(child_pid, 0);

        return {.harness_pid = harness_pid, .child_pid = child_pid};
    }
}

TEST(
    libcoro_ipc_transport_process_lifetime,
    child_dies_when_parent_dies_unexpectedly)
{
    // Become the subreaper so that when the harness dies the grandchild (IPC child process)
    // is reparented to us rather than to Docker's PID 1.  Without this, PID 1 in a container
    // does not promptly reap zombies, so kill(zombie_pid, 0) keeps returning 0 and the test
    // never detects the child as gone.
    ::prctl(PR_SET_CHILD_SUBREAPER, 1);

    auto harness = spawn_pdeathsig_harness();
    ASSERT_GT(harness.harness_pid, 0);
    ASSERT_GT(harness.child_pid, 0);

    bool child_seen_alive = false;
    for (int i = 0; i < 100; ++i)
    {
        if (::kill(harness.child_pid, 0) == 0)
        {
            child_seen_alive = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(child_seen_alive);

    ASSERT_EQ(::kill(harness.harness_pid, SIGKILL), 0);

    int harness_status = 0;
    ASSERT_EQ(::waitpid(harness.harness_pid, &harness_status, 0), harness.harness_pid);
    ASSERT_TRUE(WIFSIGNALED(harness_status));
    ASSERT_EQ(WTERMSIG(harness_status), SIGKILL);

    // Poll for up to 5 seconds.  Use waitpid(WNOHANG) so we both detect and reap the zombie;
    // kill(zombie, 0) returns 0 (not ESRCH) until the zombie is reaped.
    bool child_gone = false;
    for (int i = 0; i < 500; ++i)
    {
        int child_status = 0;
        pid_t result = ::waitpid(harness.child_pid, &child_status, WNOHANG);
        if (result == harness.child_pid)
        {
            child_gone = true;
            break;
        }
        if (result == -1 && errno == ECHILD)
        {
            // Already reaped by someone else (non-subreaper environment).
            child_gone = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_TRUE(child_gone);
}
#  endif

#endif // CANOPY_BUILD_COROUTINE
