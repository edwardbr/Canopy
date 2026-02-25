/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <transports/spsc/transport.h>

namespace rpc::spsc
{
    spsc_transport::spsc_transport(std::string name,
        std::shared_ptr<rpc::service> service,
        rpc::zone adjacent_zone_id,
        queue_type* send_spsc_queue,
        queue_type* receive_spsc_queue,
        connection_handler handler)
        : rpc::transport(name, service, adjacent_zone_id)
        , send_spsc_queue_(send_spsc_queue)
        , receive_spsc_queue_(receive_spsc_queue)
        , connection_handler_(handler)
    {
    }

    std::shared_ptr<spsc_transport> spsc_transport::create(std::string name,
        std::shared_ptr<rpc::service> service,
        rpc::zone adjacent_zone_id,
        queue_type* send_spsc_queue,
        queue_type* receive_spsc_queue,
        connection_handler handler)
    {
        auto transport = std::shared_ptr<spsc_transport>(
            new spsc_transport(name, service, adjacent_zone_id, send_spsc_queue, receive_spsc_queue, handler));

        // Set up the keep alive using member_ptr assignment
        transport->keep_alive_ = transport;

        // Set the transport status to connected
        transport->set_status(rpc::transport_status::CONNECTED);
        return transport;
    }

    // Connection handshake
    CORO_TASK(int)
    spsc_transport::inner_connect(connection_settings& input_descr, rpc::interface_descriptor& output_descr)
    {
        RPC_DEBUG("spsc_transport::connect zone={}", get_zone_id().get_val());

        auto service = get_service();
        assert(connection_handler_ || !connection_handler_); // Can be null for client side

        // Schedule onto the scheduler
        // CO_AWAIT service->get_scheduler()->schedule();

        pump_send_and_receive();

        // If this is a client-side connect, send init message to server
        if (!connection_handler_)
        {
            // Client side: register the proxy connection
            init_client_channel_response init_receive;
            int ret = CO_AWAIT call_peer(rpc::get_version(),
                init_client_channel_send{.caller_zone_id = get_zone_id().as_caller(),
                    .caller_object_id = input_descr.get_object_id(),
                    .caller_interface_id = input_descr.caller_interface_id,
                    .destination_zone_id = get_adjacent_zone_id().as_destination(),
                    .destination_interface_id = input_descr.destination_interface_id,
                    .adjacent_zone_id = get_zone_id()},
                init_receive);
            if (ret != rpc::error::OK())
            {
                RPC_ERROR("spsc_transport::connect call_peer failed {}", rpc::error::to_string(ret));
                CO_RETURN ret;
            }

            if (init_receive.err_code != rpc::error::OK())
            {
                RPC_ERROR("init_client_channel_send failed");
                CO_RETURN init_receive.err_code;
            }

            // Update the adjacent zone ID based on the response
            output_descr = {init_receive.destination_object_id, get_adjacent_zone_id().as_destination()};
        }

        CO_RETURN rpc::error::OK();
    }

    CORO_TASK(int) spsc_transport::inner_accept()
    {
        pump_send_and_receive();
        CO_RETURN rpc::error::OK();
    }

    // Client-side i_marshaller implementations
    CORO_TASK(int)
    spsc_transport::outbound_send(uint64_t protocol_version,
        rpc::encoding encoding,
        uint64_t tag,
        rpc::caller_zone caller_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::interface_ordinal interface_id,
        rpc::method method_id,
        const rpc::span& in_data,
        std::vector<char>& out_buf_,
        const std::vector<rpc::back_channel_entry>& in_back_channel,
        std::vector<rpc::back_channel_entry>& out_back_channel)
    {

        RPC_DEBUG("spsc_transport::outbound_send zone={}", get_zone_id().get_val());

        // Peer-to-peer: always transmit via SPSC queue to peer
        // The peer transport will call inbound_send for routing
        call_receive response;
        int ret = CO_AWAIT call_peer(protocol_version,
            call_send{.encoding = encoding,
                .tag = tag,
                .caller_zone_id = caller_zone_id,
                .destination_zone_id = destination_zone_id,
                .interface_id = interface_id,
                .method_id = method_id,
                .payload = std::vector<char>((const char*)in_data.begin, (const char*)in_data.end),
                .back_channel = in_back_channel},
            response);

        if (rpc::error::is_error(ret))
        {
            RPC_DEBUG("failed spsc_transport::outbound_send call_send");
            CO_RETURN ret;
        }

        out_buf_.swap(response.payload);
        out_back_channel.swap(response.back_channel);

        RPC_DEBUG("spsc_transport::outbound_send complete zone={}", get_zone_id().get_val());

        CO_RETURN response.err_code;
    }

