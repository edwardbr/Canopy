/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <transports/c_abi/dll_transport.h>

#ifndef CANOPY_BUILD_COROUTINE

#  include <cstring>

namespace rpc::c_abi
{
    namespace
    {
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
            auto remote_object = decode_remote_object(params.remote_object_id);
            auto back_channel = decode_back_channel(params.in_back_channel);
            if (!caller_zone || !remote_object || !back_channel)
                return rpc::unexpected<std::string>("invalid send params");

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
            auto remote_object = decode_remote_object(params.remote_object_id);
            auto back_channel = decode_back_channel(params.in_back_channel);
            if (!caller_zone || !remote_object || !back_channel)
                return rpc::unexpected<std::string>("invalid post params");

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
            auto remote_object = decode_remote_object(params.remote_object_id);
            auto back_channel = decode_back_channel(params.in_back_channel);
            if (!caller_zone || !remote_object || !back_channel)
                return rpc::unexpected<std::string>("invalid try_cast params");

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
            auto caller_zone = decode_zone(params.caller_zone_id);
            auto requesting_zone = decode_zone(params.requesting_zone_id);
            auto back_channel = decode_back_channel(params.in_back_channel);
            if (!remote_object || !caller_zone || !requesting_zone || !back_channel)
                return rpc::unexpected<std::string>("invalid add_ref params");

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
            auto caller_zone = decode_zone(params.caller_zone_id);
            auto back_channel = decode_back_channel(params.in_back_channel);
            if (!remote_object || !caller_zone || !back_channel)
                return rpc::unexpected<std::string>("invalid release params");

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
            auto caller_zone = decode_zone(params.caller_zone_id);
            auto back_channel = decode_back_channel(params.in_back_channel);
            if (!remote_object || !caller_zone || !back_channel)
                return rpc::unexpected<std::string>("invalid object_released params");

            return rpc::object_released_params{
                params.protocol_version, *remote_object, *caller_zone, std::move(*back_channel)};
        }

        rpc::expected<
            rpc::transport_down_params,
            std::string>
        decode_transport_down_params(const canopy_transport_down_params& params)
        {
            auto destination_zone = decode_zone(params.destination_zone_id);
            auto caller_zone = decode_zone(params.caller_zone_id);
            auto back_channel = decode_back_channel(params.in_back_channel);
            if (!destination_zone || !caller_zone || !back_channel)
                return rpc::unexpected<std::string>("invalid transport_down params");

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
                return rpc::unexpected<std::string>("invalid get_new_zone_id params");

            return rpc::get_new_zone_id_params{params.protocol_version, std::move(*back_channel)};
        }

        void free_zone_address(
            const canopy_allocator_vtable& allocator,
            canopy_zone_address* address)
        {
            if (!address->blob.data || !address->blob.size || !allocator.free)
                return;

            allocator.free(allocator.allocator_ctx, const_cast<uint8_t*>(address->blob.data), address->blob.size);
            address->blob = {};
        }

        void free_back_channel(
            const canopy_allocator_vtable& allocator,
            canopy_mut_back_channel_span* back_channel)
        {
            if (!allocator.free)
            {
                back_channel->data = nullptr;
                back_channel->size = 0;
                return;
            }

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
            if (result->out_buf.data && result->out_buf.size && allocator.free)
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
            if (!allocator.alloc)
                return rpc::error::OUT_OF_MEMORY();

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

        rpc::new_zone_id_result copy_new_zone_id_result(const canopy_new_zone_id_result& source)
        {
            auto zone = decode_zone(source.zone_id);
            auto out_back_channel = decode_back_channel(
                canopy_back_channel_span{source.out_back_channel.data, source.out_back_channel.size});
            if (!zone || !out_back_channel)
                return rpc::new_zone_id_result{rpc::error::INVALID_DATA(), {}, {}};

            return rpc::new_zone_id_result{source.error_code, *zone, std::move(*out_back_channel)};
        }
    } // namespace

