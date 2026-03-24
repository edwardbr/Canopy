/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <atomic>
#include <cerrno>
#include <transports/streaming/transport.h>

namespace rpc::stream_transport
{
    namespace
    {
        auto is_expected_disconnect_send_failure(
            rpc::transport_status transport_status, const coro::net::io_status& send_status) -> bool
        {
            if (transport_status < rpc::transport_status::DISCONNECTING)
                return false;

            if (send_status.is_closed())
                return true;

            return send_status.type == coro::net::io_status::kind::native
                   && (send_status.native_code == EPIPE || send_status.native_code == ECONNRESET);
        }
    } // namespace

    transport::transport(std::string name,
        std::shared_ptr<rpc::service> service,
        std::shared_ptr<streaming::stream> stream,
        connection_handler handler)
        : rpc::transport(name, service)
        , stream_(std::move(stream))
        , connection_handler_(std::move(handler))
    {
    }

    void transport::initialise_after_construction()
    {
        keep_alive_ = std::static_pointer_cast<rpc::stream_transport::transport>(shared_from_this());
        set_status(rpc::transport_status::CONNECTED);

        if (connection_handler_)
            get_service()->spawn(pump_send_and_receive());
    }

    void transport::set_status(rpc::transport_status new_status)
    {
        auto old_status = get_status();
        if (old_status != new_status && new_status == rpc::transport_status::DISCONNECTING)
            disconnecting_since_ = std::chrono::steady_clock::now();
        if (new_status != rpc::transport_status::CONNECTED)
        {
            send_queue_ready_.set();
        }
        rpc::transport::set_status(new_status);
    }

    std::shared_ptr<transport> make_server(std::string name,
        std::shared_ptr<rpc::service> service,
        std::shared_ptr<streaming::stream> stream,
        transport::connection_handler handler)
    {
        auto transport = std::shared_ptr<rpc::stream_transport::transport>(
            new rpc::stream_transport::transport(name, service, std::move(stream), std::move(handler)));

        transport->initialise_after_construction();

        return transport;
    }

    CORO_TASK(rpc::connect_result)
    transport::inner_connect(std::shared_ptr<rpc::object_stub> stub, connection_settings input_descr)
    {
        RPC_DEBUG("stream_transport::transport::inner_connect zone={}", get_zone_id().get_subnet());
        stub_ = stub;

        auto service = get_service();
        RPC_DEBUG("inner_connect: spawning pump for zone {}", get_zone_id().get_subnet());
        service->spawn(pump_send_and_receive());
        RPC_DEBUG("inner_connect: pump spawned, calling call_peer for zone {}", get_zone_id().get_subnet());

        if (!connection_handler_)
        {
            // Client side: send init message to server
            auto inbound_remote_object_r = get_zone_id().with_object(input_descr.get_object_id());
            if (!inbound_remote_object_r)
            {
                CO_RETURN rpc::connect_result{rpc::error::INVALID_DATA(), {}};
            }
            auto init_result
                = CO_AWAIT call_peer<init_client_channel_send, init_client_channel_response>(rpc::get_version(),
                    init_client_channel_send{.inbound_remote_object = std::move(*inbound_remote_object_r),
                        .inbound_interface_id = input_descr.inbound_interface_id,
                        .destination_zone_id = get_adjacent_zone_id(),
                        .outbound_interface_id = input_descr.outbound_interface_id,
                        .adjacent_zone_id = get_zone_id()});
            int ret = init_result.error_code;
            if (ret != rpc::error::OK())
            {
                stub_.reset();
                RPC_ERROR("stream_transport::transport::inner_connect call_peer failed {}", rpc::error::to_string(ret));
                CO_RETURN rpc::connect_result{ret, {}};
            }

            auto& init_receive = init_result.payload;

            if (init_receive.err_code != rpc::error::OK())
            {
                stub_.reset();
                RPC_ERROR("init_client_channel_send failed");
                CO_RETURN rpc::connect_result{init_receive.err_code, {}};
            }

            CO_RETURN rpc::connect_result{rpc::error::OK(), init_receive.outbound_remote_object};
        }

        CO_RETURN rpc::connect_result{rpc::error::OK(), {}};
    }

    CORO_TASK(int) transport::inner_accept()
    {
        // Pump is already running — started by create() when connection_handler was provided.
        CO_RETURN rpc::error::OK();
    }

    CORO_TASK(send_result)
    transport::outbound_send(send_params params)
    {
        RPC_DEBUG("stream_transport::transport::outbound_send zone={}", get_zone_id().get_subnet());

        auto response_result = CO_AWAIT call_peer<call_send, call_receive>(params.protocol_version,
            call_send{.encoding = params.encoding_type,
                .tag = params.tag,
                .caller_zone_id = params.caller_zone_id,
                .destination_zone_id = params.remote_object_id,
                .interface_id = params.interface_id,
                .method_id = params.method_id,
                .payload = std::move(params.in_data),
                .back_channel = std::move(params.in_back_channel)});
        int ret = response_result.error_code;

        if (rpc::error::is_error(ret))
        {
            RPC_DEBUG("failed stream_transport::transport::outbound_send call_send");
            CO_RETURN send_result{ret, {}, {}};
        }

        auto& response = response_result.payload;
        RPC_DEBUG("stream_transport::transport::outbound_send complete zone={}", get_zone_id().get_subnet());
        CO_RETURN send_result{response.err_code, std::move(response.payload), std::move(response.back_channel)};
    }

