/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <transports/sgx_coroutine/common/startup_status.h>
#include <transports/sgx_coroutine/common/io_uring_data_conversion.h>
#include <transports/sgx_coroutine/common/shared_queue.h>
#include <transports/sgx_coroutine/enclave/runtime.h>
#include <transports/sgx_coroutine/enclave/host_transport.h>
#include <edl/coroutine_enclave.h>
#include <trusted/canopy_coroutine_enclave_t.h>
#include <sgx_error.h>
#include <sgx_trts.h>
#include <cstring>
#include <rpc/rpc.h>
#ifdef CANOPY_USE_TELEMETRY
#  include <rpc/telemetry/i_telemetry_service.h>
#  include <rpc/telemetry/telemetry_service_factory.h>
#endif
#include <streaming/stream_transport.h>
#include <streaming/spsc_queue/stream.h>
#include <transports/streaming/transport.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <tuple>
#include <vector>

namespace rpc::sgx::coro::enclave
{
    host_transport::host_transport(
        std::string name,
        std::shared_ptr<rpc::service> service,
        std::shared_ptr<streaming::stream> stream,
        connection_handler handler,
        rpc::stream_transport::stream_transport_options options)
        : rpc::stream_transport::transport(
              std::move(name),
              std::move(service),
              std::move(stream),
              std::move(handler),
              options)
    {
    }

    host_transport::~host_transport()
    {
        if (runtime_destroyed_handler_)
            runtime_destroyed_handler_();
    }

    void host_transport::set_runtime_destroyed_handler(std::function<void()> handler)
    {
        runtime_destroyed_handler_ = std::move(handler);
    }

    std::shared_ptr<host_transport> host_transport::create(
        std::string name,
        std::shared_ptr<rpc::service> service,
        std::shared_ptr<streaming::stream> stream,
        connection_handler handler,
        rpc::stream_transport::stream_transport_options options)
    {
        auto transport = std::shared_ptr<host_transport>(
            new host_transport(std::move(name), std::move(service), std::move(stream), std::move(handler), options));
        transport->initialise_after_construction();
        return transport;
    }

    CORO_TASK(int)
    host_transport::retain_io_uring_control_reference(
        const rpc::shared_ptr<rpc::sgx::coro::protocol::i_io_uring_control>& control)
    {
        {
            std::lock_guard<std::mutex> lock(host_control_reference_mutex_);
            if (host_control_release_)
                CO_RETURN rpc::error::OK();
        }

        if (!control || control->__rpc_is_local())
            CO_RETURN rpc::error::INVALID_DATA();

        auto object_proxy = control->__rpc_get_object_proxy();
        if (!object_proxy)
            CO_RETURN rpc::error::INVALID_DATA();

        auto service_proxy = object_proxy->get_service_proxy();
        if (!service_proxy)
            CO_RETURN rpc::error::INVALID_DATA();

        auto transport = service_proxy->get_transport();
        if (!transport)
            CO_RETURN rpc::error::TRANSPORT_ERROR();

        if (transport.get() != this)
            CO_RETURN rpc::error::INVALID_DATA();

        auto remote_object = service_proxy->get_destination_zone_id().with_object(object_proxy->get_object_id());
        if (!remote_object)
            CO_RETURN rpc::error::INVALID_DATA();

        auto local_service = get_service();
        if (!local_service)
            CO_RETURN rpc::error::TRANSPORT_ERROR();

        auto payload_encoding = local_service->get_default_encoding();

        rpc::add_ref_params params;
        params.protocol_version = service_proxy->get_remote_rpc_version();
        params.remote_object_id = *remote_object;
        params.caller_zone_id = get_zone_id();
        params.requesting_zone_id = get_zone_id();
        params.build_out_param_channel = rpc::add_ref_options::normal;
        params.payload_encoding = payload_encoding;

        auto add_ref_result = CO_AWAIT outbound_add_ref(std::move(params));
        if (add_ref_result.error_code != rpc::error::OK())
            CO_RETURN add_ref_result.error_code;

        {
            std::lock_guard<std::mutex> lock(host_control_reference_mutex_);
            host_control_reference_ = host_control_reference{
                .protocol_version = service_proxy->get_remote_rpc_version(),
                .encoding = payload_encoding,
                .remote_object_id = *remote_object,
                .caller_zone_id = get_zone_id(),
            };
            host_control_release_ = rpc::release_params{.protocol_version = service_proxy->get_remote_rpc_version(),
                .remote_object_id = *remote_object,
                .caller_zone_id = get_zone_id(),
                .options = rpc::release_options::normal,
                .in_back_channel = {},
                .payload_type_id = 0,
                .payload_encoding = payload_encoding,
                .payload = {}};
        }

        CO_RETURN rpc::error::OK();
    }