    namespace detail
    {
        rpc::expected<
            rpc::connection_settings,
            std::string>
        decode_connection_settings(const canopy_connection_settings& input_descr)
        {
            auto remote_object = decode_remote_object(input_descr.remote_object_id);
            if (!remote_object)
                return rpc::unexpected<std::string>(std::move(remote_object.error()));

            return rpc::connection_settings{rpc::interface_ordinal(input_descr.inbound_interface_id),
                rpc::interface_ordinal(input_descr.outbound_interface_id),
                *remote_object};
        }

        int write_remote_object(
            const canopy_allocator_vtable& allocator,
            const rpc::remote_object& source,
            canopy_remote_object* output)
        {
            *output = {};
            return write_zone_address(allocator, source.get_address(), &output->address);
        }
    } // namespace detail

    parent_transport::parent_transport(
        std::string name,
        rpc::zone child_zone,
        rpc::zone parent_zone,
        canopy_parent_context parent_ctx,
        canopy_allocator_vtable allocator,
        canopy_parent_send_fn send,
        canopy_parent_post_fn post,
        canopy_parent_try_cast_fn try_cast,
        canopy_parent_add_ref_fn add_ref,
        canopy_parent_release_fn release,
        canopy_parent_object_released_fn object_released,
        canopy_parent_transport_down_fn transport_down,
        canopy_parent_get_new_zone_id_fn get_new_zone_id)
        : rpc::transport(
              name,
              child_zone)
        , parent_ctx_(parent_ctx)
        , allocator_(allocator)
        , parent_send_(send)
        , parent_post_(post)
        , parent_try_cast_(try_cast)
        , parent_add_ref_(add_ref)
        , parent_release_(release)
        , parent_object_released_(object_released)
        , parent_transport_down_(transport_down)
        , parent_get_new_zone_id_(get_new_zone_id)
    {
        set_adjacent_zone_id(parent_zone);
        set_status(rpc::transport_status::CONNECTED);
    }

    void parent_transport::set_status(rpc::transport_status status)
    {
        rpc::transport::set_status(status);

        if (status == rpc::transport_status::DISCONNECTED)
        {
            notify_all_destinations_of_disconnect();
            parent_ctx_ = nullptr;
        }
    }

    CORO_TASK(send_result)
    parent_transport::outbound_send(send_params params)
    {
        if (!parent_send_ || !parent_ctx_)
            CO_RETURN rpc::send_result{rpc::error::ZONE_NOT_FOUND(), {}, {}};

        borrowed_send_params borrowed(params);
        canopy_send_result raw_result{};
        parent_send_(parent_ctx_, &borrowed.raw, &raw_result);
        auto result = copy_send_result(raw_result);
        free_send_result(allocator_, &raw_result);
        CO_RETURN result;
    }

    CORO_TASK(void)
    parent_transport::outbound_post(post_params params)
    {
        if (!parent_post_ || !parent_ctx_)
            CO_RETURN;

        borrowed_post_params borrowed(params);
        parent_post_(parent_ctx_, &borrowed.raw);
    }

    CORO_TASK(standard_result)
    parent_transport::outbound_try_cast(try_cast_params params)
    {
        if (!parent_try_cast_ || !parent_ctx_)
            CO_RETURN rpc::standard_result{rpc::error::ZONE_NOT_FOUND(), {}};

        borrowed_try_cast_params borrowed(params);
        canopy_standard_result raw_result{};
        parent_try_cast_(parent_ctx_, &borrowed.raw, &raw_result);
        auto result = copy_standard_result(raw_result);
        free_standard_result(allocator_, &raw_result);
        CO_RETURN result;
    }

    CORO_TASK(standard_result)
    parent_transport::outbound_add_ref(add_ref_params params)
    {
        if (!parent_add_ref_ || !parent_ctx_)
            CO_RETURN rpc::standard_result{rpc::error::ZONE_NOT_FOUND(), {}};

        borrowed_add_ref_params borrowed(params);
        canopy_standard_result raw_result{};
        parent_add_ref_(parent_ctx_, &borrowed.raw, &raw_result);
        auto result = copy_standard_result(raw_result);
        free_standard_result(allocator_, &raw_result);
        CO_RETURN result;
    }

