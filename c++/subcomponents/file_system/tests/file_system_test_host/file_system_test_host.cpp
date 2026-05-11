/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <gtest/gtest.h>

#include <file_system/file_system_manager.h>
#include <io_uring/linux_io_uring_handle.h>
#include <rpc/rpc.h>

#ifdef CANOPY_BUILD_ENCLAVE
#  include <transports/sgx_coroutine/host/connect.h>
#  include <transports/sgx_coroutine/host/transport.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace
{
    std::vector<uint8_t> make_payload(
        size_t size,
        uint8_t seed)
    {
        std::vector<uint8_t> payload(size);
        for (size_t index = 0; index < payload.size(); ++index)
        {
            payload[index] = static_cast<uint8_t>(seed + static_cast<uint8_t>(index * 13U));
        }
        return payload;
    }

    class temp_tree
    {
    public:
        temp_tree()
        {
            auto name = std::string("canopy_file_system_test_")
                        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
            root_ = std::filesystem::temp_directory_path() / name;
            std::filesystem::create_directories(root_);
        }

        ~temp_tree()
        {
            std::error_code ec;
            std::filesystem::remove_all(root_, ec);
        }

        [[nodiscard]] std::filesystem::path path(const std::string& filename) const { return root_ / filename; }

    private:
        std::filesystem::path root_;
    };

    struct direct_manager_result
    {
        int error_code{rpc::error::OK()};
        std::shared_ptr<rpc::io_uring::linux_io_uring_handle> handle;
        std::shared_ptr<rpc::io_uring::controller> controller;
        rpc::shared_ptr<rpc::file_system::i_manager> manager;
    };

    direct_manager_result create_direct_manager()
    {
        rpc::io_uring::linux_io_uring_handle::options handle_options;
        handle_options.queue_depth = 128;
        handle_options.buffer_count = 32;
        handle_options.buffer_size = 64U * 1024U;
        handle_options.register_buffers = false;
        handle_options.register_fixed_files = true;
        handle_options.fixed_file_count = 64;
        handle_options.use_sqpoll = true;

        std::shared_ptr<rpc::io_uring::linux_io_uring_handle> handle;
        auto err = rpc::io_uring::linux_io_uring_handle::create(handle, handle_options);
        if (err != rpc::error::OK())
        {
            return direct_manager_result{err, {}, {}, {}};
        }

        rpc::io_uring::controller::options controller_options;
        controller_options.completion_wait_strategy = rpc::io_uring::wait_strategy::cooperative_poll;
        controller_options.use_caller_buffers_for_transfers = true;
        auto controller = std::make_shared<rpc::io_uring::controller>(handle, nullptr, controller_options);
        auto manager = rpc::file_system::create_factory(controller);
        return direct_manager_result{rpc::error::OK(), std::move(handle), std::move(controller), std::move(manager)};
    }

    void skip_if_iouring_unavailable(int error_code)
    {
        if (error_code == rpc::error::NATIVE_IO_ERROR() || error_code == rpc::error::INCOMPATIBLE_SERVICE()
            || error_code == rpc::error::PROTOCOL_ERROR())
        {
            GTEST_SKIP() << "direct io_uring fixed-file setup is unavailable in this environment";
        }
    }

    void expect_round_trip(
        const rpc::shared_ptr<rpc::file_system::i_manager>& manager,
        const std::filesystem::path& file_path,
        const std::vector<uint8_t>& payload)
    {
        ASSERT_TRUE(manager);

        auto write_error = SYNC_WAIT(manager->write_file(file_path.string(), payload));
        ASSERT_EQ(write_error, rpc::error::OK());

        std::vector<uint8_t> readback;
        auto read_error = SYNC_WAIT(manager->read_file(file_path.string(), readback));
        ASSERT_EQ(read_error, rpc::error::OK());
        EXPECT_EQ(readback, payload);
    }
}

TEST(
    FileSystemManagerHost,
    DirectIoUringRoundTrip)
{
    auto direct = create_direct_manager();
    skip_if_iouring_unavailable(direct.error_code);
    ASSERT_EQ(direct.error_code, rpc::error::OK());

    temp_tree tree;
    expect_round_trip(direct.manager, tree.path("round_trip.bin"), make_payload(130U * 1024U + 37U, 17));

    std::ignore = SYNC_WAIT(direct.controller->shutdown());
    direct.handle->close();
}

