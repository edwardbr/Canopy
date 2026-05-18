/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include "sgx_coroutine_test_host.h"

#include <attestation_test/attestation_test.h>
#include <gtest/gtest.h>
#include <rpc/rpc.h>
#include <transports/sgx_coroutine/host/connect.h>
#include <transports/sgx_coroutine/host/transport.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <utility>

namespace
{
    template<
        class Result,
        class Awaitable>
    Result run_on_manual_scheduler(
        const std::shared_ptr<coro::scheduler>& scheduler,
        Awaitable&& awaitable,
        std::chrono::milliseconds timeout = std::chrono::milliseconds{20000})
    {
        Result result{};
        result.error_code = rpc::error::CALL_TIMEOUT();
        std::atomic<bool> done{false};

        auto runner = [task = std::forward<Awaitable>(awaitable), &result, &done]() mutable -> CORO_TASK(void)
        {
            result = CO_AWAIT task;
            done.store(true, std::memory_order_release);
            CO_RETURN;
        };

        RPC_ASSERT(scheduler && scheduler->spawn_detached(runner()));

        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!done.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
            scheduler->process_events(std::chrono::milliseconds{1});

        RPC_ASSERT(done.load(std::memory_order_acquire));
        return result;
    }

    CORO_TASK(void) release_remote_interface(rpc::shared_ptr<attestation_test::i_attestation_enclave_test>& test)
    {
        test = nullptr;
        CO_RETURN;
    }

    class sgx_attestation_test_host_fixture : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            scheduler_ = std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
                coro::scheduler::options{.thread_strategy = coro::scheduler::thread_strategy_t::manual,
                    .pool = coro::thread_pool::options{.thread_count = 1}}));
            root_service_ = rpc::root_service::create("sgx attestation test host", rpc::DEFAULT_PREFIX, scheduler_);

            auto host_result = enclave_connection_test_host::create_for_test();
            ASSERT_EQ(host_result.error_code, rpc::error::OK());
            host_ = std::move(host_result.output_interface);

            transport_ = std::make_shared<rpc::sgx::coro::host::transport>(
                "attestation test child", root_service_, CANOPY_TEST_SGX_ATTESTATION_ENCLAVE_PATH);
            auto result = run_on_manual_scheduler<rpc::service_connect_result<attestation_test::i_attestation_enclave_test>>(
                scheduler_,
                rpc::sgx::coro::host::connect_to_enclave_zone<yyy::i_host, attestation_test::i_attestation_enclave_test>(
                    root_service_, "attestation test child", transport_, host_));

            test_ = std::move(result.output_interface);
            ASSERT_EQ(result.error_code, rpc::error::OK());
            ASSERT_TRUE(test_);
        }

        void TearDown() override
        {
            if (scheduler_ && test_)
            {
                ASSERT_TRUE(scheduler_->spawn_detached(release_remote_interface(test_)));
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{5000};
                while (test_ && std::chrono::steady_clock::now() < deadline)
                    scheduler_->process_events(std::chrono::milliseconds{1});
                ASSERT_FALSE(test_);
            }

            host_ = nullptr;
            transport_.reset();
            root_service_.reset();
            if (scheduler_)
                scheduler_->shutdown();
            scheduler_.reset();
        }

        std::shared_ptr<coro::scheduler> scheduler_;
        std::shared_ptr<rpc::root_service> root_service_;
        rpc::shared_ptr<yyy::i_host> host_;
        std::shared_ptr<rpc::sgx::coro::host::transport> transport_;
        rpc::shared_ptr<attestation_test::i_attestation_enclave_test> test_;
    };
}

TEST_F(
    sgx_attestation_test_host_fixture,
    sgx_sim_report_evidence_is_generated_and_verified_inside_enclave)
{
#if defined(CANOPY_ATTESTATION_BACKEND_SGX_SIM)
    attestation_test::sgx_sim_report_self_test_result result;
    auto err = SYNC_WAIT(test_->sgx_sim_report_self_test(result));
    ASSERT_EQ(err, rpc::error::OK()) << result.verifier_reason;
    EXPECT_TRUE(result.produced_report_evidence);
    EXPECT_TRUE(result.producer_verified_report);
    EXPECT_TRUE(result.verifier_accepted_report) << result.verifier_reason;
    EXPECT_EQ(result.content_format, "canopy.sgx-sim-report.v1");
    EXPECT_GT(result.payload_size, 0U);
    EXPECT_GT(result.report_size, 0U);
    EXPECT_GT(result.target_info_size, 0U);
    EXPECT_EQ(result.report_data_hash_size, 32U);
    EXPECT_GT(result.development_signature_size, 0U);
#else
    GTEST_SKIP() << "SGX SIM attestation backend is not selected";
#endif
}
