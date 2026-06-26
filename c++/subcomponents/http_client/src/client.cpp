/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <streaming/http_client/client.h>

#include <canopy/http_utils/http.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <string_view>
#include <vector>

namespace streaming::http_client
{
    namespace
    {
        constexpr auto header_name = [](const header& item) -> std::string_view { return item.name; };
        constexpr auto header_value = [](const header& item) -> std::string_view { return item.value; };

        [[nodiscard]] bool starts_with(
            std::string_view value,
            std::string_view prefix)
        {
            return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
        }

        void validate_request_line_value(
            std::string_view value,
            std::string_view field_name)
        {
            if (value.empty())
                throw std::runtime_error(std::string(field_name) + " must not be empty");
            if (std::any_of(value.begin(), value.end(), [](unsigned char ch) { return ch <= 0x20U || ch == 0x7FU; }))
            {
                throw std::runtime_error(std::string(field_name) + " contains invalid HTTP request-line characters");
            }
        }

        void validate_header(
            std::string_view name,
            std::string_view value)
        {
            if (!canopy::http_utils::is_http_token(name))
                throw std::runtime_error("HTTP header name contains invalid characters");
            if (canopy::http_utils::has_header_value_control(value))
                throw std::runtime_error("HTTP header value contains invalid control characters");
        }

        void validate_request(const request& input)
        {
            if (!canopy::http_utils::is_http_token(input.method))
                throw std::runtime_error("HTTP method contains invalid characters");
            validate_request_line_value(input.target, "HTTP request target");
            if (!input.host.empty())
                validate_header("Host", input.host);
            for (const auto& item : input.headers)
                validate_header(item.name, item.value);
        }

        [[nodiscard]] std::vector<header> parse_header_fields(std::string_view raw_headers)
        {
            std::vector<header> headers;
            const auto status_line_end = raw_headers.find("\r\n");
            if (status_line_end == std::string_view::npos)
                return headers;

            size_t line_offset = status_line_end + 2;
            while (line_offset < raw_headers.size())
            {
                const auto line_end = raw_headers.find("\r\n", line_offset);
                const auto line = raw_headers.substr(
                    line_offset, line_end == std::string_view::npos ? std::string_view::npos : line_end - line_offset);
                const auto separator = line.find(':');
                if (separator != std::string_view::npos)
                {
                    headers.push_back(
                        header{std::string(line.substr(0, separator)),
                            canopy::http_utils::trim_ascii_copy(line.substr(separator + 1))});
                }
                if (line_end == std::string_view::npos)
                    break;
                line_offset = line_end + 2;
            }
            return headers;
        }

        [[nodiscard]] bool has_complete_chunked_body(std::string_view body)
        {
            size_t offset = 0;
            while (true)
            {
                const auto line_end = body.find("\r\n", offset);
                if (line_end == std::string_view::npos)
                    return false;

                auto size_text = body.substr(offset, line_end - offset);
                const auto extension = size_text.find(';');
                if (extension != std::string_view::npos)
                    size_text = size_text.substr(0, extension);

                size_t chunk_size = 0;
                const auto parse_result
                    = std::from_chars(size_text.data(), size_text.data() + size_text.size(), chunk_size, 16);
                if (parse_result.ec != std::errc{} || parse_result.ptr != size_text.data() + size_text.size())
                    return true;

                offset = line_end + 2;
                if (chunk_size == 0)
                {
                    while (true)
                    {
                        const auto trailer_line_end = body.find("\r\n", offset);
                        if (trailer_line_end == std::string_view::npos)
                            return false;
                        if (trailer_line_end == offset)
                            return true;
                        offset = trailer_line_end + 2;
                    }
                }

                if (chunk_size > body.size() - offset)
                    return false;
                offset += chunk_size;
                if (body.size() - offset < 2)
                    return false;
                if (body.compare(offset, 2, "\r\n") != 0)
                    return true;
                offset += 2;
            }
        }

        [[nodiscard]] bool has_complete_response(const std::string& raw_response)
        {
            const auto header_end = raw_response.find("\r\n\r\n");
            if (header_end == std::string::npos)
                return false;

            const auto headers = parse_header_fields(std::string_view(raw_response).substr(0, header_end));

            const auto transfer_encoding = canopy::http_utils::ascii_lower_copy(
                std::string(
                    canopy::http_utils::find_header_value(headers, "Transfer-Encoding", header_name, header_value)
                        .value_or(std::string_view{})));
            if (transfer_encoding.find("chunked") != std::string::npos)
                return has_complete_chunked_body(std::string_view(raw_response).substr(header_end + 4));

            const auto content_length
                = canopy::http_utils::find_header_value(headers, "Content-Length", header_name, header_value);
            if (content_length && !content_length->empty())
            {
                try
                {
                    const size_t body_length = canopy::http_utils::parse_content_length(*content_length);
                    const size_t body_start = header_end + 4;
                    // Guard against size_t wrap from a server-supplied Content-Length near SIZE_MAX.
                    if (body_length > raw_response.max_size() - body_start)
                        return false;
                    return raw_response.size() >= body_start + body_length;
                }
                catch (...)
                {
                    return false;
                }
            }

            return false;
        }

        [[nodiscard]] result make_error(
            int error_code,
            std::string message,
            std::string wire_request = {})
        {
            result output;
            output.error_code = error_code;
            output.error_message = std::move(message);
            output.wire_request = std::move(wire_request);
            return output;
        }
    }

