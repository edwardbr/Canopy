// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

// ws_client_connection.cpp
#include "ws_client_connection.h"

#include <coro/coro.hpp>

#include <canopy/network_config/network_args.h>

#include <fmt/format.h>
#include <secret_llama/secret_llama.h>
#include <secret_llama/secret_llama_stub.h>
#include <websocket_demo/websocket_demo.h>
#include "transport.h"

namespace websocket_demo
{
    namespace v1
    {
        namespace
        {
            rpc::zone make_socket_zone(const canopy::network_config::ip_address& client_addr, uint16_t port)
            {
#ifdef CANOPY_FIXED_ADDRESS_SIZE
                return rpc::zone(rpc::zone_address(canopy::network_config::ip_address_to_uint64(
                                                       client_addr, canopy::network_config::ip_address_family::ipv6),
                    port));
#else
                rpc::zone_address addr(client_addr, 64, 0);
                [[maybe_unused]] bool ok = addr.set_subnet(port);
                RPC_ASSERT(ok);
                return rpc::zone(addr);
#endif
            }
        } // namespace

        ws_client_connection::ws_client_connection(std::shared_ptr<ws_stream> ws, std::shared_ptr<websocket_service> service)
            : ws_(std::move(ws))
            , service_(std::move(service))
            , msg_buffer_(1024 * 1024, '\0')
        {
        }

        // -----------------------------------------------------------------------
        // run() helpers
        // -----------------------------------------------------------------------

        // Wait for the client's initial connect_request binary frame.
        // Returns false if the connection closed or timed out before the handshake.
        coro::task<bool> ws_client_connection::wait_for_handshake()
        {
            RPC_INFO("[WS] Waiting for connect_request handshake");
            auto [status, span] = co_await ws_->recv(msg_buffer_, std::chrono::seconds{5});
            if (!status.is_ok() || span.empty())
            {
                state_ = connection_state::closed;
                co_return false;
            }

            websocket_demo::v1::connect_request req;
            const auto* begin = reinterpret_cast<const uint8_t*>(span.data());
            auto parse_err = rpc::from_protobuf<websocket_demo::v1::connect_request>({begin, begin + span.size()}, req);

            if (!parse_err.empty())
            {
                ws_->queue_close(
                    WSLAY_CODE_INVALID_FRAME_PAYLOAD_DATA, fmt::format("invalid connect_request: {}", parse_err));
                state_ = connection_state::closed;
                co_return false;
            }

            inbound_remote_object_ = req.inbound_remote_object;
            state_ = connection_state::running;
            co_return true;
        }

        // Attach the remote zone, wire up the local calculator instance, and
        // send the connect_response back to the client.
        coro::task<bool> ws_client_connection::setup_zone()
        {
            auto peer = ws_->get_peer_info();
            canopy::network_config::ip_address client_addr;

            if (peer.family == canopy::network_config::ip_address_family::ipv4)
            {
                // Convert IPv4 to IPv6 using the 6to4 mapping (RFC 3056)
                const uint64_t prefix64 = canopy::network_config::ip_address_to_uint64(peer.addr, peer.family);
                client_addr = {};
                for (int i = 7; i >= 0; --i)
                {
                    client_addr[static_cast<size_t>(i)] = static_cast<uint8_t>(prefix64 >> (8 * (7 - i)));
                }
            }
            else
            {
                client_addr = peer.addr;
            }

            canopy::network_config::network_config tmp;
            tmp.host_addr = client_addr;
            tmp.host_family = canopy::network_config::ip_address_family::ipv6;
            RPC_INFO("[WS] New client connection from [{}]:{}", tmp.get_host_string(), peer.port);

            transport_ = std::make_shared<transport>(ws_, service_);
            auto adjacent_zone = inbound_remote_object_.as_zone();
            if (!adjacent_zone.is_set())
            {
                adjacent_zone = make_socket_zone(client_addr, peer.port);
            }
            transport_->set_adjacent_zone_id(adjacent_zone);

            RPC_INFO("[WS] connect_request received, inbound_remote_object={}", inbound_remote_object_.get_subnet());
            RPC_INFO("[WS] Calling attach_remote_zone");

            rpc::interface_descriptor output_descr;
            auto ret
                = CO_AWAIT service_->attach_remote_zone<websocket_demo::v1::i_context_event, websocket_demo::v1::i_calculator>(
                    "websocket",
                    transport_,
                    [&]()
                    {
                        rpc::connection_settings cs;
                        cs.inbound_interface_id = websocket_demo::v1::i_context_event::get_id(rpc::get_version());
                        cs.outbound_interface_id = websocket_demo::v1::i_calculator::get_id(rpc::get_version());
                        cs.input_zone_id
                            = transport_->get_adjacent_zone_id().with_object(inbound_remote_object_.get_object());
                        return cs;
                    }(),
                    output_descr,
                    [](const rpc::shared_ptr<websocket_demo::v1::i_context_event>& sink,
                        rpc::shared_ptr<websocket_demo::v1::i_calculator>& local,
                        const std::shared_ptr<rpc::service>& svc) -> coro::task<int>
                    {
                        auto wsrvc = std::static_pointer_cast<websocket_service>(svc);
                        local = wsrvc->get_demo_instance();
                        if (!local)
                        {
                            RPC_ERROR("[WS] get_demo_instance returned null");
                            co_return rpc::error::OBJECT_NOT_FOUND();
                        }
                        if (sink)
                        {
                            RPC_INFO("[WS] Calling set_callback");
                            CO_AWAIT local->set_callback(sink);
                            RPC_INFO("[WS] set_callback completed");
                        }
                        co_return 0;
                    });

            RPC_INFO(
                "[WS] attach_remote_zone returned: {}, server_object_id={}", ret, output_descr.get_object_id().get_val());

            // Queue the connect_response so the client knows the zone/object IDs.
            websocket_demo::v1::connect_response connect_resp;
            connect_resp.caller_zone_id = transport_->get_adjacent_zone_id();
            connect_resp.outbound_remote_object = output_descr.destination_zone_id;

            ws_->queue_message(rpc::to_protobuf<std::vector<uint8_t>>(connect_resp));
            RPC_INFO("[WS] connect_response queued: caller_zone={} outbound_remote_object={}",
                connect_resp.caller_zone_id.get_subnet(),
                connect_resp.outbound_remote_object.get_subnet());

            co_return true;
        }

