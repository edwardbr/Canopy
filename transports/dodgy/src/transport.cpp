/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <transports/dodgy/transport.h>

namespace rpc::dodgy
{
    dodgy_transport::dodgy_transport(std::string name,
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

    std::shared_ptr<dodgy_transport> dodgy_transport::create(std::string name,
        std::shared_ptr<rpc::service> service,
        rpc::zone adjacent_zone_id,
        queue_type* send_spsc_queue,
        queue_type* receive_spsc_queue,
        connection_handler handler)
    {
        auto transport = std::shared_ptr<dodgy_transport>(
            new dodgy_transport(name, service, adjacent_zone_id, send_spsc_queue, receive_spsc_queue, handler));
        transport->keep_alive_ = transport;
        return transport;
    }

    // Connection handshake
    CORO_TASK(int)
    dodgy_transport::inner_connect(rpc::interface_descriptor input_descr, rpc::interface_descriptor& output_descr)
    {
        RPC_DEBUG("dodgy_transport::connect zone={}", get_zone_id().get_val());

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
                RPC_ERROR("dodgy_transport::connect call_peer failed {}", rpc::error::to_string(ret));
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
    dodgy_transport::outbound_send(uint64_t protocol_version,
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
        RPC_DEBUG("dodgy_transport::outbound_send zone={}", get_zone_id().get_val());

        // Check transport status
        if (get_status() != rpc::transport_status::CONNECTED)
        {
            RPC_ERROR("failed dodgy_transport::outbound_send - not connected, status = {}", static_cast<int>(get_status()));
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
            RPC_ERROR("failed dodgy_transport::outbound_send call_send");
            CO_RETURN ret;
        }

        out_buf_.swap(response.payload);
        out_back_channel.swap(response.back_channel);

        RPC_DEBUG("dodgy_transport::outbound_send complete zone={}", get_zone_id().get_val());

        CO_RETURN response.err_code;
    }

    CORO_TASK(void)
    dodgy_transport::outbound_post(uint64_t protocol_version,
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
        RPC_DEBUG("dodgy_transport::outbound_post zone={}", get_zone_id().get_val());

        // Check transport status
        if (get_status() != rpc::transport_status::CONNECTED)
        {
            RPC_ERROR("dodgy_transport::outbound_post: transport not connected");
            CO_RETURN;
        }

        // Peer-to-peer: always transmit via SPSC queue to peer
        // The peer transport will call inbound_post for routing
        // Fire-and-forget
        int ret = CO_AWAIT send_payload(protocol_version,
            dodgy::message_direction::one_way,
            dodgy::post_send{.encoding = encoding,
                .tag = tag,
                .caller_zone_id = caller_zone_id.get_val(),
                .destination_zone_id = destination_zone_id.get_val(),
                .object_id = object_id.get_val(),
                .interface_id = interface_id.get_val(),
                .method_id = method_id.get_val(),
                .payload = std::vector<char>((const char*)in_data.begin, (const char*)in_data.end),
                .back_channel = in_back_channel},
            0); // sequence number 0 for one-way

        if (ret != rpc::error::OK())
        {
            RPC_ERROR("failed dodgy_transport::outbound_post send_payload");
        }

        CO_RETURN;
    }

    CORO_TASK(int)
    dodgy_transport::outbound_try_cast(uint64_t protocol_version,
        rpc::caller_zone caller_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id,
        const std::vector<rpc::back_channel_entry>& in_back_channel,
        std::vector<rpc::back_channel_entry>& out_back_channel)
    {
        RPC_DEBUG("dodgy_transport::outbound_try_cast zone={}", get_zone_id().get_val());

        // Check transport status
        if (get_status() != rpc::transport_status::CONNECTED)
        {
            RPC_ERROR("failed dodgy_transport::outbound_try_cast - not connected, status = {}",
                static_cast<int>(get_status()));
            CO_RETURN rpc::error::TRANSPORT_ERROR();
        }

        // Peer-to-peer: always transmit via SPSC queue to peer
        // The peer transport will call inbound_try_cast for routing
        try_cast_receive response_data;
        int ret = CO_AWAIT call_peer(protocol_version,
            try_cast_send{.caller_zone_id = caller_zone_id.get_val(),
                .destination_zone_id = destination_zone_id.get_val(),
                .object_id = object_id.get_val(),
                .interface_id = interface_id.get_val(),
                .back_channel = in_back_channel},
            response_data);
        if (ret != rpc::error::OK())
        {
            RPC_ERROR("failed try_cast call_peer");
            CO_RETURN ret;
        }

        RPC_DEBUG("dodgy_transport::outbound_try_cast complete zone={}", get_zone_id().get_val());

        out_back_channel.swap(response_data.back_channel);
        CO_RETURN response_data.err_code;
    }

    CORO_TASK(int)
    dodgy_transport::outbound_add_ref(uint64_t protocol_version,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id,
        rpc::caller_zone caller_zone_id,
        rpc::known_direction_zone known_direction_zone_id,
        rpc::add_ref_options build_out_param_channel,
        const std::vector<rpc::back_channel_entry>& in_back_channel,
        std::vector<rpc::back_channel_entry>& out_back_channel)
    {
        RPC_DEBUG("dodgy_transport::outbound_add_ref zone={}", get_zone_id().get_val());

        // Check transport status
        if (get_status() != rpc::transport_status::CONNECTED)
        {
            RPC_ERROR("dodgy_transport::outbound_add_ref: transport not connected");
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

        out_back_channel.swap(response_data.back_channel);
        if (response_data.err_code != rpc::error::OK())
        {
            RPC_ERROR("failed addref_receive.err_code failed");
            RPC_ASSERT(false);
            CO_RETURN response_data.err_code;
        }

        RPC_DEBUG("dodgy_transport::outbound_add_ref complete zone={}", get_zone_id().get_val());

        CO_RETURN rpc::error::OK();
    }

    CORO_TASK(int)
    dodgy_transport::outbound_release(uint64_t protocol_version,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id,
        rpc::caller_zone caller_zone_id,
        rpc::release_options options,
        const std::vector<rpc::back_channel_entry>& in_back_channel,
        std::vector<rpc::back_channel_entry>& out_back_channel)
    {
        RPC_DEBUG("dodgy_transport::outbound_release zone={}", get_zone_id().get_val());

        // Check transport status
        if (get_status() != rpc::transport_status::CONNECTED)
        {
            RPC_ERROR("failed dodgy_transport::outbound_release - not connected, status = {}",
                static_cast<int>(get_status()));
            CO_RETURN rpc::error::TRANSPORT_ERROR();
        }

        // Peer-to-peer: always transmit via SPSC queue to peer
        // The peer transport will call inbound_release for routing
        release_receive response;
        int ret = CO_AWAIT call_peer(protocol_version,
            release_send{.destination_zone_id = destination_zone_id.get_val(),
                .object_id = object_id.get_val(),
                .caller_zone_id = caller_zone_id.get_val(),
                .options = options,
                .back_channel = in_back_channel},
            response);
        if (ret != rpc::error::OK())
        {
            RPC_ERROR("failed dodgy_transport::outbound_release release_send");
            CO_RETURN ret;
        }

        if (response.err_code != rpc::error::OK())
        {
            RPC_ERROR("failed response.err_code failed in release");
            RPC_ASSERT(false);
            CO_RETURN response.err_code;
        }

        RPC_DEBUG("dodgy_transport::outbound_release complete zone={}", get_zone_id().get_val());

        out_back_channel.swap(response.back_channel);

        CO_RETURN rpc::error::OK();
    }

    CORO_TASK(void)
    dodgy_transport::outbound_object_released(uint64_t protocol_version,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id,
        rpc::caller_zone caller_zone_id,
        const std::vector<rpc::back_channel_entry>& in_back_channel)
    {

        RPC_DEBUG("dodgy_transport::outbound_object_released zone={}", get_zone_id().get_val());

        // Check transport status
        if (get_status() != rpc::transport_status::CONNECTED)
        {
            RPC_ERROR("failed dodgy_transport::outbound_object_released - not connected, status = {}",
                static_cast<int>(get_status()));
            CO_RETURN;
        }

        // Send the object_released message using the internal send_payload method (post-like behavior)
        int ret = CO_AWAIT send_payload(protocol_version,
            message_direction::one_way,                            // Use one_way for fire-and-forget
            object_released_send{.encoding = encoding::yas_binary, // Assuming encoding field exists
                .destination_zone_id = destination_zone_id.get_val(),
                .object_id = object_id.get_val(),
                .caller_zone_id = caller_zone_id.get_val(),
                .back_channel = in_back_channel},
            0); // sequence number 0 for one-way messages

        if (ret != rpc::error::OK())
        {
            RPC_ERROR("failed dodgy_transport::outbound_object_released send_payload");
        }

        RPC_DEBUG("dodgy_transport::outbound_object_released complete zone={}", get_zone_id().get_val());
    }

    CORO_TASK(void)
    dodgy_transport::outbound_transport_down(uint64_t protocol_version,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        const std::vector<rpc::back_channel_entry>& in_back_channel)
    {
        RPC_DEBUG("dodgy_transport::outbound_transport_down zone={}", get_zone_id().get_val());

        // Check transport status
        if (get_status() != rpc::transport_status::CONNECTED)
        {
            RPC_ERROR("failed dodgy_transport::outbound_transport_down - not connected, status = {}",
                static_cast<int>(get_status()));
            CO_RETURN;
        }

        // Send the transport_down message using the internal send_payload method (post-like behavior)
        int ret = CO_AWAIT send_payload(protocol_version,
            message_direction::one_way,                           // Use one_way for fire-and-forget
            transport_down_send{.encoding = encoding::yas_binary, // Assuming encoding field exists
                .destination_zone_id = destination_zone_id.get_val(),
                .caller_zone_id = caller_zone_id.get_val(),
                .back_channel = in_back_channel},
            0); // sequence number 0 for one-way messages

        if (ret != rpc::error::OK())
        {
            RPC_ERROR("failed dodgy_transport::outbound_transport_down send_payload");
        }

        RPC_DEBUG("dodgy_transport::outbound_transport_down complete zone={}", get_zone_id().get_val());
    }

    // Producer/consumer tasks for message pumping
    CORO_TASK(void) dodgy_transport::pump_send_and_receive()
    {
        RPC_DEBUG("pump_send_and_receive zone={}", get_service()->get_zone_id().get_val());

        // Message handler lambda that processes incoming messages
        auto incoming_message_handler = [this](envelope_prefix prefix, envelope_payload payload) -> void
        {
            // Handle different message types
            if (payload.payload_fingerprint == rpc::id<call_send>::get(prefix.version))
            {
                assert(!peer_cancel_received_);
                get_service()->get_scheduler()->spawn(stub_handle_send(std::move(prefix), std::move(payload)));
            }
            else if (payload.payload_fingerprint == rpc::id<try_cast_send>::get(prefix.version))
            {
                assert(!peer_cancel_received_);
                get_service()->get_scheduler()->spawn(stub_handle_try_cast(std::move(prefix), std::move(payload)));
            }
            else if (payload.payload_fingerprint == rpc::id<addref_send>::get(prefix.version))
            {
                assert(!peer_cancel_received_);
                get_service()->get_scheduler()->spawn(stub_handle_add_ref(std::move(prefix), std::move(payload)));
            }
            else if (payload.payload_fingerprint == rpc::id<release_send>::get(prefix.version))
            {
                get_service()->get_scheduler()->spawn(stub_handle_release(std::move(prefix), std::move(payload)));
            }
            else if (payload.payload_fingerprint == rpc::id<init_client_channel_send>::get(prefix.version))
            {
                assert(!peer_cancel_received_);
                get_service()->get_scheduler()->spawn(create_stub(std::move(prefix), std::move(payload)));
            }
            else if (payload.payload_fingerprint == rpc::id<post_send>::get(prefix.version))
            {
                assert(!peer_cancel_received_);
                get_service()->get_scheduler()->spawn(stub_handle_post(std::move(prefix), std::move(payload)));
            }
            else if (payload.payload_fingerprint == rpc::id<object_released_send>::get(prefix.version))
            {
                assert(!peer_cancel_received_);
                get_service()->get_scheduler()->spawn(stub_handle_object_released(std::move(prefix), std::move(payload)));
            }
            else if (payload.payload_fingerprint == rpc::id<transport_down_send>::get(prefix.version))
            {
                assert(!peer_cancel_received_);
                get_service()->get_scheduler()->spawn(stub_handle_transport_down(std::move(prefix), std::move(payload)));
            }
            else if (payload.payload_fingerprint == rpc::id<close_connection_send>::get(prefix.version))
            {
                // Handle close connection request
                auto response_task = [this, seq = prefix.sequence_number]() -> coro::task<void>
                {
                    RPC_DEBUG("close_connection: sending response for zone {}", get_service()->get_zone_id().get_val());
                    std::ignore = CO_AWAIT send_payload(
                        rpc::get_version(), message_direction::receive, close_connection_received{}, seq);

                    close_ack_queued_ = true;
                    peer_cancel_received_ = true;

                    RPC_DEBUG("close_connection: response queued for zone {}", get_service()->get_zone_id().get_val());
                    CO_RETURN;
                };
                get_service()->get_scheduler()->spawn(response_task());
            }
            else
            {
                // now find the relevant event handler and set its values before triggering it
                result_listener* result = nullptr;
                {
                    std::scoped_lock lock(pending_transmits_mtx_);
                    auto it = pending_transmits_.find(prefix.sequence_number);
                    RPC_DEBUG("pending_transmits_ zone: {} sequence_number: {} id: {}",
                        get_service()->get_zone_id().get_val(),
                        prefix.sequence_number,
                        payload.payload_fingerprint);
                    assert(it != pending_transmits_.end());
                    result = it->second;
                    pending_transmits_.erase(it);
                    pending_transmits_count_.fetch_sub(1, std::memory_order_release);
                }

                result->prefix = std::move(prefix);
                result->payload = std::move(payload);

                RPC_DEBUG("pump_send_and_receive prefix.sequence_number {}\n prefix = {}\n payload = {}",
                    get_service()->get_zone_id().get_val(),
                    rpc::to_yas_json<std::string>(prefix),
                    rpc::to_yas_json<std::string>(result->payload));

                result->error_code = rpc::error::OK();
                result->event.set();
            }
        };

        // Schedule both producer and consumer tasks
        get_service()->get_scheduler()->spawn(receive_consumer_loop(incoming_message_handler));
        get_service()->get_scheduler()->spawn(send_producer_loop());

        RPC_DEBUG("pump_send_and_receive: scheduled tasks for zone {}", get_service()->get_zone_id().get_val());
        CO_RETURN;
    }

    CORO_TASK(void) dodgy_transport::shutdown()
    {
        if (cancel_sent_.load(std::memory_order_acquire))
        {
            // Already shutting down
            RPC_DEBUG("shutdown() already in progress for zone {}", get_service()->get_zone_id().get_val());
            CO_AWAIT shutdown_event_;
            CO_RETURN;
        }

        RPC_DEBUG("shutdown() initiating for zone {}", get_service()->get_zone_id().get_val());
        cancel_sent_.store(true, std::memory_order_release);

        // Set transport status to DISCONNECTED
        set_status(rpc::transport_status::DISCONNECTED);

        close_connection_received received{};
        auto err = CO_AWAIT call_peer(rpc::get_version(), close_connection_send{}, received);
        RPC_DEBUG("shutdown() received response for zone {}, err={}", get_service()->get_zone_id().get_val(), err);

        cancel_confirmed_.store(true, std::memory_order_release);

        if (err != rpc::error::OK())
        {
            peer_cancel_received_ = true;
        }

        // Wait for both tasks to complete
        CO_AWAIT shutdown_event_;
        RPC_DEBUG("shutdown() completed for zone {}", get_service()->get_zone_id().get_val());
        CO_RETURN;
    }

    // Receive consumer task
    CORO_TASK(void)
    dodgy_transport::receive_consumer_loop(std::function<void(envelope_prefix, envelope_payload)> incoming_message_handler)
    {
        static auto envelope_prefix_saved_size = rpc::yas_binary_saved_size(envelope_prefix());

        std::vector<uint8_t> prefix_buf(envelope_prefix_saved_size);
        std::vector<uint8_t> buf;

        bool receiving_prefix = true;
        std::span<uint8_t> receive_data;

        RPC_DEBUG("receive_consumer_loop started for zone {}", get_service()->get_zone_id().get_val());

        while (!peer_cancel_received_.load(std::memory_order_acquire) && !cancel_confirmed_.load(std::memory_order_acquire))
        {
            envelope_prefix prefix{};

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

        RPC_DEBUG("receive_consumer_loop exiting for zone {}", get_service()->get_zone_id().get_val());

        int completed = ++shutdown_sequence_completed_;
        RPC_DEBUG(
            "receive_consumer_loop: tasks_completed={} for zone {}", completed, get_service()->get_zone_id().get_val());

        if (completed == 2)
        {
            RPC_DEBUG("Both tasks completed, releasing keep_alive for zone {}", get_service()->get_zone_id().get_val());
            keep_alive_.reset();
            shutdown_event_.set();
        }

        CO_RETURN;
    }

    // Send producer task
    CORO_TASK(void) dodgy_transport::send_producer_loop()
    {
        std::span<uint8_t> send_data;

        RPC_DEBUG("send_producer_loop started for zone {}", get_service()->get_zone_id().get_val());

        while ((!close_ack_queued_.load(std::memory_order_acquire) && !cancel_confirmed_.load(std::memory_order_acquire))
               || send_queue_count_.load(std::memory_order_acquire) > 0 || !send_data.empty())
        {
            if (send_data.empty())
            {
                auto scoped_lock = CO_AWAIT send_queue_mtx_.lock();
                if (send_queue_.empty())
                {
                    scoped_lock.unlock();
                    CO_AWAIT get_service() -> get_scheduler()->schedule();
                    continue;
                }

                auto& item = send_queue_.front();
                send_data = {item.begin(), item.end()};
                scoped_lock.unlock();
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
                    auto scoped_lock = CO_AWAIT send_queue_mtx_.lock();
                    send_queue_.pop();
                    send_queue_count_.fetch_sub(1, std::memory_order_release);
                }
            }
            else
            {
                CO_AWAIT get_service() -> get_scheduler()->schedule();
            }
        }

        RPC_DEBUG("send_producer_loop completed sending for zone {}", get_service()->get_zone_id().get_val());

        {
            std::scoped_lock lock(pending_transmits_mtx_);
            for (auto it : pending_transmits_)
            {
                it.second->error_code = rpc::error::CALL_CANCELLED();
                it.second->event.set();
            }
        }

        int completed = ++shutdown_sequence_completed_;
        RPC_DEBUG("send_producer_loop: tasks_completed={} for zone {}", completed, get_service()->get_zone_id().get_val());

        if (completed == 2)
        {
            RPC_DEBUG("Both tasks completed, releasing keep_alive for zone {}", get_service()->get_zone_id().get_val());
            keep_alive_.reset();
            shutdown_event_.set();
        }

        RPC_DEBUG("send_producer_loop exiting for zone {}", get_service()->get_zone_id().get_val());
        CO_RETURN;
    }

    // Stub handlers (server-side message processing)
    CORO_TASK(void) dodgy_transport::stub_handle_send(envelope_prefix prefix, envelope_payload payload)
    {
        RPC_DEBUG("stub_handle_send");

        assert(prefix.direction == message_direction::send || prefix.direction == message_direction::one_way);

        if (cancel_sent_.load(std::memory_order_acquire))
        {
            auto err = CO_AWAIT send_payload(prefix.version,
                message_direction::receive,
                call_receive{.payload = {}, .back_channel = {}, .err_code = rpc::error::CALL_CANCELLED()},
                prefix.sequence_number);
            if (err != rpc::error::OK())
            {
                RPC_ERROR("failed send_payload");
                kill_connection();
                CO_RETURN;
            }
        }

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

        auto err = CO_AWAIT send_payload(prefix.version,
            message_direction::receive,
            call_receive{.payload = std::move(out_buf), .back_channel = std::move(out_back_channel), .err_code = ret},
            prefix.sequence_number);
        if (err != rpc::error::OK())
        {
            RPC_ERROR("failed send_payload");
            kill_connection();
            CO_RETURN;
        }
        RPC_DEBUG("send request complete");
        CO_RETURN;
    }

    CORO_TASK(void) dodgy_transport::stub_handle_post(envelope_prefix prefix, envelope_payload payload)
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

    CORO_TASK(void) dodgy_transport::stub_handle_try_cast(envelope_prefix prefix, envelope_payload payload)
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
            {request.caller_zone_id},
            {request.destination_zone_id},
            {request.object_id},
            {request.interface_id},
            request.back_channel,
            out_back_channel);
        if (ret != rpc::error::OK())
        {
            RPC_ERROR("failed try_cast");
        }

        auto err = CO_AWAIT send_payload(prefix.version,
            message_direction::receive,
            try_cast_receive{.back_channel = std::move(out_back_channel), .err_code = ret},
            prefix.sequence_number);
        if (err != rpc::error::OK())
        {
            RPC_ERROR("failed try_cast_send send_payload");
            kill_connection();
            CO_RETURN;
        }
        RPC_DEBUG("stub_handle_try_cast complete");
        CO_RETURN;
    }

    CORO_TASK(void) dodgy_transport::stub_handle_add_ref(envelope_prefix prefix, envelope_payload payload)
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

        std::vector<rpc::back_channel_entry> out_back_channel;
        // Call inbound_add_ref for routing - transport will route to correct destination
        auto ret = CO_AWAIT inbound_add_ref(prefix.version,
            {request.destination_zone_id},
            {request.object_id},
            {request.caller_zone_id},
            {request.known_direction_zone_id},
            (rpc::add_ref_options)request.build_out_param_channel,
            request.back_channel,
            out_back_channel);

        if (ret != rpc::error::OK())
        {
            RPC_ERROR("failed add_ref");
        }

        auto err = CO_AWAIT send_payload(prefix.version,
            message_direction::receive,
            addref_receive{.back_channel = std::move(out_back_channel), .err_code = ret},
            prefix.sequence_number);
        if (err != rpc::error::OK())
        {
            RPC_ERROR("failed addref_send send_payload");
            kill_connection();
            CO_RETURN;
        }
        RPC_DEBUG("add_ref request complete");
        CO_RETURN;
    }

