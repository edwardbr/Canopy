/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <transports/sgx/host_transport.h>

#ifdef FOR_SGX

#  include <edl/enclave_marshal_test.h>
#  include <trusted/enclave_marshal_test_t.h>

#  ifdef CANOPY_USE_TELEMETRY
#    include <rpc/telemetry/i_telemetry_service.h>
#  endif

namespace rpc::sgx
{
    namespace
    {
        using namespace rpc::sgx::marshal_test;

        template<typename T> std::vector<char> to_sgx_blob(const T& value)
        {
            return rpc::to_yas_binary<std::vector<char>>(value);
        }

        template<typename T>
        int from_sgx_blob(
            const char* data,
            size_t size,
            T& value)
        {
            rpc::byte_span span{reinterpret_cast<const uint8_t*>(data), size};
            auto err = rpc::from_yas_binary(span, value);
            if (!err.empty())
            {
                RPC_ERROR("SGX blob decode failed: {}", err);
                return rpc::error::INVALID_DATA();
            }
            return rpc::error::OK();
        }

        void log_sgx_failure(
            const char* call_name,
            int status)
        {
#  ifdef CANOPY_USE_TELEMETRY
            if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
                telemetry_service->message(rpc::i_telemetry_service::err, call_name);
#  endif
            RPC_ERROR("{} failed {}", call_name, status);
        }

        send_request to_sgx_request(const rpc::send_params& params)
        {
            return send_request{params.protocol_version,
                params.encoding_type,
                params.tag,
                params.caller_zone_id,
                params.remote_object_id,
                params.interface_id,
                params.method_id,
                params.in_data,
                params.in_back_channel};
        }

        post_request to_sgx_request(const rpc::post_params& params)
        {
            return post_request{params.protocol_version,
                params.encoding_type,
                params.tag,
                params.caller_zone_id,
                params.remote_object_id,
                params.interface_id,
                params.method_id,
                params.in_data,
                params.in_back_channel};
        }

        try_cast_request to_sgx_request(const rpc::try_cast_params& params)
        {
            return try_cast_request{params.protocol_version,
                params.caller_zone_id,
                params.remote_object_id,
                params.interface_id,
                params.in_back_channel};
        }

        add_ref_request to_sgx_request(const rpc::add_ref_params& params)
        {
            return add_ref_request{params.protocol_version,
                params.remote_object_id,
                params.caller_zone_id,
                params.requesting_zone_id,
                params.build_out_param_channel,
                params.in_back_channel};
        }

        release_request to_sgx_request(const rpc::release_params& params)
        {
            return release_request{
                params.protocol_version, params.remote_object_id, params.caller_zone_id, params.options, params.in_back_channel};
        }

        object_released_request to_sgx_request(const rpc::object_released_params& params)
        {
            return object_released_request{
                params.protocol_version, params.remote_object_id, params.caller_zone_id, params.in_back_channel};
        }

        transport_down_request to_sgx_request(const rpc::transport_down_params& params)
        {
            return transport_down_request{
                params.protocol_version, params.destination_zone_id, params.caller_zone_id, params.in_back_channel};
        }

        get_new_zone_id_request to_sgx_request(
            const rpc::get_new_zone_id_params& params,
            rpc::caller_zone caller_zone_id)
        {
            return get_new_zone_id_request{params.protocol_version, caller_zone_id, params.in_back_channel};
        }

        rpc::send_result from_sgx_response(const send_response& response)
        {
            return rpc::send_result{response.error_code, response.out_data, response.out_back_channel};
        }

        rpc::standard_result from_sgx_response(const standard_response& response)
        {
            return rpc::standard_result{response.error_code, response.out_back_channel};
        }

        rpc::new_zone_id_result from_sgx_response(const new_zone_id_response& response)
        {
            return rpc::new_zone_id_result{response.error_code, response.zone_id, response.out_back_channel};
        }
    }

    host_transport::host_transport(
        std::string name,
        uint64_t enclave_id,
        rpc::zone enclave_zone,
        rpc::zone host_zone)
        : rpc::transport(
              std::move(name),
              enclave_zone)
        , enclave_id_(enclave_id)
    {
        set_adjacent_zone_id(host_zone);
        set_status(rpc::transport_status::CONNECTED);
    }

    void host_transport::set_status(rpc::transport_status status)
    {
        rpc::transport::set_status(status);

        if (status == rpc::transport_status::DISCONNECTED)
            notify_all_destinations_of_disconnect();
    }

