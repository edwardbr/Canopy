/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

// types.h and version.h are included by rpc.h
#include <string>
#include <vector>
#include <type_traits>
#include <stdint.h>
#include <memory>
#include <unordered_map>

namespace rpc
{
    class object_proxy;
    class object_stub;
    class service_proxy;
    class service;
    template<class T> class shared_ptr;

    // this is a nice helper function to match an interface id to a interface in a version independent way
    template<class T> bool match(rpc::interface_ordinal interface_id)
    {
        return T::get_id(rpc::get_version()) == interface_id;
    }

    // this is the base class to all interfaces
    class casting_interface
    {
    public:
        virtual ~casting_interface() = default;
        [[nodiscard]] virtual const rpc::casting_interface* __rpc_query_interface(rpc::interface_ordinal interface_id) const = 0;

        [[nodiscard]] virtual bool __rpc_is_local() const = 0;
        [[nodiscard]] virtual std::shared_ptr<rpc::object_proxy> __rpc_get_object_proxy() const = 0;

        // only for local objects
        [[nodiscard]] virtual std::shared_ptr<rpc::object_stub> __rpc_get_stub() const = 0;
        virtual void __rpc_set_stub(const std::shared_ptr<rpc::object_stub>&) = 0;

        virtual CORO_TASK(int) __rpc_call([[maybe_unused]] uint64_t protocol_version,
            [[maybe_unused]] encoding encoding,
            [[maybe_unused]] uint64_t tag,
            [[maybe_unused]] caller_zone caller_zone_id,
            [[maybe_unused]] destination_zone destination_zone_id,
            [[maybe_unused]] object object_id,
            [[maybe_unused]] interface_ordinal interface_id,
            [[maybe_unused]] method method_id,
            [[maybe_unused]] const rpc::byte_span& in_data,
            [[maybe_unused]] std::vector<char>& out_buf_,
            [[maybe_unused]] const std::vector<rpc::back_channel_entry>& in_back_channel,
            [[maybe_unused]] std::vector<rpc::back_channel_entry>& out_back_channel)
            = 0;

        static object get_object_id(const casting_interface& iface);
        static std::shared_ptr<rpc::service_proxy> get_service_proxy(const casting_interface& iface);
        static std::shared_ptr<rpc::service> get_service(const casting_interface& iface);
        static zone get_zone(const casting_interface& iface);
        static destination_zone get_destination_zone(const casting_interface& iface);
    };

    bool are_in_same_zone(const casting_interface* first, const casting_interface* second);

    // T is a class derived from casting_interface its role is to provide access to the object proxy to the remote zone
    template<class T> class interface_proxy : public T
    {
        stdex::member_ptr<object_proxy> object_proxy_;

    protected:
        [[nodiscard]] std::shared_ptr<object_proxy> get_object_proxy() const { return object_proxy_.get_nullable(); }
        void set_object_proxy(std::shared_ptr<object_proxy> ptr) { object_proxy_ = std::move(ptr); }

    public:
        interface_proxy(std::shared_ptr<rpc::object_proxy> object_proxy)
            : object_proxy_(object_proxy)
        {
        }
        ~interface_proxy() override = default;

        [[nodiscard]] const rpc::casting_interface* __rpc_query_interface(rpc::interface_ordinal interface_id) const override
        {
            if (rpc::match<T>(interface_id))
                return static_cast<const T*>(this);
            return nullptr;
        }

        [[nodiscard]] bool __rpc_is_local() const override { return false; }
        [[nodiscard]] std::shared_ptr<rpc::object_proxy> __rpc_get_object_proxy() const override
        {
            return object_proxy_.get_nullable();
        }

        // proxies dont do stub stuff
        void __rpc_set_stub(const std::shared_ptr<rpc::object_stub>&) override { RPC_ASSERT(false); }
        [[nodiscard]] std::shared_ptr<rpc::object_stub> __rpc_get_stub() const override
        {
            RPC_ASSERT(false);
            return nullptr;
        }

        CORO_TASK(int)
        __rpc_call([[maybe_unused]] uint64_t protocol_version,
            [[maybe_unused]] encoding encoding,
            [[maybe_unused]] uint64_t tag,
            [[maybe_unused]] caller_zone caller_zone_id,
            [[maybe_unused]] destination_zone destination_zone_id,
            [[maybe_unused]] object object_id,
            [[maybe_unused]] interface_ordinal interface_id,
            [[maybe_unused]] method method_id,
            [[maybe_unused]] const rpc::byte_span& in_data,
            [[maybe_unused]] std::vector<char>& out_buf_,
            [[maybe_unused]] const std::vector<rpc::back_channel_entry>& in_back_channel,
            [[maybe_unused]] std::vector<rpc::back_channel_entry>& out_back_channel) override
        {
            RPC_ASSERT(false);
            CO_RETURN rpc::error::INVALID_CAST();
        }
    };

