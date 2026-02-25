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

        // Set the transport status to connected
        transport->set_status(rpc::transport_status::CONNECTED);

        return transport;
    }

    void tcp_transport::set_status(transport_status new_status)
    {
        if (new_status == transport_status::DISCONNECTING)
        {
            disconnecting_since_ = std::chrono::steady_clock::now();
        }
        transport::set_status(new_status);
        // Socket close is deferred to pump_send_and_receive to avoid closing the socket
        // while pump_messages is still executing a send — that would cause an exception
        // in client_.send() on the now-closed socket.
    }

    // Connection handshake
    CORO_TASK(int)
    tcp_transport::inner_connect(connection_settings& input_descr, rpc::interface_descriptor& output_descr)
    {
        RPC_TRACE("tcp_transport::connect zone={}", get_zone_id().get_val());

        auto service = get_service();
        assert(connection_handler_ || !connection_handler_); // Can be null for client side

        service->get_scheduler()->spawn(pump_send_and_receive());

        // Create the init client channel request
        init_client_channel_response init_receive;
        int ret = CO_AWAIT call_peer(rpc::get_version(),
            init_client_channel_send{.caller_zone_id = get_zone_id().as_caller(),
                .caller_object_id = input_descr.object_id,
                .caller_interface_id = input_descr.caller_interface_id,
                .destination_zone_id = get_adjacent_zone_id().as_destination(),
                .destination_interface_id = input_descr.destination_interface_id,
                .adjacent_zone_id = get_zone_id()},
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
        output_descr
            = rpc::interface_descriptor(init_receive.destination_object_id, get_adjacent_zone_id().as_destination());

        CO_RETURN rpc::error::OK();
    }

    CORO_TASK(int)
    tcp_transport::outbound_send(uint64_t protocol_version,
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
        RPC_TRACE("tcp_transport::outbound_send zone={}", get_zone_id().get_val());

        // Check transport status
        if (get_status() != rpc::transport_status::CONNECTED)
        {
            RPC_ERROR("failed tcp_transport::outbound_send - not connected, status = {}", static_cast<int>(get_status()));
            CO_RETURN rpc::error::TRANSPORT_ERROR();
        }

        // Peer-to-peer: always transmit via TCP queue to peer
        // The peer transport will call inbound_send for routing
        call_receive response;
        int ret = CO_AWAIT call_peer(protocol_version,
            call_send{.encoding = encoding,
                .tag = tag,
                .caller_zone_id = caller_zone_id,
                .destination_zone_id = destination_zone_id,
                .object_id = object_id,
                .interface_id = interface_id,
                .method_id = method_id,
                .payload = std::vector<char>((const char*)in_data.begin, (const char*)in_data.end),
                .back_channel = in_back_channel},
            response);
        if (ret != rpc::error::OK())
        {
            RPC_ERROR("failed tcp_transport::outbound_send call_send");
            CO_RETURN ret;
        }

        out_buf_.swap(response.payload);
        out_back_channel.swap(response.back_channel);

        RPC_TRACE("tcp_transport::outbound_send complete zone={}", get_zone_id().get_val());

        CO_RETURN response.err_code;
    }

    CORO_TASK(void)
    tcp_transport::outbound_post(uint64_t protocol_version,
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
        RPC_TRACE("tcp_transport::outbound_post zone={}", get_zone_id().get_val());

        // Check transport status
        if (get_status() != rpc::transport_status::CONNECTED)
        {
            RPC_ERROR("failed tcp_transport::outbound_post - not connected, status = {}", static_cast<int>(get_status()));
            CO_RETURN;
        }

        // Send the post message using the internal send_payload method
        int ret = CO_AWAIT send_payload(protocol_version,
            message_direction::one_way, // Use one_way for fire-and-forget
            post_send{.encoding = encoding,
                .tag = tag,
                .caller_zone_id = caller_zone_id,
                .destination_zone_id = destination_zone_id,
                .object_id = object_id,
                .interface_id = interface_id,
                .method_id = method_id,
                .payload = std::vector<char>((const char*)in_data.begin, (const char*)in_data.end),
                .back_channel = in_back_channel},
            0); // sequence number 0 for one-way messages

        if (ret != rpc::error::OK())
        {
            RPC_ERROR("failed tcp_transport::outbound_post send_payload");
        }

        CO_RETURN;
    }

    CORO_TASK(int)
    tcp_transport::outbound_try_cast(uint64_t protocol_version,
        rpc::caller_zone caller_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id,
        const std::vector<rpc::back_channel_entry>& in_back_channel,
        std::vector<rpc::back_channel_entry>& out_back_channel)
    {
        RPC_TRACE("tcp_transport::outbound_try_cast zone={}", get_zone_id().get_val());

        // Check transport status
        if (get_status() != rpc::transport_status::CONNECTED)
        {
            RPC_ERROR(
                "failed tcp_transport::outbound_try_cast - not connected, status = {}", static_cast<int>(get_status()));
            CO_RETURN rpc::error::TRANSPORT_ERROR();
        }

        try_cast_receive response;
        int ret = CO_AWAIT call_peer(protocol_version,
            try_cast_send{.caller_zone_id = caller_zone_id,
                .destination_zone_id = destination_zone_id,
                .object_id = object_id,
                .interface_id = interface_id,
                .back_channel = in_back_channel},
            response);
        if (ret != rpc::error::OK())
        {
            RPC_ERROR("failed tcp_transport::outbound_try_cast call_peer");
            CO_RETURN ret;
        }

        RPC_TRACE("tcp_transport::outbound_try_cast complete zone={}", get_zone_id().get_val());

        out_back_channel.swap(response.back_channel);
        CO_RETURN response.err_code;
    }

    CORO_TASK(int)
    tcp_transport::outbound_add_ref(uint64_t protocol_version,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id,
        rpc::caller_zone caller_zone_id,
        rpc::known_direction_zone known_direction_zone_id,
        rpc::add_ref_options build_out_param_channel,
        const std::vector<rpc::back_channel_entry>& in_back_channel,
        std::vector<rpc::back_channel_entry>& out_back_channel)
    {
        RPC_TRACE("tcp_transport::outbound_add_ref zone={}", get_zone_id().get_val());

        // Check transport status
        if (get_status() != rpc::transport_status::CONNECTED)
        {
            RPC_ERROR(
                "failed tcp_transport::outbound_add_ref - not connected, status = {}", static_cast<int>(get_status()));
            CO_RETURN rpc::error::TRANSPORT_ERROR();
        }

        // Peer-to-peer: always transmit via TCP queue to peer
        // The peer transport will call inbound_add_ref for routing
        addref_receive response;
        int ret = CO_AWAIT call_peer(protocol_version,
            addref_send{.destination_zone_id = destination_zone_id,
                .object_id = object_id,
                .caller_zone_id = caller_zone_id,
                .known_direction_zone_id = known_direction_zone_id,
                .build_out_param_channel = build_out_param_channel,
                .back_channel = in_back_channel},
            response);
        if (ret != rpc::error::OK())
        {
            RPC_ERROR("failed tcp_transport::outbound_add_ref addref_send");
            CO_RETURN ret;
        }

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

        RPC_TRACE("tcp_transport::outbound_add_ref complete zone={}", get_zone_id().get_val());

        CO_RETURN rpc::error::OK();
    }

    CORO_TASK(int)
    tcp_transport::outbound_release(uint64_t protocol_version,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id,
        rpc::caller_zone caller_zone_id,
        rpc::release_options options,
        const std::vector<rpc::back_channel_entry>& in_back_channel,
        std::vector<rpc::back_channel_entry>& out_back_channel)
    {
        RPC_TRACE("rpc_transport::outbound_release zone={}", get_zone_id().get_val());

        // Allow during DISCONNECTING (cleanup notifications must go through); block only when DISCONNECTED
        if (get_status() >= rpc::transport_status::DISCONNECTED)
        {
            RPC_ERROR("failed tcp_transport::outbound_release - transport disconnected, status = {}",
                static_cast<int>(get_status()));
            CO_RETURN rpc::error::TRANSPORT_ERROR();
        }

        // Peer-to-peer: always transmit via TCP queue to peer
        // The peer transport will call inbound_release for routing
        release_receive response;
        int ret = CO_AWAIT call_peer(protocol_version,
            release_send{.destination_zone_id = destination_zone_id,
                .object_id = object_id,
                .caller_zone_id = caller_zone_id,
                .options = options,
                .back_channel = in_back_channel},
            response);
        if (ret != rpc::error::OK())
        {
            RPC_ERROR("failed tcp_transport::outbound_release release_send");
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
            CO_RETURN response.err_code;
        }

        RPC_TRACE("tcp_transport::outbound_release complete zone={}", get_zone_id().get_val());

        out_back_channel.swap(response.back_channel);

        CO_RETURN rpc::error::OK();
    }

    CORO_TASK(void)
    tcp_transport::outbound_object_released(uint64_t protocol_version,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id,
        rpc::caller_zone caller_zone_id,
        const std::vector<rpc::back_channel_entry>& in_back_channel)
    {
        RPC_TRACE("tcp_transport::outbound_object_released zone={}", get_zone_id().get_val());

        // Allow during DISCONNECTING (cleanup notifications must go through); block only when DISCONNECTED
        if (get_status() == rpc::transport_status::DISCONNECTED)
        {
            RPC_ERROR("failed tcp_transport::outbound_object_released - transport disconnected, status = {}",
                static_cast<int>(get_status()));
            CO_RETURN;
        }

        // Send the object_released message using the internal send_payload method (post-like behavior)
        int ret = CO_AWAIT send_payload(protocol_version,
            message_direction::one_way, // Use one_way for fire-and-forget
            object_released_send{.encoding = encoding::yas_binary,
                .destination_zone_id = destination_zone_id,
                .object_id = object_id,
                .caller_zone_id = caller_zone_id,
                .back_channel = in_back_channel},
            0); // sequence number 0 for one-way messages

        if (ret != rpc::error::OK())
        {
            RPC_ERROR("failed tcp_transport::outbound_object_released send_payload");
        }

        RPC_TRACE("tcp_transport::outbound_object_released complete zone={}", get_zone_id().get_val());
    }

    CORO_TASK(void)
    tcp_transport::outbound_transport_down(uint64_t protocol_version,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        const std::vector<rpc::back_channel_entry>& in_back_channel)
    {
        RPC_TRACE("tcp_transport::outbound_transport_down zone={}", get_zone_id().get_val());

        // Allow during DISCONNECTING (cleanup notifications must go through); block only when DISCONNECTED
        if (get_status() == rpc::transport_status::DISCONNECTED)
        {
            RPC_ERROR("failed tcp_transport::outbound_transport_down - transport disconnected, status = {}",
                static_cast<int>(get_status()));
            CO_RETURN;
        }

        // Send the transport_down message using the internal send_payload method (post-like behavior)
        int ret = CO_AWAIT send_payload(protocol_version,
            message_direction::one_way, // Use one_way for fire-and-forget
            transport_down_send{.encoding = encoding::yas_binary,
                .destination_zone_id = destination_zone_id,
                .caller_zone_id = caller_zone_id,
                .back_channel = in_back_channel},
            0); // sequence number 0 for one-way messages

        if (ret != rpc::error::OK())
        {
            RPC_ERROR("failed tcp_transport::outbound_transport_down send_payload");
        }

        RPC_TRACE("tcp_transport::outbound_transport_down complete zone={}", get_zone_id().get_val());
    }

    CORO_TASK(void)
    tcp_transport::pump_send_and_receive()
    {
        // CRITICAL: Keep transport alive for entire duration of pump
        auto self = shared_from_this();

        RPC_TRACE("pump_send_and_receive zone={}", get_service()->get_zone_id().get_val());
        assert(client_.socket().is_valid());

        // Guard against multiple calls
        bool expected = false;
        if (!pumps_started_.compare_exchange_strong(expected, true))
        {
            RPC_ERROR("pump_send_and_receive called MULTIPLE TIMES on zone {} - BUG!",
                get_service()->get_zone_id().get_val());
            CO_RETURN;
        }

        // Cache service pointer to avoid accessing weak_ptr during shutdown
        auto service = get_service();
        if (!service)
        {
            RPC_ERROR("pump_send_and_receive: service is null");
            CO_RETURN;
        }

        // Create activity tracker to keep transport and service alive during spawned tasks
        auto tracker = std::shared_ptr<activity_tracker>(
            new activity_tracker{.transport = std::static_pointer_cast<tcp_transport>(self), .svc = service});

        CO_AWAIT pump_messages(tracker);

        // Close the socket now that pump_messages has fully exited — it is safe to do so
        // here because no code path in pump_messages can race with this close.
        RPC_TRACE("Transport disconnected, closing socket for zone {}", service->get_zone_id().get_val());
        client_.socket().shutdown();
        client_.socket().close();

        // Ensure DISCONNECTED (idempotent)
        set_status(transport_status::DISCONNECTED);

        // Cancel any outstanding request-response calls
        {
            std::scoped_lock lock(pending_transmits_mtx_);
            for (auto& [seq, listener] : pending_transmits_)
            {
                listener->error_code = rpc::error::CALL_CANCELLED();
                listener->event.set();
            }
        }

        // Release the self-referential keep_alive — allows the transport to be destroyed
        keep_alive_.reset();

        CO_RETURN;
    }

    // Two-phase close: initiator task — sends cleanup releases then signals end
    CORO_TASK(void) tcp_transport::initiator_close_task(std::shared_ptr<activity_tracker> tracker)
    {
        auto self = shared_from_this();
        auto svc = get_service();
        RPC_TRACE("initiator_close_task: starting for zone {}", svc->get_zone_id().get_val());

        // Release all stubs/proxies to adjacent zone; pump_messages is still running to
        // process the outbound_release responses
        CO_AWAIT notify_all_destinations_of_disconnect();

        CO_AWAIT send_payload(rpc::get_version(), message_direction::one_way, close_connection_send{}, 0);

        RPC_TRACE("initiator_close_task: done for zone {}", svc->get_zone_id().get_val());
        CO_RETURN;
    }

    // Two-phase close: responder task — sends cleanup releases then acknowledges close
    CORO_TASK(void) tcp_transport::responder_close_task(std::shared_ptr<activity_tracker> tracker, uint64_t version)
    {
        auto self = shared_from_this();
        auto svc = get_service();
        RPC_TRACE("responder_close_task: starting for zone {}", svc->get_zone_id().get_val());

        // Release our stubs/proxies (may be a no-op if initiator already cleaned up)
        CO_AWAIT notify_all_destinations_of_disconnect();

        // Acknowledge the close so the peer's pump loop can exit
        CO_AWAIT send_payload(version, message_direction::one_way, close_connection_ack{}, 0);

        // Signal pump loop that responder cleanup is complete (used for exit condition)
        send_cleanup_done_.store(true, std::memory_order_release);

        RPC_TRACE("responder_close_task: done for zone {}", svc->get_zone_id().get_val());
        CO_RETURN;
    }

    // Main pump loop — handles both send and receive for the lifetime of this transport
    CORO_TASK(void) tcp_transport::pump_messages(std::shared_ptr<activity_tracker> tracker)
    {
        static auto envelope_prefix_saved_size = rpc::yas_binary_saved_size(envelope_prefix());

        bool expecting_prefix = true;
        bool read_complete = true;
        envelope_prefix prefix{};
        std::vector<char> buf;
        std::span remaining_span(buf.begin(), buf.end());

        auto service = get_service();
        RPC_TRACE("pump_messages started for zone {}", service->get_zone_id().get_val());

        while (get_status() < transport_status::DISCONNECTED)
        {
            auto status = get_status();

            // ── DISCONNECTING state management ───────────────────────────────
            RPC_TRACE("pump_messages: loop top zone={} status={} peer_req={} send_done={} send_q={} pending={}",
                service->get_zone_id().get_val(),
                static_cast<int>(status),
                peer_requested_disconnection_.load(),
                send_cleanup_done_.load(),
                send_queue_count_.load(),
                pending_transmits_count_.load());
            if (status == transport_status::DISCONNECTING)
            {
                // Spawn initiator close task once (only if we initiated, not the peer)
                bool already_spawned = initiator_close_spawned_.exchange(true);
                if (!already_spawned && !peer_requested_disconnection_)
                {
                    RPC_TRACE("pump_messages: spawning initiator_close_task for zone {}", service->get_zone_id().get_val());
                    initiator_task_spawned_.store(true, std::memory_order_release);
                    service->get_scheduler()->spawn(initiator_close_task(tracker));
                }

                // Responder exit: cleanup done + all messages sent + no pending calls.
                // Blocked in simultaneous-close (initiator_task_spawned_=true): both sides sent
                // close_connection_send and we must wait for the peer's close_connection_ack
                // (initiator exit path) rather than closing the socket prematurely — which would
                // cause SIGPIPE when the peer tries to send its close_connection_ack.
                if (peer_requested_disconnection_ && send_cleanup_done_.load(std::memory_order_acquire)
                    && send_queue_count_.load(std::memory_order_acquire) == 0
                    && pending_transmits_count_.load(std::memory_order_acquire) == 0
                    && !initiator_task_spawned_.load(std::memory_order_acquire))
                {
                    RPC_TRACE("pump_messages: responder shutdown complete for zone {}", service->get_zone_id().get_val());
                    break;
                }

                // Timeout: force exit if peer never responds
                auto elapsed = std::chrono::steady_clock::now() - disconnecting_since_;
                if (elapsed >= std::chrono::milliseconds(shutdown_timeout_ms_))
                {
                    RPC_WARNING(
                        "pump_messages: shutdown timeout for zone {}, forcing exit", service->get_zone_id().get_val());
                    break;
                }
            }
            else
            {
                // ── CONNECTED: exit if no more reason to run ─────────────────
                auto keep_alive_copy = keep_alive_.get_nullable();
                bool has_pending = pending_transmits_count_.load(std::memory_order_acquire) > 0;
                bool has_send_queue = send_queue_count_.load(std::memory_order_acquire) > 0;
                if (!keep_alive_copy && !has_send_queue && !has_pending)
                {
                    break;
                }
            }

            // ── Send pending data ────────────────────────────────────────────
            if (send_queue_count_.load(std::memory_order_acquire) > 0)
            {
                RPC_TRACE("pump_messages: acquiring send_queue_mtx_ zone={}", service->get_zone_id().get_val());
                auto scoped_lock = CO_AWAIT send_queue_mtx_.lock();
                RPC_TRACE("pump_messages: acquired send_queue_mtx_ zone={}", service->get_zone_id().get_val());
                // Re-check status after the lock acquire yield point — another coroutine
                // may have set DISCONNECTED while we were waiting for the lock.
                if (get_status() >= transport_status::DISCONNECTED)
                {
                    break;
                }
                bool failed = false;
                while (!send_queue_.empty() && !failed)
                {
                    auto& item = send_queue_.front();
                    auto marshal_status = client_.send(std::span{(const char*)item.data(), item.size()});
                    if (marshal_status.first == coro::net::send_status::try_again)
                    {
                        RPC_TRACE("pump_messages: waiting for write poll zone={}", service->get_zone_id().get_val());
                        auto wstatus = CO_AWAIT client_.poll(coro::poll_op::write);
                        RPC_TRACE("pump_messages: write poll returned {} zone={}",
                            static_cast<int>(wstatus),
                            service->get_zone_id().get_val());
                        if (wstatus == coro::poll_status::timeout)
                        {
                            CO_AWAIT service->get_scheduler()->schedule();
                            break;
                        }
                        if (wstatus != coro::poll_status::event)
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
                    break;
                }
            }

            // ── Receive data ─────────────────────────────────────────────────
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
                    if (pstatus == coro::poll_status::error || pstatus == coro::poll_status::closed)
                    {
                        RPC_TRACE("pump_messages: poll indicates disconnect for zone {}", service->get_zone_id().get_val());
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

                        // ── Dispatch message ─────────────────────────────────
                        if (payload.payload_fingerprint == rpc::id<close_connection_send>::get(prefix.version))
                        {
                            RPC_TRACE("pump_messages: received close_connection_send for zone {}",
                                service->get_zone_id().get_val());
                            peer_requested_disconnection_ = true;
                            if (get_status() != transport_status::DISCONNECTING)
                            {
                                set_status(transport_status::DISCONNECTING);
                            }
                            service->get_scheduler()->spawn(responder_close_task(tracker, prefix.version));
                        }
                        else if (payload.payload_fingerprint == rpc::id<close_connection_ack>::get(prefix.version))
                        {
                            RPC_TRACE("pump_messages: received close_connection_ack — initiator exit for zone {}",
                                service->get_zone_id().get_val());
                            break; // initiator path: done
                        }
                        else if (payload.payload_fingerprint == rpc::id<init_client_channel_send>::get(prefix.version))
                        {
                            service->get_scheduler()->spawn(create_stub(tracker, std::move(prefix), std::move(payload)));
                        }
                        else if (payload.payload_fingerprint == rpc::id<call_send>::get(prefix.version))
                        {
                            service->get_scheduler()->spawn(
                                stub_handle_send(tracker, std::move(prefix), std::move(payload)));
                        }
                        else if (payload.payload_fingerprint == rpc::id<try_cast_send>::get(prefix.version))
                        {
                            service->get_scheduler()->spawn(
                                stub_handle_try_cast(tracker, std::move(prefix), std::move(payload)));
                        }
                        else if (payload.payload_fingerprint == rpc::id<addref_send>::get(prefix.version))
                        {
                            service->get_scheduler()->spawn(
                                stub_handle_add_ref(tracker, std::move(prefix), std::move(payload)));
                        }
                        else if (payload.payload_fingerprint == rpc::id<release_send>::get(prefix.version))
                        {
                            service->get_scheduler()->spawn(
                                stub_handle_release(tracker, std::move(prefix), std::move(payload)));
                        }
                        else if (payload.payload_fingerprint == rpc::id<post_send>::get(prefix.version))
                        {
                            service->get_scheduler()->spawn(
                                stub_handle_post(tracker, std::move(prefix), std::move(payload)));
                        }
                        else if (payload.payload_fingerprint == rpc::id<object_released_send>::get(prefix.version))
                        {
                            service->get_scheduler()->spawn(
                                stub_handle_object_released(tracker, std::move(prefix), std::move(payload)));
                        }
                        else if (payload.payload_fingerprint == rpc::id<transport_down_send>::get(prefix.version))
                        {
                            service->get_scheduler()->spawn(
                                stub_handle_transport_down(tracker, std::move(prefix), std::move(payload)));
                        }
                        else
                        {
                            result_listener* result = nullptr;
                            {
                                std::scoped_lock lock(pending_transmits_mtx_);
                                auto it = pending_transmits_.find(prefix.sequence_number);
                                RPC_TRACE("pending_transmits_ zone: {} sequence_number: {} id: {}",
                                    service->get_zone_id().get_val(),
                                    prefix.sequence_number,
                                    payload.payload_fingerprint);
                                if (it != pending_transmits_.end())
                                {
                                    result = it->second;
                                    pending_transmits_.erase(it);
                                    pending_transmits_count_.fetch_sub(1, std::memory_order_release);
                                }
                                else
                                {
                                    RPC_WARNING("No pending transmit for sequence_number: {} id: {}",
                                        prefix.sequence_number,
                                        payload.payload_fingerprint);
                                }
                            }
                            if (result)
                            {
                                result->prefix = std::move(prefix);
                                result->payload = std::move(payload);
                                result->error_code = rpc::error::OK();
                                result->event.set();
                            }
                        }

                        // Yield to allow spawned tasks to run
                        CO_AWAIT service->get_scheduler()->schedule();
                        RPC_TRACE("pump_messages: resumed from schedule zone={} status={}",
                            service->get_zone_id().get_val(),
                            static_cast<int>(get_status()));
                    }
                }
                else if (recv_status == coro::net::recv_status::closed)
                {
                    RPC_INFO("tcp transport connection closed for zone {}", service->get_zone_id().get_val());
                    break;
                }
                else
                {
                    RPC_ERROR("failed invalid received message {}", coro::net::to_string(recv_status));
                    break;
                }
            }
        }

        RPC_TRACE("pump_messages exiting for zone {}", service->get_zone_id().get_val());
        CO_RETURN;
    }

    // Stub handlers (server-side message processing)
    CORO_TASK(void)
    tcp_transport::stub_handle_send(std::shared_ptr<activity_tracker>, envelope_prefix prefix, envelope_payload payload)
    {
        RPC_TRACE("stub_handle_send");

        assert(prefix.direction == message_direction::send || prefix.direction == message_direction::one_way);

        call_send request;
        auto str_err = rpc::from_yas_compressed_binary(rpc::span(payload.payload), request);
        if (!str_err.empty())
        {
            RPC_ERROR("failed from_yas_compressed_binary call_send");
            set_status(transport_status::DISCONNECTING);
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
            request.object_id,
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

        auto err = CO_AWAIT send_payload(prefix.version,
            message_direction::receive,
            call_receive{.payload = std::move(out_buf), .back_channel = std::move(out_back_channel), .err_code = ret},
            prefix.sequence_number);
        if (err != rpc::error::OK())
        {
            RPC_ERROR("failed send_payload");
            set_status(transport_status::DISCONNECTING);
            CO_RETURN;
        }
        RPC_TRACE("send request complete");
        CO_RETURN;
    }

    CORO_TASK(void)
    tcp_transport::stub_handle_post(std::shared_ptr<activity_tracker>, envelope_prefix prefix, envelope_payload payload)
    {
        RPC_TRACE("stub_handle_post");

        post_send request;
        auto str_err = rpc::from_yas_compressed_binary(rpc::span(payload.payload), request);
        if (!str_err.empty())
        {
            RPC_ERROR("failed post_send from_yas_compressed_binary");
            set_status(transport_status::DISCONNECTING);
            CO_RETURN;
        }

        // Call inbound_post for routing - transport will route to correct destination
        CO_AWAIT inbound_post(prefix.version,
            request.encoding,
            request.tag,
            request.caller_zone_id,
            request.destination_zone_id,
            request.object_id,
            request.interface_id,
            request.method_id,
            request.payload,
            request.back_channel);

        // No response needed for post operations (fire-and-forget)
        RPC_TRACE("stub_handle_post complete");
        CO_RETURN;
    }

    CORO_TASK(void)
    tcp_transport::stub_handle_try_cast(std::shared_ptr<activity_tracker>, envelope_prefix prefix, envelope_payload payload)
    {
        RPC_TRACE("stub_handle_try_cast");

        try_cast_send request;
        auto str_err = rpc::from_yas_compressed_binary(rpc::span(payload.payload), request);
        if (!str_err.empty())
        {
            RPC_ERROR("failed try_cast_send from_yas_compressed_binary");
            set_status(transport_status::DISCONNECTING);
            CO_RETURN;
        }

        std::vector<rpc::back_channel_entry> out_back_channel;
        // Call inbound_try_cast for routing - transport will route to correct destination
        auto ret = CO_AWAIT inbound_try_cast(prefix.version,
            request.caller_zone_id,
            request.destination_zone_id,
            request.object_id,
            request.interface_id,
            request.back_channel,
            out_back_channel);

        if (rpc::error::is_error(ret))
        {
            RPC_TRACE("inbound_send error {}", ret);
        }

        auto err = CO_AWAIT send_payload(prefix.version,
            message_direction::receive,
            try_cast_receive{.back_channel = std::move(out_back_channel), .err_code = ret},
            prefix.sequence_number);
        if (err != rpc::error::OK())
        {
            RPC_ERROR("failed try_cast_send send_payload");
            set_status(transport_status::DISCONNECTING);
            CO_RETURN;
        }
        RPC_TRACE("stub_handle_try_cast complete");
        CO_RETURN;
    }

    CORO_TASK(void)
    tcp_transport::stub_handle_add_ref(std::shared_ptr<activity_tracker>, envelope_prefix prefix, envelope_payload payload)
    {
        RPC_TRACE("stub_handle_add_ref");

        addref_send request;
        auto str_err = rpc::from_yas_compressed_binary(rpc::span(payload.payload), request);
        if (!str_err.empty())
        {
            RPC_ERROR("failed addref_send from_yas_compressed_binary");
            set_status(transport_status::DISCONNECTING);
            CO_RETURN;
        }

        std::vector<rpc::back_channel_entry> out_back_channel;
        // Call inbound_add_ref for routing - transport will route to correct destination
        auto ret = CO_AWAIT inbound_add_ref(prefix.version,
            request.destination_zone_id,
            request.object_id,
            request.caller_zone_id,
            request.known_direction_zone_id,
            request.build_out_param_channel,
            request.back_channel,
            out_back_channel);

        if (rpc::error::is_error(ret))
        {
            RPC_DEBUG("inbound_add_ref error {}", ret);
        }

        auto err = CO_AWAIT send_payload(prefix.version,
            message_direction::receive,
            addref_receive{.back_channel = std::move(out_back_channel), .err_code = ret},
            prefix.sequence_number);
        if (err != rpc::error::OK())
        {
            RPC_ERROR("failed addref_send send_payload");
            set_status(transport_status::DISCONNECTING);
            CO_RETURN;
        }
        RPC_TRACE("add_ref request complete");
        CO_RETURN;
    }

    CORO_TASK(void)
    tcp_transport::stub_handle_release(std::shared_ptr<activity_tracker>, envelope_prefix prefix, envelope_payload payload)
    {
        RPC_TRACE("stub_handle_release");

        release_send request;
        auto str_err = rpc::from_yas_compressed_binary(rpc::span(payload.payload), request);
        if (!str_err.empty())
        {
            RPC_ERROR("failed release_send from_yas_compressed_binary");
            set_status(transport_status::DISCONNECTING);
            CO_RETURN;
        }

        std::vector<rpc::back_channel_entry> out_back_channel;
        // Call inbound_release for routing - transport will route to correct destination
        auto ret = CO_AWAIT inbound_release(prefix.version,
            request.destination_zone_id,
            request.object_id,
            request.caller_zone_id,
            request.options,
            request.back_channel,
            out_back_channel);

        if (rpc::error::is_error(ret))
        {
            RPC_DEBUG("inbound_release error {}", ret);
        }

        auto err = CO_AWAIT send_payload(prefix.version,
            message_direction::receive,
            release_receive{.back_channel = std::move(out_back_channel), .err_code = ret},
            prefix.sequence_number);
        if (err != rpc::error::OK())
        {
            RPC_ERROR("failed release_send send_payload");
            set_status(transport_status::DISCONNECTING);
            CO_RETURN;
        }

        auto count = get_destination_count();
        RPC_ASSERT(count >= 0);
        if (count <= 0)
        {
            RPC_TRACE("destination_count reached 0, triggering disconnect");
            set_status(rpc::transport_status::DISCONNECTING);
        }
        RPC_TRACE("release request complete");
        CO_RETURN;
    }

    CORO_TASK(void)
    tcp_transport::stub_handle_object_released(
        std::shared_ptr<activity_tracker>, envelope_prefix prefix, envelope_payload payload)
    {
        RPC_TRACE("stub_handle_object_released");

        object_released_send request;
        auto str_err = rpc::from_yas_compressed_binary(rpc::span(payload.payload), request);
        if (!str_err.empty())
        {
            RPC_ERROR("failed object_released_send from_yas_compressed_binary");
            set_status(transport_status::DISCONNECTING);
            CO_RETURN;
        }

        // Call inbound_object_released for routing - transport will route to correct destination
        CO_AWAIT inbound_object_released(
            prefix.version, request.destination_zone_id, request.object_id, request.caller_zone_id, request.back_channel);

        // No response needed for object_released (fire-and-forget)
        RPC_TRACE("stub_handle_object_released complete");
        CO_RETURN;
    }

    CORO_TASK(void)
    tcp_transport::stub_handle_transport_down(
        std::shared_ptr<activity_tracker>, envelope_prefix prefix, envelope_payload payload)
    {
        RPC_TRACE("stub_handle_transport_down");

        transport_down_send request;
        auto str_err = rpc::from_yas_compressed_binary(rpc::span(payload.payload), request);
        if (!str_err.empty())
        {
            RPC_ERROR("failed transport_down_send from_yas_compressed_binary");
            set_status(transport_status::DISCONNECTING);
            CO_RETURN;
        }

        // Call inbound_transport_down for routing - transport will route to correct destination
        CO_AWAIT inbound_transport_down(
            prefix.version, request.destination_zone_id, request.caller_zone_id, request.back_channel);

        // No response needed for transport_down (fire-and-forget)
        RPC_TRACE("stub_handle_transport_down complete");
        CO_RETURN;
    }

    CORO_TASK(void)
    tcp_transport::create_stub(std::shared_ptr<activity_tracker>, envelope_prefix prefix, envelope_payload payload)
    {
        RPC_TRACE("create_stub zone: {}", get_service()->get_zone_id().get_val());

        init_client_channel_send request;
        auto err = rpc::from_yas_compressed_binary(rpc::span(payload.payload), request);
        if (!err.empty())
        {
            RPC_ERROR("failed create_stub init_client_channel_send deserialization");
            CO_RETURN;
        }
        rpc::connection_settings input_descr{.caller_interface_id = request.caller_interface_id,
            .destination_interface_id = request.destination_interface_id,
            .object_id = request.caller_object_id,
            .input_zone_id = request.caller_zone_id.as_destination()};
        rpc::interface_descriptor output_interface;

        // Update the adjacent zone ID from the handshake message
        // The server transport is initially created with zone{0}, but now we know the real client zone
        // Use adjacent_zone_id (the zone of the transport) not caller_zone_id (which may be different in pass-through)
        set_adjacent_zone_id(request.adjacent_zone_id);

        int ret = CO_AWAIT connection_handler_(input_descr, output_interface, get_service(), keep_alive_.get_nullable());
        connection_handler_ = nullptr;
        if (ret != rpc::error::OK())
        {
            set_status(rpc::transport_status::DISCONNECTED);
            RPC_ERROR("failed to connect to zone {}", ret);
            CO_RETURN;
        }

        auto send_err = CO_AWAIT send_payload(prefix.version,
            message_direction::receive,
            init_client_channel_response{.err_code = rpc::error::OK(),
                .destination_zone_id = output_interface.destination_zone_id,
                .destination_object_id = output_interface.object_id,
                .caller_zone_id = input_descr.input_zone_id.as_caller()},
            prefix.sequence_number);
        if (send_err != rpc::error::OK())
        {
            RPC_ERROR("failed to send init_client_channel_response");
        }

        CO_RETURN;
    }
}
