/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <transports/c_abi/transport.h>

#ifndef CANOPY_BUILD_COROUTINE

#  include <rpc/internal/address_utils.h>

#  include <array>
#  include <cstdlib>
#  include <cstring>

namespace rpc::c_abi
{
    namespace
    {
        constexpr uint16_t internet_address_bytes = 16u;
        constexpr uint8_t version_bits = 8u;
        constexpr uint8_t address_type_bits = 3u;
        constexpr uint8_t has_port_bits = 1u;
        constexpr uint8_t has_validation_bits = 1u;
        constexpr uint8_t reserved_capability_bits = 3u;
        constexpr uint8_t size_field_bits = 8u;
        constexpr uint8_t port_bits = 16u;
        constexpr uint16_t version_offset_bits = 0u;
        constexpr uint16_t address_type_offset_bits = version_offset_bits + version_bits;
        constexpr uint16_t subnet_size_offset_bits = 16u;
        constexpr uint16_t object_id_size_offset_bits = 24u;
        constexpr uint16_t header_bits = rpc::default_values::capability_blob_bytes * 8u;
        constexpr uint8_t address_type_mask = 0x7u;
        constexpr uint8_t has_port_mask = 0x8u;
        constexpr uint8_t has_validation_mask = 0x10u;

        uint16_t address_bits_for_type(rpc::address_type type)
        {
            switch (type)
            {
            case rpc::address_type::local:
                return 0;
            case rpc::address_type::ipv4:
                return 32;
            case rpc::address_type::ipv6:
            case rpc::address_type::ipv6_tun:
                return 128;
            }

            return 0;
        }

        canopy_const_byte_buffer borrow_bytes(const std::vector<char>& bytes)
        {
            return canopy_const_byte_buffer{reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()};
        }

        canopy_const_byte_buffer borrow_bytes(const std::vector<uint8_t>& bytes)
        {
            return canopy_const_byte_buffer{bytes.data(), bytes.size()};
        }

        canopy_zone_address borrow_zone_address(const rpc::zone_address& address)
        {
            const auto& blob = address.get_blob();
            return canopy_zone_address{borrow_bytes(blob)};
        }

        canopy_zone borrow_zone(const rpc::zone& zone)
        {
            return canopy_zone{borrow_zone_address(zone.get_address())};
        }

        canopy_remote_object borrow_remote_object(const rpc::remote_object& remote_object)
        {
            return canopy_remote_object{borrow_zone_address(remote_object.get_address())};
        }

        struct borrowed_back_channel
        {
            std::vector<canopy_back_channel_entry> entries;
            canopy_back_channel_span span{};

            borrowed_back_channel() = default;

            explicit borrowed_back_channel(const std::vector<rpc::back_channel_entry>& source)
            {
                entries.reserve(source.size());
                for (const auto& entry : source)
                {
                    entries.push_back(canopy_back_channel_entry{entry.type_id, borrow_bytes(entry.payload)});
                }

                span.data = entries.data();
                span.size = entries.size();
            }
        };

        struct borrowed_connection_settings
        {
            canopy_remote_object remote_object{};
            canopy_connection_settings raw{};

            explicit borrowed_connection_settings(const rpc::connection_settings& input_descr)
                : remote_object(borrow_remote_object(input_descr.remote_object_id))
                , raw{input_descr.inbound_interface_id.get_val(),
                      input_descr.outbound_interface_id.get_val(),
                      remote_object}
            {
            }
        };

        struct borrowed_send_params
        {
            borrowed_back_channel back_channel;
            canopy_zone caller_zone{};
            canopy_remote_object remote_object{};
            canopy_send_params raw{};

            explicit borrowed_send_params(const rpc::send_params& params)
                : back_channel(params.in_back_channel)
                , caller_zone(borrow_zone(params.caller_zone_id))
                , remote_object(borrow_remote_object(params.remote_object_id))
                , raw{params.protocol_version,
                      static_cast<uint64_t>(params.encoding_type),
                      params.tag,
                      caller_zone,
                      remote_object,
                      params.interface_id.get_val(),
                      params.method_id.get_val(),
                      borrow_bytes(params.in_data),
                      back_channel.span,
                      params.request_id}
            {
            }
        };

        struct borrowed_post_params
        {
            borrowed_back_channel back_channel;
            canopy_zone caller_zone{};
            canopy_remote_object remote_object{};
            canopy_post_params raw{};

            explicit borrowed_post_params(const rpc::post_params& params)
                : back_channel(params.in_back_channel)
                , caller_zone(borrow_zone(params.caller_zone_id))
                , remote_object(borrow_remote_object(params.remote_object_id))
                , raw{params.protocol_version,
                      static_cast<uint64_t>(params.encoding_type),
                      params.tag,
                      caller_zone,
                      remote_object,
                      params.interface_id.get_val(),
                      params.method_id.get_val(),
                      borrow_bytes(params.in_data),
                      back_channel.span}
            {
            }
        };

        struct borrowed_try_cast_params
        {
            borrowed_back_channel back_channel;
            canopy_zone caller_zone{};
            canopy_remote_object remote_object{};
            canopy_try_cast_params raw{};

