/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#ifndef CANOPY_BUILD_COROUTINE

#  include <map>
#  include <memory>
#  include <string>
#  include <json/json_dom.h>
#  include <rpc/rpc.h>
#  include <sgx_blocking_transport/sgx_blocking_transport_config.h>

namespace rpc::sgx_blocking_transport
{
    // Host-side SGX transport. This is the transport-shaped replacement for the
    // old enclave_service_proxy path. It lives in the host zone and issues
    // ECALLs into an enclave-backed child zone.
    class enclave_transport : public rpc::transport
    {
        struct enclave_owner
        {
            explicit enclave_owner(uint64_t eid)
                : eid_(eid)
            {
            }

            uint64_t eid_ = 0;
            ~enclave_owner();
        };

        std::shared_ptr<enclave_owner> enclave_owner_;
        uint64_t eid_ = 0;
        const std::string enclave_path_;
        rpc::optional<rpc::sgx_enclave_runtime::runtime_settings> enclave_runtime_settings_;
        std::map<std::string, json::v1::object> enclave_startup_applications_;

    public:
        [[nodiscard]] static int validate_startup_settings(const transport_settings& settings);

        enclave_transport(
            std::string name,
            std::shared_ptr<rpc::service> service,
            std::string enclave_path);
        enclave_transport(
            std::string name,
            std::shared_ptr<rpc::service> service,
            transport_settings settings);

        ~enclave_transport() override CANOPY_DEFAULT_DESTRUCTOR;

        CORO_TASK(rpc::connect_result)
        inner_connect(
            std::shared_ptr<rpc::object_stub> stub,
            rpc::connection_settings input_descr) override;

        CORO_TASK(int) inner_accept() override { CO_RETURN rpc::error::OK(); }

        CORO_TASK(rpc::send_result) outbound_send(rpc::send_params params) override;
        CORO_TASK(void) outbound_post(rpc::post_params params) override;
        CORO_TASK(rpc::standard_result) outbound_try_cast(rpc::try_cast_params params) override;
        CORO_TASK(rpc::get_schema_result) outbound_get_schema(rpc::get_schema_params params) override;
        CORO_TASK(rpc::standard_result) outbound_add_ref(rpc::add_ref_params params) override;
        CORO_TASK(rpc::standard_result) outbound_release(rpc::release_params params) override;
        CORO_TASK(void) outbound_object_released(rpc::object_released_params params) override;
        CORO_TASK(void) outbound_transport_down(rpc::transport_down_params params) override;

        const std::string& get_enclave_path() const { return enclave_path_; }
        [[nodiscard]] rpc::optional<rpc::sgx_enclave_runtime::runtime_settings> get_enclave_runtime_startup_settings() const
        {
            return enclave_runtime_settings_;
        }
    };
}

#endif
