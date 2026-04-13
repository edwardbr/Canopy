/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <transports/sgx/transport.h>

#ifndef FOR_SGX

#  include <thread>

#  ifdef CANOPY_BUILD_ENCLAVE
#    include <sgx_capable.h>
#    include <sgx_urts.h>
#    include <edl/enclave_marshal_test.h>
#    include <rpc/rpc.h>
#    include <untrusted/enclave_marshal_test_u.h>

#    ifdef CANOPY_USE_TELEMETRY
#      include <rpc/telemetry/i_telemetry_service.h>
#    endif

namespace rpc::sgx
{
    namespace
    {
        using namespace rpc::sgx::marshal_test;
        std::mutex host_transport_registry_mutex;
        std::unordered_map<uint64_t, std::weak_ptr<enclave_transport>> host_transport_registry;

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
#    ifdef CANOPY_USE_TELEMETRY
            if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
            {
                auto message = std::string(call_name) + " failed " + std::to_string(status);
                telemetry_service->message(rpc::i_telemetry_service::err, message.c_str());
            }
#    endif
            RPC_ERROR("{} gave an enclave error {}", call_name, status);
        }

        template<typename Fn> sgx_status_t call_ecall_with_retry_thread(Fn&& fn)
        {
            auto status = fn();
            if (status == SGX_ERROR_ECALL_NOT_ALLOWED)
            {
                auto task = std::thread([&]() { status = fn(); });
                task.join();
            }
            return status;
        }

        init_request to_sgx_request(
            const rpc::remote_object& host_remote_object,
            rpc::zone child_zone_id)
        {
            return init_request{host_remote_object, child_zone_id};
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

        send_result from_sgx_response(const send_response& response)
        {
            return send_result{response.error_code, response.out_data, response.out_back_channel};
        }

        rpc::standard_result from_sgx_response(const standard_response& response)
        {
            return rpc::standard_result{response.error_code, response.out_back_channel};
        }

        rpc::send_params from_sgx_request(const send_request& request)
        {
            return rpc::send_params{request.protocol_version,
                request.encoding_type,
                request.tag,
                request.caller_zone_id,
                request.remote_object_id,
                request.interface_id,
                request.method_id,
                request.in_data,
                request.in_back_channel};
        }

        rpc::post_params from_sgx_request(const post_request& request)
        {
            return rpc::post_params{request.protocol_version,
                request.encoding_type,
                request.tag,
                request.caller_zone_id,
                request.remote_object_id,
                request.interface_id,
                request.method_id,
                request.in_data,
                request.in_back_channel};
        }

        rpc::try_cast_params from_sgx_request(const try_cast_request& request)
        {
            return rpc::try_cast_params{request.protocol_version,
                request.caller_zone_id,
                request.remote_object_id,
                request.interface_id,
                request.in_back_channel};
        }

        rpc::add_ref_params from_sgx_request(const add_ref_request& request)
        {
            return rpc::add_ref_params{request.protocol_version,
                request.remote_object_id,
                request.caller_zone_id,
                request.requesting_zone_id,
                request.build_out_param_channel,
                request.in_back_channel};
        }

        rpc::release_params from_sgx_request(const release_request& request)
        {
            return rpc::release_params{request.protocol_version,
                request.remote_object_id,
                request.caller_zone_id,
                request.options,
                request.in_back_channel};
        }

        rpc::object_released_params from_sgx_request(const object_released_request& request)
        {
            return rpc::object_released_params{
                request.protocol_version, request.remote_object_id, request.caller_zone_id, request.in_back_channel};
        }

        rpc::transport_down_params from_sgx_request(const transport_down_request& request)
        {
            return rpc::transport_down_params{
                request.protocol_version, request.destination_zone_id, request.caller_zone_id, request.in_back_channel};
        }

        rpc::get_new_zone_id_params from_sgx_request(const get_new_zone_id_request& request)
        {
            return rpc::get_new_zone_id_params{request.protocol_version, request.in_back_channel};
        }

        send_response to_sgx_response(const rpc::send_result& result)
        {
            return send_response{result.error_code, result.out_buf, result.out_back_channel};
        }

        standard_response to_sgx_response(const rpc::standard_result& result)
        {
            return standard_response{result.error_code, result.out_back_channel};
        }

        new_zone_id_response to_sgx_response(const rpc::new_zone_id_result& result)
        {
            return new_zone_id_response{result.error_code, result.zone_id, result.out_back_channel};
        }

        rpc::connect_result from_sgx_response(const init_response& response)
        {
            return rpc::connect_result{response.error_code, response.example_object_id};
        }

        template<typename T>
        int write_blob_response(
            const T& value,
            size_t output_capacity,
            char* output_buffer,
            size_t* output_size)
        {
            if (!output_size)
                return rpc::error::INVALID_DATA();

            auto blob = to_sgx_blob(value);
            *output_size = blob.size();
            if (*output_size > output_capacity)
                return rpc::error::NEED_MORE_MEMORY();

            if (output_buffer && !blob.empty())
                memcpy(output_buffer, blob.data(), blob.size());
            return rpc::error::OK();
        }

        void register_host_transport(
            uint64_t enclave_id,
            const std::shared_ptr<enclave_transport>& transport)
        {
            std::lock_guard lock(host_transport_registry_mutex);
            host_transport_registry[enclave_id] = transport;
        }

        void unregister_host_transport(uint64_t enclave_id)
        {
            std::lock_guard lock(host_transport_registry_mutex);
            host_transport_registry.erase(enclave_id);
        }

        std::shared_ptr<enclave_transport> get_host_transport(uint64_t enclave_id)
        {
            std::lock_guard lock(host_transport_registry_mutex);
            auto it = host_transport_registry.find(enclave_id);
            if (it == host_transport_registry.end())
            {
                RPC_CRITICAL("Looking up transport from enclave id {} not found", enclave_id);
                return {};
            }
            auto transport = it->second.lock();
            if (!transport)
            {
                RPC_ERROR("Looking up transport from enclave id {} has expired", enclave_id);
                host_transport_registry.erase(it);
            }
            return transport;
        }
    }

