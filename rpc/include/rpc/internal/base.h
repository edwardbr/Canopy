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
    public:
        virtual ~base() = default;

        // Query to see if this class supports an an interface
        const rpc::casting_interface* query_interface(rpc::interface_ordinal interface_id) const override
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

        // Get the address of the implementation, needed to do reverse lookups in the stub table and for
        // proper dynamic casting in clang and gcc, msvc is much more forgiving
        void* get_address() const override { return (void*)static_cast<const Implementation*>(this); }
    };
}