    CORO_TASK(int)
    host_transport::wake_host_iouring()
    {
        std::optional<host_control_reference> control_reference;
        {
            std::lock_guard<std::mutex> lock(host_control_reference_mutex_);
            control_reference = host_control_reference_;
        }
        if (!control_reference)
            CO_RETURN rpc::error::INVALID_DATA();

        std::vector<char> in_buf;
        auto ret
            = rpc::sgx::coro::protocol::i_io_uring_control::proxy_serialiser<rpc::serialiser::yas, rpc::encoding>::wake_iouring(
                in_buf, control_reference->encoding);
        if (rpc::error::is_error(ret))
            CO_RETURN ret;

        auto send_result = CO_AWAIT outbound_send(
            rpc::send_params{
                .protocol_version = control_reference->protocol_version,
                .encoding_type = control_reference->encoding,
                .tag = 0,
                .caller_zone_id = control_reference->caller_zone_id,
                .remote_object_id = control_reference->remote_object_id,
                .interface_id = rpc::sgx::coro::protocol::i_io_uring_control::get_id(control_reference->protocol_version),
                .method_id = {2},
                .in_data = std::move(in_buf),
                .in_back_channel = {},
                .request_id = 0,
            });
        ret = send_result.error_code;
        if (ret == rpc::error::OBJECT_GONE() || rpc::error::is_critical(ret))
            CO_RETURN ret;

        CO_RETURN rpc::sgx::coro::protocol::i_io_uring_control::proxy_deserialiser<rpc::serialiser::yas, rpc::encoding>::wake_iouring(
            send_result.out_buf, control_reference->encoding);
    }

    CORO_TASK(int)
    host_transport::get_iouring_data(rpc::io_uring::data& ring_data)
    {
        std::optional<host_control_reference> control_reference;
        {
            std::lock_guard<std::mutex> lock(host_control_reference_mutex_);
            control_reference = host_control_reference_;
        }
        if (!control_reference)
            CO_RETURN rpc::error::INVALID_DATA();

        std::vector<char> in_buf;
        auto ret
            = rpc::sgx::coro::protocol::i_io_uring_control::proxy_serialiser<rpc::serialiser::yas, rpc::encoding>::get_iouring_data(
                in_buf, control_reference->encoding);
        if (rpc::error::is_error(ret))
            CO_RETURN ret;

        auto send_result = CO_AWAIT outbound_send(
            rpc::send_params{
                .protocol_version = control_reference->protocol_version,
                .encoding_type = control_reference->encoding,
                .tag = 0,
                .caller_zone_id = control_reference->caller_zone_id,
                .remote_object_id = control_reference->remote_object_id,
                .interface_id = rpc::sgx::coro::protocol::i_io_uring_control::get_id(control_reference->protocol_version),
                .method_id = {3},
                .in_data = std::move(in_buf),
                .in_back_channel = {},
                .request_id = 0,
            });
        ret = send_result.error_code;
        if (ret == rpc::error::OBJECT_GONE() || rpc::error::is_critical(ret))
            CO_RETURN ret;

        rpc::sgx::coro::protocol::io_uring_data wire_data;
        ret = rpc::sgx::coro::protocol::i_io_uring_control::proxy_deserialiser<rpc::serialiser::yas, rpc::encoding>::get_iouring_data(
            wire_data, send_result.out_buf, control_reference->encoding);
        if (ret == rpc::error::OK())
            rpc::sgx::coro::protocol::copy_to_native(wire_data, ring_data);
        CO_RETURN ret;
    }

    int host_transport::release_io_uring_control_reference()
    {
        std::optional<rpc::release_params> release_params;
        {
            std::lock_guard<std::mutex> lock(host_control_reference_mutex_);
            host_control_reference_.reset();
            release_params.swap(host_control_release_);
        }

        if (!release_params)
            return rpc::error::OK();

        // The retained shared reference exists only to keep the host-side
        // io_uring control object alive while enclave code uses an optimistic
        // pointer. This is part of transport-private shutdown: even if the
        // public transport status is already DISCONNECTED, the enclave stream
        // may still be draining to the host, so enqueue the release directly
        // instead of going through outbound_release/status checks.
        send_payload_release_send(
            release_params->protocol_version,
            rpc::stream_transport::message_direction::one_way,
            rpc::stream_transport::release_send{.destination_zone_id = release_params->remote_object_id,
                .caller_zone_id = release_params->caller_zone_id,
                .options = release_params->options,
                .back_channel = std::move(release_params->in_back_channel),
                .payload_type_id = release_params->payload_type_id,
                .payload_encoding = release_params->payload_encoding,
                .payload = std::move(release_params->payload)},
            0);

        return rpc::error::OK();
    }

    void host_transport::on_disconnecting()
    {
        auto release_error = release_io_uring_control_reference();
        if (release_error != rpc::error::OK())
            RPC_WARNING("failed to release retained host io_uring control reference: {}", release_error);
    }
}