    enclave_transport::enclave_owner::~enclave_owner()
    {
        unregister_host_transport(eid_);
        marshal_test_destroy_enclave(eid_);
        sgx_destroy_enclave(eid_);
    }

    enclave_transport::enclave_transport(
        std::string name,
        std::shared_ptr<rpc::service> service,
        std::string enclave_path)
        : rpc::transport(
              std::move(name),
              std::move(service))
        , enclave_path_(std::move(enclave_path))
    {
    }

    CORO_TASK(rpc::connect_result)
    enclave_transport::inner_connect(
        std::shared_ptr<rpc::object_stub> stub,
        rpc::connection_settings input_descr)
    {
        sgx_launch_token_t token = {0};
        int updated = 0;
#    ifdef _WIN32
        auto status = sgx_create_enclavea(enclave_path_.data(), SGX_DEBUG_FLAG, &token, &updated, &eid_, NULL);
#    else
        auto status = sgx_create_enclave(enclave_path_.data(), SGX_DEBUG_FLAG, &token, &updated, &eid_, NULL);
#    endif
        if (status)
        {
            log_sgx_failure("sgx_create_enclave", status);
            CO_RETURN rpc::connect_result{rpc::error::TRANSPORT_ERROR(), {}};
        }

        auto svc = get_service();

        // Allocate a zone ID for the DLL zone
        get_new_zone_id_params zone_params;
        zone_params.protocol_version = rpc::get_version();
        auto zone_result = CO_AWAIT svc->get_new_zone_id(std::move(zone_params));
        if (zone_result.error_code != rpc::error::OK())
        {
            RPC_ERROR("[dynamic_library] get_new_zone_id failed: {}", zone_result.error_code);
            CO_RETURN rpc::connect_result{zone_result.error_code, {}};
        }

        rpc::zone adjacent_zone_id = zone_result.zone_id;
        set_adjacent_zone_id(adjacent_zone_id);
        svc->add_transport(adjacent_zone_id, shared_from_this());

        if (stub)
        {
            auto ret = CO_AWAIT stub->add_ref(false, false, adjacent_zone_id);
            if (ret != rpc::error::OK())
            {
                sgx_destroy_enclave(eid_);
                CO_RETURN rpc::connect_result{ret, {}};
            }
        }

        register_host_transport(eid_, std::static_pointer_cast<enclave_transport>(shared_from_this()));

        auto init_request = to_sgx_blob(to_sgx_request(
            input_descr.remote_object_id.is_set() ? input_descr.remote_object_id : get_zone_id().get_address(),
            adjacent_zone_id));
        std::vector<char> init_response_blob(1024);
        int err_code = rpc::error::OK();
        size_t init_response_size = 0;
        status = marshal_test_init_enclave(
            eid_,
            &err_code,
            eid_,
            init_request.size(),
            init_request.data(),
            init_response_blob.size(),
            init_response_blob.data(),
            &init_response_size);
        if (status)
        {
            log_sgx_failure("marshal_test_init_enclave", status);
            unregister_host_transport(eid_);
            sgx_destroy_enclave(eid_);
            svc->remove_transport(adjacent_zone_id);
            CO_RETURN rpc::connect_result{rpc::error::TRANSPORT_ERROR(), {}};
        }

        if (err_code == rpc::error::NEED_MORE_MEMORY())
        {
            init_response_blob.resize(init_response_size);
            status = marshal_test_init_enclave(
                eid_,
                &err_code,
                eid_,
                init_request.size(),
                init_request.data(),
                init_response_blob.size(),
                init_response_blob.data(),
                &init_response_size);
            if (status)
            {
                log_sgx_failure("marshal_test_init_enclave", status);
                unregister_host_transport(eid_);
                sgx_destroy_enclave(eid_);
                svc->remove_transport(adjacent_zone_id);
                CO_RETURN rpc::connect_result{rpc::error::TRANSPORT_ERROR(), {}};
            }
        }

        if (err_code != rpc::error::OK())
        {
            unregister_host_transport(eid_);
            sgx_destroy_enclave(eid_);
            svc->remove_transport(adjacent_zone_id);
            CO_RETURN rpc::connect_result{err_code, {}};
        }

        init_response init_result{};
        err_code = from_sgx_blob(init_response_blob.data(), init_response_size, init_result);
        if (err_code != rpc::error::OK())
        {
            unregister_host_transport(eid_);
            sgx_destroy_enclave(eid_);
            svc->remove_transport(adjacent_zone_id);
            CO_RETURN rpc::connect_result{err_code, {}};
        }

        enclave_owner_ = std::make_shared<enclave_owner>(eid_);
        set_status(rpc::transport_status::CONNECTED);
        CO_RETURN from_sgx_response(init_result);
    }