    CORO_TASK(void)
    transport::outbound_post(post_params params)
    {
        RPC_DEBUG("stream_transport::transport::outbound_post zone={}", get_zone_id().get_subnet());

        if (get_status() != rpc::transport_status::CONNECTED)
        {
            RPC_ERROR("stream_transport::transport::outbound_post: transport not connected");
            CO_RETURN;
        }

        send_payload_post_send(params.protocol_version,
            message_direction::one_way,
            post_send{.encoding = params.encoding_type,
                .tag = params.tag,
                .caller_zone_id = params.caller_zone_id,
                .destination_zone_id = params.remote_object_id,
                .interface_id = params.interface_id,
                .method_id = params.method_id,
                .payload = std::move(params.in_data),
                .back_channel = std::move(params.in_back_channel)},
            0);

        CO_RETURN;
    }

    CORO_TASK(standard_result)
    transport::outbound_try_cast(try_cast_params params)
    {
        RPC_DEBUG("stream_transport::transport::outbound_try_cast zone={}", get_zone_id().get_subnet());

        auto response_result = CO_AWAIT call_peer<try_cast_send, try_cast_receive>(params.protocol_version,
            try_cast_send{.caller_zone_id = params.caller_zone_id,
                .destination_zone_id = params.remote_object_id,
                .interface_id = params.interface_id,
                .back_channel = std::move(params.in_back_channel)});
        int ret = response_result.error_code;
        if (ret != rpc::error::OK())
        {
            RPC_ERROR("failed try_cast call_peer");
            CO_RETURN standard_result{ret, {}};
        }

        auto& response_data = response_result.payload;
        CO_RETURN standard_result{response_data.err_code, std::move(response_data.back_channel)};
    }

    CORO_TASK(standard_result)
    transport::outbound_add_ref(add_ref_params params)
    {
        RPC_DEBUG("stream_transport::transport::outbound_add_ref zone={}", get_zone_id().get_subnet());

        auto response_result = CO_AWAIT call_peer<addref_send, addref_receive>(params.protocol_version,
            addref_send{.destination_zone_id = params.remote_object_id,
                .caller_zone_id = params.caller_zone_id,
                .requesting_zone_id = params.requesting_zone_id,
                .build_out_param_channel = params.build_out_param_channel,
                .back_channel = std::move(params.in_back_channel)});
        int ret = response_result.error_code;
        if (ret != rpc::error::OK())
        {
            RPC_ERROR("failed add_ref addref_send");
            CO_RETURN standard_result{ret, {}};
        }

        auto& response_data = response_result.payload;
        if (response_data.err_code != rpc::error::OK())
        {
            RPC_ERROR("failed addref_receive.err_code");
#ifdef CANOPY_USE_TELEMETRY
            if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
            {
                auto error_message = std::string("add_ref failed ") + std::to_string(response_data.err_code);
                telemetry_service->message(rpc::i_telemetry_service::err, error_message.c_str());
            }
#endif
            CO_RETURN standard_result{response_data.err_code, std::move(response_data.back_channel)};
        }

        RPC_DEBUG("stream_transport::transport::outbound_add_ref complete zone={}", get_zone_id().get_subnet());
        CO_RETURN standard_result{rpc::error::OK(), std::move(response_data.back_channel)};
    }

    CORO_TASK(standard_result)
    transport::outbound_release(release_params params)
    {
        RPC_DEBUG("stream_transport::transport::outbound_release zone={}", get_zone_id().get_subnet());

        if (get_status() != rpc::transport_status::CONNECTED && get_status() != rpc::transport_status::DISCONNECTING)
        {
            RPC_ERROR("failed stream_transport::transport::outbound_release - not connected, status = {}",
                static_cast<int>(get_status()));
            CO_RETURN standard_result{rpc::error::TRANSPORT_ERROR(), {}};
        }

        send_payload_release_send(params.protocol_version,
            message_direction::one_way,
            release_send{.destination_zone_id = params.remote_object_id,
                .caller_zone_id = params.caller_zone_id,
                .options = params.options,
                .back_channel = std::move(params.in_back_channel)},
            0);

        auto count = get_destination_count();
        RPC_ASSERT(count >= 0);
        if (count <= 0 && get_status() == rpc::transport_status::CONNECTED)
        {
            RPC_DEBUG("destination_count reached 0, triggering graceful shutdown");
            set_status(rpc::transport_status::DISCONNECTING);
        }

        CO_RETURN standard_result{rpc::error::OK(), {}};
    }

    CORO_TASK(void)
    transport::outbound_object_released(object_released_params params)
    {
        RPC_DEBUG("stream_transport::transport::outbound_object_released zone={}", get_zone_id().get_subnet());

        if (get_status() == rpc::transport_status::DISCONNECTED)
        {
            RPC_ERROR("failed stream_transport::transport::outbound_object_released - transport disconnected");
            CO_RETURN;
        }

        send_payload_object_released_send(params.protocol_version,
            message_direction::one_way,
            object_released_send{.encoding = encoding::yas_binary,
                .destination_zone_id = params.remote_object_id,
                .caller_zone_id = params.caller_zone_id,
                .back_channel = std::move(params.in_back_channel)},
            0);

        RPC_DEBUG("stream_transport::transport::outbound_object_released complete zone={}", get_zone_id().get_subnet());
    }

    CORO_TASK(void)
    transport::outbound_transport_down(transport_down_params params)
    {
        RPC_DEBUG("stream_transport::transport::outbound_transport_down zone={}", get_zone_id().get_subnet());

        if (get_status() == rpc::transport_status::DISCONNECTED)
        {
            RPC_ERROR("failed stream_transport::transport::outbound_transport_down - transport disconnected");
            CO_RETURN;
        }

        send_payload_transport_down_send(params.protocol_version,
            message_direction::one_way,
            transport_down_send{.encoding = encoding::yas_binary,
                .destination_zone_id = params.destination_zone_id,
                .caller_zone_id = params.caller_zone_id,
                .back_channel = std::move(params.in_back_channel)},
            0);

        RPC_DEBUG("stream_transport::transport::outbound_transport_down complete zone={}", get_zone_id().get_subnet());
    }

