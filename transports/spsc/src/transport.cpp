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
        // SPSC transport starts in CONNECTING state
        // Will transition to CONNECTED after successful connection
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
        transport->keep_alive_ = transport;
        return transport;
    }

    // Connection handshake
    CORO_TASK(int)
    spsc_transport::connect(rpc::interface_descriptor input_descr, rpc::interface_descriptor& output_descr)
    {
        RPC_DEBUG("spsc_transport::connect zone={}", get_zone_id().get_val());

        auto service = get_service();
        assert(connection_handler_ || !connection_handler_); // Can be null for client side

        // Schedule onto the scheduler
        CO_AWAIT service->get_scheduler()->schedule();

        // If this is a client-side connect, send init message to server
        if (!connection_handler_)
        {
            // Client side: register the proxy connection
            init_client_channel_response init_receive;
            int ret = CO_AWAIT call_peer(rpc::get_version(),
                init_client_channel_send{.caller_zone_id = get_zone_id().get_val(),
                    .caller_object_id = input_descr.object_id.get_val(),
                    .destination_zone_id = get_adjacent_zone_id().get_val()},
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
            rpc::object output_object_id = {init_receive.destination_object_id};
            output_descr = {output_object_id, get_adjacent_zone_id().as_destination()};
        }

        // Set transport to CONNECTED
        set_status(rpc::transport_status::CONNECTED);

        CO_RETURN rpc::error::OK();
    }

    // Client-side i_marshaller implementations
    CORO_TASK(int)
    spsc_transport::send(uint64_t protocol_version,
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
#ifdef CANOPY_USE_TELEMETRY
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_transport_outbound_send(
                get_zone_id(), get_adjacent_zone_id(), destination_zone_id, caller_zone_id, object_id, interface_id, method_id);
        }
#endif
        RPC_DEBUG("spsc_transport::send zone={}", get_zone_id().get_val());

        // Check transport status
        if (get_status() != rpc::transport_status::CONNECTED)
        {
            RPC_ERROR("failed spsc_transport::send - not connected, status = {}", static_cast<int>(get_status()));
            CO_RETURN rpc::error::TRANSPORT_ERROR();
        }

        // Peer-to-peer: always transmit via SPSC queue to peer
        // The peer transport will call inbound_send for routing
        call_receive response;
        int ret = CO_AWAIT call_peer(protocol_version,
            call_send{.encoding = encoding,
                .tag = tag,
                .caller_zone_id = caller_zone_id.get_val(),
                .destination_zone_id = destination_zone_id.get_val(),
                .object_id = object_id.get_val(),
                .interface_id = interface_id.get_val(),
                .method_id = method_id.get_val(),
                .payload = std::vector<char>((const char*)in_data.begin, (const char*)in_data.end),
                .back_channel = in_back_channel},
            response);
        if (ret != rpc::error::OK())
        {
            RPC_ERROR("failed spsc_transport::send call_send");
            CO_RETURN ret;
        }

        out_buf_.swap(response.payload);
        out_back_channel.swap(response.back_channel);

        RPC_DEBUG("spsc_transport::send complete zone={}", get_zone_id().get_val());

        CO_RETURN response.err_code;
    }

    CORO_TASK(void)
    spsc_transport::post(uint64_t protocol_version,
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
#ifdef CANOPY_USE_TELEMETRY
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_transport_outbound_post(
                get_zone_id(), get_adjacent_zone_id(), destination_zone_id, caller_zone_id, object_id, interface_id, method_id);
        }
#endif
        RPC_DEBUG("spsc_transport::post zone={}", get_zone_id().get_val());

        // Check transport status
        if (get_status() != rpc::transport_status::CONNECTED)
        {
            RPC_ERROR("spsc_transport::post: transport not connected");
            CO_RETURN;
        }

        // Peer-to-peer: always transmit via SPSC queue to peer
        // The peer transport will call inbound_post for routing
        // Fire-and-forget
        send_payload(protocol_version,
            spsc::message_direction::one_way,
            spsc::post_send{.encoding = encoding,
                .tag = tag,
                .caller_zone_id = caller_zone_id.get_val(),
                .destination_zone_id = destination_zone_id.get_val(),
                .object_id = object_id.get_val(),
                .interface_id = interface_id.get_val(),
                .method_id = method_id.get_val(),
                .payload = std::vector<char>((const char*)in_data.begin, (const char*)in_data.end),
                .back_channel = in_back_channel},
            0); // sequence number 0 for one-way

        CO_RETURN;
    }

    CORO_TASK(int)
    spsc_transport::try_cast(uint64_t protocol_version,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id,
        const std::vector<rpc::back_channel_entry>& in_back_channel,
        std::vector<rpc::back_channel_entry>& out_back_channel)
    {
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_transport_outbound_try_cast(
                get_zone_id(), get_adjacent_zone_id(), destination_zone_id, get_zone_id().as_caller(), object_id, interface_id);
        }