            explicit borrowed_try_cast_params(const rpc::try_cast_params& params)
                : back_channel(params.in_back_channel)
                , caller_zone(borrow_zone(params.caller_zone_id))
                , remote_object(borrow_remote_object(params.remote_object_id))
                , raw{params.protocol_version,
                      caller_zone,
                      remote_object,
                      params.interface_id.get_val(),
                      back_channel.span}
            {
            }
        };

        struct borrowed_add_ref_params
        {
            borrowed_back_channel back_channel;
            canopy_remote_object remote_object{};
            canopy_zone caller_zone{};
            canopy_zone requesting_zone{};
            canopy_add_ref_params raw{};

            explicit borrowed_add_ref_params(const rpc::add_ref_params& params)
                : back_channel(params.in_back_channel)
                , remote_object(borrow_remote_object(params.remote_object_id))
                , caller_zone(borrow_zone(params.caller_zone_id))
                , requesting_zone(borrow_zone(params.requesting_zone_id))
                , raw{params.protocol_version,
                      remote_object,
                      caller_zone,
                      requesting_zone,
                      static_cast<uint8_t>(params.build_out_param_channel),
                      back_channel.span,
                      params.request_id}
            {
            }
        };

        struct borrowed_release_params
        {
            borrowed_back_channel back_channel;
            canopy_remote_object remote_object{};
            canopy_zone caller_zone{};
            canopy_release_params raw{};

            explicit borrowed_release_params(const rpc::release_params& params)
                : back_channel(params.in_back_channel)
                , remote_object(borrow_remote_object(params.remote_object_id))
                , caller_zone(borrow_zone(params.caller_zone_id))
                , raw{params.protocol_version,
                      remote_object,
                      caller_zone,
                      static_cast<uint8_t>(params.options),
                      back_channel.span}
            {
            }
        };

        struct borrowed_object_released_params
        {
            borrowed_back_channel back_channel;
            canopy_remote_object remote_object{};
            canopy_zone caller_zone{};
            canopy_object_released_params raw{};

            explicit borrowed_object_released_params(const rpc::object_released_params& params)
                : back_channel(params.in_back_channel)
                , remote_object(borrow_remote_object(params.remote_object_id))
                , caller_zone(borrow_zone(params.caller_zone_id))
                , raw{params.protocol_version,
                      remote_object,
                      caller_zone,
                      back_channel.span}
            {
            }
        };

        struct borrowed_transport_down_params
        {
            borrowed_back_channel back_channel;
            canopy_zone destination_zone{};
            canopy_zone caller_zone{};
            canopy_transport_down_params raw{};

            explicit borrowed_transport_down_params(const rpc::transport_down_params& params)
                : back_channel(params.in_back_channel)
                , destination_zone(borrow_zone(params.destination_zone_id))
                , caller_zone(borrow_zone(params.caller_zone_id))
                , raw{params.protocol_version,
                      destination_zone,
                      caller_zone,
                      back_channel.span}
            {
            }
        };

        struct borrowed_get_new_zone_id_params
        {
            borrowed_back_channel back_channel;
            canopy_get_new_zone_id_params raw{};

            explicit borrowed_get_new_zone_id_params(const rpc::get_new_zone_id_params& params)
                : back_channel(params.in_back_channel)
                , raw{params.protocol_version,
                      back_channel.span}
            {
            }
        };

        std::vector<uint8_t> copy_bytes(const canopy_const_byte_buffer& bytes)
        {
            if (!bytes.data || bytes.size == 0)
                return {};

            return std::vector<uint8_t>(bytes.data, bytes.data + bytes.size);
        }

        std::vector<char> copy_chars(const canopy_byte_buffer& bytes)
        {
            if (!bytes.data || bytes.size == 0)
                return {};

            auto start = reinterpret_cast<const char*>(bytes.data);
            return std::vector<char>(start, start + bytes.size);
        }

        std::vector<uint8_t> extract_host_bytes(
            const std::vector<uint8_t>& blob,
            uint16_t address_offset_bits,
            uint16_t address_bits)
        {
            std::vector<uint8_t> host(internet_address_bytes, 0);
            auto byte_offset = static_cast<size_t>(address_offset_bits / 8u);
            auto byte_count = static_cast<size_t>(address_bits / 8u);
            for (size_t i = 0; i < byte_count && i < host.size() && byte_offset + i < blob.size(); ++i)
                host[i] = blob[byte_offset + i];
            return host;
        }

        std::vector<uint8_t> extract_prefix_bits(
            const std::vector<uint8_t>& host,
            uint16_t width_bits)
        {
            auto required_bytes = static_cast<size_t>((width_bits + 7u) / 8u);
            std::vector<uint8_t> prefix(required_bytes, 0);
            for (size_t i = 0; i < required_bytes && i < host.size(); ++i)
                prefix[i] = host[i];

            if (required_bytes == 0)
                return prefix;

            auto leading_unused_bits = static_cast<uint8_t>((required_bytes * 8u) - width_bits);
            if (leading_unused_bits != 0)
            {
                auto mask = static_cast<uint8_t>(0xffu >> leading_unused_bits);
                prefix.front() = static_cast<uint8_t>(prefix.front() & mask);
            }

            return prefix;
        }