    CORO_TASK(transport::message_hook_result)
    transport::run_custom_message_handlers(message_handler_context context)
    {
        for (auto& handler : custom_message_handlers_)
        {
            auto result = CO_AWAIT handler(context);
            if (result != message_hook_result::unhandled)
                CO_RETURN result;
        }

        CO_RETURN message_hook_result::unhandled;
    }

    CORO_TASK(bool)
    transport::dispatch_builtin_message(message_handler_context context)
    {
        auto& prefix = *context.prefix;
        auto& payload = *context.payload;
        auto tracker = std::move(context.tracker);
        if (payload.payload_fingerprint == rpc::id<init_client_channel_send>::get(prefix.version))
        {
            get_service()->spawn(create_stub(tracker, prefix, std::move(payload)));
            CO_RETURN true;
        }
        else if (payload.payload_fingerprint == rpc::id<call_send>::get(prefix.version))
        {
            get_service()->spawn(stub_handle_send(tracker, prefix, std::move(payload)));
            CO_RETURN true;
        }
        else if (payload.payload_fingerprint == rpc::id<post_send>::get(prefix.version))
        {
            get_service()->spawn(stub_handle_post(tracker, prefix, std::move(payload)));
            CO_RETURN true;
        }
        else if (payload.payload_fingerprint == rpc::id<try_cast_send>::get(prefix.version))
        {
            get_service()->spawn(stub_handle_try_cast(tracker, prefix, std::move(payload)));
            CO_RETURN true;
        }
        else if (payload.payload_fingerprint == rpc::id<addref_send>::get(prefix.version))
        {
            get_service()->spawn(stub_handle_add_ref(tracker, prefix, std::move(payload)));
            CO_RETURN true;
        }
        else if (payload.payload_fingerprint == rpc::id<release_send>::get(prefix.version))
        {
            get_service()->spawn(stub_handle_release(tracker, prefix, std::move(payload)));
            CO_RETURN true;
        }
        else if (payload.payload_fingerprint == rpc::id<object_released_send>::get(prefix.version))
        {
            get_service()->spawn(stub_handle_object_released(tracker, prefix, std::move(payload)));
            CO_RETURN true;
        }
        else if (payload.payload_fingerprint == rpc::id<transport_down_send>::get(prefix.version))
        {
            get_service()->spawn(stub_handle_transport_down(tracker, prefix, std::move(payload)));
            CO_RETURN true;
        }
        else if (payload.payload_fingerprint == rpc::id<init_client_initial_channel_response>::get(prefix.version))
        {
            init_client_initial_channel_response early_response;
            auto str_err = rpc::from_yas_binary(rpc::byte_span(payload.payload), early_response);
            if (str_err.empty())
            {
                RPC_DEBUG("pump: received init_client_initial_channel_response, adjacent_zone={}",
                    early_response.zone_id.get_subnet());
                set_adjacent_zone_id(early_response.zone_id);
                get_service()->add_transport(early_response.zone_id, shared_from_this());

                if (stub_)
                {
                    auto ret = CO_AWAIT stub_->add_ref(false, false, early_response.zone_id);
                    stub_.reset();
                    if (ret != rpc::error::OK())
                    {
                        set_status(transport_status::DISCONNECTING);
                    }
                }
            }
            else
            {
                RPC_ERROR("failed to deserialize init_client_initial_channel_response");
            }
            CO_RETURN true;
        }
        else if (payload.payload_fingerprint == rpc::id<close_connection_send>::get(prefix.version))
        {
            set_status(rpc::transport_status::DISCONNECTING);
            peer_requested_disconnection_ = true;
            CO_RETURN true;
        }
        else if (payload.payload_fingerprint == rpc::id<close_connection_ack>::get(prefix.version))
        {
            set_status(rpc::transport_status::DISCONNECTED);
            CO_RETURN true;
        }

        CO_RETURN false;
    }

    CORO_TASK(void) transport::pump_send_and_receive()
    {
        auto self = shared_from_this();
        RPC_DEBUG("pump_send_and_receive zone={}", get_zone_id().get_subnet());

        bool expected = false;
        if (!pumps_started_.compare_exchange_strong(expected, true))
        {
            RPC_ERROR("pump_send_and_receive called MULTIPLE TIMES on zone {} - BUG!", get_zone_id().get_subnet());
            CO_RETURN;
        }

        auto svc = get_service();
        auto tracker = std::shared_ptr<activity_tracker>(new activity_tracker{
            .transport = std::static_pointer_cast<rpc::stream_transport::transport>(self), .svc = svc});
        svc->spawn(receive_consumer_loop(tracker));
        svc->spawn(send_producer_loop(tracker));

        RPC_DEBUG("pump_send_and_receive: scheduled tasks for zone {}", get_zone_id().get_subnet());
    }

