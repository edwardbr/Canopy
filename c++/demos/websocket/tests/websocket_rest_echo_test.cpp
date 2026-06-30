// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#include "rest_echo_service.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

#include <canopy/rest/connection.h>
#include <canopy/rest/connection_factory.h>
#include <canopy/rest/http_server_adapter.h>
#include <connection_factory/connection_factory.h>
#include <json/json_dom.h>
#include <rpc/rpc.h>
#include <streaming/stream.h>

namespace rest_v1 = websocket_demo::rest::v1;

namespace
{
    auto trim_header_value(std::string value) -> std::string
    {
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
            value.erase(value.begin());
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
            value.pop_back();
        return value;
    }

    auto parse_http_request(std::string raw) -> canopy::http_server::request
    {
        canopy::http_server::request request;
        const auto header_end = raw.find("\r\n\r\n");
        const auto headers = raw.substr(0, header_end);
        request.body = header_end == std::string::npos ? std::string{} : raw.substr(header_end + 4);

        const auto request_line_end = headers.find("\r\n");
        const auto request_line = headers.substr(0, request_line_end);
        std::istringstream request_line_stream(request_line);
        request_line_stream >> request.method >> request.url;

        size_t offset = request_line_end == std::string::npos ? headers.size() : request_line_end + 2;
        while (offset < headers.size())
        {
            const auto line_end = headers.find("\r\n", offset);
            const auto line
                = headers.substr(offset, line_end == std::string::npos ? std::string::npos : line_end - offset);
            const auto colon = line.find(':');
            if (colon != std::string::npos)
                request.headers[line.substr(0, colon)] = trim_header_value(line.substr(colon + 1));
            if (line_end == std::string::npos)
                break;
            offset = line_end + 2;
        }

        return request;
    }

    auto build_http_response(canopy::http_server::response response) -> std::string
    {
        if (response.status_text.empty())
            response.status_text = canopy::http_server::status_text(response.status_code);
        if (response.headers.find("Content-Length") == response.headers.end())
            response.headers["Content-Length"] = std::to_string(response.body.size());
        if (response.headers.find("Connection") == response.headers.end())
            response.headers["Connection"] = "close";

        std::string output = "HTTP/1.1 " + std::to_string(response.status_code) + " " + response.status_text + "\r\n";
        for (const auto& [name, value] : response.headers)
            output += name + ": " + value + "\r\n";
        output += "\r\n";
        output += response.body;
        return output;
    }

    class dispatch_stream final : public streaming::stream
    {
    public:
        explicit dispatch_stream(canopy::rest::endpoint_registry rest_handlers)
            : rest_handlers_(std::move(rest_handlers))
        {
        }

        auto receive(
            rpc::mutable_byte_span buffer,
            std::chrono::milliseconds) -> CORO_TASK(streaming::receive_result) override
        {
            if (pending_response_.empty())
                CO_RETURN std::make_pair(rpc::io_status{FLD(type) rpc::io_status::kind::closed}, rpc::mutable_byte_span{});

            const auto byte_count = std::min(buffer.size(), pending_response_.size());
            std::copy(pending_response_.data(), pending_response_.data() + byte_count, buffer.data());
            pending_response_.erase(0, byte_count);
            CO_RETURN std::make_pair(
                rpc::io_status{FLD(type) rpc::io_status::kind::ok}, rpc::mutable_byte_span{buffer.data(), byte_count});
        }

        auto send(rpc::byte_span buffer) -> CORO_TASK(rpc::io_status) override
        {
            const std::string raw_request(reinterpret_cast<const char*>(buffer.data()), buffer.size());
            auto request = parse_http_request(raw_request);
            auto response = CO_AWAIT canopy::rest::handle_http_request(request, rest_handlers_);
            if (!response)
                response = canopy::http_server::make_json_response(404, R"({"error":"not found","status":404})");
            pending_response_ = build_http_response(std::move(*response));
            CO_RETURN rpc::io_status{FLD(type) rpc::io_status::kind::ok};
        }

        [[nodiscard]] bool is_closed() const override { return closed_; }

