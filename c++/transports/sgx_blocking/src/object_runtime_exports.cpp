/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#ifndef CANOPY_BUILD_COROUTINE

#  include <transports/sgx_blocking/object_runtime.h>

int sgx_blocking_init_enclave(
    uint64_t enclave_id,
    size_t req_sz,
    const char* req,
    size_t resp_cap,
    char* resp,
    size_t* resp_sz)
{
    return rpc::sgx_blocking_transport::object_runtime::runtime::init(enclave_id, req_sz, req, resp_cap, resp, resp_sz);
}

void sgx_blocking_destroy_enclave()
{
    rpc::sgx_blocking_transport::object_runtime::runtime::destroy();
}

int call_enclave(
    size_t req_sz,
    const char* req,
    size_t resp_cap,
    char* resp,
    size_t* resp_sz,
    void** enclave_retry_buffer)
{
    return rpc::sgx_blocking_transport::object_runtime::runtime::call(
        req_sz, req, resp_cap, resp, resp_sz, enclave_retry_buffer);
}

int post_enclave(
    size_t req_sz,
    const char* req)
{
    return rpc::sgx_blocking_transport::object_runtime::runtime::post(req_sz, req);
}

int try_cast_enclave(
    size_t req_sz,
    const char* req,
    size_t resp_cap,
    char* resp,
    size_t* resp_sz)
{
    return rpc::sgx_blocking_transport::object_runtime::runtime::try_cast(req_sz, req, resp_cap, resp, resp_sz);
}

int get_schema_enclave(
    size_t req_sz,
    const char* req,
    size_t resp_cap,
    char* resp,
    size_t* resp_sz)
{
    return rpc::sgx_blocking_transport::object_runtime::runtime::get_schema(req_sz, req, resp_cap, resp, resp_sz);
}

int add_ref_enclave(
    size_t req_sz,
    const char* req,
    size_t resp_cap,
    char* resp,
    size_t* resp_sz)
{
    return rpc::sgx_blocking_transport::object_runtime::runtime::add_ref(req_sz, req, resp_cap, resp, resp_sz);
}

int release_enclave(
    size_t req_sz,
    const char* req,
    size_t resp_cap,
    char* resp,
    size_t* resp_sz)
{
    return rpc::sgx_blocking_transport::object_runtime::runtime::release(req_sz, req, resp_cap, resp, resp_sz);
}

int object_released_enclave(
    size_t req_sz,
    const char* req)
{
    return rpc::sgx_blocking_transport::object_runtime::runtime::object_released(req_sz, req);
}

int transport_down_enclave(
    size_t req_sz,
    const char* req)
{
    return rpc::sgx_blocking_transport::object_runtime::runtime::transport_down(req_sz, req);
}

int get_new_zone_id_enclave(
    size_t req_sz,
    const char* req,
    size_t resp_cap,
    char* resp,
    size_t* resp_sz)
{
    return rpc::sgx_blocking_transport::object_runtime::runtime::get_new_zone_id(req_sz, req, resp_cap, resp, resp_sz);
}

#endif
