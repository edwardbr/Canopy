/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include <rpc/rpc.h>
#include <streaming/stream.h>

namespace streaming::http_client
{
    struct header
    {
        std::string name;
        std::string value;
    };

    struct request
    {
        std::string method{"GET"};
        std::string target{"/"};
        std::string host;
        std::vector<header> headers;
        std::string body;
        bool close_connection{true};
    };

    struct response
    {
        int status_code{0};
        std::string reason;
        std::vector<header> headers;
        std::string raw_headers;
        std::string body;
    };

    struct result
    {
        int error_code{rpc::error::OK()};
        std::string error_message;
        std::string wire_request;
        response value;
    };

    [[nodiscard]] std::string build_request(const request& input);
    [[nodiscard]] result parse_response(std::string raw_response);

    CORO_TASK(result)
    send_request(
        std::shared_ptr<::streaming::stream> stream,
        const request& input,
        std::chrono::milliseconds receive_timeout = std::chrono::milliseconds{10000},
        size_t max_response_bytes = 2U * 1024U * 1024U);
}
