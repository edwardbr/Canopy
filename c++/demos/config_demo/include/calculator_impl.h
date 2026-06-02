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

        CORO_TASK(int)
        add(int a,
            int b,
            int& result) override
        {
            result = a + b;
            CO_RETURN rpc::error::OK();
        }

        CORO_TASK(int)
        subtract(
            int a,
            int b,
            int& result) override
        {
            result = a - b;
            CO_RETURN rpc::error::OK();
        }

        CORO_TASK(int)
        multiply(
            int a,
            int b,
            int& result) override
        {
            result = a * b;
            CO_RETURN rpc::error::OK();
        }

        CORO_TASK(int)
        divide(
            int a,
            int b,
            int& result) override
        {
            if (b == 0)
                CO_RETURN rpc::error::INVALID_DATA();

            result = a / b;
            CO_RETURN rpc::error::OK();
        }

    private:
        std::weak_ptr<rpc::service> service_;
    };
} // namespace config_demo::v1
