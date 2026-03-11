// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

// http_client_connection.cpp
#include "http_client_connection.h"

#include <fmt/format.h>

namespace websocket_demo
{
    namespace v1
    {
        http_client_connection::http_client_connection(
            std::shared_ptr<streaming::stream> stream, std::shared_ptr<websocket_service> service)
            : stream_(std::move(stream))
            , service_(std::move(service))
        {
        }

        int http_client_connection::on_method(llhttp_t* parser, const char* at, size_t length)
        {
            auto* ctx = static_cast<http_request_context*>(parser->data);
            ctx->method.assign(at, length);
            return 0;
        }

        int http_client_connection::on_url(llhttp_t* parser, const char* at, size_t length)
        {
            auto* ctx = static_cast<http_request_context*>(parser->data);
            ctx->url.assign(at, length);
            return 0;
        }

        int http_client_connection::on_header_field(llhttp_t* parser, const char* at, size_t length)
        {
            auto* ctx = static_cast<http_request_context*>(parser->data);

            if (!ctx->current_header_field.empty() && !ctx->current_header_value.empty())
            {
                ctx->headers[ctx->current_header_field] = ctx->current_header_value;
                ctx->current_header_value.clear();
            }

            ctx->current_header_field.assign(at, length);
            return 0;
        }

        int http_client_connection::on_header_value(llhttp_t* parser, const char* at, size_t length)
        {
            auto* ctx = static_cast<http_request_context*>(parser->data);
            ctx->current_header_value.assign(at, length);
            return 0;
        }

        int http_client_connection::on_headers_complete(llhttp_t* parser)
        {
            auto* ctx = static_cast<http_request_context*>(parser->data);

            if (!ctx->current_header_field.empty() && !ctx->current_header_value.empty())
            {
                ctx->headers[ctx->current_header_field] = ctx->current_header_value;
                ctx->current_header_field.clear();
                ctx->current_header_value.clear();
            }

            ctx->headers_complete = true;
            return 0;
        }

        int http_client_connection::on_body(llhttp_t* parser, const char* at, size_t length)
        {
            auto* ctx = static_cast<http_request_context*>(parser->data);
            ctx->body.append(at, length);
            return 0;
        }

        int http_client_connection::on_message_complete(llhttp_t* parser)
        {
            auto* ctx = static_cast<http_request_context*>(parser->data);
            ctx->message_complete = true;
            return 0;
        }

        std::string http_client_connection::build_http_response(int status_code,
            const std::string& status_text,
            const std::map<std::string, std::string>& headers,
            const std::string& body)
        {
            std::string response = fmt::format("HTTP/1.1 {} {}\r\n", status_code, status_text);

            for (const auto& [key, value] : headers)
            {
                response += fmt::format("{}: {}\r\n", key, value);
            }

            if (!body.empty() && headers.find("Content-Length") == headers.end())
            {
                response += fmt::format("Content-Length: {}\r\n", body.length());
            }

            response += "\r\n";
            response += body;

            return response;
        }

        auto http_client_connection::handle() -> coro::task<void>
        {
            std::string buffer(8192, '\0');

            try
            {
                llhttp_t parser;
                llhttp_settings_t settings;
                http_request_context ctx;

                llhttp_settings_init(&settings);
                settings.on_method = on_method;
                settings.on_url = on_url;
                settings.on_header_field = on_header_field;
                settings.on_header_value = on_header_value;
                settings.on_headers_complete = on_headers_complete;
                settings.on_body = on_body;
                settings.on_message_complete = on_message_complete;

                llhttp_init(&parser, HTTP_REQUEST, &settings);
                parser.data = &ctx;

                bool is_websocket_upgrade = false;
                do
                {
                    auto [rstatus, rspan] = co_await stream_->receive(buffer);

                    if (!rstatus.is_ok() || rspan.empty())
                    {
                        RPC_ERROR("Failed to read HTTP request");
                        co_return;
                    }

                    {
                        std::string preview;
                        for (size_t i = 0; i < std::min(size_t(100), rspan.size()); i++)
                        {
                            unsigned char c = static_cast<unsigned char>(rspan.data()[i]);
                            if (c >= 32 && c < 127)
                            {
                                preview += static_cast<char>(c);
                            }
                            else
                            {
                                preview += fmt::format("\\x{:02x}", static_cast<int>(c));
                            }
                        }
                        RPC_DEBUG("Received {} bytes, first bytes: {}", rspan.size(), preview);
                    }

                    enum llhttp_errno err = llhttp_execute(&parser, rspan.data(), rspan.size());
                    if (err == HPE_PAUSED_UPGRADE)
                    {
                        is_websocket_upgrade = true;
                        llhttp_resume_after_upgrade(&parser);
                        err = HPE_OK;
                    }
                    else if (err != HPE_OK)
                    {
                        RPC_ERROR("HTTP parse error: {}", llhttp_errno_name(err));
                        std::string response = create_error_response(400, "Bad Request");
                        co_await stream_->send(std::span<const char>{response});
                        co_return;
                    }
                } while (!ctx.message_complete);

                if (is_websocket_upgrade)
                {
                    if (!(co_await handle_websocket_upgrade(ctx)))
                    {
                        RPC_ERROR("WebSocket upgrade failed");
                    }
                    co_return;
                }

                std::string method = llhttp_method_name(static_cast<llhttp_method_t>(parser.method));
                std::string path = ctx.url;
                RPC_INFO("HTTP {} request for: {}", method, path);

                std::string response;

                if (path.starts_with("/api/"))
                {
                    response = handle_rest_request(method, path, ctx.body);
                    RPC_INFO("Handled REST API request: {} {}", method, path);
                }
                else
                {
                    response = handle_browser_request(method, path);
                    RPC_INFO("Handled browser content request: {} {}", method, path);
                }

                auto httpstatus = co_await stream_->send(std::span<const char>{response});
                if (!httpstatus.is_ok())
                {
                    RPC_ERROR("Failed to send HTTP response for: {}", path);
                }
            }
            catch (const std::exception& e)
            {
                RPC_ERROR("Exception in http_client_connection::handle: {}", e.what());
            }

            co_return;
        }
    }
}
