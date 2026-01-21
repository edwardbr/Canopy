/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <tuple>
#include <rpc/rpc.h>

#include <transports/tcp/transport.h>

namespace rpc::tcp
{
    tcp_transport::tcp_transport(std::string name,
        std::shared_ptr<rpc::service> service,
        rpc::zone adjacent_zone_id,
        std::chrono::milliseconds timeout,
        coro::net::tcp::client client,
        connection_handler handler)
        : rpc::transport(name, service, adjacent_zone_id)
        , client_(std::move(client))
        , timeout_(timeout)
        , connection_handler_(std::move(handler))
    {
        set_status(rpc::transport_status::CONNECTING);
    }

    std::shared_ptr<tcp_transport> tcp_transport::create(std::string name,
        std::shared_ptr<rpc::service> service,
        rpc::zone adjacent_zone_id,
        std::chrono::milliseconds timeout,
        coro::net::tcp::client client,
        connection_handler handler)
    {
        auto transport = std::shared_ptr<tcp_transport>(
            new tcp_transport(name, service, adjacent_zone_id, timeout, std::move(client), std::move(handler)));

        // Set up the keep alive using member_ptr assignment
        transport->keep_alive_ = transport;

        return transport;
    }

    void tcp_transport::kill_connection()
    {
        RPC_DEBUG("kill_connection() closing socket");
        // Set the connection as disconnected
        set_status(rpc::transport_status::DISCONNECTED);
        // Close the socket to wake up any blocking poll operations
        client_.socket().shutdown();
        client_.socket().close();
    }

    // Connection handshake
    CORO_TASK(int)
    tcp_transport::connect(rpc::interface_descriptor input_descr, rpc::interface_descriptor& output_descr)
    {
        RPC_DEBUG("tcp_transport::connect zone={}", get_zone_id().get_val());

        // Create the init client channel request
        init_client_channel_response init_receive;
        int ret = CO_AWAIT call_peer(rpc::get_version(),
            init_client_channel_send{.caller_zone_id = input_descr.destination_zone_id.get_val(),
                .caller_object_id = input_descr.object_id.get_val(),
                .destination_zone_id = get_adjacent_zone_id().get_val(),
                .adjacent_zone_id = get_service()->get_zone_id().get_val()},
            init_receive);
        if (ret != rpc::error::OK())
        {
            RPC_ERROR("tcp_transport->call_peer init_client_channel_send failed {}", rpc::error::to_string(ret));
            CO_RETURN ret;
        }

        if (init_receive.err_code != rpc::error::OK())
        {
            RPC_ERROR("init_client_channel_send failed");
            CO_RETURN init_receive.err_code;
        }

        // Update the adjacent zone ID based on the response
        rpc::object output_object_id = {init_receive.destination_object_id};
        output_descr = rpc::interface_descriptor(output_object_id, get_adjacent_zone_id().as_destination());

        // Set the transport status to connected
        set_status(rpc::transport_status::CONNECTED);

        CO_RETURN rpc::error::OK();
    }

    CORO_TASK(int)
    tcp_transport::send(uint64_t protocol_version,
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
        RPC_DEBUG("tcp_transport::send zone={}", get_zone_id().get_val());

        // Check transport status
        if (get_status() != rpc::transport_status::CONNECTED)
        {
            RPC_ERROR("failed tcp_transport::send - not connected, status = {}", static_cast<int>(get_status()));
            CO_RETURN rpc::error::TRANSPORT_ERROR();
        }

        // Peer-to-peer: always transmit via TCP queue to peer
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
            RPC_ERROR("failed tcp_transport::send call_send");
            CO_RETURN ret;
        }

        out_buf_.swap(response.payload);
        out_back_channel.swap(response.back_channel);

        RPC_DEBUG("tcp_transport::send complete zone={}", get_zone_id().get_val());