        [[maybe_unused]] rpc::expected<
            rpc::zone_address_args,
            std::string>
        decode_zone_address_args(const canopy_zone_address& address)
        {
            auto blob = copy_bytes(address.blob);
            if (blob.size() < rpc::default_values::capability_blob_bytes)
                return rpc::unexpected<std::string>("c_abi zone_address blob is too small");

            auto version = static_cast<uint8_t>(rpc::get_bits_le(blob, version_offset_bits, version_bits));
            auto header_byte = static_cast<uint8_t>(rpc::get_bits_le(
                blob,
                address_type_offset_bits,
                static_cast<uint16_t>(address_type_bits + has_port_bits + has_validation_bits + reserved_capability_bits)));
            if ((header_byte & static_cast<uint8_t>(~(address_type_mask | has_port_mask | has_validation_mask))) != 0)
                return rpc::unexpected<std::string>("c_abi zone_address blob uses reserved capability bits");

            auto type = static_cast<rpc::address_type>(header_byte & address_type_mask);
            auto has_port = (header_byte & has_port_mask) != 0;
            auto has_validation = (header_byte & has_validation_mask) != 0;
            auto subnet_size_bits = static_cast<uint8_t>(rpc::get_bits_le(blob, subnet_size_offset_bits, size_field_bits));
            auto object_id_size_bits
                = static_cast<uint8_t>(rpc::get_bits_le(blob, object_id_size_offset_bits, size_field_bits));
            auto address_bits = address_bits_for_type(type);
            auto address_offset_bits = static_cast<uint16_t>(header_bits + (has_port ? port_bits : 0u));
            auto subnet_offset_bits = static_cast<uint16_t>(address_offset_bits + address_bits);
            auto object_offset_bits = static_cast<uint16_t>(subnet_offset_bits + subnet_size_bits);
            auto validation_offset_bits = type == rpc::address_type::ipv6_tun
                                              ? static_cast<uint16_t>(address_offset_bits + 128u)
                                              : static_cast<uint16_t>(object_offset_bits + object_id_size_bits);

            auto port = has_port ? static_cast<uint16_t>(rpc::get_bits_le(blob, header_bits, port_bits)) : 0u;
            auto host = extract_host_bytes(blob, address_offset_bits, address_bits);

            std::vector<uint8_t> routing_prefix;
            uint64_t subnet = 0;
            uint64_t object_id = 0;

            if (type == rpc::address_type::ipv6_tun)
            {
                auto routing_bits = static_cast<uint16_t>(128u - subnet_size_bits - object_id_size_bits);
                routing_prefix = extract_prefix_bits(host, routing_bits);
                subnet = rpc::get_bits_be(host, routing_bits, subnet_size_bits);
                object_id
                    = rpc::get_bits_be(host, static_cast<uint16_t>(routing_bits + subnet_size_bits), object_id_size_bits);
            }
            else
            {
                routing_prefix = extract_prefix_bits(host, address_bits);
                subnet = rpc::get_bits_le(blob, subnet_offset_bits, subnet_size_bits);
                object_id = rpc::get_bits_le(blob, object_offset_bits, object_id_size_bits);
            }

            std::vector<uint8_t> validation_bits;
            if (has_validation)
            {
                auto validation_offset_bytes = static_cast<size_t>(validation_offset_bits / 8u);
                if (validation_offset_bytes > blob.size())
                    return rpc::unexpected<std::string>("c_abi zone_address validation offset is invalid");
                validation_bits.assign(blob.begin() + static_cast<std::ptrdiff_t>(validation_offset_bytes), blob.end());
            }

            return rpc::zone_address_args(
                version, type, port, routing_prefix, subnet_size_bits, subnet, object_id_size_bits, object_id, validation_bits);
        }

        rpc::expected<
            rpc::zone_address,
            std::string>
        decode_zone_address(const canopy_zone_address& address)
        {
            return rpc::zone_address::from_blob(copy_bytes(address.blob));
        }

        rpc::expected<
            rpc::zone,
            std::string>
        decode_zone(const canopy_zone& zone)
        {
            auto address = decode_zone_address(zone.address);
            if (!address)
                return rpc::unexpected<std::string>(std::move(address.error()));
            return rpc::zone(*address);
        }

        rpc::expected<
            rpc::remote_object,
            std::string>
        decode_remote_object(const canopy_remote_object& remote_object)
        {
            auto address = decode_zone_address(remote_object.address);
            if (!address)
                return rpc::unexpected<std::string>(std::move(address.error()));
            return rpc::remote_object(*address);
        }

        rpc::expected<
            std::vector<rpc::back_channel_entry>,
            std::string>
        decode_back_channel(const canopy_back_channel_span& span)
        {
            std::vector<rpc::back_channel_entry> result;
            result.reserve(span.size);
            for (size_t i = 0; i < span.size; ++i)
            {
                const auto& entry = span.data[i];
                result.push_back(rpc::back_channel_entry{entry.type_id, copy_bytes(entry.payload)});
            }
            return result;
        }

        rpc::expected<
            rpc::send_params,
            std::string>
        decode_send_params(const canopy_send_params& params)
        {
            auto caller_zone = decode_zone(params.caller_zone_id);
            if (!caller_zone)
                return rpc::unexpected<std::string>(std::move(caller_zone.error()));
            auto remote_object = decode_remote_object(params.remote_object_id);
            if (!remote_object)
                return rpc::unexpected<std::string>(std::move(remote_object.error()));
            auto back_channel = decode_back_channel(params.in_back_channel);
            if (!back_channel)
                return rpc::unexpected<std::string>(std::move(back_channel.error()));

            return rpc::send_params{params.protocol_version,
                static_cast<rpc::encoding>(params.encoding_type),
                params.tag,
                *caller_zone,
                *remote_object,
                rpc::interface_ordinal(params.interface_id),
                rpc::method(params.method_id),
                copy_chars(canopy_byte_buffer{const_cast<uint8_t*>(params.in_data.data), params.in_data.size}),
                std::move(*back_channel),
                params.request_id};
        }

