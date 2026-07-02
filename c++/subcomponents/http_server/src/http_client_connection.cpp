// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#include <canopy/http_server/http_client_connection.h>

#include <canopy/http_utils/http.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <limits>
#include <map>
#include <optional>
#include <string_view>
#include <vector>

#include <fmt/format.h>
#include <llhttp.h>
#include <streaming/websocket/stream.h>

#if defined(CANOPY_BUILD_ZLIB)
#  include <streaming/detail/zlib_allocator.h>
#  include <zlib.h>
#endif

#if defined(CANOPY_SECURE_STREAM_BACKEND_MBEDTLS)
#  include <mbedtls/base64.h>
#  include <mbedtls/sha1.h>
#elif defined(CANOPY_SECURE_STREAM_BACKEND_OPENSSL)
#  include <openssl/bio.h>
#  include <openssl/buffer.h>
#  include <openssl/evp.h>
#  include <openssl/sha.h>
#else
#  error "canopy_http_server requires CANOPY_SECURE_STREAM_BACKEND_OPENSSL or CANOPY_SECURE_STREAM_BACKEND_MBEDTLS"
#endif

namespace canopy::http_server
{
    namespace
    {
        using canopy::http_utils::ascii_iequals;
        using canopy::http_utils::header_contains_token;
        using canopy::http_utils::is_header_safe;
        using canopy::http_utils::is_status_text_safe;
        using canopy::http_utils::trim_ascii;

        constexpr auto map_header_name = [](const auto& header) -> std::string_view { return header.first; };
        constexpr auto map_header_value = [](const auto& header) -> std::string_view { return header.second; };

        constexpr size_t websocket_client_key_bytes = 16;

        bool exceeds_limit(
            size_t value,
            uint64_t limit)
        {
            return limit != 0 && static_cast<uint64_t>(value) > limit;
        }

        bool timeout_expired(
            std::chrono::steady_clock::time_point started,
            std::chrono::milliseconds limit)
        {
            return limit > std::chrono::milliseconds{0} && std::chrono::steady_clock::now() - started >= limit;
        }

        auto timeout_from_ms(uint64_t value) -> std::chrono::milliseconds
        {
            const auto max_value = static_cast<uint64_t>(std::numeric_limits<std::chrono::milliseconds::rep>::max());
            return std::chrono::milliseconds{static_cast<std::chrono::milliseconds::rep>(std::min(value, max_value))};
        }

        auto find_header(
            const request& request,
            std::string_view name) -> std::optional<std::string_view>
        {
            return canopy::http_utils::find_header_value(request.headers, name, map_header_name, map_header_value);
        }

        auto find_header(
            const std::map<
                std::string,
                std::string>& headers,
            std::string_view name) -> std::optional<std::string_view>
        {
            return canopy::http_utils::find_header_value(headers, name, map_header_name, map_header_value);
        }

        auto find_header(
            std::map<
                std::string,
                std::string>& headers,
            std::string_view name)
            -> std::map<
                std::string,
                std::string>::iterator
        {
            return canopy::http_utils::find_header(headers, name, map_header_name);
        }

        void set_header(
            response& response,
            std::string name,
            std::string value)
        {
            auto it = find_header(response.headers, name);
            if (it == response.headers.end())
            {
                response.headers.emplace(std::move(name), std::move(value));
                return;
            }
            it->second = std::move(value);
        }

        void erase_header(
            response& response,
            std::string_view name)
        {
            auto it = find_header(response.headers, name);
            if (it != response.headers.end())
                response.headers.erase(it);
        }

        void normalise_no_body_content_length(response& response)
        {
            auto it = find_header(response.headers, "Content-Length");
            if (it == response.headers.end())
                return;

            try
            {
                if (canopy::http_utils::parse_content_length(it->second) == 0)
                {
                    it->second = "0";
                    return;
                }
            }
            catch (...)
            {
                (void)0;
            }

            response.headers.erase(it);
        }

