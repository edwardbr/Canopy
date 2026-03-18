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

#include <rpc/internal/marshaller_params.h>

namespace rpc
{
    inline const std::vector<rpc::back_channel_entry>& empty_back_channel() noexcept
    {
        static const std::vector<rpc::back_channel_entry> empty;
        return empty;
    }

    // the used for marshalling data between zones
    class i_marshaller
    {
    public:
        virtual ~i_marshaller() = default;

        // send a function call to a different zone and expect a reply
        virtual CORO_TASK(send_result) send(send_params params) = 0;

        // post a function call to a different zone and not expect a reply (unidirectional), in synchronous builds this may still block
        virtual CORO_TASK(void) post(post_params params) = 0;

        // query if an object implements an interface
        virtual CORO_TASK(standard_result) try_cast(try_cast_params params) = 0;

        // add ownership of an object to a caller if shared or to just prop up the transport chain if optimistic
        virtual CORO_TASK(standard_result) add_ref(add_ref_params params) = 0;

        // notify that a zone is no longer interested in a remote object or transport chain
        virtual CORO_TASK(standard_result) release(release_params params) = 0;

        // notify callers that an object has been released (for callers with optimistic ref counts only) unidirectional call
        virtual CORO_TASK(void) object_released(object_released_params params) = 0;

        // notify callers that a transport is down unidirectional call
        virtual CORO_TASK(void) transport_down(transport_down_params params) = 0;

        // request a new zone id from the root zone
        virtual CORO_TASK(new_zone_id_result) get_new_zone_id(get_new_zone_id_params params) = 0;
    };

    struct retry_buffer
    {
        std::vector<char> data;
        int return_value;
    };

    struct connect_result
    {
        int error_code;
        rpc::remote_object output_descriptor;

        connect_result() = default;
        connect_result(int error_code, rpc::remote_object output_descriptor)
            : error_code(error_code)
            , output_descriptor(output_descriptor)
        {
        }
    };

}
