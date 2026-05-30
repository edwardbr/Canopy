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
#include <security/attestation/backends/sgx_dcap/sgx_dcap_backend.h>
#include <security/attestation/backends/sgx_dcap/sgx_dcap_host_quote_provider.h>
#include <security/attestation/backends/sgx_dcap/sgx_dcap_host_quote_verifier.h>
#include <security/attestation/backends/sgx_epid/sgx_epid_host_quote_provider.h>
#include <security/attestation/backends/simulation/simulation_backend.h>
#include <transports/sgx_coroutine/host/connect.h>
#include <transports/sgx_coroutine/host/transport.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#if (defined(CANOPY_ATTESTATION_BACKEND_SGX_EPID) || defined(CANOPY_ATTESTATION_BACKEND_DCAP))                         \
    && !defined(CANOPY_FAKE_SGX) && !defined(_WIN32)
#  include <dlfcn.h>
#endif

#if defined(CANOPY_ATTESTATION_BACKEND_SGX_EPID) && !defined(CANOPY_FAKE_SGX) && !defined(_WIN32)
#  if defined(__clang__)
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wc99-extensions"
#  endif
#  include <sgx_uae_epid.h>
#  if defined(__clang__)
#    pragma clang diagnostic pop
#  endif
#  define CANOPY_ATTESTATION_TEST_HAS_EPID_PSW 1
#else
#  define CANOPY_ATTESTATION_TEST_HAS_EPID_PSW 0
#endif

#if defined(CANOPY_ATTESTATION_BACKEND_DCAP) && !defined(CANOPY_FAKE_SGX) && !defined(_WIN32)
#  if defined(__clang__)
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wc99-extensions"
#    pragma clang diagnostic ignored "-Wflexible-array-extensions"
#  endif
#  include <sgx_dcap_ql_wrapper.h>
#  include <sgx_dcap_quoteverify.h>
#  if defined(__clang__)
#    pragma clang diagnostic pop
#  endif
#  define CANOPY_ATTESTATION_TEST_HAS_DCAP_PSW 1
#else
#  define CANOPY_ATTESTATION_TEST_HAS_DCAP_PSW 0
#endif

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

