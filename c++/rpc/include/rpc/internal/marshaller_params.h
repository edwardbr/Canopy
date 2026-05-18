/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <optional>
#include <vector>

// rpc_types.h, types.h, error_codes.h, and serialiser.h are included by rpc.h

namespace rpc
{
    // Input parameter bundles for i_marshaller methods.
    // All reference parameters are owned by-value to satisfy
    // cppcoreguidelines-avoid-reference-coroutine-parameters.

    template<typename T>
    [[nodiscard]] typed_payload make_typed_payload(
        const T& value,
        uint64_t protocol_version,
        encoding payload_encoding)
    {
        typed_payload payload;
        payload.set_value(value, protocol_version, payload_encoding);
        return payload;
    }

    struct send_params
    {
        uint64_t protocol_version;
        encoding encoding_type;
        uint64_t tag;
        caller_zone caller_zone_id;
        remote_object remote_object_id;
        interface_ordinal interface_id;
        method method_id;
        std::vector<char> in_data;
        std::vector<rpc::back_channel_entry> in_back_channel;
        uint64_t request_id = 0;
    };

    struct post_params
    {
        uint64_t protocol_version;
        encoding encoding_type;
        uint64_t tag;
        caller_zone caller_zone_id;
        remote_object remote_object_id;
        interface_ordinal interface_id;
        method method_id;
        std::vector<char> in_data;
        std::vector<rpc::back_channel_entry> in_back_channel;
    };

    struct try_cast_params
    {
        uint64_t protocol_version;
        caller_zone caller_zone_id;
        remote_object remote_object_id;
        interface_ordinal interface_id;
        std::vector<rpc::back_channel_entry> in_back_channel;
        std::optional<rpc::typed_payload> payload;
    };

    struct add_ref_params
    {
        uint64_t protocol_version;
        remote_object remote_object_id;
        caller_zone caller_zone_id;
        requesting_zone requesting_zone_id;
        add_ref_options build_out_param_channel;
        std::vector<rpc::back_channel_entry> in_back_channel;
        uint64_t request_id = 0;
        std::optional<rpc::typed_payload> payload;
    };

    struct release_params
    {
        uint64_t protocol_version;
        remote_object remote_object_id;
        caller_zone caller_zone_id;
        release_options options;
        std::vector<rpc::back_channel_entry> in_back_channel;
        std::optional<rpc::typed_payload> payload;
    };

    struct handshake_params
    {
        uint64_t protocol_version;
        caller_zone caller_zone_id;
        destination_zone destination_zone_id;
        uint64_t type_id = 0;
        encoding payload_encoding = encoding::not_set;
        std::vector<char> payload;
        std::vector<rpc::back_channel_entry> in_back_channel;
    };

    struct object_released_params
    {
        uint64_t protocol_version;
        remote_object remote_object_id;
        caller_zone caller_zone_id;
        std::vector<rpc::back_channel_entry> in_back_channel;
        std::optional<rpc::typed_payload> payload;
    };

    struct transport_down_params
    {
        uint64_t protocol_version;
        destination_zone destination_zone_id;
        caller_zone caller_zone_id;
        std::vector<rpc::back_channel_entry> in_back_channel;
        std::optional<rpc::typed_payload> payload;
    };

    struct get_new_zone_id_params
    {
        uint64_t protocol_version;
        std::vector<rpc::back_channel_entry> in_back_channel;
    };

    // Result structs - outputs travel up the return chain via move

    struct standard_result
    {
        int error_code;
        std::vector<rpc::back_channel_entry> out_back_channel;

        standard_result() = default;
        standard_result(
            int error_code,
            std::vector<rpc::back_channel_entry> out_back_channel)
            : error_code(error_code)
            , out_back_channel(std::move(out_back_channel))
        {
        }
    };

    struct handshake_result : standard_result
    {
        uint64_t type_id = 0;
        std::vector<char> payload;

        handshake_result() = default;
        handshake_result(
            int ec,
            uint64_t tid,
            std::vector<char> p,
            std::vector<rpc::back_channel_entry> bce)
            : standard_result(
                  ec,
                  std::move(bce))
            , type_id(tid)
            , payload(std::move(p))
        {
        }
    };

    struct send_result : standard_result
    {
        std::vector<char> out_buf;

        send_result() = default;
        send_result(
            int ec,
            std::vector<char> buf,
            std::vector<rpc::back_channel_entry> bce)
            : standard_result(
                  ec,
                  std::move(bce))
            , out_buf(std::move(buf))
        {
        }
    };

    struct new_zone_id_result : standard_result
    {
        zone zone_id;

        new_zone_id_result() = default;
        new_zone_id_result(
            int ec,
            zone z,
            std::vector<rpc::back_channel_entry> bce)
            : standard_result(
                  ec,
                  std::move(bce))
            , zone_id(z)
        {
        }
    };

} // namespace rpc
