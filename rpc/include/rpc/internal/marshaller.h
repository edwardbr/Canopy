/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <string>
#include <memory>
#include <unordered_map>
#include <mutex>

// types.h, error_codes.h, and serialiser.h are included by rpc.h

namespace rpc
{
    // the used for marshalling data between zones
    class i_marshaller
    {
    public:
        virtual ~i_marshaller() = default;

        // send a function call to a different zone and expect a reply
        virtual CORO_TASK(int) send(uint64_t protocol_version,
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
            std::vector<rpc::back_channel_entry>& out_back_channel)
            = 0;

        // post a function call to a different zone and not expect a reply (unidirectional), in synchronous builds this may still block
        virtual CORO_TASK(void) post(uint64_t protocol_version,
            encoding encoding,
            uint64_t tag,
            caller_zone caller_zone_id,
            destination_zone destination_zone_id,
            object object_id,
            interface_ordinal interface_id,
            method method_id,
            const rpc::span& in_data,
            const std::vector<rpc::back_channel_entry>& in_back_channel)
            = 0;

        // query if an object implements an interface
        virtual CORO_TASK(int) try_cast(uint64_t protocol_version,
            caller_zone caller_zone_id,
            destination_zone destination_zone_id,
            object object_id,
            interface_ordinal interface_id,
            const std::vector<rpc::back_channel_entry>& in_back_channel,
            std::vector<rpc::back_channel_entry>& out_back_channel)
            = 0;

        // add ownership of an object to a caller if shared or to just prop up the transport chain if optimistic
        virtual CORO_TASK(int) add_ref(uint64_t protocol_version,
            destination_zone destination_zone_id,
            object object_id,
            caller_zone caller_zone_id,
            known_direction_zone known_direction_zone_id,
            add_ref_options build_out_param_channel,
            const std::vector<rpc::back_channel_entry>& in_back_channel,
            std::vector<rpc::back_channel_entry>& out_back_channel)
            = 0;

        // notify that a zone is no longer interested in a remote object or transport chain
        virtual CORO_TASK(int) release(uint64_t protocol_version,
            destination_zone destination_zone_id,
            object object_id,
            caller_zone caller_zone_id,
            release_options options,
            const std::vector<rpc::back_channel_entry>& in_back_channel,
            std::vector<rpc::back_channel_entry>& out_back_channel)
            = 0;

        // notify callers that an object has been released (for callers with optimistic ref counts only) unidirectional call
        virtual CORO_TASK(void) object_released(uint64_t protocol_version,
            destination_zone destination_zone_id,
            object object_id,
            caller_zone caller_zone_id,
            const std::vector<rpc::back_channel_entry>& in_back_channel)
            = 0;

        // notify callers that a transport is down unidirectional call
        virtual CORO_TASK(void) transport_down(uint64_t protocol_version,
            destination_zone destination_zone_id,
            caller_zone caller_zone_id,
            const std::vector<rpc::back_channel_entry>& in_back_channel)
            = 0;
    };

    struct retry_buffer
    {
        std::vector<char> data;
        int return_value;
    };

}
