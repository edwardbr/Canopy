// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#include <canopy/http_server/http_client_connection.h>

#include <algorithm>

#include <fmt/format.h>
#include <llhttp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <streaming/websocket/stream.h>

namespace canopy::http_server
{
    namespace
    {
        auto calculate_ws_accept(std::string_view client_key) -> std::string
        {
            std::string combined = std::string(client_key) + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

            unsigned char hash[SHA_DIGEST_LENGTH];
            SHA1(reinterpret_cast<const unsigned char*>(combined.c_str()), combined.size(), hash);

            BIO* bio = BIO_new(BIO_s_mem());
            BIO* b64 = BIO_new(BIO_f_base64());
            bio = BIO_push(b64, bio);

            BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
            BIO_write(bio, hash, SHA_DIGEST_LENGTH);
            BIO_flush(bio);

            BUF_MEM* buffer_ptr = nullptr;
            BIO_get_mem_ptr(bio, &buffer_ptr);

            std::string result(buffer_ptr->data, buffer_ptr->length);
            BIO_free_all(bio);
            return result;
        }

        auto default_is_rest_request(const request& request) -> bool
        {
            return request_path(request.url).starts_with("/api/");
        }
    } // namespace

    client_connection::client_connection(std::shared_ptr<streaming::stream> stream, handler_set handlers)
        : stream_(std::move(stream))
        , handlers_(std::move(handlers))
    {
    }

    int client_connection::on_method(llhttp_t* parser, const char* at, size_t length)
    {
        auto* ctx = static_cast<parser_request_context*>(parser->data);
        ctx->parsed_request.method.append(at, length);
        return 0;
    }

    int client_connection::on_url(llhttp_t* parser, const char* at, size_t length)
    {
        auto* ctx = static_cast<parser_request_context*>(parser->data);
        ctx->parsed_request.url.append(at, length);
        return 0;
    }

    int client_connection::on_header_field(llhttp_t* parser, const char* at, size_t length)
    {
        auto* ctx = static_cast<parser_request_context*>(parser->data);

        if (ctx->reading_header_value)
        {
            flush_header(*ctx);
        }

        ctx->current_header_field.append(at, length);
        ctx->reading_header_value = false;
        return 0;
    }

    int client_connection::on_header_value(llhttp_t* parser, const char* at, size_t length)
    {
        auto* ctx = static_cast<parser_request_context*>(parser->data);
        ctx->current_header_value.append(at, length);
        ctx->reading_header_value = true;
        return 0;
    }

    int client_connection::on_headers_complete(llhttp_t* parser)
    {
        auto* ctx = static_cast<parser_request_context*>(parser->data);
        flush_header(*ctx);
        ctx->headers_complete = true;
        return 0;
    }

    int client_connection::on_body(llhttp_t* parser, const char* at, size_t length)
    {
        auto* ctx = static_cast<parser_request_context*>(parser->data);
        ctx->parsed_request.body.append(at, length);
        return 0;
    }

    int client_connection::on_message_complete(llhttp_t* parser)
    {
        auto* ctx = static_cast<parser_request_context*>(parser->data);
        ctx->message_complete = true;
        return 0;
    }

    void client_connection::flush_header(parser_request_context& ctx)
    {
        if (ctx.current_header_field.empty())
        {
            return;
        }

        ctx.parsed_request.headers[ctx.current_header_field] = ctx.current_header_value;
        ctx.current_header_field.clear();
        ctx.current_header_value.clear();
    }

    auto client_connection::build_http_response(const response& input, bool keep_alive) -> std::string
    {
        response output = input;
        if (output.status_text.empty())
        {
            output.status_text = status_text(output.status_code);
        }

        if (output.headers.find("Connection") == output.headers.end())
        {
            output.headers["Connection"] = keep_alive ? "keep-alive" : "close";
        }

        if (!output.body.empty() && output.headers.find("Content-Length") == output.headers.end())
        {
            output.headers["Content-Length"] = std::to_string(output.body.size());
        }

        std::string wire_response = fmt::format("HTTP/1.1 {} {}\r\n", output.status_code, output.status_text);
        for (const auto& [key, value] : output.headers)
        {
            wire_response += fmt::format("{}: {}\r\n", key, value);
        }

        wire_response += "\r\n";
        wire_response += output.body;
        return wire_response;
    }

    auto client_connection::build_websocket_handshake_response(const std::string& accept_key) -> std::string
    {
        response handshake;
        handshake.status_code = 101;
        handshake.status_text = status_text(101);
        handshake.headers = {{"Upgrade", "websocket"}, {"Connection", "Upgrade"}, {"Sec-WebSocket-Accept", accept_key}};
        return build_http_response(handshake, false);
    }

    auto client_connection::dispatch_request(const request& request) const -> std::optional<response>
    {
        const auto is_rest_request
            = handlers_.is_rest_request ? handlers_.is_rest_request(request) : default_is_rest_request(request);

        if (is_rest_request)
        {
            if (handlers_.rest_handler)
            {
                return handlers_.rest_handler(request);
            }
            return make_text_response(404, "Not Found");
        }

        if (handlers_.webpage_handler)
        {
            return handlers_.webpage_handler(request);
        }

        return make_text_response(404, "Not Found");
    }

