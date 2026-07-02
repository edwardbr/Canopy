/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <rpc/rpc.h>

#include <algorithm>

namespace rpc
{
    namespace
    {
        void filter_interface(
            std::vector<rpc::interface_descriptor>& descriptors,
            rpc::optional<rpc::interface_ordinal> interface_id)
        {
            if (!interface_id.has_value() || interface_id->get_val() == 0)
                return;

            descriptors.erase(
                std::remove_if(
                    descriptors.begin(),
                    descriptors.end(),
                    [&](const rpc::interface_descriptor& descriptor) { return descriptor.interface_id != *interface_id; }),
                descriptors.end());
        }

        const rpc::function_info* find_method(
            const std::vector<rpc::interface_descriptor>& descriptors,
            rpc::interface_ordinal interface_id,
            rpc::method method_id)
        {
            for (const auto& descriptor : descriptors)
            {
                if (descriptor.interface_id != interface_id)
                    continue;

                for (const auto& method : descriptor.methods)
                {
                    if (method.id == method_id)
                        return &method;
                }
            }
            return nullptr;
        }

        rpc::encoding call_encoding(
            rpc::encoding requested,
            const std::shared_ptr<rpc::service_proxy>& service_proxy)
        {
            if (requested != rpc::encoding::not_set)
                return rpc::effective_encoding(requested);
            if (service_proxy)
                return rpc::effective_encoding(service_proxy->get_encoding());
            return rpc::effective_encoding(rpc::encoding::yas_json);
        }

    }

    bool are_in_same_zone(
        const casting_interface* first,
        const casting_interface* second)
    {
        // Consolidated null and locality checks
        if (!first || !second)
            return true;
        if (first->__rpc_is_local() || second->__rpc_is_local())
            return true;

        // Compare zones directly
        auto first_zone_id = casting_interface::get_zone(*first);
        auto second_zone_id = casting_interface::get_zone(*second);
        return first_zone_id == second_zone_id;
    }

    object casting_interface::get_object_id(const casting_interface& iface)
    {
        auto obj = iface.__rpc_get_object_proxy();
        if (!obj)
            return {0};
        return obj->get_object_id();
    }

    std::shared_ptr<rpc::service_proxy> casting_interface::get_service_proxy(const casting_interface& iface)
    {
        auto obj = iface.__rpc_get_object_proxy();
        if (!obj)
            return nullptr;
        return obj->get_service_proxy();
    }

    std::shared_ptr<rpc::service> casting_interface::get_service(const casting_interface& iface)
    {
        auto proxy = get_service_proxy(iface);
        if (!proxy)
            return nullptr;
        return proxy->get_operating_zone_service();
    }

    zone casting_interface::get_zone(const casting_interface& iface)
    {
        auto proxy = get_service_proxy(iface);
        if (!proxy)
            return zone();
        return proxy->get_zone_id();
    }

    destination_zone casting_interface::get_destination_zone(const casting_interface& iface)
    {
        auto proxy = get_service_proxy(iface);
        if (!proxy)
            return destination_zone();
        return proxy->get_destination_zone_id();
    }

    remote_object casting_interface::get_remote_object(const casting_interface& iface)
    {
        auto dest = get_destination_zone(iface);
        auto obj = get_object_id(iface);
        auto r = dest.with_object(obj);
        RPC_ASSERT(r.has_value());
        return std::move(*r);
    }

    std::vector<rpc::interface_descriptor> casting_interface::get_schema(
        const casting_interface& iface,
        rpc::encoding enc,
        rpc::schema_flavor flavor,
        bool include_deprecated)
    {
        std::vector<rpc::interface_descriptor> out;
        iface.__rpc_enumerate_schemas(enc, flavor, include_deprecated, out);
        return out;
    }

    // NOLINTBEGIN(cppcoreguidelines-avoid-reference-coroutine-parameters): this helper intentionally borrows
    // existing casting_interface/output objects; this is a stable API contract used by coroutine call sites.
    CORO_TASK(int)
    casting_interface::get_schema(
        const casting_interface& iface,
        std::vector<rpc::interface_descriptor>& out,
        rpc::encoding enc,
        rpc::schema_flavor flavor,
        bool include_deprecated,
        rpc::optional<rpc::interface_ordinal> interface_id)
    {
        out.clear();

        if (iface.__rpc_is_local())
        {
            iface.__rpc_enumerate_schemas(enc, flavor, include_deprecated, out);
            filter_interface(out, interface_id);
            CO_RETURN rpc::error::OK();
        }

        auto service_proxy = casting_interface::get_service_proxy(iface);
        if (!service_proxy)
            CO_RETURN rpc::error::ZONE_NOT_INITIALISED();

        auto result = CO_AWAIT service_proxy->sp_get_schema(
            service_proxy->get_destination_zone_id(),
            casting_interface::get_object_id(iface),
            interface_id,
            enc,
            flavor,
            include_deprecated);
        if (result.error_code != rpc::error::OK())
            CO_RETURN result.error_code;

        const auto* response = result.response_if_plain();
        if (!response)
            CO_RETURN rpc::error::INVALID_DATA();

        out = response->interfaces;
        filter_interface(out, interface_id);
        CO_RETURN rpc::error::OK();
    }
    // NOLINTEND(cppcoreguidelines-avoid-reference-coroutine-parameters)