    CORO_TASK(void)
    transport::receive_consumer_loop(std::shared_ptr<activity_tracker> tracker)
    {
        auto self = shared_from_this();
        auto svc = get_service();
        RPC_ASSERT(svc);
        static auto envelope_prefix_saved_size = rpc::yas_binary_saved_size(envelope_prefix());

        std::vector<char> prefix_buf(envelope_prefix_saved_size);
        std::vector<char> payload_buf;

        bool receiving_prefix = true;
        rpc::mutable_byte_span remaining;

        RPC_DEBUG("receive_consumer_loop started for zone {}", get_zone_id().get_subnet());

        envelope_prefix prefix{};

        bool stop_loop = false;
        while (get_status() < rpc::transport_status::DISCONNECTED && !stop_loop)
        {
            if (receiving_prefix && remaining.empty())
                remaining = prefix_buf;

            auto [recv_status, recv_bytes] = CO_AWAIT stream_->receive(remaining, std::chrono::milliseconds(1));

            if (recv_status.is_closed())
            {
                RPC_DEBUG("receive_consumer_loop: stream closed for zone {}", get_zone_id().get_subnet());
                break;
            }

            if (recv_status.is_timeout())
            {
                if (get_status() == rpc::transport_status::DISCONNECTING)
                {
                    if (peer_requested_disconnection_)
                    {
                        if (send_cleanup_done_.load(std::memory_order_acquire))
                        {
                            send_payload_close_connection_ack(
                                rpc::get_version(), message_direction::one_way, close_connection_ack{}, 0);
                            CO_AWAIT flush_send_queue();
                            set_status(rpc::transport_status::DISCONNECTED);
                            stop_loop = true;
                            break;
                        }

                        auto elapsed = std::chrono::steady_clock::now() - disconnecting_since_;
                        if (elapsed >= std::chrono::milliseconds(shutdown_timeout_ms_))
                        {
                            RPC_WARNING(
                                "receive_consumer_loop: responder shutdown timeout for zone {}, forcing DISCONNECTED",
                                get_zone_id().get_subnet());
                            set_status(rpc::transport_status::DISCONNECTED);
                            stop_loop = true;
                            break;
                        }
                    }
                    else
                    {
                        auto elapsed = std::chrono::steady_clock::now() - disconnecting_since_;
                        if (elapsed >= std::chrono::milliseconds(shutdown_timeout_ms_))
                        {
                            RPC_WARNING(
                                "receive_consumer_loop: initiator shutdown timeout for zone {}, forcing DISCONNECTED",
                                get_zone_id().get_subnet());
                            set_status(rpc::transport_status::DISCONNECTED);
                            stop_loop = true;
                            break;
                        }
                    }
                }
                CO_AWAIT svc->schedule();
                continue;
            }

            if (!recv_status.is_ok())
            {
                RPC_WARNING("receive_consumer_loop: stream receive failed for zone {} status={} native={}",
                    get_zone_id().get_subnet(),
                    static_cast<int>(recv_status.type),
                    recv_status.native_code);
                break;
            }

            remaining = remaining.subspan(recv_bytes.size());

            if (!remaining.empty())
                continue;

            if (receiving_prefix)
            {
                auto str_err = rpc::from_yas_binary(rpc::byte_span(prefix_buf), prefix);
                if (!str_err.empty())
                {
                    RPC_ERROR("Deserialization FAILED: {}", str_err);
                    break;
                }
                if (static_cast<uint64_t>(prefix.direction) == 0)
                {
                    RPC_ERROR("invalid envelope prefix: missing direction");
                    set_status(rpc::transport_status::DISCONNECTING);
                    break;
                }
                receiving_prefix = false;
                payload_buf.assign(prefix.payload_size, '\0');
                remaining = payload_buf;
                continue;
            }

            // Full payload received — dispatch
            {
                RPC_DEBUG("receive_consumer_loop: full payload received seq={} bytes={} zone={}",
                    prefix.sequence_number,
                    payload_buf.size(),
                    get_zone_id().get_subnet());
                envelope_payload payload;
                auto str_err = rpc::from_yas_binary(rpc::byte_span(payload_buf), payload);
                if (!str_err.empty())
                {
                    RPC_ERROR("invalid envelope payload: {}", str_err);
                    set_status(rpc::transport_status::DISCONNECTING);
                    break;
                }

                auto hook_result = CO_AWAIT run_custom_message_handlers(
                    message_handler_context{.tracker = tracker, .prefix = &prefix, .payload = &payload});
                if (hook_result == message_hook_result::handled)
                {
                    receiving_prefix = true;
                    remaining = rpc::mutable_byte_span{};
                    continue;
                }
                else if (hook_result == message_hook_result::rejected)
                {
                    RPC_WARNING("message rejected fingerprint={}", payload.payload_fingerprint);
                    set_status(rpc::transport_status::DISCONNECTING);
                    break;
                }

                if (CO_AWAIT dispatch_builtin_message(
                        message_handler_context{.tracker = tracker, .prefix = &prefix, .payload = &payload}))
                {
                    if (get_status() == rpc::transport_status::DISCONNECTED)
                    {
                        stop_loop = true;
                    }
                }
                else
                {
                    // Find the pending result listener and wake it
                    std::shared_ptr<result_listener> result;
                    {
                        std::scoped_lock lock(pending_transmits_mtx_);
                        auto it = pending_transmits_.find(prefix.sequence_number);
                        RPC_DEBUG("pending_transmits_ zone: {} sequence_number: {} id: {}",
                            get_zone_id().get_subnet(),
                            prefix.sequence_number,
                            payload.payload_fingerprint);

                        if (it != pending_transmits_.end())
                        {
                            result = std::move(it->second);
                            pending_transmits_.erase(it);
                        }
                        else
                        {
                            RPC_WARNING("No pending transmit found for sequence_number: {}, ignoring message id: {}",
                                prefix.sequence_number,
                                payload.payload_fingerprint);
                        }
                    }

                    if (result)
                    {
                        result->prefix = prefix;
                        result->payload = std::move(payload);
                        RPC_DEBUG("pump receive prefix.sequence_number {}\n prefix = {}\n payload = {}",
                            get_zone_id().get_subnet(),
                            rpc::to_yas_json<std::string>(result->prefix),
                            rpc::to_yas_json<std::string>(result->payload));

                        result->event.set();
                    }
                }

                receiving_prefix = true;
                remaining = rpc::mutable_byte_span{};
            }
        }

        if (get_status() < rpc::transport_status::DISCONNECTED)
        {
            RPC_DEBUG("receive_consumer_loop: forcing DISCONNECTED for zone {}", get_zone_id().get_subnet());
            set_status(rpc::transport_status::DISCONNECTED);
        }

        RPC_DEBUG("receive_consumer_loop exiting for zone {}", get_zone_id().get_subnet());
        CO_RETURN;
    }

