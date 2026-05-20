/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include "sgx_coroutine_test_host.h"

#include <attestation/sgx_sim_protocol.h>
#include <attestation_test/attestation_test.h>
#include <gtest/gtest.h>
#include <rpc/internal/serialiser.h>
#include <rpc/rpc.h>
#include <security/attestation/service.h>
#include <security/attestation/backends/simulation/simulation_backend.h>
#include <transports/sgx_coroutine/host/connect.h>
#include <transports/sgx_coroutine/host/transport.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

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

#if defined(CANOPY_ATTESTATION_BACKEND_SGX_SIM)
    [[nodiscard]] bool parse_local_challenge(
        const attestation_test::sgx_sim_test_cmw& challenge,
        rpc::attestation::sgx_sim_local_attestation_challenge& out)
    {
        if (challenge.media_type != canopy::security::attestation::simulation_evidence_media_type)
            return false;
        if (challenge.content_format != canopy::security::attestation::simulation_local_challenge_content_format)
            return false;
        return rpc::from_canonical_crypto(rpc::byte_span(challenge.payload), out).empty();
    }

    [[nodiscard]] bool parse_local_report(
        const attestation_test::sgx_sim_test_cmw& report,
        rpc::attestation::sgx_sim_local_attestation_report& out)
    {
        if (report.media_type != canopy::security::attestation::simulation_evidence_media_type)
            return false;
        if (report.content_format != canopy::security::attestation::simulation_local_report_evidence_content_format)
            return false;
        return rpc::from_canonical_crypto(rpc::byte_span(report.payload), out).empty();
    }

    [[nodiscard]] attestation_test::sgx_sim_test_cmw make_tampered_challenge(
        const attestation_test::sgx_sim_test_cmw& challenge,
        rpc::attestation::sgx_sim_local_attestation_challenge parsed_challenge)
    {
        if (!parsed_challenge.nonce.empty())
            parsed_challenge.nonce[0] ^= 0xff;

        auto tampered = challenge;
        auto payload = rpc::to_canonical_crypto<std::vector<uint8_t>>(parsed_challenge);
        tampered.payload = std::move(payload);
        return tampered;
    }
#endif

    class root_service_owner final
    {
    public:
        root_service_owner(
            const char* name,
            const std::shared_ptr<coro::scheduler>& scheduler)
            : service_(
                  rpc::root_service::create(
                      name,
                      rpc::DEFAULT_PREFIX,
                      scheduler))
        {
        }

        [[nodiscard]] const std::shared_ptr<rpc::service>& service() const { return service_; }

        void set_shutdown_event(const std::shared_ptr<rpc::event>& shutdown_event)
        {
            if (service_)
                service_->set_shutdown_event(shutdown_event);
        }

        void reset() { service_.reset(); }

    private:
        // The root service API is shared_ptr-based. Keep that ownership in a
        // small fixture-local scope object so the test itself does not retain
        // service/transport references beyond the shutdown phase.
        std::shared_ptr<rpc::service> service_;
    };

    class sgx_attestation_test_host_fixture : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
#if !defined(CANOPY_ATTESTATION_BACKEND_SGX_SIM)
            GTEST_SKIP() << "SGX SIM attestation backend is not selected";
#else
            scheduler_ = std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
                coro::scheduler::options{.thread_strategy = coro::scheduler::thread_strategy_t::manual,
                    .pool = coro::thread_pool::options{.thread_count = 1}}));
            root_service_ = std::make_unique<root_service_owner>("sgx attestation test host", scheduler_);

            auto host_result = enclave_connection_test_host::create_for_test();
            ASSERT_EQ(host_result.error_code, rpc::error::OK());
            host_ = std::move(host_result.output_interface);

            ASSERT_TRUE(connect_enclave("attestation test child A", test_a_));
            ASSERT_TRUE(connect_enclave("attestation test child B", test_b_));
