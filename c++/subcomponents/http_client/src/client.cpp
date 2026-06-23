/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <streaming/http_client/client.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace streaming::http_client
{
    namespace
    {
        [[nodiscard]] bool starts_with(
            std::string_view value,
            std::string_view prefix)
        {
            return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
        }

        [[nodiscard]] std::string to_lower(std::string value)
        {
            std::transform(
                value.begin(),
                value.end(),
                value.begin(),
                [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            return value;
        }

        [[nodiscard]] std::string trim(std::string_view value)
        {
            while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
                value.remove_prefix(1);
            while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
                value.remove_suffix(1);
            return std::string(value);
        }

        [[nodiscard]] bool header_name_equals(
            std::string_view lhs,
            std::string_view rhs)
        {
            if (lhs.size() != rhs.size())
                return false;
            for (size_t i = 0; i < lhs.size(); ++i)
            {
                if (std::tolower(static_cast<unsigned char>(lhs[i])) != std::tolower(static_cast<unsigned char>(rhs[i])))
                {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] bool has_header(
            const std::vector<header>& headers,
            std::string_view name)
        {
            return std::any_of(
                headers.begin(),
                headers.end(),
                [name](const header& item) { return header_name_equals(item.name, name); });
        }

        [[nodiscard]] std::string_view find_header(
            const std::vector<header>& headers,
            std::string_view name)
        {
            for (const auto& item : headers)
            {
                if (header_name_equals(item.name, name))
                    return item.value;
            }
            return {};
        }

        [[nodiscard]] std::string decode_chunked_body(const std::string& body)
        {
            std::string decoded;
            size_t offset = 0;
            while (true)
            {
                const auto line_end = body.find("\r\n", offset);
                if (line_end == std::string::npos)
                    throw std::runtime_error("invalid chunked HTTP response");

                auto size_text = body.substr(offset, line_end - offset);
                const auto extension = size_text.find(';');
                if (extension != std::string::npos)
                    size_text.resize(extension);

                size_t chunk_size = 0;
                const auto size_view = std::string_view(size_text);
                const auto parse_result
                    = std::from_chars(size_view.data(), size_view.data() + size_view.size(), chunk_size, 16);
                if (parse_result.ec != std::errc{} || parse_result.ptr != size_view.data() + size_view.size())
                    throw std::runtime_error("invalid chunk size in HTTP response");

                offset = line_end + 2;
                if (chunk_size == 0)
                    break;
                if (offset + chunk_size > body.size())
                    throw std::runtime_error("truncated chunked HTTP response");

                decoded.append(body.data() + offset, chunk_size);
                offset += chunk_size;
                if (offset + 2 > body.size() || body.compare(offset, 2, "\r\n") != 0)
                    throw std::runtime_error("invalid chunk delimiter in HTTP response");
                offset += 2;
            }
            return decoded;
        }

        [[nodiscard]] size_t parse_content_length(std::string_view value)
        {
            const auto trimmed = trim(value);
            value = std::string_view(trimmed);
            size_t result = 0;
            const auto parse_result = std::from_chars(value.data(), value.data() + value.size(), result);
            if (parse_result.ec != std::errc{} || parse_result.ptr != value.data() + value.size())
                throw std::runtime_error("invalid HTTP Content-Length");
            return result;
        }

        [[nodiscard]] bool has_complete_response(const std::string& raw_response)
        {
            const auto header_end = raw_response.find("\r\n\r\n");
            if (header_end == std::string::npos)
                return false;

            auto parsed = parse_response(raw_response);
            if (parsed.error_code != rpc::error::OK())
                return false;

            const auto transfer_encoding = to_lower(std::string(find_header(parsed.value.headers, "Transfer-Encoding")));
            if (transfer_encoding.find("chunked") != std::string::npos)
                return true;

            const auto content_length = find_header(parsed.value.headers, "Content-Length");
            if (!content_length.empty())
            {
                try
                {
                    return raw_response.size() >= header_end + 4 + parse_content_length(content_length);
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
        std::ostringstream output;
        output << input.method << " " << input.target << " HTTP/1.1\r\n";
        if (!input.host.empty() && !has_header(input.headers, "Host"))
            output << "Host: " << input.host << "\r\n";
        for (const auto& item : input.headers)
            output << item.name << ": " << item.value << "\r\n";
        if (!input.body.empty() && !has_header(input.headers, "Content-Length"))
            output << "Content-Length: " << input.body.size() << "\r\n";
        if (input.close_connection && !has_header(input.headers, "Connection"))
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
            output.value.reason = trim(output.value.reason);
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
                        header{line.substr(0, separator), trim(std::string_view(line).substr(separator + 1))});
                }
                if (line_end == std::string::npos)
                    break;
                line_offset = line_end + 2;
            }

            output.value.body = raw_response.substr(header_end + 4);
            const auto transfer_encoding = to_lower(std::string(find_header(output.value.headers, "Transfer-Encoding")));
            if (transfer_encoding.find("chunked") != std::string::npos)
            {
                output.value.body = decode_chunked_body(output.value.body);
            }
            else
            {
                const auto content_length = find_header(output.value.headers, "Content-Length");
                if (!content_length.empty())
                {
                    const auto body_length = parse_content_length(content_length);
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

        auto wire_request = build_request(input);
        auto send_status = CO_AWAIT stream->send(rpc::byte_span(wire_request));
        if (!send_status.is_ok())
            CO_RETURN make_error(rpc::error::TRANSPORT_ERROR(), "failed to send HTTP request over stream", wire_request);

        std::array<char, 4096> buffer{};
        std::string raw_response;
        while (raw_response.size() < max_response_bytes)
        {
            auto [status, span]
                = CO_AWAIT stream->receive(rpc::mutable_byte_span{buffer.data(), buffer.size()}, receive_timeout);
            if (status.is_ok() && !span.empty())
            {
                raw_response.append(reinterpret_cast<const char*>(span.data()), span.size());
                if (has_complete_response(raw_response))
                    break;
                continue;
            }

            if (!raw_response.empty())
                break;

            if (status.is_timeout())
                CO_RETURN make_error(rpc::error::CALL_TIMEOUT(), "timed out waiting for HTTP response", wire_request);

            CO_RETURN make_error(rpc::error::TRANSPORT_ERROR(), "stream closed before HTTP response", wire_request);
        }

        if (raw_response.size() >= max_response_bytes)
            CO_RETURN make_error(rpc::error::INVALID_DATA(), "HTTP response exceeded maximum size", wire_request);

        auto parsed = parse_response(std::move(raw_response));
        parsed.wire_request = std::move(wire_request);
        CO_RETURN parsed;
    }
}