        rpc::expected<
            rpc::post_params,
            std::string>
        decode_post_params(const canopy_post_params& params)
        {
            auto caller_zone = decode_zone(params.caller_zone_id);
            if (!caller_zone)
                return rpc::unexpected<std::string>(std::move(caller_zone.error()));
            auto remote_object = decode_remote_object(params.remote_object_id);
            if (!remote_object)
                return rpc::unexpected<std::string>(std::move(remote_object.error()));
            auto back_channel = decode_back_channel(params.in_back_channel);
            if (!back_channel)
                return rpc::unexpected<std::string>(std::move(back_channel.error()));

            return rpc::post_params{params.protocol_version,
                static_cast<rpc::encoding>(params.encoding_type),
                params.tag,
                *caller_zone,
                *remote_object,
                rpc::interface_ordinal(params.interface_id),
                rpc::method(params.method_id),
                copy_chars(canopy_byte_buffer{const_cast<uint8_t*>(params.in_data.data), params.in_data.size}),
                std::move(*back_channel)};
        }

        rpc::expected<
            rpc::try_cast_params,
            std::string>
        decode_try_cast_params(const canopy_try_cast_params& params)
        {
            auto caller_zone = decode_zone(params.caller_zone_id);
            if (!caller_zone)
                return rpc::unexpected<std::string>(std::move(caller_zone.error()));
            auto remote_object = decode_remote_object(params.remote_object_id);
            if (!remote_object)
                return rpc::unexpected<std::string>(std::move(remote_object.error()));
            auto back_channel = decode_back_channel(params.in_back_channel);
            if (!back_channel)
                return rpc::unexpected<std::string>(std::move(back_channel.error()));

            return rpc::try_cast_params{params.protocol_version,
                *caller_zone,
                *remote_object,
                rpc::interface_ordinal(params.interface_id),
                std::move(*back_channel)};
        }

        rpc::expected<
            rpc::add_ref_params,
            std::string>
        decode_add_ref_params(const canopy_add_ref_params& params)
        {
            auto remote_object = decode_remote_object(params.remote_object_id);
            if (!remote_object)
                return rpc::unexpected<std::string>(std::move(remote_object.error()));
            auto caller_zone = decode_zone(params.caller_zone_id);
            if (!caller_zone)
                return rpc::unexpected<std::string>(std::move(caller_zone.error()));
            auto requesting_zone = decode_zone(params.requesting_zone_id);
            if (!requesting_zone)
                return rpc::unexpected<std::string>(std::move(requesting_zone.error()));
            auto back_channel = decode_back_channel(params.in_back_channel);
            if (!back_channel)
                return rpc::unexpected<std::string>(std::move(back_channel.error()));

            return rpc::add_ref_params{params.protocol_version,
                *remote_object,
                *caller_zone,
                *requesting_zone,
                static_cast<rpc::add_ref_options>(params.build_out_param_channel),
                std::move(*back_channel),
                params.request_id};
        }

        rpc::expected<
            rpc::release_params,
            std::string>
        decode_release_params(const canopy_release_params& params)
        {
            auto remote_object = decode_remote_object(params.remote_object_id);
            if (!remote_object)
                return rpc::unexpected<std::string>(std::move(remote_object.error()));
            auto caller_zone = decode_zone(params.caller_zone_id);
            if (!caller_zone)
                return rpc::unexpected<std::string>(std::move(caller_zone.error()));
            auto back_channel = decode_back_channel(params.in_back_channel);
            if (!back_channel)
                return rpc::unexpected<std::string>(std::move(back_channel.error()));

            return rpc::release_params{params.protocol_version,
                *remote_object,
                *caller_zone,
                static_cast<rpc::release_options>(params.options),
                std::move(*back_channel)};
        }

        rpc::expected<
            rpc::object_released_params,
            std::string>
        decode_object_released_params(const canopy_object_released_params& params)
        {
            auto remote_object = decode_remote_object(params.remote_object_id);
            if (!remote_object)
                return rpc::unexpected<std::string>(std::move(remote_object.error()));
            auto caller_zone = decode_zone(params.caller_zone_id);
            if (!caller_zone)
                return rpc::unexpected<std::string>(std::move(caller_zone.error()));
            auto back_channel = decode_back_channel(params.in_back_channel);
            if (!back_channel)
                return rpc::unexpected<std::string>(std::move(back_channel.error()));

            return rpc::object_released_params{
                params.protocol_version, *remote_object, *caller_zone, std::move(*back_channel)};
        }