        auto parse_qvalue(std::string_view input) -> int
        {
            input = trim_ascii(input);
            if (input.empty())
                return 0;

            if (input.front() == '0')
            {
                input.remove_prefix(1);
                if (input.empty())
                    return 0;
                if (input.front() != '.')
                    return 0;
                input.remove_prefix(1);

                int scale = 100;
                int value = 0;
                while (!input.empty() && scale > 0)
                {
                    const auto digit = static_cast<unsigned char>(input.front());
                    if (!std::isdigit(digit))
                        return 0;
                    value += static_cast<int>(digit - '0') * scale;
                    scale /= 10;
                    input.remove_prefix(1);
                }

                while (!input.empty())
                {
                    if (!std::isdigit(static_cast<unsigned char>(input.front())))
                        return 0;
                    input.remove_prefix(1);
                }

                return value;
            }

            if (input.front() != '1')
                return 0;
            input.remove_prefix(1);
            if (input.empty())
                return 1000;
            if (input.front() != '.')
                return 0;
            input.remove_prefix(1);
            while (!input.empty())
            {
                if (input.front() != '0')
                    return 0;
                input.remove_prefix(1);
            }
            return 1000;
        }

        auto parse_content_coding_q(std::string_view value) -> int
        {
            const auto semicolon = value.find(';');
            auto parameters = semicolon == std::string_view::npos ? std::string_view{} : value.substr(semicolon + 1);
            int qvalue = 1000;

            while (!parameters.empty())
            {
                const auto separator = parameters.find(';');
                auto parameter = trim_ascii(parameters.substr(0, separator));
                const auto equals = parameter.find('=');
                if (equals != std::string_view::npos)
                {
                    auto name = trim_ascii(parameter.substr(0, equals));
                    auto setting = trim_ascii(parameter.substr(equals + 1));
                    if (ascii_iequals(name, "q"))
                        qvalue = parse_qvalue(setting);
                }

                if (separator == std::string_view::npos)
                    break;
                parameters.remove_prefix(separator + 1);
            }

            return qvalue;
        }

        auto accepted_gzip_qvalue(const request& request) -> int
        {
            auto header = find_header(request, "Accept-Encoding");
            if (!header)
                return 0;

            int gzip_qvalue = -1;
            int wildcard_qvalue = -1;
            auto value = *header;
            while (!value.empty())
            {
                const auto comma = value.find(',');
                auto part = trim_ascii(value.substr(0, comma));
                const auto semicolon = part.find(';');
                auto coding = trim_ascii(part.substr(0, semicolon));
                const auto qvalue = parse_content_coding_q(part);

                if (ascii_iequals(coding, "gzip"))
                    gzip_qvalue = qvalue;
                else if (coding == "*")
                    wildcard_qvalue = qvalue;

                if (comma == std::string_view::npos)
                    break;
                value.remove_prefix(comma + 1);
            }

            return gzip_qvalue >= 0 ? gzip_qvalue : std::max(wildcard_qvalue, 0);
        }

        bool content_type_allows_http_compression(std::string_view content_type)
        {
            const auto semicolon = content_type.find(';');
            content_type = trim_ascii(content_type.substr(0, semicolon));
            if (content_type.empty())
                return true;

            auto starts_with = [content_type](std::string_view prefix)
            {
                return content_type.size() >= prefix.size()
                       && ascii_iequals(content_type.substr(0, prefix.size()), prefix);
            };

            if (starts_with("text/"))
                return true;
            if (ascii_iequals(content_type, "application/javascript") || ascii_iequals(content_type, "application/json")
                || ascii_iequals(content_type, "application/xml") || ascii_iequals(content_type, "application/wasm")
                || ascii_iequals(content_type, "image/svg+xml"))
            {
                return true;
            }

            return false;
        }

        bool response_is_compressible(const response& response)
        {
            if (response.body.empty())
                return false;
            if (response.status_code < 200 || response.status_code == 204 || response.status_code == 304)
                return false;
            if (find_header(response.headers, "Content-Encoding"))
                return false;
            if (find_header(response.headers, "Transfer-Encoding"))
                return false;
            if (find_header(response.headers, "Content-Range"))
                return false;
            auto cache_control = find_header(response.headers, "Cache-Control");
            if (cache_control && header_contains_token(*cache_control, "no-transform"))
                return false;

            auto content_type = find_header(response.headers, "Content-Type");
            return !content_type || content_type_allows_http_compression(*content_type);
        }

        bool response_status_allows_body(int status_code)
        {
            return status_code >= 200 && status_code != 204 && status_code != 304;
        }

