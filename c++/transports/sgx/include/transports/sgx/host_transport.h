/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#ifndef CANOPY_BUILD_COROUTINE

#  include <memory>
#  include <string>

#  include <rpc/rpc.h>

namespace rpc::sgx
{
    // Enclave-side SGX transport. This is the transport-shaped replacement for
    // the old host_service_proxy path. It lives inside the enclave and calls
    // back to the host via OCALLs.
    class host_transport : public rpc::transport
    {
        uint64_t enclave_id_ = 0;

    public:
        host_transport(
            std::string name,
            uint64_t enclave_id,
            rpc::zone enclave_zone,
            rpc::zone host_zone);

        ~host_transport() override CANOPY_DEFAULT_DESTRUCTOR;

        void set_status(rpc::transport_status status) override;

        CORO_TASK(rpc::connect_result)
        inner_connect(
            std::shared_ptr<rpc::object_stub> stub,
            rpc::connection_settings input_descr) override;

        CORO_TASK(int) inner_accept() override { CO_RETURN rpc::error::OK(); }

        CORO_TASK(rpc::send_result) outbound_send(rpc::send_params params) override;
        CORO_TASK(void) outbound_post(rpc::post_params params) override;
        CORO_TASK(rpc::standard_result) outbound_try_cast(rpc::try_cast_params params) override;
        CORO_TASK(rpc::standard_result) outbound_add_ref(rpc::add_ref_params params) override;
        CORO_TASK(rpc::standard_result) outbound_release(rpc::release_params params) override;
        CORO_TASK(void) outbound_object_released(rpc::object_released_params params) override;
        CORO_TASK(void) outbound_transport_down(rpc::transport_down_params params) override;
        CORO_TASK(rpc::new_zone_id_result) outbound_get_new_zone_id(rpc::get_new_zone_id_params params) override;
    };
}

#endif