    CORO_TASK(bool) transport::flush_send_queue()
    {
        while (true)
        {
            queued_send_message item;
            {
                std::scoped_lock g(send_queue_mtx_);
                if (!high_priority_send_queue_.empty())
                {
                    item = std::move(high_priority_send_queue_.front());
                    high_priority_send_queue_.pop();
                }
                else if (!normal_send_queue_.empty())
                {
                    item = std::move(normal_send_queue_.front());
                    normal_send_queue_.pop();
                }
                else
                    break;
            }
            auto send_part = [this](const std::vector<uint8_t>& data) -> CORO_TASK(coro::net::io_status)
            {
                if (data.empty())
                    CO_RETURN coro::net::io_status{.type = coro::net::io_status::kind::ok};
                CO_RETURN CO_AWAIT stream_->send(rpc::byte_span{reinterpret_cast<const char*>(data.data()), data.size()});
            };

            auto send_status = CO_AWAIT send_part(item.prefix_data);
            if (send_status.is_ok())
                send_status = CO_AWAIT send_part(item.payload_data);
            if (!send_status.is_ok())
            {
                if (is_expected_disconnect_send_failure(get_status(), send_status))
                {
                    RPC_DEBUG("flush_send_queue: expected disconnect-time send failure for zone {} status={} native={}",
                        get_zone_id().get_subnet(),
                        static_cast<int>(send_status.type),
                        send_status.native_code);
                }
                else
                {
                    RPC_WARNING("flush_send_queue: stream send failed for zone {} status={} native={}",
                        get_zone_id().get_subnet(),
                        static_cast<int>(send_status.type),
                        send_status.native_code);
                }
                if (get_status() < rpc::transport_status::DISCONNECTED)
                    set_status(rpc::transport_status::DISCONNECTED);
                CO_RETURN false;
            }
        }
        CO_RETURN true;
    }

    CORO_TASK(void) transport::send_producer_loop(std::shared_ptr<activity_tracker> tracker)
    {
        auto self = shared_from_this();
        auto svc = get_service();
        RPC_ASSERT(svc);

        RPC_DEBUG("send_producer_loop started for zone {}", get_zone_id().get_subnet());

        while (get_status() == rpc::transport_status::CONNECTED)
        {
            queued_send_message item;
            bool had_item = false;
            {
                std::scoped_lock g(send_queue_mtx_);
                if (!high_priority_send_queue_.empty())
                {
                    item = std::move(high_priority_send_queue_.front());
                    high_priority_send_queue_.pop();
                    had_item = true;
                }
                else if (!normal_send_queue_.empty())
                {
                    item = std::move(normal_send_queue_.front());
                    normal_send_queue_.pop();
                    had_item = true;
                }
                else if (get_status() == rpc::transport_status::CONNECTED)
                {
                    send_queue_ready_.reset();
                }
            }

            if (had_item)
            {
                RPC_DEBUG("send_producer_loop: sending seq={} direction={} payload_bytes={} zone={}",
                    item.sequence_number,
                    static_cast<uint64_t>(item.direction),
                    item.payload_data.size(),
                    get_zone_id().get_subnet());
                auto send_part = [this](const std::vector<uint8_t>& data) -> CORO_TASK(coro::net::io_status)
                {
                    if (data.empty())
                        CO_RETURN coro::net::io_status{.type = coro::net::io_status::kind::ok};
                    CO_RETURN CO_AWAIT stream_->send(
                        rpc::byte_span{reinterpret_cast<const char*>(data.data()), data.size()});
                };
                auto send_status = CO_AWAIT send_part(item.prefix_data);
                if (send_status.is_ok())
                    send_status = CO_AWAIT send_part(item.payload_data);
                if (send_status.is_ok())
                {
                    RPC_DEBUG("send_producer_loop: sent seq={} zone={}", item.sequence_number, get_zone_id().get_subnet());
                }
                if (!send_status.is_ok())
                {
                    if (is_expected_disconnect_send_failure(get_status(), send_status))
                    {
                        RPC_DEBUG(
                            "send_producer_loop: expected disconnect-time send failure for zone {} status={} native={}",
                            get_zone_id().get_subnet(),
                            static_cast<int>(send_status.type),
                            send_status.native_code);
                    }
                    else
                    {
                        RPC_WARNING("send_producer_loop: stream send failed for zone {} status={} native={}",
                            get_zone_id().get_subnet(),
                            static_cast<int>(send_status.type),
                            send_status.native_code);
                    }
                    if (get_status() < rpc::transport_status::DISCONNECTED)
                        set_status(rpc::transport_status::DISCONNECTED);
                    break;
                }
            }
            else
            {
                if (get_status() != rpc::transport_status::CONNECTED)
                    continue;
                CO_AWAIT svc->schedule();
                if (get_status() != rpc::transport_status::CONNECTED)
                    continue;
                if (send_queue_ready_.is_set())
                    continue;
                RPC_DEBUG("send_producer_loop: waiting on send_queue_ready_ zone={}", get_zone_id().get_subnet());
                CO_AWAIT send_queue_ready_;
                RPC_DEBUG("send_producer_loop: send_queue_ready_ fired zone={}", get_zone_id().get_subnet());
            }
        }

        // Flush any messages queued before DISCONNECTING was set
        if (get_status() == rpc::transport_status::DISCONNECTED)
        {
            send_cleanup_done_.store(true, std::memory_order_release);
            RPC_DEBUG("send_producer_loop exiting after disconnect for zone {}", get_zone_id().get_subnet());
            CO_RETURN;
        }

        if (!(CO_AWAIT flush_send_queue()))
        {
            send_cleanup_done_.store(true, std::memory_order_release);
            RPC_DEBUG("send_producer_loop aborting cleanup flush for zone {}", get_zone_id().get_subnet());
            CO_RETURN;
        }

        // Send cleanup notifications — adds stub-release and transport-down messages
        CO_AWAIT notify_all_destinations_of_disconnect();

        if (!peer_requested_disconnection_)
        {
            RPC_DEBUG("send_producer_loop: sending close_connection_send for zone {}", get_zone_id().get_subnet());
            send_payload_close_connection_send(rpc::get_version(), message_direction::one_way, close_connection_send{}, 0);
        }

        if (!(CO_AWAIT flush_send_queue()))
        {
            send_cleanup_done_.store(true, std::memory_order_release);
            RPC_DEBUG("send_producer_loop aborting final cleanup flush for zone {}", get_zone_id().get_subnet());
            CO_RETURN;
        }

        RPC_DEBUG("send_producer_loop: cleanup done, signalling receive loop for zone {}", get_zone_id().get_subnet());
        send_cleanup_done_.store(true, std::memory_order_release);

        RPC_DEBUG("send_producer_loop completed for zone {}", get_zone_id().get_subnet());
        CO_RETURN;
    }