        void add_vary_accept_encoding(response& response)
        {
            auto vary = find_header(response.headers, "Vary");
            if (vary == response.headers.end())
            {
                response.headers["Vary"] = "Accept-Encoding";
                return;
            }
            if (ascii_iequals(trim_ascii(vary->second), "*") || header_contains_token(vary->second, "Accept-Encoding"))
                return;
            vary->second += ", Accept-Encoding";
        }

#if defined(CANOPY_BUILD_ZLIB)
        auto gzip_compress(std::string_view input) -> std::optional<std::string>
        {
            z_stream stream{};
            streaming::detail::initialise_zlib_allocator(stream);
            const auto init_result
                = deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, MAX_WBITS + 16, 8, Z_DEFAULT_STRATEGY);
            if (init_result != Z_OK)
            {
                RPC_WARNING("HTTP gzip deflateInit2 failed: {}", init_result);
                return std::nullopt;
            }

            std::string output;
            std::array<unsigned char, size_t{16U} * 1024U> buffer{};
            std::array<unsigned char, size_t{16U} * 1024U> input_buffer{};
            size_t input_offset = 0;
            int deflate_result = Z_OK;

            do
            {
                const auto remaining = input.size() - input_offset;
                const auto input_size = std::min(remaining, input_buffer.size());
                std::copy_n(input.data() + input_offset, input_size, input_buffer.data());
                stream.next_in = input_buffer.data();
                stream.avail_in = static_cast<uInt>(input_size);
                input_offset += input_size;
                const auto flush = input_offset == input.size() ? Z_FINISH : Z_NO_FLUSH;

                do
                {
                    stream.next_out = buffer.data();
                    stream.avail_out = static_cast<uInt>(buffer.size());
                    deflate_result = deflate(&stream, flush);
                    if (deflate_result != Z_OK && deflate_result != Z_STREAM_END)
                    {
                        RPC_WARNING(
                            "HTTP gzip deflate failed: result={} msg={} avail_in={} avail_out={} total_in={} "
                            "total_out={}",
                            deflate_result,
                            stream.msg ? stream.msg : "",
                            stream.avail_in,
                            stream.avail_out,
                            stream.total_in,
                            stream.total_out);
                        deflateEnd(&stream);
                        return std::nullopt;
                    }

                    const auto produced = buffer.size() - stream.avail_out;
                    output.append(reinterpret_cast<const char*>(buffer.data()), produced);
                } while (stream.avail_out == 0 && deflate_result != Z_STREAM_END);
            } while (deflate_result != Z_STREAM_END);

            const auto end_result = deflateEnd(&stream);
            if (end_result != Z_OK)
            {
                RPC_WARNING("HTTP gzip deflateEnd failed: {}", end_result);
                return std::nullopt;
            }

            return output;
        }
#endif

        auto apply_http_compression(
            const request& request,
            response output) -> response
        {
#if defined(CANOPY_BUILD_ZLIB)
            if (accepted_gzip_qvalue(request) <= 0 || !response_is_compressible(output))
                return output;

            auto compressed = gzip_compress(output.body);
            if (!compressed)
            {
                RPC_WARNING("HTTP gzip compression failed; sending uncompressed response");
                return output;
            }

            output.body = std::move(*compressed);
            set_header(output, "Content-Encoding", "gzip");
            add_vary_accept_encoding(output);
            erase_header(output, "Content-Length");
#else
            (void)request;
#endif
            return output;
        }

        bool websocket_extension_requested(
            std::string_view value,
            std::string_view extension_name)
        {
            while (!value.empty())
            {
                const auto comma = value.find(',');
                auto extension = trim_ascii(value.substr(0, comma));
                const auto semicolon = extension.find(';');
                auto name = trim_ascii(extension.substr(0, semicolon));
                if (ascii_iequals(name, extension_name))
                    return true;
                if (comma == std::string_view::npos)
                    break;
                value.remove_prefix(comma + 1);
            }
            return false;
        }

        auto negotiate_websocket_extensions(const request& request) -> std::optional<std::string>
        {
            auto extensions = find_header(request, "Sec-WebSocket-Extensions");
            if (!extensions || !websocket_extension_requested(*extensions, "permessage-deflate"))
                return std::nullopt;

#if defined(CANOPY_BUILD_ZLIB)
            return std::string("permessage-deflate; server_no_context_takeover; client_no_context_takeover");
#else
            return std::nullopt;
#endif
        }