#if CANOPY_ATTESTATION_TEST_HAS_EPID_PSW
    constexpr size_t epid_spid_size = 16U;
    constexpr uint64_t test_epid_transcript_id = 0x4550494454455354ULL;

    struct epid_psw_library
    {
        using init_quote_fn = sgx_status_t (*)(
            sgx_target_info_t*,
            sgx_epid_group_id_t*);
        using calc_quote_size_fn = sgx_status_t (*)(
            const uint8_t*,
            uint32_t,
            uint32_t*);
        using get_quote_fn = sgx_status_t (*)(
            const sgx_report_t*,
            sgx_quote_sign_type_t,
            const sgx_spid_t*,
            const sgx_quote_nonce_t*,
            const uint8_t*,
            uint32_t,
            sgx_report_t*,
            sgx_quote_t*,
            uint32_t);

        epid_psw_library() = default;

        ~epid_psw_library()
        {
            if (handle)
                dlclose(handle);
        }

        epid_psw_library(const epid_psw_library&) = delete;
        auto operator=(const epid_psw_library&) -> epid_psw_library& = delete;

        void* handle{nullptr};
        init_quote_fn init_quote{nullptr};
        calc_quote_size_fn calc_quote_size{nullptr};
        get_quote_fn get_quote{nullptr};
    };

    template<typename Function>
    [[nodiscard]] auto load_symbol(
        void* handle,
        const char* name,
        Function& function,
        std::string& error) -> bool
    {
        dlerror();
        auto* symbol = dlsym(handle, name);
        if (!symbol)
        {
            const auto* dl_error = dlerror();
            error = dl_error ? dl_error : name;
            return false;
        }
        function = reinterpret_cast<Function>(symbol);
        return true;
    }

    [[nodiscard]] auto load_epid_psw_library(std::string& error) -> std::unique_ptr<epid_psw_library>
    {
        void* handle = dlopen("libsgx_epid.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!handle)
            handle = dlopen("libsgx_epid.so", RTLD_NOW | RTLD_LOCAL);
        if (!handle)
        {
            const auto* dl_error = dlerror();
            error = dl_error ? dl_error : "libsgx_epid.so could not be loaded";
            return {};
        }

        auto library = std::unique_ptr<epid_psw_library>(new epid_psw_library{});
        library->handle = handle;
        if (!load_symbol(handle, "sgx_init_quote", library->init_quote, error)
            || !load_symbol(handle, "sgx_calc_quote_size", library->calc_quote_size, error)
            || !load_symbol(handle, "sgx_get_quote", library->get_quote, error))
        {
            return {};
        }
        return library;
    }

    [[nodiscard]] auto sgx_status_to_hex(sgx_status_t status) -> std::string
    {
        std::ostringstream out;
        out << "0x" << std::hex << std::setw(8) << std::setfill('0') << static_cast<uint32_t>(status);
        return out.str();
    }

    [[nodiscard]] auto is_environmental_epid_status(sgx_status_t status) -> bool
    {
        return status == SGX_ERROR_NETWORK_FAILURE || status == SGX_ERROR_SERVICE_UNAVAILABLE
               || status == SGX_ERROR_AE_INVALID_EPIDBLOB;
    }

    [[nodiscard]] auto copy_bytes(const auto& value) -> std::vector<uint8_t>
    {
        const auto* begin = reinterpret_cast<const uint8_t*>(&value);
        return std::vector<uint8_t>(begin, begin + sizeof(value));
    }

    [[nodiscard]] auto report_data_matches(
        const std::vector<uint8_t>& report_blob,
        const std::vector<uint8_t>& expected_hash) -> bool
    {
        if (report_blob.size() != sizeof(sgx_report_t) || expected_hash.size() != 32U)
            return false;

        sgx_report_t report{};
        std::memcpy(&report, report_blob.data(), sizeof(report));
        if (!std::equal(expected_hash.begin(), expected_hash.end(), report.body.report_data.d))
            return false;

        const auto* zero_begin = report.body.report_data.d + expected_hash.size();
        const auto* zero_end = report.body.report_data.d + sizeof(report.body.report_data.d);
        return std::all_of(zero_begin, zero_end, [](uint8_t value) { return value == 0; });
    }

    [[nodiscard]] auto test_epid_binding() -> canopy::security::attestation::evidence_binding
    {
        canopy::security::attestation::evidence_binding binding;
        binding.subject = canopy::security::attestation::identity{
            "sgx-epid-attestation-test-enclave", "sgx-epid-attestation-test-zone"};
        binding.transcript_id = test_epid_transcript_id;
        binding.nonce = {0x43, 0x61, 0x6e, 0x6f, 0x70, 0x79, 0x2d, 0x45, 0x50, 0x49, 0x44};
        return binding;
    }

    [[nodiscard]] auto test_report_data_hash() -> std::vector<uint8_t>
    {
        std::vector<uint8_t> report_data(32U);
        for (size_t index = 0; index < report_data.size(); ++index)
            report_data[index] = static_cast<uint8_t>(0xa0U + index);
        return report_data;
    }
#endif

#if CANOPY_ATTESTATION_TEST_HAS_DCAP_PSW
    constexpr uint64_t test_dcap_transcript_id = 0x4443415054455354ULL;

    struct dcap_psw_library
    {
        using get_target_info_fn = quote3_error_t (*)(sgx_target_info_t*);
        using get_quote_size_fn = quote3_error_t (*)(uint32_t*);
        using get_quote_fn = quote3_error_t (*)(
            const sgx_report_t*,
            uint32_t,
            uint8_t*);
        using get_supplemental_data_size_fn = quote3_error_t (*)(uint32_t*);
        using verify_quote_fn = quote3_error_t (*)(
            const uint8_t*,
            uint32_t,
            const sgx_ql_qve_collateral_t*,
            time_t,
            uint32_t*,
            sgx_ql_qv_result_t*,
            sgx_ql_qe_report_info_t*,
            uint32_t,
            uint8_t*);

        dcap_psw_library() = default;

        ~dcap_psw_library()
        {
            if (quote_handle)
                dlclose(quote_handle);
            if (verify_handle)
                dlclose(verify_handle);
        }

        dcap_psw_library(const dcap_psw_library&) = delete;
        auto operator=(const dcap_psw_library&) -> dcap_psw_library& = delete;

        void* quote_handle{nullptr};
        void* verify_handle{nullptr};
        get_target_info_fn get_target_info{nullptr};
        get_quote_size_fn get_quote_size{nullptr};
        get_quote_fn get_quote{nullptr};
        get_supplemental_data_size_fn get_supplemental_data_size{nullptr};
        verify_quote_fn verify_quote{nullptr};
    };

    template<typename Function>
    [[nodiscard]] auto load_dcap_symbol(
        void* handle,
        const char* name,
        Function& function,
        std::string& error) -> bool
    {
        dlerror();
        auto* symbol = dlsym(handle, name);
        if (!symbol)
        {
            const auto* dl_error = dlerror();
            error = dl_error ? dl_error : name;
            return false;
        }
        function = reinterpret_cast<Function>(symbol);
        return true;
    }

    [[nodiscard]] auto open_dcap_library(
        const char* soname,
        const char* fallback,
        std::string& error) -> void*
    {
        auto* handle = dlopen(soname, RTLD_NOW | RTLD_LOCAL);
        if (!handle)
            handle = dlopen(fallback, RTLD_NOW | RTLD_LOCAL);
        if (!handle)
        {
            const auto* dl_error = dlerror();
            error = dl_error ? dl_error : soname;
        }
        return handle;
    }

    [[nodiscard]] auto load_dcap_psw_library(std::string& error) -> std::unique_ptr<dcap_psw_library>
    {
        auto library = std::unique_ptr<dcap_psw_library>(new dcap_psw_library{});
        library->quote_handle = open_dcap_library("libsgx_dcap_ql.so.1", "libsgx_dcap_ql.so", error);
        if (!library->quote_handle)
            return {};
        library->verify_handle = open_dcap_library("libsgx_dcap_quoteverify.so.1", "libsgx_dcap_quoteverify.so", error);
        if (!library->verify_handle)
            return {};

        if (!load_dcap_symbol(library->quote_handle, "sgx_qe_get_target_info", library->get_target_info, error)
            || !load_dcap_symbol(library->quote_handle, "sgx_qe_get_quote_size", library->get_quote_size, error)
            || !load_dcap_symbol(library->quote_handle, "sgx_qe_get_quote", library->get_quote, error)
            || !load_dcap_symbol(
                library->verify_handle, "sgx_qv_get_quote_supplemental_data_size", library->get_supplemental_data_size, error)
            || !load_dcap_symbol(library->verify_handle, "sgx_qv_verify_quote", library->verify_quote, error))
        {
            return {};
        }
        return library;
    }

    [[nodiscard]] auto quote3_status_to_hex(quote3_error_t status) -> std::string
    {
        std::ostringstream out;
        out << "0x" << std::hex << std::setw(8) << std::setfill('0') << static_cast<uint32_t>(status);
        return out.str();
    }

    [[nodiscard]] auto qv_result_name(sgx_ql_qv_result_t result) -> const char*
    {
        switch (result)
        {
        case SGX_QL_QV_RESULT_OK:
            return "OK";
        case SGX_QL_QV_RESULT_CONFIG_NEEDED:
            return "CONFIG_NEEDED";
        case SGX_QL_QV_RESULT_OUT_OF_DATE:
            return "OUT_OF_DATE";
        case SGX_QL_QV_RESULT_OUT_OF_DATE_CONFIG_NEEDED:
            return "OUT_OF_DATE_CONFIG_NEEDED";
        case SGX_QL_QV_RESULT_INVALID_SIGNATURE:
            return "INVALID_SIGNATURE";
        case SGX_QL_QV_RESULT_REVOKED:
            return "REVOKED";
        case SGX_QL_QV_RESULT_UNSPECIFIED:
            return "UNSPECIFIED";
        case SGX_QL_QV_RESULT_SW_HARDENING_NEEDED:
            return "SW_HARDENING_NEEDED";
        case SGX_QL_QV_RESULT_CONFIG_AND_SW_HARDENING_NEEDED:
            return "CONFIG_AND_SW_HARDENING_NEEDED";
        default:
            return "UNKNOWN";
        }
    }

    [[nodiscard]] auto is_environmental_dcap_status(quote3_error_t status) -> bool
    {
        return status == SGX_QL_NO_DEVICE || status == SGX_QL_SERVICE_UNAVAILABLE || status == SGX_QL_NETWORK_FAILURE
               || status == SGX_QL_SERVICE_TIMEOUT || status == SGX_QL_ERROR_INVALID_PRIVILEGE
               || status == SGX_QL_NO_PLATFORM_CERT_DATA || status == SGX_QL_CERTS_UNAVAILABLE
               || status == SGX_QL_PLATFORM_UNKNOWN || status == SGX_QL_ROOT_CA_UNTRUSTED
               || status == SGX_QL_CONFIG_INVALID_JSON || status == SGX_QL_ENCLAVE_LOAD_ERROR
               || status == SGX_QL_INTERFACE_UNAVAILABLE || status == SGX_QL_PLATFORM_LIB_UNAVAILABLE
               || status == SGX_QL_PSW_NOT_AVAILABLE;
    }

    [[nodiscard]] auto copy_dcap_bytes(const auto& value) -> std::vector<uint8_t>
    {
        const auto* begin = reinterpret_cast<const uint8_t*>(&value);
        return std::vector<uint8_t>(begin, begin + sizeof(value));
    }

    [[nodiscard]] auto dcap_report_data_matches(
        const std::vector<uint8_t>& report_blob,
        const std::vector<uint8_t>& expected_hash) -> bool
    {
        if (report_blob.size() != sizeof(sgx_report_t) || expected_hash.size() != 32U)
            return false;

        sgx_report_t report{};
        std::memcpy(&report, report_blob.data(), sizeof(report));
        if (!std::equal(expected_hash.begin(), expected_hash.end(), report.body.report_data.d))
            return false;

        const auto* zero_begin = report.body.report_data.d + expected_hash.size();
        const auto* zero_end = report.body.report_data.d + sizeof(report.body.report_data.d);
        return std::all_of(zero_begin, zero_end, [](uint8_t value) { return value == 0; });
    }

    [[nodiscard]] auto test_dcap_binding() -> canopy::security::attestation::evidence_binding
    {
        canopy::security::attestation::evidence_binding binding;
        binding.subject = canopy::security::attestation::identity{
            "sgx-dcap-attestation-test-enclave", "sgx-dcap-attestation-test-zone"};
        binding.transcript_id = test_dcap_transcript_id;
        binding.nonce = {0x43, 0x61, 0x6e, 0x6f, 0x70, 0x79, 0x2d, 0x44, 0x43, 0x41, 0x50};
        return binding;
    }

    [[nodiscard]] auto test_dcap_policy() -> canopy::security::attestation::attestation_policy
    {
        canopy::security::attestation::attestation_policy policy;
        policy.required_backend_id = canopy::security::attestation::sgx_dcap_backend_id;
        policy.minimum_security_level = canopy::security::attestation::security_level::hardware;
        policy.allow_development_evidence = false;
        return policy;
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
#if !defined(CANOPY_ATTESTATION_BACKEND_SGX_SIM) && !defined(CANOPY_ATTESTATION_BACKEND_SGX_EPID)                      \
    && !defined(CANOPY_ATTESTATION_BACKEND_DCAP)
            GTEST_SKIP() << "an SGX attestation backend is not selected";
#else
            enclave_path_ = resolve_enclave_path();
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
            auto transport
                = std::make_shared<rpc::sgx_coroutine_transport::host::transport>(name, root_service_->service(), enclave_path_);
            transport_refs_.push_back(transport);
            auto result = run_on_manual_scheduler<rpc::service_connect_result<attestation_test::i_attestation_enclave_test>>(
                scheduler_,
                rpc::sgx_coroutine_transport::host::connect_to_enclave_zone<yyy::i_host, attestation_test::i_attestation_enclave_test>(
                    root_service_->service(), name, std::move(transport), host_));

            test = std::move(result.output_interface);
            if (result.error_code != rpc::error::OK() || !test)
            {
                ADD_FAILURE() << "failed to connect " << name << " error=" << result.error_code;
                return false;
            }
            return true;
        }

        [[nodiscard]] static std::string resolve_enclave_path()
        {
            if (const char* override_path = std::getenv("CANOPY_TEST_SGX_ATTESTATION_ENCLAVE_PATH"))
            {
                if (override_path[0] != '\0')
                    return override_path;
            }

            const std::filesystem::path configured_path{CANOPY_TEST_SGX_ATTESTATION_ENCLAVE_PATH};
            std::error_code error;
            if (std::filesystem::exists(configured_path, error))
                return configured_path.string();

            const auto executable_path = std::filesystem::read_symlink("/proc/self/exe", error);
            if (!error)
            {
                const auto sibling_path = executable_path.parent_path() / "libsgx_attestation_test_enclave.signed.so";
                if (std::filesystem::exists(sibling_path, error))
                    return sibling_path.string();
            }

            return configured_path.string();
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
            const std::vector<std::weak_ptr<rpc::sgx_coroutine_transport::host::transport>>& transports,
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
        std::vector<std::weak_ptr<rpc::sgx_coroutine_transport::host::transport>> transport_refs_;
        std::string enclave_path_;
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
    sgx_dcap_backend_produces_and_verifies_quote_with_platform_libraries)
{
#if CANOPY_ATTESTATION_TEST_HAS_DCAP_PSW
    std::string load_error;
    auto psw = load_dcap_psw_library(load_error);
    if (!psw)
        GTEST_SKIP() << "Intel DCAP quote libraries are unavailable: " << load_error;

    quote3_error_t target_info_status = SGX_QL_SUCCESS;
    quote3_error_t quote_size_status = SGX_QL_SUCCESS;
    quote3_error_t get_quote_status = SGX_QL_SUCCESS;
    quote3_error_t supplemental_data_size_status = SGX_QL_SUCCESS;
    quote3_error_t verify_quote_status = SGX_QL_SUCCESS;
    sgx_ql_qv_result_t quote_verification_result = SGX_QL_QV_RESULT_UNSPECIFIED;
    uint32_t collateral_expiration_status = 0U;
    bool enclave_report_requested = false;
    bool enclave_report_data_matched = false;

    canopy::security::attestation::sgx_dcap_host_quote_provider_options provider_options;
    provider_options.functions.get_target_info
        = [&]() -> std::optional<canopy::security::attestation::sgx_dcap_target_info_result>
    {
        sgx_target_info_t target_info{};
        target_info_status = psw->get_target_info(&target_info);
        if (target_info_status != SGX_QL_SUCCESS)
            return std::nullopt;

        canopy::security::attestation::sgx_dcap_target_info_result result;
        result.qe_target_info = copy_dcap_bytes(target_info);
        return result;
    };

    provider_options.functions.get_quote_size = [&]() -> std::optional<uint32_t>
    {
        uint32_t quote_size = 0U;
        quote_size_status = psw->get_quote_size(&quote_size);
        if (quote_size_status != SGX_QL_SUCCESS)
            return std::nullopt;
        return quote_size;
    };

    provider_options.report_producer
        = [&](const canopy::security::attestation::sgx_dcap_report_request& request) -> std::optional<std::vector<uint8_t>>
    {
        enclave_report_requested = true;
        std::vector<uint8_t> report;
        const auto error
            = SYNC_WAIT(test_a_->sgx_dcap_make_quote_report(request.qe_target_info, request.report_data_sha256, report));
        if (error != rpc::error::OK())
            return std::nullopt;

        enclave_report_data_matched = dcap_report_data_matches(report, request.report_data_sha256);
        return report;
    };

    provider_options.functions.get_quote
        = [&](const canopy::security::attestation::sgx_dcap_get_quote_request& request) -> std::optional<std::vector<uint8_t>>
    {
        if (request.report.size() != sizeof(sgx_report_t) || request.quote_size == 0)
        {
            get_quote_status = SGX_QL_ERROR_INVALID_PARAMETER;
            return std::nullopt;
        }

        sgx_report_t report{};
        std::memcpy(&report, request.report.data(), sizeof(report));

        std::vector<uint8_t> quote(request.quote_size);
        get_quote_status = psw->get_quote(&report, request.quote_size, quote.data());
        if (get_quote_status != SGX_QL_SUCCESS)
            return std::nullopt;
        return quote;
    };

    canopy::security::attestation::sgx_dcap_host_quote_verifier_options verifier_options;
    verifier_options.verify_quote
        = [&](const canopy::security::attestation::sgx_dcap_verifier_input& input,
              const canopy::security::attestation::attestation_policy&) -> canopy::security::attestation::sgx_dcap_host_quote_verification
    {
        canopy::security::attestation::sgx_dcap_host_quote_verification verification;

        uint32_t supplemental_data_size = 0U;
        supplemental_data_size_status = psw->get_supplemental_data_size(&supplemental_data_size);
        if (supplemental_data_size_status != SGX_QL_SUCCESS)
        {
            verification.failure_reason = "sgx_qv_get_quote_supplemental_data_size failed with "
                                          + quote3_status_to_hex(supplemental_data_size_status);
            return verification;
        }

        std::vector<uint8_t> supplemental_data(supplemental_data_size);
        collateral_expiration_status = 0U;
        quote_verification_result = SGX_QL_QV_RESULT_UNSPECIFIED;
        verify_quote_status = psw->verify_quote(
            input.quote.quote.data(),
            static_cast<uint32_t>(input.quote.quote.size()),
            nullptr,
            std::time(nullptr),
            &collateral_expiration_status,
            &quote_verification_result,
            nullptr,
            supplemental_data_size,
            supplemental_data.empty() ? nullptr : supplemental_data.data());

        verification.call_succeeded = verify_quote_status == SGX_QL_SUCCESS;
        verification.result.quote_verification_result = static_cast<uint64_t>(quote_verification_result);
        verification.result.quote_verification_result_name = qv_result_name(quote_verification_result);
        verification.result.collateral_expired = static_cast<uint8_t>(collateral_expiration_status != 0U);
        verification.result.verification_time = static_cast<uint64_t>(std::time(nullptr));
        verification.result.supplemental_data = std::move(supplemental_data);
        if (!verification.call_succeeded)
        {
            verification.failure_reason = "sgx_qv_verify_quote failed with " + quote3_status_to_hex(verify_quote_status);
        }
        return verification;
    };

    auto provider
        = std::make_shared<canopy::security::attestation::sgx_dcap_host_quote_provider>(std::move(provider_options));
    auto verifier
        = std::make_shared<canopy::security::attestation::sgx_dcap_host_quote_verifier>(std::move(verifier_options));
    canopy::security::attestation::sgx_dcap_backend backend(std::move(provider), std::move(verifier));

    const auto binding = test_dcap_binding();
    auto evidence = backend.produce_evidence(binding);
    if (evidence.content_format != canopy::security::attestation::sgx_dcap_quote_evidence_content_format)
    {
        const auto failure_status = target_info_status != SGX_QL_SUCCESS
                                        ? target_info_status
                                        : (quote_size_status != SGX_QL_SUCCESS ? quote_size_status : get_quote_status);
        if (is_environmental_dcap_status(failure_status))
        {
            GTEST_SKIP() << "Intel DCAP quote generation is unavailable in this environment: "
                         << quote3_status_to_hex(failure_status);
        }

        FAIL() << "DCAP quote provider failed; target_info=" << quote3_status_to_hex(target_info_status)
               << " quote_size=" << quote3_status_to_hex(quote_size_status)
               << " get_quote=" << quote3_status_to_hex(get_quote_status)
               << " enclave_report_requested=" << enclave_report_requested
               << " report_data_matched=" << enclave_report_data_matched;
    }

    EXPECT_TRUE(enclave_report_requested);
    EXPECT_TRUE(enclave_report_data_matched);
    EXPECT_GT(evidence.payload.size(), 0U);

    auto verdict = backend.verify_evidence(evidence, binding, test_dcap_policy());
    if (!verdict.accepted && is_environmental_dcap_status(verify_quote_status))
    {
        GTEST_SKIP() << "Intel DCAP quote verification is unavailable in this environment: "
                     << quote3_status_to_hex(verify_quote_status);
    }

    EXPECT_TRUE(verdict.accepted) << verdict.reason << " verify_quote=" << quote3_status_to_hex(verify_quote_status)
                                  << " qv_result=" << qv_result_name(quote_verification_result)
                                  << " collateral_expired=" << collateral_expiration_status
                                  << " supplemental_data_size_status="
                                  << quote3_status_to_hex(supplemental_data_size_status);
    EXPECT_EQ(quote_verification_result, SGX_QL_QV_RESULT_OK);
    EXPECT_EQ(collateral_expiration_status, 0U);
#else
    GTEST_SKIP() << "SGX DCAP PSW test is only available in Linux SGX DCAP builds";
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

TEST_F(
    sgx_attestation_test_host_fixture,
    sgx_epid_host_quote_provider_uses_psw_and_enclave_report)
{
#if CANOPY_ATTESTATION_TEST_HAS_EPID_PSW
    std::string load_error;
    auto psw = load_epid_psw_library(load_error);
    if (!psw)
        GTEST_SKIP() << "Intel EPID PSW library is unavailable: " << load_error;

    sgx_status_t init_quote_status = SGX_SUCCESS;
    sgx_status_t calculate_quote_size_status = SGX_SUCCESS;
    sgx_status_t get_quote_status = SGX_SUCCESS;
    bool enclave_report_requested = false;
    bool enclave_report_data_matched = false;

    canopy::security::attestation::sgx_epid_host_quote_provider_options options;
    options.spid.resize(epid_spid_size);
    options.quote_sign_type = SGX_UNLINKABLE_SIGNATURE;

    options.functions.init_quote = [&]() -> std::optional<canopy::security::attestation::sgx_epid_init_quote_result>
    {
        sgx_target_info_t target_info{};
        sgx_epid_group_id_t epid_group_id{};
        init_quote_status = psw->init_quote(&target_info, &epid_group_id);
        if (init_quote_status != SGX_SUCCESS)
            return std::nullopt;

        canopy::security::attestation::sgx_epid_init_quote_result result;
        result.qe_target_info = copy_bytes(target_info);
        std::memcpy(
            &result.extended_epid_group_id,
            &epid_group_id,
            std::min(sizeof(result.extended_epid_group_id), sizeof(epid_group_id)));
        return result;
    };

    options.functions.calculate_quote_size = [&](const std::vector<uint8_t>& sig_rl) -> std::optional<uint32_t>
    {
        uint32_t quote_size = 0;
        const auto* sig_rl_data = sig_rl.empty() ? nullptr : sig_rl.data();
        calculate_quote_size_status = psw->calc_quote_size(sig_rl_data, static_cast<uint32_t>(sig_rl.size()), &quote_size);
        if (calculate_quote_size_status != SGX_SUCCESS)
            return std::nullopt;
        return quote_size;
    };

    options.report_producer
        = [&](const canopy::security::attestation::sgx_epid_report_request& request) -> std::optional<std::vector<uint8_t>>
    {
        enclave_report_requested = true;
        std::vector<uint8_t> report;
        const auto error
            = SYNC_WAIT(test_a_->sgx_epid_make_quote_report(request.qe_target_info, request.report_data_sha256, report));
        if (error != rpc::error::OK())
            return std::nullopt;

        enclave_report_data_matched = report_data_matches(report, request.report_data_sha256);
        return report;
    };

    options.functions.get_quote
        = [&](const canopy::security::attestation::sgx_epid_get_quote_request& request) -> std::optional<std::vector<uint8_t>>
    {
        if (request.report.size() != sizeof(sgx_report_t) || request.spid.size() != epid_spid_size
            || request.quote_size == 0)
        {
            get_quote_status = SGX_ERROR_INVALID_PARAMETER;
            return std::nullopt;
        }

        sgx_report_t report{};
        std::memcpy(&report, request.report.data(), sizeof(report));

        sgx_spid_t spid{};
        std::memcpy(spid.id, request.spid.data(), sizeof(spid.id));

        sgx_quote_nonce_t nonce{};
        std::vector<uint8_t> quote(request.quote_size);
        const auto* sig_rl_data = request.sig_rl.empty() ? nullptr : request.sig_rl.data();
        get_quote_status = psw->get_quote(
            &report,
            static_cast<sgx_quote_sign_type_t>(request.quote_sign_type),
            &spid,
            &nonce,
            sig_rl_data,
            static_cast<uint32_t>(request.sig_rl.size()),
            nullptr,
            reinterpret_cast<sgx_quote_t*>(quote.data()),
            static_cast<uint32_t>(quote.size()));
        if (get_quote_status != SGX_SUCCESS)
            return std::nullopt;
        return quote;
    };

    canopy::security::attestation::sgx_epid_host_quote_provider provider(std::move(options));
    canopy::security::attestation::sgx_epid_quote_request request;
    request.binding = test_epid_binding();
    request.report_data_sha256 = test_report_data_hash();

    auto material = provider.produce_quote(request);
    if (!material.has_value())
    {
        const auto failure_status
            = init_quote_status != SGX_SUCCESS
                  ? init_quote_status
                  : (calculate_quote_size_status != SGX_SUCCESS ? calculate_quote_size_status : get_quote_status);
        if (is_environmental_epid_status(failure_status))
        {
            GTEST_SKIP() << "Intel EPID PSW did not provide quote material in this environment: "
                         << sgx_status_to_hex(failure_status);
        }

        FAIL() << "EPID quote provider failed; init=" << sgx_status_to_hex(init_quote_status)
               << " calc=" << sgx_status_to_hex(calculate_quote_size_status)
               << " get_quote=" << sgx_status_to_hex(get_quote_status)
               << " enclave_report_requested=" << enclave_report_requested
               << " report_data_matched=" << enclave_report_data_matched;
    }

    EXPECT_TRUE(enclave_report_requested);
    EXPECT_TRUE(enclave_report_data_matched);
    EXPECT_EQ(material->quote_sign_type, SGX_UNLINKABLE_SIGNATURE);
    EXPECT_EQ(material->spid.size(), epid_spid_size);
    EXPECT_GT(material->quote.size(), 0U);
#else
    GTEST_SKIP() << "SGX EPID PSW test is only available in Linux SGX EPID builds";
#endif
}