    CORO_TASK(rpc::connect_result)
    host_transport::inner_connect(
        std::shared_ptr<rpc::object_stub> stub,
        rpc::connection_settings input_descr)
    {
        std::ignore = stub;
        std::ignore = input_descr;
        CO_RETURN rpc::connect_result{rpc::error::ZONE_NOT_SUPPORTED(), {}};
    }

    CORO_TASK(rpc::send_result)
    host_transport::outbound_send(rpc::send_params params)
    {
        auto req = to_sgx_blob(to_sgx_request(params));
        std::vector<char> resp(4096);

        int err_code = rpc::error::OK();
        size_t resp_sz = 0;
        auto status = ::call_host(&err_code, enclave_id_, req.size(), req.data(), resp.size(), resp.data(), &resp_sz);
        if (status)
        {
            log_sgx_failure("call_host", status);
            CO_RETURN rpc::send_result{rpc::error::TRANSPORT_ERROR(), {}, {}};
        }

        if (err_code == rpc::error::NEED_MORE_MEMORY())
        {
            resp.resize(resp_sz);
            status = ::call_host(&err_code, enclave_id_, req.size(), req.data(), resp.size(), resp.data(), &resp_sz);
            if (status)
            {
                log_sgx_failure("call_host", status);
                CO_RETURN rpc::send_result{rpc::error::TRANSPORT_ERROR(), {}, {}};
            }
        }

        if (err_code != rpc::error::OK())
            CO_RETURN rpc::send_result{err_code, {}, {}};

        send_response result;
        err_code = from_sgx_blob(resp.data(), resp_sz, result);
        if (err_code != rpc::error::OK())
            CO_RETURN rpc::send_result{err_code, {}, {}};

        CO_RETURN from_sgx_response(result);
    }

    CORO_TASK(void)
    host_transport::outbound_post(rpc::post_params params)
    {
        if (params.remote_object_id.as_zone() != get_adjacent_zone_id())
            CO_RETURN;

        auto req = to_sgx_blob(to_sgx_request(params));
        int err_code = rpc::error::OK();
        auto status = ::post_host(&err_code, enclave_id_, req.size(), req.data());
        if (status)
            log_sgx_failure("post_host", status);
        CO_RETURN;
    }

    CORO_TASK(rpc::standard_result)
    host_transport::outbound_try_cast(rpc::try_cast_params params)
    {
        auto req = to_sgx_blob(to_sgx_request(params));
        std::vector<char> resp(1024);
        int err_code = rpc::error::OK();
        size_t resp_sz = 0;

        auto status = ::try_cast_host(&err_code, enclave_id_, req.size(), req.data(), resp.size(), resp.data(), &resp_sz);
        if (status)
        {
            log_sgx_failure("try_cast_host", status);
            CO_RETURN rpc::standard_result{rpc::error::TRANSPORT_ERROR(), {}};
        }

        if (err_code == rpc::error::NEED_MORE_MEMORY())
        {
            resp.resize(resp_sz);
            status = ::try_cast_host(&err_code, enclave_id_, req.size(), req.data(), resp.size(), resp.data(), &resp_sz);
            if (status)
            {
                log_sgx_failure("try_cast_host", status);
                CO_RETURN rpc::standard_result{rpc::error::TRANSPORT_ERROR(), {}};
            }
        }

        if (err_code != rpc::error::OK())
            CO_RETURN rpc::standard_result{err_code, {}};

        standard_response result;
        err_code = from_sgx_blob(resp.data(), resp_sz, result);
        if (err_code != rpc::error::OK())
            CO_RETURN rpc::standard_result{err_code, {}};

        CO_RETURN from_sgx_response(result);
    }

    CORO_TASK(rpc::standard_result)
    host_transport::outbound_add_ref(rpc::add_ref_params params)
    {
        auto req = to_sgx_blob(to_sgx_request(params));
        std::vector<char> resp(1024);
        int err_code = rpc::error::OK();
        size_t resp_sz = 0;

        auto status = ::add_ref_host(&err_code, enclave_id_, req.size(), req.data(), resp.size(), resp.data(), &resp_sz);
        if (status)
        {
            log_sgx_failure("add_ref_host", status);
            CO_RETURN rpc::standard_result{rpc::error::TRANSPORT_ERROR(), {}};
        }

        if (err_code == rpc::error::NEED_MORE_MEMORY())
        {
            resp.resize(resp_sz);
            status = ::add_ref_host(&err_code, enclave_id_, req.size(), req.data(), resp.size(), resp.data(), &resp_sz);
            if (status)
            {
                log_sgx_failure("add_ref_host", status);
                CO_RETURN rpc::standard_result{rpc::error::TRANSPORT_ERROR(), {}};
            }
        }

        if (err_code != rpc::error::OK())
            CO_RETURN rpc::standard_result{err_code, {}};

        standard_response result;
        err_code = from_sgx_blob(resp.data(), resp_sz, result);
        if (err_code != rpc::error::OK())
            CO_RETURN rpc::standard_result{err_code, {}};

        CO_RETURN from_sgx_response(result);
    }