        bool matches_allowed_value(
            std::string_view value,
            const std::vector<std::string>& allowed_values)
        {
            value = trim_ascii(value);
            return std::any_of(
                allowed_values.begin(),
                allowed_values.end(),
                [value](const std::string& allowed) { return ascii_iequals(value, trim_ascii(allowed)); });
        }

        auto decode_base64(std::string_view input) -> std::optional<std::vector<unsigned char>>
        {
            input = trim_ascii(input);
            if (input.empty() || input.size() % 4 != 0)
                return std::nullopt;

            size_t padding = 0;
            for (auto it = input.rbegin(); it != input.rend() && *it == '='; ++it)
                ++padding;
            if (padding > 2)
                return std::nullopt;

            for (size_t i = 0; i < input.size(); ++i)
            {
                const auto c = static_cast<unsigned char>(input[i]);
                const bool base64_char = std::isalnum(c) || c == '+' || c == '/';
                const bool padding_char = c == '=';
                if (!base64_char && !padding_char)
                    return std::nullopt;
                if (padding_char && i < input.size() - padding)
                    return std::nullopt;
            }

#if defined(CANOPY_SECURE_STREAM_BACKEND_MBEDTLS)
            size_t decoded_length = 0;
            auto size_result = mbedtls_base64_decode(
                nullptr, 0, &decoded_length, reinterpret_cast<const unsigned char*>(input.data()), input.size());
            if (size_result != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL || decoded_length == 0)
                return std::nullopt;

            std::vector<unsigned char> decoded(decoded_length);
            auto decode_result = mbedtls_base64_decode(
                decoded.data(),
                decoded.size(),
                &decoded_length,
                reinterpret_cast<const unsigned char*>(input.data()),
                input.size());
            if (decode_result != 0)
                return std::nullopt;
            decoded.resize(decoded_length);
            return decoded;
#elif defined(CANOPY_SECURE_STREAM_BACKEND_OPENSSL)
            std::vector<unsigned char> decoded((input.size() / 4) * 3);
            const auto decoded_length = EVP_DecodeBlock(
                decoded.data(), reinterpret_cast<const unsigned char*>(input.data()), static_cast<int>(input.size()));
            if (decoded_length < 0 || static_cast<size_t>(decoded_length) < padding)
                return std::nullopt;
            decoded.resize(static_cast<size_t>(decoded_length) - padding);
            return decoded;
#endif
        }

        bool has_nonzero_content_length(const request& request)
        {
            auto header = find_header(request, "Content-Length");
            if (!header)
                return false;

            auto value = trim_ascii(*header);
            return !value.empty() && value != "0";
        }

        auto validate_websocket_upgrade_request(
            const request& request,
            const client_connection_limits& limits) -> std::optional<std::string>
        {
            if (request.method != "GET")
                return "WebSocket upgrade method must be GET";

            auto host = find_header(request, "Host");
            if (!host || trim_ascii(*host).empty())
                return "Missing WebSocket Host header";
            if (!limits.allowed_websocket_hosts.empty() && !matches_allowed_value(*host, limits.allowed_websocket_hosts))
                return "WebSocket Host is not allowed";

            auto upgrade = find_header(request, "Upgrade");
            if (!upgrade || !ascii_iequals(trim_ascii(*upgrade), "websocket"))
                return "Missing or invalid WebSocket Upgrade header";

            auto connection = find_header(request, "Connection");
            if (!connection || !header_contains_token(*connection, "Upgrade"))
                return "Missing WebSocket Connection upgrade token";

            auto version = find_header(request, "Sec-WebSocket-Version");
            if (!version || trim_ascii(*version) != "13")
                return "Missing or invalid WebSocket version";

            auto key = find_header(request, "Sec-WebSocket-Key");
            if (!key)
                return "Missing Sec-WebSocket-Key header";

            auto decoded_key = decode_base64(*key);
            if (!decoded_key || decoded_key->size() != websocket_client_key_bytes)
                return "Invalid Sec-WebSocket-Key header";

            auto origin = find_header(request, "Origin");
            if (origin && trim_ascii(*origin).empty())
                return "Empty WebSocket Origin header";
            if (limits.require_websocket_origin && !origin)
                return "Missing WebSocket Origin header";
            if (origin)
            {
                const auto origin_value = trim_ascii(*origin);
                if (!limits.allowed_websocket_origins.empty()
                    && !matches_allowed_value(origin_value, limits.allowed_websocket_origins))
                    return "WebSocket Origin is not allowed";
                if (ascii_iequals(origin_value, "null")
                    && !matches_allowed_value(origin_value, limits.allowed_websocket_origins))
                    return "WebSocket Origin is not allowed";
            }

            if (!request.body.empty() || has_nonzero_content_length(request))
                return "WebSocket upgrade request must not contain a body";

            return std::nullopt;
        }

