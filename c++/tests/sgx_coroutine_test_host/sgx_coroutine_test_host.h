/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <memory>
#include <new>
#include <string>
#include <utility>

#include <example/example.h>
#include <io_uring/host_controller.h>

class host_with_io_uring_control
    : public rpc::base<host_with_io_uring_control, yyy::i_host, rpc::io_uring::i_host_io_uring_control>,
      public rpc::enable_shared_from_this<host_with_io_uring_control>
{
public:
    static rpc::service_connect_result<yyy::i_host> create_for_test(
        std::shared_ptr<coro::scheduler> scheduler = {},
        rpc::io_uring::host_controller::options controller_options = {})
    {
        std::unique_ptr<rpc::io_uring::host_controller> controller;
        controller_options.register_fixed_files = true;
        if (controller_options.fixed_file_count == 0)
        {
            controller_options.fixed_file_count = 256;
        }
        auto error_code = rpc::io_uring::host_controller::create(controller, controller_options, std::move(scheduler));
        if (error_code != rpc::error::OK())
        {
            return {error_code, {}};
        }

        try
        {
            auto host_ptr = rpc::shared_ptr<yyy::i_host>(new host_with_io_uring_control(std::move(controller)));
            return {rpc::error::OK(), rpc::static_pointer_cast<yyy::i_host>(host_ptr)};
        }
        catch (...)
        {
            return {rpc::error::EXCEPTION(), {}};
        }
    }

    ~host_with_io_uring_control() override = default;

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

    CORO_TASK(int) wake_iouring() override
    {
        if (!controller_)
        {
            CO_RETURN rpc::error::RESOURCE_CLOSED();
        }

        CO_RETURN controller_->wake_iouring();
    }

    CORO_TASK(int) get_iouring_data(rpc::io_uring::data& ring_data) override
    {
        if (!controller_)
        {
            CO_RETURN rpc::error::RESOURCE_CLOSED();
        }

        CO_RETURN controller_->get_iouring_data(ring_data);
    }

private:
    explicit host_with_io_uring_control(std::unique_ptr<rpc::io_uring::host_controller> controller) noexcept
        : controller_(std::move(controller))
    {
    }

    std::unique_ptr<rpc::io_uring::host_controller> controller_;
};