        rpc::expected<
            rpc::transport_down_params,
            std::string>
        decode_transport_down_params(const canopy_transport_down_params& params)
        {
            auto destination_zone = decode_zone(params.destination_zone_id);
            if (!destination_zone)
                return rpc::unexpected<std::string>(std::move(destination_zone.error()));
            auto caller_zone = decode_zone(params.caller_zone_id);
            if (!caller_zone)
                return rpc::unexpected<std::string>(std::move(caller_zone.error()));
            auto back_channel = decode_back_channel(params.in_back_channel);
            if (!back_channel)
                return rpc::unexpected<std::string>(std::move(back_channel.error()));

            return rpc::transport_down_params{
                params.protocol_version, *destination_zone, *caller_zone, std::move(*back_channel)};
        }

        rpc::expected<
            rpc::get_new_zone_id_params,
            std::string>
        decode_get_new_zone_id_params(const canopy_get_new_zone_id_params& params)
        {
            auto back_channel = decode_back_channel(params.in_back_channel);
            if (!back_channel)
                return rpc::unexpected<std::string>(std::move(back_channel.error()));

            return rpc::get_new_zone_id_params{params.protocol_version, std::move(*back_channel)};
        }

        void free_zone_address(
            const canopy_allocator_vtable& allocator,
            canopy_zone_address* address)
        {
            if (!address->blob.data || !address->blob.size)
                return;

            allocator.free(allocator.allocator_ctx, const_cast<uint8_t*>(address->blob.data), address->blob.size);
            address->blob = {};
        }

        void free_back_channel(
            const canopy_allocator_vtable& allocator,
            canopy_mut_back_channel_span* back_channel)
        {
            if (back_channel->data)
            {
                for (size_t i = 0; i < back_channel->size; ++i)
                {
                    auto& entry = back_channel->data[i];
                    if (entry.payload.data && entry.payload.size)
                        allocator.free(
                            allocator.allocator_ctx, const_cast<uint8_t*>(entry.payload.data), entry.payload.size);
                }

                allocator.free(
                    allocator.allocator_ctx,
                    reinterpret_cast<uint8_t*>(back_channel->data),
                    back_channel->size * sizeof(canopy_back_channel_entry));
            }

            back_channel->data = nullptr;
            back_channel->size = 0;
        }

        void free_standard_result(
            const canopy_allocator_vtable& allocator,
            canopy_standard_result* result)
        {
            free_back_channel(allocator, &result->out_back_channel);
            result->error_code = 0;
        }

        void free_send_result(
            const canopy_allocator_vtable& allocator,
            canopy_send_result* result)
        {
            if (result->out_buf.data && result->out_buf.size)
                allocator.free(allocator.allocator_ctx, result->out_buf.data, result->out_buf.size);
            result->out_buf = {};
            free_back_channel(allocator, &result->out_back_channel);
            result->error_code = 0;
        }

        void free_new_zone_id_result(
            const canopy_allocator_vtable& allocator,
            canopy_new_zone_id_result* result)
        {
            free_zone_address(allocator, &result->zone_id.address);
            free_back_channel(allocator, &result->out_back_channel);
            result->error_code = 0;
        }

        int allocate_buffer(
            const canopy_allocator_vtable& allocator,
            size_t size,
            canopy_byte_buffer* output)
        {
            *output = {};
            if (size == 0)
                return rpc::error::OK();

            auto allocated = allocator.alloc(allocator.allocator_ctx, size);
            if (!allocated.data || allocated.size < size)
            {
                if (allocated.data && allocator.free)
                    allocator.free(allocator.allocator_ctx, allocated.data, allocated.size);
                return rpc::error::OUT_OF_MEMORY();
            }

            *output = allocated;
            return rpc::error::OK();
        }

        int write_zone_address(
            const canopy_allocator_vtable& allocator,
            const rpc::zone_address& address,
            canopy_zone_address* output)
        {
            *output = {};
            const auto& blob = address.get_blob();
            canopy_byte_buffer buffer{};
            auto err = allocate_buffer(allocator, blob.size(), &buffer);
            if (err != rpc::error::OK())
                return err;

            if (buffer.data && !blob.empty())
                std::memcpy(buffer.data, blob.data(), blob.size());
            output->blob.data = buffer.data;
            output->blob.size = buffer.size;
            return rpc::error::OK();
        }

        int write_back_channel(
            const canopy_allocator_vtable& allocator,
            const std::vector<rpc::back_channel_entry>& source,
            canopy_mut_back_channel_span* output)
        {
            output->data = nullptr;
            output->size = 0;
            if (source.empty())
                return rpc::error::OK();

            canopy_byte_buffer entries_buffer{};
            auto err = allocate_buffer(allocator, source.size() * sizeof(canopy_back_channel_entry), &entries_buffer);
            if (err != rpc::error::OK())
                return err;

            auto* entries = reinterpret_cast<canopy_back_channel_entry*>(entries_buffer.data);
            for (size_t i = 0; i < source.size(); ++i)
                entries[i] = {};

            for (size_t i = 0; i < source.size(); ++i)
            {
                const auto& entry = source[i];
                entries[i].type_id = entry.type_id;

                canopy_byte_buffer payload{};
                err = allocate_buffer(allocator, entry.payload.size(), &payload);
                if (err != rpc::error::OK())
                {
                    output->data = entries;
                    output->size = source.size();
                    free_back_channel(allocator, output);
                    return err;
                }

                if (payload.data && !entry.payload.empty())
                    std::memcpy(payload.data, entry.payload.data(), entry.payload.size());
                entries[i].payload.data = payload.data;
                entries[i].payload.size = payload.size;
            }

            output->data = entries;
            output->size = source.size();
            return rpc::error::OK();
        }