#endif
        }

        void TearDown() override
        {
            if (!scheduler_)
                return;

            auto transport_refs = std::move(transport_refs_);

            auto shutdown_event = std::make_shared<rpc::event>(false);
            if (root_service_)
                root_service_->set_shutdown_event(shutdown_event);

            std::atomic<bool> interfaces_released{false};
            auto release_task = [&]() -> CORO_TASK(void)
            {
                // Release generated RPC proxies on the scheduler so any
                // release messages, transport callbacks, and service cleanup
                // run in the same event context that created the enclave links.
                test_a_ = nullptr;
                test_b_ = nullptr;
                host_ = nullptr;
                interfaces_released.store(true, std::memory_order_release);
                CO_RETURN;
            };
            ASSERT_TRUE(scheduler_->spawn_detached(release_task()));
            wait_for_flag(interfaces_released, std::chrono::milliseconds{5000});
            ASSERT_TRUE(interfaces_released.load(std::memory_order_acquire));

            std::atomic<bool> root_shutdown_complete{false};
            auto root_service = std::move(root_service_);
            auto root_shutdown_task = [root_service = std::move(root_service),
                                          shutdown_event,
                                          &root_shutdown_complete]() mutable -> CORO_TASK(void)
            {
                if (root_service)
                    root_service->reset();
                root_service.reset();
                if (shutdown_event)
                    CO_AWAIT shutdown_event->wait();
                root_shutdown_complete.store(true, std::memory_order_release);
                CO_RETURN;
            };
            ASSERT_TRUE(scheduler_->spawn_detached(root_shutdown_task()));
            wait_for_flag(root_shutdown_complete, std::chrono::milliseconds{5000});
            ASSERT_TRUE(root_shutdown_complete.load(std::memory_order_acquire));
            wait_for_transports(transport_refs, std::chrono::milliseconds{5000});
            for (const auto& transport : transport_refs)
                ASSERT_TRUE(transport.expired());

            if (scheduler_)
                scheduler_->shutdown();
            scheduler_.reset();
        }

        [[nodiscard]] bool connect_enclave(
            const char* name,
            rpc::shared_ptr<attestation_test::i_attestation_enclave_test>& test)
        {
            auto transport = std::make_shared<rpc::sgx::coro::host::transport>(
                name, root_service_->service(), CANOPY_TEST_SGX_ATTESTATION_ENCLAVE_PATH);
            transport_refs_.push_back(transport);
            auto result = run_on_manual_scheduler<rpc::service_connect_result<attestation_test::i_attestation_enclave_test>>(
                scheduler_,
                rpc::sgx::coro::host::connect_to_enclave_zone<yyy::i_host, attestation_test::i_attestation_enclave_test>(
                    root_service_->service(), name, std::move(transport), host_));

            test = std::move(result.output_interface);
            if (result.error_code != rpc::error::OK() || !test)
            {
                ADD_FAILURE() << "failed to connect " << name << " error=" << result.error_code;
                return false;
            }
            return true;
        }

        void wait_for_flag(
            const std::atomic<bool>& flag,
            std::chrono::milliseconds timeout)
        {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (!flag.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
                scheduler_->process_events(std::chrono::milliseconds{1});
        }

        void wait_for_transports(
            const std::vector<std::weak_ptr<rpc::sgx::coro::host::transport>>& transports,
            std::chrono::milliseconds timeout)
        {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (std::chrono::steady_clock::now() < deadline)
            {
                bool all_expired = true;
                for (const auto& transport : transports)
                    all_expired = all_expired && transport.expired();
                if (all_expired)
                    return;
                scheduler_->process_events(std::chrono::milliseconds{1});
            }
        }

        std::shared_ptr<coro::scheduler> scheduler_;
        std::unique_ptr<root_service_owner> root_service_;
        rpc::shared_ptr<yyy::i_host> host_;
        std::vector<std::weak_ptr<rpc::sgx::coro::host::transport>> transport_refs_;
        rpc::shared_ptr<attestation_test::i_attestation_enclave_test> test_a_;
        rpc::shared_ptr<attestation_test::i_attestation_enclave_test> test_b_;
    };
}

