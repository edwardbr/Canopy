/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#ifndef CANOPY_BUILD_COROUTINE

#  include <sgx_trts.h>

#  include <cstring>
#  include <functional>
#  include <memory>
#  include <string>
#  include <tuple>
#  include <utility>
#  include <vector>

#  include <edl/sgx_blocking_marshal.h>
#  include <json/json_dom.h>
#  include <rpc/rpc.h>
#  include <trusted/sgx_blocking_transport_t.h>
#  include <transports/sgx_blocking/host_transport.h>
#  ifdef CANOPY_USE_TELEMETRY
#    include <rpc/telemetry/i_telemetry_service.h>
#    include <rpc/telemetry/telemetry_service_factory.h>
#  endif

namespace rpc::sgx_blocking_transport::object_runtime
{
    namespace detail
    {
        using namespace rpc::sgx_blocking_transport::marshal;

        template<typename T> std::vector<char> to_sgx_blob(const T& value)
        {
            return rpc::to_yas_binary<std::vector<char>>(value);
        }

        template<typename T>
        int from_sgx_blob(
            size_t size,
            const char* buffer,
            T& value)
        {
            rpc::byte_span span{reinterpret_cast<const uint8_t*>(buffer), size};
            auto err = rpc::from_yas_binary(span, value);
            if (!err.empty())
            {
                RPC_ERROR("SGX blob decode failed: {}", err);
                return rpc::error::INVALID_DATA();
            }
            return rpc::error::OK();
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
                std::memcpy(output_buffer, blob.data(), blob.size());
            return rpc::error::OK();
        }

        inline rpc::send_params from_sgx_request(const send_request& request)
        {
            return rpc::send_params{FLD(protocol_version) request.protocol_version,
                FLD(encoding_type) request.encoding_type,
                FLD(tag) request.tag,
                FLD(caller_zone_id) request.caller_zone_id,
                FLD(remote_object_id) request.remote_object_id,
                FLD(interface_id) request.interface_id,
                FLD(method_id) request.method_id,
                FLD(in_data) request.in_data,
                FLD(in_back_channel) request.in_back_channel,
                FLD(request_id) request.request_id};
        }

        inline rpc::post_params from_sgx_request(const post_request& request)
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

        inline rpc::try_cast_params from_sgx_request(const try_cast_request& request)
        {
            return rpc::try_cast_params{request.protocol_version,
                request.caller_zone_id,
                request.remote_object_id,
                request.interface_id,
                request.in_back_channel,
                request.payload};
        }

        inline rpc::add_ref_params from_sgx_request(const add_ref_request& request)
        {
            return rpc::add_ref_params{request.protocol_version,
                request.remote_object_id,
                request.caller_zone_id,
                request.requesting_zone_id,
                request.build_out_param_channel,
                request.in_back_channel,
                request.request_id,
                request.payload};
        }

        inline rpc::release_params from_sgx_request(const release_request& request)
        {
            return rpc::release_params{request.protocol_version,
                request.remote_object_id,
                request.caller_zone_id,
                request.options,
                request.in_back_channel,
                request.payload};
        }

        inline rpc::object_released_params from_sgx_request(const object_released_request& request)
        {
            return rpc::object_released_params{request.protocol_version,
                request.remote_object_id,
                request.caller_zone_id,
                request.in_back_channel,
                request.payload};
        }

        inline rpc::transport_down_params from_sgx_request(const transport_down_request& request)
        {
            return rpc::transport_down_params{request.protocol_version,
                request.destination_zone_id,
                request.caller_zone_id,
                request.in_back_channel,
                request.payload};
        }

        inline rpc::get_new_zone_id_params from_sgx_request(const get_new_zone_id_request& request)
        {
            return rpc::get_new_zone_id_params{request.protocol_version, request.in_back_channel};
        }

        inline send_response to_sgx_response(const rpc::send_result& result)
        {
            return send_response{result.error_code, result.out_buf, result.out_back_channel};
        }

        inline standard_response to_sgx_response(const rpc::standard_result& result)
        {
            return standard_response{result.error_code, result.out_back_channel};
        }

        inline init_response to_sgx_init_response(
            int error_code,
            const rpc::remote_object& object_id)
        {
            return init_response{error_code, object_id};
        }

        inline rpc::connection_settings from_sgx_init_request(const init_request& request)
        {
            rpc::connection_settings input_descr{};
            input_descr.inbound_interface_id = request.inbound_interface_id;
            input_descr.outbound_interface_id = request.outbound_interface_id;
            input_descr.encoding_type = request.encoding_type;
            input_descr.remote_object_id = request.host_remote_object;
            return input_descr;
        }

        using connection_factory = std::function<int(
            const init_request&, std::shared_ptr<rpc::sgx_blocking_transport::host_transport>, size_t, char*, size_t*)>;

