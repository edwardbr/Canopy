/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <config_demo/config_demo.h>
#include <rpc/rpc.h>

#include <memory>

namespace config_demo::v1
{
    class calculator_impl final : public rpc::base<calculator_impl, i_calculator>
    {
    public:
        calculator_impl() = default;

        explicit calculator_impl(std::shared_ptr<rpc::service> service)
            : service_(std::move(service))
        {
        }

        CORO_TASK(demo_error)
        add(int a,
            int b,
            int& result) override
        {
            result = a + b;
            CO_RETURN rpc::error::OK();
        }

        CORO_TASK(demo_error)
        subtract(
            int a,
            int b,
            int& result) override
        {
            result = a - b;
            CO_RETURN rpc::error::OK();
        }

        CORO_TASK(demo_error)
        multiply(
            int a,
            int b,
            int& result) override
        {
            result = a * b;
            CO_RETURN rpc::error::OK();
        }

        CORO_TASK(demo_error)
        divide(
            int a,
            int b,
            int& result) override
        {
            if (b == 0)
                CO_RETURN demo_error::INVALID_ARGUMENT;

            result = a / b;
            CO_RETURN rpc::error::OK();
        }

    private:
        std::weak_ptr<rpc::service> service_;
    };
} // namespace config_demo::v1
