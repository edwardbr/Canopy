/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <rpc_objects/calculator/calculator_impl.h>

#include <utility>

namespace calculator::v1
{
    namespace
    {
        class calculator_impl final : public rpc::base<calculator_impl, i_calculator>
        {
        public:
            calculator_impl() = default;

            explicit calculator_impl(std::shared_ptr<rpc::service> service)
                : service_(std::move(service))
            {
            }

            CORO_TASK(calculator_error)
            add(int a,
                int b,
                int& sum) override
            {
                sum = a + b;
                CO_RETURN rpc::error::OK();
            }

            CORO_TASK(calculator_error)
            subtract(
                int a,
                int b,
                int& difference) override
            {
                difference = a - b;
                CO_RETURN rpc::error::OK();
            }

            CORO_TASK(calculator_error)
            multiply(
                int a,
                int b,
                int& product) override
            {
                product = a * b;
                CO_RETURN rpc::error::OK();
            }

            CORO_TASK(calculator_error)
            divide(
                int a,
                int b,
                int& quotient) override
            {
                if (b == 0)
                    CO_RETURN calculator_error::INVALID_ARGUMENT;

                quotient = a / b;
                CO_RETURN rpc::error::OK();
            }

        private:
            std::weak_ptr<rpc::service> service_;
        };
    }

    rpc::shared_ptr<i_calculator> make_calculator()
    {
        return rpc::shared_ptr<i_calculator>(new calculator_impl());
    }

    rpc::shared_ptr<i_calculator> make_calculator(std::shared_ptr<rpc::service> service)
    {
        return rpc::shared_ptr<i_calculator>(new calculator_impl(std::move(service)));
    }

    rpc::module::object_factory<
        i_calculator,
        i_calculator>
    make_calculator_factory()
    {
        return rpc::module::make_object_factory_with_service<i_calculator, i_calculator>(
            [](std::shared_ptr<rpc::service> service) { return make_calculator(std::move(service)); });
    }
}