    CORO_TASK(void)
    spsc_transport::outbound_post(uint64_t protocol_version,
        rpc::encoding encoding,
        uint64_t tag,
        rpc::caller_zone caller_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::interface_ordinal interface_id,
        rpc::method method_id,
        const rpc::span& in_data,
        const std::vector<rpc::back_channel_entry>& in_back_channel)
    {
        RPC_DEBUG("spsc_transport::outbound_post zone={}", get_zone_id().get_val());

        // Check transport status
        if (get_status() != rpc::transport_status::CONNECTED)
        {
            RPC_ERROR("spsc_transport::outbound_post: transport not connected");
            CO_RETURN;
        }

        // Peer-to-peer: always transmit via SPSC queue to peer
        // The peer transport will call inbound_post for routing
        // Fire-and-forget
        send_payload(protocol_version,
            spsc::message_direction::one_way,
            spsc::post_send{.encoding = encoding,
                .tag = tag,
                .caller_zone_id = caller_zone_id,
                .destination_zone_id = destination_zone_id,
                .interface_id = interface_id,
                .method_id = method_id,
                .payload = std::vector<char>((const char*)in_data.begin, (const char*)in_data.end),
                .back_channel = in_back_channel},
            0); // sequence number 0 for one-way

        CO_RETURN;
    }

    CORO_TASK(int)
    spsc_transport::outbound_try_cast(uint64_t protocol_version,
        rpc::caller_zone caller_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::interface_ordinal interface_id,
        const std::vector<rpc::back_channel_entry>& in_back_channel,
        std::vector<rpc::back_channel_entry>& out_back_channel)
    {
        RPC_DEBUG("spsc_transport::outbound_try_cast zone={}", get_zone_id().get_val());

        // Peer-to-peer: always transmit via SPSC queue to peer
        // The peer transport will call inbound_try_cast for routing
        try_cast_receive response_data;
        int ret = CO_AWAIT call_peer(protocol_version,
            try_cast_send{.caller_zone_id = caller_zone_id,
                .destination_zone_id = destination_zone_id,
                .interface_id = interface_id,
                .back_channel = in_back_channel},
            response_data);
        if (ret != rpc::error::OK())
        {
            RPC_ERROR("failed try_cast call_peer");
            CO_RETURN ret;
        }

        RPC_DEBUG("spsc_transport::outbound_try_cast complete zone={}", get_zone_id().get_val());

        out_back_channel.swap(response_data.back_channel);
        CO_RETURN response_data.err_code;
    }

