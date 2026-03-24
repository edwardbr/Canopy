/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <transports/mock_test/transport.h>
#include <iostream>

namespace rpc::mock_test
{
    mock_transport::mock_transport(
        std::string name,
        std::shared_ptr<rpc::service> service)
        : rpc::transport(
              name,
              service)
    {
        set_status(rpc::transport_status::CONNECTED);
    }

    void mock_transport::record_call(
        call_record::call_type type,
        uint64_t protocol_version,
        rpc::remote_object destination_zone_id,
        rpc::caller_zone caller_zone_id)
    {
        std::scoped_lock lock(call_history_mtx_);
        call_history_.push_back(
            call_record{.type = type,
                .protocol_version = protocol_version,
                .destination_zone_id = destination_zone_id,
                .caller_zone_id = caller_zone_id,
                .object_id = destination_zone_id.get_object_id(),
                .timestamp = std::chrono::steady_clock::now()});
    }

    void mock_transport::set_force_failure(
        bool force_failure,
        int error_code)
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

    CORO_TASK(rpc::connect_result)
    mock_transport::inner_connect(
        std::shared_ptr<rpc::object_stub> stub,
        connection_settings input_descr)
    {
        std::ignore = stub;
        std::ignore = input_descr;
        CO_RETURN rpc::connect_result{rpc::error::OK(), {}};
    }

    CORO_TASK(send_result)
    mock_transport::outbound_send(send_params params)
    {
        send_count_.fetch_add(1, std::memory_order_acq_rel);
        record_call(call_record::call_type::SEND, params.protocol_version, params.remote_object_id, params.caller_zone_id);

        if (force_failure_.load(std::memory_order_acquire))
        {
            CO_RETURN send_result{forced_error_code_.load(std::memory_order_acquire), {}, {}};
        }

        // Check if custom handler is set
        send_handler handler;
        {
            std::scoped_lock lock(send_handler_mtx_);
            if (send_handler_)
            {
                handler = send_handler_;
            }
            else
            {
                CO_RETURN send_result{rpc::error::OK(), {}, {}};
            }
        }
        CO_RETURN CO_AWAIT handler(std::move(params));

        // Default successful response
        CO_RETURN send_result{rpc::error::OK(), {}, {}};
    }

    CORO_TASK(void)
    mock_transport::outbound_post(post_params params)
    {
        post_count_.fetch_add(1, std::memory_order_acq_rel);
        record_call(call_record::call_type::POST, params.protocol_version, params.remote_object_id, params.caller_zone_id);
        CO_RETURN;
    }

    CORO_TASK(standard_result)
    mock_transport::outbound_try_cast(try_cast_params params)
    {
        try_cast_count_.fetch_add(1, std::memory_order_acq_rel);
        record_call(
            call_record::call_type::TRY_CAST, params.protocol_version, params.remote_object_id, params.caller_zone_id);

        if (force_failure_.load(std::memory_order_acquire))
        {
            CO_RETURN standard_result{forced_error_code_.load(std::memory_order_acquire), {}};
        }

        CO_RETURN standard_result{rpc::error::OK(), {}};
    }

    CORO_TASK(standard_result)
    mock_transport::outbound_add_ref(add_ref_params params)
    {
        add_ref_count_.fetch_add(1, std::memory_order_acq_rel);
        record_call(
            call_record::call_type::ADD_REF, params.protocol_version, params.remote_object_id, params.caller_zone_id);

        if (force_failure_.load(std::memory_order_acquire))
        {
            CO_RETURN standard_result{forced_error_code_.load(std::memory_order_acquire), {}};
        }

        CO_RETURN standard_result{rpc::error::OK(), {}};
    }

    CORO_TASK(standard_result)
    mock_transport::outbound_release(release_params params)
    {
        release_count_.fetch_add(1, std::memory_order_acq_rel);
        record_call(
            call_record::call_type::RELEASE, params.protocol_version, params.remote_object_id, params.caller_zone_id);

        if (force_failure_.load(std::memory_order_acquire))
        {
            CO_RETURN standard_result{forced_error_code_.load(std::memory_order_acquire), {}};
        }

        CO_RETURN standard_result{rpc::error::OK(), {}};
    }

    CORO_TASK(void)
    mock_transport::outbound_object_released(object_released_params params)
    {
        object_released_count_.fetch_add(1, std::memory_order_acq_rel);
        record_call(
            call_record::call_type::OBJECT_RELEASED, params.protocol_version, params.remote_object_id, params.caller_zone_id);
        CO_RETURN;
    }

    CORO_TASK(void)
    mock_transport::outbound_transport_down(transport_down_params params)
    {
        transport_down_count_.fetch_add(1, std::memory_order_acq_rel);
        record_call(
            call_record::call_type::TRANSPORT_DOWN,
            params.protocol_version,
            rpc::remote_object(params.destination_zone_id),
            params.caller_zone_id);

        mark_as_down();
        CO_RETURN;
    }
}