        int write_standard_result(
            const canopy_allocator_vtable& allocator,
            const rpc::standard_result& source,
            canopy_standard_result* output)
        {
            *output = {};
            output->error_code = source.error_code;
            auto err = write_back_channel(allocator, source.out_back_channel, &output->out_back_channel);
            if (err != rpc::error::OK())
            {
                free_standard_result(allocator, output);
                output->error_code = err;
                return err;
            }
            return output->error_code;
        }

        int write_send_result(
            const canopy_allocator_vtable& allocator,
            const rpc::send_result& source,
            canopy_send_result* output)
        {
            *output = {};
            output->error_code = source.error_code;
            auto err = allocate_buffer(allocator, source.out_buf.size(), &output->out_buf);
            if (err != rpc::error::OK())
            {
                output->error_code = err;
                return err;
            }
            if (output->out_buf.data && !source.out_buf.empty())
                std::memcpy(output->out_buf.data, source.out_buf.data(), source.out_buf.size());

            err = write_back_channel(allocator, source.out_back_channel, &output->out_back_channel);
            if (err != rpc::error::OK())
            {
                free_send_result(allocator, output);
                output->error_code = err;
                return err;
            }
            return output->error_code;
        }

        int write_new_zone_id_result(
            const canopy_allocator_vtable& allocator,
            const rpc::new_zone_id_result& source,
            canopy_new_zone_id_result* output)
        {
            *output = {};
            output->error_code = source.error_code;
            auto err = write_zone_address(allocator, source.zone_id.get_address(), &output->zone_id.address);
            if (err != rpc::error::OK())
            {
                output->error_code = err;
                return err;
            }

            err = write_back_channel(allocator, source.out_back_channel, &output->out_back_channel);
            if (err != rpc::error::OK())
            {
                free_new_zone_id_result(allocator, output);
                output->error_code = err;
                return err;
            }
            return output->error_code;
        }

        rpc::standard_result copy_standard_result(const canopy_standard_result& source)
        {
            auto out_back_channel = decode_back_channel(
                canopy_back_channel_span{source.out_back_channel.data, source.out_back_channel.size});
            if (!out_back_channel)
                return rpc::standard_result{rpc::error::INVALID_DATA(), {}};
            return rpc::standard_result{source.error_code, std::move(*out_back_channel)};
        }

        rpc::send_result copy_send_result(const canopy_send_result& source)
        {
            auto out_back_channel = decode_back_channel(
                canopy_back_channel_span{source.out_back_channel.data, source.out_back_channel.size});
            if (!out_back_channel)
                return rpc::send_result{rpc::error::INVALID_DATA(), {}, {}};
            return rpc::send_result{source.error_code, copy_chars(source.out_buf), std::move(*out_back_channel)};
        }

        [[maybe_unused]] rpc::new_zone_id_result copy_new_zone_id_result(const canopy_new_zone_id_result& source)
        {
            auto zone = decode_zone(source.zone_id);
            if (!zone)
                return rpc::new_zone_id_result{rpc::error::INVALID_DATA(), {}, {}};
            auto out_back_channel = decode_back_channel(
                canopy_back_channel_span{source.out_back_channel.data, source.out_back_channel.size});
            if (!out_back_channel)
                return rpc::new_zone_id_result{rpc::error::INVALID_DATA(), {}, {}};

            return rpc::new_zone_id_result{source.error_code, *zone, std::move(*out_back_channel)};
        }
    } // namespace

    child_transport::child_transport(
        std::string name,
        std::shared_ptr<rpc::service> service,
        std::string library_path)
        : rpc::transport(
              name,
              service)
        , allocator_{this,
              &child_transport::cb_alloc,
              &child_transport::cb_free}
        , library_path_(std::move(library_path))
    {
    }

    child_transport::~child_transport()
    {
        if (child_ctx_ && loader_.exports().destroy)
        {
            loader_.exports().destroy(child_ctx_);
            child_ctx_ = nullptr;
        }

        unload_library();
    }

    canopy_byte_buffer child_transport::cb_alloc(
        void*,
        size_t size)
    {
        if (size == 0)
            return {};

        auto* data = static_cast<uint8_t*>(std::malloc(size));
        if (!data)
            return {};

        return canopy_byte_buffer{data, size};
    }

    void child_transport::cb_free(
        void*,
        uint8_t* data,
        size_t)
    {
        std::free(data);
    }

    int child_transport::load_library()
    {
        if (!loader_.load(library_path_))
            return rpc::error::TRANSPORT_ERROR();

        return rpc::error::OK();
    }

    void child_transport::unload_library()
    {
        child_ctx_ = nullptr;
        loader_.unload();
    }

    void child_transport::on_destination_count_zero()
    {
        if (child_ctx_ && loader_.exports().destroy)
        {
            loader_.exports().destroy(child_ctx_);
            child_ctx_ = nullptr;
        }

        unload_library();
        set_status(rpc::transport_status::DISCONNECTED);
    }

    void child_transport::set_status(rpc::transport_status status)
    {
        rpc::transport::set_status(status);

        if (status == rpc::transport_status::DISCONNECTED)
        {
            child_ctx_ = nullptr;
        }
    }