    CORO_TASK(int)
    spsc_transport::outbound_add_ref(uint64_t protocol_version,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::known_direction_zone known_direction_zone_id,
        rpc::add_ref_options build_out_param_channel,
        const std::vector<rpc::back_channel_entry>& in_back_channel,
        std::vector<rpc::back_channel_entry>& out_back_channel)
    {
        RPC_DEBUG("spsc_transport::outbound_add_ref zone={}", get_zone_id().get_val());

        // Peer-to-peer: always transmit via SPSC queue to peer
        // The peer transport will call inbound_add_ref for routing
        addref_receive response_data;
        int ret = CO_AWAIT call_peer(protocol_version,
            addref_send{.destination_zone_id = destination_zone_id,
                .caller_zone_id = caller_zone_id,
                .known_direction_zone_id = known_direction_zone_id,
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
            RPC_ERROR("failed addref_receive.err_code failed");
#ifdef CANOPY_USE_TELEMETRY
            if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
            {
                auto error_message = std::string("add_ref failed ") + std::to_string(response_data.err_code);
                telemetry_service->message(rpc::i_telemetry_service::err, error_message.c_str());
            }
#endif
            RPC_ASSERT(false);
            CO_RETURN response_data.err_code;
        }

        RPC_DEBUG("spsc_transport::outbound_add_ref complete zone={}", get_zone_id().get_val());

        CO_RETURN rpc::error::OK();
    }

    CORO_TASK(int)
    spsc_transport::outbound_release(uint64_t protocol_version,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::release_options options,
        const std::vector<rpc::back_channel_entry>& in_back_channel,
        std::vector<rpc::back_channel_entry>& out_back_channel)
    {
        RPC_DEBUG("spsc_transport::outbound_release zone={}", get_zone_id().get_val());

        // Check transport status
        if (get_status() != rpc::transport_status::CONNECTED && get_status() != rpc::transport_status::DISCONNECTING)
        {
            RPC_ERROR(
                "failed spsc_transport::outbound_release - not connected, status = {}", static_cast<int>(get_status()));
            CO_RETURN rpc::error::TRANSPORT_ERROR();
        }

        send_payload(protocol_version,
            message_direction::one_way,
            release_send{.destination_zone_id = destination_zone_id,
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
    spsc_transport::outbound_object_released(uint64_t protocol_version,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        const std::vector<rpc::back_channel_entry>& in_back_channel)
    {
        RPC_DEBUG("spsc_transport::outbound_object_released zone={}", get_zone_id().get_val());

        // Allow during DISCONNECTING (cleanup messages must get through); block only when fully disconnected
        if (get_status() == rpc::transport_status::DISCONNECTED)
        {
            RPC_ERROR("failed spsc_transport::outbound_object_released - transport disconnected, status = {}",
                static_cast<int>(get_status()));
            CO_RETURN;
        }

        // Send the object_released message using the internal send_payload method (post-like behavior)
        send_payload(protocol_version,
            message_direction::one_way, // Use one_way for fire-and-forget
            object_released_send{.encoding = encoding::yas_binary,
                .destination_zone_id = destination_zone_id,
                .caller_zone_id = caller_zone_id,
                .back_channel = in_back_channel},
            0); // sequence number 0 for one-way messages

        RPC_DEBUG("spsc_transport::outbound_object_released complete zone={}", get_zone_id().get_val());
    }

    CORO_TASK(void)
    spsc_transport::outbound_transport_down(uint64_t protocol_version,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        const std::vector<rpc::back_channel_entry>& in_back_channel)
    {
        RPC_DEBUG("spsc_transport::outbound_transport_down zone={}", get_zone_id().get_val());

        // Allow during DISCONNECTING (cleanup messages must get through); block only when fully disconnected
        if (get_status() == rpc::transport_status::DISCONNECTED)
        {
            RPC_ERROR("failed spsc_transport::outbound_transport_down - transport disconnected, status = {}",
                static_cast<int>(get_status()));
            CO_RETURN;
        }

        // Send the transport_down message using the internal send_payload method (post-like behavior)
        send_payload(protocol_version,
            message_direction::one_way, // Use one_way for fire-and-forget
            transport_down_send{.encoding = encoding::yas_binary,
                .destination_zone_id = destination_zone_id,
                .caller_zone_id = caller_zone_id,
                .back_channel = in_back_channel},
            0); // sequence number 0 for one-way messages

        RPC_DEBUG("spsc_transport::outbound_transport_down complete zone={}", get_zone_id().get_val());
    }

    // Producer/consumer tasks for message pumping
    void spsc_transport::pump_send_and_receive()
    {
        auto self = shared_from_this();
        RPC_DEBUG("pump_send_and_receive zone={}", get_zone_id().get_val());

        // Guard against multiple calls
        bool expected = false;
        if (!pumps_started_.compare_exchange_strong(expected, true))
        {
            RPC_ERROR("pump_send_and_receive called MULTIPLE TIMES on zone {} - BUG!", get_zone_id().get_val());
            return;
        }

        // Schedule both producer and consumer tasks
        auto svc = get_service();
        auto scheduler = svc->get_scheduler();
        auto tracker = std::shared_ptr<activity_tracker>(
            new activity_tracker{.transport = std::static_pointer_cast<spsc_transport>(self), .svc = svc});
        scheduler->spawn(receive_consumer_loop(tracker));
        scheduler->spawn(send_producer_loop(tracker));

        RPC_DEBUG("pump_send_and_receive: scheduled tasks for zone {}", get_zone_id().get_val());
    }

    // Receive consumer task
    CORO_TASK(void)
    spsc_transport::receive_consumer_loop(std::shared_ptr<activity_tracker> tracker)
    {
        auto self = shared_from_this(); // keep this alive until finished
        auto svc = get_service();       // keep the service alive until finished
        RPC_ASSERT(svc);

        static auto envelope_prefix_saved_size = rpc::yas_binary_saved_size(envelope_prefix());

        std::vector<uint8_t> prefix_buf(envelope_prefix_saved_size);
        std::vector<uint8_t> buf;

        bool receiving_prefix = true;
        std::span<uint8_t> receive_data;

        RPC_DEBUG("receive_consumer_loop started for zone {}", get_zone_id().get_val());

        // Prefix must persist across loop iterations while receiving payload chunks
        envelope_prefix prefix{};

        // Continue running while CONNECTED or DISCONNECTING (drain during shutdown).
        // Loop exits when DISCONNECTED (after close_connection_ack received or timeout).
        bool stop_loop = false;
        while (get_status() < rpc::transport_status::DISCONNECTED && !stop_loop)
        {

            // Receive prefix chunks
            if (receiving_prefix)
            {
                if (receive_data.empty())
                {
                    receive_data = {prefix_buf.begin(), prefix_buf.end()};
                }

                bool received_any = false;
                while (!receive_data.empty())
                {
                    message_blob blob;
                    if (!receive_spsc_queue_->pop(blob))
                    {
                        if (!received_any && get_status() == rpc::transport_status::DISCONNECTING)
                        {
                            // Queue is empty while disconnecting — handle based on role
                            if (peer_requested_disconnection_)
                            {
                                // Responder: wait until send loop finishes cleanup, then send ack
                                if (send_cleanup_done_.load(std::memory_order_acquire))
                                {
                                    RPC_DEBUG(
                                        "receive_consumer_loop: responder sending close_connection_ack for zone {}",
                                        get_zone_id().get_val());
                                    send_payload(rpc::get_version(), message_direction::one_way, close_connection_ack{}, 0);
                                    // Flush close_connection_ack directly — send loop has already exited
                                    std::span<uint8_t> ack_send_data;
                                    auto ack_status = push_message(ack_send_data);
                                    while (ack_status == send_queue_status::SEND_QUEUE_NOT_EMPTY)
                                        ack_status = push_message(ack_send_data);
                                    RPC_DEBUG("receive_consumer_loop: responder transition to DISCONNECTED zone {}",
                                        get_zone_id().get_val());
                                    set_status(rpc::transport_status::DISCONNECTED);
                                    stop_loop = true;
                                    break;
                                }
                                else
                                {
                                    // Send loop still running — check timeout before yielding
                                    auto elapsed = std::chrono::steady_clock::now() - disconnecting_since_;
                                    if (elapsed >= std::chrono::milliseconds(shutdown_timeout_ms_))
                                    {
                                        RPC_WARNING(
                                            "receive_consumer_loop: responder shutdown timeout for zone {}, forcing "
                                            "DISCONNECTED",
                                            get_zone_id().get_val());
                                        set_status(rpc::transport_status::DISCONNECTED);
                                        stop_loop = true;
                                        break;
                                    }
                                }
                            }
                            else
                            {
                                // Initiator: check shutdown timeout waiting for close_connection_ack
                                auto elapsed = std::chrono::steady_clock::now() - disconnecting_since_;
                                if (elapsed >= std::chrono::milliseconds(shutdown_timeout_ms_))
                                {
                                    RPC_WARNING(
                                        "receive_consumer_loop: shutdown timeout for zone {}, forcing DISCONNECTED",
                                        get_zone_id().get_val());
                                    set_status(rpc::transport_status::DISCONNECTED);
                                    stop_loop = true;
                                    break;
                                }
                            }
                        }

                        if (!received_any)
                        {
                            CO_AWAIT svc->get_scheduler()->schedule();
                        }
                        break;
                    }

                    received_any = true;
                    size_t copy_size = std::min(receive_data.size(), blob.size());
                    std::copy_n(blob.begin(), copy_size, receive_data.begin());

                    if (receive_data.size() <= blob.size())
                    {
                        receive_data = {receive_data.end(), receive_data.end()};
                    }
                    else
                    {
                        receive_data = receive_data.subspan(blob.size(), receive_data.size() - blob.size());
                    }
                }

                if (stop_loop)
                    break;

                if (receive_data.empty())
                {
                    auto str_err = rpc::from_yas_binary(rpc::span(prefix_buf), prefix);
                    if (!str_err.empty())
                    {
                        RPC_ERROR("Deserialization FAILED: {}", str_err);
                        break;
                    }
                    assert(prefix.direction);
                    receiving_prefix = false;
                }
                else
                {
                    continue;
                }
            }

            // Receive payload chunks
            if (!receiving_prefix)
            {
                if (receive_data.empty())
                {
                    buf = std::vector<uint8_t>(prefix.payload_size);
                    receive_data = {buf.begin(), buf.end()};
                }

                bool received_any = false;
                while (!receive_data.empty())
                {
                    message_blob blob;
                    if (!receive_spsc_queue_->pop(blob))
                    {
                        // if (get_status() == rpc::transport_status::DISCONNECTING)
                        // {
                        //     stop_loop = true;
                        //     break;
                        // }
                        if (!received_any)
                        {
                            // CO_AWAIT svc->get_scheduler()->yield_for(std::chrono::milliseconds(1));
                            CO_AWAIT svc->get_scheduler()->schedule();
                        }
                        break;
                    }

                    received_any = true;
                    size_t copy_size = std::min(receive_data.size(), blob.size());
                    std::copy_n(blob.begin(), copy_size, receive_data.begin());

                    if (receive_data.size() <= blob.size())
                    {
                        receive_data = {receive_data.end(), receive_data.end()};
                    }
                    else
                    {
                        receive_data = receive_data.subspan(blob.size(), receive_data.size() - blob.size());
                    }
                }

                if (stop_loop)
                    break;

                if (receive_data.empty())
                {
                    envelope_payload payload;
                    auto str_err = rpc::from_yas_binary(rpc::span(buf), payload);
                    if (!str_err.empty())
                    {
                        RPC_ERROR("failed bad payload format");
                        break;
                    }

                    // Handle different message types
                    if (payload.payload_fingerprint == rpc::id<init_client_channel_send>::get(prefix.version))
                    {
                        RPC_DEBUG("pump: received init_client_channel_send seq={}", prefix.sequence_number);
                        get_service()->get_scheduler()->spawn(create_stub(tracker, std::move(prefix), std::move(payload)));
                    }
                    else if (payload.payload_fingerprint == rpc::id<call_send>::get(prefix.version))
                    {
                        RPC_DEBUG("pump: received call_send seq={}", prefix.sequence_number);
                        get_service()->get_scheduler()->spawn(
                            stub_handle_send(tracker, std::move(prefix), std::move(payload)));
                    }
                    else if (payload.payload_fingerprint == rpc::id<post_send>::get(prefix.version))
                    {
                        RPC_DEBUG("pump: received post_send seq={}", prefix.sequence_number);
                        get_service()->get_scheduler()->spawn(
                            stub_handle_post(tracker, std::move(prefix), std::move(payload)));
                    }
                    else if (payload.payload_fingerprint == rpc::id<try_cast_send>::get(prefix.version))
                    {
                        RPC_DEBUG("pump: received try_cast_send seq={}", prefix.sequence_number);
                        get_service()->get_scheduler()->spawn(
                            stub_handle_try_cast(tracker, std::move(prefix), std::move(payload)));
                    }
                    else if (payload.payload_fingerprint == rpc::id<addref_send>::get(prefix.version))
                    {
                        RPC_DEBUG("pump: received addref_send seq={}", prefix.sequence_number);
                        get_service()->get_scheduler()->spawn(
                            stub_handle_add_ref(tracker, std::move(prefix), std::move(payload)));
                    }
                    else if (payload.payload_fingerprint == rpc::id<release_send>::get(prefix.version))
                    {
                        RPC_DEBUG("pump: received release_send seq={}", prefix.sequence_number);
                        get_service()->get_scheduler()->spawn(
                            stub_handle_release(tracker, std::move(prefix), std::move(payload)));
                    }
                    else if (payload.payload_fingerprint == rpc::id<object_released_send>::get(prefix.version))
                    {
                        RPC_DEBUG("pump: received object_released_send seq={}", prefix.sequence_number);
                        get_service()->get_scheduler()->spawn(
                            stub_handle_object_released(tracker, std::move(prefix), std::move(payload)));
                    }
                    else if (payload.payload_fingerprint == rpc::id<transport_down_send>::get(prefix.version))
                    {
                        RPC_DEBUG("pump: received transport_down_send seq={}", prefix.sequence_number);
                        get_service()->get_scheduler()->spawn(
                            stub_handle_transport_down(tracker, std::move(prefix), std::move(payload)));
                    }
                    else if (payload.payload_fingerprint == rpc::id<close_connection_send>::get(prefix.version))
                    {
                        RPC_DEBUG("pump: received close_connection_send seq={} zone={}",
                            prefix.sequence_number,
                            get_zone_id().get_val());
                        set_status(rpc::transport_status::DISCONNECTING);
                        peer_requested_disconnection_ = true;
                        // Continue loop to drain remaining messages; ack sent when queue empty + send done
                    }
                    else if (payload.payload_fingerprint == rpc::id<close_connection_ack>::get(prefix.version))
                    {
                        RPC_DEBUG("pump: received close_connection_ack — shutdown confirmed zone={}",
                            get_zone_id().get_val());
                        set_status(rpc::transport_status::DISCONNECTED);
                        stop_loop = true;
                    }
                    else
                    {
                        // now find the relevant event handler and set its values before triggering it
                        result_listener* result = nullptr;
                        {
                            std::scoped_lock lock(pending_transmits_mtx_);
                            auto it = pending_transmits_.find(prefix.sequence_number);
                            RPC_DEBUG("pending_transmits_ zone: {} sequence_number: {} id: {}",
                                get_zone_id().get_val(),
                                prefix.sequence_number,
                                payload.payload_fingerprint);

                            if (it != pending_transmits_.end())
                            {
                                result = it->second;
                                pending_transmits_.erase(it);
                            }
                            else
                            {
                                RPC_WARNING(
                                    "No pending transmit found for sequence_number: {}, ignoring message id: {}",
                                    prefix.sequence_number,
                                    payload.payload_fingerprint);
                            }
                        }

                        // Set event AFTER releasing the lock to avoid deadlock if the resumed
                        // coroutine immediately makes another call_peer which needs the lock
                        if (result)
                        {
                            result->prefix = std::move(prefix);
                            result->payload = std::move(payload);

                            RPC_DEBUG("pump_send_and_receive prefix.sequence_number {}\n prefix = {}\n payload = {}",
                                get_zone_id().get_val(),
                                rpc::to_yas_json<std::string>(result->prefix),
                                rpc::to_yas_json<std::string>(result->payload));

                            result->error_code = result->error_code;
                            result->event.set();
                        }
                    }

                    receiving_prefix = true;
                }
            }
        }

        RPC_DEBUG("receive_consumer_loop exiting for zone {}", get_zone_id().get_val());

        CO_RETURN;
    }

    // Send producer task
    spsc_transport::send_queue_status spsc_transport::push_message(std::span<uint8_t>& send_data)
    {
        if (send_data.empty())
        {
            // note be careful here using an unRAII'd lock here ensure that you unlo
            std::scoped_lock g(send_queue_mtx_);
            if (send_queue_.empty())
            {
                return send_queue_status::SEND_QUEUE_EMPTY;
            }

            auto& item = send_queue_.front();
            send_data = {item.begin(), item.end()};
        }

        message_blob send_blob;
        if (send_data.size() < send_blob.size())
        {
            std::copy(send_data.begin(), send_data.end(), send_blob.begin());
            send_data = {send_data.end(), send_data.end()};
        }
        else
        {
            std::copy_n(send_data.begin(), send_blob.size(), send_blob.begin());
            send_data = send_data.subspan(send_blob.size(), send_data.size() - send_blob.size());
        }

        if (send_spsc_queue_->push(send_blob))
        {
            if (send_data.empty())
            {
                std::scoped_lock g(send_queue_mtx_);
                send_queue_.pop();
            }

            return send_queue_status::SEND_QUEUE_NOT_EMPTY;
        }
        else
        {
            return send_queue_status::SPSC_QUEUE_FULL;
        }
    }

    // Send producer task
    CORO_TASK(void) spsc_transport::send_producer_loop(std::shared_ptr<activity_tracker> tracker)
    {
        auto self = shared_from_this(); // keep this alive until finished
        auto svc = get_service();       // keep the service alive until finished
        RPC_ASSERT(svc);

        RPC_DEBUG("send_producer_loop started for zone {}", get_zone_id().get_val());

        std::span<uint8_t> send_data;
        while (get_status() == rpc::transport_status::CONNECTED)
        {
            auto status = push_message(send_data);
            if (status == send_queue_status::SEND_QUEUE_EMPTY || status == send_queue_status::SPSC_QUEUE_FULL)
            {
                CO_AWAIT svc->get_scheduler()->schedule();
            }
        }

        // Flush any messages queued before DISCONNECTING was set
        auto status = push_message(send_data);
        while (status == send_queue_status::SEND_QUEUE_NOT_EMPTY)
            status = push_message(send_data);

        // Run cleanup notifications — this adds stub-release and transport-down messages to send_queue_
        CO_AWAIT notify_all_destinations_of_disconnect();

        if (!peer_requested_disconnection_)
        {
            // Initiator: append close_connection_send after all cleanup messages
            RPC_DEBUG("send_producer_loop: sending close_connection_send for zone {}", get_zone_id().get_val());
            send_payload(rpc::get_version(), message_direction::one_way, close_connection_send{}, 0);
        }

        // Flush cleanup messages (and close_connection_send if initiator)
        status = push_message(send_data);
        while (status == send_queue_status::SEND_QUEUE_NOT_EMPTY)
            status = push_message(send_data);

        if (status == send_queue_status::SPSC_QUEUE_FULL)
        {
            RPC_WARNING("send_producer_loop: SPSC queue full during shutdown flush for zone {}", get_zone_id().get_val());
        }

        // Always signal send_cleanup_done_ — handles both normal responder path and the
        // simultaneous-disconnect case where the initiator's receive loop enters the responder
        // path (peer_requested=true) after the send loop already took the initiator path.
        RPC_DEBUG("send_producer_loop: cleanup done, signalling receive loop for zone {}", get_zone_id().get_val());
        send_cleanup_done_.store(true, std::memory_order_release);

        RPC_DEBUG("send_producer_loop completed for zone {}", get_zone_id().get_val());
        CO_RETURN;
    }

    CORO_TASK(void)
    spsc_transport::cleanup(std::shared_ptr<spsc_transport> transport, std::shared_ptr<rpc::service> svc)
    {
        RPC_DEBUG("Both loops completed, finalising transport for zone {}", transport->get_zone_id().get_val());
        // Cancel any outstanding request-response calls (they will never receive a reply)
        {
            std::scoped_lock lock(transport->pending_transmits_mtx_);
            for (auto it : transport->pending_transmits_)
            {
                it.second->error_code = rpc::error::CALL_CANCELLED();
                it.second->event.set();
            }
        }
        // Release the self-referential keep_alive — allows the transport to be destroyed
        transport->keep_alive_.reset();

        co_return;
    }

    // Stub handlers (server-side message processing)
    CORO_TASK(void)
    spsc_transport::stub_handle_send(std::shared_ptr<activity_tracker>, envelope_prefix prefix, envelope_payload payload)
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
        // Call inbound_send for routing - transport will route to correct destination
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
        RPC_DEBUG("send request complete");
        CO_RETURN;
    }

    CORO_TASK(void)
    spsc_transport::stub_handle_post(std::shared_ptr<activity_tracker>, envelope_prefix prefix, envelope_payload payload)
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

        // Call inbound_post for routing - transport will route to correct destination
        CO_AWAIT inbound_post(prefix.version,
            request.encoding,
            request.tag,
            request.caller_zone_id,
            request.destination_zone_id,
            request.interface_id,
            request.method_id,
            request.payload,
            request.back_channel);

        // No response needed for post operations (fire-and-forget)
        RPC_DEBUG("stub_handle_post complete");
        CO_RETURN;
    }

    CORO_TASK(void)
    spsc_transport::stub_handle_try_cast(std::shared_ptr<activity_tracker>, envelope_prefix prefix, envelope_payload payload)
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
        // Call inbound_try_cast for routing - transport will route to correct destination
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
    spsc_transport::stub_handle_add_ref(std::shared_ptr<activity_tracker>, envelope_prefix prefix, envelope_payload payload)
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
        // Call inbound_add_ref for routing - transport will route to correct destination
        auto ret = CO_AWAIT inbound_add_ref(prefix.version,
            request.destination_zone_id,
            request.caller_zone_id,
            request.known_direction_zone_id,
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
        RPC_DEBUG("add_ref request complete");
        CO_RETURN;
    }

    CORO_TASK(void)
    spsc_transport::stub_handle_release(std::shared_ptr<activity_tracker>, envelope_prefix prefix, envelope_payload payload)
    {
        RPC_DEBUG("stub_handle_release");

        // we process this even if (get_status() != rpc::transport_status::CONNECTED)

        release_send request;
        auto str_err = rpc::from_yas_binary(rpc::span(payload.payload), request);
        if (!str_err.empty())
        {
            RPC_ERROR("failed release_send from_yas_binary");
            set_status(rpc::transport_status::DISCONNECTING);
            CO_RETURN;
        }

        std::vector<rpc::back_channel_entry> out_back_channel;
        // Call inbound_release for routing - transport will route to correct destination
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
        RPC_DEBUG("release request complete");
        CO_RETURN;
    }

    CORO_TASK(void)
    spsc_transport::stub_handle_object_released(
        std::shared_ptr<activity_tracker>, envelope_prefix prefix, envelope_payload payload)
    {
        RPC_DEBUG("stub_handle_object_released");

        // we have to process this even if (get_status() != rpc::transport_status::CONNECTED)

        object_released_send request;
        auto str_err = rpc::from_yas_binary(rpc::span(payload.payload), request);
        if (!str_err.empty())
        {
            RPC_ERROR("failed object_released_send from_yas_binary");
            set_status(rpc::transport_status::DISCONNECTING);
            CO_RETURN;
        }

        // Call inbound_object_released for routing - transport will route to correct destination
        CO_AWAIT inbound_object_released(
            prefix.version, request.destination_zone_id, request.caller_zone_id, request.back_channel);

        // No response needed for object_released (fire-and-forget)
        RPC_DEBUG("stub_handle_object_released complete");
        CO_RETURN;
    }

    CORO_TASK(void)
    spsc_transport::stub_handle_transport_down(
        std::shared_ptr<activity_tracker>, envelope_prefix prefix, envelope_payload payload)
    {
        RPC_DEBUG("stub_handle_transport_down");

        // we have to process this even if (get_status() != rpc::transport_status::CONNECTED)

        transport_down_send request;
        auto str_err = rpc::from_yas_binary(rpc::span(payload.payload), request);
        if (!str_err.empty())
        {
            RPC_ERROR("failed transport_down_send from_yas_binary");
            set_status(rpc::transport_status::DISCONNECTING);
            CO_RETURN;
        }

        // Call inbound_transport_down for routing - transport will route to correct destination
        CO_AWAIT inbound_transport_down(
            prefix.version, request.destination_zone_id, request.caller_zone_id, request.back_channel);

        // No response needed for transport_down (fire-and-forget)
        RPC_DEBUG("stub_handle_transport_down complete");
        CO_RETURN;
    }

    CORO_TASK(void)
    spsc_transport::create_stub(std::shared_ptr<activity_tracker>, envelope_prefix prefix, envelope_payload payload)
    {
        RPC_DEBUG("create_stub zone: {}", get_zone_id().get_val());

        init_client_channel_send request;
        auto err = rpc::from_yas_binary(rpc::span(payload.payload), request);
        if (!err.empty())
        {
            RPC_ERROR("failed create_stub init_client_channel_send deserialization");
            CO_RETURN;
        }
        rpc::connection_settings input_descr;
        input_descr.caller_interface_id = request.caller_interface_id;
        input_descr.destination_interface_id = request.destination_interface_id;
        input_descr.input_zone_id = request.caller_zone_id.as_destination().with_object(request.caller_object_id);
        rpc::interface_descriptor output_interface;

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
                .destination_zone_id = output_interface.destination_zone_id,
                .destination_object_id = output_interface.get_object_id(),
                .caller_zone_id = input_descr.input_zone_id.as_caller()},
            prefix.sequence_number);

        CO_RETURN;
    }
}
