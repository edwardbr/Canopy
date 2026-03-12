// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#include "http_client_connection.h"

#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <streaming/ws_stream.h>
#include <transports/streaming/transport.h>
#include <websocket_demo/websocket_demo.h>

namespace websocket_demo
{
    namespace v1
    {
        std::string calculate_ws_accept(std::string_view client_key)
        {
            std::string combined = std::string(client_key) + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

            unsigned char hash[SHA_DIGEST_LENGTH];
            SHA1(reinterpret_cast<const unsigned char*>(combined.c_str()), combined.size(), hash);

            BIO* bio;
            BIO* b64;
            BUF_MEM* buffer_ptr;

            b64 = BIO_new(BIO_f_base64());
            bio = BIO_new(BIO_s_mem());
            bio = BIO_push(b64, bio);

            BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
            BIO_write(bio, hash, SHA_DIGEST_LENGTH);
            BIO_flush(bio);
            BIO_get_mem_ptr(bio, &buffer_ptr);
            BIO_set_close(bio, BIO_NOCLOSE);
            BIO_free_all(bio);

            std::string result(buffer_ptr->data, buffer_ptr->length);
            BUF_MEM_free(buffer_ptr);
            return result;
        }

        std::string http_client_connection::build_websocket_handshake_response(const std::string& accept_key)
        {
            std::map<std::string, std::string> headers
                = {{"Upgrade", "websocket"}, {"Connection", "Upgrade"}, {"Sec-WebSocket-Accept", accept_key}};

            return build_http_response(101, "Switching Protocols", headers, "");
        }

        // wire a transport to a websocket
        auto http_client_connection::handle_websocket_upgrade(const http_request_context& ctx)
            -> coro::task<std::shared_ptr<rpc::stream_transport::transport>>
        {
            // sent the upgrade acknowlegement
            auto key_it = ctx.headers.find("Sec-WebSocket-Key");
            if (key_it == ctx.headers.end())
            {
                RPC_ERROR("Missing Sec-WebSocket-Key header");
                co_return nullptr;
            }

            std::string accept_key = calculate_ws_accept(key_it->second);
            std::string handshake_response = build_websocket_handshake_response(accept_key);

            auto wsstatus = co_await stream_->send(std::span<const char>{handshake_response});
            if (!wsstatus.is_ok())
            {
                RPC_ERROR("Failed to send WebSocket handshake response");
                co_return nullptr;
            }
            RPC_INFO("WebSocket handshake completed");

            // now we pin a transport to this websocket

            // first wrap it in a standard transport compatible stream class
            auto ws = std::make_shared<streaming::ws_stream>(stream_);
            auto wsrvc = std::static_pointer_cast<websocket_service>(service_);

            // Create the transport with the service and ws_stream
            auto transport = rpc::stream_transport::transport::create("websocket",
                service_,
                ws,
                [wsrvc](const rpc::connection_settings& input_descr,
                    rpc::interface_descriptor& output_descr,
                    std::shared_ptr<rpc::service> svc,
                    std::shared_ptr<rpc::stream_transport::transport> self_transport) -> coro::task<int>
                {
                    RPC_INFO("[WS] Client connecting, zone={}", input_descr.input_zone_id.get_subnet());
                    co_return CO_AWAIT svc
                        ->attach_remote_zone<websocket_demo::v1::i_context_event, websocket_demo::v1::i_calculator>(
                            "websocket",
                            self_transport,
                            input_descr,
                            output_descr,
                            [wsrvc](const rpc::shared_ptr<websocket_demo::v1::i_context_event>& sink,
                                rpc::shared_ptr<websocket_demo::v1::i_calculator>& local,
                                const std::shared_ptr<rpc::service>&) -> coro::task<int>
                            {
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
                                co_return rpc::error::OK();
                            });
                });

            // we do not want the demo to accept peer to peer connection requests that web clients should be sending
            transport->reject_message_type<rpc::stream_transport::init_client_channel_send>();

            // instead we have a custom connector that takes a websocket_demo::v1::connect_request which is used instead
            transport->add_typed_message_handler<websocket_demo::v1::connect_request>(
                [transport](auto,
                    rpc::stream_transport::envelope_prefix& prefix,
                    rpc::stream_transport::envelope_payload&,
                    websocket_demo::v1::connect_request& request) -> CORO_TASK(rpc::stream_transport::transport::message_hook_result)
                {
                    rpc::interface_descriptor output_descr;
                    auto ret = CO_AWAIT transport->run_custom_connect(request.inbound_remote_object,
                        websocket_demo::v1::i_context_event::get_id(prefix.version),
                        websocket_demo::v1::i_calculator::get_id(prefix.version),
                        output_descr);
                    if (ret != rpc::error::OK())
                    {
                        CO_RETURN rpc::stream_transport::transport::message_hook_result::rejected;
                    }

                    transport->send_custom_connect_response<websocket_demo::v1::connect_response>(prefix.version,
                        prefix.sequence_number,
                        request.inbound_remote_object.as_zone(),
                        output_descr.destination_zone_id);

                    CO_RETURN rpc::stream_transport::transport::message_hook_result::handled;
                });
            co_return transport;
        }
    }
}