    auto client_connection::handle_websocket_upgrade(const request& request) -> coro::task<std::shared_ptr<rpc::transport>>
    {
        auto key_it = request.headers.find("Sec-WebSocket-Key");
        if (key_it == request.headers.end())
        {
            RPC_ERROR("Missing Sec-WebSocket-Key header");
            co_return nullptr;
        }

        if (!handlers_.websocket_upgrade_handler)
        {
            RPC_ERROR("No websocket upgrade handler configured");
            auto error_response = build_http_response(make_text_response(501, "Not Implemented"), false);
            co_await stream_->send(rpc::byte_span{error_response});
            co_return nullptr;
        }

        std::string accept_key = calculate_ws_accept(key_it->second);
        auto handshake_response = build_websocket_handshake_response(accept_key);

        auto wsstatus = co_await stream_->send(rpc::byte_span{handshake_response});
        if (!wsstatus.is_ok())
        {
            RPC_ERROR("Failed to send WebSocket handshake response");
            co_return nullptr;
        }

        RPC_INFO("WebSocket handshake completed");
        auto ws_stream = std::make_shared<streaming::websocket::stream>(stream_);
        co_return CO_AWAIT handlers_.websocket_upgrade_handler(request, ws_stream);
    }

    auto client_connection::handle() -> coro::task<std::shared_ptr<rpc::transport>>
    {
        std::string receive_buffer(8192, '\0');
        std::string pending_input;

        try
        {
            llhttp_t parser;
            llhttp_settings_t settings;

            llhttp_settings_init(&settings);
            settings.on_method = on_method;
            settings.on_url = on_url;
            settings.on_header_field = on_header_field;
            settings.on_header_value = on_header_value;
            settings.on_headers_complete = on_headers_complete;
            settings.on_body = on_body;
            settings.on_message_complete = on_message_complete;

            llhttp_init(&parser, HTTP_REQUEST, &settings);

            while (true)
            {
                parser_request_context ctx;
                llhttp_reset(&parser);
                parser.data = &ctx;

                bool is_websocket_upgrade = false;
                while (!ctx.message_complete)
                {
                    if (pending_input.empty())
                    {
                        auto [read_status, read_span] = co_await stream_->receive(
                            rpc::mutable_byte_span{receive_buffer.data(), receive_buffer.size()});
                        if (!read_status.is_ok() || read_span.empty())
                        {
                            RPC_ERROR("Failed to read HTTP request");
                            co_return nullptr;
                        }

                        std::string preview;
                        for (size_t i = 0; i < std::min<size_t>(100, read_span.size()); ++i)
                        {
                            unsigned char c = static_cast<unsigned char>(read_span.data()[i]);
                            if (c >= 32 && c < 127)
                            {
                                preview += static_cast<char>(c);
                            }
                            else
                            {
                                preview += fmt::format("\\x{:02x}", static_cast<int>(c));
                            }
                        }
                        RPC_DEBUG("Received {} bytes, first bytes: {}", read_span.size(), preview);

                        pending_input.append(reinterpret_cast<const char*>(read_span.data()), read_span.size());
                    }

                    auto err = llhttp_execute(&parser, pending_input.data(), pending_input.size());
                    const char* error_pos = llhttp_get_error_pos(&parser);
                    size_t consumed
                        = error_pos ? static_cast<size_t>(error_pos - pending_input.data()) : pending_input.size();

                    if (err == HPE_PAUSED_UPGRADE)
                    {
                        is_websocket_upgrade = true;
                        llhttp_resume_after_upgrade(&parser);
                        pending_input.erase(0, consumed);
                        break;
                    }

                    if (err != HPE_OK)
                    {
                        RPC_ERROR("HTTP parse error: {}", llhttp_errno_name(err));
                        auto error_response = build_http_response(make_text_response(400, "Bad Request"), false);
                        co_await stream_->send(rpc::byte_span{error_response});
                        co_return nullptr;
                    }

                    pending_input.erase(0, consumed);
                }

                ctx.parsed_request.keep_alive = llhttp_should_keep_alive(&parser) != 0;

                if (is_websocket_upgrade)
                {
                    co_return CO_AWAIT handle_websocket_upgrade(ctx.parsed_request);
                }

                auto method_name = llhttp_method_name(static_cast<llhttp_method_t>(parser.method));
                if (ctx.parsed_request.method.empty() && method_name)
                {
                    ctx.parsed_request.method = method_name;
                }

                const auto path = request_path(ctx.parsed_request.url);
                RPC_INFO("HTTP {} request for: {}", ctx.parsed_request.method, path);

                auto response = dispatch_request(ctx.parsed_request).value_or(make_text_response(404, "Not Found"));
                auto wire_response = build_http_response(response, ctx.parsed_request.keep_alive);

                auto send_status = co_await stream_->send(rpc::byte_span{wire_response});
                if (!send_status.is_ok())
                {
                    RPC_ERROR("Failed to send HTTP response for: {}", path);
                    co_return nullptr;
                }

                if (!ctx.parsed_request.keep_alive)
                {
                    co_return nullptr;
                }
            }
        }
        catch (const std::exception& e)
        {
            RPC_ERROR("Exception in client_connection::handle: {}", e.what());
        }

        co_return nullptr;
    }

    auto status_text(int status_code) -> std::string
    {
        switch (status_code)
        {
        case 101:
            return "Switching Protocols";
        case 200:
            return "OK";
        case 400:
            return "Bad Request";
        case 403:
            return "Forbidden";
        case 404:
            return "Not Found";
        case 405:
            return "Method Not Allowed";
        case 500:
            return "Internal Server Error";
        case 501:
            return "Not Implemented";
        default:
            return "OK";
        }
    }

    auto request_path(std::string_view target) -> std::string
    {
        const auto separator = target.find_first_of("?#");
        return std::string(target.substr(0, separator));
    }

    auto make_text_response(int status_code, std::string body, std::string content_type) -> response
    {
        response output;
        output.status_code = status_code;
        output.status_text = status_text(status_code);
        output.headers["Content-Type"] = std::move(content_type);
        output.body = std::move(body);
        return output;
    }

    auto make_json_response(int status_code, std::string json_body) -> response
    {
        return make_text_response(status_code, std::move(json_body), "application/json");
    }
} // namespace canopy::http_server