    constexpr uint64_t STD_VECTOR_UINT_8_ID = 0x71FC1FAC5CD5E6FA;
    constexpr uint64_t STD_STRING_ID = 0x71FC1FAC5CD5E6F9;
    constexpr uint64_t UINT_8_ID = 0x71FC1FAC5CD5E6F8;
    constexpr uint64_t UINT_16_ID = 0x71FC1FAC5CD5E6F7;
    constexpr uint64_t UINT_32_ID = 0x71FC1FAC5CD5E6F6;
    constexpr uint64_t UINT_64_ID = 0x71FC1FAC5CD5E6F5;
    constexpr uint64_t INT_8_ID = 0x71FC1FAC5CD5E6F4;
    constexpr uint64_t INT_16_ID = 0x71FC1FAC5CD5E6F3;
    constexpr uint64_t INT_32_ID = 0x71FC1FAC5CD5E6F2;
    constexpr uint64_t INT_64_ID = 0x71FC1FAC5CD5E6F1;
    constexpr uint64_t FLOAT_32_ID = 0x71FC1FAC5CD5E6F0;
    constexpr uint64_t FLOAT_64_ID = 0x71FC1FAC5CD5E6EF;

    // declarations more can be added but these are the most commonly used

    template<> class id<std::string>
    {
    public:
        static constexpr uint64_t get(uint64_t) { return STD_STRING_ID; }
    };

    template<> class id<std::vector<uint8_t>>
    {
    public:
        static constexpr uint64_t get(uint64_t) { return STD_VECTOR_UINT_8_ID; }
    };

    template<> class id<uint8_t>
    {
    public:
        static constexpr uint64_t get(uint64_t) { return UINT_8_ID; }
    };

    template<> class id<uint16_t>
    {
    public:
        static constexpr uint64_t get(uint64_t) { return UINT_16_ID; }
    };

    template<> class id<uint32_t>
    {
    public:
        static constexpr uint64_t get(uint64_t) { return UINT_32_ID; }
    };

    template<> class id<uint64_t>
    {
    public:
        static constexpr uint64_t get(uint64_t) { return UINT_64_ID; }
    };

    template<> class id<int8_t>
    {
    public:
        static constexpr uint64_t get(uint64_t) { return INT_8_ID; }
    };

    template<> class id<int16_t>
    {
    public:
        static constexpr uint64_t get(uint64_t) { return INT_16_ID; }
    };

    template<> class id<int32_t>
    {
    public:
        static constexpr uint64_t get(uint64_t) { return INT_32_ID; }
    };

    template<> class id<int64_t>
    {
    public:
        static constexpr uint64_t get(uint64_t) { return INT_64_ID; }
    };

    template<> class id<float>
    {
    public:
        static constexpr uint64_t get(uint64_t) { return FLOAT_32_ID; }
    };

    template<> class id<double>
    {
    public:
        static constexpr uint64_t get(uint64_t) { return FLOAT_64_ID; }
    };
}