    std::string build_request(const request& input)
    {
        validate_request(input);

        std::ostringstream output;
        output << input.method << " " << input.target << " HTTP/1.1\r\n";
        if (!input.host.empty() && !canopy::http_utils::has_header(input.headers, "Host", header_name))
            output << "Host: " << input.host << "\r\n";
        for (const auto& item : input.headers)
            output << item.name << ": " << item.value << "\r\n";
        if (!input.body.empty() && !canopy::http_utils::has_header(input.headers, "Content-Length", header_name))
            output << "Content-Length: " << input.body.size() << "\r\n";
        if (input.close_connection && !canopy::http_utils::has_header(input.headers, "Connection", header_name))
            output << "Connection: close\r\n";
        output << "\r\n";
        output << input.body;
        return output.str();
    }

    result parse_response(std::string raw_response)
    {
        try
        {
            const auto header_end = raw_response.find("\r\n\r\n");
            if (header_end == std::string::npos)
                return make_error(rpc::error::INVALID_DATA(), "HTTP response did not contain a header terminator");

            result output;
            output.value.raw_headers = raw_response.substr(0, header_end);

            const auto status_line_end = output.value.raw_headers.find("\r\n");
            const auto status_line = output.value.raw_headers.substr(0, status_line_end);
            std::istringstream status_stream(status_line);
            std::string http_version;
            status_stream >> http_version >> output.value.status_code;
            std::getline(status_stream, output.value.reason);
            output.value.reason = canopy::http_utils::trim_ascii_copy(output.value.reason);
            if (!starts_with(http_version, "HTTP/") || output.value.status_code == 0)
                return make_error(rpc::error::INVALID_DATA(), "invalid HTTP status line: " + status_line);

            size_t line_offset
                = status_line_end == std::string::npos ? output.value.raw_headers.size() : status_line_end + 2;
            while (line_offset < output.value.raw_headers.size())
            {
                const auto line_end = output.value.raw_headers.find("\r\n", line_offset);
                const auto line = output.value.raw_headers.substr(
                    line_offset, line_end == std::string::npos ? std::string::npos : line_end - line_offset);
                const auto separator = line.find(':');
                if (separator != std::string::npos)
                {
                    output.value.headers.push_back(
                        header{line.substr(0, separator),
                            canopy::http_utils::trim_ascii_copy(std::string_view(line).substr(separator + 1))});
                }
                if (line_end == std::string::npos)
                    break;
                line_offset = line_end + 2;
            }

            output.value.body = raw_response.substr(header_end + 4);
            const auto transfer_encoding = canopy::http_utils::ascii_lower_copy(
                std::string(
                    canopy::http_utils::find_header_value(output.value.headers, "Transfer-Encoding", header_name, header_value)
                        .value_or(std::string_view{})));
            if (transfer_encoding.find("chunked") != std::string::npos)
            {
                output.value.body = canopy::http_utils::decode_chunked_body(output.value.body);
            }
            else
            {
                const auto content_length = canopy::http_utils::find_header_value(
                    output.value.headers, "Content-Length", header_name, header_value);
                if (content_length && !content_length->empty())
                {
                    const auto body_length = canopy::http_utils::parse_content_length(*content_length);
                    if (output.value.body.size() < body_length)
                        return make_error(rpc::error::INVALID_DATA(), "truncated HTTP response body");
                    output.value.body.resize(body_length);
                }
            }

            return output;
        }
        catch (const std::exception& ex)
        {
            return make_error(rpc::error::INVALID_DATA(), ex.what());
        }
    }

    CORO_TASK(result)
    send_request(
        std::shared_ptr<::streaming::stream> stream,
        const request& input,
        std::chrono::milliseconds receive_timeout,
        size_t max_response_bytes)
    {
        if (!stream)
            CO_RETURN make_error(rpc::error::INVALID_DATA(), "HTTP client requires a stream");

        std::string wire_request;
        try
        {
            wire_request = build_request(input);
        }
        catch (const std::exception& ex)
        {
            CO_RETURN make_error(rpc::error::INVALID_DATA(), ex.what());
        }

        auto send_status = CO_AWAIT stream->send(rpc::byte_span(wire_request));
        if (!send_status.is_ok())
            CO_RETURN make_error(rpc::error::TRANSPORT_ERROR(), "failed to send HTTP request over stream", wire_request);

        std::array<char, 4096> buffer{};
        std::string raw_response;
        bool response_complete = false;
        while (raw_response.size() < max_response_bytes)
        {
            auto [status, span]
                = CO_AWAIT stream->receive(rpc::mutable_byte_span{buffer.data(), buffer.size()}, receive_timeout);
            if (status.is_ok() && !span.empty())
            {
                raw_response.append(reinterpret_cast<const char*>(span.data()), span.size());
                if (raw_response.size() > max_response_bytes)
                    CO_RETURN make_error(rpc::error::INVALID_DATA(), "HTTP response exceeded maximum size", wire_request);
                if (has_complete_response(raw_response))
                {
                    response_complete = true;
                    break;
                }
                continue;
            }

            // Hard transport error: propagate immediately regardless of buffered data.
            if (!status.is_ok() && !status.is_timeout())
                CO_RETURN make_error(rpc::error::TRANSPORT_ERROR(), "stream error mid-response", wire_request);

            // Timeout: always an error — a partial buffer is not a valid response.
            if (status.is_timeout())
                CO_RETURN make_error(rpc::error::CALL_TIMEOUT(), "timed out waiting for HTTP response", wire_request);

            // Graceful EOF (is_ok(), empty span): body terminator for Connection: close responses.
            if (!raw_response.empty())
            {
                response_complete = true;
                break;
            }

            CO_RETURN make_error(rpc::error::TRANSPORT_ERROR(), "stream closed before HTTP response", wire_request);
        }

        if (!response_complete)
            CO_RETURN make_error(rpc::error::INVALID_DATA(), "HTTP response exceeded maximum size", wire_request);

        auto parsed = parse_response(std::move(raw_response));
        parsed.wire_request = std::move(wire_request);
        CO_RETURN parsed;
    }
}
