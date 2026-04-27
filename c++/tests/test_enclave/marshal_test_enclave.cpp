/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <sgx_trts.h>

#include <cstdio>
#include <cstring>
#include <stdio.h>
#include <string>

#include <rpc/rpc.h>

#include <edl/enclave_marshal_test.h>
#include <trusted/enclave_marshal_test_t.h>
#include <common/foo_impl.h>
#include <transports/sgx/host_transport.h>
#include <example/example.h>

using namespace marshalled_tests;

namespace
{
    using namespace rpc::sgx::marshal_test;

    std::shared_ptr<rpc::child_service> rpc_server;
    std::shared_ptr<rpc::sgx::host_transport> g_host_transport;
    uint64_t g_enclave_id = 0;

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
            memcpy(output_buffer, blob.data(), blob.size());
        return rpc::error::OK();
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

    rpc::connection_settings from_sgx_request(const init_request& request)
    {
        rpc::connection_settings input_descr{};
        input_descr.inbound_interface_id = yyy::i_host::get_id(rpc::get_version());
        input_descr.outbound_interface_id = yyy::i_example::get_id(rpc::get_version());
        input_descr.remote_object_id = request.host_remote_object;
        return input_descr;
    }

    init_response to_sgx_response(
        int error_code,
        const rpc::remote_object& example_object_id)
    {
        return init_response{error_code, example_object_id};
    }

}

int marshal_test_init_enclave(
    uint64_t enclave_id,
    size_t req_sz,
    const char* req,
    size_t resp_cap,
    char* resp,
    size_t* resp_sz)
{
    init_request request{};
    auto err_code = from_sgx_blob(req_sz, req, request);
    if (err_code != rpc::error::OK())
        return err_code;

    auto input_descr = from_sgx_request(request);
    g_enclave_id = enclave_id;

    g_host_transport = std::make_shared<rpc::sgx::host_transport>(
        "test_enclave", g_enclave_id, request.child_zone_id, rpc::zone{request.host_remote_object.as_zone()});

    auto ret = rpc::child_service::create_child_zone<yyy::i_host, yyy::i_example>(
        "test_enclave",
        g_host_transport,
        input_descr,
        [](const rpc::shared_ptr<yyy::i_host>& host,
            std::shared_ptr<rpc::child_service> child_service_ptr) -> CORO_TASK(rpc::service_connect_result<yyy::i_example>)
        {
            rpc_server = child_service_ptr;
            rpc::shared_ptr<yyy::i_example> new_example(new marshalled_tests::example(child_service_ptr, host));
            CO_RETURN rpc::service_connect_result<yyy::i_example>{rpc::error::OK(), new_example};
        });

    if (ret.error_code != rpc::error::OK())
        return write_blob_response(to_sgx_response(ret.error_code, {}), resp_cap, resp, resp_sz);

    return write_blob_response(to_sgx_response(rpc::error::OK(), ret.descriptor), resp_cap, resp, resp_sz);
}

void marshal_test_destroy_enclave()
{
    rpc_server.reset();
    g_host_transport.reset();
    g_enclave_id = 0;
}

int call_enclave(
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

        memcpy(resp, retry_buf->data.data(), retry_buf->data.size());
        auto ret = retry_buf->return_value;
        delete retry_buf;
        retry_buf = nullptr;
        return ret;
    }

    send_request request;
    auto err = from_sgx_blob(req_sz, req, request);
    if (err != rpc::error::OK())
        return err;

    auto params = from_sgx_request(request);
    if (params.protocol_version > rpc::get_version())
        return rpc::error::INVALID_VERSION();

    auto result = g_host_transport->inbound_send(std::move(params));
    auto blob = to_sgx_blob(to_sgx_response(result));

    *resp_sz = blob.size();
    if (*resp_sz <= resp_cap)
    {
        memcpy(resp, blob.data(), blob.size());
        return rpc::error::OK();
    }

    retry_buf = new rpc::retry_buffer{std::move(blob), rpc::error::OK()};
    return rpc::error::NEED_MORE_MEMORY();
}

int post_enclave(
    size_t req_sz,
    const char* req)
{
    post_request request;
    auto err = from_sgx_blob(req_sz, req, request);
    if (err != rpc::error::OK())
        return err;
    auto params = from_sgx_request(request);
    if (params.protocol_version > rpc::get_version())
        return rpc::error::INVALID_VERSION();
    g_host_transport->inbound_post(std::move(params));
    return rpc::error::OK();
}

int try_cast_enclave(
    size_t req_sz,
    const char* req,
    size_t resp_cap,
    char* resp,
    size_t* resp_sz)
{
    try_cast_request request;
    auto err = from_sgx_blob(req_sz, req, request);
    if (err != rpc::error::OK())
        return err;
    auto params = from_sgx_request(request);
    if (params.protocol_version > rpc::get_version())
        return rpc::error::INVALID_VERSION();
    auto result = g_host_transport->inbound_try_cast(std::move(params));
    return write_blob_response(to_sgx_response(result), resp_cap, resp, resp_sz);
}

int add_ref_enclave(
    size_t req_sz,
    const char* req,
    size_t resp_cap,
    char* resp,
    size_t* resp_sz)
{
    add_ref_request request;
    auto err = from_sgx_blob(req_sz, req, request);
    if (err != rpc::error::OK())
        return err;
    auto params = from_sgx_request(request);
    if (params.protocol_version > rpc::get_version())
        return rpc::error::INCOMPATIBLE_SERVICE();
    auto result = g_host_transport->inbound_add_ref(std::move(params));
    return write_blob_response(to_sgx_response(result), resp_cap, resp, resp_sz);
}

int release_enclave(
    size_t req_sz,
    const char* req,
    size_t resp_cap,
    char* resp,
    size_t* resp_sz)
{
    release_request request;
    auto err = from_sgx_blob(req_sz, req, request);
    if (err != rpc::error::OK())
        return err;
    auto params = from_sgx_request(request);
    if (params.protocol_version > rpc::get_version())
        return rpc::error::INCOMPATIBLE_SERVICE();
    auto result = g_host_transport->inbound_release(std::move(params));
    return write_blob_response(to_sgx_response(result), resp_cap, resp, resp_sz);
}

int object_released_enclave(
    size_t req_sz,
    const char* req)
{
    object_released_request request;
    auto err = from_sgx_blob(req_sz, req, request);
    if (err != rpc::error::OK())
        return err;
    auto params = from_sgx_request(request);
    g_host_transport->inbound_object_released(std::move(params));
    return rpc::error::OK();
}

int transport_down_enclave(
    size_t req_sz,
    const char* req)
{
    transport_down_request request;
    auto err = from_sgx_blob(req_sz, req, request);
    if (err != rpc::error::OK())
        return err;
    auto params = from_sgx_request(request);
    g_host_transport->inbound_transport_down(std::move(params));
    return rpc::error::OK();
}

int get_new_zone_id_enclave(
    size_t req_sz,
    const char* req,
    size_t resp_cap,
    char* resp,
    size_t* resp_sz)
{
    get_new_zone_id_request request;
    auto err = from_sgx_blob(req_sz, req, request);
    if (err != rpc::error::OK())
        return err;
    auto params = from_sgx_request(request);
    std::ignore = params;

    new_zone_id_response result{rpc::error::NOT_IMPLEMENTED(), {}, {}};
    return write_blob_response(result, resp_cap, resp, resp_sz);
}

extern "C"
{
    int _Uelf64_valid_object()
    {
        return -1;
    }
}