        auto calculate_ws_accept(std::string_view client_key) -> std::string
        {
            std::string combined = std::string(trim_ascii(client_key)) + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

#if defined(CANOPY_SECURE_STREAM_BACKEND_MBEDTLS)
            std::array<unsigned char, 20> hash{};
            if (mbedtls_sha1(reinterpret_cast<const unsigned char*>(combined.data()), combined.size(), hash.data()) != 0)
            {
                RPC_ERROR("Failed to calculate WebSocket accept SHA-1 hash");
                return {};
            }

            size_t encoded_length = 0;
            auto encode_result = mbedtls_base64_encode(nullptr, 0, &encoded_length, hash.data(), hash.size());
            if (encode_result != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL || encoded_length == 0)
            {
                RPC_ERROR("Failed to size WebSocket accept base64 buffer");
                return {};
            }

            std::string result(encoded_length, '\0');
            encode_result = mbedtls_base64_encode(
                reinterpret_cast<unsigned char*>(result.data()), result.size(), &encoded_length, hash.data(), hash.size());
            if (encode_result != 0)
            {
                RPC_ERROR("Failed to encode WebSocket accept hash");
                return {};
            }

            result.resize(encoded_length);
            return result;
#elif defined(CANOPY_SECURE_STREAM_BACKEND_OPENSSL)
            std::array<unsigned char, SHA_DIGEST_LENGTH> hash{};
            SHA1(reinterpret_cast<const unsigned char*>(combined.c_str()), combined.size(), hash.data());

            BIO* bio = BIO_new(BIO_s_mem());
            BIO* b64 = BIO_new(BIO_f_base64());
            bio = BIO_push(b64, bio);

            BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
            BIO_write(bio, hash.data(), static_cast<int>(hash.size()));
            BIO_flush(bio);

            BUF_MEM* buffer_ptr = nullptr;
            BIO_ctrl(bio, BIO_C_GET_BUF_MEM_PTR, 0, static_cast<void*>(&buffer_ptr));

            std::string result(buffer_ptr->data, buffer_ptr->length);
            BIO_free_all(bio);
            return result;
#endif
        }

