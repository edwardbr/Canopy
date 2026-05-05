/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#ifdef CANOPY_BUILD_ENCLAVE

#  include "test_host.h"
#  include "test_globals.h"
#  include <common/tests.h>
#  include <common/transport_setup_base.h>
#  include <gtest/gtest.h>
#  include <memory>
#  include <new>
#  include <transports/sgx_coroutine/enclave/transport.h>
#  include <utility>
#  include <vector>

namespace sgx_coroutine_setup_detail
{
    template<class HostImplementation>
    rpc::service_connect_result<yyy::i_host> create_host_for_setup(std::shared_ptr<coro::scheduler> scheduler)
    {
        if constexpr (requires { HostImplementation::create_for_test(scheduler); })
        {
            return HostImplementation::create_for_test(std::move(scheduler));
        }
        else
        {
            try
            {
                rpc::shared_ptr<HostImplementation> host_ptr(new HostImplementation());
                return {rpc::error::OK(), rpc::static_pointer_cast<yyy::i_host>(host_ptr)};
            }
            catch (const std::bad_alloc&)
            {
                return {rpc::error::OUT_OF_MEMORY(), {}};
            }
            catch (...)
            {
                return {rpc::error::EXCEPTION(), {}};
            }
        }
    }
} // namespace sgx_coroutine_setup_detail

template<bool UseHostInChild, bool RunStandardTests, bool CreateNewZoneThenCreateSubordinatedZone, class HostImplementation = host>
class sgx_coroutine_setup
    : public transport_setup_base<UseHostInChild, RunStandardTests, CreateNewZoneThenCreateSubordinatedZone>
{
    std::vector<std::weak_ptr<rpc::sgx::coro::enclave::transport>> transports_;

    [[nodiscard]] bool all_transports_expired() const
    {
        for (const auto& transport : transports_)
        {
            if (!transport.expired())
                return false;
        }
        return true;
    }

public:
    ~sgx_coroutine_setup() override = default;

    bool is_sgx_setup() const { return true; }

    void set_up()
    {
        this->start_telemetry_test();

        this->io_scheduler_ = std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
            coro::scheduler::options{
                .thread_strategy = coro::scheduler::thread_strategy_t::manual,
                .pool = coro::thread_pool::options{.thread_count = 1},
            }));
        this->root_service_ = rpc::root_service::create("host", rpc::DEFAULT_PREFIX, this->io_scheduler_);
        current_host_service = this->root_service_;

        auto host_result = sgx_coroutine_setup_detail::create_host_for_setup<HostImplementation>(this->io_scheduler_);
        RPC_ASSERT(host_result.error_code == rpc::error::OK());
        this->i_host_ptr_ = std::move(host_result.output_interface);
        this->local_host_ptr_ = this->i_host_ptr_;

        auto host_ptr = this->use_host_in_child_ ? this->i_host_ptr_ : nullptr;
        auto transport = std::make_shared<rpc::sgx::coro::enclave::transport>(
            "main child", this->root_service_, coroutine_enclave_path);
        transports_.push_back(transport);
        auto result = SYNC_WAIT((this->root_service_->template connect_to_zone<yyy::i_host, yyy::i_example>(
            "main child", transport, host_ptr)));

        this->i_example_ptr_ = std::move(result.output_interface);
        RPC_ASSERT(result.error_code == rpc::error::OK());
    }

    void tear_down()
    {
        auto scheduler = this->io_scheduler_;
        auto service_shutdown_event = this->make_root_shutdown_event_for_test();
        this->release_interfaces_and_root_service_for_test(service_shutdown_event);

        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{5000};
        while (!all_transports_expired() && std::chrono::steady_clock::now() < deadline)
        {
            scheduler->process_events(std::chrono::milliseconds{1});
        }
        RPC_ASSERT(all_transports_expired());
        transports_.clear();

        scheduler->shutdown();

        this->io_scheduler_ = nullptr;
        scheduler.reset();
        this->reset_telemetry_for_test();
    }

    CORO_TASK(rpc::shared_ptr<yyy::i_example>) create_new_zone()
    {
        rpc::shared_ptr<yyy::i_example> ptr;
        rpc::get_new_zone_id_params zone_params{};
        zone_params.protocol_version = rpc::get_version();
        auto zone_result = CO_AWAIT this->root_service_->get_new_zone_id(zone_params);
        if (zone_result.error_code != rpc::error::OK())
            CO_RETURN nullptr;

        auto transport = std::make_shared<rpc::sgx::coro::enclave::transport>(
            "main child", this->root_service_, coroutine_enclave_path);
        transports_.push_back(transport);
        transport->set_adjacent_zone_id(zone_result.zone_id);
        auto result = CO_AWAIT this->root_service_->template connect_to_zone<yyy::i_host, yyy::i_example>(
            "main child", transport, this->use_host_in_child_ ? this->i_host_ptr_ : nullptr);

        ptr = std::move(result.output_interface);
        if (result.error_code != rpc::error::OK())
            CO_RETURN nullptr;
        if (CreateNewZoneThenCreateSubordinatedZone)
        {
            rpc::shared_ptr<yyy::i_example> new_ptr;
            auto err_code = CO_AWAIT ptr->create_example_in_subordinate_zone(
                new_ptr, this->use_host_in_child_ ? this->i_host_ptr_ : nullptr);
            if (err_code != rpc::error::OK())
                CO_RETURN nullptr;
            ptr = new_ptr;
        }
        CO_RETURN ptr;
    }
};

#endif