    CORO_TASK(void) dodgy_transport::stub_handle_release(envelope_prefix prefix, envelope_payload payload)
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

        std::vector<rpc::back_channel_entry> out_back_channel;
        // Call inbound_release for routing - transport will route to correct destination
        auto ret = CO_AWAIT inbound_release(prefix.version,
            {request.destination_zone_id},
            {request.object_id},
            {request.caller_zone_id},
            request.options,
            request.back_channel,
            out_back_channel);

        if (ret != rpc::error::OK())
        {
            RPC_ERROR("failed release");
        }

        auto err = CO_AWAIT send_payload(prefix.version,
            message_direction::receive,
            release_receive{.back_channel = std::move(out_back_channel), .err_code = ret},
            prefix.sequence_number);
        if (err != rpc::error::OK())
        {
            RPC_ERROR("failed release_send send_payload");
            kill_connection();
            CO_RETURN;
        }
        RPC_DEBUG("release request complete");
        CO_RETURN;
    }

    CORO_TASK(void) dodgy_transport::stub_handle_object_released(envelope_prefix prefix, envelope_payload payload)
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

    CORO_TASK(void) dodgy_transport::stub_handle_transport_down(envelope_prefix prefix, envelope_payload payload)
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

    CORO_TASK(void)
    dodgy_transport::create_stub(rpc::dodgy::envelope_prefix prefix, rpc::dodgy::envelope_payload payload)
    {
        RPC_DEBUG("create_stub zone: {}", get_service()->get_zone_id().get_val());

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

        auto send_err = CO_AWAIT send_payload(prefix.version,
            message_direction::receive,
            init_client_channel_response{.err_code = rpc::error::OK(),
                .destination_zone_id = output_interface.destination_zone_id.get_val(),
                .destination_object_id = output_interface.object_id.get_val(),
                .caller_zone_id = input_descr.destination_zone_id.get_val()},
            prefix.sequence_number);
        if (send_err != rpc::error::OK())
        {
            RPC_ERROR("failed to send init_client_channel_response");
        }

        CO_RETURN;
    }

    CORO_TASK(void) dodgy_transport::trigger_network_failure()
    {
        RPC_INFO("dodgy_transport::trigger_network_failure: Simulating network failure");

        // Get the zones for both sides of the connection
        auto local_service = get_service();
        if (!local_service)
        {
            RPC_ERROR("trigger_network_failure: No local service available");
            CO_RETURN;
        }

        auto local_zone = local_service->get_zone_id();
        auto adjacent = get_adjacent_zone_id();

        RPC_INFO(
            "Triggering transport_down from local_zone={} to adjacent_zone={}", local_zone.get_val(), adjacent.get_val());

        // Notify our local service that the transport to the adjacent zone is down
        CO_AWAIT local_service->transport_down(rpc::get_version(), local_zone.as_destination(), adjacent.as_caller(), {});

        // Send transport_down message to the peer (if the connection is still alive)
        // This is done by sending the message through the queue
        if (!peer_cancel_received_.load(std::memory_order_acquire))
        {
            rpc::dodgy::transport_down_send msg{.encoding = rpc::encoding::yas_binary,
                .destination_zone_id = adjacent.get_val(),
                .caller_zone_id = local_zone.get_val(),
                .back_channel = {}};

            int ret = CO_AWAIT send_payload(rpc::get_version(), rpc::dodgy::message_direction::one_way, std::move(msg), 0);

            if (ret != rpc::error::OK())
            {
                RPC_WARNING("trigger_network_failure: Failed to send transport_down to peer");
            }
        }

        // Mark the transport as disconnected
        set_status(rpc::transport_status::DISCONNECTED);

        RPC_INFO("dodgy_transport::trigger_network_failure: Network failure simulation complete");
        CO_RETURN;
    }
}
