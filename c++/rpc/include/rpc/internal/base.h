/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

namespace rpc
{
    // Base class for all rpc objects it provides essential casting and reflection services
    // derive your class from this class and you will get more features for free when they arrive
    template<typename Implementation, typename... Interfaces> class base : public rpc::i_noop, public Interfaces...
    {
        std::weak_ptr<rpc::object_stub> stub_;

    protected:
        // NOLINTBEGIN(bugprone-crtp-constructor-accessibility): Canopy allows two-stage CRTP bases.
        base() = default;
        base(const base&) = default;
        base(base&&) noexcept = default;
        base& operator=(const base&) = default;
        base& operator=(base&&) noexcept = default;
        // NOLINTEND(bugprone-crtp-constructor-accessibility)

    public:
        ~base() override = default;

        // base is a collection of interface stubs it does not support proxy functionallity
        [[nodiscard]] bool __rpc_is_local() const override { return true; }
        [[nodiscard]] std::shared_ptr<rpc::object_proxy> __rpc_get_object_proxy() const override { return nullptr; }

        // Query to see if this class supports an an interface
        [[nodiscard]] const rpc::casting_interface* __rpc_query_interface(rpc::interface_ordinal interface_id) const override
        {
            const rpc::casting_interface* out = nullptr;
            if (rpc::match<rpc::i_noop>(interface_id))
            {
                out = static_cast<const rpc::i_noop*>(static_cast<const Implementation*>(this));
            }
            else
            {
                (
                    [&]
                    {
                        if (rpc::match<Interfaces>(interface_id))
                            out = static_cast<const Interfaces*>(static_cast<const Implementation*>(this));
                        return out != nullptr;
                    }()
                    || ...);
            }
            return out;
        }

        // overriden stub functionallity
        [[nodiscard]] std::shared_ptr<rpc::object_stub> __rpc_get_stub() const override { return stub_.lock(); }
        void __rpc_set_stub(const std::shared_ptr<rpc::object_stub>& stub) override { stub_ = stub; }

        CORO_TASK(rpc::send_result)
        __rpc_call(rpc::send_params params) override
        {
            send_result result{rpc::error::INVALID_INTERFACE_ID(), {}, {}};
            const auto interface_id = params.interface_id;
            [[maybe_unused]] bool found
                = ((rpc::match<Interfaces>(interface_id)
                           ? ((result = CO_AWAIT Interfaces::stub_caller::call(
                                   static_cast<Interfaces*>(static_cast<Implementation*>(this)), std::move(params))),
                                 true)
                           : false)
                    || ...);
            CO_RETURN result;
        }

        // Tier 2 (callee): describe every interface this object implements by
        // folding over the compile-time Interfaces pack — the same pack used by
        // __rpc_query_interface / __rpc_call above. Honours the deprecation
        // filter inside append_interface_descriptor.
        void __rpc_enumerate_schemas(
            rpc::encoding enc,
            rpc::schema_flavor flavor,
            bool include_deprecated,
            std::vector<rpc::interface_descriptor>& out) const override
        {
            (rpc::append_interface_descriptor<Interfaces>(enc, flavor, include_deprecated, out), ...);
        }
    };
}
