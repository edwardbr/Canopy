// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <coro/coro.hpp>
#include <llhttp.h>
#include <rpc/rpc.h>
#include <streaming/stream.h>
#include <transports/streaming/transport.h>

namespace canopy::http_server
{
    struct request
    {
        std::string method;
        std::string url;
        std::map<std::string, std::string> headers;
        std::string body;
        bool keep_alive{false};
    };

    struct response
    {
        int status_code{200};
        std::string status_text;
        std::map<std::string, std::string> headers;
        std::string body;
    };

    using request_handler = std::function<std::optional<response>(const request&)>;
    using websocket_handler
        = std::function<coro::task<std::shared_ptr<rpc::transport>>(const request&, std::shared_ptr<streaming::stream>)>;
    using rest_request_selector = std::function<bool(const request&)>;

    struct handler_set
    {
        request_handler webpage_handler;
        request_handler rest_handler;
        websocket_handler websocket_upgrade_handler;
        rest_request_selector is_rest_request;
    };

    class client_connection
    {
    public:
        explicit client_connection(
            std::shared_ptr<streaming::stream> stream,
            handler_set handlers);

        auto handle() -> coro::task<std::shared_ptr<rpc::transport>>;

    private:
        struct parser_request_context
        {
            request parsed_request;
            std::string current_header_field;
            std::string current_header_value;
            bool reading_header_value{false};
            bool headers_complete{false};
            bool message_complete{false};
        };

        static int on_method(
            llhttp_t* parser,
            const char* at,
            size_t length);
        static int on_url(
            llhttp_t* parser,
            const char* at,
            size_t length);
        static int on_header_field(
            llhttp_t* parser,
            const char* at,
            size_t length);
        static int on_header_value(
            llhttp_t* parser,
            const char* at,
            size_t length);
        static int on_headers_complete(llhttp_t* parser);
        static int on_body(
            llhttp_t* parser,
            const char* at,
            size_t length);
        static int on_message_complete(llhttp_t* parser);

        static void flush_header(parser_request_context& ctx);
        static auto build_http_response(
            const response& response,
            bool keep_alive) -> std::string;
        static auto build_websocket_handshake_response(const std::string& accept_key) -> std::string;

        [[nodiscard]] auto dispatch_request(const request& request) const -> std::optional<response>;
        auto handle_websocket_upgrade(const request& request) -> coro::task<std::shared_ptr<rpc::transport>>;

        std::shared_ptr<streaming::stream> stream_;
        handler_set handlers_;
    };

    auto status_text(int status_code) -> std::string;
    auto request_path(std::string_view target) -> std::string;
    auto make_text_response(
        int status_code,
        std::string body,
        std::string content_type = "text/plain") -> response;
    auto make_json_response(
        int status_code,
        std::string json_body) -> response;
} // namespace canopy::http_server
