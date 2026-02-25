/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <rpc/rpc.h>

namespace rpc
{
    // This is for enclaves to call the host
    class host_service_proxy : public service_proxy
    {
        host_service_proxy(const char* name, destination_zone host_zone_id, const std::shared_ptr<rpc::child_service>& svc);
        host_service_proxy(const host_service_proxy& other) = default;

        std::shared_ptr<rpc::service_proxy> clone() override
        {
            return std::shared_ptr<host_service_proxy>(new host_service_proxy(*this));
        }

        static std::shared_ptr<rpc::service_proxy> create(
            const char* name, destination_zone host_zone_id, const std::shared_ptr<rpc::child_service>& svc);

        int initialise();

        int send(uint64_t protocol_version,
            encoding encoding,
            uint64_t tag,
            caller_zone caller_zone_id,
            destination_zone destination_zone_id,
            object object_id,
            interface_ordinal interface_id,
            method method_id,
            const rpc::span& in_data,
            std::vector<char>& out_buf_,
            const std::vector<rpc::back_channel_entry>& in_back_channel,
            std::vector<rpc::back_channel_entry>& out_back_channel) override;
        void post(uint64_t protocol_version,
            encoding encoding,
            uint64_t tag,
            caller_zone caller_zone_id,
            destination_zone destination_zone_id,
            object object_id,
            interface_ordinal interface_id,
            method method_id,
            const rpc::span& in_data,
            const std::vector<rpc::back_channel_entry>& in_back_channel) override;
        int try_cast(uint64_t protocol_version,
            destination_zone destination_zone_id,
            object object_id,
            interface_ordinal interface_id,
            const std::vector<rpc::back_channel_entry>& in_back_channel,
            std::vector<rpc::back_channel_entry>& out_back_channel) override;
        int add_ref(uint64_t protocol_version,
            destination_zone destination_zone_id,
            object object_id,
            caller_zone caller_zone_id,
            requesting_zone requesting_zone_id,
            add_ref_options build_out_param_channel,
            const std::vector<rpc::back_channel_entry>& in_back_channel,
            std::vector<rpc::back_channel_entry>& out_back_channel) override;
        int release(uint64_t protocol_version,
            destination_zone destination_zone_id,
            object object_id,
            caller_zone caller_zone_id,
            rpc::release_options options,
            const std::vector<rpc::back_channel_entry>& in_back_channel,
            std::vector<rpc::back_channel_entry>& out_back_channel) override;

        friend rpc::child_service;

    public:
        virtual ~host_service_proxy() = default;
    };
}