    CORO_TASK(void)
    transport::cleanup(std::shared_ptr<rpc::stream_transport::transport> transport, std::shared_ptr<rpc::service> svc)
    {
        RPC_DEBUG("Both loops completed, finalising transport for zone {}", transport->get_zone_id().get_subnet());
        {
            std::scoped_lock lock(transport->pending_transmits_mtx_);
            for (auto it : transport->pending_transmits_)
            {
                it.second->error_code = rpc::error::CALL_CANCELLED();
                it.second->event.set();
            }
            transport->pending_transmits_.clear();
        }
        if (transport->stream_)
        {
            CO_AWAIT transport->stream_->set_closed();
            transport->stream_.reset();
        }
        transport->keep_alive_.reset();
        co_return;
    }

    CORO_TASK(void)
    transport::stub_handle_send(std::shared_ptr<activity_tracker>, envelope_prefix prefix, envelope_payload payload)
    {
        RPC_DEBUG("stub_handle_send");

        if (prefix.direction != message_direction::send && prefix.direction != message_direction::one_way)
        {
            RPC_ERROR("invalid call_send direction {}", static_cast<uint64_t>(prefix.direction));
            set_status(rpc::transport_status::DISCONNECTING);
            CO_RETURN;
        }

        if (get_status() != rpc::transport_status::CONNECTED)
        {
            CO_RETURN;
        }

        call_send request;
        auto str_err = rpc::from_yas_binary(rpc::byte_span(payload.payload), request);
        if (!str_err.empty())
        {
            RPC_ERROR("failed from_yas_binary call_send");
            set_status(rpc::transport_status::DISCONNECTING);
            CO_RETURN;
        }

        auto send_result = CO_AWAIT inbound_send(rpc::send_params{
            .protocol_version = prefix.version,
            .encoding_type = request.encoding,
            .tag = request.tag,
            .caller_zone_id = request.caller_zone_id,
            .remote_object_id = request.destination_zone_id,
            .interface_id = request.interface_id,
            .method_id = request.method_id,
            .in_data = std::move(request.payload),
            .in_back_channel = std::move(request.back_channel),
        });

        if (rpc::error::is_error(send_result.error_code))
        {
            RPC_DEBUG("inbound_send error {}", send_result.error_code);
        }

        if (prefix.direction == message_direction::one_way)
            CO_RETURN;

        send_payload_call_receive(prefix.version,
            message_direction::receive,
            call_receive{.payload = std::move(send_result.out_buf),
                .back_channel = std::move(send_result.out_back_channel),
                .err_code = send_result.error_code},
            prefix.sequence_number);
        RPC_DEBUG("stub_handle_send complete");
        CO_RETURN;
    }

    CORO_TASK(void)
    transport::stub_handle_post(std::shared_ptr<activity_tracker>, envelope_prefix prefix, envelope_payload payload)
    {
        RPC_DEBUG("stub_handle_post");

        if (get_status() != rpc::transport_status::CONNECTED)
        {
            CO_RETURN;
        }

        post_send request;
        auto str_err = rpc::from_yas_binary(rpc::byte_span(payload.payload), request);
        if (!str_err.empty())
        {
            RPC_ERROR("failed post_send from_yas_binary");
            set_status(rpc::transport_status::DISCONNECTING);
            CO_RETURN;
        }

        CO_AWAIT inbound_post(rpc::post_params{
            .protocol_version = prefix.version,
            .encoding_type = request.encoding,
            .tag = request.tag,
            .caller_zone_id = request.caller_zone_id,
            .remote_object_id = request.destination_zone_id,
            .interface_id = request.interface_id,
            .method_id = request.method_id,
            .in_data = std::move(request.payload),
            .in_back_channel = std::move(request.back_channel),
        });

        RPC_DEBUG("stub_handle_post complete");
        CO_RETURN;
    }

