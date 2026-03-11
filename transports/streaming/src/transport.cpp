/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <transports/streaming/transport.h>

namespace rpc::stream_transport
{
    streaming_transport::streaming_transport(std::string name,
        std::shared_ptr<rpc::service> service,
        std::shared_ptr<streaming::stream> stream,
        connection_handler handler)
        : rpc::transport(name, service)
        , stream_(std::move(stream))
        , connection_handler_(std::move(handler))
    {
    }

    std::shared_ptr<streaming_transport> streaming_transport::create(std::string name,
        std::shared_ptr<rpc::service> service,
        std::shared_ptr<streaming::stream> stream,
        connection_handler handler)
    {
        auto transport = std::shared_ptr<streaming_transport>(
            new streaming_transport(name, service, std::move(stream), std::move(handler)));

        transport->keep_alive_ = transport;
        transport->set_status(rpc::transport_status::CONNECTED);

        // Server-side transports (those with a connection_handler) start the pump immediately
        // so they can receive the client's init message.  Client-side transports have no
        // connection_handler and their pump is started inside inner_connect().
        if (transport->connection_handler_)
            service->spawn(transport->pump_send_and_receive());

        return transport;
    }

    CORO_TASK(int)
    streaming_transport::inner_connect(const std::shared_ptr<rpc::object_stub>& stub,
        connection_settings& input_descr,
        rpc::interface_descriptor& output_descr)
    {
        RPC_DEBUG("streaming_transport::inner_connect zone={}", get_zone_id().get_subnet());
        stub_ = stub;

        auto service = get_service();
        RPC_DEBUG("inner_connect: spawning pump for zone {}", get_zone_id().get_subnet());
        service->spawn(pump_send_and_receive());
        RPC_DEBUG("inner_connect: pump spawned, calling call_peer for zone {}", get_zone_id().get_subnet());

        if (!connection_handler_)
        {
            // Client side: send init message to server
            init_client_channel_response init_receive;
            int ret = CO_AWAIT call_peer(rpc::get_version(),
                init_client_channel_send{.inbound_remote_object = get_zone_id().with_object(input_descr.get_object_id()),
                    .inbound_interface_id = input_descr.inbound_interface_id,
                    .destination_zone_id = get_adjacent_zone_id(),
                    .outbound_interface_id = input_descr.outbound_interface_id,
                    .adjacent_zone_id = get_zone_id()},
                init_receive);
            if (ret != rpc::error::OK())
            {
                stub_.reset();
                RPC_ERROR("streaming_transport::inner_connect call_peer failed {}", rpc::error::to_string(ret));
                CO_RETURN ret;
            }

            if (init_receive.err_code != rpc::error::OK())
            {
                stub_.reset();
                RPC_ERROR("init_client_channel_send failed");
                CO_RETURN init_receive.err_code;
            }

            output_descr = rpc::interface_descriptor(init_receive.outbound_remote_object);
        }

        CO_RETURN rpc::error::OK();
    }

    CORO_TASK(int) streaming_transport::inner_accept()
    {
        // Pump is already running — started by create() when connection_handler was provided.
        CO_RETURN rpc::error::OK();
    }

    CORO_TASK(int)
    streaming_transport::outbound_send(uint64_t protocol_version,
        rpc::encoding encoding,
        uint64_t tag,
        rpc::caller_zone caller_zone_id,
        rpc::remote_object remote_object_id,
        rpc::interface_ordinal interface_id,
        rpc::method method_id,
        const rpc::span& in_data,
        std::vector<char>& out_buf_,
        const std::vector<rpc::back_channel_entry>& in_back_channel,
        std::vector<rpc::back_channel_entry>& out_back_channel)
    {
        RPC_DEBUG("streaming_transport::outbound_send zone={}", get_zone_id().get_subnet());

        call_receive response;
        int ret = CO_AWAIT call_peer(protocol_version,
            call_send{.encoding = encoding,
                .tag = tag,
                .caller_zone_id = caller_zone_id,
                .destination_zone_id = remote_object_id,
                .interface_id = interface_id,
                .method_id = method_id,
                .payload = std::vector<char>((const char*)in_data.begin, (const char*)in_data.end),
                .back_channel = in_back_channel},
            response);

        if (rpc::error::is_error(ret))
        {
            RPC_DEBUG("failed streaming_transport::outbound_send call_send");
            CO_RETURN ret;
        }

        out_buf_.swap(response.payload);
        out_back_channel.swap(response.back_channel);

        RPC_DEBUG("streaming_transport::outbound_send complete zone={}", get_zone_id().get_subnet());
        CO_RETURN response.err_code;
    }