#endif
        RPC_DEBUG("spsc_transport::try_cast zone={}", get_zone_id().get_val());

        // Check transport status
        if (get_status() != rpc::transport_status::CONNECTED)
        {
            RPC_ERROR("failed spsc_transport::try_cast - not connected, status = {}", static_cast<int>(get_status()));
            CO_RETURN rpc::error::TRANSPORT_ERROR();
        }

        // Peer-to-peer: always transmit via SPSC queue to peer
        // The peer transport will call inbound_try_cast for routing
        try_cast_receive response_data;
        int ret = CO_AWAIT call_peer(protocol_version,
            try_cast_send{.destination_zone_id = destination_zone_id.get_val(),
                .object_id = object_id.get_val(),
                .interface_id = interface_id.get_val(),
                .back_channel = in_back_channel},
            response_data);
        if (ret != rpc::error::OK())
        {
            RPC_ERROR("failed try_cast call_peer");
            CO_RETURN ret;
        }

        RPC_DEBUG("spsc_transport::try_cast complete zone={}", get_zone_id().get_val());

        out_back_channel.swap(response_data.back_channel);
        CO_RETURN response_data.err_code;
    }

    CORO_TASK(int)
    spsc_transport::add_ref(uint64_t protocol_version,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id,
        rpc::caller_zone caller_zone_id,
        rpc::known_direction_zone known_direction_zone_id,
        rpc::add_ref_options build_out_param_channel,
        uint64_t& reference_count,
        const std::vector<rpc::back_channel_entry>& in_back_channel,
        std::vector<rpc::back_channel_entry>& out_back_channel)
    {
        RPC_DEBUG("spsc_transport::add_ref zone={}", get_zone_id().get_val());

        // Check transport status
        if (get_status() != rpc::transport_status::CONNECTED)
        {
            RPC_ERROR("spsc_transport::add_ref: transport not connected");
            CO_RETURN rpc::error::TRANSPORT_ERROR();
        }

        // Peer-to-peer: always transmit via SPSC queue to peer
        // The peer transport will call inbound_add_ref for routing
        addref_receive response_data;
        int ret = CO_AWAIT call_peer(protocol_version,
            addref_send{.destination_zone_id = destination_zone_id.get_val(),
                .object_id = object_id.get_val(),
                .caller_zone_id = caller_zone_id.get_val(),
                .known_direction_zone_id = known_direction_zone_id.get_val(),
                .build_out_param_channel = build_out_param_channel,
                .back_channel = in_back_channel},
            response_data);
        if (ret != rpc::error::OK())
        {
            RPC_ERROR("failed add_ref addref_send");
            CO_RETURN ret;
        }

        reference_count = response_data.ref_count;
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
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_transport_outbound_add_ref(get_zone_id(),
                get_adjacent_zone_id(),
                destination_zone_id,
                caller_zone_id,
                object_id,
                known_direction_zone_id,
                build_out_param_channel,
                reference_count);
        }
#endif
        RPC_DEBUG("spsc_transport::add_ref complete zone={}", get_zone_id().get_val());

        CO_RETURN rpc::error::OK();
    }

    CORO_TASK(int)
    spsc_transport::release(uint64_t protocol_version,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id,
        rpc::caller_zone caller_zone_id,
        rpc::release_options options,
        uint64_t& reference_count,
        const std::vector<rpc::back_channel_entry>& in_back_channel,
        std::vector<rpc::back_channel_entry>& out_back_channel)
    {
        reference_count = 0;
        RPC_DEBUG("spsc_transport::release zone={}", get_zone_id().get_val());

        // Check transport status
        if (get_status() != rpc::transport_status::CONNECTED)
        {
            RPC_ERROR("failed spsc_transport::release - not connected, status = {}", static_cast<int>(get_status()));
            CO_RETURN rpc::error::TRANSPORT_ERROR();
        }

        send_payload(protocol_version,
            message_direction::one_way,
            release_send{.destination_zone_id = destination_zone_id.get_val(),
                .object_id = object_id.get_val(),
                .caller_zone_id = caller_zone_id.get_val(),
                .options = options,
                .back_channel = in_back_channel},
            0);

#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_transport_outbound_release(
                get_zone_id(), get_adjacent_zone_id(), destination_zone_id, caller_zone_id, object_id, options, reference_count);
        }
