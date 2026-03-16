/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

namespace rpc
{
    // Base class for all rpc objects it provides essential casting and reflection services
    // derive your class from this class and you will get more features for free when they arrive
    template<typename Implementation, typename... Interfaces> class base : public Interfaces...
    {
        std::weak_ptr<rpc::object_stub> stub_;

    public:
        ~base() override = default;

        // base is a collection of interface stubs it does not support proxy functionallity
        [[nodiscard]] bool __rpc_is_local() const override { return true; }
        [[nodiscard]] std::shared_ptr<rpc::object_proxy> __rpc_get_object_proxy() const override { return nullptr; }

        // Query to see if this class supports an an interface
        [[nodiscard]] const rpc::casting_interface* __rpc_query_interface(rpc::interface_ordinal interface_id) const override
        {
            const rpc::casting_interface* out = nullptr;
            (
                [&]
                {
                    if (rpc::match<Interfaces>(interface_id))
                        out = static_cast<const Interfaces*>(static_cast<const Implementation*>(this));
                    return out != nullptr;
                }()
                || ...);
            return out;
        }

        // overriden stub functionallity
        [[nodiscard]] std::shared_ptr<rpc::object_stub> __rpc_get_stub() const override { return stub_.lock(); }
        void __rpc_set_stub(const std::shared_ptr<rpc::object_stub>& stub) override { stub_ = stub; }

        CORO_TASK(rpc::send_result)
        __rpc_call(rpc::send_params params) override
        {
            send_result result{rpc::error::INVALID_INTERFACE_ID(), {}, {}};
            [[maybe_unused]] bool found
                = ((rpc::match<Interfaces>(params.interface_id)
                           ? ((result = CO_AWAIT Interfaces::stub_caller::call(
                                   static_cast<Interfaces*>(static_cast<Implementation*>(this)), std::move(params))),
                                 true)
                           : false)
                    || ...);
            CO_RETURN result;
        }
    };
}