    CORO_TASK(void)
    streaming_transport::outbound_post(uint64_t protocol_version,
        rpc::encoding encoding,
        uint64_t tag,
        rpc::caller_zone caller_zone_id,
        rpc::remote_object remote_object_id,
        rpc::interface_ordinal interface_id,
        rpc::method method_id,
        const rpc::span& in_data,
        const std::vector<rpc::back_channel_entry>& in_back_channel)
    {
        RPC_DEBUG("streaming_transport::outbound_post zone={}", get_zone_id().get_subnet());

        if (get_status() != rpc::transport_status::CONNECTED)
        {
            RPC_ERROR("streaming_transport::outbound_post: transport not connected");
            CO_RETURN;
        }

        send_payload(protocol_version,
            message_direction::one_way,
            post_send{.encoding = encoding,
                .tag = tag,
                .caller_zone_id = caller_zone_id,
                .destination_zone_id = remote_object_id,
                .interface_id = interface_id,
                .method_id = method_id,
                .payload = std::vector<char>((const char*)in_data.begin, (const char*)in_data.end),
                .back_channel = in_back_channel},
            0);

        CO_RETURN;
    }

    CORO_TASK(int)
    streaming_transport::outbound_try_cast(uint64_t protocol_version,
        rpc::caller_zone caller_zone_id,
        rpc::remote_object remote_object_id,
        rpc::interface_ordinal interface_id,
        const std::vector<rpc::back_channel_entry>& in_back_channel,
        std::vector<rpc::back_channel_entry>& out_back_channel)
    {
        RPC_DEBUG("streaming_transport::outbound_try_cast zone={}", get_zone_id().get_subnet());

        try_cast_receive response_data;
        int ret = CO_AWAIT call_peer(protocol_version,
            try_cast_send{.caller_zone_id = caller_zone_id,
                .destination_zone_id = remote_object_id,
                .interface_id = interface_id,
                .back_channel = in_back_channel},
            response_data);
        if (ret != rpc::error::OK())
        {
            RPC_ERROR("failed try_cast call_peer");
            CO_RETURN ret;
        }

        out_back_channel.swap(response_data.back_channel);
        CO_RETURN response_data.err_code;
    }

    CORO_TASK(int)
    streaming_transport::outbound_add_ref(uint64_t protocol_version,
        rpc::remote_object remote_object_id,
        rpc::caller_zone caller_zone_id,
        rpc::requesting_zone requesting_zone_id,
        rpc::add_ref_options build_out_param_channel,
        const std::vector<rpc::back_channel_entry>& in_back_channel,
        std::vector<rpc::back_channel_entry>& out_back_channel)
    {
        RPC_DEBUG("streaming_transport::outbound_add_ref zone={}", get_zone_id().get_subnet());

        addref_receive response_data;
        int ret = CO_AWAIT call_peer(protocol_version,
            addref_send{.destination_zone_id = remote_object_id,
                .caller_zone_id = caller_zone_id,
                .requesting_zone_id = requesting_zone_id,
                .build_out_param_channel = build_out_param_channel,
                .back_channel = in_back_channel},
            response_data);
        if (ret != rpc::error::OK())
        {
            RPC_ERROR("failed add_ref addref_send");
            CO_RETURN ret;
        }

        out_back_channel.swap(response_data.back_channel);
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
            CO_RETURN response_data.err_code;
        }