    CORO_TASK(standard_result)
    parent_transport::outbound_release(release_params params)
    {
        if (!parent_release_ || !parent_ctx_)
            CO_RETURN rpc::standard_result{rpc::error::ZONE_NOT_FOUND(), {}};

        borrowed_release_params borrowed(params);
        canopy_standard_result raw_result{};
        parent_release_(parent_ctx_, &borrowed.raw, &raw_result);
        auto result = copy_standard_result(raw_result);
        free_standard_result(allocator_, &raw_result);
        CO_RETURN result;
    }

    CORO_TASK(void)
    parent_transport::outbound_object_released(object_released_params params)
    {
        if (!parent_object_released_ || !parent_ctx_)
            CO_RETURN;

        borrowed_object_released_params borrowed(params);
        parent_object_released_(parent_ctx_, &borrowed.raw);
    }

    CORO_TASK(void)
    parent_transport::outbound_transport_down(transport_down_params params)
    {
        if (!parent_transport_down_ || !parent_ctx_)
            CO_RETURN;

        borrowed_transport_down_params borrowed(params);
        parent_transport_down_(parent_ctx_, &borrowed.raw);
    }

    CORO_TASK(new_zone_id_result)
    parent_transport::outbound_get_new_zone_id(get_new_zone_id_params params)
    {
        if (!parent_get_new_zone_id_ || !parent_ctx_)
            CO_RETURN rpc::new_zone_id_result{rpc::error::ZONE_NOT_FOUND(), {}, {}};

        borrowed_get_new_zone_id_params borrowed(params);
        canopy_new_zone_id_result raw_result{};
        parent_get_new_zone_id_(parent_ctx_, &borrowed.raw, &raw_result);
        auto result = copy_new_zone_id_result(raw_result);
        free_new_zone_id_result(allocator_, &raw_result);
        CO_RETURN result;
    }
} // namespace rpc::c_abi