    CORO_TASK(rpc::connect_result)
    child_transport::inner_connect(
        std::shared_ptr<rpc::object_stub> stub,
        connection_settings input_descr)
    {
        auto svc = get_service();

        get_new_zone_id_params zone_params;
        zone_params.protocol_version = rpc::get_version();
        auto zone_result = CO_AWAIT svc->get_new_zone_id(std::move(zone_params));
        if (zone_result.error_code != rpc::error::OK())
            CO_RETURN rpc::connect_result{zone_result.error_code, {}};

        rpc::zone adjacent_zone_id = zone_result.zone_id;
        set_adjacent_zone_id(adjacent_zone_id);
        svc->add_transport(adjacent_zone_id, shared_from_this());

        if (stub)
        {
            auto ret = CO_AWAIT stub->add_ref(false, false, adjacent_zone_id);
            if (ret != rpc::error::OK())
                CO_RETURN rpc::connect_result{ret, {}};
        }

        if (int load_err = load_library(); load_err != rpc::error::OK())
            CO_RETURN rpc::connect_result{load_err, {}};

        std::string transport_name = get_name();
        borrowed_connection_settings borrowed_input_descr(input_descr);
        auto parent_zone_id = get_zone_id();
        auto parent_zone = borrow_zone(parent_zone_id);
        auto child_zone = borrow_zone(adjacent_zone_id);

        canopy_dll_init_params init_params{};
        init_params.name = transport_name.c_str();
        init_params.parent_zone = parent_zone;
        init_params.child_zone = child_zone;
        init_params.input_descr = &borrowed_input_descr.raw;
        init_params.parent_ctx = this;
        init_params.allocator = allocator_;
        init_params.parent_send = &child_transport::cb_send;
        init_params.parent_post = &child_transport::cb_post;
        init_params.parent_try_cast = &child_transport::cb_try_cast;
        init_params.parent_add_ref = &child_transport::cb_add_ref;
        init_params.parent_release = &child_transport::cb_release;
        init_params.parent_object_released = &child_transport::cb_object_released;
        init_params.parent_transport_down = &child_transport::cb_transport_down;
        init_params.parent_get_new_zone_id = &child_transport::cb_get_new_zone_id;

        auto init_err = loader_.exports().init(&init_params);
        if (init_err != rpc::error::OK())
        {
            unload_library();
            CO_RETURN rpc::connect_result{init_err, {}};
        }

        child_ctx_ = init_params.child_ctx;
        auto output_descriptor = decode_remote_object(init_params.output_obj);
        free_zone_address(allocator_, &init_params.output_obj.address);

        if (!output_descriptor)
        {
            if (child_ctx_ && loader_.exports().destroy)
            {
                loader_.exports().destroy(child_ctx_);
                child_ctx_ = nullptr;
            }
            unload_library();
            CO_RETURN rpc::connect_result{rpc::error::INVALID_DATA(), {}};
        }

        set_status(rpc::transport_status::CONNECTED);
        CO_RETURN rpc::connect_result{rpc::error::OK(), *output_descriptor};
    }

    CORO_TASK(send_result)
    child_transport::outbound_send(send_params params)
    {
        if (!child_ctx_ || !loader_.exports().send)
            CO_RETURN rpc::send_result{rpc::error::ZONE_NOT_FOUND(), {}, {}};

        borrowed_send_params borrowed(params);
        canopy_send_result raw_result{};
        loader_.exports().send(child_ctx_, &borrowed.raw, &raw_result);
        auto result = copy_send_result(raw_result);
        free_send_result(allocator_, &raw_result);
        CO_RETURN result;
    }

    CORO_TASK(void)
    child_transport::outbound_post(post_params params)
    {
        if (!child_ctx_ || !loader_.exports().post)
            CO_RETURN;

        borrowed_post_params borrowed(params);
        loader_.exports().post(child_ctx_, &borrowed.raw);
    }

    CORO_TASK(standard_result)
    child_transport::outbound_try_cast(try_cast_params params)
    {
        if (!child_ctx_ || !loader_.exports().try_cast)
            CO_RETURN rpc::standard_result{rpc::error::ZONE_NOT_FOUND(), {}};

        borrowed_try_cast_params borrowed(params);
        canopy_standard_result raw_result{};
        loader_.exports().try_cast(child_ctx_, &borrowed.raw, &raw_result);
        auto result = copy_standard_result(raw_result);
        free_standard_result(allocator_, &raw_result);
        CO_RETURN result;
    }

    CORO_TASK(standard_result)
    child_transport::outbound_add_ref(add_ref_params params)
    {
        if (!child_ctx_ || !loader_.exports().add_ref)
            CO_RETURN rpc::standard_result{rpc::error::ZONE_NOT_FOUND(), {}};

        borrowed_add_ref_params borrowed(params);
        canopy_standard_result raw_result{};
        loader_.exports().add_ref(child_ctx_, &borrowed.raw, &raw_result);
        auto result = copy_standard_result(raw_result);
        free_standard_result(allocator_, &raw_result);
        CO_RETURN result;
    }

    CORO_TASK(standard_result)
    child_transport::outbound_release(release_params params)
    {
        if (!child_ctx_ || !loader_.exports().release)
            CO_RETURN rpc::standard_result{rpc::error::ZONE_NOT_FOUND(), {}};

        borrowed_release_params borrowed(params);
        canopy_standard_result raw_result{};
        loader_.exports().release(child_ctx_, &borrowed.raw, &raw_result);
        auto result = copy_standard_result(raw_result);
        free_standard_result(allocator_, &raw_result);
        CO_RETURN result;
    }