    CORO_TASK(rpc::standard_result)
    host_transport::outbound_release(rpc::release_params params)
    {
        auto req = to_sgx_blob(to_sgx_request(params));
        std::vector<char> resp(1024);
        int err_code = rpc::error::OK();
        size_t resp_sz = 0;

        auto status = ::release_host(&err_code, enclave_id_, req.size(), req.data(), resp.size(), resp.data(), &resp_sz);
        if (status)
        {
            log_sgx_failure("release_host", status);
            CO_RETURN rpc::standard_result{rpc::error::TRANSPORT_ERROR(), {}};
        }

        if (err_code == rpc::error::NEED_MORE_MEMORY())
        {
            resp.resize(resp_sz);
            status = ::release_host(&err_code, enclave_id_, req.size(), req.data(), resp.size(), resp.data(), &resp_sz);
            if (status)
            {
                log_sgx_failure("release_host", status);
                CO_RETURN rpc::standard_result{rpc::error::TRANSPORT_ERROR(), {}};
            }
        }

        if (err_code != rpc::error::OK())
            CO_RETURN rpc::standard_result{err_code, {}};

        standard_response result;
        err_code = from_sgx_blob(resp.data(), resp_sz, result);
        if (err_code != rpc::error::OK())
            CO_RETURN rpc::standard_result{err_code, {}};

        CO_RETURN from_sgx_response(result);
    }

    CORO_TASK(void)
    host_transport::outbound_object_released(rpc::object_released_params params)
    {
        auto req = to_sgx_blob(to_sgx_request(params));
        int err_code = rpc::error::OK();
        auto status = ::object_released_host(&err_code, enclave_id_, req.size(), req.data());
        if (status)
            log_sgx_failure("object_released_host", status);
        CO_RETURN;
    }

    CORO_TASK(void)
    host_transport::outbound_transport_down(rpc::transport_down_params params)
    {
        auto req = to_sgx_blob(to_sgx_request(params));
        int err_code = rpc::error::OK();
        auto status = ::transport_down_host(&err_code, enclave_id_, req.size(), req.data());
        if (status)
            log_sgx_failure("transport_down_host", status);
        CO_RETURN;
    }

    CORO_TASK(rpc::new_zone_id_result)
    host_transport::outbound_get_new_zone_id(rpc::get_new_zone_id_params params)
    {
        auto req = to_sgx_blob(to_sgx_request(params, get_zone_id()));
        std::vector<char> resp(1024);
        size_t resp_sz = 0;

        int err_code = rpc::error::OK();
        auto status
            = ::get_new_zone_id_host(&err_code, enclave_id_, req.size(), req.data(), resp.size(), resp.data(), &resp_sz);
        if (status)
        {
            log_sgx_failure("get_new_zone_id_host", status);
            CO_RETURN rpc::new_zone_id_result{rpc::error::TRANSPORT_ERROR(), {}, {}};
        }

        if (err_code == rpc::error::NEED_MORE_MEMORY())
        {
            resp.resize(resp_sz);
            status = ::get_new_zone_id_host(
                &err_code, enclave_id_, req.size(), req.data(), resp.size(), resp.data(), &resp_sz);
            if (status)
            {
                log_sgx_failure("get_new_zone_id_host", status);
                CO_RETURN rpc::new_zone_id_result{rpc::error::TRANSPORT_ERROR(), {}, {}};
            }
        }

        if (err_code != rpc::error::OK())
            CO_RETURN rpc::new_zone_id_result{err_code, {}, {}};

        new_zone_id_response result;
        err_code = from_sgx_blob(resp.data(), resp_sz, result);
        if (err_code != rpc::error::OK())
            CO_RETURN rpc::new_zone_id_result{err_code, {}, {}};

        CO_RETURN from_sgx_response(result);
    }
}

#endif