        struct factory_entry
        {
            rpc::interface_ordinal inbound_interface_id;
            rpc::interface_ordinal outbound_interface_id;
            std::string service_name;
            connection_factory connect;
        };

        inline std::vector<factory_entry>& registry()
        {
            static std::vector<factory_entry> entries;
            return entries;
        }

        inline std::weak_ptr<rpc::sgx_blocking_transport::host_transport>& active_host_transport()
        {
            static std::weak_ptr<rpc::sgx_blocking_transport::host_transport> transport;
            return transport;
        }

        inline uint64_t& active_enclave_id()
        {
            static uint64_t enclave_id = 0;
            return enclave_id;
        }

        inline const factory_entry* find_factory(
            rpc::interface_ordinal inbound_interface_id,
            rpc::interface_ordinal outbound_interface_id)
        {
            for (const auto& entry : registry())
            {
                if (entry.inbound_interface_id == inbound_interface_id
                    && entry.outbound_interface_id == outbound_interface_id)
                    return &entry;
            }
            return nullptr;
        }
    }

    inline void register_connection_factory(
        std::string service_name,
        rpc::interface_ordinal inbound_interface_id,
        rpc::interface_ordinal outbound_interface_id,
        detail::connection_factory factory)
    {
        detail::registry().push_back(
            detail::factory_entry{inbound_interface_id, outbound_interface_id, std::move(service_name), std::move(factory)});
    }

    class runtime
    {
    public:
        static int init(
            uint64_t enclave_id,
            size_t req_sz,
            const char* req,
            size_t resp_cap,
            char* resp,
            size_t* resp_sz)
        {
            detail::init_request request{};
            auto err_code = detail::from_sgx_blob(req_sz, req, request);
            if (err_code != rpc::error::OK())
                return err_code;

            auto* factory = detail::find_factory(request.inbound_interface_id, request.outbound_interface_id);
            if (!factory)
            {
                RPC_ERROR(
                    "No SGX blocking RPC object registered for interface pair {} -> {}",
                    request.inbound_interface_id.get_val(),
                    request.outbound_interface_id.get_val());
                return detail::write_blob_response(
                    detail::to_sgx_init_response(rpc::error::INVALID_INTERFACE_ID(), {}), resp_cap, resp, resp_sz);
            }

            detail::active_enclave_id() = enclave_id;
            rpc::sgx_blocking_transport::set_runtime_startup_settings(request.runtime_settings);

#  ifdef CANOPY_USE_TELEMETRY
            rpc::telemetry::create_enclave_telemetry_service(rpc::telemetry::telemetry_service_);
#  endif

            auto host_transport = std::make_shared<rpc::sgx_blocking_transport::host_transport>(
                factory->service_name,
                detail::active_enclave_id(),
                request.child_zone_id,
                rpc::zone{request.host_remote_object.as_zone()});

            detail::active_host_transport() = host_transport;
            return factory->connect(request, std::move(host_transport), resp_cap, resp, resp_sz);
        }

        static void destroy()
        {
            rpc::sgx_blocking_transport::set_runtime_startup_settings(json::v1::object{json::v1::map{}});
            detail::active_host_transport().reset();
            detail::active_enclave_id() = 0;
        }

        static int call(
            size_t req_sz,
            const char* req,
            size_t resp_cap,
            char* resp,
            size_t* resp_sz,
            void** enclave_retry_buffer)
        {
            if (!enclave_retry_buffer)
            {
                RPC_ERROR("Invalid data - null enclave_retry_buffer");
                return rpc::error::INVALID_DATA();
            }

            auto*& retry_buf = *reinterpret_cast<rpc::retry_buffer**>(enclave_retry_buffer);
            if (retry_buf && !sgx_is_within_enclave(retry_buf, sizeof(rpc::retry_buffer*)))
            {
                RPC_ERROR("Security error - retry_buf not within enclave");
                return rpc::error::SECURITY_ERROR();
            }

            if (retry_buf)
            {
                *resp_sz = retry_buf->data.size();
                if (*resp_sz > resp_cap)
                    return rpc::error::NEED_MORE_MEMORY();

                std::memcpy(resp, retry_buf->data.data(), retry_buf->data.size());
                auto ret = retry_buf->return_value;
                delete retry_buf;
                retry_buf = nullptr;
                return ret;
            }

            detail::send_request request{};
            auto err = detail::from_sgx_blob(req_sz, req, request);
            if (err != rpc::error::OK())
                return err;

            auto params = detail::from_sgx_request(request);
            if (params.protocol_version > rpc::get_version())
                return rpc::error::INVALID_VERSION();

            auto transport = detail::active_host_transport().lock();
            if (!transport)
            {
                RPC_ERROR("host transport is missing");
                return rpc::error::INVALID_DATA();
            }

            auto result = transport->inbound_send(std::move(params));
            auto blob = detail::to_sgx_blob(detail::to_sgx_response(result));

            *resp_sz = blob.size();
            if (*resp_sz <= resp_cap)
            {
                std::memcpy(resp, blob.data(), blob.size());
                return rpc::error::OK();
            }

            retry_buf = new rpc::retry_buffer{std::move(blob), rpc::error::OK()};
            return rpc::error::NEED_MORE_MEMORY();
        }