#endif
        CO_RETURN rpc::error::OK();
    }

    CORO_TASK(void)
    spsc_transport::object_released(uint64_t protocol_version,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id,
        rpc::caller_zone caller_zone_id,
        const std::vector<rpc::back_channel_entry>& in_back_channel)
    {
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_transport_outbound_object_released(
                get_zone_id(), get_adjacent_zone_id(), destination_zone_id, caller_zone_id, object_id);
        }
#endif
        RPC_DEBUG("spsc_transport::object_released zone={}", get_zone_id().get_val());

        // Check transport status
        if (get_status() != rpc::transport_status::CONNECTED)
        {
            RPC_ERROR(
                "failed spsc_transport::object_released - not connected, status = {}", static_cast<int>(get_status()));
            CO_RETURN;
        }

        // Send the object_released message using the internal send_payload method (post-like behavior)
        send_payload(protocol_version,
            message_direction::one_way,                            // Use one_way for fire-and-forget
            object_released_send{.encoding = encoding::yas_binary, // Assuming encoding field exists
                .destination_zone_id = destination_zone_id.get_val(),
                .object_id = object_id.get_val(),
                .caller_zone_id = caller_zone_id.get_val(),
                .back_channel = in_back_channel},
            0); // sequence number 0 for one-way messages

        RPC_DEBUG("spsc_transport::object_released complete zone={}", get_zone_id().get_val());
    }

    CORO_TASK(void)
    spsc_transport::transport_down(uint64_t protocol_version,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        const std::vector<rpc::back_channel_entry>& in_back_channel)
    {
#ifdef CANOPY_USE_TELEMETRY
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_transport_outbound_transport_down(
                get_zone_id(), get_adjacent_zone_id(), destination_zone_id, caller_zone_id);
        }