    CORO_TASK(rpc::send_result)
    enclave_transport::outbound_send(rpc::send_params params)
    {
        auto req = to_sgx_blob(to_sgx_request(params));
        std::vector<char> resp(4096);

        int err_code = rpc::error::OK();
        size_t resp_sz = 0;
        void* retry_buffer = nullptr;
        auto status = call_ecall_with_retry_thread(
            [&]()
            {
                return ::call_enclave(
                    eid_, &err_code, req.size(), req.data(), resp.size(), resp.data(), &resp_sz, &retry_buffer);
            });

        if (status)
        {
            log_sgx_failure("call_enclave", status);
            CO_RETURN rpc::send_result{rpc::error::TRANSPORT_ERROR(), {}, {}};
        }

        if (err_code == rpc::error::NEED_MORE_MEMORY())
        {
            resp.resize(resp_sz);
            status = call_ecall_with_retry_thread(
                [&]()
                {
                    return ::call_enclave(
                        eid_, &err_code, req.size(), req.data(), resp.size(), resp.data(), &resp_sz, &retry_buffer);
                });
            if (status)
            {
                log_sgx_failure("call_enclave", status);
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
    enclave_transport::outbound_post(rpc::post_params params)
    {
        if (params.remote_object_id.as_zone() != get_adjacent_zone_id())
            CO_RETURN;

        auto req = to_sgx_blob(to_sgx_request(params));

        int err_code = rpc::error::OK();
        auto status
            = call_ecall_with_retry_thread([&]() { return ::post_enclave(eid_, &err_code, req.size(), req.data()); });

        if (status)
            log_sgx_failure("post_enclave", status);

        CO_RETURN;
    }

    CORO_TASK(rpc::standard_result)
    enclave_transport::outbound_try_cast(rpc::try_cast_params params)
    {
        auto req = to_sgx_blob(to_sgx_request(params));
        std::vector<char> resp(1024);
        int err_code = rpc::error::OK();
        size_t resp_sz = 0;

        auto status = call_ecall_with_retry_thread([&]()
            { return ::try_cast_enclave(eid_, &err_code, req.size(), req.data(), resp.size(), resp.data(), &resp_sz); });

        if (status)
        {
            log_sgx_failure("try_cast_enclave", status);
            CO_RETURN rpc::standard_result{rpc::error::TRANSPORT_ERROR(), {}};
        }

        if (err_code == rpc::error::NEED_MORE_MEMORY())
        {
            resp.resize(resp_sz);
            status = call_ecall_with_retry_thread(
                [&]()
                {
                    return ::try_cast_enclave(eid_, &err_code, req.size(), req.data(), resp.size(), resp.data(), &resp_sz);
                });
            if (status)
            {
                log_sgx_failure("try_cast_enclave", status);
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
    enclave_transport::outbound_add_ref(rpc::add_ref_params params)
    {
        auto req = to_sgx_blob(to_sgx_request(params));
        std::vector<char> resp(1024);
        int err_code = rpc::error::OK();
        size_t resp_sz = 0;

        auto status = call_ecall_with_retry_thread([&]()
            { return ::add_ref_enclave(eid_, &err_code, req.size(), req.data(), resp.size(), resp.data(), &resp_sz); });

        if (status)
        {
            log_sgx_failure("add_ref_enclave", status);
            CO_RETURN rpc::standard_result{rpc::error::TRANSPORT_ERROR(), {}};
        }

        if (err_code == rpc::error::NEED_MORE_MEMORY())
        {
            resp.resize(resp_sz);
            status = call_ecall_with_retry_thread(
                [&]()
                {
                    return ::add_ref_enclave(eid_, &err_code, req.size(), req.data(), resp.size(), resp.data(), &resp_sz);
                });
            if (status)
            {
                log_sgx_failure("add_ref_enclave", status);
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
    enclave_transport::outbound_release(rpc::release_params params)
    {
        auto req = to_sgx_blob(to_sgx_request(params));
        std::vector<char> resp(1024);
        int err_code = rpc::error::OK();
        size_t resp_sz = 0;

        auto status = call_ecall_with_retry_thread([&]()
            { return ::release_enclave(eid_, &err_code, req.size(), req.data(), resp.size(), resp.data(), &resp_sz); });

        if (status)
        {
            log_sgx_failure("release_enclave", status);
            CO_RETURN rpc::standard_result{rpc::error::TRANSPORT_ERROR(), {}};
        }

        if (err_code == rpc::error::NEED_MORE_MEMORY())
        {
            resp.resize(resp_sz);
            status = call_ecall_with_retry_thread(
                [&]()
                {
                    return ::release_enclave(eid_, &err_code, req.size(), req.data(), resp.size(), resp.data(), &resp_sz);
                });
            if (status)
            {
                log_sgx_failure("release_enclave", status);
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
    enclave_transport::outbound_object_released(rpc::object_released_params params)
    {
        auto req = to_sgx_blob(to_sgx_request(params));
        int err_code = rpc::error::OK();
        auto status = call_ecall_with_retry_thread(
            [&]() { return ::object_released_enclave(eid_, &err_code, req.size(), req.data()); });
        if (status)
            log_sgx_failure("object_released_enclave", status);
        CO_RETURN;
    }

    CORO_TASK(void)
    enclave_transport::outbound_transport_down(rpc::transport_down_params params)
    {
        auto req = to_sgx_blob(to_sgx_request(params));
        int err_code = rpc::error::OK();
        auto status = call_ecall_with_retry_thread(
            [&]() { return ::transport_down_enclave(eid_, &err_code, req.size(), req.data()); });
        if (status)
            log_sgx_failure("transport_down_enclave", status);
        CO_RETURN;
    }
}

extern "C"
{
    int call_host(
        uint64_t enclave_id,
        size_t req_sz,
        const char* req,
        size_t resp_cap,
        char* resp,
        size_t* resp_sz)
    {
        rpc::sgx::marshal_test::send_request request;
        auto err = rpc::sgx::from_sgx_blob(req, req_sz, request);
        if (err != rpc::error::OK())
            return err;
        auto transport = rpc::sgx::get_host_transport(enclave_id);
        if (!transport)
            return rpc::error::TRANSPORT_ERROR();
        auto result = transport->inbound_send(rpc::sgx::from_sgx_request(request));
        return rpc::sgx::write_blob_response(rpc::sgx::to_sgx_response(result), resp_cap, resp, resp_sz);
    }

    int post_host(
        uint64_t enclave_id,
        size_t req_sz,
        const char* req)
    {
        rpc::sgx::marshal_test::post_request request;
        auto err = rpc::sgx::from_sgx_blob(req, req_sz, request);
        if (err != rpc::error::OK())
            return err;
        auto transport = rpc::sgx::get_host_transport(enclave_id);
        if (!transport)
            return rpc::error::TRANSPORT_ERROR();
        transport->inbound_post(rpc::sgx::from_sgx_request(request));
        return rpc::error::OK();
    }

    int try_cast_host(
        uint64_t enclave_id,
        size_t req_sz,
        const char* req,
        size_t resp_cap,
        char* resp,
        size_t* resp_sz)
    {
        rpc::sgx::marshal_test::try_cast_request request;
        auto err = rpc::sgx::from_sgx_blob(req, req_sz, request);
        if (err != rpc::error::OK())
            return err;
        auto transport = rpc::sgx::get_host_transport(enclave_id);
        if (!transport)
            return rpc::error::TRANSPORT_ERROR();
        auto result = transport->inbound_try_cast(rpc::sgx::from_sgx_request(request));
        return rpc::sgx::write_blob_response(rpc::sgx::to_sgx_response(result), resp_cap, resp, resp_sz);
    }

    int add_ref_host(
        uint64_t enclave_id,
        size_t req_sz,
        const char* req,
        size_t resp_cap,
        char* resp,
        size_t* resp_sz)
    {
        rpc::sgx::marshal_test::add_ref_request request;
        auto err = rpc::sgx::from_sgx_blob(req, req_sz, request);
        if (err != rpc::error::OK())
            return err;
        auto transport = rpc::sgx::get_host_transport(enclave_id);
        if (!transport)
            return rpc::error::TRANSPORT_ERROR();
        auto result = transport->inbound_add_ref(rpc::sgx::from_sgx_request(request));
        return rpc::sgx::write_blob_response(rpc::sgx::to_sgx_response(result), resp_cap, resp, resp_sz);
    }

    int release_host(
        uint64_t enclave_id,
        size_t req_sz,
        const char* req,
        size_t resp_cap,
        char* resp,
        size_t* resp_sz)
    {
        rpc::sgx::marshal_test::release_request request;
        auto err = rpc::sgx::from_sgx_blob(req, req_sz, request);
        if (err != rpc::error::OK())
            return err;
        auto transport = rpc::sgx::get_host_transport(enclave_id);
        if (!transport)
            return rpc::error::TRANSPORT_ERROR();
        auto result = transport->inbound_release(rpc::sgx::from_sgx_request(request));
        return rpc::sgx::write_blob_response(rpc::sgx::to_sgx_response(result), resp_cap, resp, resp_sz);
    }

    int object_released_host(
        uint64_t enclave_id,
        size_t req_sz,
        const char* req)
    {
        rpc::sgx::marshal_test::object_released_request request;
        auto err = rpc::sgx::from_sgx_blob(req, req_sz, request);
        if (err != rpc::error::OK())
            return err;
        auto transport = rpc::sgx::get_host_transport(enclave_id);
        if (!transport)
            return rpc::error::TRANSPORT_ERROR();
        transport->inbound_object_released(rpc::sgx::from_sgx_request(request));
        return rpc::error::OK();
    }

    int transport_down_host(
        uint64_t enclave_id,
        size_t req_sz,
        const char* req)
    {
        rpc::sgx::marshal_test::transport_down_request request;
        auto err = rpc::sgx::from_sgx_blob(req, req_sz, request);
        if (err != rpc::error::OK())
            return err;
        auto transport = rpc::sgx::get_host_transport(enclave_id);
        if (!transport)
            return rpc::error::TRANSPORT_ERROR();
        transport->inbound_transport_down(rpc::sgx::from_sgx_request(request));
        return rpc::error::OK();
    }

    int get_new_zone_id_host(
        uint64_t enclave_id,
        size_t req_sz,
        const char* req,
        size_t resp_cap,
        char* resp,
        size_t* resp_sz)
    {
        rpc::sgx::marshal_test::get_new_zone_id_request request;
        auto err = rpc::sgx::from_sgx_blob(req, req_sz, request);
        if (err != rpc::error::OK())
            return err;
        auto transport = rpc::sgx::get_host_transport(enclave_id);
        if (!transport)
        {
            return rpc::error::TRANSPORT_ERROR();
        }
        auto result = transport->get_new_zone_id(rpc::sgx::from_sgx_request(request));
        return rpc::sgx::write_blob_response(rpc::sgx::to_sgx_response(result), resp_cap, resp, resp_sz);
    }

    void hang()
    {
        std::abort();
    }

    void on_service_creation_host(
        const char*,
        uint64_t,
        uint64_t)
    {
    }
    void on_service_deletion_host(uint64_t) { }
    void on_service_try_cast_host(
        uint64_t,
        uint64_t,
        uint64_t,
        uint64_t,
        uint64_t)
    {
    }
    void on_service_add_ref_host(
        uint64_t,
        uint64_t,
        uint64_t,
        uint64_t,
        uint64_t,
        uint64_t)
    {
    }
    void on_service_release_host(
        uint64_t,
        uint64_t,
        uint64_t,
        uint64_t,
        uint64_t)
    {
    }
    void on_service_proxy_creation_host(
        const char*,
        const char*,
        uint64_t,
        uint64_t,
        uint64_t)
    {
    }
    void on_cloned_service_proxy_creation_host(
        const char*,
        const char*,
        uint64_t,
        uint64_t,
        uint64_t)
    {
    }
    void on_service_proxy_deletion_host(
        uint64_t,
        uint64_t,
        uint64_t)
    {
    }
    void on_service_proxy_try_cast_host(
        uint64_t,
        uint64_t,
        uint64_t,
        uint64_t,
        uint64_t)
    {
    }
    void on_service_proxy_add_ref_host(
        uint64_t,
        uint64_t,
        uint64_t,
        uint64_t,
        uint64_t,
        uint64_t)
    {
    }
    void on_service_proxy_release_host(
        uint64_t,
        uint64_t,
        uint64_t,
        uint64_t,
        uint64_t)
    {
    }
    void on_service_proxy_add_external_ref_host(
        uint64_t,
        uint64_t,
        uint64_t)
    {
    }
    void on_service_proxy_release_external_ref_host(
        uint64_t,
        uint64_t,
        uint64_t)
    {
    }
    void on_impl_creation_host(
        const char*,
        uint64_t,
        uint64_t)
    {
    }
    void on_impl_deletion_host(
        uint64_t,
        uint64_t)
    {
    }
    void on_stub_creation_host(
        uint64_t,
        uint64_t,
        uint64_t)
    {
    }
    void on_stub_deletion_host(
        uint64_t,
        uint64_t)
    {
    }
    void on_stub_send_host(
        uint64_t,
        uint64_t,
        uint64_t,
        uint64_t)
    {
    }
    void on_stub_add_ref_host(
        uint64_t,
        uint64_t,
        uint64_t,
        uint64_t,
        uint64_t)
    {
    }
    void on_stub_release_host(
        uint64_t,
        uint64_t,
        uint64_t,
        uint64_t,
        uint64_t)
    {
    }
    void on_object_proxy_creation_host(
        uint64_t,
        uint64_t,
        uint64_t,
        int)
    {
    }
    void on_object_proxy_deletion_host(
        uint64_t,
        uint64_t,
        uint64_t)
    {
    }
    void on_interface_proxy_creation_host(
        const char*,
        uint64_t,
        uint64_t,
        uint64_t,
        uint64_t)
    {
    }
    void on_interface_proxy_deletion_host(
        uint64_t,
        uint64_t,
        uint64_t,
        uint64_t)
    {
    }
    void on_interface_proxy_send_host(
        const char*,
        uint64_t,
        uint64_t,
        uint64_t,
        uint64_t,
        uint64_t)
    {
    }
    void message_host(
        uint64_t,
        const char*)
    {
    }
    void on_transport_creation_host(
        const char*,
        uint64_t,
        uint64_t,
        uint32_t)
    {
    }
    void on_transport_deletion_host(
        uint64_t,
        uint64_t)
    {
    }
    void on_transport_status_change_host(
        const char*,
        uint64_t,
        uint64_t,
        uint32_t,
        uint32_t)
    {
    }
    void on_transport_add_destination_host(
        uint64_t,
        uint64_t,
        uint64_t,
        uint64_t)
    {
    }
    void on_transport_remove_passthrough_host(
        uint64_t,
        uint64_t,
        uint64_t,
        uint64_t)
    {
    }
    void on_pass_through_creation_host(
        uint64_t,
        uint64_t,
        uint64_t,
        uint64_t,
        uint64_t)
    {
    }
    void on_pass_through_deletion_host(
        uint64_t,
        uint64_t,
        uint64_t)
    {
    }
    void on_pass_through_add_ref_host(
        uint64_t,
        uint64_t,
        uint64_t,
        uint64_t,
        int64_t,
        int64_t)
    {
    }
    void on_pass_through_release_host(
        uint64_t,
        uint64_t,
        uint64_t,
        int64_t,
        int64_t)
    {
    }
    void on_pass_through_status_change_host(
        uint64_t,
        uint64_t,
        uint64_t,
        uint32_t,
        uint32_t)
    {
    }
}

#  endif

#endif