        auto default_is_rest_request(const request& request) -> bool
        {
            auto path = canopy::http_utils::request_path(request.url, "");
            return path.size() >= 5 && path.compare(0, 5, "/api/") == 0;
        }
    } // namespace

    client_connection::client_connection(
        std::shared_ptr<streaming::stream> stream,
        handler_set handlers,
        client_connection_limits limits)
        : stream_(std::move(stream))
        , handlers_(std::move(handlers))
        , limits_(limits)
    {
    }

    int client_connection::on_method(
        llhttp_t* parser,
        const char* at,
        size_t length)
    {
        auto* ctx = static_cast<parser_request_context*>(parser->data);
        const auto& limits = limits_for(*ctx);
        ctx->parsed_request.method.append(at, length);
        if (exceeds_limit(ctx->parsed_request.method.size(), limits.max_method_bytes))
            return -1;
        return 0;
    }

    int client_connection::on_url(
        llhttp_t* parser,
        const char* at,
        size_t length)
    {
        auto* ctx = static_cast<parser_request_context*>(parser->data);
        const auto& limits = limits_for(*ctx);
        ctx->parsed_request.url.append(at, length);
        if (exceeds_limit(ctx->parsed_request.url.size(), limits.max_url_bytes))
            return -1;
        return 0;
    }

    int client_connection::on_header_field(
        llhttp_t* parser,
        const char* at,
        size_t length)
    {
        auto* ctx = static_cast<parser_request_context*>(parser->data);
        const auto& limits = limits_for(*ctx);

        if (ctx->reading_header_value)
        {
            if (!flush_header(*ctx))
                return -1;
        }

        ctx->current_header_field.append(at, length);
        if (exceeds_limit(ctx->current_header_field.size(), limits.max_header_name_bytes))
            return -1;
        ctx->reading_header_value = false;
        return 0;
    }

    int client_connection::on_header_value(
        llhttp_t* parser,
        const char* at,
        size_t length)
    {
        auto* ctx = static_cast<parser_request_context*>(parser->data);
        const auto& limits = limits_for(*ctx);
        ctx->current_header_value.append(at, length);
        if (exceeds_limit(ctx->current_header_value.size(), limits.max_header_value_bytes))
            return -1;
        ctx->reading_header_value = true;
        return 0;
    }

    int client_connection::on_headers_complete(llhttp_t* parser)
    {
        auto* ctx = static_cast<parser_request_context*>(parser->data);
        if (!flush_header(*ctx))
            return -1;
        ctx->headers_complete = true;
        return 0;
    }

    int client_connection::on_body(
        llhttp_t* parser,
        const char* at,
        size_t length)
    {
        auto* ctx = static_cast<parser_request_context*>(parser->data);
        const auto& limits = limits_for(*ctx);
        ctx->parsed_request.body.append(at, length);
        if (exceeds_limit(ctx->parsed_request.body.size(), limits.max_body_bytes))
            return -1;
        return 0;
    }

    int client_connection::on_message_complete(llhttp_t* parser)
    {
        auto* ctx = static_cast<parser_request_context*>(parser->data);
        ctx->message_complete = true;
        return 0;
    }

    auto client_connection::limits_for(const parser_request_context& ctx) -> const client_connection_limits&
    {
        static const client_connection_limits defaults;
        return ctx.limits != nullptr ? *ctx.limits : defaults;
    }

    bool client_connection::flush_header(parser_request_context& ctx)
    {
        if (ctx.current_header_field.empty())
        {
            return true;
        }

        ++ctx.header_count;
        if (exceeds_limit(ctx.header_count, limits_for(ctx).max_header_count))
            return false;

        ctx.parsed_request.headers[ctx.current_header_field] = ctx.current_header_value;
        ctx.current_header_field.clear();
        ctx.current_header_value.clear();
        return true;
    }

    auto client_connection::build_http_response(
        const response& input,
        bool keep_alive) -> std::string
    {
        return build_http_response(request{}, input, keep_alive);
    }

    auto client_connection::build_http_response(
        const request& request,
        const response& input,
        bool keep_alive) -> std::string
    {
        response output = input;
        output = apply_http_compression(request, std::move(output));
        if (output.status_code < 100 || output.status_code > 999)
        {
            RPC_WARNING("HTTP response status code {} is invalid; sending 500", output.status_code);
            output.status_code = 500;
        }
        if (output.status_text.empty())
        {
            output.status_text = status_text(output.status_code);
        }
        else if (!is_status_text_safe(output.status_text))
        {
            RPC_WARNING("HTTP response status text contained control characters; replacing it");
            output.status_text = status_text(output.status_code);
        }

        if (output.headers.find("Connection") == output.headers.end())
        {
            output.headers["Connection"] = keep_alive ? "keep-alive" : "close";
        }

        erase_header(output, "Transfer-Encoding");
        if (!response_status_allows_body(output.status_code))
        {
            output.body.clear();
            normalise_no_body_content_length(output);
        }
        else
            set_header(output, "Content-Length", std::to_string(output.body.size()));

        std::string wire_response = fmt::format("HTTP/1.1 {} {}\r\n", output.status_code, output.status_text);
        for (const auto& [key, value] : output.headers)
        {
            if (!is_header_safe(key, value))
            {
                RPC_WARNING("Skipping unsafe HTTP response header '{}'", key);
                continue;
            }
            wire_response += fmt::format("{}: {}\r\n", key, value);
        }

        wire_response += "\r\n";
        wire_response += output.body;
        return wire_response;
    }

    auto client_connection::build_websocket_handshake_response(
        const std::string& accept_key,
        const std::optional<std::string>& negotiated_extensions) -> std::string
    {
        response handshake;
        handshake.status_code = 101;
        handshake.status_text = status_text(101);
        handshake.headers = {{"Upgrade", "websocket"}, {"Connection", "Upgrade"}, {"Sec-WebSocket-Accept", accept_key}};
        if (negotiated_extensions)
            handshake.headers["Sec-WebSocket-Extensions"] = *negotiated_extensions;
        return build_http_response(handshake, false);
    }

    auto client_connection::dispatch_request(request request) const -> CORO_TASK(std::optional<response>)
    {
        const auto is_rest_request
            = handlers_.is_rest_request ? handlers_.is_rest_request(request) : default_is_rest_request(request);

        if (is_rest_request)
        {
            if (handlers_.rest_handler)
            {
                CO_RETURN CO_AWAIT handlers_.rest_handler(request);
            }
            CO_RETURN make_text_response(404, "Not Found");
        }

        if (handlers_.webpage_handler)
        {
            CO_RETURN CO_AWAIT handlers_.webpage_handler(std::move(request));
        }

        CO_RETURN make_text_response(404, "Not Found");
    }

    auto client_connection::handle_websocket_upgrade(request request) -> CORO_TASK(std::shared_ptr<rpc::transport>)
    {
        if (auto validation_error = validate_websocket_upgrade_request(request, limits_))
        {
            RPC_WARNING("Rejecting WebSocket upgrade: {}", *validation_error);
            auto error_response = build_http_response(make_text_response(400, "Bad Request"), false);
            CO_AWAIT stream_->send(rpc::byte_span{error_response});
            CO_RETURN nullptr;
        }

        if (!handlers_.websocket_upgrade_handler)
        {
            RPC_ERROR("No websocket upgrade handler configured");
            auto error_response = build_http_response(make_text_response(501, "Not Implemented"), false);
            CO_AWAIT stream_->send(rpc::byte_span{error_response});
            CO_RETURN nullptr;
        }

        auto key = find_header(request, "Sec-WebSocket-Key");
        if (!key.has_value())
        {
            auto error_response = build_http_response(make_text_response(400, "Bad Request"), false);
            CO_AWAIT stream_->send(rpc::byte_span{error_response});
            CO_RETURN nullptr;
        }

        std::string accept_key = calculate_ws_accept(key.value());
        if (accept_key.empty())
        {
            auto error_response = build_http_response(make_text_response(400, "Bad Request"), false);
            CO_AWAIT stream_->send(rpc::byte_span{error_response});
            CO_RETURN nullptr;
        }
        auto negotiated_extensions = negotiate_websocket_extensions(request);
        auto handshake_response = build_websocket_handshake_response(accept_key, negotiated_extensions);

        auto wsstatus = CO_AWAIT stream_->send(rpc::byte_span{handshake_response});
        if (!wsstatus.is_ok())
        {
            RPC_ERROR("Failed to send WebSocket handshake response");
            CO_RETURN nullptr;
        }

        RPC_INFO("WebSocket handshake completed");
        rpc::websocket_stream::stream_settings websocket_settings;
        websocket_settings.role = rpc::websocket_stream::endpoint_role::server;
        if (negotiated_extensions)
        {
            websocket_settings.permessage_deflate.enabled = true;
            websocket_settings.permessage_deflate.server_no_context_takeover = true;
            websocket_settings.permessage_deflate.client_no_context_takeover = true;
        }
        auto ws_stream = std::make_shared<streaming::websocket::stream>(
            stream_, websocket_settings, rpc::websocket_stream::endpoint_role::server);
        CO_RETURN CO_AWAIT handlers_.websocket_upgrade_handler(std::move(request), ws_stream);
    }

    auto client_connection::handle() -> CORO_TASK(std::shared_ptr<rpc::transport>)
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
                ctx.limits = &limits_;
                const auto request_started = std::chrono::steady_clock::now();
                llhttp_reset(&parser);
                parser.data = &ctx;

                bool is_websocket_upgrade = false;
                while (!ctx.message_complete)
                {
                    if (pending_input.empty())
                    {
                        auto [read_status, read_span] = CO_AWAIT stream_->receive(
                            rpc::mutable_byte_span{receive_buffer.data(), receive_buffer.size()},
                            timeout_from_ms(limits_.receive_poll_timeout_ms));
                        if (read_status.is_timeout())
                        {
                            const auto timed_out
                                = !ctx.headers_complete
                                      ? timeout_expired(request_started, timeout_from_ms(limits_.header_timeout_ms))
                                      : timeout_expired(request_started, timeout_from_ms(limits_.request_timeout_ms));
                            if (timed_out)
                            {
                                RPC_WARNING("HTTP client timed out while sending a request");
                                auto error_response
                                    = build_http_response(make_text_response(408, "Request Timeout"), false);
                                CO_AWAIT stream_->send(rpc::byte_span{error_response});
                                CO_RETURN nullptr;
                            }
                            continue;
                        }
                        if (!read_status.is_ok() || read_span.empty())
                        {
                            if (read_status.is_closed() || read_span.empty())
                            {
                                RPC_INFO("HTTP client closed the connection before sending another request");
                            }
                            else
                            {
                                RPC_WARNING(
                                    "failed to read HTTP request: status={} native_code={}",
                                    read_status.message(),
                                    read_status.native_code);
                            }
                            CO_RETURN nullptr;
                        }

                        std::string preview;
                        for (size_t i = 0; i < std::min<size_t>(100, read_span.size()); ++i)
                        {
                            auto c = static_cast<unsigned char>(read_span.data()[i]);
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
                        if (exceeds_limit(pending_input.size(), limits_.max_pending_input_bytes))
                        {
                            RPC_WARNING("HTTP request exceeded pending input limit");
                            auto error_response = build_http_response(make_text_response(400, "Bad Request"), false);
                            CO_AWAIT stream_->send(rpc::byte_span{error_response});
                            CO_RETURN nullptr;
                        }
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
                        CO_AWAIT stream_->send(rpc::byte_span{error_response});
                        CO_RETURN nullptr;
                    }

                    pending_input.erase(0, consumed);
                }

                ctx.parsed_request.keep_alive = llhttp_should_keep_alive(&parser) != 0;
                auto connection_header = find_header(ctx.parsed_request, "Connection");
                if (connection_header && header_contains_token(*connection_header, "close"))
                    ctx.parsed_request.keep_alive = false;

                if (is_websocket_upgrade)
                {
                    CO_RETURN CO_AWAIT handle_websocket_upgrade(ctx.parsed_request);
                }

                auto method_name = llhttp_method_name(static_cast<llhttp_method_t>(parser.method));
                if (ctx.parsed_request.method.empty() && method_name)
                {
                    ctx.parsed_request.method = method_name;
                }

                const auto path = canopy::http_utils::request_path(ctx.parsed_request.url, "");
                RPC_INFO("HTTP {} request for: {}", ctx.parsed_request.method, path);

                auto response
                    = (CO_AWAIT dispatch_request(ctx.parsed_request)).value_or(make_text_response(404, "Not Found"));
                auto wire_response = build_http_response(ctx.parsed_request, response, ctx.parsed_request.keep_alive);

                auto send_status = CO_AWAIT stream_->send(rpc::byte_span{wire_response});
                if (!send_status.is_ok())
                {
                    RPC_ERROR("Failed to send HTTP response for: {}", path);
                    CO_RETURN nullptr;
                }

                if (!ctx.parsed_request.keep_alive)
                {
                    CO_RETURN nullptr;
                }
            }
        }
        catch (const std::exception& e)
        {
            RPC_ERROR("Exception in client_connection::handle: {}", e.what());
        }

        CO_RETURN nullptr;
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
        case 408:
            return "Request Timeout";
        case 500:
            return "Internal Server Error";
        case 501:
            return "Not Implemented";
        default:
            return "OK";
        }
    }

    auto make_text_response(
        int status_code,
        std::string body,
        std::string content_type) -> response
    {
        response output;
        output.status_code = status_code;
        output.status_text = status_text(status_code);
        output.headers["Content-Type"] = std::move(content_type);
        output.body = std::move(body);
        return output;
    }

    auto make_json_response(
        int status_code,
        std::string json_body) -> response
    {
        return make_text_response(status_code, std::move(json_body), "application/json");
    }
} // namespace canopy::http_server