        CO_RETURN response.err_code;
    }

    CORO_TASK(void)
    tcp_transport::post(uint64_t protocol_version,
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
        RPC_DEBUG("tcp_transport::post zone={}", get_zone_id().get_val());

        // Check transport status
        if (get_status() != rpc::transport_status::CONNECTED)
        {
            RPC_ERROR("failed tcp_transport::post - not connected, status = {}", static_cast<int>(get_status()));
            CO_RETURN;
        }

        // Send the post message using the internal send_payload method
        int ret = CO_AWAIT send_payload(protocol_version,
            message_direction::one_way, // Use one_way for fire-and-forget
            post_send{.encoding = encoding,
                .tag = tag,
                .caller_zone_id = caller_zone_id.get_val(),
                .destination_zone_id = destination_zone_id.get_val(),
                .object_id = object_id.get_val(),
                .interface_id = interface_id.get_val(),
                .method_id = method_id.get_val(),
                .payload = std::vector<char>((const char*)in_data.begin, (const char*)in_data.end),
                .back_channel = in_back_channel},
            0); // sequence number 0 for one-way messages

        if (ret != rpc::error::OK())
        {
            RPC_ERROR("failed tcp_transport::post send_payload");
        }

        CO_RETURN;
    }

    CORO_TASK(int)
    tcp_transport::try_cast(uint64_t protocol_version,
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
        RPC_DEBUG("tcp_transport::try_cast zone={}", get_zone_id().get_val());

        // Check transport status
        if (get_status() != rpc::transport_status::CONNECTED)
        {
            RPC_ERROR("failed tcp_transport::try_cast - not connected, status = {}", static_cast<int>(get_status()));
            CO_RETURN rpc::error::TRANSPORT_ERROR();
        }

        try_cast_receive response;
        int ret = CO_AWAIT call_peer(protocol_version,
            try_cast_send{.destination_zone_id = destination_zone_id.get_val(),
                .object_id = object_id.get_val(),
                .interface_id = interface_id.get_val(),
                .back_channel = in_back_channel},
            response);
        if (ret != rpc::error::OK())
        {
            RPC_ERROR("failed tcp_transport::try_cast call_peer");
            CO_RETURN ret;
        }

        RPC_DEBUG("tcp_transport::try_cast complete zone={}", get_zone_id().get_val());

        out_back_channel.swap(response.back_channel);
        CO_RETURN response.err_code;
    }

    CORO_TASK(int)
    tcp_transport::add_ref(uint64_t protocol_version,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id,
        rpc::caller_zone caller_zone_id,
        rpc::known_direction_zone known_direction_zone_id,
        rpc::add_ref_options build_out_param_channel,
        uint64_t& reference_count,
        const std::vector<rpc::back_channel_entry>& in_back_channel,
        std::vector<rpc::back_channel_entry>& out_back_channel)
    {
        RPC_DEBUG("tcp_transport::add_ref zone={}", get_zone_id().get_val());

        // Check transport status
        if (get_status() != rpc::transport_status::CONNECTED)
        {
            RPC_ERROR("failed tcp_transport::add_ref - not connected, status = {}", static_cast<int>(get_status()));
            CO_RETURN rpc::error::TRANSPORT_ERROR();
        }

        // Peer-to-peer: always transmit via TCP queue to peer
        // The peer transport will call inbound_add_ref for routing
        addref_receive response;
        int ret = CO_AWAIT call_peer(protocol_version,
            addref_send{.destination_zone_id = destination_zone_id.get_val(),
                .object_id = object_id.get_val(),
                .caller_zone_id = caller_zone_id.get_val(),
                .known_direction_zone_id = known_direction_zone_id.get_val(),
                .build_out_param_channel = build_out_param_channel,
                .back_channel = in_back_channel},
            response);
        if (ret != rpc::error::OK())
        {
            RPC_ERROR("failed tcp_transport::add_ref addref_send");
            CO_RETURN ret;
        }

        reference_count = response.ref_count;
        out_back_channel.swap(response.back_channel);
        if (response.err_code != rpc::error::OK())
        {
            RPC_ERROR("failed addref_receive.err_code failed");
#ifdef CANOPY_USE_TELEMETRY
            if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
            {
                auto error_message = std::string("add_ref failed ") + std::to_string(response.err_code);
                telemetry_service->message(rpc::i_telemetry_service::err, error_message.c_str());
            }
#endif
            RPC_ASSERT(false);
            CO_RETURN response.err_code;
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
        RPC_DEBUG("tcp_transport::add_ref complete zone={}", get_zone_id().get_val());

        CO_RETURN rpc::error::OK();
    }

    CORO_TASK(int)
    tcp_transport::release(uint64_t protocol_version,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id,
        rpc::caller_zone caller_zone_id,
        rpc::release_options options,
        uint64_t& reference_count,
        const std::vector<rpc::back_channel_entry>& in_back_channel,
        std::vector<rpc::back_channel_entry>& out_back_channel)
    {
        RPC_DEBUG("rpc_transport::release zone={}", get_zone_id().get_val());

        // Check transport status
        if (get_status() != rpc::transport_status::CONNECTED)
        {
            RPC_ERROR("failed tcp_transport::release - not connected, status = {}", static_cast<int>(get_status()));
            CO_RETURN rpc::error::TRANSPORT_ERROR();
        }

        // Peer-to-peer: always transmit via TCP queue to peer
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
            RPC_ERROR("failed tcp_transport::release release_send");
            CO_RETURN ret;
        }

        if (response.err_code != rpc::error::OK())
        {
            RPC_ERROR("failed response.err_code failed in release");
#ifdef CANOPY_USE_TELEMETRY
            if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
            {
                auto error_message = std::string("release failed ") + std::to_string(response.err_code);
                telemetry_service->message(rpc::i_telemetry_service::err, error_message.c_str());
            }
#endif
            RPC_ASSERT(false);
            CO_RETURN response.err_code;
        }

        RPC_DEBUG("tcp_transport::release complete zone={}", get_zone_id().get_val());

        reference_count = response.ref_count;
        out_back_channel.swap(response.back_channel);
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
    tcp_transport::object_released(uint64_t protocol_version,
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
        RPC_DEBUG("tcp_transport::object_released zone={}", get_zone_id().get_val());

        // Check transport status
        if (get_status() != rpc::transport_status::CONNECTED)
        {
            RPC_ERROR("failed tcp_transport::object_released - not connected, status = {}", static_cast<int>(get_status()));
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
            RPC_ERROR("failed tcp_transport::object_released send_payload");
        }

        RPC_DEBUG("tcp_transport::object_released complete zone={}", get_zone_id().get_val());
    }

    CORO_TASK(void)
    tcp_transport::transport_down(uint64_t protocol_version,
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
        RPC_DEBUG("tcp_transport::transport_down zone={}", get_zone_id().get_val());

        // Check transport status
        if (get_status() != rpc::transport_status::CONNECTED)
        {
            RPC_ERROR("failed tcp_transport::transport_down - not connected, status = {}", static_cast<int>(get_status()));
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
            RPC_ERROR("failed tcp_transport::transport_down send_payload");
        }

        RPC_DEBUG("tcp_transport::transport_down complete zone={}", get_zone_id().get_val());
    }

    CORO_TASK(void)
    tcp_transport::pump_send_and_receive()
    {
        RPC_DEBUG("pump_send_and_receive zone={}", get_service()->get_zone_id().get_val());
        assert(client_.socket().is_valid());

        // Cache service pointer to avoid accessing weak_ptr during shutdown
        auto service = get_service();
        if (!service)
        {
            RPC_ERROR("pump_send_and_receive: service is null");
            CO_RETURN;
        }

        // Message handler lambda that processes incoming messages

        CO_AWAIT pump_messages(
            [this, service](envelope_prefix prefix, envelope_payload payload) -> void
            {
                // Handle different message types
                if (payload.payload_fingerprint == rpc::id<init_client_channel_send>::get(prefix.version))
                {
                    assert(get_status() == rpc::transport_status::CONNECTING);
                    get_service()->get_scheduler()->spawn(create_stub(std::move(prefix), std::move(payload)));
                }
                else if (payload.payload_fingerprint == rpc::id<call_send>::get(prefix.version))
                {
                    assert(get_status() == rpc::transport_status::CONNECTED);
                    get_service()->get_scheduler()->spawn(stub_handle_send(std::move(prefix), std::move(payload)));
                }
                else if (payload.payload_fingerprint == rpc::id<try_cast_send>::get(prefix.version))
                {
                    assert(get_status() == rpc::transport_status::CONNECTED);
                    get_service()->get_scheduler()->spawn(stub_handle_try_cast(std::move(prefix), std::move(payload)));
                }
                else if (payload.payload_fingerprint == rpc::id<addref_send>::get(prefix.version))
                {
                    assert(get_status() == rpc::transport_status::CONNECTED);
                    get_service()->get_scheduler()->spawn(stub_handle_add_ref(std::move(prefix), std::move(payload)));
                }
                else if (payload.payload_fingerprint == rpc::id<release_send>::get(prefix.version))
                {
                    get_service()->get_scheduler()->spawn(stub_handle_release(std::move(prefix), std::move(payload)));
                }
                else if (payload.payload_fingerprint == rpc::id<post_send>::get(prefix.version))
                {
                    assert(get_status() == rpc::transport_status::CONNECTED);
                    get_service()->get_scheduler()->spawn(stub_handle_post(std::move(prefix), std::move(payload)));
                }
                else if (payload.payload_fingerprint == rpc::id<object_released_send>::get(prefix.version))
                {
                    assert(get_status() == rpc::transport_status::CONNECTED);
                    get_service()->get_scheduler()->spawn(
                        stub_handle_object_released(std::move(prefix), std::move(payload)));
                }
                else if (payload.payload_fingerprint == rpc::id<transport_down_send>::get(prefix.version))
                {
                    assert(get_status() == rpc::transport_status::CONNECTED);
                    get_service()->get_scheduler()->spawn(
                        stub_handle_transport_down(std::move(prefix), std::move(payload)));
                }
                else
                {
                    [[maybe_unused]] auto zone_id = get_service()->get_zone_id();
                    // now find the relevant event handler and set its values before triggering it
                    result_listener* result = nullptr;
                    {
                        std::scoped_lock lock(pending_transmits_mtx_);
                        auto it = pending_transmits_.find(prefix.sequence_number);
                        RPC_DEBUG("pending_transmits_ zone: {} sequence_number: {} id: {}",
                            zone_id.get_val(),
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
                        zone_id.get_val(),
                        rpc::to_yas_json<std::string>(prefix),
                        rpc::to_yas_json<std::string>(result->payload));

                    result->error_code = rpc::error::OK();
                    result->event.set();
                }
            });
    }

    CORO_TASK(void)
    tcp_transport::shutdown()
    {
        if (get_status() != rpc::transport_status::CONNECTED)
        {
            // Already shutting down
            RPC_DEBUG("shutdown() already in progress for zone {}", get_service()->get_zone_id().get_val());
            CO_RETURN;
        }

        RPC_DEBUG("shutdown() initiating for zone {}", get_service()->get_zone_id().get_val());

        // Wait for both tasks to complete
        CO_AWAIT shutdown_event_;

        // Set transport status to DISCONNECTED
        set_status(rpc::transport_status::DISCONNECTED);
        RPC_DEBUG("shutdown() completed for zone {}", get_service()->get_zone_id().get_val());
        CO_RETURN;
    }

    // Receive consumer task
    CORO_TASK(void)
    tcp_transport::pump_messages(std::function<void(envelope_prefix, envelope_payload)> incoming_message_handler)
    {
        static auto envelope_prefix_saved_size = rpc::yas_binary_saved_size(envelope_prefix());

        bool expecting_prefix = true;
        bool read_complete = true;
        envelope_prefix prefix{};
        std::vector<char> buf;
        std::span remaining_span(buf.begin(), buf.end());

        auto service = get_service();
        RPC_DEBUG("pump_messages started for zone {}", service->get_zone_id().get_val());

        // Main pump loop - check conditions in a thread-safe manner
        while (true)
        {
            // Check keep_alive_ using thread-safe member_ptr access
            auto keep_alive_copy = keep_alive_.get_nullable();

            // Check if we should continue the loop
            bool has_pending = pending_transmits_count_.load(std::memory_order_acquire) > 0;
            bool has_send_queue = send_queue_count_.load(std::memory_order_acquire) > 0;

            // Exit loop if no reason to continue
            if (!keep_alive_copy && !has_send_queue && !has_pending)
            {
                break;
            }

            // send any pending data
            if (has_send_queue)
            {
                auto scoped_lock = CO_AWAIT send_queue_mtx_.lock();
                bool failed = false;
                // Process send queue
                while (!send_queue_.empty() && !failed)
                {
                    auto& item = send_queue_.front();
                    auto marshal_status = client_.send(std::span{(const char*)item.data(), item.size()});
                    if (marshal_status.first == coro::net::send_status::try_again)
                    {
                        auto status = CO_AWAIT client_.poll(coro::poll_op::write);
                        if (status == coro::poll_status::timeout)
                        {
                            CO_AWAIT service->get_scheduler()->schedule();
                            break;
                        }
                        if (status != coro::poll_status::event)
                        {
                            failed = true;
                            break;
                        }

                        marshal_status = client_.send(std::span{(const char*)item.data(), item.size()});
                    }
                    send_queue_.pop();
                    send_queue_count_.fetch_sub(1, std::memory_order_release);
                    if (marshal_status.first != coro::net::send_status::ok)
                    {
                        RPC_ERROR("failed to send data to peer");
                        failed = true;
                        break;
                    }
                }

                if (failed)
                {
                    set_status(rpc::transport_status::DISCONNECTED);
                    break;
                }
            }
            // now poll the network for any incoming traffic
            if (read_complete)
            {
                size_t buf_size = expecting_prefix ? envelope_prefix_saved_size : prefix.payload_size;
                buf = std::vector<char>(buf_size, '\0');
                remaining_span = std::span(buf.begin(), buf.end());
                read_complete = false;
            }

            {
                auto [recv_status, recv_bytes] = client_.recv(remaining_span);

                if (recv_status == coro::net::recv_status::try_again || recv_status == coro::net::recv_status::would_block)
                {
                    auto pstatus = CO_AWAIT client_.poll(coro::poll_op::read, std::chrono::milliseconds(1));
                    if (pstatus == coro::poll_status::timeout)
                    {
                        CO_AWAIT service->get_scheduler()->schedule();
                        continue;
                    }
                    if (pstatus == coro::poll_status::error)
                    {
                        RPC_ERROR("failed pstatus == coro::poll_status::error");
                        break;
                    }

                    if (pstatus == coro::poll_status::closed)
                    {
                        RPC_ERROR("failed pstatus == coro::poll_status::closed");
                        break;
                    }
                    std::tie(recv_status, recv_bytes) = client_.recv(remaining_span);
                    if (recv_status == coro::net::recv_status::try_again
                        || recv_status == coro::net::recv_status::would_block)
                    {
                        CO_AWAIT service->get_scheduler()->schedule();
                        continue;
                    }
                }
                if (recv_status == coro::net::recv_status::ok)
                {
                    if (recv_bytes.size() != remaining_span.size())
                    {
                        remaining_span = std::span(remaining_span.begin() + recv_bytes.size(), remaining_span.end());
                        continue;
                    }
                    else
                    {
                        // buf is now fully proceed with reading the rest of the data with this message
                        read_complete = true;
                        if (expecting_prefix)
                        {
                            expecting_prefix = false;
                            auto str_err = rpc::from_yas_binary(rpc::span(buf), prefix);
                            if (!str_err.empty())
                            {
                                RPC_ERROR("failed invalid prefix");
                                break;
                            }
                            assert(prefix.direction);
                        }
                        else
                        {
                            expecting_prefix = true;

                            envelope_payload payload;
                            auto str_err = rpc::from_yas_binary(rpc::span(buf), payload);
                            if (!str_err.empty())
                            {
                                RPC_ERROR("failed bad payload format");
                                break;
                            }

                            incoming_message_handler(std::move(prefix), std::move(payload));

                            // Yield to allow scheduled tasks to run
                            CO_AWAIT service->get_scheduler()->schedule();
                        }
                    }
                }
                else
                {
                    RPC_ERROR("failed invalid received message");
                    break;
                }
            }
        }

        RPC_DEBUG("pump_messages exiting for zone {}", service->get_zone_id().get_val());

        // Close the socket now that we're done
        kill_connection();

        {
            std::scoped_lock lock(pending_transmits_mtx_);
            for (auto it : pending_transmits_)
            {
                it.second->error_code = rpc::error::CALL_CANCELLED();
                it.second->event.set();
            }
        }

        shutdown_event_.set();

        CO_RETURN;
    }

    // Stub handlers (server-side message processing)
    CORO_TASK(void) tcp_transport::stub_handle_send(envelope_prefix prefix, envelope_payload payload)
    {
        RPC_DEBUG("stub_handle_send");

        assert(prefix.direction == message_direction::send || prefix.direction == message_direction::one_way);

        call_send request;
        auto str_err = rpc::from_yas_compressed_binary(rpc::span(payload.payload), request);
        if (!str_err.empty())
        {
            RPC_ERROR("failed from_yas_compressed_binary call_send");
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

    CORO_TASK(void) tcp_transport::stub_handle_post(envelope_prefix prefix, envelope_payload payload)
    {
        RPC_DEBUG("stub_handle_post");

        post_send request;
        auto str_err = rpc::from_yas_compressed_binary(rpc::span(payload.payload), request);
        if (!str_err.empty())
        {
            RPC_ERROR("failed post_send from_yas_compressed_binary");
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

    CORO_TASK(void) tcp_transport::stub_handle_try_cast(envelope_prefix prefix, envelope_payload payload)
    {
        RPC_DEBUG("stub_handle_try_cast");

        try_cast_send request;
        auto str_err = rpc::from_yas_compressed_binary(rpc::span(payload.payload), request);
        if (!str_err.empty())
        {
            RPC_ERROR("failed try_cast_send from_yas_compressed_binary");
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

    CORO_TASK(void) tcp_transport::stub_handle_add_ref(envelope_prefix prefix, envelope_payload payload)
    {
        RPC_DEBUG("stub_handle_add_ref");

        addref_send request;
        auto str_err = rpc::from_yas_compressed_binary(rpc::span(payload.payload), request);
        if (!str_err.empty())
        {
            RPC_ERROR("failed addref_send from_yas_compressed_binary");
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

        auto err = CO_AWAIT send_payload(prefix.version,
            message_direction::receive,
            addref_receive{.ref_count = ref_count, .back_channel = std::move(out_back_channel), .err_code = ret},
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

    CORO_TASK(void) tcp_transport::stub_handle_release(envelope_prefix prefix, envelope_payload payload)
    {
        RPC_DEBUG("stub_handle_release");

        release_send request;
        auto str_err = rpc::from_yas_compressed_binary(rpc::span(payload.payload), request);
        if (!str_err.empty())
        {
            RPC_ERROR("failed release_send from_yas_compressed_binary");
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

        auto err = CO_AWAIT send_payload(prefix.version,
            message_direction::receive,
            release_receive{.ref_count = ref_count, .back_channel = std::move(out_back_channel), .err_code = ret},
            prefix.sequence_number);
        if (err != rpc::error::OK())
        {
            RPC_ERROR("failed release_send send_payload");
            kill_connection();
            CO_RETURN;
        }

        auto count = get_destination_count();
        RPC_ASSERT(count >= 0);
        if (count <= 0)
        {
            keep_alive_.reset();
        }
        RPC_DEBUG("release request complete");
        CO_RETURN;
    }

    CORO_TASK(void) tcp_transport::stub_handle_object_released(envelope_prefix prefix, envelope_payload payload)
    {
        RPC_DEBUG("stub_handle_object_released");

        object_released_send request;
        auto str_err = rpc::from_yas_compressed_binary(rpc::span(payload.payload), request);
        if (!str_err.empty())
        {
            RPC_ERROR("failed object_released_send from_yas_compressed_binary");
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

    CORO_TASK(void) tcp_transport::stub_handle_transport_down(envelope_prefix prefix, envelope_payload payload)
    {
        RPC_DEBUG("stub_handle_transport_down");

        transport_down_send request;
        auto str_err = rpc::from_yas_compressed_binary(rpc::span(payload.payload), request);
        if (!str_err.empty())
        {
            RPC_ERROR("failed transport_down_send from_yas_compressed_binary");
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

    CORO_TASK(void) tcp_transport::create_stub(envelope_prefix prefix, envelope_payload payload)
    {
        RPC_DEBUG("create_stub zone: {}", get_service()->get_zone_id().get_val());

        init_client_channel_send request;
        auto err = rpc::from_yas_compressed_binary(rpc::span(payload.payload), request);
        if (!err.empty())
        {
            RPC_ERROR("failed create_stub init_client_channel_send deserialization");
            CO_RETURN;
        }
        rpc::interface_descriptor input_descr{{request.caller_object_id}, {request.caller_zone_id}};
        rpc::interface_descriptor output_interface;

        // Update the adjacent zone ID from the handshake message
        // The server transport is initially created with zone{0}, but now we know the real client zone
        // Use adjacent_zone_id (the zone of the transport) not caller_zone_id (which may be different in pass-through)
        set_adjacent_zone_id(rpc::zone{request.adjacent_zone_id});

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
}