    CORO_TASK(void)
    transport::stub_handle_try_cast(std::shared_ptr<activity_tracker>, envelope_prefix prefix, envelope_payload payload)
    {
        RPC_DEBUG("stub_handle_try_cast");

        if (get_status() != rpc::transport_status::CONNECTED)
        {
            CO_RETURN;
        }

        try_cast_send request;
        auto str_err = rpc::from_yas_binary(rpc::byte_span(payload.payload), request);
        if (!str_err.empty())
        {
            RPC_ERROR("failed try_cast_send from_yas_binary");
            set_status(rpc::transport_status::DISCONNECTING);
            CO_RETURN;
        }

        auto tc_result = CO_AWAIT inbound_try_cast(rpc::try_cast_params{
            .protocol_version = prefix.version,
            .caller_zone_id = request.caller_zone_id,
            .remote_object_id = request.destination_zone_id,
            .interface_id = request.interface_id,
            .in_back_channel = std::move(request.back_channel),
        });

        if (rpc::error::is_error(tc_result.error_code))
        {
            RPC_DEBUG("inbound_try_cast error {}", tc_result.error_code);
        }

        send_payload_try_cast_receive(prefix.version,
            message_direction::receive,
            try_cast_receive{.back_channel = std::move(tc_result.out_back_channel), .err_code = tc_result.error_code},
            prefix.sequence_number);
        RPC_DEBUG("stub_handle_try_cast complete");
        CO_RETURN;
    }

    CORO_TASK(void)
    transport::stub_handle_add_ref(std::shared_ptr<activity_tracker>, envelope_prefix prefix, envelope_payload payload)
    {
        RPC_DEBUG("stub_handle_add_ref");

        if (get_status() != rpc::transport_status::CONNECTED)
        {
            CO_RETURN;
        }

        addref_send request;
        auto str_err = rpc::from_yas_binary(rpc::byte_span(payload.payload), request);
        if (!str_err.empty())
        {
            RPC_ERROR("failed addref_send from_yas_binary");
            set_status(rpc::transport_status::DISCONNECTING);
            CO_RETURN;
        }

        auto ar_result = CO_AWAIT inbound_add_ref(rpc::add_ref_params{
            .protocol_version = prefix.version,
            .remote_object_id = request.destination_zone_id,
            .caller_zone_id = request.caller_zone_id,
            .requesting_zone_id = request.requesting_zone_id,
            .build_out_param_channel = request.build_out_param_channel,
            .in_back_channel = std::move(request.back_channel),
        });

        if (rpc::error::is_error(ar_result.error_code))
        {
            RPC_DEBUG("inbound_add_ref error {}", ar_result.error_code);
        }

        send_payload_addref_receive(prefix.version,
            message_direction::receive,
            addref_receive{.back_channel = std::move(ar_result.out_back_channel), .err_code = ar_result.error_code},
            prefix.sequence_number);
        RPC_DEBUG("stub_handle_add_ref complete");
        CO_RETURN;
    }

    CORO_TASK(void)
    transport::stub_handle_release(std::shared_ptr<activity_tracker>, envelope_prefix prefix, envelope_payload payload)
    {
        RPC_DEBUG("stub_handle_release");

        release_send request;
        auto str_err = rpc::from_yas_binary(rpc::byte_span(payload.payload), request);
        if (!str_err.empty())
        {
            RPC_ERROR("failed release_send from_yas_binary");
            set_status(rpc::transport_status::DISCONNECTING);
            CO_RETURN;
        }

        auto rel_result = CO_AWAIT inbound_release(rpc::release_params{
            .protocol_version = prefix.version,
            .remote_object_id = request.destination_zone_id,
            .caller_zone_id = request.caller_zone_id,
            .options = request.options,
            .in_back_channel = std::move(request.back_channel),
        });

        if (rpc::error::is_error(rel_result.error_code))
        {
            RPC_DEBUG("inbound_release error {}", rel_result.error_code);
        }

        auto count = get_destination_count();
        RPC_ASSERT(count >= 0);
        if (count <= 0)
        {
            RPC_DEBUG("destination_count reached 0, triggering disconnect");
            set_status(rpc::transport_status::DISCONNECTING);
        }
        RPC_DEBUG("stub_handle_release complete");
        CO_RETURN;
    }

    CORO_TASK(void)
    transport::stub_handle_object_released(
        std::shared_ptr<activity_tracker>, envelope_prefix prefix, envelope_payload payload)
    {
        RPC_DEBUG("stub_handle_object_released");

        object_released_send request;
        auto str_err = rpc::from_yas_binary(rpc::byte_span(payload.payload), request);
        if (!str_err.empty())
        {
            RPC_ERROR("failed object_released_send from_yas_binary");
            set_status(rpc::transport_status::DISCONNECTING);
            CO_RETURN;
        }

        CO_AWAIT inbound_object_released(rpc::object_released_params{
            .protocol_version = prefix.version,
            .remote_object_id = request.destination_zone_id,
            .caller_zone_id = request.caller_zone_id,
            .in_back_channel = std::move(request.back_channel),
        });

        RPC_DEBUG("stub_handle_object_released complete");
        CO_RETURN;
    }

    CORO_TASK(void)
    transport::stub_handle_transport_down(std::shared_ptr<activity_tracker>, envelope_prefix prefix, envelope_payload payload)
    {
        RPC_DEBUG("stub_handle_transport_down");

        transport_down_send request;
        auto str_err = rpc::from_yas_binary(rpc::byte_span(payload.payload), request);
        if (!str_err.empty())
        {
            RPC_ERROR("failed transport_down_send from_yas_binary");
            set_status(rpc::transport_status::DISCONNECTING);
            CO_RETURN;
        }

        CO_AWAIT inbound_transport_down(rpc::transport_down_params{
            .protocol_version = prefix.version,
            .destination_zone_id = request.destination_zone_id,
            .caller_zone_id = request.caller_zone_id,
            .in_back_channel = std::move(request.back_channel),
        });

        RPC_DEBUG("stub_handle_transport_down complete");
        CO_RETURN;
    }

