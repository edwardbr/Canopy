// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

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
    using coroutine_request_handler = std::function<CORO_TASK(std::optional<response>)(const request&)>;
    using websocket_handler
        = std::function<CORO_TASK(std::shared_ptr<rpc::transport>)(const request&, std::shared_ptr<streaming::stream>)>;
    using rest_request_selector = std::function<bool(const request&)>;

    struct client_connection_limits
    {
        uint64_t max_method_bytes{256};
        uint64_t max_url_bytes{4096};
        uint64_t max_header_name_bytes{256};
        uint64_t max_header_value_bytes{8192};
        uint64_t max_header_count{128};
        uint64_t max_body_bytes{1024 * 1024};
        uint64_t max_pending_input_bytes{64 * 1024};
        std::chrono::milliseconds receive_poll_timeout{250};
        std::chrono::milliseconds header_timeout{10000};
        std::chrono::milliseconds request_timeout{30000};
    };

    struct handler_set
    {
        coroutine_request_handler webpage_handler;
        request_handler rest_handler;
        websocket_handler websocket_upgrade_handler;
        rest_request_selector is_rest_request;
    };

    class client_connection
    {
    public:
        explicit client_connection(
            std::shared_ptr<streaming::stream> stream,
            handler_set handlers,
            client_connection_limits limits = {});

        auto handle() -> CORO_TASK(std::shared_ptr<rpc::transport>);

    private:
        struct parser_request_context
        {
            request parsed_request;
            const client_connection_limits* limits{nullptr};
            std::string current_header_field;
            std::string current_header_value;
            size_t header_count{0};
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

        static auto limits_for(const parser_request_context& ctx) -> const client_connection_limits&;
        static bool flush_header(parser_request_context& ctx);
        static auto build_http_response(
            const response& response,
            bool keep_alive) -> std::string;
        static auto build_websocket_handshake_response(const std::string& accept_key) -> std::string;

        [[nodiscard]] auto dispatch_request(const request& request) const -> CORO_TASK(std::optional<response>);
        auto handle_websocket_upgrade(const request& request) -> CORO_TASK(std::shared_ptr<rpc::transport>);

        std::shared_ptr<streaming::stream> stream_;
        handler_set handlers_;
        client_connection_limits limits_;
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