        // Parse an incoming binary payload as an RPC envelope and dispatch it.
        void ws_client_connection::handle_envelope(const std::span<const char> payload)
        {
            websocket_demo::v1::envelope envelope;
            const auto* begin = reinterpret_cast<const uint8_t*>(payload.data());
            auto error = rpc::from_protobuf<websocket_demo::v1::envelope>({begin, begin + payload.size()}, envelope);
            if (!error.empty())
            {
                RPC_ERROR("Received message ({} bytes) parsing error: {}", payload.size(), error);
                ws_->queue_close(WSLAY_CODE_INVALID_FRAME_PAYLOAD_DATA, fmt::format("invalid message format {}", error));
                return;
            }

            if (envelope.message_type == rpc::id<websocket_demo::v1::request>::get(rpc::get_version()))
            {
                service_->spawn(transport_->stub_handle_send(std::move(envelope)));
                return;
            }

            if (envelope.message_type == rpc::id<websocket_demo::v1::response>::get(rpc::get_version()))
            {
                websocket_demo::v1::response response;
                auto resp_error = rpc::from_protobuf<websocket_demo::v1::response>(envelope.data, response);
                if (!resp_error.empty())
                {
                    ws_->queue_close(
                        WSLAY_CODE_INVALID_FRAME_PAYLOAD_DATA, fmt::format("invalid message format {}", resp_error));
                }
                return;
            }

            ws_->queue_close(WSLAY_CODE_INVALID_FRAME_PAYLOAD_DATA, "unknown message_type");
        }

        // -----------------------------------------------------------------------
        // Main coroutine
        // -----------------------------------------------------------------------

        coro::task<void> ws_client_connection::run()
        {
            try
            {
                if (!co_await wait_for_handshake())
                    co_return;

                if (!co_await setup_zone())
                    co_return;

                RPC_INFO("[WS] Entering WebSocket message loop");

                while (true)
                {
                    ws_->drain_pending();

                    bool want_read = ws_->wants_read();
                    bool want_write = ws_->wants_write();

                    if (!want_read && !want_write)
                    {
                        RPC_INFO("WebSocket connection closing normally");
                        break;
                    }

                    // Prioritise outgoing streaming data
                    if (want_write && !co_await ws_->do_send())
                        break;

                    if (want_read)
                    {
                        auto [status, span] = co_await ws_->recv(msg_buffer_, std::chrono::milliseconds{5});
                        if (status.is_closed())
                        {
                            RPC_INFO("Client disconnected");
                            break;
                        }
                        if (status.is_ok() && !span.empty())
                        {
                            handle_envelope(span);
                        }
                    }
                }

                RPC_INFO("WebSocket connection closed");
            }
            catch (const std::exception& e)
            {
                RPC_ERROR("Exception in ws_client_connection::run: {}", e.what());
            }

            co_return;
        }
    }
}