TEST(
    FileSystemManagerHost,
    ConcurrentDirectIoUringRoundTripsUseOneController)
{
    auto direct = create_direct_manager();
    skip_if_iouring_unavailable(direct.error_code);
    ASSERT_EQ(direct.error_code, rpc::error::OK());

    temp_tree tree;
    std::vector<std::future<int>> workers;
    for (uint8_t index = 0; index < 6; ++index)
    {
        workers.emplace_back(
            std::async(
                std::launch::async,
                [manager = direct.manager, file_path = tree.path("concurrent_" + std::to_string(index) + ".bin"), index]
                {
                    const auto payload = make_payload(32U * 1024U + static_cast<size_t>(index) * 97U, index);
                    auto write_error = SYNC_WAIT(manager->write_file(file_path.string(), payload));
                    if (write_error != rpc::error::OK())
                    {
                        return write_error;
                    }

                    std::vector<uint8_t> readback;
                    auto read_error = SYNC_WAIT(manager->read_file(file_path.string(), readback));
                    if (read_error != rpc::error::OK())
                    {
                        return read_error;
                    }

                    return readback == payload ? rpc::error::OK() : rpc::error::INVALID_DATA();
                }));
    }

    for (auto& worker : workers)
    {
        EXPECT_EQ(worker.get(), rpc::error::OK());
    }

    std::ignore = SYNC_WAIT(direct.controller->shutdown());
    direct.handle->close();
}

TEST(
    FileSystemManagerHost,
    ListFilesIsNotClaimedWithoutDirectIoUringDirectoryEnumeration)
{
    auto direct = create_direct_manager();
    skip_if_iouring_unavailable(direct.error_code);
    ASSERT_EQ(direct.error_code, rpc::error::OK());

    std::vector<rpc::file_system::file_info> files(1);
    auto error = SYNC_WAIT(direct.manager->list_files(".", files));
    EXPECT_EQ(error, rpc::error::NOT_IMPLEMENTED());
    EXPECT_TRUE(files.empty());

    std::ignore = SYNC_WAIT(direct.controller->shutdown());
    direct.handle->close();
}

#ifdef CANOPY_BUILD_ENCLAVE
namespace
{
    class file_system_sgx_fixture : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            scheduler_ = std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
                coro::scheduler::options{
                    .thread_strategy = coro::scheduler::thread_strategy_t::manual,
                    .pool = coro::thread_pool::options{.thread_count = 1},
                }));
            root_service_ = rpc::root_service::create("file system sgx test host", rpc::DEFAULT_PREFIX, scheduler_);

            rpc::io_uring::host_controller::options host_controller_options;
            host_controller_options.register_fixed_files = true;
            host_controller_options.fixed_file_count = 64;
            host_controller_options.buffer_count = 32;
            host_controller_options.buffer_size = 64U * 1024U;
            host_controller_options.use_sqpoll = true;

            auto transport = std::make_shared<rpc::sgx::coro::host::transport>(
                "file system test enclave", root_service_, CANOPY_FILE_SYSTEM_TEST_ENCLAVE_PATH);
            transports_.push_back(transport);

            auto result
                = SYNC_WAIT((rpc::sgx::coro::host::connect_to_enclave_zone<rpc::i_noop, rpc::file_system::i_manager>(
                    root_service_, "file system test enclave", transport, {}, host_controller_options)));
            connect_error_ = result.error_code;
            manager_ = std::move(result.output_interface);
        }

        void TearDown() override
        {
            if (!scheduler_)
            {
                return;
            }

            auto shutdown_event = std::make_shared<rpc::event>(false);
            if (root_service_)
            {
                root_service_->set_shutdown_event(shutdown_event);
            }

            teardown_done_.store(false);
            ASSERT_TRUE(scheduler_->spawn_detached(release_for_teardown(std::move(shutdown_event))));
            while (!teardown_done_.load())
            {
                scheduler_->process_events(std::chrono::milliseconds(1));
            }

            auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{5000};
            while (!all_transports_expired() && std::chrono::steady_clock::now() < deadline)
            {
                scheduler_->process_events(std::chrono::milliseconds(1));
            }
            EXPECT_TRUE(all_transports_expired());
            transports_.clear();

            scheduler_->shutdown();
            scheduler_.reset();
        }

        [[nodiscard]] bool all_transports_expired() const
        {
            for (const auto& transport : transports_)
            {
                if (!transport.expired())
                {
                    return false;
                }
            }
            return true;
        }

        CORO_TASK(void) release_for_teardown(std::shared_ptr<rpc::event> shutdown_event)
        {
            manager_ = nullptr;
            root_service_ = nullptr;
            if (shutdown_event)
            {
                CO_AWAIT shutdown_event->wait();
            }
            teardown_done_.store(true);
            CO_RETURN;
        }

        std::shared_ptr<coro::scheduler> scheduler_;
        std::shared_ptr<rpc::root_service> root_service_;
        rpc::shared_ptr<rpc::file_system::i_manager> manager_;
        std::vector<std::weak_ptr<rpc::sgx::coro::host::transport>> transports_;
        std::atomic_bool teardown_done_{false};
        int connect_error_{rpc::error::OK()};
    };
}

TEST_F(
    file_system_sgx_fixture,
    EnclaveDirectIoUringRoundTrip)
{
    skip_if_iouring_unavailable(connect_error_);
    ASSERT_EQ(connect_error_, rpc::error::OK());
    ASSERT_TRUE(manager_);

    temp_tree tree;
    expect_round_trip(manager_, tree.path("enclave_round_trip.bin"), make_payload(96U * 1024U + 11U, 91));
}
#endif