        auto set_closed() -> CORO_TASK(void) override
        {
            closed_ = true;
            CO_RETURN;
        }

        [[nodiscard]] auto get_peer_info() const -> streaming::peer_info override { return {}; }

    private:
        canopy::rest::endpoint_registry rest_handlers_;
        std::string pending_response_;
        bool closed_{false};
    };
}

TEST(
    WebsocketRestEcho,
    RestHandlerCallsInjectedRpcObject)
{
    canopy::rest::endpoint_registry rest_handlers;
    rest_handlers.add_object("echo", websocket_demo::v1::make_echo_service(), "/custom");

    canopy::http_server::request request;
    request.method = "POST";
    request.url = "/custom/echo";
    request.body = R"("hello from handler")";

    auto response = SYNC_WAIT(canopy::rest::handle_http_request(request, rest_handlers));
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status_code, 200);

    EXPECT_EQ(json::v1::parse(response->body).get<std::string>(), "hello from handler");
}

TEST(
    WebsocketRestEcho,
    RestDependenciesCanDispatchMultipleGeneratedHandlers)
{
    canopy::rest::endpoint_registry rest_handlers;
    rest_handlers.add_object("first_echo", websocket_demo::v1::make_echo_service(), "/first");
    rest_handlers.add_object("second_echo", websocket_demo::v1::make_echo_service(), "/second");

    canopy::http_server::request first_request;
    first_request.method = "POST";
    first_request.url = "/first/echo";
    first_request.body = R"("first handler")";

    auto first_response = SYNC_WAIT(canopy::rest::handle_http_request(first_request, rest_handlers));
    ASSERT_TRUE(first_response);
    EXPECT_EQ(first_response->status_code, 200);
    EXPECT_EQ(json::v1::parse(first_response->body).get<std::string>(), "first handler");

    canopy::http_server::request second_request;
    second_request.method = "POST";
    second_request.url = "/second/echo";
    second_request.body = R"("second handler")";

    auto second_response = SYNC_WAIT(canopy::rest::handle_http_request(second_request, rest_handlers));
    ASSERT_TRUE(second_response);
    EXPECT_EQ(second_response->status_code, 200);
    EXPECT_EQ(json::v1::parse(second_response->body).get<std::string>(), "second handler");
}

TEST(
    WebsocketRestEcho,
    GeneratedRestClientCanCallInjectedRpcObject)
{
    canopy::rest::endpoint_registry rest_handlers;
    rest_handlers.add_object("echo", websocket_demo::v1::make_echo_service(), "/custom");
    auto stream = std::make_shared<dispatch_stream>(rest_handlers);

    rpc::connection_factory::context factory_context;
    factory_context.register_connect_base_stream<rpc::connection_factory::service_settings>(
        "websocket_demo_rest_loopback",
        [stream](
            rpc::connection_factory::service_settings,
            std::shared_ptr<rpc::service>,
            const rpc::connection_factory::context&) -> CORO_TASK(rpc::connection_factory::stream_result)
        { CO_RETURN rpc::connection_factory::stream_result{rpc::error::OK(), stream}; });

    rpc::stream_layers::stream_layer_settings layer;
    layer.type = "websocket_demo_rest_loopback";

    rest_v1::i_echo::rest_settings settings;
    settings.endpoint.scheme = "http";
    settings.endpoint.host = "websocket-demo.test";
    settings.endpoint.base_path = "/custom";
    canopy::rest::connection_factory_settings factory_settings;
    factory_settings.stream_connection.stream_layers.push_back(layer);
    factory_settings.factory_context = std::move(factory_context);
    canopy::rest::use_connection_factory(settings, std::move(factory_settings));

    auto connected = SYNC_WAIT(rest_v1::i_echo::rest_caller::connect(std::move(settings)));
    ASSERT_EQ(connected.error_code, rpc::error::OK());
    ASSERT_TRUE(connected.object);

    std::string request = "hello from generated client";
    std::string response;

    const auto error = SYNC_WAIT(connected.object->echo(request, response));
    EXPECT_EQ(error, rpc::error::OK());
    EXPECT_EQ(response, request);
}