extern "C"
{
    CANOPY_C_ABI_EXPORT void canopy_dll_destroy(canopy_child_context child_ctx)
    {
        if (!child_ctx)
            return;

        auto* ctx = static_cast<rpc::c_abi::dll_context*>(child_ctx);
        if (ctx->destroyed.exchange(true))
            return;

        ctx->service.reset();
        if (ctx->transport)
            ctx->transport->set_status(rpc::transport_status::DISCONNECTED);
        ctx->transport.reset();
        delete ctx;
    }

    CANOPY_C_ABI_EXPORT int32_t canopy_dll_send(
        canopy_child_context child_ctx,
        const canopy_send_params* params,
        canopy_send_result* result)
    {
        auto* ctx = static_cast<rpc::c_abi::dll_context*>(child_ctx);
        if (!ctx || !ctx->transport || !params || !result)
        {
            if (result)
                *result = {};
            return rpc::error::INVALID_DATA();
        }

        auto native_params = rpc::c_abi::decode_send_params(*params);
        if (!native_params)
        {
            *result = {};
            result->error_code = rpc::error::INVALID_DATA();
            return result->error_code;
        }

        auto native_result = ctx->transport->inbound_send(std::move(*native_params));
        return rpc::c_abi::write_send_result(ctx->allocator_, native_result, result);
    }

    CANOPY_C_ABI_EXPORT void canopy_dll_post(
        canopy_child_context child_ctx,
        const canopy_post_params* params)
    {
        auto* ctx = static_cast<rpc::c_abi::dll_context*>(child_ctx);
        if (!ctx || !ctx->transport || !params)
            return;

        auto native_params = rpc::c_abi::decode_post_params(*params);
        if (!native_params)
            return;

        ctx->transport->inbound_post(std::move(*native_params));
    }

    CANOPY_C_ABI_EXPORT int32_t canopy_dll_try_cast(
        canopy_child_context child_ctx,
        const canopy_try_cast_params* params,
        canopy_standard_result* result)
    {
        auto* ctx = static_cast<rpc::c_abi::dll_context*>(child_ctx);
        if (!ctx || !ctx->transport || !params || !result)
        {
            if (result)
                *result = {};
            return rpc::error::INVALID_DATA();
        }

        auto native_params = rpc::c_abi::decode_try_cast_params(*params);
        if (!native_params)
        {
            *result = {};
            result->error_code = rpc::error::INVALID_DATA();
            return result->error_code;
        }

        auto native_result = ctx->transport->inbound_try_cast(std::move(*native_params));
        return rpc::c_abi::write_standard_result(ctx->allocator_, native_result, result);
    }

    CANOPY_C_ABI_EXPORT int32_t canopy_dll_add_ref(
        canopy_child_context child_ctx,
        const canopy_add_ref_params* params,
        canopy_standard_result* result)
    {
        auto* ctx = static_cast<rpc::c_abi::dll_context*>(child_ctx);
        if (!ctx || !ctx->transport || !params || !result)
        {
            if (result)
                *result = {};
            return rpc::error::INVALID_DATA();
        }

        auto native_params = rpc::c_abi::decode_add_ref_params(*params);
        if (!native_params)
        {
            *result = {};
            result->error_code = rpc::error::INVALID_DATA();
            return result->error_code;
        }

        auto native_result = ctx->transport->inbound_add_ref(std::move(*native_params));
        return rpc::c_abi::write_standard_result(ctx->allocator_, native_result, result);
    }

    CANOPY_C_ABI_EXPORT int32_t canopy_dll_release(
        canopy_child_context child_ctx,
        const canopy_release_params* params,
        canopy_standard_result* result)
    {
        auto* ctx = static_cast<rpc::c_abi::dll_context*>(child_ctx);
        if (!ctx || !ctx->transport || !params || !result)
        {
            if (result)
                *result = {};
            return rpc::error::INVALID_DATA();
        }

        auto native_params = rpc::c_abi::decode_release_params(*params);
        if (!native_params)
        {
            *result = {};
            result->error_code = rpc::error::INVALID_DATA();
            return result->error_code;
        }

        auto native_result = ctx->transport->inbound_release(std::move(*native_params));
        return rpc::c_abi::write_standard_result(ctx->allocator_, native_result, result);
    }

    CANOPY_C_ABI_EXPORT void canopy_dll_object_released(
        canopy_child_context child_ctx,
        const canopy_object_released_params* params)
    {
        auto* ctx = static_cast<rpc::c_abi::dll_context*>(child_ctx);
        if (!ctx || !ctx->transport || !params)
            return;

        auto native_params = rpc::c_abi::decode_object_released_params(*params);
        if (!native_params)
            return;

        ctx->transport->inbound_object_released(std::move(*native_params));
    }

    CANOPY_C_ABI_EXPORT void canopy_dll_transport_down(
        canopy_child_context child_ctx,
        const canopy_transport_down_params* params)
    {
        auto* ctx = static_cast<rpc::c_abi::dll_context*>(child_ctx);
        if (!ctx || !ctx->transport || !params)
            return;

        auto native_params = rpc::c_abi::decode_transport_down_params(*params);
        if (!native_params)
            return;

        ctx->transport->inbound_transport_down(std::move(*native_params));
    }

    CANOPY_C_ABI_EXPORT int32_t canopy_dll_get_new_zone_id(
        canopy_child_context child_ctx,
        const canopy_get_new_zone_id_params* params,
        canopy_new_zone_id_result* result)
    {
        auto* ctx = static_cast<rpc::c_abi::dll_context*>(child_ctx);
        if (!ctx || !ctx->transport || !params || !result)
        {
            if (result)
                *result = {};
            return rpc::error::INVALID_DATA();
        }

        auto native_params = rpc::c_abi::decode_get_new_zone_id_params(*params);
        if (!native_params)
        {
            *result = {};
            result->error_code = rpc::error::INVALID_DATA();
            return result->error_code;
        }

        auto native_result = ctx->transport->outbound_get_new_zone_id(std::move(*native_params));
        return rpc::c_abi::write_new_zone_id_result(ctx->allocator_, native_result, result);
    }
}

#endif