    // NOLINTBEGIN(cppcoreguidelines-avoid-reference-coroutine-parameters): this helper intentionally borrows
    // an existing casting_interface object while the caller awaits the RPC call.
    CORO_TASK(rpc::send_result)
    casting_interface::call(
        const casting_interface& iface,
        rpc::send_params params,
        rpc::schema_flavor flavor,
        bool include_deprecated)
    {
        auto service_proxy = casting_interface::get_service_proxy(iface);
        params.encoding_type = call_encoding(params.encoding_type, service_proxy);

        std::vector<rpc::interface_descriptor> descriptors;
        auto err = CO_AWAIT casting_interface::get_schema(
            iface, descriptors, params.encoding_type, flavor, include_deprecated, params.interface_id);
        if (err != rpc::error::OK())
            CO_RETURN rpc::send_result{err, {}, {}};

        const auto* method = find_method(descriptors, params.interface_id, params.method_id);
        if (!method || method->post)
            CO_RETURN rpc::send_result{rpc::error::INVALID_METHOD_ID(), {}, {}};

        if (iface.__rpc_is_local())
        {
            if (params.protocol_version == 0)
                params.protocol_version = rpc::get_version();
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): local dispatch requires mutable interface access through a const validation path.
            CO_RETURN CO_AWAIT const_cast<rpc::casting_interface&>(iface).__rpc_call(std::move(params));
        }

        auto object_proxy = iface.__rpc_get_object_proxy();
        if (!object_proxy)
            CO_RETURN rpc::send_result{rpc::error::ZONE_NOT_INITIALISED(), {}, {}};

        service_proxy = object_proxy->get_service_proxy();
        if (!service_proxy)
            CO_RETURN rpc::send_result{rpc::error::ZONE_NOT_INITIALISED(), {}, {}};

        const auto protocol_version
            = params.protocol_version == 0 ? service_proxy->get_remote_rpc_version() : params.protocol_version;
        CO_RETURN CO_AWAIT object_proxy->send(
            protocol_version,
            params.encoding_type,
            params.tag,
            params.interface_id,
            params.method_id,
            std::move(params.in_data),
            params.request_id);
    }
    // NOLINTEND(cppcoreguidelines-avoid-reference-coroutine-parameters)

    // NOLINTBEGIN(cppcoreguidelines-avoid-reference-coroutine-parameters): this helper intentionally borrows
    // an existing casting_interface object while the caller awaits the RPC post.
    CORO_TASK(int)
    casting_interface::post(
        const casting_interface& iface,
        rpc::post_params params,
        rpc::schema_flavor flavor,
        bool include_deprecated)
    {
        auto service_proxy = casting_interface::get_service_proxy(iface);
        params.encoding_type = call_encoding(params.encoding_type, service_proxy);

        std::vector<rpc::interface_descriptor> descriptors;
        auto err = CO_AWAIT casting_interface::get_schema(
            iface, descriptors, params.encoding_type, flavor, include_deprecated, params.interface_id);
        if (err != rpc::error::OK())
            CO_RETURN err;

        const auto* method = find_method(descriptors, params.interface_id, params.method_id);
        if (!method || !method->post)
            CO_RETURN rpc::error::INVALID_METHOD_ID();

        if (iface.__rpc_is_local())
        {
            rpc::send_params send;
            send.protocol_version = params.protocol_version == 0 ? rpc::get_version() : params.protocol_version;
            send.encoding_type = params.encoding_type;
            send.tag = params.tag;
            send.caller_zone_id = params.caller_zone_id;
            send.remote_object_id = params.remote_object_id;
            send.interface_id = params.interface_id;
            send.method_id = params.method_id;
            send.in_data = std::move(params.in_data);
            send.in_back_channel = std::move(params.in_back_channel);
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): local dispatch requires mutable interface access through a const validation path.
            auto result = CO_AWAIT const_cast<rpc::casting_interface&>(iface).__rpc_call(std::move(send));
            CO_RETURN result.error_code;
        }

        auto object_proxy = iface.__rpc_get_object_proxy();
        if (!object_proxy)
            CO_RETURN rpc::error::ZONE_NOT_INITIALISED();

        service_proxy = object_proxy->get_service_proxy();
        if (!service_proxy)
            CO_RETURN rpc::error::ZONE_NOT_INITIALISED();

        const auto protocol_version
            = params.protocol_version == 0 ? service_proxy->get_remote_rpc_version() : params.protocol_version;
        CO_RETURN CO_AWAIT object_proxy->post(
            protocol_version, params.encoding_type, params.tag, params.interface_id, params.method_id, std::move(params.in_data));
    }
    // NOLINTEND(cppcoreguidelines-avoid-reference-coroutine-parameters)

    // (Removed dead commented-out function get_channel_zone)
}
