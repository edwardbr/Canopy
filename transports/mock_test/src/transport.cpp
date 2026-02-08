/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <transports/mock_test/transport.h>
#include <iostream>

namespace rpc::mock_test
{
    mock_transport::mock_transport(std::string name, std::shared_ptr<rpc::service> service, rpc::zone adjacent_zone_id)
        : rpc::transport(name, service, adjacent_zone_id)
    {
        set_status(rpc::transport_status::CONNECTED);
    }

    void mock_transport::record_call(call_record::call_type type,
        uint64_t protocol_version,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id)
    {
        std::scoped_lock lock(call_history_mtx_);
        call_history_.push_back(call_record{
            type, protocol_version, destination_zone_id, caller_zone_id, object_id, std::chrono::steady_clock::now()});
    }

    void mock_transport::set_force_failure(bool force_failure, int error_code)
    {
        force_failure_.store(force_failure, std::memory_order_release);
        forced_error_code_.store(error_code, std::memory_order_release);
        if (force_failure)
        {
            set_status(rpc::transport_status::DISCONNECTED);
        }
        else
        {
            set_status(rpc::transport_status::CONNECTED);
        }
    }

    void mock_transport::clear_call_history()
    {
        std::scoped_lock lock(call_history_mtx_);
        call_history_.clear();
    }

    std::vector<mock_transport::call_record> mock_transport::get_call_history() const
    {
        std::scoped_lock lock(call_history_mtx_);
        return call_history_;
    }

    void mock_transport::mark_as_down()
    {
        set_status(rpc::transport_status::DISCONNECTED);
    }

    void mock_transport::mark_as_up()
    {
        set_status(rpc::transport_status::CONNECTED);
    }

    CORO_TASK(int)
    mock_transport::inner_connect(rpc::interface_descriptor input_descr, rpc::interface_descriptor& output_descr)
    {
        std::ignore = input_descr;
        std::ignore = output_descr;
        CO_RETURN rpc::error::OK();
    }

    CORO_TASK(int)
    mock_transport::outbound_send(uint64_t protocol_version,
        rpc::encoding encoding,
        uint64_t tag,
        rpc::caller_zone caller_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id,
        rpc::method method_id,
        const rpc::span& in_data,
        std::vector<char>& out_buf_,
        const std::vector<rpc::back_channel_entry>& in_back_channel,
        std::vector<rpc::back_channel_entry>& out_back_channel)
    {
        send_count_.fetch_add(1, std::memory_order_acq_rel);
        record_call(call_record::call_type::SEND, protocol_version, destination_zone_id, caller_zone_id, object_id);

        if (force_failure_.load(std::memory_order_acquire))
        {
            CO_RETURN forced_error_code_.load(std::memory_order_acquire);
        }

        // Check if custom handler is set
        {
            std::scoped_lock lock(send_handler_mtx_);
            if (send_handler_)
            {
                CO_RETURN CO_AWAIT send_handler_(protocol_version,
                    encoding,
                    tag,
                    caller_zone_id,
                    destination_zone_id,
                    object_id,
                    interface_id,
                    method_id,
                    in_data,
                    out_buf_,
                    in_back_channel,
                    out_back_channel);
            }
        }

        // Default successful response
        CO_RETURN rpc::error::OK();
    }

    CORO_TASK(void)
    mock_transport::outbound_post(uint64_t protocol_version,
        rpc::encoding encoding,
        uint64_t tag,
        rpc::caller_zone caller_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id,
        rpc::method method_id,
        const rpc::span& in_data,
        const std::vector<rpc::back_channel_entry>& in_back_channel)
    {
        std::ignore = encoding;
        std::ignore = tag;
        std::ignore = interface_id;
        std::ignore = method_id;
        std::ignore = in_data;
        std::ignore = in_back_channel;

        post_count_.fetch_add(1, std::memory_order_acq_rel);
        record_call(call_record::call_type::POST, protocol_version, destination_zone_id, caller_zone_id, object_id);
        CO_RETURN;
    }

    CORO_TASK(int)
    mock_transport::outbound_try_cast(uint64_t protocol_version,
        rpc::caller_zone caller_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id,
        const std::vector<rpc::back_channel_entry>& in_back_channel,
        std::vector<rpc::back_channel_entry>& out_back_channel)
    {
        std::ignore = interface_id;
        std::ignore = in_back_channel;
        std::ignore = out_back_channel;

        try_cast_count_.fetch_add(1, std::memory_order_acq_rel);
        record_call(call_record::call_type::TRY_CAST, protocol_version, destination_zone_id, caller_zone_id, object_id);

        if (force_failure_.load(std::memory_order_acquire))
        {
            CO_RETURN forced_error_code_.load(std::memory_order_acquire);
        }

        CO_RETURN rpc::error::OK();
    }

    CORO_TASK(int)
    mock_transport::outbound_add_ref(uint64_t protocol_version,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id,
        rpc::caller_zone caller_zone_id,
        rpc::known_direction_zone known_direction_zone_id,
        rpc::add_ref_options build_out_param_channel,
        const std::vector<rpc::back_channel_entry>& in_back_channel,
        std::vector<rpc::back_channel_entry>& out_back_channel)
    {
        std::ignore = known_direction_zone_id;
        std::ignore = build_out_param_channel;
        std::ignore = in_back_channel;
        std::ignore = out_back_channel;

        add_ref_count_.fetch_add(1, std::memory_order_acq_rel);
        record_call(call_record::call_type::ADD_REF, protocol_version, destination_zone_id, caller_zone_id, object_id);

        if (force_failure_.load(std::memory_order_acquire))
        {
            CO_RETURN forced_error_code_.load(std::memory_order_acquire);
        }

        CO_RETURN rpc::error::OK();
    }

    CORO_TASK(int)
    mock_transport::outbound_release(uint64_t protocol_version,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id,
        rpc::caller_zone caller_zone_id,
        rpc::release_options options,
        const std::vector<rpc::back_channel_entry>& in_back_channel,
        std::vector<rpc::back_channel_entry>& out_back_channel)
    {
        std::ignore = options;
        std::ignore = in_back_channel;
        std::ignore = out_back_channel;

        release_count_.fetch_add(1, std::memory_order_acq_rel);
        record_call(call_record::call_type::RELEASE, protocol_version, destination_zone_id, caller_zone_id, object_id);

        if (force_failure_.load(std::memory_order_acquire))
        {
            CO_RETURN forced_error_code_.load(std::memory_order_acquire);
        }

        CO_RETURN rpc::error::OK();
    }

    CORO_TASK(void)
    mock_transport::outbound_object_released(uint64_t protocol_version,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id,
        rpc::caller_zone caller_zone_id,
        const std::vector<rpc::back_channel_entry>& in_back_channel)
    {
        std::ignore = in_back_channel;

        object_released_count_.fetch_add(1, std::memory_order_acq_rel);
        record_call(
            call_record::call_type::OBJECT_RELEASED, protocol_version, destination_zone_id, caller_zone_id, object_id);
        CO_RETURN;
    }

    CORO_TASK(void)
    mock_transport::outbound_transport_down(uint64_t protocol_version,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        const std::vector<rpc::back_channel_entry>& in_back_channel)
    {
        std::ignore = in_back_channel;

        transport_down_count_.fetch_add(1, std::memory_order_acq_rel);
        record_call(
            call_record::call_type::TRANSPORT_DOWN, protocol_version, destination_zone_id, caller_zone_id, rpc::object{0});

        mark_as_down();
        CO_RETURN;
    }
}