TEST_F(
    sgx_attestation_test_host_fixture,
    sgx_sim_report_evidence_is_generated_and_verified_inside_enclave)
{
#if defined(CANOPY_ATTESTATION_BACKEND_SGX_SIM)
    attestation_test::sgx_sim_report_self_test_result result;
    auto err = SYNC_WAIT(test_a_->sgx_sim_report_self_test(result));
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

TEST_F(
    sgx_attestation_test_host_fixture,
    sgx_sim_peer_targeted_reports_are_verified_between_two_enclaves)
{
#if defined(CANOPY_ATTESTATION_BACKEND_SGX_SIM)
    attestation_test::sgx_sim_test_cmw challenge_b;
    ASSERT_EQ(SYNC_WAIT(test_b_->sgx_sim_make_local_attestation_challenge(0x21, challenge_b)), rpc::error::OK());

    rpc::attestation::sgx_sim_local_attestation_challenge parsed_challenge_b;
    ASSERT_TRUE(parse_local_challenge(challenge_b, parsed_challenge_b));
    ASSERT_GT(parsed_challenge_b.target_info.size(), 0U);
    ASSERT_EQ(parsed_challenge_b.nonce.size(), canopy::security::attestation::attestation_nonce_size);

    attestation_test::sgx_sim_test_cmw report_a_to_b;
    ASSERT_EQ(SYNC_WAIT(test_a_->sgx_sim_make_local_attestation_report(challenge_b, report_a_to_b)), rpc::error::OK());

    rpc::attestation::sgx_sim_local_attestation_report parsed_report_a_to_b;
    ASSERT_TRUE(parse_local_report(report_a_to_b, parsed_report_a_to_b));
    ASSERT_GT(parsed_report_a_to_b.report.size(), 0U);
    ASSERT_EQ(parsed_report_a_to_b.report_data_sha256.size(), 32U);

    attestation_test::sgx_sim_local_attestation_verification verification_a_to_b;
    ASSERT_EQ(
        SYNC_WAIT(test_b_->sgx_sim_verify_local_attestation_report(report_a_to_b, challenge_b, verification_a_to_b)),
        rpc::error::OK())
        << verification_a_to_b.failure_reason;
    EXPECT_TRUE(verification_a_to_b.accepted) << verification_a_to_b.failure_reason;
    EXPECT_TRUE(verification_a_to_b.report_data_matched);
    EXPECT_TRUE(verification_a_to_b.sgx_verify_report_succeeded);

    auto tampered_challenge = make_tampered_challenge(challenge_b, parsed_challenge_b);
    attestation_test::sgx_sim_local_attestation_verification tampered_verification;
    ASSERT_EQ(
        SYNC_WAIT(
            test_b_->sgx_sim_verify_local_attestation_report(report_a_to_b, tampered_challenge, tampered_verification)),
        rpc::error::OK())
        << tampered_verification.failure_reason;
    EXPECT_FALSE(tampered_verification.accepted);
    EXPECT_FALSE(tampered_verification.report_data_matched);
    EXPECT_FALSE(tampered_verification.sgx_verify_report_succeeded);

    attestation_test::sgx_sim_test_cmw challenge_a;
    ASSERT_EQ(SYNC_WAIT(test_a_->sgx_sim_make_local_attestation_challenge(0x63, challenge_a)), rpc::error::OK());

    rpc::attestation::sgx_sim_local_attestation_challenge parsed_challenge_a;
    ASSERT_TRUE(parse_local_challenge(challenge_a, parsed_challenge_a));
    ASSERT_GT(parsed_challenge_a.target_info.size(), 0U);
    ASSERT_EQ(parsed_challenge_a.nonce.size(), canopy::security::attestation::attestation_nonce_size);

    attestation_test::sgx_sim_test_cmw report_b_to_a;
    ASSERT_EQ(SYNC_WAIT(test_b_->sgx_sim_make_local_attestation_report(challenge_a, report_b_to_a)), rpc::error::OK());

    rpc::attestation::sgx_sim_local_attestation_report parsed_report_b_to_a;
    ASSERT_TRUE(parse_local_report(report_b_to_a, parsed_report_b_to_a));
    ASSERT_GT(parsed_report_b_to_a.report.size(), 0U);
    ASSERT_EQ(parsed_report_b_to_a.report_data_sha256.size(), 32U);

    attestation_test::sgx_sim_local_attestation_verification verification_b_to_a;
    ASSERT_EQ(
        SYNC_WAIT(test_a_->sgx_sim_verify_local_attestation_report(report_b_to_a, challenge_a, verification_b_to_a)),
        rpc::error::OK())
        << verification_b_to_a.failure_reason;
    EXPECT_TRUE(verification_b_to_a.accepted) << verification_b_to_a.failure_reason;
    EXPECT_TRUE(verification_b_to_a.report_data_matched);
    EXPECT_TRUE(verification_b_to_a.sgx_verify_report_succeeded);
#else
    GTEST_SKIP() << "SGX SIM attestation backend is not selected";
#endif
}