        static int post(
            size_t req_sz,
            const char* req)
        {
            detail::post_request request{};
            auto err = detail::from_sgx_blob(req_sz, req, request);
            if (err != rpc::error::OK())
                return err;

            auto params = detail::from_sgx_request(request);
            if (params.protocol_version > rpc::get_version())
                return rpc::error::INVALID_VERSION();

            auto transport = detail::active_host_transport().lock();
            if (!transport)
            {
                RPC_ERROR("host transport is missing");
                return rpc::error::INVALID_DATA();
            }

            transport->inbound_post(std::move(params));
            return rpc::error::OK();
        }

        static int try_cast(
            size_t req_sz,
            const char* req,
            size_t resp_cap,
            char* resp,
            size_t* resp_sz)
        {
            detail::try_cast_request request{};
            auto err = detail::from_sgx_blob(req_sz, req, request);
            if (err != rpc::error::OK())
                return err;

            auto params = detail::from_sgx_request(request);
            if (params.protocol_version > rpc::get_version())
                return rpc::error::INVALID_VERSION();

            auto transport = detail::active_host_transport().lock();
            if (!transport)
            {
                RPC_ERROR("host transport is missing");
                return rpc::error::INVALID_DATA();
            }

            auto result = transport->inbound_try_cast(std::move(params));
            return detail::write_blob_response(detail::to_sgx_response(result), resp_cap, resp, resp_sz);
        }

        static int add_ref(
            size_t req_sz,
            const char* req,
            size_t resp_cap,
            char* resp,
            size_t* resp_sz)
        {
            detail::add_ref_request request{};
            auto err = detail::from_sgx_blob(req_sz, req, request);
            if (err != rpc::error::OK())
                return err;

            auto params = detail::from_sgx_request(request);
            if (params.protocol_version > rpc::get_version())
                return rpc::error::INCOMPATIBLE_SERVICE();

            auto transport = detail::active_host_transport().lock();
            if (!transport)
            {
                RPC_ERROR("host transport is missing");
                return rpc::error::INVALID_DATA();
            }

            auto result = transport->inbound_add_ref(std::move(params));
            return detail::write_blob_response(detail::to_sgx_response(result), resp_cap, resp, resp_sz);
        }

        static int release(
            size_t req_sz,
            const char* req,
            size_t resp_cap,
            char* resp,
            size_t* resp_sz)
        {
            detail::release_request request{};
            auto err = detail::from_sgx_blob(req_sz, req, request);
            if (err != rpc::error::OK())
                return err;

            auto params = detail::from_sgx_request(request);
            if (params.protocol_version > rpc::get_version())
                return rpc::error::INCOMPATIBLE_SERVICE();

            auto transport = detail::active_host_transport().lock();
            if (!transport)
            {
                RPC_ERROR("host transport is missing");
                return rpc::error::INVALID_DATA();
            }

            auto result = transport->inbound_release(std::move(params));
            return detail::write_blob_response(detail::to_sgx_response(result), resp_cap, resp, resp_sz);
        }

        static int object_released(
            size_t req_sz,
            const char* req)
        {
            detail::object_released_request request{};
            auto err = detail::from_sgx_blob(req_sz, req, request);
            if (err != rpc::error::OK())
                return err;

            auto transport = detail::active_host_transport().lock();
            if (!transport)
            {
                RPC_ERROR("host transport is missing");
                return rpc::error::INVALID_DATA();
            }

            transport->inbound_object_released(detail::from_sgx_request(request));
            return rpc::error::OK();
        }

        static int transport_down(
            size_t req_sz,
            const char* req)
        {
            detail::transport_down_request request{};
            auto err = detail::from_sgx_blob(req_sz, req, request);
            if (err != rpc::error::OK())
                return err;

            auto transport = detail::active_host_transport().lock();
            if (!transport)
            {
                RPC_ERROR("host transport is missing");
                return rpc::error::INVALID_DATA();
            }

            transport->inbound_transport_down(detail::from_sgx_request(request));
            return rpc::error::OK();
        }

        static int get_new_zone_id(
            size_t req_sz,
            const char* req,
            size_t resp_cap,
            char* resp,
            size_t* resp_sz)
        {
            detail::get_new_zone_id_request request{};
            auto err = detail::from_sgx_blob(req_sz, req, request);
            if (err != rpc::error::OK())
                return err;

            std::ignore = detail::from_sgx_request(request);
            detail::new_zone_id_response result{rpc::error::NOT_IMPLEMENTED(), {}, {}};
            return detail::write_blob_response(result, resp_cap, resp, resp_sz);
        }
    };
}

#endif