        RPC_DEBUG("streaming_transport::outbound_add_ref complete zone={}", get_zone_id().get_subnet());
        CO_RETURN rpc::error::OK();
    }

    CORO_TASK(int)
    streaming_transport::outbound_release(uint64_t protocol_version,
        rpc::remote_object remote_object_id,
        rpc::caller_zone caller_zone_id,
        rpc::release_options options,
        const std::vector<rpc::back_channel_entry>& in_back_channel,
        std::vector<rpc::back_channel_entry>& out_back_channel)
    {
        RPC_DEBUG("streaming_transport::outbound_release zone={}", get_zone_id().get_subnet());

        if (get_status() != rpc::transport_status::CONNECTED && get_status() != rpc::transport_status::DISCONNECTING)
        {
            RPC_ERROR("failed streaming_transport::outbound_release - not connected, status = {}",
                static_cast<int>(get_status()));
            CO_RETURN rpc::error::TRANSPORT_ERROR();
        }

        send_payload(protocol_version,
            message_direction::one_way,
            release_send{.destination_zone_id = remote_object_id,
                .caller_zone_id = caller_zone_id,
                .options = options,
                .back_channel = in_back_channel},
            0);

        auto count = get_destination_count();
        RPC_ASSERT(count >= 0);
        if (count <= 0 && get_status() == rpc::transport_status::CONNECTED)
        {
            RPC_DEBUG("destination_count reached 0, triggering graceful shutdown");
            set_status(rpc::transport_status::DISCONNECTING);
        }

        CO_RETURN rpc::error::OK();
    }

    CORO_TASK(void)
    streaming_transport::outbound_object_released(uint64_t protocol_version,
        rpc::remote_object remote_object_id,
        rpc::caller_zone caller_zone_id,
        const std::vector<rpc::back_channel_entry>& in_back_channel)
    {
        RPC_DEBUG("streaming_transport::outbound_object_released zone={}", get_zone_id().get_subnet());

        if (get_status() == rpc::transport_status::DISCONNECTED)
        {
            RPC_ERROR("failed streaming_transport::outbound_object_released - transport disconnected");
            CO_RETURN;
        }

        send_payload(protocol_version,
            message_direction::one_way,
            object_released_send{.encoding = encoding::yas_binary,
                .destination_zone_id = remote_object_id,
                .caller_zone_id = caller_zone_id,
                .back_channel = in_back_channel},
            0);

        RPC_DEBUG("streaming_transport::outbound_object_released complete zone={}", get_zone_id().get_subnet());
    }

    CORO_TASK(void)
    streaming_transport::outbound_transport_down(uint64_t protocol_version,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        const std::vector<rpc::back_channel_entry>& in_back_channel)
    {
        RPC_DEBUG("streaming_transport::outbound_transport_down zone={}", get_zone_id().get_subnet());

        if (get_status() == rpc::transport_status::DISCONNECTED)
        {
            RPC_ERROR("failed streaming_transport::outbound_transport_down - transport disconnected");
            CO_RETURN;
        }

        send_payload(protocol_version,
            message_direction::one_way,
            transport_down_send{.encoding = encoding::yas_binary,
                .destination_zone_id = destination_zone_id,
                .caller_zone_id = caller_zone_id,
                .back_channel = in_back_channel},
            0);

        RPC_DEBUG("streaming_transport::outbound_transport_down complete zone={}", get_zone_id().get_subnet());
    }

    CORO_TASK(void) streaming_transport::pump_send_and_receive()
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
        auto tracker = std::shared_ptr<activity_tracker>(
            new activity_tracker{.transport = std::static_pointer_cast<streaming_transport>(self), .svc = svc});
        svc->spawn(receive_consumer_loop(tracker));
        svc->spawn(send_producer_loop(tracker));

        RPC_DEBUG("pump_send_and_receive: scheduled tasks for zone {}", get_zone_id().get_subnet());
    }

    CORO_TASK(void)
    streaming_transport::receive_consumer_loop(std::shared_ptr<activity_tracker> tracker)
    {
        auto self = shared_from_this();
        auto svc = get_service();
        RPC_ASSERT(svc);

        static auto envelope_prefix_saved_size = rpc::yas_binary_saved_size(envelope_prefix());

        std::vector<char> prefix_buf(envelope_prefix_saved_size);
        std::vector<char> payload_buf;

        bool receiving_prefix = true;
        std::span<char> remaining;

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
                            RPC_DEBUG("receive_consumer_loop: responder sending close_connection_ack for zone {}",
                                get_zone_id().get_subnet());
                            send_payload(rpc::get_version(), message_direction::one_way, close_connection_ack{}, 0);
                            CO_AWAIT flush_send_queue();
                            RPC_DEBUG("receive_consumer_loop: responder transition to DISCONNECTED zone {}",
                                get_zone_id().get_subnet());
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
                auto str_err = rpc::from_yas_binary(rpc::span(prefix_buf), prefix);
                if (!str_err.empty())
                {
                    RPC_ERROR("Deserialization FAILED: {}", str_err);
                    break;
                }
                assert(prefix.direction);
                receiving_prefix = false;
                payload_buf.assign(prefix.payload_size, '\0');
                remaining = payload_buf;
                continue;
            }

            // Full payload received — dispatch
            {
                envelope_payload payload;
                auto str_err = rpc::from_yas_binary(rpc::span(payload_buf), payload);
                if (!str_err.empty())
                {
                    RPC_ERROR("failed bad payload format");
                    break;
                }

                if (payload.payload_fingerprint == rpc::id<init_client_channel_send>::get(prefix.version))
                {
                    RPC_DEBUG("pump: received init_client_channel_send seq={}", prefix.sequence_number);
                    get_service()->spawn(create_stub(tracker, std::move(prefix), std::move(payload)));
                }
                else if (payload.payload_fingerprint == rpc::id<call_send>::get(prefix.version))
                {
                    RPC_DEBUG("pump: received call_send seq={}", prefix.sequence_number);
                    get_service()->spawn(stub_handle_send(tracker, std::move(prefix), std::move(payload)));
                }
                else if (payload.payload_fingerprint == rpc::id<post_send>::get(prefix.version))
                {
                    RPC_DEBUG("pump: received post_send seq={}", prefix.sequence_number);
                    get_service()->spawn(stub_handle_post(tracker, std::move(prefix), std::move(payload)));
                }
                else if (payload.payload_fingerprint == rpc::id<try_cast_send>::get(prefix.version))
                {
                    RPC_DEBUG("pump: received try_cast_send seq={}", prefix.sequence_number);
                    get_service()->spawn(stub_handle_try_cast(tracker, std::move(prefix), std::move(payload)));
                }
                else if (payload.payload_fingerprint == rpc::id<addref_send>::get(prefix.version))
                {
                    RPC_DEBUG("pump: received addref_send seq={}", prefix.sequence_number);
                    get_service()->spawn(stub_handle_add_ref(tracker, std::move(prefix), std::move(payload)));
                }
                else if (payload.payload_fingerprint == rpc::id<release_send>::get(prefix.version))
                {
                    RPC_DEBUG("pump: received release_send seq={}", prefix.sequence_number);
                    get_service()->spawn(stub_handle_release(tracker, std::move(prefix), std::move(payload)));
                }
                else if (payload.payload_fingerprint == rpc::id<object_released_send>::get(prefix.version))
                {
                    RPC_DEBUG("pump: received object_released_send seq={}", prefix.sequence_number);
                    get_service()->spawn(stub_handle_object_released(tracker, std::move(prefix), std::move(payload)));
                }
                else if (payload.payload_fingerprint == rpc::id<transport_down_send>::get(prefix.version))
                {
                    RPC_DEBUG("pump: received transport_down_send seq={}", prefix.sequence_number);
                    get_service()->spawn(stub_handle_transport_down(tracker, std::move(prefix), std::move(payload)));
                }
                else if (payload.payload_fingerprint == rpc::id<init_client_initial_channel_response>::get(prefix.version))
                {
                    init_client_initial_channel_response early_response;
                    auto str_err2 = rpc::from_yas_binary(rpc::span(payload.payload), early_response);
                    if (str_err2.empty())
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
                }
                else if (payload.payload_fingerprint == rpc::id<close_connection_send>::get(prefix.version))
                {
                    RPC_DEBUG("pump: received close_connection_send seq={} zone={}",
                        prefix.sequence_number,
                        get_zone_id().get_subnet());
                    set_status(rpc::transport_status::DISCONNECTING);
                    peer_requested_disconnection_ = true;
                }
                else if (payload.payload_fingerprint == rpc::id<close_connection_ack>::get(prefix.version))
                {
                    RPC_DEBUG(
                        "pump: received close_connection_ack — shutdown confirmed zone={}", get_zone_id().get_subnet());
                    set_status(rpc::transport_status::DISCONNECTED);
                    stop_loop = true;
                }
                else
                {
                    // Find the pending result listener and wake it
                    result_listener* result = nullptr;
                    {
                        std::scoped_lock lock(pending_transmits_mtx_);
                        auto it = pending_transmits_.find(prefix.sequence_number);
                        RPC_DEBUG("pending_transmits_ zone: {} sequence_number: {} id: {}",
                            get_zone_id().get_subnet(),
                            prefix.sequence_number,
                            payload.payload_fingerprint);

                        if (it != pending_transmits_.end())
                        {
                            result = it->second;
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
                        result->prefix = std::move(prefix);
                        result->payload = std::move(payload);

                        RPC_DEBUG("pump receive prefix.sequence_number {}\n prefix = {}\n payload = {}",
                            get_zone_id().get_subnet(),
                            rpc::to_yas_json<std::string>(result->prefix),
                            rpc::to_yas_json<std::string>(result->payload));

                        result->event.set();
                    }
                }

                receiving_prefix = true;
                remaining = std::span<char>{};
            }
        }

        RPC_DEBUG("receive_consumer_loop exiting for zone {}", get_zone_id().get_subnet());
        CO_RETURN;
    }

    CORO_TASK(void) streaming_transport::flush_send_queue()
    {
        while (true)
        {
            std::vector<uint8_t> item;
            {
                std::scoped_lock g(send_queue_mtx_);
                if (send_queue_.empty())
                    break;
                item = std::move(send_queue_.front());
                send_queue_.pop();
            }
            CO_AWAIT stream_->send(std::span<const char>{(const char*)item.data(), item.size()});
        }
    }

    CORO_TASK(void) streaming_transport::send_producer_loop(std::shared_ptr<activity_tracker> tracker)
    {
        auto self = shared_from_this();
        auto svc = get_service();
        RPC_ASSERT(svc);

        RPC_DEBUG("send_producer_loop started for zone {}", get_zone_id().get_subnet());

        while (get_status() == rpc::transport_status::CONNECTED)
        {
            std::vector<uint8_t> item;
            bool had_item = false;
            {
                std::scoped_lock g(send_queue_mtx_);
                if (!send_queue_.empty())
                {
                    item = std::move(send_queue_.front());
                    send_queue_.pop();
                    had_item = true;
                }
            }

            if (had_item)
            {
                CO_AWAIT stream_->send(std::span<const char>{(const char*)item.data(), item.size()});
            }
            else
            {
                CO_AWAIT svc->schedule();
            }
        }

        // Flush any messages queued before DISCONNECTING was set
        CO_AWAIT flush_send_queue();

        // Send cleanup notifications — adds stub-release and transport-down messages
        CO_AWAIT notify_all_destinations_of_disconnect();

        if (!peer_requested_disconnection_)
        {
            RPC_DEBUG("send_producer_loop: sending close_connection_send for zone {}", get_zone_id().get_subnet());
            send_payload(rpc::get_version(), message_direction::one_way, close_connection_send{}, 0);
        }

        CO_AWAIT flush_send_queue();

        RPC_DEBUG("send_producer_loop: cleanup done, signalling receive loop for zone {}", get_zone_id().get_subnet());
        send_cleanup_done_.store(true, std::memory_order_release);

        RPC_DEBUG("send_producer_loop completed for zone {}", get_zone_id().get_subnet());
        CO_RETURN;
    }

    CORO_TASK(void)
    streaming_transport::cleanup(std::shared_ptr<streaming_transport> transport, std::shared_ptr<rpc::service> svc)
    {
        RPC_DEBUG("Both loops completed, finalising transport for zone {}", transport->get_zone_id().get_subnet());
        {
            std::scoped_lock lock(transport->pending_transmits_mtx_);
            for (auto it : transport->pending_transmits_)
            {
                it.second->error_code = rpc::error::CALL_CANCELLED();
                it.second->event.set();
            }
        }
        transport->keep_alive_.reset();
        co_return;
    }

    CORO_TASK(void)
    streaming_transport::stub_handle_send(std::shared_ptr<activity_tracker>, envelope_prefix prefix, envelope_payload payload)
    {
        RPC_DEBUG("stub_handle_send");

        assert(prefix.direction == message_direction::send || prefix.direction == message_direction::one_way);

        if (get_status() != rpc::transport_status::CONNECTED)
        {
            CO_RETURN;
        }

        call_send request;
        auto str_err = rpc::from_yas_binary(rpc::span(payload.payload), request);
        if (!str_err.empty())
        {
            RPC_ERROR("failed from_yas_binary call_send");
            set_status(rpc::transport_status::DISCONNECTING);
            CO_RETURN;
        }

        std::vector<char> out_buf;
        std::vector<rpc::back_channel_entry> out_back_channel;
        auto ret = CO_AWAIT inbound_send(prefix.version,
            request.encoding,
            request.tag,
            request.caller_zone_id,
            request.destination_zone_id,
            request.interface_id,
            request.method_id,
            request.payload,
            out_buf,
            request.back_channel,
            out_back_channel);

        if (rpc::error::is_error(ret))
        {
            RPC_DEBUG("inbound_send error {}", ret);
        }

        if (prefix.direction == message_direction::one_way)
            CO_RETURN;

        send_payload(prefix.version,
            message_direction::receive,
            call_receive{.payload = std::move(out_buf), .back_channel = std::move(out_back_channel), .err_code = ret},
            prefix.sequence_number);
        RPC_DEBUG("stub_handle_send complete");
        CO_RETURN;
    }

    CORO_TASK(void)
    streaming_transport::stub_handle_post(std::shared_ptr<activity_tracker>, envelope_prefix prefix, envelope_payload payload)
    {
        RPC_DEBUG("stub_handle_post");

        if (get_status() != rpc::transport_status::CONNECTED)
        {
            CO_RETURN;
        }

        post_send request;
        auto str_err = rpc::from_yas_binary(rpc::span(payload.payload), request);
        if (!str_err.empty())
        {
            RPC_ERROR("failed post_send from_yas_binary");
            set_status(rpc::transport_status::DISCONNECTING);
            CO_RETURN;
        }

        CO_AWAIT inbound_post(prefix.version,
            request.encoding,
            request.tag,
            request.caller_zone_id,
            request.destination_zone_id,
            request.interface_id,
            request.method_id,
            request.payload,
            request.back_channel);

        RPC_DEBUG("stub_handle_post complete");
        CO_RETURN;
    }

    CORO_TASK(void)
    streaming_transport::stub_handle_try_cast(
        std::shared_ptr<activity_tracker>, envelope_prefix prefix, envelope_payload payload)
    {
        RPC_DEBUG("stub_handle_try_cast");

        if (get_status() != rpc::transport_status::CONNECTED)
        {
            CO_RETURN;
        }

        try_cast_send request;
        auto str_err = rpc::from_yas_binary(rpc::span(payload.payload), request);
        if (!str_err.empty())
        {
            RPC_ERROR("failed try_cast_send from_yas_binary");
            set_status(rpc::transport_status::DISCONNECTING);
            CO_RETURN;
        }

        std::vector<rpc::back_channel_entry> out_back_channel;
        auto ret = CO_AWAIT inbound_try_cast(prefix.version,
            request.caller_zone_id,
            request.destination_zone_id,
            request.interface_id,
            request.back_channel,
            out_back_channel);

        if (rpc::error::is_error(ret))
        {
            RPC_DEBUG("inbound_try_cast error {}", ret);
        }

        send_payload(prefix.version,
            message_direction::receive,
            try_cast_receive{.back_channel = std::move(out_back_channel), .err_code = ret},
            prefix.sequence_number);
        RPC_DEBUG("stub_handle_try_cast complete");
        CO_RETURN;
    }

    CORO_TASK(void)
    streaming_transport::stub_handle_add_ref(
        std::shared_ptr<activity_tracker>, envelope_prefix prefix, envelope_payload payload)
    {
        RPC_DEBUG("stub_handle_add_ref");

        if (get_status() != rpc::transport_status::CONNECTED)
        {
            CO_RETURN;
        }

        addref_send request;
        auto str_err = rpc::from_yas_binary(rpc::span(payload.payload), request);
        if (!str_err.empty())
        {
            RPC_ERROR("failed addref_send from_yas_binary");
            set_status(rpc::transport_status::DISCONNECTING);
            CO_RETURN;
        }

        std::vector<rpc::back_channel_entry> out_back_channel;
        auto ret = CO_AWAIT inbound_add_ref(prefix.version,
            request.destination_zone_id,
            request.caller_zone_id,
            request.requesting_zone_id,
            request.build_out_param_channel,
            request.back_channel,
            out_back_channel);

        if (rpc::error::is_error(ret))
        {
            RPC_DEBUG("inbound_add_ref error {}", ret);
        }

        send_payload(prefix.version,
            message_direction::receive,
            addref_receive{.back_channel = std::move(out_back_channel), .err_code = ret},
            prefix.sequence_number);
        RPC_DEBUG("stub_handle_add_ref complete");
        CO_RETURN;
    }

    CORO_TASK(void)
    streaming_transport::stub_handle_release(
        std::shared_ptr<activity_tracker>, envelope_prefix prefix, envelope_payload payload)
    {
        RPC_DEBUG("stub_handle_release");

        release_send request;
        auto str_err = rpc::from_yas_binary(rpc::span(payload.payload), request);
        if (!str_err.empty())
        {
            RPC_ERROR("failed release_send from_yas_binary");
            set_status(rpc::transport_status::DISCONNECTING);
            CO_RETURN;
        }

        std::vector<rpc::back_channel_entry> out_back_channel;
        auto ret = CO_AWAIT inbound_release(prefix.version,
            request.destination_zone_id,
            request.caller_zone_id,
            request.options,
            request.back_channel,
            out_back_channel);

        if (rpc::error::is_error(ret))
        {
            RPC_DEBUG("inbound_release error {}", ret);
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
    streaming_transport::stub_handle_object_released(
        std::shared_ptr<activity_tracker>, envelope_prefix prefix, envelope_payload payload)
    {
        RPC_DEBUG("stub_handle_object_released");

        object_released_send request;
        auto str_err = rpc::from_yas_binary(rpc::span(payload.payload), request);
        if (!str_err.empty())
        {
            RPC_ERROR("failed object_released_send from_yas_binary");
            set_status(rpc::transport_status::DISCONNECTING);
            CO_RETURN;
        }

        CO_AWAIT inbound_object_released(
            prefix.version, request.destination_zone_id, request.caller_zone_id, request.back_channel);

        RPC_DEBUG("stub_handle_object_released complete");
        CO_RETURN;
    }

    CORO_TASK(void)
    streaming_transport::stub_handle_transport_down(
        std::shared_ptr<activity_tracker>, envelope_prefix prefix, envelope_payload payload)
    {
        RPC_DEBUG("stub_handle_transport_down");

        transport_down_send request;
        auto str_err = rpc::from_yas_binary(rpc::span(payload.payload), request);
        if (!str_err.empty())
        {
            RPC_ERROR("failed transport_down_send from_yas_binary");
            set_status(rpc::transport_status::DISCONNECTING);
            CO_RETURN;
        }

        CO_AWAIT inbound_transport_down(
            prefix.version, request.destination_zone_id, request.caller_zone_id, request.back_channel);

        RPC_DEBUG("stub_handle_transport_down complete");
        CO_RETURN;
    }

    CORO_TASK(void)
    streaming_transport::create_stub(std::shared_ptr<activity_tracker>, envelope_prefix prefix, envelope_payload payload)
    {
        RPC_DEBUG("create_stub zone: {}", get_zone_id().get_subnet());

        init_client_channel_send request;
        auto err = rpc::from_yas_binary(rpc::span(payload.payload), request);
        if (!err.empty())
        {
            RPC_ERROR("failed create_stub init_client_channel_send deserialization");
            CO_RETURN;
        }

        rpc::connection_settings input_descr;
        input_descr.inbound_interface_id = request.inbound_interface_id;
        input_descr.outbound_interface_id = request.outbound_interface_id;
        input_descr.input_zone_id = request.inbound_remote_object;
        rpc::interface_descriptor output_interface;

        set_adjacent_zone_id(request.adjacent_zone_id);

        // Immediately inform the peer of our zone_id before invoking connection_handler_
        send_payload(
            prefix.version, message_direction::one_way, init_client_initial_channel_response{.zone_id = get_zone_id()}, 0);

        int ret = CO_AWAIT connection_handler_(input_descr, output_interface, get_service(), keep_alive_.get_nullable());
        connection_handler_ = nullptr;
        if (ret != rpc::error::OK())
        {
            RPC_ERROR("failed to connect to zone {}", ret);
            CO_RETURN;
        }

        send_payload(prefix.version,
            message_direction::receive,
            init_client_channel_response{.err_code = rpc::error::OK(),
                .outbound_remote_object = output_interface.destination_zone_id,
                .caller_zone_id = input_descr.input_zone_id.as_zone()},
            prefix.sequence_number);

        CO_RETURN;
    }
}
