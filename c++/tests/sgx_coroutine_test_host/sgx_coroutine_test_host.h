/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <memory>
#include <new>
#include <string>

#include <example/example.h>

class enclave_connection_test_host : public rpc::base<enclave_connection_test_host, yyy::i_host>,
                                     public rpc::enable_shared_from_this<enclave_connection_test_host>
{
public:
    static rpc::service_connect_result<yyy::i_host> create_for_test()
    {
        try
        {
            auto host_ptr = rpc::shared_ptr<yyy::i_host>(new enclave_connection_test_host());
            return {rpc::error::OK(), rpc::static_pointer_cast<yyy::i_host>(host_ptr)};
        }
        catch (...)
        {
            return {rpc::error::EXCEPTION(), {}};
        }
    }

    ~enclave_connection_test_host() override = default;

    CORO_TASK(error_code) create_enclave(rpc::shared_ptr<yyy::i_example>& target) override
    {
        target = nullptr;
        CO_RETURN rpc::error::INCOMPATIBLE_SERVICE();
    }

    CORO_TASK(error_code) create_local_zone(rpc::shared_ptr<yyy::i_example>& target) override
    {
        target = nullptr;
        CO_RETURN rpc::error::INCOMPATIBLE_SERVICE();
    }

    CORO_TASK(error_code)
    look_up_app(
        const std::string& app_name,
        rpc::shared_ptr<yyy::i_example>& app) override
    {
        (void)app_name;
        app = nullptr;
        CO_RETURN rpc::error::INCOMPATIBLE_SERVICE();
    }

    CORO_TASK(error_code)
    set_app(
        const std::string& name,
        const rpc::shared_ptr<yyy::i_example>& app) override
    {
        (void)name;
        (void)app;
        CO_RETURN rpc::error::INCOMPATIBLE_SERVICE();
    }

    CORO_TASK(error_code) unload_app(const std::string& name) override
    {
        (void)name;
        CO_RETURN rpc::error::INCOMPATIBLE_SERVICE();
    }

private:
    enclave_connection_test_host() noexcept { }
};