#endif
        RPC_DEBUG("spsc_transport::transport_down zone={}", get_zone_id().get_val());

        // Check transport status
        if (get_status() != rpc::transport_status::CONNECTED)
        {
            RPC_ERROR("failed spsc_transport::transport_down - not connected, status = {}", static_cast<int>(get_status()));
            CO_RETURN;
        }

        // Send the transport_down message using the internal send_payload method (post-like behavior)
        send_payload(protocol_version,
            message_direction::one_way,                           // Use one_way for fire-and-forget
            transport_down_send{.encoding = encoding::yas_binary, // Assuming encoding field exists
                .destination_zone_id = destination_zone_id.get_val(),
                .caller_zone_id = caller_zone_id.get_val(),
                .back_channel = in_back_channel},
            0); // sequence number 0 for one-way messages

        RPC_DEBUG("spsc_transport::transport_down complete zone={}", get_zone_id().get_val());
    }

    // Producer/consumer tasks for message pumping
    CORO_TASK(void) spsc_transport::pump_send_and_receive()
    {
        RPC_DEBUG("pump_send_and_receive zone={}", get_zone_id().get_val());

        // Message handler lambda that processes incoming messages
        auto incoming_message_handler = [this](envelope_prefix prefix, envelope_payload payload) -> void
        {
            // Handle different message types
            if (payload.payload_fingerprint == rpc::id<call_send>::get(prefix.version))
            {
                RPC_DEBUG("pump: received call_send seq={}", prefix.sequence_number);
                get_service()->get_scheduler()->spawn(stub_handle_send(std::move(prefix), std::move(payload)));
            }
            else if (payload.payload_fingerprint == rpc::id<try_cast_send>::get(prefix.version))
            {
                RPC_DEBUG("pump: received try_cast_send seq={}", prefix.sequence_number);
                get_service()->get_scheduler()->spawn(stub_handle_try_cast(std::move(prefix), std::move(payload)));
            }
            else if (payload.payload_fingerprint == rpc::id<addref_send>::get(prefix.version))
            {
                RPC_DEBUG("pump: received addref_send seq={}", prefix.sequence_number);
                get_service()->get_scheduler()->spawn(stub_handle_add_ref(std::move(prefix), std::move(payload)));
            }
            else if (payload.payload_fingerprint == rpc::id<release_send>::get(prefix.version))
            {
                RPC_DEBUG("pump: received release_send seq={}", prefix.sequence_number);
                get_service()->get_scheduler()->spawn(stub_handle_release(std::move(prefix), std::move(payload)));
            }
            else if (payload.payload_fingerprint == rpc::id<init_client_channel_send>::get(prefix.version))
            {
                RPC_DEBUG("pump: received init_client_channel_send seq={}", prefix.sequence_number);
                get_service()->get_scheduler()->spawn(create_stub(std::move(prefix), std::move(payload)));
            }
            else if (payload.payload_fingerprint == rpc::id<post_send>::get(prefix.version))
            {
                RPC_DEBUG("pump: received post_send seq={}", prefix.sequence_number);
                get_service()->get_scheduler()->spawn(stub_handle_post(std::move(prefix), std::move(payload)));
            }
            else if (payload.payload_fingerprint == rpc::id<object_released_send>::get(prefix.version))
            {
                RPC_DEBUG("pump: received object_released_send seq={}", prefix.sequence_number);
                get_service()->get_scheduler()->spawn(stub_handle_object_released(std::move(prefix), std::move(payload)));
            }
            else if (payload.payload_fingerprint == rpc::id<transport_down_send>::get(prefix.version))
            {
                RPC_DEBUG("pump: received transport_down_send seq={}", prefix.sequence_number);
                get_service()->get_scheduler()->spawn(stub_handle_transport_down(std::move(prefix), std::move(payload)));
            }
            else if (payload.payload_fingerprint == rpc::id<close_connection_send>::get(prefix.version))
            {
                RPC_DEBUG("pump: received close_connection_send seq={}", prefix.sequence_number);
                set_status(rpc::transport_status::DISCONNECTED);
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
                        RPC_WARNING("No pending transmit found for sequence_number: {}, ignoring message id: {}",
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

                    result->error_code = rpc::error::OK();
                    result->event.set();
                }
            }
        };

        // Schedule both producer and consumer tasks
        get_service()->get_scheduler()->spawn(receive_consumer_loop(incoming_message_handler));
        get_service()->get_scheduler()->spawn(send_producer_loop());

        RPC_DEBUG("pump_send_and_receive: scheduled tasks for zone {}", get_zone_id().get_val());
        CO_RETURN;
    }

    CORO_TASK(void) spsc_transport::shutdown()
    {
        if (get_status() == rpc::transport_status::DISCONNECTED)
        {
            // Already shutting down
            RPC_DEBUG("shutdown() already in progress for zone {}", get_zone_id().get_val());
            CO_AWAIT shutdown_event_;
            CO_RETURN;
        }

        RPC_DEBUG("shutdown() initiating for zone {}", get_zone_id().get_val());

        // Set transport status to DISCONNECTED
        set_status(rpc::transport_status::DISCONNECTED);

        // Wait for both tasks to complete
        CO_AWAIT shutdown_event_;
        RPC_DEBUG("shutdown() completed for zone {}", get_zone_id().get_val());
        CO_RETURN;
    }

    // Receive consumer task
    CORO_TASK(void)
    spsc_transport::receive_consumer_loop(std::function<void(envelope_prefix, envelope_payload)> incoming_message_handler)
    {
        static auto envelope_prefix_saved_size = rpc::yas_binary_saved_size(envelope_prefix());

        std::vector<uint8_t> prefix_buf(envelope_prefix_saved_size);
        std::vector<uint8_t> buf;

        bool receiving_prefix = true;
        std::span<uint8_t> receive_data;

        RPC_DEBUG("receive_consumer_loop started for zone {}", get_zone_id().get_val());

        // Prefix must persist across loop iterations while receiving payload chunks
        envelope_prefix prefix{};

        // Continue running if:
        // - Not cancelled by peer (or we're waiting for close ack, or we have pending operations), AND
        // - Not cancelled by us
        // The waiting_for_close_ack_ check handles simultaneous shutdown where both sides
        // send close_connection_send - we need to keep receiving to get the response.
        // We also need to stay alive while the response_task is running (after receiving close_connection_send),
        // which is indicated by waiting_for_close_ack_ being true AND close_ack_queued_ being false
        // (meaning the response hasn't been sent yet).
        // Additionally, we must stay alive until response_task completes sending the close ack
        // and the send loop has processed it (indicated by waiting_for_close_ack_ being true
        // AND response_task_complete being false).
        while (get_status() != rpc::transport_status::DISCONNECTED)
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
                        if (!received_any)
                        {
                            CO_AWAIT get_service() -> get_scheduler()->schedule();
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

                if (receive_data.empty())
                {
                    auto str_err = rpc::from_yas_binary(rpc::span(prefix_buf), prefix);
                    if (!str_err.empty())
                    {
                        RPC_ERROR("failed invalid prefix");
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
                        if (!received_any)
                        {
                            CO_AWAIT get_service() -> get_scheduler()->schedule();
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

                if (receive_data.empty())
                {
                    envelope_payload payload;
                    auto str_err = rpc::from_yas_binary(rpc::span(buf), payload);
                    if (!str_err.empty())
                    {
                        RPC_ERROR("failed bad payload format");
                        break;
                    }

                    incoming_message_handler(std::move(prefix), std::move(payload));
                    receiving_prefix = true;
                }
            }
        }

        RPC_DEBUG("receive_consumer_loop exiting for zone {}", get_zone_id().get_val());

        int completed = ++shutdown_sequence_completed_;
        RPC_DEBUG("receive_consumer_loop: tasks_completed={} for zone {}", completed, get_zone_id().get_val());

        if (completed == 2)
        {
            RPC_DEBUG("Both tasks completed, releasing keep_alive for zone {}", get_zone_id().get_val());
            keep_alive_.reset();
            shutdown_event_.set();
        }

        CO_RETURN;
    }

    // Send producer task
    CORO_TASK(void) spsc_transport::send_producer_loop()
    {
        std::span<uint8_t> send_data;

        RPC_DEBUG("send_producer_loop started for zone {}", get_zone_id().get_val());

        while (get_status() != rpc::transport_status::DISCONNECTED)
        {
            if (send_data.empty())
            {
                // note be careful here using an unRAII'd lock here ensure that you unlo
                send_queue_mtx_.lock();
                if (send_queue_.empty())
                {
                    send_queue_mtx_.unlock();
                    CO_AWAIT get_service() -> get_scheduler()->schedule();
                    continue;
                }

                auto& item = send_queue_.front();
                send_data = {item.begin(), item.end()};
                send_queue_mtx_.unlock();
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
            }
            else
            {
                CO_AWAIT get_service() -> get_scheduler()->schedule();
            }
        }

        RPC_DEBUG("send cancellation message {}", get_zone_id().get_val());

        envelope_payload payload_envelope
            = {.payload_fingerprint = rpc::id<close_connection_send>::get(rpc::get_version()),
                .payload = rpc::to_yas_binary(close_connection_send{})};

        auto prefix = envelope_prefix{.version = rpc::get_version(),
            .direction = rpc::spsc::message_direction::one_way,
            .sequence_number = 0,
            .payload_size = rpc::yas_binary_saved_size(payload_envelope)};

        // Serialize to vectors first, then copy to message_blobs
        send_spsc_queue_->push(rpc::to_yas_binary<message_blob>(prefix));
        send_spsc_queue_->push(rpc::to_yas_binary<message_blob>(payload_envelope));

        RPC_DEBUG("send_producer_loop completed sending for zone {}", get_zone_id().get_val());

        {
            std::scoped_lock lock(pending_transmits_mtx_);
            for (auto it : pending_transmits_)
            {
                it.second->error_code = rpc::error::CALL_CANCELLED();
                it.second->event.set();
            }
        }

        int completed = ++shutdown_sequence_completed_;
        RPC_DEBUG("send_producer_loop: tasks_completed={} for zone {}", completed, get_zone_id().get_val());

        if (completed == 2)
        {
            RPC_DEBUG("Both tasks completed, releasing keep_alive for zone {}", get_zone_id().get_val());
            keep_alive_.reset();
            shutdown_event_.set();
        }

        RPC_DEBUG("send_producer_loop exiting for zone {}", get_zone_id().get_val());
        CO_RETURN;
    }

    // Stub handlers (server-side message processing)
    CORO_TASK(void) spsc_transport::stub_handle_send(envelope_prefix prefix, envelope_payload payload)
    {
        RPC_DEBUG("stub_handle_send");

        assert(prefix.direction == message_direction::send || prefix.direction == message_direction::one_way);

        call_send request;
        auto str_err = rpc::from_yas_binary(rpc::span(payload.payload), request);
        if (!str_err.empty())
        {
            RPC_ERROR("failed from_yas_binary call_send");
            kill_connection();
            CO_RETURN;
        }

        std::vector<char> out_buf;
        std::vector<rpc::back_channel_entry> out_back_channel;
        // Call inbound_send for routing - transport will route to correct destination
        auto ret = CO_AWAIT inbound_send(prefix.version,
            request.encoding,
            request.tag,
            {request.caller_zone_id},
            {request.destination_zone_id},
            {request.object_id},
            {request.interface_id},
            {request.method_id},
            request.payload,
            out_buf,
            request.back_channel,
            out_back_channel);

        if (ret != rpc::error::OK())
        {
            RPC_ERROR("failed send");
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

    CORO_TASK(void) spsc_transport::stub_handle_post(envelope_prefix prefix, envelope_payload payload)
    {
        RPC_DEBUG("stub_handle_post");

        post_send request;
        auto str_err = rpc::from_yas_binary(rpc::span(payload.payload), request);
        if (!str_err.empty())
        {
            RPC_ERROR("failed post_send from_yas_binary");
            kill_connection();
            CO_RETURN;
        }

        // Call inbound_post for routing - transport will route to correct destination
        CO_AWAIT inbound_post(prefix.version,
            request.encoding,
            request.tag,
            {request.caller_zone_id},
            {request.destination_zone_id},
            {request.object_id},
            {request.interface_id},
            {request.method_id},
            request.payload,
            request.back_channel);

        // No response needed for post operations (fire-and-forget)
        RPC_DEBUG("stub_handle_post complete");
        CO_RETURN;
    }

    CORO_TASK(void) spsc_transport::stub_handle_try_cast(envelope_prefix prefix, envelope_payload payload)
    {
        RPC_DEBUG("stub_handle_try_cast");

        try_cast_send request;
        auto str_err = rpc::from_yas_binary(rpc::span(payload.payload), request);
        if (!str_err.empty())
        {
            RPC_ERROR("failed try_cast_send from_yas_binary");
            kill_connection();
            CO_RETURN;
        }

        std::vector<rpc::back_channel_entry> out_back_channel;
        // Call inbound_try_cast for routing - transport will route to correct destination
        auto ret = CO_AWAIT inbound_try_cast(prefix.version,
            {request.destination_zone_id},
            {request.object_id},
            {request.interface_id},
            request.back_channel,
            out_back_channel);
        if (ret != rpc::error::OK())
        {
            RPC_ERROR("failed try_cast");
        }

        send_payload(prefix.version,
            message_direction::receive,
            try_cast_receive{.back_channel = std::move(out_back_channel), .err_code = ret},
            prefix.sequence_number);
        RPC_DEBUG("stub_handle_try_cast complete");
        CO_RETURN;
    }

    CORO_TASK(void) spsc_transport::stub_handle_add_ref(envelope_prefix prefix, envelope_payload payload)
    {
        RPC_DEBUG("stub_handle_add_ref");

        addref_send request;
        auto str_err = rpc::from_yas_binary(rpc::span(payload.payload), request);
        if (!str_err.empty())
        {
            RPC_ERROR("failed addref_send from_yas_binary");
            kill_connection();
            CO_RETURN;
        }

        uint64_t ref_count = 0;
        std::vector<rpc::back_channel_entry> out_back_channel;
        // Call inbound_add_ref for routing - transport will route to correct destination
        auto ret = CO_AWAIT inbound_add_ref(prefix.version,
            {request.destination_zone_id},
            {request.object_id},
            {request.caller_zone_id},
            {request.known_direction_zone_id},
            (rpc::add_ref_options)request.build_out_param_channel,
            ref_count,
            request.back_channel,
            out_back_channel);

        if (ret != rpc::error::OK())
        {
            RPC_ERROR("failed add_ref");
        }

        send_payload(prefix.version,
            message_direction::receive,
            addref_receive{.ref_count = ref_count, .back_channel = std::move(out_back_channel), .err_code = ret},
            prefix.sequence_number);
        RPC_DEBUG("add_ref request complete");
        CO_RETURN;
    }

    CORO_TASK(void) spsc_transport::stub_handle_release(envelope_prefix prefix, envelope_payload payload)
    {
        RPC_DEBUG("stub_handle_release");

        release_send request;
        auto str_err = rpc::from_yas_binary(rpc::span(payload.payload), request);
        if (!str_err.empty())
        {
            RPC_ERROR("failed release_send from_yas_binary");
            kill_connection();
            CO_RETURN;
        }

        uint64_t ref_count = 0;
        std::vector<rpc::back_channel_entry> out_back_channel;
        // Call inbound_release for routing - transport will route to correct destination
        auto ret = CO_AWAIT inbound_release(prefix.version,
            {request.destination_zone_id},
            {request.object_id},
            {request.caller_zone_id},
            request.options,
            ref_count,
            request.back_channel,
            out_back_channel);

        if (ret != rpc::error::OK())
        {
            RPC_ERROR("failed release");
        }

        RPC_DEBUG("release request complete");
        CO_RETURN;
    }

    CORO_TASK(void) spsc_transport::stub_handle_object_released(envelope_prefix prefix, envelope_payload payload)
    {
        RPC_DEBUG("stub_handle_object_released");

        object_released_send request;
        auto str_err = rpc::from_yas_binary(rpc::span(payload.payload), request);
        if (!str_err.empty())
        {
            RPC_ERROR("failed object_released_send from_yas_binary");
            kill_connection();
            CO_RETURN;
        }

        // Call inbound_object_released for routing - transport will route to correct destination
        CO_AWAIT inbound_object_released(
            prefix.version, {request.destination_zone_id}, {request.object_id}, {request.caller_zone_id}, request.back_channel);

        // No response needed for object_released (fire-and-forget)
        RPC_DEBUG("stub_handle_object_released complete");
        CO_RETURN;
    }

    CORO_TASK(void) spsc_transport::stub_handle_transport_down(envelope_prefix prefix, envelope_payload payload)
    {
        RPC_DEBUG("stub_handle_transport_down");

        transport_down_send request;
        auto str_err = rpc::from_yas_binary(rpc::span(payload.payload), request);
        if (!str_err.empty())
        {
            RPC_ERROR("failed transport_down_send from_yas_binary");
            kill_connection();
            CO_RETURN;
        }

        // Call inbound_transport_down for routing - transport will route to correct destination
        CO_AWAIT inbound_transport_down(
            prefix.version, {request.destination_zone_id}, {request.caller_zone_id}, request.back_channel);

        // No response needed for transport_down (fire-and-forget)
        RPC_DEBUG("stub_handle_transport_down complete");
        CO_RETURN;
    }

    CORO_TASK(void) spsc_transport::create_stub(envelope_prefix prefix, envelope_payload payload)
    {
        RPC_DEBUG("create_stub zone: {}", get_zone_id().get_val());

        init_client_channel_send request;
        auto err = rpc::from_yas_binary(rpc::span(payload.payload), request);
        if (!err.empty())
        {
            RPC_ERROR("failed create_stub init_client_channel_send deserialization");
            CO_RETURN;
        }
        rpc::interface_descriptor input_descr{{request.caller_object_id}, {request.caller_zone_id}};
        rpc::interface_descriptor output_interface;

        int ret = CO_AWAIT connection_handler_(input_descr, output_interface, get_service(), keep_alive_.get_nullable());
        connection_handler_ = nullptr;
        if (ret != rpc::error::OK())
        {
            RPC_ERROR("failed to connect to zone {}", ret);
            CO_RETURN;
        }

        // Set transport to CONNECTED after successful server-side handshake
        set_status(rpc::transport_status::CONNECTED);

        send_payload(prefix.version,
            message_direction::receive,
            init_client_channel_response{.err_code = rpc::error::OK(),
                .destination_zone_id = output_interface.destination_zone_id.get_val(),
                .destination_object_id = output_interface.object_id.get_val(),
                .caller_zone_id = input_descr.destination_zone_id.get_val()},
            prefix.sequence_number);

        CO_RETURN;
    }
}