    CORO_TASK(void)
    child_transport::outbound_object_released(object_released_params params)
    {
        if (!child_ctx_ || !loader_.exports().object_released)
            CO_RETURN;

        borrowed_object_released_params borrowed(params);
        loader_.exports().object_released(child_ctx_, &borrowed.raw);
    }

    CORO_TASK(void)
    child_transport::outbound_transport_down(transport_down_params params)
    {
        if (!child_ctx_ || !loader_.exports().transport_down)
            CO_RETURN;

        borrowed_transport_down_params borrowed(params);
        loader_.exports().transport_down(child_ctx_, &borrowed.raw);
    }

    int child_transport::cb_send(
        canopy_parent_context parent_ctx,
        const canopy_send_params* params,
        canopy_send_result* result)
    {
        auto* transport = static_cast<child_transport*>(parent_ctx);
        if (!transport || !params || !result)
            return rpc::error::INVALID_DATA();

        auto native_params = decode_send_params(*params);
        if (!native_params)
        {
            *result = {};
            result->error_code = rpc::error::INVALID_DATA();
            return result->error_code;
        }

        auto native_result = transport->inbound_send(std::move(*native_params));
        return write_send_result(transport->allocator_, native_result, result);
    }

    void child_transport::cb_post(
        canopy_parent_context parent_ctx,
        const canopy_post_params* params)
    {
        auto* transport = static_cast<child_transport*>(parent_ctx);
        if (!transport || !params)
            return;

        auto native_params = decode_post_params(*params);
        if (!native_params)
            return;

        transport->inbound_post(std::move(*native_params));
    }

    int child_transport::cb_try_cast(
        canopy_parent_context parent_ctx,
        const canopy_try_cast_params* params,
        canopy_standard_result* result)
    {
        auto* transport = static_cast<child_transport*>(parent_ctx);
        if (!transport || !params || !result)
            return rpc::error::INVALID_DATA();

        auto native_params = decode_try_cast_params(*params);
        if (!native_params)
        {
            *result = {};
            result->error_code = rpc::error::INVALID_DATA();
            return result->error_code;
        }

        auto native_result = transport->inbound_try_cast(std::move(*native_params));
        return write_standard_result(transport->allocator_, native_result, result);
    }

    int child_transport::cb_add_ref(
        canopy_parent_context parent_ctx,
        const canopy_add_ref_params* params,
        canopy_standard_result* result)
    {
        auto* transport = static_cast<child_transport*>(parent_ctx);
        if (!transport || !params || !result)
            return rpc::error::INVALID_DATA();

        auto native_params = decode_add_ref_params(*params);
        if (!native_params)
        {
            *result = {};
            result->error_code = rpc::error::INVALID_DATA();
            return result->error_code;
        }

        auto native_result = transport->inbound_add_ref(std::move(*native_params));
        return write_standard_result(transport->allocator_, native_result, result);
    }

    int child_transport::cb_release(
        canopy_parent_context parent_ctx,
        const canopy_release_params* params,
        canopy_standard_result* result)
    {
        auto* transport = static_cast<child_transport*>(parent_ctx);
        if (!transport || !params || !result)
            return rpc::error::INVALID_DATA();

        auto native_params = decode_release_params(*params);
        if (!native_params)
        {
            *result = {};
            result->error_code = rpc::error::INVALID_DATA();
            return result->error_code;
        }

        auto native_result = transport->inbound_release(std::move(*native_params));
        return write_standard_result(transport->allocator_, native_result, result);
    }

    void child_transport::cb_object_released(
        canopy_parent_context parent_ctx,
        const canopy_object_released_params* params)
    {
        auto* transport = static_cast<child_transport*>(parent_ctx);
        if (!transport || !params)
            return;

        auto native_params = decode_object_released_params(*params);
        if (!native_params)
            return;

        transport->inbound_object_released(std::move(*native_params));
    }

    void child_transport::cb_transport_down(
        canopy_parent_context parent_ctx,
        const canopy_transport_down_params* params)
    {
        auto* transport = static_cast<child_transport*>(parent_ctx);
        if (!transport || !params)
            return;

        auto native_params = decode_transport_down_params(*params);
        if (!native_params)
            return;

        transport->inbound_transport_down(std::move(*native_params));
    }

    int child_transport::cb_get_new_zone_id(
        canopy_parent_context parent_ctx,
        const canopy_get_new_zone_id_params* params,
        canopy_new_zone_id_result* result)
    {
        auto* transport = static_cast<child_transport*>(parent_ctx);
        if (!transport || !params || !result)
            return rpc::error::INVALID_DATA();

        auto native_params = decode_get_new_zone_id_params(*params);
        if (!native_params)
        {
            *result = {};
            result->error_code = rpc::error::INVALID_DATA();
            return result->error_code;
        }

        auto svc = transport->get_service();
        if (!svc)
        {
            *result = {};
            result->error_code = rpc::error::ZONE_NOT_FOUND();
            return result->error_code;
        }

        auto native_result = svc->get_new_zone_id(std::move(*native_params));
        return write_new_zone_id_result(transport->allocator_, native_result, result);
    }
} // namespace rpc::c_abi

#endif