    CORO_TASK(void)
    transport::create_stub(std::shared_ptr<activity_tracker>, envelope_prefix prefix, envelope_payload payload)
    {
        RPC_DEBUG("create_stub zone: {}", get_zone_id().get_subnet());

        init_client_channel_send request;
        auto err = rpc::from_yas_binary(rpc::byte_span(payload.payload), request);
        if (!err.empty())
        {
            RPC_ERROR("failed create_stub init_client_channel_send deserialization");
            CO_RETURN;
        }

        rpc::connection_settings input_descr;
        input_descr.inbound_interface_id = request.inbound_interface_id;
        input_descr.outbound_interface_id = request.outbound_interface_id;
        input_descr.remote_object_id = request.inbound_remote_object;
        set_adjacent_zone_id(request.adjacent_zone_id);

        // Immediately inform the peer of our zone_id before invoking connection_handler_
        send_payload_init_client_initial_channel_response(
            prefix.version, message_direction::one_way, init_client_initial_channel_response{.zone_id = get_zone_id()}, 0);

        auto connect_result = CO_AWAIT connection_handler_(input_descr, get_service(), keep_alive_.get_nullable());
        connection_handler_ = nullptr;
        auto& output_interface = connect_result.output_descriptor;
        int ret = connect_result.error_code;
        if (ret != rpc::error::OK())
        {
            RPC_ERROR("failed to connect to zone {}", ret);
            CO_RETURN;
        }

        send_payload_init_client_channel_response(prefix.version,
            message_direction::receive,
            init_client_channel_response{.err_code = rpc::error::OK(),
                .outbound_remote_object = output_interface,
                .caller_zone_id = input_descr.remote_object_id.as_zone()},
            prefix.sequence_number);

        CO_RETURN;
    }

    void transport::send_payload_post_send(
        uint64_t protocol_version, message_direction direction, post_send&& payload, uint64_t sequence_number)
    {
        if (run_outgoing_handlers(protocol_version, direction, sequence_number, payload))
            return;
        send_payload(protocol_version, direction, std::move(payload), sequence_number);
    }

    void transport::send_payload_release_send(
        uint64_t protocol_version, message_direction direction, release_send&& payload, uint64_t sequence_number)
    {
        if (run_outgoing_handlers(protocol_version, direction, sequence_number, payload))
            return;
        send_payload(protocol_version, direction, std::move(payload), sequence_number);
    }

    void transport::send_payload_object_released_send(
        uint64_t protocol_version, message_direction direction, object_released_send&& payload, uint64_t sequence_number)
    {
        if (run_outgoing_handlers(protocol_version, direction, sequence_number, payload))
            return;
        send_payload(protocol_version, direction, std::move(payload), sequence_number);
    }

    void transport::send_payload_transport_down_send(
        uint64_t protocol_version, message_direction direction, transport_down_send&& payload, uint64_t sequence_number)
    {
        if (run_outgoing_handlers(protocol_version, direction, sequence_number, payload))
            return;
        send_payload(protocol_version, direction, std::move(payload), sequence_number);
    }

    void transport::send_payload_close_connection_ack(
        uint64_t protocol_version, message_direction direction, close_connection_ack&& payload, uint64_t sequence_number)
    {
        if (run_outgoing_handlers(protocol_version, direction, sequence_number, payload))
            return;
        send_payload(protocol_version, direction, payload, sequence_number, send_priority::high);
    }

    void transport::send_payload_close_connection_send(
        uint64_t protocol_version, message_direction direction, close_connection_send&& payload, uint64_t sequence_number)
    {
        if (run_outgoing_handlers(protocol_version, direction, sequence_number, payload))
            return;
        send_payload(protocol_version, direction, payload, sequence_number, send_priority::high);
    }

    void transport::send_payload_call_receive(
        uint64_t protocol_version, message_direction direction, call_receive&& payload, uint64_t sequence_number)
    {
        if (run_outgoing_handlers(protocol_version, direction, sequence_number, payload))
            return;
        send_payload(protocol_version, direction, std::move(payload), sequence_number, send_priority::high);
    }

    void transport::send_payload_try_cast_receive(
        uint64_t protocol_version, message_direction direction, try_cast_receive&& payload, uint64_t sequence_number)
    {
        if (run_outgoing_handlers(protocol_version, direction, sequence_number, payload))
            return;
        send_payload(protocol_version, direction, std::move(payload), sequence_number, send_priority::high);
    }

    void transport::send_payload_addref_receive(
        uint64_t protocol_version, message_direction direction, addref_receive&& payload, uint64_t sequence_number)
    {
        if (run_outgoing_handlers(protocol_version, direction, sequence_number, payload))
            return;
        send_payload(protocol_version, direction, std::move(payload), sequence_number, send_priority::high);
    }

    void transport::send_payload_init_client_initial_channel_response(uint64_t protocol_version,
        message_direction direction,
        init_client_initial_channel_response&& payload,
        uint64_t sequence_number)
    {
        if (run_outgoing_handlers(protocol_version, direction, sequence_number, payload))
            return;
        send_payload(protocol_version, direction, payload, sequence_number, send_priority::high);
    }

    void transport::send_payload_init_client_channel_response(uint64_t protocol_version,
        message_direction direction,
        init_client_channel_response&& payload,
        uint64_t sequence_number)
    {
        if (run_outgoing_handlers(protocol_version, direction, sequence_number, payload))
            return;
        send_payload(protocol_version, direction, payload, sequence_number, send_priority::high);
    }
}
